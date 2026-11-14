// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// tune: KiCad's meander placers over the PNS host (world.hpp): the single-track
// tuner (MEANDER_PLACER), the pair-length tuner (DP_MEANDER_PLACER) and the
// pair-skew tuner (MEANDER_SKEW_PLACER). A span of one track is meandered toward
// a target; the answer is a proposal and the length as the engine counts it.
#include "engine.hpp"

#include <algorithm>
#include <map>
#include <stdexcept>
#include <tuple>

#include <pns_diff_pair.h>
#include <pns_meander.h>
#include <pns_meander_placer_base.h>
#include <pns_topology.h>

#include "world.hpp"

namespace tr {

using namespace host;

namespace {

struct StopGuard {
    PNS::ROUTER& r;
    ~StopGuard()
    {
        if (r.RoutingInProgress())
            r.StopRouting();
    }
};

PNS::NET_HANDLE known_net(Session& s, const std::string& name)
{
    PNS::NET_HANDLE h = name.empty() ? nullptr : s.iface.net(name);
    if (!h)
        throw RequestError("unknown_net", "net \"" + name + "\" is on no item of the loaded board");
    return h;
}

// The track to tune, checked against the request's net or pair.
const Track& tuned_track(Session& s, const Board& b, const TuneRequest& rq)
{
    const Track* t = b.find_track(rq.track);
    if (!t)
        throw RequestError("unknown_item", "no track \"" + rq.track + "\"");
    if (t->locked)
        throw RequestError("locked", "track \"" + rq.track + "\" is locked");
    if (rq.aim == Aim::Length) {
        known_net(s, rq.net);
        if (t->net != rq.net)
            throw RequestError("net_mismatch", "track \"" + rq.track + "\" is on net \"" + t->net + "\", not \""
                                                       + rq.net + "\"");
        return *t;
    }
    PNS::NET_HANDLE p = known_net(s, rq.p), n = known_net(s, rq.n);
    if (t->net != rq.p && t->net != rq.n)
        throw RequestError("pair_mismatch", "track \"" + rq.track + "\" is on net \"" + t->net
                                                    + "\", a member of neither \"" + rq.p + "\" nor \"" + rq.n + "\"");
    s.iface.resolver().set_pair(p, n);
    return *t;
}

void check_on_track(PNS::ITEM* it, int layer, const Pt& p, const char* which, const std::string& id)
{
    const SHAPE* sh = it->Shape(layer);
    if (!sh || !sh->Collide(vi(p), 0))
        throw RequestError("invalid_params", std::string(which) + " is not on track \"" + id + "\"");
}

// The corner radius percentage for `square`: the sharpest corner the engine
// draws. MEANDER_SHAPE::Fit refuses a corner under half the track width (KiCad
// issue #8629), and the radius is spacing x percent / 200, rounded down.
int square_percent(int64_t width, int64_t spacing)
{
    const int64_t p = (100 * width + spacing - 1) / spacing + 1;
    return static_cast<int>(std::min<int64_t>(p, 100));
}

PNS::MEANDER_SETTINGS meander_settings(const TuneRequest& rq, int64_t width)
{
    PNS::MEANDER_SETTINGS ms; // the engine's defaults, then the request's
    if (rq.spacing)
        ms.m_spacing = static_cast<int>(*rq.spacing);
    if (rq.amp_min) {
        ms.m_minAmplitude = static_cast<int>(*rq.amp_min);
        ms.m_maxAmplitude = static_cast<int>(*rq.amp_max);
    }
    // chamfer: KiCad's default radius (80 % of half the spacing); square: the sharpest
    ms.m_cornerStyle = PNS::MEANDER_STYLE_CHAMFER;
    ms.m_cornerRadiusPercentage = rq.corners == Corners::Square ? square_percent(width, ms.m_spacing) : 80;
    ms.m_singleSided = rq.single_sided;
    if (rq.aim == Aim::Skew) {
        ms.m_targetSkew.SetMin(static_cast<int>(rq.target - rq.tolerance));
        ms.m_targetSkew.SetOpt(static_cast<int>(rq.target));
        ms.m_targetSkew.SetMax(static_cast<int>(rq.target + rq.tolerance));
    }
    else {
        ms.m_targetLength.SetMin(rq.target - rq.tolerance);
        ms.m_targetLength.SetOpt(rq.target);
        ms.m_targetLength.SetMax(rq.target + rq.tolerance);
    }
    return ms;
}

PNS::ROUTER_MODE tune_mode(Aim a)
{
    switch (a) {
    case Aim::Length: return PNS::PNS_MODE_TUNE_SINGLE;
    case Aim::PairLength: return PNS::PNS_MODE_TUNE_DIFF_PAIR;
    case Aim::Skew: return PNS::PNS_MODE_TUNE_DIFF_PAIR_SKEW;
    }
    return PNS::PNS_MODE_TUNE_SINGLE;
}

// The pair as the pair tuners assemble it (TOPOLOGY::AssembleDiffPair, as
// DP_MEANDER_PLACER::Start and MEANDER_SKEW_PLACER::Start do): a partner
// track of the other member alongside, at one width, on the same layer.
PNS::DIFF_PAIR coupled_pair(Session& s, PNS::ITEM* it, const std::string& id)
{
    PNS::TOPOLOGY topo(s.router.GetWorld());
    PNS::DIFF_PAIR dp;
    if (!topo.AssembleDiffPair(it, dp) || !dp.PLine().SegmentCount() || !dp.NLine().SegmentCount())
        throw RequestError("pair_not_coupled", "the engine finds no partner track coupled with \"" + id
                                                       + "\" (the other member, parallel, at the same width and layer)");
    return dp;
}

// The partner's length for a skew answer: the path the skew placer measures
// it by, assembled the same way.
long long partner_length(Session& s, PNS::ITEM* it, PNS::DIFF_PAIR& dp)
{
    PNS::TOPOLOGY topo(s.router.GetWorld());
    PNS::LINE& other = it->Net() == dp.NetP() ? dp.NLine() : dp.PLine();
    PNS::ITEM_SET path = topo.AssembleTuningPath(&s.iface, other.GetLink(0));
    return s.iface.CalculateRoutedPathLength(path, nullptr, nullptr, nullptr);
}

TuneStatus status_of(PNS::MEANDER_PLACER_BASE::TUNING_STATUS st)
{
    switch (st) {
    case PNS::MEANDER_PLACER_BASE::TUNED: return TuneStatus::Tuned;
    case PNS::MEANDER_PLACER_BASE::TOO_LONG: return TuneStatus::TooLong;
    default: return TuneStatus::TooShort;
    }
}

using TrackKey = std::tuple<std::string, int, int64_t, int64_t, int64_t, int64_t, int64_t, bool, int64_t, int64_t>;

TrackKey key_of(const Track& t)
{
    // a track's geometry without its direction
    Pt a = t.start, z = t.end;
    if (std::make_pair(z.x, z.y) < std::make_pair(a.x, a.y))
        std::swap(a, z);
    return {t.net, t.layer, a.x, a.y, z.x, z.y, t.width, t.has_mid, t.mid.x, t.mid.y};
}

// The engine rewrites the whole line it tunes; a part it gave back unchanged
// is neither removed nor added.
Proposal without_unchanged(const Proposal& p, const Board& b)
{
    std::map<TrackKey, int> removed;
    for (const auto& id : p.remove)
        if (const Track* t = b.find_track(id))
            removed[key_of(*t)]++;
    std::map<TrackKey, int> same;
    Proposal out;
    for (const auto& t : p.add_tracks) {
        auto it = removed.find(key_of(t));
        if (it != removed.end() && it->second > 0) {
            it->second--;
            same[key_of(t)]++;
            continue;
        }
        out.add_tracks.push_back(t);
    }
    for (const auto& id : p.remove) {
        const Track* t = b.find_track(id);
        if (t) {
            auto it = same.find(key_of(*t));
            if (it != same.end() && it->second > 0) {
                it->second--;
                continue;
            }
        }
        out.remove.push_back(id);
    }
    out.add_vias = p.add_vias;
    return out;
}

// The placer's answer once it has started on the span.
TuneResult tune_span(Session& s, const Board& b, const TuneRequest& rq, const Track& t, long long partner)
{
    auto* placer = dynamic_cast<PNS::MEANDER_PLACER_BASE*>(s.router.Placer());
    if (!placer)
        throw std::runtime_error("the router started no meander placer");
    placer->UpdateSettings(meander_settings(rq, t.width));
    if (!s.router.Move(vi(rq.to), nullptr)) {
        if (rq.aim == Aim::Length)
            throw refusal(s.router, "tune_refused", "PNS could not tune this span");
        throw RequestError("pair_not_coupled", "the span holds no section the engine sees as coupled");
    }
    TuneResult r;
    r.status = status_of(placer->TuningStatus());
    const long long result = placer->TuningLengthResult();
    r.length = rq.aim == Aim::Skew ? partner + result : result;
    r.skew = rq.aim == Aim::Skew ? result : 0;
    if (r.status != TuneStatus::TooLong) { // too long: the engine adds no meander
        s.router.FixRoute(vi(rq.to), nullptr, false, false);
        s.router.CommitRouting();
        r.proposal = without_unchanged(make_proposal(s.iface, b), b);
    }
    return r;
}

} // namespace

TuneResult run_tune(const Board& b, const TuneRequest& rq)
{
    Session s(b, Mode::Walkaround, true);
    const Track& t = tuned_track(s, b, rq);
    PNS::ITEM* it = s.item_of(t.id);
    if (!it)
        throw std::runtime_error("a track is missing from the router world");
    check_on_track(it, t.layer, rq.from, "from", t.id);
    check_on_track(it, t.layer, rq.to, "to", t.id);
    long long partner = 0;
    if (rq.aim != Aim::Length) {
        PNS::DIFF_PAIR dp = coupled_pair(s, it, t.id);
        if (rq.aim == Aim::Skew)
            partner = partner_length(s, it, dp);
    }
    s.router.SetMode(tune_mode(rq.aim));
    StopGuard guard{s.router};
    if (!s.router.StartRouting(vi(rq.from), it, t.layer))
        throw refusal(s.router, "tune_refused", "PNS refused to tune this track here");
    return tune_span(s, b, rq, t, partner);
}

} // namespace tr
