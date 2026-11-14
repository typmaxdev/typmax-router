// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// The PNS host (world.hpp): the world sync, the commit capture and the
// proposal's order and ids. Every request builds a fresh PNS world from an
// immutable Board snapshot (sorted by id, so the world is built in the same
// order every time); the loaded board itself is never mutated here.
#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <tuple>

namespace tr {
namespace host {

// PNS layer = the board's layer index (0 = top). KiCad numbers its copper
// layers F_Cu = 0, B_Cu = 2, In1_Cu = 4, … (layer_ids.h); PNS asks for that
// numbering through GetBoardLayerFromPNSLayer only for length tuning.
PCB_LAYER_ID board_layer(int pns, int n)
{
    if (pns < 0 || pns >= n)
        return UNDEFINED_LAYER;
    if (pns == 0)
        return F_Cu;
    if (pns == n - 1)
        return B_Cu;
    return static_cast<PCB_LAYER_ID>((pns + 1) * 2);
}

void check_layer(int l, int n)
{
    if (l < 0 || l >= n)
        throw std::runtime_error("router produced an item on a layer the board does not have: " + std::to_string(l));
}
namespace {


// KiCad's rotation convention (PROTOCOL.md): y points down, positive angles
// turn counter-clockwise on screen: x' = x cos + y sin, y' = -x sin + y cos.
VECTOR2I rotate(int64_t x, int64_t y, double deg)
{
    double r = deg * M_PI / 180.0;
    double c = std::cos(r), s = std::sin(r);
    // snap the exact quarter turns so 90/180/270 stay exact integers
    double q = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
    if (q == 0) { c = 1; s = 0; }
    else if (q == 90) { c = 0; s = 1; }
    else if (q == 180) { c = -1; s = 0; }
    else if (q == 270) { c = 0; s = -1; }
    double xr = x * c + y * s;
    double yr = -x * s + y * c;
    return VECTOR2I(static_cast<int>(std::llround(xr)), static_cast<int>(std::llround(yr)));
}

bool quarter_turn(double deg, bool& swap)
{
    double q = std::fmod(std::fmod(deg, 360.0) + 360.0, 360.0);
    if (q == 0 || q == 180) {
        swap = false;
        return true;
    }
    if (q == 90 || q == 270) {
        swap = true;
        return true;
    }
    return false;
}

SHAPE* pad_shape(const Pad& p)
{
    const VECTOR2I c = vi(p.pos);
    switch (p.shape) {
    case PadShape::Circle: return new SHAPE_CIRCLE(c, static_cast<int>(std::max(p.w, p.h) / 2));
    case PadShape::Oval: {
        if (p.w == p.h)
            return new SHAPE_CIRCLE(c, static_cast<int>(p.w / 2));
        int64_t len = std::abs(p.w - p.h) / 2;
        int width = static_cast<int>(std::min(p.w, p.h));
        VECTOR2I d = p.w > p.h ? rotate(len, 0, p.rotation) : rotate(0, len, p.rotation);
        return new SHAPE_SEGMENT(c - d, c + d, width);
    }
    case PadShape::Rect:
    case PadShape::RoundRect: { // roundrect: its bounding rect (conservative), PROTOCOL.md
        bool swap = false;
        if (quarter_turn(p.rotation, swap)) {
            int w = static_cast<int>(swap ? p.h : p.w), h = static_cast<int>(swap ? p.w : p.h);
            return new SHAPE_RECT(c - VECTOR2I(w / 2, h / 2), w, h);
        }
        auto* s = new SHAPE_SIMPLE();
        const int64_t hx = p.w / 2, hy = p.h / 2;
        const int64_t corners[4][2] = {{-hx, -hy}, {hx, -hy}, {hx, hy}, {-hx, hy}};
        for (auto& k : corners)
            s->Append(c + rotate(k[0], k[1], p.rotation));
        return s;
    }
    case PadShape::Polygon: {
        auto* s = new SHAPE_SIMPLE();
        for (const auto& v : p.polygon)
            s->Append(c + rotate(v.x, v.y, p.rotation));
        return s;
    }
    }
    return nullptr;
}

std::string hex64(uint64_t h)
{
    char b[17];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

uint64_t fnv1a(const std::string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}
} // namespace

IFACE::IFACE(const Board& b) : m_board(b)
{
    // net codes 1..N in name order: the same board always yields the same codes
    int code = 0;
    for (const auto& n : b.net_names())
        m_nets.emplace(n, NETINFO_ITEM(++code, n));
    m_resolver = new RESOLVER(*this);
    m_virtual = make_parent<BOARD_ITEM>(Kind::VIRTUAL, "", PCB_SHAPE_T, UNDEFINED_LAYER);
}

IFACE::~IFACE() { delete m_resolver; }

PNS::RULE_RESOLVER* IFACE::GetRuleResolver() { return m_resolver; }

// Track length only, as PROTOCOL.md's `tune` says: every line, segment and arc
// of the path at its drawn length (an arc's along its sweep); vias add 0, pads
// add no pad-to-die, and no part inside a pad is clipped off.
long long IFACE::CalculateRoutedPathLength(const PNS::ITEM_SET& aLine, const PNS::SOLID*, const PNS::SOLID*,
                                           const NETCLASS*)
{
    long long total = 0;
    for (const PNS::ITEM* it : aLine.CItems()) {
        switch (it->Kind()) {
        case PNS::ITEM::LINE_T: total += static_cast<const PNS::LINE*>(it)->CLine().Length(); break;
        case PNS::ITEM::SEGMENT_T: total += static_cast<const PNS::SEGMENT*>(it)->Seg().Length(); break;
        case PNS::ITEM::ARC_T: total += std::llround(static_cast<const PNS::ARC*>(it)->CArc().GetLength()); break;
        default: break; // vias and pads: no track length
        }
    }
    return total;
}

// A polygon as KiCad syncs a keepout: its triangles, one locked SOLID each.
// IsKeepout makes them block every track and via, inside as well as across.
void IFACE::add_solid_poly(PNS::NODE* n, const std::vector<Pt>& poly, int layer, BOARD_ITEM* par,
                           PNS::NET_HANDLE net)
{
    SHAPE_POLY_SET ps;
    ps.NewOutline();
    for (const auto& p : poly)
        ps.Append(vi(p));
    ps.CacheTriangulation(false);
    if (!ps.IsTriangulationUpToDate()) {
        // a polygon KiCad cannot triangulate (self-intersecting): KiCad drops the
        // rule area; we keep its boundary, which no track may cross (PROTOCOL.md)
        for (size_t i = 0; i < poly.size(); i++) {
            const Pt& a = poly[i];
            const Pt& b = poly[(i + 1) % poly.size()];
            if (a == b)
                continue;
            auto s = std::make_unique<PNS::SOLID>();
            s->SetLayer(layer);
            s->SetNet(net);
            s->SetParent(par);
            s->SetShape(new SHAPE_SEGMENT(vi(a), vi(b), 0));
            s->SetIsCompoundShapePrimitive();
            s->SetRoutable(false);
            s->Mark(PNS::MK_LOCKED);
            n->Add(std::move(s));
        }
        return;
    }
    for (unsigned k = 0; k < ps.TriangulatedPolyCount(); k++) {
        const SHAPE_POLY_SET::TRIANGULATED_POLYGON* tri = ps.TriangulatedPolygon(k);
        for (size_t i = 0; i < tri->GetTriangleCount(); i++) {
            VECTOR2I a, b, c;
            tri->GetTriangle(i, a, b, c);
            auto* shape = new SHAPE_SIMPLE();
            shape->Append(a);
            shape->Append(b);
            shape->Append(c);
            auto s = std::make_unique<PNS::SOLID>();
            s->SetLayer(layer);
            s->SetNet(net);
            s->SetParent(par);
            s->SetShape(shape);
            s->SetIsCompoundShapePrimitive();
            s->SetRoutable(false);
            s->Mark(PNS::MK_LOCKED);
            n->Add(std::move(s));
        }
    }
}

void IFACE::SyncWorld(PNS::NODE* aNode)
{
    m_world = aNode;
    const int n = nlayers();
    for (const auto& p : m_board.pads) {
        auto s = std::make_unique<PNS::SOLID>();
        int lo = p.layers.front(), hi = p.layers.back();
        if (p.drill > 0) { // plated or not, a hole goes through every layer
            lo = 0;
            hi = n - 1;
        }
        PNS::NET_HANDLE h = net(p.net);
        PAD* par = make_parent<PAD>(Kind::PAD, p.id, static_cast<NETINFO_ITEM*>(h),
                                    p.drill > 0 && p.net.empty() ? PAD_ATTRIB::NPTH
                                    : p.drill > 0                ? PAD_ATTRIB::PTH
                                                                 : PAD_ATTRIB::SMD);
        s->SetLayers(PNS_LAYER_RANGE(lo, hi));
        s->SetNet(h);
        s->SetParent(par);
        s->SetOrientation(EDA_ANGLE(p.rotation, DEGREES_T));
        s->SetPos(vi(p.pos));
        s->SetOffset(VECTOR2I(0, 0));
        s->SetShape(pad_shape(p));
        if (p.drill > 0) {
            s->SetHole(new PNS::HOLE(new SHAPE_CIRCLE(vi(p.pos), static_cast<int>(p.drill / 2))));
            s->Hole()->SetLayers(PNS_LAYER_RANGE(0, n - 1));
        }
        s->SetRoutable(!p.net.empty());
        aNode->Add(std::move(s));
    }
    for (const auto& t : m_board.tracks) {
        PNS::NET_HANDLE h = net(t.net);
        if (t.has_mid) {
            PCB_ARC* par = make_parent<PCB_ARC>(Kind::TRACK, t.id, PCB_ARC_T, static_cast<NETINFO_ITEM*>(h),
                                                board_layer(t.layer, n));
            SHAPE_ARC sa(vi(t.start), vi(t.mid), vi(t.end), static_cast<int>(t.width));
            auto a = std::make_unique<PNS::ARC>(sa, h);
            a->SetWidth(static_cast<int>(t.width));
            a->SetLayer(t.layer);
            a->SetParent(par);
            if (t.locked)
                a->Mark(PNS::MK_LOCKED);
            aNode->Add(std::move(a));
        }
        else {
            PCB_TRACK* par = make_parent<PCB_TRACK>(Kind::TRACK, t.id, PCB_TRACE_T, static_cast<NETINFO_ITEM*>(h),
                                                    board_layer(t.layer, n));
            auto s = std::make_unique<PNS::SEGMENT>(SEG(vi(t.start), vi(t.end)), h);
            s->SetWidth(static_cast<int>(t.width));
            s->SetLayer(t.layer);
            s->SetParent(par);
            if (t.locked)
                s->Mark(PNS::MK_LOCKED);
            aNode->Add(std::move(s));
        }
    }
    for (const auto& v : m_board.vias) {
        PNS::NET_HANDLE h = net(v.net);
        bool through = v.layer_from == 0 && v.layer_to == n - 1;
        PCB_VIA* par = make_parent<PCB_VIA>(Kind::VIA, v.id, PCB_VIA_T, static_cast<NETINFO_ITEM*>(h), F_Cu);
        const PNS_LAYER_RANGE span(v.layer_from, v.layer_to);
        auto x = std::make_unique<PNS::VIA>(vi(v.pos), span, static_cast<int>(v.diameter), static_cast<int>(v.drill),
                                            h, through ? VIATYPE::THROUGH : VIATYPE::BLIND);
        x->SetParent(par);
        x->SetHole(PNS::HOLE::MakeCircularHole(vi(v.pos), static_cast<int>(v.drill / 2), span));
        if (v.locked)
            x->Mark(PNS::MK_LOCKED);
        aNode->Add(std::move(x));
    }
    for (const auto& c : m_board.copper) {
        // fixed copper (text, graphics): KiCad syncs text as a net-less SOLID
        PNS::NET_HANDLE h = c.net.empty() ? nullptr : net(c.net);
        BOARD_ITEM* par = make_parent<BOARD_ITEM>(Kind::COPPER, c.id, PCB_SHAPE_T, board_layer(c.layer, n));
        auto s = std::make_unique<PNS::SOLID>();
        s->SetLayer(c.layer);
        s->SetNet(h);
        auto* shape = new SHAPE_SIMPLE();
        int64_t sx = 0, sy = 0;
        for (const auto& p : c.polygon) {
            shape->Append(vi(p));
            sx += p.x;
            sy += p.y;
        }
        const int64_t k = static_cast<int64_t>(c.polygon.size());
        s->SetPos(VECTOR2I(static_cast<int>(sx / k), static_cast<int>(sy / k)));
        s->SetOffset(VECTOR2I(0, 0));
        s->SetShape(shape);
        s->SetRoutable(false);
        s->SetParent(par);
        s->Mark(PNS::MK_LOCKED);
        aNode->Add(std::move(s));
    }
    // the board edge as KiCad syncs Edge.Cuts: zero-width segments on every
    // layer, net-less (so they collide with every net), not routable
    BOARD_ITEM* edge = make_parent<BOARD_ITEM>(Kind::OUTLINE, "", PCB_SHAPE_T, Edge_Cuts);
    for (const auto& poly : m_board.outline) {
        for (size_t i = 0; i < poly.size(); i++) {
            const Pt& a = poly[i];
            const Pt& b = poly[(i + 1) % poly.size()];
            if (a == b)
                continue;
            auto s = std::make_unique<PNS::SOLID>();
            s->SetLayers(PNS_LAYER_RANGE(0, n - 1));
            s->SetRoutable(false);
            s->SetNet(nullptr);
            s->SetParent(edge);
            s->SetShape(new SHAPE_SEGMENT(vi(a), vi(b), 0));
            s->SetIsCompoundShapePrimitive();
            s->Mark(PNS::MK_LOCKED);
            aNode->Add(std::move(s));
        }
    }
    for (const auto& k : m_board.keepouts) {
        ZONE* par = make_parent<ZONE>(Kind::KEEPOUT, k.id, nullptr, true, wxString(k.id));
        for (int l : k.layers)
            add_solid_poly(aNode, k.polygon, l, par, nullptr);
    }

    aNode->SetRuleResolver(m_resolver);
    int64_t worst = std::max({m_board.rules.max_clearance(), m_board.rules.hole_clearance,
                              m_board.rules.hole_to_hole, m_board.rules.edge_clearance});
    aNode->SetMaxClearance(static_cast<int>(4 * std::max<int64_t>(worst, 1)));
}

void IFACE::capture_remove(PNS::ITEM* aItem)
{
    const Tag* t = tag(aItem->Parent());
    if (!t) { // an item this session created and dropped again: it is no longer added
        m_added_tracks.erase(aItem);
        m_added_vias.erase(aItem);
        return;
    }
    if (t->kind != Kind::TRACK && t->kind != Kind::VIA)
        throw std::runtime_error("router tried to remove a non-copper item: " + t->id);
    removed.insert(t->id);
}

std::vector<Track> IFACE::added_tracks() const
{
    std::vector<Track> out;
    for (const auto& [item, t] : m_added_tracks)
        out.push_back(t);
    return out; // in pointer order: make_proposal sorts
}

std::vector<Via> IFACE::added_vias() const
{
    std::vector<Via> out;
    for (const auto& [item, v] : m_added_vias)
        out.push_back(v);
    return out;
}

void IFACE::capture_add(PNS::ITEM* aItem)
{
    const int n = nlayers();
    switch (aItem->Kind()) {
    case PNS::ITEM::SEGMENT_T: {
        auto* s = static_cast<PNS::SEGMENT*>(aItem);
        Track t;
        t.net = net_name(s->Net());
        t.layer = s->Layer();
        check_layer(t.layer, n);
        t.start = pt(s->Seg().A);
        t.end = pt(s->Seg().B);
        t.width = s->Width();
        m_added_tracks[aItem] = t;
        break;
    }
    case PNS::ITEM::ARC_T: {
        auto* a = static_cast<PNS::ARC*>(aItem);
        const SHAPE_ARC& sa = a->CArc();
        Track t;
        t.net = net_name(a->Net());
        t.layer = a->Layer();
        check_layer(t.layer, n);
        t.start = pt(sa.GetP0());
        t.mid = pt(sa.GetArcMid());
        t.end = pt(sa.GetP1());
        t.has_mid = true;
        t.width = a->Width();
        m_added_tracks[aItem] = t;
        break;
    }
    case PNS::ITEM::VIA_T: {
        auto* x = static_cast<PNS::VIA*>(aItem);
        Via v;
        v.net = net_name(x->Net());
        v.pos = pt(x->Pos());
        v.diameter = x->Diameter(x->Layers().Start());
        v.drill = x->Drill();
        v.layer_from = x->Layers().Start();
        v.layer_to = x->Layers().End();
        if (v.layer_from > v.layer_to)
            std::swap(v.layer_from, v.layer_to);
        check_layer(v.layer_from, n);
        check_layer(v.layer_to, n);
        m_added_vias[aItem] = v;
        break;
    }
    default: break; // solids are never added by routing or dragging
    }
}

std::string track_key(const Track& t)
{
    char b[256];
    std::snprintf(b, sizeof b, "track|%d|%lld,%lld|%lld,%lld|%d|%lld,%lld|%lld|", t.layer, (long long)t.start.x,
                  (long long)t.start.y, (long long)t.end.x, (long long)t.end.y, t.has_mid ? 1 : 0,
                  (long long)t.mid.x, (long long)t.mid.y, (long long)t.width);
    return b + t.net;
}

std::string via_key(const Via& v)
{
    char b[256];
    std::snprintf(b, sizeof b, "via|%lld,%lld|%lld|%lld|%d-%d|", (long long)v.pos.x, (long long)v.pos.y,
                  (long long)v.diameter, (long long)v.drill, v.layer_from, v.layer_to);
    return b + v.net;
}

bool track_less(const Track& a, const Track& b)
{
    auto ka = std::make_tuple(a.net, a.layer, a.start.x, a.start.y, a.end.x, a.end.y, a.width, a.has_mid, a.mid.x,
                              a.mid.y);
    auto kb = std::make_tuple(b.net, b.layer, b.start.x, b.start.y, b.end.x, b.end.y, b.width, b.has_mid, b.mid.x,
                              b.mid.y);
    return ka < kb;
}

bool via_less(const Via& a, const Via& b)
{
    return std::make_tuple(a.net, a.pos.x, a.pos.y, a.layer_from, a.layer_to, a.diameter, a.drill)
           < std::make_tuple(b.net, b.pos.x, b.pos.y, b.layer_from, b.layer_to, b.diameter, b.drill);
}

// Deterministic proposal: PNS hands the batch over in pointer order (an earlier prototype's
// finding), so sort, then give each new item an id hashed from its geometry.
Proposal make_proposal(const IFACE& io, const Board& b)
{
    Proposal p;
    p.remove.assign(io.removed.begin(), io.removed.end()); // std::set: sorted
    p.add_tracks = io.added_tracks();
    p.add_vias = io.added_vias();
    std::sort(p.add_tracks.begin(), p.add_tracks.end(), track_less);
    std::sort(p.add_vias.begin(), p.add_vias.end(), via_less);
    std::set<std::string> taken;
    for (const auto& t : b.tracks)
        taken.insert(t.id);
    for (const auto& v : b.vias)
        taken.insert(v.id);
    for (const auto& x : b.pads)
        taken.insert(x.id);
    for (const auto& k : b.keepouts)
        taken.insert(k.id);
    for (const auto& c : b.copper)
        taken.insert(c.id);
    for (const auto& id : p.remove)
        taken.erase(id);
    auto fresh = [&taken](const std::string& base) {
        std::string id = base;
        for (int k = 1; taken.count(id); k++)
            id = base + "-" + std::to_string(k);
        taken.insert(id);
        return id;
    };
    for (auto& t : p.add_tracks)
        t.id = fresh("t" + hex64(fnv1a(track_key(t))));
    for (auto& v : p.add_vias)
        v.id = fresh("v" + hex64(fnv1a(via_key(v))));
    return p;
}

PNS::PNS_MODE pns_mode(Mode m)
{
    switch (m) {
    case Mode::Walkaround: return PNS::RM_Walkaround;
    case Mode::Shove: return PNS::RM_Shove;
    case Mode::MarkObstacles: return PNS::RM_MarkObstacles;
    }
    return PNS::RM_Walkaround;
}


// An engine refusal: our message, and PNS's own reason (if any) as `detail`.
RequestError refusal(PNS::ROUTER& r, const std::string& code, const std::string& msg)
{
    return RequestError(code, msg, std::string(r.FailureReason()));
}

// The placer's current trace end, if any.
std::optional<VECTOR2I> trace_end(PNS::ROUTER& r)
{
    if (!r.Placer())
        return std::nullopt;
    PNS::ITEM_SET ts = r.Placer()->Traces();
    for (PNS::ITEM* it : ts.Items()) {
        if (it->Kind() == PNS::ITEM::LINE_T) {
            auto* l = static_cast<PNS::LINE*>(it);
            if (l->PointCount() > 0)
                return l->CPoint(-1);
        }
    }
    return std::nullopt;
}

int trace_points(PNS::ROUTER& r)
{
    if (!r.Placer())
        return 0;
    PNS::ITEM_SET ts = r.Placer()->Traces();
    for (PNS::ITEM* it : ts.Items())
        if (it->Kind() == PNS::ITEM::LINE_T)
            return static_cast<PNS::LINE*>(it)->PointCount();
    return 0;
}

// Same-net hole-to-hole (review M-2): a drill does not care about nets, and
// KiCad's DRC `hole_to_hole` has no net test. KiCad 10's PNS checks every hole
// pair whatever the nets (ITEM::collideSimple); this check is the service's own
// backstop, independent of the engine: every hole the proposal adds (a new or
// moved via) against every other hole of the resulting board on the same net,
// or with no net. Different-net pairs are left to the engine. Exact integer
// arithmetic, in half-nm units.
void check_same_net_holes(const Board& b, const Proposal& p)
{
    struct Hole {
        Pt c;
        int64_t drill;
        std::string net, id;
    };
    std::set<std::string> removed(p.remove.begin(), p.remove.end());
    std::vector<Hole> fixed, added;
    for (const auto& x : b.pads)
        if (x.drill > 0)
            fixed.push_back({x.pos, x.drill, x.net, x.id});
    for (const auto& v : b.vias)
        if (!removed.count(v.id))
            fixed.push_back({v.pos, v.drill, v.net, v.id});
    for (const auto& v : p.add_vias)
        added.push_back({v.pos, v.drill, v.net, v.id});
    const int64_t h2h = b.rules.hole_to_hole;
    auto too_close = [h2h](const Hole& a, const Hole& c) {
        if (!a.net.empty() && !c.net.empty() && a.net != c.net)
            return false; // different nets: PNS enforced it
        const __int128 dx = 2 * static_cast<__int128>(a.c.x - c.c.x);
        const __int128 dy = 2 * static_cast<__int128>(a.c.y - c.c.y);
        const __int128 need = static_cast<__int128>(a.drill) + c.drill + 2 * static_cast<__int128>(h2h);
        return dx * dx + dy * dy < need * need;
    };
    auto refuse = [h2h](const Hole& a, const Hole& c) {
        throw RequestError("hole_too_close",
                           "the hole of via \"" + a.id + "\" is closer than hole_to_hole (" + std::to_string(h2h)
                                   + " nm) to the hole of \"" + c.id + "\" on the same net; the engine does not "
                                   "check same-net holes, the service does");
    };
    for (size_t i = 0; i < added.size(); i++) {
        for (const auto& f : fixed)
            if (too_close(added[i], f))
                refuse(added[i], f);
        for (size_t j = i + 1; j < added.size(); j++)
            if (too_close(added[i], added[j]))
                refuse(added[i], added[j]);
    }
}

} // namespace host
} // namespace tr
