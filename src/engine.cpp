// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// PNS host (KiCad's push-and-shove router, vendor/kicad). Every request builds a fresh PNS world from an immutable Board
// snapshot (sorted by id, so the world is built in the same order every time),
// runs one placer/dragger session, and captures PNS's commit as a proposal.
// The loaded board itself is never mutated here.
#include "engine.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <set>

#include <memory>
#include <utility>

// KiCad (vendor/kicad, at the tag vendor/kicad.manifest names)
#include <geometry/eda_angle.h>
#include <geometry/shape_arc.h>
#include <geometry/shape_circle.h>
#include <geometry/shape_poly_set.h>
#include <geometry/shape_rect.h>
#include <geometry/shape_segment.h>
#include <geometry/shape_simple.h>
#include <layer_ids.h>
#include <pns_arc.h>
#include <pns_debug_decorator.h>
#include <pns_hole.h>
#include <pns_line.h>
#include <pns_node.h>
#include <pns_placement_algo.h>
#include <pns_router.h>
#include <pns_routing_settings.h>
#include <pns_segment.h>
#include <pns_sizes_settings.h>
#include <pns_solid.h>
#include <pns_via.h>
// the board model's stand-ins (shim/include)
#include <board_item.h>
#include <netinfo.h>
#include <pad.h>
#include <pcb_track.h>
#include <zone.h>

namespace tr {

double g_last_world_ms = 0;

namespace {

// Every PNS item the world holds has a BOARD_ITEM parent (shim/include): PNS
// reads its type and layer (ROUTER::StartRouting, LINE_PLACER's start
// posture, the edge test in ITEM::collideSimple), and a commit hands back the
// items it touched by parent. A parent's TAG says which board item it is.
enum class Kind { PAD, TRACK, VIA, OUTLINE, KEEPOUT, COPPER, VIRTUAL };

struct Tag {
    Kind kind;
    std::string id;
};

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

VECTOR2I vi(const Pt& p) { return VECTOR2I(static_cast<int>(p.x), static_cast<int>(p.y)); }
Pt pt(const VECTOR2I& v) { return Pt{v.x, v.y}; }

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

class RESOLVER;

class IFACE : public PNS::ROUTER_IFACE {
public:
    IFACE(const Board& b);
    ~IFACE() override;

    // ROUTER_IFACE
    void SyncWorld(PNS::NODE* aNode) override;
    void AddItem(PNS::ITEM* aItem) override { capture_add(aItem); }
    void UpdateItem(PNS::ITEM* aItem) override
    {
        capture_remove(aItem);
        capture_add(aItem);
    }
    void RemoveItem(PNS::ITEM* aItem) override { capture_remove(aItem); }
    bool IsAnyLayerVisible(const PNS_LAYER_RANGE&) const override { return true; }
    bool IsItemVisible(const PNS::ITEM*) const override { return true; }
    bool IsFlashedOnLayer(const PNS::ITEM*, int) const override { return true; }
    bool IsFlashedOnLayer(const PNS::ITEM*, const PNS_LAYER_RANGE&) const override { return true; }
    bool IsPNSCopperLayer(int l) const override { return l >= 0 && l < nlayers(); }
    void DisplayItem(const PNS::ITEM*, int, bool, int) override {}
    void DisplayPathLine(const SHAPE_LINE_CHAIN&, int) override {}
    void DisplayRatline(const SHAPE_LINE_CHAIN&, PNS::NET_HANDLE) override {}
    void HideItem(PNS::ITEM*) override {}
    void Commit() override {}
    bool ImportSizes(PNS::SIZES_SETTINGS&, PNS::ITEM*, PNS::NET_HANDLE, VECTOR2D) override { return true; }
    int StackupHeight(int, int) const override { return 0; }
    void EraseView() override {}
    int GetNetCode(PNS::NET_HANDLE h) const override
    {
        return h ? static_cast<const NETINFO_ITEM*>(h)->GetNetCode() : -1;
    }
    wxString GetNetName(PNS::NET_HANDLE h) const override
    {
        return h ? static_cast<const NETINFO_ITEM*>(h)->GetNetname() : wxString();
    }
    void UpdateNet(PNS::NET_HANDLE) override {}
    PNS::NET_HANDLE GetOrphanedNetHandle() override { return &m_orphan; }
    PNS::NODE* GetWorld() const override { return m_world; }
    PNS::RULE_RESOLVER* GetRuleResolver() override;
    PNS::DEBUG_DECORATOR* GetDebugDecorator() override { return nullptr; }
    // length tuning only; the service does not tune
    long long CalculateRoutedPathLength(const PNS::ITEM_SET&, const PNS::SOLID*, const PNS::SOLID*,
                                        const NETCLASS*) override { return 0; }
    int64_t CalculateRoutedPathDelay(const PNS::ITEM_SET&, const PNS::SOLID*, const PNS::SOLID*,
                                     const NETCLASS*) override { return 0; }
    int64_t CalculateLengthForDelay(int64_t, int, bool, int, int, const NETCLASS*) override { return 0; }
    int64_t CalculateDelayForShapeLineChain(const SHAPE_LINE_CHAIN&, int, bool, int, int,
                                            const NETCLASS*) override { return 0; }
    PCB_LAYER_ID GetBoardLayerFromPNSLayer(int l) const override { return board_layer(l, nlayers()); }
    int GetPNSLayerFromBoardLayer(PCB_LAYER_ID l) const override
    {
        for (int i = 0; i < nlayers(); i++)
            if (board_layer(i, nlayers()) == l)
                return i;
        return -1;
    }

    // no net (empty name) -> KiCad's orphaned net: no-net items never collide
    // with each other (PROTOCOL.md's guarantees table); unknown -> null
    PNS::NET_HANDLE net(const std::string& name)
    {
        if (name.empty())
            return &m_orphan;
        auto it = m_nets.find(name);
        return it == m_nets.end() ? nullptr : &it->second;
    }
    std::string net_name(PNS::NET_HANDLE h) const
    {
        return h ? std::string(static_cast<const NETINFO_ITEM*>(h)->GetNetname()) : std::string();
    }
    const NetClass& net_class(PNS::NET_HANDLE h) const { return m_board.rules.class_for(net_name(h)); }
    const Tag* tag(const BOARD_ITEM* p) const
    {
        if (!p)
            return nullptr;
        auto it = m_tags.find(p);
        return it == m_tags.end() ? nullptr : &it->second;
    }
    BOARD_ITEM* parent(const std::string& id) const
    {
        auto it = m_by_id.find(id);
        return it == m_by_id.end() ? nullptr : it->second;
    }
    BOARD_ITEM* virtual_parent() { return m_virtual; }
    const Board& board() const { return m_board; }
    int nlayers() const { return static_cast<int>(m_board.layers.size()); }

    std::set<std::string> removed;
    std::vector<Track> added_tracks;
    std::vector<Via> added_vias;

private:
    template <typename T, typename... A>
    T* make_parent(Kind k, const std::string& id, A&&... a)
    {
        auto p = std::make_unique<T>(std::forward<A>(a)...);
        T* raw = p.get();
        m_parents.push_back(std::move(p));
        m_tags[raw] = Tag{k, id};
        if (k != Kind::OUTLINE && k != Kind::VIRTUAL)
            m_by_id[id] = raw;
        return raw;
    }
    void add_solid_poly(PNS::NODE* n, const std::vector<Pt>& poly, int layer, BOARD_ITEM* par, PNS::NET_HANDLE net);
    void capture_add(PNS::ITEM* aItem);
    void capture_remove(PNS::ITEM* aItem);

    const Board& m_board;
    PNS::NODE* m_world = nullptr;
    NETINFO_ITEM m_orphan{0, ""};
    std::map<std::string, NETINFO_ITEM> m_nets;
    std::vector<std::unique_ptr<BOARD_ITEM>> m_parents;
    std::map<const BOARD_ITEM*, Tag> m_tags;
    std::map<std::string, BOARD_ITEM*> m_by_id;
    BOARD_ITEM* m_virtual = nullptr;
    RESOLVER* m_resolver = nullptr;
};

// The rules, in the shape of KiCad's own resolver (pns_kicad_iface.cpp,
// PNS_PCBNEW_RULE_RESOLVER::Clearance) over the board's net classes: a hole
// pair takes hole_to_hole (whatever the nets), a hole against copper of
// another net hole_clearance, copper against copper of another net the larger
// class clearance, anything against the board edge edge_clearance; the
// largest that applies wins. Same-net pairs answer -1 (no clearance).
//
// The clearance EPSILON is KiCad's too (BOARD_DESIGN_SETTINGS::GetDRCEpsilon,
// ADVANCED_CFG m_DRCEpsilon = 0.5 um): PNS builds its walkaround hulls at the
// full clearance and asks for collisions with the epsilon taken off, so a path
// that hugs a hull is not read back as colliding with it. KiCad's DRC accepts
// the same 0.5 um (PROTOCOL.md, the guarantees table).
constexpr int CLEARANCE_EPSILON = 500; // nm

class RESOLVER : public PNS::RULE_RESOLVER {
public:
    explicit RESOLVER(IFACE& i) : m_iface(i) {}

    int ClearanceEpsilon() const override { return CLEARANCE_EPSILON; }

    int Clearance(const PNS::ITEM* a, const PNS::ITEM* b, bool aUseEpsilon) override
    {
        int rv = clearance(a, b);
        if (aUseEpsilon && rv > 0)
            rv = std::max(0, rv - CLEARANCE_EPSILON);
        return rv;
    }

    int clearance(const PNS::ITEM* a, const PNS::ITEM* b)
    {
        const Rules& r = m_iface.board().rules;
        if (!b)
            return static_cast<int>(m_iface.net_class(a->Net()).clearance);
        const bool same_net = a->Net() && a->Net() == b->Net();
        int rv = 0;
        if (is_hole(a) && is_hole(b))
            rv = std::max(rv, static_cast<int>(r.hole_to_hole));
        else if ((is_hole(a) || is_hole(b)) && !same_net)
            rv = std::max(rv, static_cast<int>(r.hole_clearance));
        if (is_copper(a) && is_copper(b) && !same_net)
            rv = std::max(rv, static_cast<int>(std::max(m_iface.net_class(a->Net()).clearance,
                                                        m_iface.net_class(b->Net()).clearance)));
        if (is_edge(a) || is_edge(b))
            rv = std::max(rv, static_cast<int>(r.edge_clearance));
        if (same_net && rv == 0)
            rv = -1;
        return rv;
    }
    PNS::NET_HANDLE DpCoupledNet(PNS::NET_HANDLE) override { return nullptr; }
    int DpNetPolarity(PNS::NET_HANDLE) override { return 0; }
    bool DpNetPair(const PNS::ITEM*, PNS::NET_HANDLE&, PNS::NET_HANDLE&) override { return false; }
    int NetCode(PNS::NET_HANDLE h) override { return m_iface.GetNetCode(h); }
    wxString NetName(PNS::NET_HANDLE h) override { return m_iface.GetNetName(h); }
    bool IsInNetTie(const PNS::ITEM*) override { return false; }
    bool IsNetTieExclusion(const PNS::ITEM*, const VECTOR2I&, const PNS::ITEM*) override { return false; }
    bool IsDrilledHole(const PNS::ITEM* a) override { return is_hole(a); }
    bool IsNonPlatedSlot(const PNS::ITEM*) override { return false; } // holes are round
    // A keepout blocks every track and via inside it (KiCad: a rule area with
    // "no tracks" and "no vias"); a pad inside one was put there by the board.
    bool IsKeepout(const PNS::ITEM* aObstacle, const PNS::ITEM* aItem, bool* aEnforce) override
    {
        const Tag* t = m_iface.tag(aObstacle->Parent());
        if (!t || t->kind != Kind::KEEPOUT)
            return false;
        const Tag* o = m_iface.tag(aItem->BoardItem());
        *aEnforce = aItem->OfKind(PNS::ITEM::SEGMENT_T | PNS::ITEM::ARC_T | PNS::ITEM::VIA_T | PNS::ITEM::LINE_T
                                  | PNS::ITEM::HOLE_T)
                    && !(o && (o->kind == Kind::PAD || o->kind == Kind::KEEPOUT || o->kind == Kind::OUTLINE
                               || o->kind == Kind::COPPER));
        return true;
    }
    bool QueryConstraint(PNS::CONSTRAINT_TYPE t, const PNS::ITEM* a, const PNS::ITEM* b, int,
                         PNS::CONSTRAINT* c) override
    {
        const Rules& r = m_iface.board().rules;
        int v;
        switch (t) {
        case PNS::CONSTRAINT_TYPE::CT_CLEARANCE:
            if (!a)
                return false;
            v = b ? std::max(clearance(a, b), 0) : clearance(a, nullptr);
            break;
        case PNS::CONSTRAINT_TYPE::CT_HOLE_CLEARANCE: v = static_cast<int>(r.hole_clearance); break;
        case PNS::CONSTRAINT_TYPE::CT_HOLE_TO_HOLE: v = static_cast<int>(r.hole_to_hole); break;
        case PNS::CONSTRAINT_TYPE::CT_EDGE_CLEARANCE: v = static_cast<int>(r.edge_clearance); break;
        default: return false;
        }
        c->m_Type = t;
        c->m_Value.SetMin(v);
        return true;
    }

private:
    static bool is_hole(const PNS::ITEM* a) { return a && a->OfKind(PNS::ITEM::HOLE_T); }
    bool is_edge(const PNS::ITEM* a) const
    {
        const Tag* t = a ? m_iface.tag(a->Parent()) : nullptr;
        return t && t->kind == Kind::OUTLINE;
    }
    // as KiCad: an item with no parent (one the router made) or a parent on copper
    bool is_copper(const PNS::ITEM* a) const
    {
        const Tag* t = m_iface.tag(a->Parent());
        return !t || (t->kind != Kind::OUTLINE && t->kind != Kind::KEEPOUT);
    }

    IFACE& m_iface;
};

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
    if (!t)
        return; // an item this session created and dropped again
    if (t->kind != Kind::TRACK && t->kind != Kind::VIA)
        throw std::runtime_error("router tried to remove a non-copper item: " + t->id);
    removed.insert(t->id);
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
        added_tracks.push_back(t);
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
        added_tracks.push_back(t);
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
        added_vias.push_back(v);
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
    p.add_tracks = io.added_tracks;
    p.add_vias = io.added_vias;
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

// One router session over a fresh world.
struct Session {
    IFACE iface;
    PNS::ROUTING_SETTINGS settings{nullptr, ""};
    PNS::ROUTER router;

    Session(const Board& b, Mode m, bool remove_loops) : iface(b)
    {
        settings.SetMode(pns_mode(m));
        settings.SetRemoveLoops(remove_loops);
        // Shove's own 1000 ms wall-clock cap would make a shove's answer depend
        // on the machine's load (review D-1). Off: shove stops on its iteration
        // limit (250) alone, and the service's time limit bounds the request.
        // KiCad keeps the cap private; its persistent parameter
        // "shove_time_limit" is the one way in (shim/include/settings), and
        // shim/time_limit.cpp reads INT_MAX ms as "no limit".
        if (!settings.Set("shove_time_limit", INT_MAX))
            throw std::runtime_error("ROUTING_SETTINGS has no shove_time_limit parameter");
        router.SetInterface(&iface);
        router.LoadSettings(&settings);
        auto t0 = std::chrono::steady_clock::now();
        router.SyncWorld();
        g_last_world_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    PNS::ITEM* item_of(const std::string& id)
    {
        BOARD_ITEM* p = iface.parent(id);
        return p ? router.GetWorld()->FindItemByParent(p) : nullptr;
    }

    // A free-point start on no copper of the net: PNS takes a line's net from its
    // start ITEM only (pns_line_placer.cpp, LINE_PLACER::Start), so a bare point
    // would route a NET-LESS line (review M-1: every pad of the net an obstacle,
    // the Default class's clearance, "net": null in the proposal). Give the
    // placer a start item of the requested net instead: a routable point SOLID
    // on the route's layer. Its parent is the session's VIRTUAL item (PNS reads
    // a start solid's parent type), which is never captured into a proposal and
    // which no free point can resolve to.
    PNS::ITEM* virtual_start(const VECTOR2I& p, PNS::NET_HANDLE net, int pns_layer)
    {
        auto solid = std::make_unique<PNS::SOLID>();
        solid->SetLayer(pns_layer);
        solid->SetNet(net);
        solid->SetPos(p);
        solid->SetOffset(VECTOR2I(0, 0));
        solid->SetShape(new SHAPE_CIRCLE(p, 0));
        solid->SetRoutable(true);
        solid->SetParent(iface.virtual_parent());
        PNS::ITEM* raw = solid.get();
        router.GetWorld()->Add(std::move(solid));
        return raw;
    }

    // The item a free point lands on: same net, on the layer, lowest kind then id.
    PNS::ITEM* item_at(const Pt& p, PNS::NET_HANDLE net, int pns_layer)
    {
        PNS::ITEM* best = nullptr;
        std::string best_key;
        PNS::ITEM_SET hits = router.QueryHoverItems(vi(p));
        for (PNS::ITEM* it : hits.Items()) {
            if (it->Net() != net || !it->Layers().Overlaps(pns_layer))
                continue;
            const Tag* t = iface.tag(it->Parent());
            if (!t || (t->kind != Kind::PAD && t->kind != Kind::TRACK && t->kind != Kind::VIA))
                continue;
            std::string key = std::to_string(static_cast<int>(t->kind)) + "|" + t->id;
            if (!best || key < best_key) {
                best = it;
                best_key = key;
            }
        }
        return best;
    }
};

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

} // namespace

Proposal run_route(const Board& b, const RouteRequest& rq)
{
    const int n = static_cast<int>(b.layers.size());
    Session s(b, rq.mode, rq.remove_loops);
    PNS::NET_HANDLE net = rq.net.empty() ? nullptr : s.iface.net(rq.net);
    if (!net)
        throw RequestError("unknown_net", "net \"" + rq.net + "\" is on no item of the loaded board");
    const NetClass& nc = b.rules.class_for(rq.net);

    // the via the route would place, after the net class's defaults: it must be one
    // `apply` (board.cpp parse_via) and the board's own classes accept (review r3 E-1)
    const int64_t via_diameter = rq.via_diameter.value_or(nc.via_diameter);
    const int64_t via_drill = rq.via_drill.value_or(nc.via_drill);
    if (via_drill >= via_diameter)
        throw RequestError("invalid_params", "via_drill (" + std::to_string(via_drill)
                           + " nm) must be smaller than via_diameter (" + std::to_string(via_diameter)
                           + " nm), after the net class's defaults");

    PNS::SIZES_SETTINGS sz;
    sz.SetTrackWidth(static_cast<int>(rq.width.value_or(nc.track_width)));
    sz.SetTrackWidthIsExplicit(true);
    sz.SetViaDiameter(static_cast<int>(via_diameter));
    sz.SetViaDrill(static_cast<int>(via_drill));
    sz.SetViaType(VIATYPE::THROUGH);
    sz.SetMinClearance(static_cast<int>(nc.clearance));
    sz.SetHoleToHole(static_cast<int>(b.rules.hole_to_hole));
    // a via spans the whole stack (SIZES_SETTINGS's own default pair is KiCad's
    // F_Cu/B_Cu numbering, not PNS layers)
    sz.ClearLayerPairs();
    sz.AddLayerPair(0, n - 1);
    s.router.UpdateSizes(sz);

    int layer = rq.layer;

    auto resolve = [&](const Anchor& a, const char* which, int on_layer) -> std::pair<VECTOR2I, PNS::ITEM*> {
        if (a.is_pad) {
            const Pad* pad = b.find_pad(a.pad_id);
            if (!pad)
                throw RequestError("unknown_item", std::string(which) + ": no pad \"" + a.pad_id + "\"");
            if (pad->net != rq.net)
                throw RequestError("net_mismatch", std::string(which) + ": pad \"" + a.pad_id + "\" is on net \""
                                                           + pad->net + "\", not \"" + rq.net + "\"");
            PNS::ITEM* it = s.item_of(pad->id);
            if (!it)
                throw std::runtime_error("pad missing from the router world: " + pad->id);
            if (!it->Layers().Overlaps(on_layer))
                throw RequestError("layer_mismatch", std::string(which) + ": pad \"" + a.pad_id
                                                             + "\" is not on layer " + b.layers[on_layer]);
            return {vi(pad->pos), it};
        }
        return {vi(a.point), s.item_at(a.point, net, on_layer)};
    };

    auto [p0, startItem] = resolve(rq.start, "start", layer);
    if (!startItem)
        startItem = s.virtual_start(p0, net, layer);
    if (!s.router.StartRouting(p0, startItem, layer))
        throw refusal(s.router, "start_not_routable", "PNS refused to start routing here");

    auto stop = [&s]() {
        if (s.router.RoutingInProgress())
            s.router.StopRouting();
    };

    // feed the cursor from `from` toward `to` (exclusive) in steps of rq.step
    auto approach = [&](const VECTOR2I& from, const VECTOR2I& to) {
        if (rq.step <= 0)
            return;
        double len = (to - from).EuclideanNorm();
        int n = static_cast<int>(std::min<double>(std::ceil(len / static_cast<double>(rq.step)), 2000.0));
        for (int k = 1; k < n; k++) {
            VECTOR2I p(from.x + static_cast<int>(std::llround(static_cast<double>(to.x - from.x) * k / n)),
                       from.y + static_cast<int>(std::llround(static_cast<double>(to.y - from.y) * k / n)));
            s.router.Move(p, nullptr);
        }
    };

    VECTOR2I last = p0;
    for (size_t i = 0; i < rq.waypoints.size(); i++) {
        const Waypoint& w = rq.waypoints[i];
        VECTOR2I wp = vi(w.point);
        // A via where the trace already ends leaves PNS an empty head line and
        // NODE::NearestObstacle dereferences it (review r2 N-1): refuse it.
        if (w.via && wp == last) {
            stop();
            throw RequestError("invalid_params", "waypoint " + std::to_string(i)
                                                     + " asks for a via where the trace already is; move first");
        }
        approach(last, wp);
        s.router.Move(wp, nullptr);
        if (w.via) {
            if (!rq.via_allowed) {
                stop();
                throw RequestError("via_not_allowed", "waypoint " + std::to_string(i) + " asks for a via but via_allowed is false");
            }
            s.router.ToggleViaPlacement();
            s.router.Move(wp, nullptr);
        }
        auto e = trace_end(s.router);
        if (!e || *e != wp) {
            stop();
            throw RequestError("unroutable", "could not reach waypoint " + std::to_string(i)
                                                     + (rq.mode == Mode::Walkaround ? " (walkaround)" : ""));
        }
        // A non-final FixRoute returns false by design (it returns "reached the real
        // end"); success shows as an emptied head, failure leaves the trace in place.
        s.router.FixRoute(wp, nullptr, false, false);
        if (trace_points(s.router) > 1) {
            stop();
            throw refusal(s.router, rq.mode == Mode::MarkObstacles ? "collision" : "unroutable", "could not fix the route at waypoint " + std::to_string(i));
        }
        if (w.via) {
            layer = w.layer_after;
            if (!s.router.SwitchLayer(layer)) {
                stop();
                throw RequestError("unroutable", "could not switch to layer " + b.layers[w.layer_after]
                                                         + " after the via at waypoint " + std::to_string(i));
            }
        }
        last = wp;
    }

    VECTOR2I pend = last;
    PNS::ITEM* endItem = nullptr;
    if (rq.end) {
        auto r = resolve(*rq.end, "end", layer);
        pend = r.first;
        endItem = r.second;
        approach(last, pend);
        s.router.Move(pend, endItem);
        auto e = trace_end(s.router);
        bool reached = e && *e == pend;
        if (!reached && e && endItem && endItem->Shape(layer) && endItem->Shape(layer)->Collide(*e, 0))
            reached = true; // smart-pad ending on the pad's copper
        if (!reached) {
            stop();
            throw RequestError("unroutable", "PNS could not reach the end point in mode "
                                                     + std::string(rq.mode == Mode::Walkaround ? "walkaround"
                                                                   : rq.mode == Mode::Shove ? "shove"
                                                                                            : "mark_obstacles"));
        }
        if (!s.router.FixRoute(pend, endItem, true, false)) {
            stop();
            throw refusal(s.router, rq.mode == Mode::MarkObstacles ? "collision" : "unroutable", "PNS could not fix the route");
        }
    }
    s.router.CommitRouting();
    stop();
    Proposal p = make_proposal(s.iface, b);
    check_same_net_holes(b, p);
    return p;
}

Proposal run_drag(const Board& b, const DragRequest& rq)
{
    Session s(b, rq.mode, true);
    const Track* t = b.find_track(rq.item);
    const Via* v = t ? nullptr : b.find_via(rq.item);
    if (!t && !v)
        throw RequestError("unknown_item", "no track or via \"" + rq.item + "\" (pads are not draggable here)");
    const std::string& net = t ? t->net : v->net;
    if ((t && t->locked) || (v && v->locked))
        throw RequestError("locked", "item \"" + rq.item + "\" is locked");
    // PNS never lets two no-net items collide (they share KiCad's orphaned net,
    // and same-net pairs are skipped), so a dragged no-net item would cross no-net copper and
    // NPTH holes freely (review M-5): refuse it rather than propose that.
    if (net.empty())
        throw RequestError("drag_refused", "item \"" + rq.item + "\" has no net; the engine cannot keep a "
                                           "no-net item clear of other no-net copper, so it is not draggable");
    PNS::ITEM* it = s.item_of(rq.item);
    if (!it)
        throw std::runtime_error("item missing from the router world: " + rq.item);
    VECTOR2I from;
    if (rq.from)
        from = vi(*rq.from);
    else if (t)
        from = VECTOR2I(static_cast<int>((t->start.x + t->end.x) / 2), static_cast<int>((t->start.y + t->end.y) / 2));
    else
        from = vi(v->pos);
    if (!s.router.StartDragging(from, it, PNS::DM_ANY))
        throw refusal(s.router, "drag_refused", "PNS refused to drag this item");
    s.router.Move(vi(rq.to), nullptr);
    bool ok = s.router.FixRoute(vi(rq.to), nullptr, true, false);
    if (s.router.RoutingInProgress())
        s.router.StopRouting();
    if (!ok)
        throw refusal(s.router, rq.mode == Mode::MarkObstacles ? "collision" : "unroutable", "PNS could not complete the drag");
    Proposal p = make_proposal(s.iface, b);
    check_same_net_holes(b, p);
    return p;
}

std::shared_ptr<Board> apply_proposal(const Board& b, const Proposal& p)
{
    auto nb = std::make_shared<Board>(b);
    std::set<std::string> rm(p.remove.begin(), p.remove.end());
    for (const auto& id : rm)
        if (!b.find_track(id) && !b.find_via(id))
            throw RequestError("stale_proposal", "remove names no track or via \"" + id + "\"");
    nb->tracks.erase(std::remove_if(nb->tracks.begin(), nb->tracks.end(),
                                    [&rm](const Track& t) { return rm.count(t.id) > 0; }),
                     nb->tracks.end());
    nb->vias.erase(std::remove_if(nb->vias.begin(), nb->vias.end(), [&rm](const Via& v) { return rm.count(v.id) > 0; }),
                   nb->vias.end());
    for (const auto& t : p.add_tracks)
        nb->tracks.push_back(t);
    for (const auto& v : p.add_vias)
        nb->vias.push_back(v);
    nb->sort_items();
    try {
        nb->check_unique_ids();
    }
    catch (const RequestError& e) {
        throw RequestError("stale_proposal", e.what());
    }
    return nb;
}

tj::Value proposal_json(const Proposal& p, const Board& b)
{
    tj::Value o = tj::obj();
    tj::Value rm = tj::arr();
    for (const auto& id : p.remove)
        rm.push(id);
    o.set("remove", rm);
    tj::Value add = tj::arr();
    for (const auto& t : p.add_tracks)
        add.push(track_json(t, b));
    for (const auto& v : p.add_vias)
        add.push(via_json(v, b));
    o.set("add", add);
    return o;
}

Proposal parse_proposal(const tj::Value& v, const Board& b)
{
    Proposal p;
    if (!v.is_object())
        throw RequestError("invalid_params", "proposal: expected an object");
    try {
        if (const tj::Value* rm = v.find("remove"); rm && !rm->is_null()) {
            for (const auto& x : rm->as_array())
                p.remove.push_back(x.as_string());
        }
        if (const tj::Value* add = v.find("add"); add && !add->is_null()) {
            const auto& a = add->as_array();
            for (size_t i = 0; i < a.size(); i++) {
                std::string where = "proposal.add[" + std::to_string(i) + "]";
                const tj::Value* k = a[i].find("kind");
                if (!k || !k->is_string())
                    throw RequestError("invalid_params", where + ": missing \"kind\" (track|via)");
                if (k->as_string() == "track")
                    p.add_tracks.push_back(parse_track(a[i], b, where));
                else if (k->as_string() == "via")
                    p.add_vias.push_back(parse_via(a[i], b, where));
                else
                    throw RequestError("invalid_params", where + ": kind must be track or via");
            }
        }
    }
    catch (const RequestError& e) {
        throw RequestError(e.code == "invalid_board" ? "invalid_params" : e.code, e.what());
    }
    catch (const std::exception& e) {
        throw RequestError("invalid_params", std::string("proposal: ") + e.what());
    }
    std::sort(p.remove.begin(), p.remove.end());
    p.remove.erase(std::unique(p.remove.begin(), p.remove.end()), p.remove.end());
    return p;
}

} // namespace tr
