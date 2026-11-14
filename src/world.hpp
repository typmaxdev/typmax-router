// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// The PNS host every method shares: a PNS world built from a Board snapshot
// (IFACE), the board's rules in KiCad's resolver shape (RESOLVER), one router
// session over a fresh world (Session), and the commit captured as a sorted,
// deterministic proposal. engine.cpp, pair.cpp and tune.cpp drive it.
#pragma once

#include <chrono>
#include <climits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "engine.hpp"

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
namespace host {

// Every PNS item the world holds has a BOARD_ITEM parent (shim/include): PNS
// reads its type and layer (ROUTER::StartRouting, LINE_PLACER's start
// posture, the edge test in ITEM::collideSimple), and a commit hands back the
// items it touched by parent. A parent's TAG says which board item it is.
enum class Kind { PAD, TRACK, VIA, OUTLINE, KEEPOUT, COPPER, VIRTUAL };

struct Tag {
    Kind kind;
    std::string id;
};

// PNS layer -> KiCad's PCB_LAYER_ID numbering (see world.cpp)
PCB_LAYER_ID board_layer(int pns, int n);
void check_layer(int l, int n);

inline VECTOR2I vi(const Pt& p) { return VECTOR2I(static_cast<int>(p.x), static_cast<int>(p.y)); }
inline Pt pt(const VECTOR2I& v) { return Pt{v.x, v.y}; }

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
    // The meander placers' length: track length only (world.cpp), no via
    // height, no pad-to-die, nothing clipped inside a pad (PROTOCOL.md, tune).
    long long CalculateRoutedPathLength(const PNS::ITEM_SET& aLine, const PNS::SOLID*, const PNS::SOLID*,
                                        const NETCLASS*) override;
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
    RESOLVER& resolver() { return *m_resolver; }
    const Board& board() const { return m_board; }
    int nlayers() const { return static_cast<int>(m_board.layers.size()); }

    std::set<std::string> removed;
    // What the session's commits added and still hold, keyed by the engine's item: a
    // pair route commits every fixed leg into the world at once, and a later leg or the
    // final commit may merge, re-optimise or shove that copper again (review PR #11 r1
    // M-1), so an item made during the request and removed again leaves the capture.
    std::vector<Track> added_tracks() const;
    std::vector<Via> added_vias() const;

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
    std::map<const PNS::ITEM*, Track> m_added_tracks;
    std::map<const PNS::ITEM*, Via> m_added_vias;
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
    // The one differential pair a request names (route_pair, tune): KiCad
    // finds pairs by net-name suffix; the service takes the client's word.
    void set_pair(PNS::NET_HANDLE p, PNS::NET_HANDLE n)
    {
        m_pair_p = p;
        m_pair_n = n;
    }
    PNS::NET_HANDLE DpCoupledNet(PNS::NET_HANDLE h) override
    {
        if (!h || !m_pair_p)
            return nullptr;
        return h == m_pair_p ? m_pair_n : h == m_pair_n ? m_pair_p : nullptr;
    }
    int DpNetPolarity(PNS::NET_HANDLE h) override { return h && h == m_pair_p ? 1 : h && h == m_pair_n ? -1 : 0; }
    bool DpNetPair(const PNS::ITEM* a, PNS::NET_HANDLE& p, PNS::NET_HANDLE& n) override
    {
        if (!a || !m_pair_p || (a->Net() != m_pair_p && a->Net() != m_pair_n))
            return false;
        p = m_pair_p;
        n = m_pair_n;
        return true;
    }
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
    PNS::NET_HANDLE m_pair_p = nullptr, m_pair_n = nullptr;
};

// Deterministic proposal from a session's captured commit (world.cpp).
Proposal make_proposal(const IFACE& io, const Board& b);
PNS::PNS_MODE pns_mode(Mode m);

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

RequestError refusal(PNS::ROUTER& r, const std::string& code, const std::string& msg);
std::optional<VECTOR2I> trace_end(PNS::ROUTER& r);
int trace_points(PNS::ROUTER& r);
void check_same_net_holes(const Board& b, const Proposal& p);

} // namespace host
} // namespace tr
