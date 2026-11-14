// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// The route and drag methods over the PNS host (world.hpp), and apply.
#include "engine.hpp"

#include <algorithm>
#include <cmath>

#include "world.hpp"

namespace tr {

double g_last_world_ms = 0;

using namespace host;

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
