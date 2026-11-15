// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// The route_pair and tune params (PROTOCOL.md), checked at the boundary: each
// wrong param is invalid_params naming it, before any engine work.
#include "params.hpp"

#include <cstdlib>

namespace tr {
namespace params {

namespace {

constexpr int64_t MAX_LENGTH = 10'000'000'000; // 10 m of track: far past any board
constexpr int64_t MAX_SKEW = 1'000'000'000;    // the skew placer counts in a 32-bit int

const tj::Value& required(const tj::Value& p, const char* k)
{
    const tj::Value* v = opt(p, k);
    if (!v)
        bad_params(std::string("\"") + k + "\" is required");
    return *v;
}

PairEnd pair_end_param(const tj::Value& v, const std::string& which)
{
    if (!v.is_object() || !opt(v, "p") || !opt(v, "n"))
        bad_params(which + " must be {\"p\": end, \"n\": end}, each {\"pad\": id} or {\"point\": [x, y]}");
    PairEnd e;
    e.p = anchor_param(*opt(v, "p"), (which + ".p").c_str());
    e.n = anchor_param(*opt(v, "n"), (which + ".n").c_str());
    return e;
}

int64_t int_in(const tj::Value& p, const char* k, int64_t lo, int64_t hi, const char* what)
{
    const tj::Value& v = required(p, k);
    if (!v.is_int() || v.as_int() < lo || v.as_int() > hi)
        bad_params(std::string("\"") + k + "\" must be " + what);
    return v.as_int();
}

// walkaround (the default) or shove: the pair placer's two modes the protocol offers
Mode pair_mode(const tj::Value& p)
{
    const tj::Value* v = opt(p, "mode");
    if (!v)
        return Mode::Walkaround;
    if (v->is_string() && v->as_string() == "walkaround")
        return Mode::Walkaround;
    if (v->is_string() && v->as_string() == "shove")
        return Mode::Shove;
    bad_params("\"mode\" must be walkaround or shove");
}

Aim aim_param(const tj::Value& p)
{
    const tj::Value& v = required(p, "aim");
    if (v.is_string() && v.as_string() == "length")
        return Aim::Length;
    if (v.is_string() && v.as_string() == "pair_length")
        return Aim::PairLength;
    if (v.is_string() && v.as_string() == "skew")
        return Aim::Skew;
    bad_params("\"aim\" must be length, pair_length or skew");
}

void tune_nets(const tj::Value& p, TuneRequest& r)
{
    if (r.aim == Aim::Length) {
        if (opt(p, "p") || opt(p, "n"))
            bad_params("aim length tunes one \"net\"; \"p\" and \"n\" are for pair_length and skew");
        r.net = req_str(p, "net");
        return;
    }
    if (opt(p, "net"))
        bad_params("aims pair_length and skew name the pair as \"p\" and \"n\", not \"net\"");
    r.p = req_str(p, "p");
    r.n = req_str(p, "n");
    if (r.p == r.n)
        bad_params("\"p\" and \"n\" must be two different nets");
}

void tune_shape(const tj::Value& p, TuneRequest& r)
{
    r.spacing = opt_size(p, "spacing");
    if (const tj::Value* a = opt(p, "amplitude")) {
        if (!a->is_array() || a->as_array().size() != 2)
            bad_params("\"amplitude\" must be [min, max]");
        tj::Value o = tj::obj();
        o.set("min", a->as_array()[0]);
        o.set("max", a->as_array()[1]);
        r.amp_min = opt_size(o, "min");
        r.amp_max = opt_size(o, "max");
        if (!r.amp_min || !r.amp_max || *r.amp_min > *r.amp_max)
            bad_params("\"amplitude\" must be [min, max], two sizes (nm) with min <= max");
    }
    if (const tj::Value* c = opt(p, "corners")) {
        if (c->is_string() && c->as_string() == "square")
            r.corners = Corners::Square;
        else if (c->is_string() && c->as_string() == "chamfer")
            r.corners = Corners::Chamfer;
        else
            bad_params("\"corners\" must be square or chamfer");
    }
    r.single_sided = opt_bool(p, "single_sided", false);
    if (const tj::Value* m = opt(p, "mode"); m && !(m->is_string() && m->as_string() == "walkaround"))
        bad_params("\"mode\" must be walkaround: the engine's meander placers never shove");
}

} // namespace

PairRequest parse_route_pair(const tj::Value& p, const Board& b)
{
    PairRequest r;
    r.p = req_str(p, "p");
    r.n = req_str(p, "n");
    if (r.p == r.n)
        bad_params("\"p\" and \"n\" must be two different nets");
    r.layer = layer_param(required(p, "layer"), b, "layer");
    r.start = pair_end_param(required(p, "start"), "start");
    if (const tj::Value* e = opt(p, "end"))
        r.end = pair_end_param(*e, "end");
    r.waypoints = waypoints_param(p, b);
    if (!r.end && r.waypoints.empty())
        bad_params("a pair route needs an \"end\" or at least one waypoint");
    for (size_t i = 1; i < r.waypoints.size(); i++)
        if (r.waypoints[i].point == r.waypoints[i - 1].point)
            bad_params("waypoint " + std::to_string(i) + " is where the pair already is; move first");
    required(p, "gap");
    r.gap = *opt_size(p, "gap");
    r.via_gap = opt_size(p, "via_gap");
    r.mode = pair_mode(p);
    r.via_allowed = opt_bool(p, "via_allowed", false);
    return r;
}

TuneRequest parse_tune(const tj::Value& p)
{
    TuneRequest r;
    r.aim = aim_param(p);
    tune_nets(p, r);
    if (r.aim == Aim::Skew)
        r.target = int_in(p, "target", -MAX_SKEW, MAX_SKEW, "an integer skew in nm, |skew| <= 1e9");
    else
        r.target = int_in(p, "target", 1, MAX_LENGTH, "a positive integer length in nm, <= 1e10");
    r.tolerance = int_in(p, "tolerance", 0, MAX_SKEW, "an integer in 0..1e9 nm");
    if (r.aim == Aim::Skew && std::abs(r.target) + r.tolerance > MAX_SKEW)
        bad_params("|target| + tolerance must stay within 1e9 nm for a skew");
    r.track = req_str(p, "track");
    r.from = point_param(required(p, "from"), "from");
    r.to = point_param(required(p, "to"), "to");
    if (r.from == r.to)
        bad_params("\"from\" and \"to\" must be two different points");
    tune_shape(p, r);
    return r;
}

} // namespace params
} // namespace tr
