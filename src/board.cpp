// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
#include "board.hpp"

#include <algorithm>
#include <cmath>

namespace tr {

namespace {

// |coordinate| bound: KiCad's own board limit is about +-2.1 m in nm (int32);
// keep 2x headroom so router arithmetic (sums, hulls) cannot overflow.
constexpr int64_t COORD_LIMIT = 1000000000LL; // 1 m
constexpr int64_t SIZE_LIMIT = 100000000LL;   // 100 mm for any width/diameter/clearance
constexpr size_t MAX_ITEMS = 2000000;

[[noreturn]] void bad(const std::string& where, const std::string& what)
{
    throw RequestError("invalid_board", where + ": " + what);
}

const tj::Value& req(const tj::Value& o, const char* key, const std::string& where)
{
    if (!o.is_object())
        bad(where, "expected an object");
    const tj::Value* v = o.find(key);
    if (!v)
        bad(where, std::string("missing \"") + key + "\"");
    return *v;
}

int64_t get_int(const tj::Value& v, const std::string& where)
{
    try {
        return v.as_int();
    }
    catch (const std::exception&) {
        bad(where, "expected an integer (nanometres)");
    }
}

int64_t get_size(const tj::Value& v, const std::string& where, bool allow_zero)
{
    int64_t s = get_int(v, where);
    if (s < 0 || (!allow_zero && s == 0) || s > SIZE_LIMIT)
        bad(where, "size out of range: " + std::to_string(s));
    return s;
}

std::string get_str(const tj::Value& v, const std::string& where)
{
    if (!v.is_string())
        bad(where, "expected a string");
    return v.as_string();
}

std::string get_net(const tj::Value& o, const std::string& where)
{
    const tj::Value* n = o.find("net");
    if (!n || n->is_null())
        return "";
    return get_str(*n, where + ".net");
}

std::string get_id(const tj::Value& o, const std::string& where)
{
    std::string id = get_str(req(o, "id", where), where + ".id");
    if (id.empty() || id.size() > 200)
        bad(where + ".id", "must be 1..200 characters");
    return id;
}

int get_layer(const tj::Value& v, const Board& b, const std::string& where)
{
    std::string name = get_str(v, where);
    int i = b.layer_index(name);
    if (i < 0)
        bad(where, "unknown copper layer \"" + name + "\"");
    return i;
}

std::vector<int> get_layers(const tj::Value& v, const Board& b, const std::string& where)
{
    if (!v.is_array() || v.as_array().empty())
        bad(where, "expected a non-empty array of layer names");
    std::vector<int> out;
    for (size_t i = 0; i < v.as_array().size(); i++)
        out.push_back(get_layer(v.as_array()[i], b, where + "[" + std::to_string(i) + "]"));
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<Pt> get_polygon(const tj::Value& v, const std::string& where, bool relative)
{
    if (!v.is_array() || v.as_array().size() < 3)
        bad(where, "expected an array of at least 3 points");
    if (v.as_array().size() > 100000)
        bad(where, "too many vertices");
    std::vector<Pt> out;
    for (size_t i = 0; i < v.as_array().size(); i++) {
        Pt p = parse_point(v.as_array()[i], where + "[" + std::to_string(i) + "]");
        if (relative && (std::llabs(p.x) > SIZE_LIMIT || std::llabs(p.y) > SIZE_LIMIT))
            bad(where, "relative vertex out of range");
        out.push_back(p);
    }
    return out;
}

bool get_bool(const tj::Value& o, const char* key, bool dflt, const std::string& where)
{
    const tj::Value* v = o.find(key);
    if (!v || v->is_null())
        return dflt;
    if (!v->is_bool())
        bad(where + "." + key, "expected a boolean");
    return v->as_bool();
}

Pad parse_pad(const tj::Value& v, const Board& b, const std::string& where)
{
    Pad p;
    p.id = get_id(v, where);
    if (const tj::Value* f = v.find("footprint"); f && !f->is_null())
        p.footprint = get_str(*f, where + ".footprint");
    if (const tj::Value* f = v.find("pad"); f && !f->is_null())
        p.pad = get_str(*f, where + ".pad");
    p.net = get_net(v, where);
    p.layers = get_layers(req(v, "layers", where), b, where + ".layers");
    std::string shape = get_str(req(v, "shape", where), where + ".shape");
    if (shape == "circle")
        p.shape = PadShape::Circle;
    else if (shape == "rect")
        p.shape = PadShape::Rect;
    else if (shape == "roundrect")
        p.shape = PadShape::RoundRect;
    else if (shape == "oval")
        p.shape = PadShape::Oval;
    else if (shape == "polygon")
        p.shape = PadShape::Polygon;
    else
        bad(where + ".shape", "one of circle, rect, roundrect, oval, polygon; got \"" + shape + "\"");
    p.pos = parse_point(req(v, "position", where), where + ".position");
    if (const tj::Value* r = v.find("rotation"); r && !r->is_null()) {
        if (!r->is_number())
            bad(where + ".rotation", "expected degrees as a number");
        p.rotation = r->as_double();
        if (!std::isfinite(p.rotation) || std::fabs(p.rotation) > 3600)
            bad(where + ".rotation", "out of range");
    }
    const tj::Value& sz = req(v, "size", where);
    if (!sz.is_array() || sz.as_array().size() != 2)
        bad(where + ".size", "expected [w, h]");
    p.w = get_size(sz.as_array()[0], where + ".size[0]", false);
    p.h = get_size(sz.as_array()[1], where + ".size[1]", false);
    if (const tj::Value* d = v.find("drill"); d && !d->is_null())
        p.drill = get_size(*d, where + ".drill", true);
    if (p.shape == PadShape::Polygon)
        p.polygon = get_polygon(req(v, "polygon", where), where + ".polygon", true);
    return p;
}

Keepout parse_keepout(const tj::Value& v, const Board& b, const std::string& where)
{
    Keepout k;
    k.id = get_id(v, where);
    k.layers = get_layers(req(v, "layers", where), b, where + ".layers");
    k.polygon = get_polygon(req(v, "polygon", where), where + ".polygon", false);
    return k;
}

Copper parse_copper(const tj::Value& v, const Board& b, const std::string& where)
{
    Copper c;
    c.id = get_id(v, where);
    c.net = get_net(v, where);
    c.layer = get_layer(req(v, "layer", where), b, where + ".layer");
    c.polygon = get_polygon(req(v, "polygon", where), where + ".polygon", false);
    return c;
}

const tj::Value& get_array(const tj::Value& o, const char* key, const std::string& where)
{
    static const tj::Value empty = tj::arr();
    const tj::Value* v = o.find(key);
    if (!v || v->is_null())
        return empty;
    if (!v->is_array())
        bad(where + "." + key, "expected an array");
    if (v->as_array().size() > MAX_ITEMS)
        bad(where + "." + key, "too many items");
    return *v;
}

void parse_rules(const tj::Value& v, Rules& r)
{
    const std::string where = "board.rules";
    const tj::Value& classes = req(v, "classes", where);
    if (!classes.is_object() || classes.as_object().empty())
        bad(where + ".classes", "expected a non-empty object of net classes");
    for (const auto& [name, c] : classes.as_object()) {
        std::string w = where + ".classes." + name;
        NetClass nc;
        nc.name = name;
        nc.clearance = get_size(req(c, "clearance", w), w + ".clearance", true);
        nc.track_width = get_size(req(c, "track_width", w), w + ".track_width", false);
        nc.via_diameter = get_size(req(c, "via_diameter", w), w + ".via_diameter", false);
        nc.via_drill = get_size(req(c, "via_drill", w), w + ".via_drill", false);
        if (nc.via_drill >= nc.via_diameter)
            bad(w, "via_drill must be smaller than via_diameter");
        r.classes[name] = nc;
    }
    r.default_class = get_str(req(v, "default_class", where), where + ".default_class");
    if (!r.classes.count(r.default_class))
        bad(where + ".default_class", "names no class: \"" + r.default_class + "\"");
    if (const tj::Value* nc = v.find("net_classes"); nc && !nc->is_null()) {
        if (!nc->is_object())
            bad(where + ".net_classes", "expected an object {net: class}");
        for (const auto& [net, cls] : nc->as_object()) {
            std::string c = get_str(cls, where + ".net_classes." + net);
            if (!r.classes.count(c))
                bad(where + ".net_classes." + net, "names no class: \"" + c + "\"");
            r.net_class[net] = c;
        }
    }
    const NetClass& d = r.classes.at(r.default_class);
    r.edge_clearance = d.clearance;
    r.hole_clearance = d.clearance;
    r.hole_to_hole = d.clearance;
    if (const tj::Value* e = v.find("edge_clearance"); e && !e->is_null())
        r.edge_clearance = get_size(*e, where + ".edge_clearance", true);
    if (const tj::Value* e = v.find("hole_clearance"); e && !e->is_null())
        r.hole_clearance = get_size(*e, where + ".hole_clearance", true);
    if (const tj::Value* e = v.find("hole_to_hole"); e && !e->is_null())
        r.hole_to_hole = get_size(*e, where + ".hole_to_hole", true);
}

} // namespace

int64_t check_coord(int64_t v, const std::string& where)
{
    if (v > COORD_LIMIT || v < -COORD_LIMIT)
        bad(where, "coordinate out of range (|v| <= 1e9 nm)");
    return v;
}

Pt parse_point(const tj::Value& v, const std::string& where)
{
    if (!v.is_array() || v.as_array().size() != 2)
        throw RequestError("invalid_board", where + ": expected a point [x, y] in integer nanometres");
    Pt p;
    p.x = check_coord(get_int(v.as_array()[0], where + "[0]"), where + "[0]");
    p.y = check_coord(get_int(v.as_array()[1], where + "[1]"), where + "[1]");
    return p;
}

const NetClass& Rules::class_for(const std::string& net) const
{
    auto it = net_class.find(net);
    if (it != net_class.end())
        return classes.at(it->second);
    return classes.at(default_class);
}

int64_t Rules::max_clearance() const
{
    int64_t m = std::max({edge_clearance, hole_clearance, hole_to_hole});
    for (const auto& [n, c] : classes)
        m = std::max(m, c.clearance);
    return m;
}

int Board::layer_index(const std::string& name) const
{
    for (size_t i = 0; i < layers.size(); i++)
        if (layers[i] == name)
            return static_cast<int>(i);
    return -1;
}

template <typename T> static const T* find_by_id(const std::vector<T>& v, const std::string& id)
{
    auto it = std::lower_bound(v.begin(), v.end(), id, [](const T& a, const std::string& k) { return a.id < k; });
    if (it != v.end() && it->id == id)
        return &*it;
    return nullptr;
}

const Pad* Board::find_pad(const std::string& id) const { return find_by_id(pads, id); }
const Track* Board::find_track(const std::string& id) const { return find_by_id(tracks, id); }
const Via* Board::find_via(const std::string& id) const { return find_by_id(vias, id); }

std::set<std::string> Board::net_names() const
{
    std::set<std::string> s;
    for (const auto& p : pads)
        if (!p.net.empty())
            s.insert(p.net);
    for (const auto& t : tracks)
        if (!t.net.empty())
            s.insert(t.net);
    for (const auto& v : vias)
        if (!v.net.empty())
            s.insert(v.net);
    for (const auto& c : copper)
        if (!c.net.empty())
            s.insert(c.net);
    // A net named only in rules.net_classes is not a net of the board (review
    // M-4): it carries no item, so it is not counted and cannot be routed.
    return s;
}

void Board::sort_items()
{
    auto by_id = [](const auto& a, const auto& b) { return a.id < b.id; };
    std::sort(pads.begin(), pads.end(), by_id);
    std::sort(tracks.begin(), tracks.end(), by_id);
    std::sort(vias.begin(), vias.end(), by_id);
    std::sort(keepouts.begin(), keepouts.end(), by_id);
    std::sort(copper.begin(), copper.end(), by_id);
}

void Board::check_unique_ids() const
{
    std::set<std::string> seen;
    auto check = [&](const std::string& id, const char* kind) {
        if (!seen.insert(id).second)
            throw RequestError("invalid_board", std::string("duplicate item id \"") + id + "\" (" + kind + ")");
    };
    for (const auto& p : pads)
        check(p.id, "pad");
    for (const auto& t : tracks)
        check(t.id, "track");
    for (const auto& v : vias)
        check(v.id, "via");
    for (const auto& k : keepouts)
        check(k.id, "keepout");
    for (const auto& c : copper)
        check(c.id, "copper");
}

Track parse_track(const tj::Value& v, const Board& b, const std::string& where)
{
    Track t;
    t.id = get_id(v, where);
    t.net = get_net(v, where);
    t.layer = get_layer(req(v, "layer", where), b, where + ".layer");
    t.start = parse_point(req(v, "start", where), where + ".start");
    t.end = parse_point(req(v, "end", where), where + ".end");
    t.width = get_size(req(v, "width", where), where + ".width", false);
    if (const tj::Value* m = v.find("mid"); m && !m->is_null()) {
        t.has_mid = true;
        t.mid = parse_point(*m, where + ".mid");
    }
    // The engine drops a zero-length segment from its world without a word, so
    // such a track would be no obstacle at all (review M-3). A converter maps a
    // KiCad dot of copper to a `copper` disc instead (tools/kicad_to_board.py).
    if (t.start == t.end)
        bad(where, "zero-length track (start == end); give a copper dot as a \"copper\" polygon");
    t.locked = get_bool(v, "locked", false, where);
    return t;
}

Via parse_via(const tj::Value& v, const Board& b, const std::string& where)
{
    Via x;
    x.id = get_id(v, where);
    x.net = get_net(v, where);
    x.pos = parse_point(req(v, "position", where), where + ".position");
    x.diameter = get_size(req(v, "diameter", where), where + ".diameter", false);
    x.drill = get_size(req(v, "drill", where), where + ".drill", false);
    if (x.drill >= x.diameter)
        bad(where, "drill must be smaller than diameter");
    std::vector<int> ls = get_layers(req(v, "layers", where), b, where + ".layers");
    if (ls.size() != 2)
        bad(where + ".layers", "expected the via's two end layers [from, to]");
    x.layer_from = ls.front();
    x.layer_to = ls.back();
    x.locked = get_bool(v, "locked", false, where);
    return x;
}

std::shared_ptr<Board> parse_board(const tj::Value& v)
{
    auto b = std::make_shared<Board>();
    const std::string where = "board";
    if (!v.is_object())
        bad(where, "expected an object");
    const tj::Value& layers = req(v, "layers", where);
    if (!layers.is_array() || layers.as_array().empty() || layers.as_array().size() > 32)
        bad(where + ".layers", "expected 1..32 copper layer names, top to bottom");
    for (size_t i = 0; i < layers.as_array().size(); i++) {
        std::string n = get_str(layers.as_array()[i], where + ".layers[" + std::to_string(i) + "]");
        if (n.empty() || b->layer_index(n) >= 0)
            bad(where + ".layers", "layer names must be non-empty and unique");
        b->layers.push_back(n);
    }
    parse_rules(req(v, "rules", where), b->rules);

    const tj::Value& outline = get_array(v, "outline", where);
    for (size_t i = 0; i < outline.as_array().size(); i++)
        b->outline.push_back(get_polygon(outline.as_array()[i], where + ".outline[" + std::to_string(i) + "]", false));

    const tj::Value& pads = get_array(v, "pads", where);
    for (size_t i = 0; i < pads.as_array().size(); i++)
        b->pads.push_back(parse_pad(pads.as_array()[i], *b, where + ".pads[" + std::to_string(i) + "]"));
    const tj::Value& tracks = get_array(v, "tracks", where);
    for (size_t i = 0; i < tracks.as_array().size(); i++)
        b->tracks.push_back(parse_track(tracks.as_array()[i], *b, where + ".tracks[" + std::to_string(i) + "]"));
    const tj::Value& vias = get_array(v, "vias", where);
    for (size_t i = 0; i < vias.as_array().size(); i++)
        b->vias.push_back(parse_via(vias.as_array()[i], *b, where + ".vias[" + std::to_string(i) + "]"));
    const tj::Value& keepouts = get_array(v, "keepouts", where);
    for (size_t i = 0; i < keepouts.as_array().size(); i++)
        b->keepouts.push_back(
                parse_keepout(keepouts.as_array()[i], *b, where + ".keepouts[" + std::to_string(i) + "]"));

    const tj::Value& copper = get_array(v, "copper", where);
    for (size_t i = 0; i < copper.as_array().size(); i++)
        b->copper.push_back(parse_copper(copper.as_array()[i], *b, where + ".copper[" + std::to_string(i) + "]"));

    b->sort_items();
    b->check_unique_ids();
    return b;
}

static tj::Value point_json(const Pt& p)
{
    tj::Value a = tj::arr();
    a.push(p.x);
    a.push(p.y);
    return a;
}

tj::Value track_json(const Track& t, const Board& b)
{
    tj::Value o = tj::obj();
    o.set("kind", "track");
    o.set("id", t.id);
    o.set("net", t.net.empty() ? tj::Value() : tj::Value(t.net));
    o.set("layer", b.layers.at(t.layer));
    o.set("start", point_json(t.start));
    o.set("end", point_json(t.end));
    if (t.has_mid)
        o.set("mid", point_json(t.mid));
    o.set("width", t.width);
    return o;
}

tj::Value via_json(const Via& v, const Board& b)
{
    tj::Value o = tj::obj();
    o.set("kind", "via");
    o.set("id", v.id);
    o.set("net", v.net.empty() ? tj::Value() : tj::Value(v.net));
    o.set("position", point_json(v.pos));
    o.set("diameter", v.diameter);
    o.set("drill", v.drill);
    tj::Value ls = tj::arr();
    ls.push(b.layers.at(v.layer_from));
    ls.push(b.layers.at(v.layer_to));
    o.set("layers", ls);
    return o;
}

} // namespace tr
