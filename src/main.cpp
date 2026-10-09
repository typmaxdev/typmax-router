// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// typmax-router: a PNS routing service speaking JSON Lines on stdin/stdout.
// PROTOCOL.md is the contract. One request line in, one response line out.
#include <pthread.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>

#include "board.hpp"
#include "engine.hpp"
#include "json.hpp"

namespace {

constexpr int PROTOCOL_VERSION = 2;
// Bump SERVICE_VERSION whenever the same request could get a different answer
// (a KiCad update does: README.md "Updating to a new KiCad release").
constexpr const char* SERVICE_VERSION = "0.3.1";
// The vendored KiCad, from vendor/kicad.manifest (CMakeLists.txt)
constexpr const char* PNS_SOURCE = "KiCad " TYPMAX_KICAD_TAG " pcbnew/router @ " TYPMAX_KICAD_COMMIT;
constexpr size_t WORKER_STACK = 64 * 1024 * 1024; // PNS recursion is deep (an earlier prototype: 64 KB crashes)

int g_out_fd = 1;                 // the protocol channel (fd 1 is re-pointed at stderr)
char g_crash_line[1024];          // pre-rendered fatal line for the signal handler
int64_t g_default_limit_ms = 10000;
size_t g_max_line = 64u * 1024 * 1024; // 64 MiB: video's board is 5.8 MB of .kicad_pcb (review R-7)
bool g_test_hooks = false;              // TYPMAX_ROUTER_TEST_HOOKS=1: the crash hooks the tests use

void write_all(int fd, const char* p, size_t n)
{
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w <= 0)
            return;
        p += w;
        n -= static_cast<size_t>(w);
    }
}

void emit(const tj::Value& v)
{
    std::string s = v.dump();
    s += '\n';
    write_all(g_out_fd, s.data(), s.size());
}

tj::Value envelope(const tj::Value& id)
{
    tj::Value o = tj::obj();
    o.set("id", id);
    o.set("protocol_version", PROTOCOL_VERSION);
    return o;
}

tj::Value error_response(const tj::Value& id, const std::string& code, const std::string& msg, bool fatal = false)
{
    tj::Value o = envelope(id);
    o.set("ok", false);
    tj::Value e = tj::obj();
    e.set("code", code);
    e.set("message", msg);
    if (fatal)
        e.set("fatal", true);
    o.set("error", e);
    return o;
}

void set_crash_line(const tj::Value& id)
{
    std::string s = error_response(id, "internal_crash",
                                   "the router crashed on this request; the service exits and must be restarted",
                                   true)
                            .dump()
                    + "\n";
    if (s.size() >= sizeof g_crash_line)
        s = error_response(tj::Value(), "internal_crash", "the router crashed", true).dump() + "\n";
    std::memcpy(g_crash_line, s.data(), s.size() + 1);
}

// After a response line is out, a crash belongs to no request the client is
// still waiting on: point the pre-rendered line back at the null id (review R-4).
void reset_crash_line() { set_crash_line(tj::Value()); }

extern "C" void on_fatal_signal(int)
{
    write_all(g_out_fd, g_crash_line, std::strlen(g_crash_line));
    _exit(70);
}

// ---- state -------------------------------------------------------------------

struct State {
    std::shared_ptr<const tr::Board> board;
    int64_t revision = 0;
    uint64_t requests = 0;
    double last_ms = 0;
};

State g_state;

[[noreturn]] void bad_params(const std::string& m) { throw tr::RequestError("invalid_params", m); }

const tj::Value* opt(const tj::Value& p, const char* k)
{
    const tj::Value* v = p.find(k);
    return (v && !v->is_null()) ? v : nullptr;
}

std::string req_str(const tj::Value& p, const char* k)
{
    const tj::Value* v = opt(p, k);
    if (!v || !v->is_string())
        bad_params(std::string("\"") + k + "\" must be a string");
    return v->as_string();
}

bool opt_bool(const tj::Value& p, const char* k, bool d)
{
    const tj::Value* v = opt(p, k);
    if (!v)
        return d;
    if (!v->is_bool())
        bad_params(std::string("\"") + k + "\" must be a boolean");
    return v->as_bool();
}

std::optional<int64_t> opt_size(const tj::Value& p, const char* k)
{
    const tj::Value* v = opt(p, k);
    if (!v)
        return std::nullopt;
    if (!v->is_int() || v->as_int() <= 0 || v->as_int() > 100000000)
        bad_params(std::string("\"") + k + "\" must be a positive integer (nm, <= 100 mm)");
    return v->as_int();
}

tr::Pt point_param(const tj::Value& v, const std::string& where)
{
    try {
        return tr::parse_point(v, where);
    }
    catch (const tr::RequestError& e) {
        bad_params(e.what());
    }
}

int layer_param(const tj::Value& v, const tr::Board& b, const std::string& where)
{
    if (!v.is_string())
        bad_params(where + " must be a layer name");
    int l = b.layer_index(v.as_string());
    if (l < 0)
        bad_params(where + ": unknown copper layer \"" + v.as_string() + "\"");
    return l;
}

tr::Mode mode_param(const tj::Value& p, tr::Mode d)
{
    const tj::Value* v = opt(p, "mode");
    if (!v)
        return d;
    if (!v->is_string())
        bad_params("\"mode\" must be walkaround, shove or mark_obstacles");
    const std::string& m = v->as_string();
    if (m == "walkaround")
        return tr::Mode::Walkaround;
    if (m == "shove")
        return tr::Mode::Shove;
    if (m == "mark_obstacles")
        return tr::Mode::MarkObstacles;
    bad_params("\"mode\" must be walkaround, shove or mark_obstacles; got \"" + m + "\"");
}

tr::Anchor anchor_param(const tj::Value& v, const char* which)
{
    tr::Anchor a;
    if (!v.is_object())
        bad_params(std::string(which) + " must be {\"pad\": id} or {\"point\": [x, y]}");
    const tj::Value* pad = opt(v, "pad");
    const tj::Value* pnt = opt(v, "point");
    if ((pad != nullptr) == (pnt != nullptr))
        bad_params(std::string(which) + " must name exactly one of \"pad\" or \"point\"");
    if (pad) {
        if (!pad->is_string())
            bad_params(std::string(which) + ".pad must be a pad id");
        a.is_pad = true;
        a.pad_id = pad->as_string();
    }
    else {
        a.point = point_param(*pnt, std::string(which) + ".point");
    }
    return a;
}

const tr::Board& need_board()
{
    if (!g_state.board)
        throw tr::RequestError("no_board", "no board is loaded; send load_board first");
    return *g_state.board;
}

tj::Value counts(const tr::Board& b)
{
    tj::Value c = tj::obj();
    c.set("layers", static_cast<int64_t>(b.layers.size()));
    c.set("pads", static_cast<int64_t>(b.pads.size()));
    c.set("tracks", static_cast<int64_t>(b.tracks.size()));
    c.set("vias", static_cast<int64_t>(b.vias.size()));
    c.set("keepouts", static_cast<int64_t>(b.keepouts.size()));
    c.set("copper", static_cast<int64_t>(b.copper.size()));
    c.set("nets", static_cast<int64_t>(b.net_names().size()));
    return c;
}

// Who computed a proposal (review P-4): a stored proposal names the engine that
// made it, so a client can tell whether a replay may differ. `version` moves on
// any change to what a request answers.
tj::Value engine_json()
{
    tj::Value e = tj::obj();
    e.set("name", "typmax-router");
    e.set("version", SERVICE_VERSION);
    e.set("pns", PNS_SOURCE);
    e.set("protocol", PROTOCOL_VERSION);
    return e;
}

tj::Value proposal_result(const tr::Proposal& p, const tr::Board& b)
{
    tj::Value r = tj::obj();
    r.set("revision", g_state.revision);
    r.set("engine", engine_json());
    r.set("proposal", tr::proposal_json(p, b));
    return r;
}

tj::Value do_route(const tj::Value& p)
{
    const tr::Board& b = need_board();
    tr::RouteRequest r;
    r.net = req_str(p, "net");
    const tj::Value* layer = opt(p, "layer");
    if (!layer)
        bad_params("\"layer\" is required");
    r.layer = layer_param(*layer, b, "layer");
    const tj::Value* start = opt(p, "start");
    if (!start)
        bad_params("\"start\" is required");
    r.start = anchor_param(*start, "start");
    if (const tj::Value* e = opt(p, "end"))
        r.end = anchor_param(*e, "end");
    if (const tj::Value* w = opt(p, "waypoints")) {
        if (!w->is_array() || w->as_array().size() > 1000)
            bad_params("\"waypoints\" must be an array of at most 1000 waypoints");
        for (size_t i = 0; i < w->as_array().size(); i++) {
            const tj::Value& x = w->as_array()[i];
            std::string where = "waypoints[" + std::to_string(i) + "]";
            if (!x.is_object() || !opt(x, "point"))
                bad_params(where + " must be {\"point\": [x, y], \"via\"?: bool, \"layer\"?: name}");
            tr::Waypoint wp;
            wp.point = point_param(*opt(x, "point"), where + ".point");
            wp.via = opt_bool(x, "via", false);
            if (wp.via && b.layers.size() < 2) // review R-6: index 0 is both F_Cu and B_Cu there
                bad_params(where + ": a via needs a board with at least two copper layers");
            if (wp.via) {
                const tj::Value* l = opt(x, "layer");
                if (!l)
                    bad_params(where + ": a via waypoint names the \"layer\" to continue on");
                wp.layer_after = layer_param(*l, b, where + ".layer");
            }
            r.waypoints.push_back(wp);
        }
    }
    if (!r.end && r.waypoints.empty())
        bad_params("a route needs an \"end\" or at least one waypoint");
    r.width = opt_size(p, "width");
    r.via_diameter = opt_size(p, "via_diameter");
    r.via_drill = opt_size(p, "via_drill");
    r.mode = mode_param(p, tr::Mode::Walkaround);
    r.via_allowed = opt_bool(p, "via_allowed", false);
    r.remove_loops = opt_bool(p, "remove_loops", true);
    if (const tj::Value* st = opt(p, "step")) {
        if (!st->is_int() || st->as_int() < 0 || st->as_int() > 1000000000)
            bad_params("\"step\" must be an integer >= 0 (nm; 0 = one jump)");
        r.step = st->as_int();
    }
    return proposal_result(tr::run_route(b, r), b);
}

tj::Value do_drag(const tj::Value& p)
{
    const tr::Board& b = need_board();
    tr::DragRequest r;
    r.item = req_str(p, "item");
    const tj::Value* to = opt(p, "to");
    if (!to)
        bad_params("\"to\" is required");
    r.to = point_param(*to, "to");
    if (const tj::Value* f = opt(p, "from"))
        r.from = point_param(*f, "from");
    r.mode = mode_param(p, tr::Mode::Shove);
    return proposal_result(tr::run_drag(b, r), b);
}

tj::Value do_apply(const tj::Value& p)
{
    const tr::Board& b = need_board();
    // Required since protocol 2 (review P-3): the one write path always knows
    // the revision it computed against, and an optional check invites a lost update.
    const tj::Value* br = opt(p, "base_revision");
    if (!br || !br->is_int())
        bad_params("\"base_revision\" is required: the integer revision the proposal was computed against");
    if (br->as_int() != g_state.revision)
        throw tr::RequestError("stale_proposal", "base_revision " + std::to_string(br->as_int())
                                                         + " is not the loaded revision "
                                                         + std::to_string(g_state.revision));
    const tj::Value* prop = opt(p, "proposal");
    if (!prop)
        bad_params("\"proposal\" is required");
    tr::Proposal pr = tr::parse_proposal(*prop, b);
    auto nb = tr::apply_proposal(b, pr);
    g_state.board = nb;
    g_state.revision++;
    tj::Value r = tj::obj();
    r.set("revision", g_state.revision);
    r.set("removed", static_cast<int64_t>(pr.remove.size()));
    r.set("added", static_cast<int64_t>(pr.add_tracks.size() + pr.add_vias.size()));
    r.set("counts", counts(*nb));
    return r;
}

tj::Value do_load(const tj::Value& p)
{
    const tj::Value* b = opt(p, "board");
    if (!b)
        bad_params("\"board\" is required");
    auto nb = tr::parse_board(*b);
    g_state.board = nb;
    g_state.revision = 0;
    tj::Value r = tj::obj();
    r.set("revision", g_state.revision);
    r.set("counts", counts(*nb));
    return r;
}

tj::Value do_ping()
{
    tj::Value r = tj::obj();
    r.set("pong", true);
    r.set("service", "typmax-router");
    r.set("version", SERVICE_VERSION);
    r.set("engine", engine_json());
    return r;
}

tj::Value do_stats()
{
    tj::Value r = tj::obj();
    r.set("loaded", g_state.board != nullptr);
    r.set("revision", g_state.revision);
    if (g_state.board)
        r.set("counts", counts(*g_state.board));
    r.set("requests", static_cast<int64_t>(g_state.requests));
    r.set("last_elapsed_ms", g_state.last_ms);
    r.set("default_time_limit_ms", g_default_limit_ms);
    return r;
}

tj::Value dispatch(const std::string& method, const tj::Value& params)
{
    if (method == "ping")
        return do_ping();
    if (method == "stats")
        return do_stats();
    if (method == "load_board")
        return do_load(params);
    if (method == "route")
        return do_route(params);
    if (method == "drag")
        return do_drag(params);
    if (method == "apply")
        return do_apply(params);
    if (g_test_hooks && method == "__test_crash__")
        std::raise(SIGSEGV); // the internal_crash path, end to end (tests/run_tests.py)
    throw tr::RequestError("unknown_method", "unknown method \"" + method + "\"");
}

// ---- the worker: one long-lived thread with a big stack ----------------------

struct Worker {
    std::mutex m;
    std::condition_variable cv;
    std::function<void()> job;
    bool has_job = false, done = false;

    static void* entry(void* self)
    {
        auto* w = static_cast<Worker*>(self);
        // an alternate signal stack, so a stack overflow still reaches on_fatal_signal
        static char altstack[256 * 1024];
        stack_t ss{};
        ss.ss_sp = altstack;
        ss.ss_size = sizeof altstack;
        sigaltstack(&ss, nullptr);
        for (;;) {
            std::function<void()> j;
            {
                std::unique_lock<std::mutex> lk(w->m);
                w->cv.wait(lk, [w] { return w->has_job; });
                j = std::move(w->job);
            }
            j();
            {
                std::lock_guard<std::mutex> lk(w->m);
                w->has_job = false;
                w->done = true;
            }
            w->cv.notify_all();
        }
        return nullptr;
    }

    void start()
    {
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, WORKER_STACK);
        pthread_t t;
        if (pthread_create(&t, &a, &Worker::entry, this) != 0) {
            std::fprintf(stderr, "typmax-router: cannot start the worker thread\n");
            std::exit(2);
        }
        pthread_attr_destroy(&a);
        pthread_detach(t);
    }

    // false = time limit exceeded (the job is still running)
    bool run(std::function<void()> j, int64_t limit_ms)
    {
        std::unique_lock<std::mutex> lk(m);
        job = std::move(j);
        has_job = true;
        done = false;
        cv.notify_all();
        return cv.wait_for(lk, std::chrono::milliseconds(limit_ms), [this] { return done; });
    }
};

Worker g_worker;

// The envelope of one request line, parsed and checked. `error` is set when the
// line is answered without dispatch.
struct Envelope {
    tj::Value id; // null until parsed
    std::string method;
    tj::Value params;
    int64_t limit = 0;
    bool timing = false;
    tj::Value error; // a ready error response, or null
};

Envelope parse_envelope(const std::string& line)
{
    Envelope env;
    env.limit = g_default_limit_ms;
    if (g_test_hooks && line.find("\"__test_crash_in_parse__\"") != std::string::npos)
        std::raise(SIGSEGV);
    tj::Value req;
    try {
        req = tj::Value::parse(line);
    }
    catch (const std::bad_alloc&) {
        env.error = error_response(env.id, "request_too_large", "out of memory parsing the request");
        return env;
    }
    catch (const std::exception& e) {
        env.error = error_response(env.id, "parse_error", e.what());
        return env;
    }
    if (!req.is_object()) {
        env.error = error_response(env.id, "invalid_request", "a request is a JSON object");
        return env;
    }
    if (const tj::Value* i = req.find("id")) {
        if (i->is_string() || i->is_int())
            env.id = *i;
        else if (!i->is_null()) {
            env.error = error_response(env.id, "invalid_request", "\"id\" must be a string or an integer");
            return env;
        }
    }
    const tj::Value* pv = req.find("protocol_version");
    if (!pv || !pv->is_int() || pv->as_int() != PROTOCOL_VERSION) {
        env.error = error_response(env.id, "unsupported_protocol_version",
                                   "this service speaks protocol_version " + std::to_string(PROTOCOL_VERSION));
        return env;
    }
    const tj::Value* mv = req.find("method");
    if (!mv || !mv->is_string()) {
        env.error = error_response(env.id, "invalid_request", "\"method\" must be a string");
        return env;
    }
    env.method = mv->as_string();
    env.params = tj::obj();
    if (const tj::Value* p = req.find("params"); p && !p->is_null()) {
        if (!p->is_object()) {
            env.error = error_response(env.id, "invalid_request", "\"params\" must be an object");
            return env;
        }
        env.params = *p;
    }
    try {
        if (const tj::Value* t = opt(env.params, "time_limit_ms")) {
            if (!t->is_int() || t->as_int() < 1 || t->as_int() > 600000)
                bad_params("\"time_limit_ms\" must be an integer in 1..600000");
            env.limit = t->as_int();
        }
        env.timing = opt_bool(env.params, "timing", false);
    }
    catch (const tr::RequestError& e) {
        env.error = error_response(env.id, e.code, e.what());
    }
    return env;
}

[[noreturn]] void fatal_timeout(const tj::Value& id, int64_t limit, const char* phase)
{
    // The PNS session cannot be interrupted safely and PNS keeps a process-wide
    // router singleton, so the only safe recovery is a restart (PROTOCOL.md).
    emit(error_response(id, "timeout",
                        std::string(phase) + " exceeded time_limit_ms=" + std::to_string(limit)
                                + "; the service exits and must be restarted",
                        true));
    _exit(3);
}

void handle_line(const std::string& line)
{
    // Phase 1, parsing: on the worker under the default time limit, so no input
    // (a quadratic or merely huge line) can stall the service outside every
    // limit (review R-1). The request's own limit is not known until it parses.
    Envelope env;
    if (!g_worker.run([&]() { env = parse_envelope(line); }, g_default_limit_ms))
        fatal_timeout(tj::Value(), g_default_limit_ms, "parsing the request");
    if (!env.error.is_null()) {
        emit(env.error);
        return;
    }

    // Phase 2, the method, under the request's limit.
    set_crash_line(env.id);
    const tj::Value& id = env.id;
    const std::string& method = env.method;
    const tj::Value& params = env.params;
    const bool timing = env.timing;
    tj::Value response;
    double elapsed = 0;
    // The job captures this frame's locals by reference. That is safe only because
    // a job that outlives its limit never returns here: fatal_timeout _exit()s.
    // A "recover instead of exit" change would turn this into a use-after-return.
    bool finished = g_worker.run(
            [&]() {
                auto t0 = std::chrono::steady_clock::now();
                try {
                    tj::Value result = dispatch(method, params);
                    elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                    if (timing && result.is_object()) {
                        result.set("elapsed_ms", elapsed);
                        if (method == "route" || method == "drag")
                            result.set("world_ms", tr::g_last_world_ms);
                    }
                    response = envelope(id);
                    response.set("ok", true);
                    response.set("result", result);
                }
                catch (const tr::RequestError& e) {
                    response = error_response(id, e.code, e.what());
                    if (!e.detail.empty())
                        response.mut_object().back().second.set("detail", e.detail);
                }
                catch (const tj::ParseError& e) {
                    response = error_response(id, "invalid_params", e.what());
                }
                catch (const std::bad_alloc&) {
                    response = error_response(id, "internal_error", "out of memory");
                }
                catch (const std::exception& e) {
                    response = error_response(id, "internal_error", e.what());
                }
                catch (...) {
                    response = error_response(id, "internal_error", "unknown exception");
                }
                elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            },
            env.limit);
    if (!finished)
        fatal_timeout(id, env.limit, "the request");
    g_state.requests++;
    g_state.last_ms = elapsed;
    emit(response);
    reset_crash_line();
}

// Reads newline-terminated lines from a file descriptor without ever holding
// more than max + one chunk of a line in memory (review R-2): a line past the
// cap is discarded as it streams in and reported once its newline arrives.
class LineReader {
public:
    enum Status { EndOfInput, Line, TooLarge };

    LineReader(int fd, size_t max) : m_fd(fd), m_max(max) {}

    Status next(std::string& line)
    {
        bool discarding = false;
        for (;;) {
            size_t nl = m_buf.find('\n', m_scan);
            if (nl != std::string::npos) {
                if (discarding) {
                    consume(nl + 1);
                    return TooLarge;
                }
                line.assign(m_buf, m_start, nl - m_start);
                consume(nl + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.size() > m_max)
                    return TooLarge;
                return Line;
            }
            m_scan = m_buf.size();
            if (!discarding && m_buf.size() - m_start > m_max + 1) // +1: a trailing '\r' is not counted
                discarding = true;
            if (discarding) { // keep nothing of an over-long line
                m_buf.clear();
                m_start = m_scan = 0;
            }
            if (m_eof) {
                if (discarding)
                    return TooLarge;
                if (m_start == m_buf.size())
                    return EndOfInput;
                line.assign(m_buf, m_start, std::string::npos);
                consume(m_buf.size());
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return line.size() > m_max ? TooLarge : Line;
            }
            fill();
        }
    }

private:
    void consume(size_t upto)
    {
        m_start = m_scan = upto;
        if (m_start == m_buf.size()) {
            m_buf.clear();
            m_start = m_scan = 0;
        }
        else if (m_start > (1u << 20) && m_start > m_buf.size() / 2) {
            m_buf.erase(0, m_start);
            m_start = m_scan = 0;
        }
    }

    void fill()
    {
        char chunk[1 << 16];
        for (;;) {
            ssize_t n = ::read(m_fd, chunk, sizeof chunk);
            if (n > 0) {
                m_buf.append(chunk, static_cast<size_t>(n));
                return;
            }
            if (n < 0 && errno == EINTR)
                continue;
            m_eof = true; // 0 = EOF; a read error ends the input the same way
            return;
        }
    }

    int m_fd;
    size_t m_max;
    std::string m_buf;
    size_t m_start = 0, m_scan = 0;
    bool m_eof = false;
};

// A strict decimal argument: digits only, within [lo, hi]; else exit 2 (review R-3).
int64_t numeric_arg(const char* flag, const char* text, int64_t lo, int64_t hi)
{
    errno = 0;
    char* end = nullptr;
    long long v = std::strtoll(text, &end, 10);
    if (*text == '\0' || *end != '\0' || errno == ERANGE || v < lo || v > hi || !std::isdigit(static_cast<unsigned char>(*text))) {
        std::fprintf(stderr, "typmax-router: %s takes an integer in %lld..%lld, got \"%s\"\n", flag,
                     static_cast<long long>(lo), static_cast<long long>(hi), text);
        std::exit(2);
    }
    return v;
}

} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--time-limit-ms" && i + 1 < argc) {
            g_default_limit_ms = numeric_arg("--time-limit-ms", argv[++i], 1, 600000);
        }
        else if (a == "--max-line-bytes" && i + 1 < argc) {
            g_max_line = static_cast<size_t>(numeric_arg("--max-line-bytes", argv[++i], 1, int64_t(1) << 32));
        }
        else if (a == "--version") {
            std::printf("typmax-router %s (protocol %d)\n", SERVICE_VERSION, PROTOCOL_VERSION);
            return 0;
        }
        else {
            std::fprintf(stderr,
                         "usage: typmax-router [--time-limit-ms N] [--max-line-bytes N] [--version]\n"
                         "JSON Lines on stdin/stdout; see PROTOCOL.md\n");
            return 2;
        }
    }
    // Keep the protocol channel private: anything the vendored code prints goes to stderr.
    g_out_fd = dup(1);
    dup2(2, 1);
    set_crash_line(tj::Value());
    for (int s : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT}) {
        struct sigaction sa{};
        sa.sa_handler = on_fatal_signal;
        sa.sa_flags = SA_ONSTACK;
        sigemptyset(&sa.sa_mask);
        sigaction(s, &sa, nullptr);
    }
    std::signal(SIGPIPE, SIG_IGN);

    if (const char* h = std::getenv("TYPMAX_ROUTER_TEST_HOOKS"); h && std::strcmp(h, "1") == 0)
        g_test_hooks = true;

    g_worker.start();
    LineReader reader(0, g_max_line);
    std::string line;
    for (;;) {
        LineReader::Status st = reader.next(line);
        if (st == LineReader::EndOfInput)
            break;
        if (st == LineReader::TooLarge) {
            emit(error_response(tj::Value(), "request_too_large",
                                "request line exceeds " + std::to_string(g_max_line) + " bytes"));
            continue;
        }
        if (line.find_first_not_of(" \t") == std::string::npos)
            continue;
        handle_line(line);
    }
    // EOF: leave without running static destructors. The worker thread is parked on
    // a condition variable that a normal exit would destroy under it (glibc hangs).
    _exit(0);
}
