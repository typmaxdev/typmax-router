// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// route_pair: KiCad's differential-pair placer (DIFF_PAIR_PLACER) over the
// PNS host (world.hpp). The request names the pair's two nets; the resolver
// couples exactly those two (RESOLVER::set_pair), and the commit comes back as
// one proposal holding both members' copper.
#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

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
    // where the engine lands each member on these ends (FindDpPrimitivePair's anchors): a pad's centre, a free
    // point, a track's free end
    VECTOR2I ap, an;
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
void check_coupled(Session& s, Ends& e, const std::string& which)
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
    e.ap = found.AnchorP();
    e.an = found.AnchorN();
}

Ends resolve_ends(Session& s, const Board& b, const PairEnd& e, const Member& p, const Member& n, int layer,
                  const std::string& which)
{
    Ends r{resolve_end(s, b, e.p, p, layer, which + ".p"), resolve_end(s, b, e.n, n, layer, which + ".n"), {}, {}};
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

// The midpoint of the two members' head ends, where the pair's centreline ends now;
// none while either head holds no segment (a fix needs a segment of each).
std::optional<VECTOR2I> head_mid(PNS::ROUTER& r)
{
    std::vector<VECTOR2I> ends;
    PNS::ITEM_SET ts = r.Placer()->Traces();
    for (PNS::ITEM* it : ts.Items())
        if (it->Kind() == PNS::ITEM::LINE_T && static_cast<PNS::LINE*>(it)->SegmentCount() > 0)
            ends.push_back(static_cast<PNS::LINE*>(it)->CPoint(-1));
    if (ends.size() != 2)
        return std::nullopt;
    return VECTOR2I((ends[0].x + ends[1].x) / 2, (ends[0].y + ends[1].y) / 2);
}

// How far the pair's centreline may end from a waypoint and still have reached it: the
// gateways snap the twin heads to the 45-degree grid around the click (half a pitch).
int64_t reach_tolerance(const Board& b, const PairRequest& rq)
{
    return (rq.gap + b.rules.class_for(rq.p).track_width) / 2;
}

// One waypoint: the pair's centreline is fixed there (and twin vias placed).
void pair_waypoint(Session& s, const Board& b, const PairRequest& rq, size_t i, int& layer)
{
    const Waypoint& w = rq.waypoints[i];
    const VECTOR2I wp = vi(w.point);
    const std::string at = "waypoint " + std::to_string(i);
    if (w.via && !rq.via_allowed)
        throw RequestError("via_not_allowed", at + " asks for a via but via_allowed is false");
    bool ok = s.router.Move(wp, nullptr);
    if (w.via) {
        s.router.ToggleViaPlacement();
        ok = s.router.Move(wp, nullptr);
    }
    const std::optional<VECTOR2I> mid = ok ? head_mid(s.router) : std::nullopt;
    // as route refuses a waypoint its trace does not end on: the placer keeps its last
    // good trace when the gateways do not fit, short of the click (review PR #11 r1 M-2)
    if (!mid || (*mid - wp).EuclideanNorm() > static_cast<double>(reach_tolerance(b, rq)))
        throw refusal(s.router, "unroutable", "PNS could not bring the pair to " + at);
    s.router.FixRoute(wp, nullptr, false, false); // chained: returns false by design
    if (w.via) {
        layer = w.layer_after;
        if (!s.router.SwitchLayer(layer))
            throw RequestError("unroutable", "could not switch the pair to layer " + b.layers[layer] + " after the "
                                                     "vias at " + at);
    }
}

// The members' head ends now: P's line's last point and N's (none while either holds no segment).
std::optional<std::pair<VECTOR2I, VECTOR2I>> head_ends(PNS::ROUTER& r, PNS::NET_HANDLE p)
{
    std::optional<VECTOR2I> hp, hn;
    PNS::ITEM_SET ts = r.Placer()->Traces();
    for (PNS::ITEM* it : ts.Items()) {
        if (it->Kind() != PNS::ITEM::LINE_T || static_cast<PNS::LINE*>(it)->SegmentCount() == 0)
            continue;
        (it->Net() == p ? hp : hn) = static_cast<PNS::LINE*>(it)->CPoint(-1);
    }
    if (!hp || !hn)
        return std::nullopt;
    return std::make_pair(*hp, *hn);
}

// Whether the pair's heads end on the end pair's anchors: both members reached (PR #11 r2 R2-M1).
bool reached(PNS::ROUTER& r, const Ends& e, PNS::NET_HANDLE p)
{
    const auto h = head_ends(r, p);
    return h && h->first == e.ap && h->second == e.an;
}

// The last leg onto the end pair. After a waypoint fixed on a diagonal the placer's gateways do not fit, and it
// answers true for a leg it did not lay: it keeps its old head and re-adds it (PR #11 r2 R2-M1). So the heads must end
// on the end pair's anchors, as `route` checks its trace's end; short of them the answer is unroutable, never a
// proposal without the leg. (Tried and measured not to help: the cursor fed toward the end in steps, and a placer
// started afresh from the free ends of the copper fixed so far.)
void pair_finish(Session& s, const Ends& e, PNS::NET_HANDLE p, Mode m)
{
    const std::string mode = m == Mode::Shove ? "shove" : "walkaround";
    if (!s.router.Move(e.p.first, e.p.second) || !reached(s.router, e, p))
        throw refusal(s.router, "unroutable", "PNS could not bring the pair onto its end pair in mode " + mode);
    // not forced: true only when the pair snapped onto its end primitives
    if (!s.router.FixRoute(e.p.first, e.p.second, false, false))
        throw refusal(s.router, "unroutable", "PNS could not land the pair on its ends in mode " + mode);
}

// Whether two straight tracks of one net and layer lie on one line and share a length: copper laid twice.
bool laid_twice(const Track& a, const Track& b)
{
    if (a.net != b.net || a.layer != b.layer || a.has_mid || b.has_mid)
        return false;
    const double dx = static_cast<double>(a.end.x - a.start.x), dy = static_cast<double>(a.end.y - a.start.y);
    const double len = std::hypot(dx, dy);
    if (len == 0)
        return false;
    const auto off = [&](const Pt& q) { return std::abs((q.x - a.start.x) * dy - (q.y - a.start.y) * dx) / len; };
    const auto along = [&](const Pt& q) { return ((q.x - a.start.x) * dx + (q.y - a.start.y) * dy) / len; };
    if (off(b.start) > 2 || off(b.end) > 2)
        return false;
    const double lo = std::min(along(b.start), along(b.end)), hi = std::max(along(b.start), along(b.end));
    return std::min(len, hi) - std::max(0.0, lo) > 2;
}

// Whether `net`'s added tracks join `from` to `to` (a walk over shared ends; a via joins its layers at its centre).
bool joined(const Proposal& pr, const std::string& net, const VECTOR2I& from, const VECTOR2I& to)
{
    std::map<std::pair<int64_t, int64_t>, std::vector<std::pair<int64_t, int64_t>>> adj;
    for (const Track& t : pr.add_tracks) {
        if (t.net != net)
            continue;
        adj[{t.start.x, t.start.y}].push_back({t.end.x, t.end.y});
        adj[{t.end.x, t.end.y}].push_back({t.start.x, t.start.y});
    }
    std::set<std::pair<int64_t, int64_t>> seen{{from.x, from.y}};
    std::vector<std::pair<int64_t, int64_t>> todo{{from.x, from.y}};
    while (!todo.empty()) {
        const auto at = todo.back();
        todo.pop_back();
        for (const auto& q : adj[at])
            if (seen.insert(q).second)
                todo.push_back(q);
    }
    return seen.count({to.x, to.y}) > 0;
}

// The proposal's own check behind the engine's (PR #11 r2 R2-M1): with an end, each member joins its start anchor to
// its end anchor, and no member's copper is laid twice. Else unroutable: the engine's state is not a pair route.
void check_laid(const Proposal& pr, const Ends& start, const std::optional<Ends>& end, const Member& p, const Member& n)
{
    for (size_t i = 0; i < pr.add_tracks.size(); i++)
        for (size_t j = i + 1; j < pr.add_tracks.size(); j++)
            if ((pr.add_tracks[i].net == p.name || pr.add_tracks[i].net == n.name) && laid_twice(pr.add_tracks[i], pr.add_tracks[j]))
                throw RequestError("unroutable", "PNS laid part of the pair twice; the pair is not routed");
    if (end && (!joined(pr, p.name, start.ap, end->ap) || !joined(pr, n.name, start.an, end->an)))
        throw RequestError("unroutable", "PNS did not join each member from its start to its end; the pair is not routed");
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
        pair_finish(s, *end, p.net, rq.mode);
    s.router.CommitRouting();
    Proposal pr = make_proposal(s.iface, b);
    check_laid(pr, start, end, p, n);
    check_same_net_holes(b, pr);
    return pr;
}

} // namespace tr
