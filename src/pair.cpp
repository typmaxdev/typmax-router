// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// route_pair: KiCad's differential-pair placer (DIFF_PAIR_PLACER) over the
// PNS host (world.hpp). The request names the pair's two nets; the resolver
// couples exactly those two (RESOLVER::set_pair), and the commit comes back as
// one proposal holding both members' copper.
#include "engine.hpp"

#include <algorithm>
#include <stdexcept>

#include <pns_diff_pair.h>
#include <pns_diff_pair_placer.h>

#include "world.hpp"

namespace tr {

using namespace host;

namespace {

struct Member {
    const std::string& name;
    PNS::NET_HANDLE net;
    const NetClass& nc;
};

using End = std::pair<VECTOR2I, PNS::ITEM*>;

struct Ends {
    End p, n;
};

// Ends the routing session on every path out, a refusal included.
struct StopGuard {
    PNS::ROUTER& r;
    ~StopGuard()
    {
        if (r.RoutingInProgress())
            r.StopRouting();
    }
};

Member member(Session& s, const Board& b, const std::string& name)
{
    PNS::NET_HANDLE h = name.empty() ? nullptr : s.iface.net(name);
    if (!h)
        throw RequestError("unknown_net", "net \"" + name + "\" is on no item of the loaded board");
    return Member{name, h, b.rules.class_for(name)};
}

// The least edge-to-edge gap of the twin vias the engine admits: copper
// clearance, and hole_to_hole and hole_clearance over the annular ring, as
// SIZES_SETTINGS::EffectiveDiffPairViaGap counts it.
int64_t least_via_gap(const Board& b, int64_t clearance, const NetClass& nc)
{
    const int64_t ring = (nc.via_diameter - nc.via_drill) / 2;
    return std::max({clearance, b.rules.hole_to_hole - 2 * ring, b.rules.hole_clearance - ring});
}

// The pair's own rules, checked before the engine runs; returns the via gap.
int64_t check_pair_rules(const Board& b, const PairRequest& rq, const Member& p, const Member& n)
{
    if (p.nc.track_width != n.nc.track_width || p.nc.via_diameter != n.nc.via_diameter
        || p.nc.via_drill != n.nc.via_drill)
        throw RequestError("pair_class_mismatch",
                           "the classes of \"" + p.name + "\" (" + p.nc.name + ") and \"" + n.name + "\" ("
                                   + n.nc.name + ") differ in track_width, via_diameter or via_drill; the "
                                   "engine draws both members of a pair at one size");
    const int64_t clearance = std::max(p.nc.clearance, n.nc.clearance);
    if (rq.gap < clearance)
        throw RequestError("gap_too_small", "gap " + std::to_string(rq.gap) + " nm is under the pair's clearance ("
                                                    + std::to_string(clearance) + " nm, the larger class's)");
    const int64_t least = least_via_gap(b, clearance, p.nc);
    if (rq.via_gap && *rq.via_gap < least)
        throw RequestError("gap_too_small", "via_gap " + std::to_string(*rq.via_gap)
                                                    + " nm is under the least the twin vias admit ("
                                                    + std::to_string(least) + " nm: clearance, hole_to_hole and "
                                                    "hole_clearance over the via's annular ring)");
    return rq.via_gap.value_or(least);
}

void pair_sizes(Session& s, const Board& b, const PairRequest& rq, const Member& p, const Member& n,
                int64_t via_gap)
{
    const NetClass& nc = p.nc; // equal sizes: check_pair_rules
    PNS::SIZES_SETTINGS sz;
    sz.SetTrackWidth(static_cast<int>(nc.track_width));
    sz.SetTrackWidthIsExplicit(true);
    sz.SetDiffPairWidth(static_cast<int>(nc.track_width));
    sz.SetDiffPairGap(static_cast<int>(rq.gap));
    sz.SetDiffPairViaGapSameAsTraceGap(false);
    sz.SetDiffPairViaGap(static_cast<int>(via_gap));
    sz.SetDiffPairHoleToHole(static_cast<int>(b.rules.hole_to_hole));
    sz.SetDiffPairCopperToHole(static_cast<int>(b.rules.hole_clearance));
    sz.SetViaDiameter(static_cast<int>(nc.via_diameter));
    sz.SetViaDrill(static_cast<int>(nc.via_drill));
    sz.SetViaType(VIATYPE::THROUGH);
    sz.SetMinClearance(static_cast<int>(std::max(p.nc.clearance, n.nc.clearance)));
    sz.SetHoleToHole(static_cast<int>(b.rules.hole_to_hole));
    sz.ClearLayerPairs();
    sz.AddLayerPair(0, static_cast<int>(b.layers.size()) - 1);
    s.router.UpdateSizes(sz);
}

// One member's end: a pad of the member's net on the layer, or a free point
// (the member's copper there, else a point of the member's net).
End resolve_end(Session& s, const Board& b, const Anchor& a, const Member& m, int layer, const std::string& which)
{
    if (!a.is_pad) {
        PNS::ITEM* it = s.item_at(a.point, m.net, layer);
        return {vi(a.point), it ? it : s.virtual_start(vi(a.point), m.net, layer)};
    }
    const Pad* pad = b.find_pad(a.pad_id);
    if (!pad)
        throw RequestError("unknown_item", which + ": no pad \"" + a.pad_id + "\"");
    if (pad->net != m.name)
        throw RequestError("pair_mismatch", which + ": pad \"" + a.pad_id + "\" is on net \"" + pad->net
                                                    + "\", not the member's \"" + m.name + "\"");
    PNS::ITEM* it = s.item_of(pad->id);
    if (!it)
        throw std::runtime_error("a pad is missing from the router world");
    if (!it->Layers().Overlaps(layer))
        throw RequestError("layer_mismatch", which + ": pad \"" + a.pad_id + "\" is not on layer " + b.layers[layer]);
    return {vi(pad->pos), it};
}

// DP_PRIMITIVE_PAIR holds clones: the same item is the same board item (or
// the same point item) with the same anchor.
bool same_item(const PNS::ITEM* a, const PNS::ITEM* b)
{
    return a && b && a->Kind() == b->Kind() && a->Parent() == b->Parent() && a->Anchor(0) == b->Anchor(0);
}

// The engine couples a P item with the nearest N item of the same kind
// (DIFF_PAIR_PLACER::FindDpPrimitivePair); the request's N end must be it.
void check_coupled(Session& s, const Ends& e, const std::string& which)
{
    PNS::DP_PRIMITIVE_PAIR found;
    wxString why;
    if (!PNS::DIFF_PAIR_PLACER::FindDpPrimitivePair(s.router.GetWorld(), e.p.first, e.p.second, found, &why))
        throw RequestError("pair_not_coupled", which + ": the engine finds no coupled pair of ends here",
                           std::string(why));
    if (!same_item(found.PrimP(), e.p.second) || !same_item(found.PrimN(), e.n.second))
        throw RequestError("pair_not_coupled", which + ".n is not the end the engine couples with " + which
                                                       + ".p: it takes the nearest item of the n net of the "
                                                       "same kind (pad with pad, via with via, track with track)");
}

Ends resolve_ends(Session& s, const Board& b, const PairEnd& e, const Member& p, const Member& n, int layer,
                  const std::string& which)
{
    Ends r{resolve_end(s, b, e.p, p, layer, which + ".p"), resolve_end(s, b, e.n, n, layer, which + ".n")};
    check_coupled(s, r, which);
    return r;
}

int end_layer(const PairRequest& rq)
{
    int layer = rq.layer;
    for (const Waypoint& w : rq.waypoints)
        if (w.via)
            layer = w.layer_after;
    return layer;
}

// Both members' head lines hold copper (a fix needs a segment of each).
bool has_heads(PNS::ROUTER& r)
{
    int lines = 0;
    PNS::ITEM_SET ts = r.Placer()->Traces();
    for (PNS::ITEM* it : ts.Items())
        if (it->Kind() == PNS::ITEM::LINE_T && static_cast<PNS::LINE*>(it)->SegmentCount() > 0)
            lines++;
    return lines == 2;
}

// One waypoint: the pair's centreline is fixed there (and twin vias placed).
void pair_waypoint(Session& s, const Board& b, const PairRequest& rq, size_t i, int& layer)
{
    const Waypoint& w = rq.waypoints[i];
    const VECTOR2I wp = vi(w.point);
    const std::string at = "waypoint " + std::to_string(i);
    if (w.via && !rq.via_allowed)
        throw RequestError("via_not_allowed", at + " asks for a via but via_allowed is false");
    if (i > 0 && rq.waypoints[i - 1].point == w.point)
        throw RequestError("invalid_params", at + " is where the pair already is; move first");
    bool ok = s.router.Move(wp, nullptr);
    if (w.via) {
        s.router.ToggleViaPlacement();
        ok = s.router.Move(wp, nullptr);
    }
    if (!ok || !has_heads(s.router))
        throw refusal(s.router, "unroutable", "PNS could not bring the pair to " + at);
    s.router.FixRoute(wp, nullptr, false, false); // chained: returns false by design
    if (w.via) {
        layer = w.layer_after;
        if (!s.router.SwitchLayer(layer))
            throw RequestError("unroutable", "could not switch the pair to layer " + b.layers[layer] + " after the "
                                                     "vias at " + at);
    }
}

void pair_finish(Session& s, const Ends& e, Mode m)
{
    const char* mode = m == Mode::Shove ? "shove" : "walkaround";
    if (!s.router.Move(e.p.first, e.p.second))
        throw refusal(s.router, "unroutable", std::string("PNS could not reach the end pair in mode ") + mode);
    // not forced: true only when the pair snapped onto its end primitives
    if (!s.router.FixRoute(e.p.first, e.p.second, false, false))
        throw refusal(s.router, "unroutable", std::string("PNS could not land the pair on its ends in mode ") + mode);
}

} // namespace

Proposal run_route_pair(const Board& b, const PairRequest& rq)
{
    Session s(b, rq.mode, true);
    const Member p = member(s, b, rq.p), n = member(s, b, rq.n);
    const int64_t via_gap = check_pair_rules(b, rq, p, n);
    s.iface.resolver().set_pair(p.net, n.net);
    s.router.SetMode(PNS::PNS_MODE_ROUTE_DIFF_PAIR);
    pair_sizes(s, b, rq, p, n, via_gap);

    // every end is resolved before routing starts: a free point adds a point
    // item to the root world, which must not change under the placer's branches
    const Ends start = resolve_ends(s, b, rq.start, p, n, rq.layer, "start");
    std::optional<Ends> end;
    if (rq.end)
        end = resolve_ends(s, b, *rq.end, p, n, end_layer(rq), "end");

    StopGuard guard{s.router};
    if (!s.router.StartRouting(start.p.first, start.p.second, rq.layer))
        throw refusal(s.router, "start_not_routable", "PNS refused to start the pair here");
    int layer = rq.layer;
    for (size_t i = 0; i < rq.waypoints.size(); i++)
        pair_waypoint(s, b, rq, i, layer);
    if (end)
        pair_finish(s, *end, rq.mode);
    s.router.CommitRouting();
    Proposal pr = make_proposal(s.iface, b);
    check_same_net_holes(b, pr);
    return pr;
}

} // namespace tr
