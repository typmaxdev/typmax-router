// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// The routing engine: a PNS world built from a Board snapshot per request, a
// ROUTER_IFACE that turns PNS's commit into a proposal, and the route/drag ops.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "board.hpp"
#include "json.hpp"

namespace tr {

enum class Mode { Walkaround, Shove, MarkObstacles };

struct Proposal {
    std::vector<std::string> remove; // ids of existing tracks/vias, sorted
    std::vector<Track> add_tracks;   // sorted (see engine.cpp: proposal_order)
    std::vector<Via> add_vias;
};

struct Anchor {
    bool is_pad = false;
    std::string pad_id;
    Pt point;
};

struct Waypoint {
    Pt point;
    bool via = false;
    int layer_after = -1; // layer to continue on after a via
};

struct RouteRequest {
    std::string net;
    int layer = 0;
    Anchor start;
    std::optional<Anchor> end;
    std::vector<Waypoint> waypoints;
    std::optional<int64_t> width;
    std::optional<int64_t> via_diameter, via_drill;
    Mode mode = Mode::Walkaround;
    bool via_allowed = false;
    bool remove_loops = true;
    // Cursor emulation: the placer is moved toward each target in steps of at
    // most this length, the way a mouse drag feeds KiCad's interactive router
    // (its walkaround commits progress into the tail as it goes). 0 = one jump.
    int64_t step = 0;
};

struct DragRequest {
    std::string item;
    std::optional<Pt> from; // grab point, default: segment midpoint / via centre
    Pt to;
    Mode mode = Mode::Shove;
};

// Wall time of the last PNS world build (SyncWorld), for `timing` responses.
extern double g_last_world_ms;

// Both throw RequestError for a request the router refuses (with a code).
Proposal run_route(const Board& b, const RouteRequest& r);
Proposal run_drag(const Board& b, const DragRequest& r);

// Apply a proposal to a board snapshot; returns the new snapshot.
std::shared_ptr<Board> apply_proposal(const Board& b, const Proposal& p);

tj::Value proposal_json(const Proposal& p, const Board& b);
Proposal parse_proposal(const tj::Value& v, const Board& b);

} // namespace tr
