// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// The service's own board model: the neutral board JSON (PROTOCOL.md), parsed
// and validated. Nothing here knows about PNS; engine.cpp translates.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "json.hpp"

namespace tr {

// A request the client got wrong: becomes an error object, never a crash.
// `detail` carries the engine's own text (PNS's failure reason), kept apart
// from our message so neither is mistaken for the stable `code`.
struct RequestError : std::runtime_error {
    RequestError(std::string code, const std::string& msg, std::string detail = "")
        : std::runtime_error(msg), code(std::move(code)), detail(std::move(detail))
    {
    }
    std::string code;
    std::string detail;
};

struct Pt {
    int64_t x = 0, y = 0;
    bool operator==(const Pt& o) const { return x == o.x && y == o.y; }
    bool operator!=(const Pt& o) const { return !(*this == o); }
};

enum class PadShape { Circle, Rect, RoundRect, Oval, Polygon };

struct Pad {
    std::string id, footprint, pad, net;
    std::vector<int> layers; // indices into Board::layers
    PadShape shape = PadShape::Circle;
    Pt pos;
    double rotation = 0; // degrees, KiCad convention (see PROTOCOL.md)
    int64_t w = 0, h = 0;
    int64_t drill = 0;      // 0 = SMD / no hole
    std::vector<Pt> polygon; // Polygon shape: vertices relative to pos, unrotated
};

struct Track {
    std::string id, net;
    int layer = 0;
    Pt start, end;
    int64_t width = 0;
    bool has_mid = false; // arc through `mid`
    Pt mid;
    bool locked = false;
};

struct Via {
    std::string id, net;
    Pt pos;
    int64_t diameter = 0, drill = 0;
    int layer_from = 0, layer_to = 0; // indices into Board::layers, from <= to
    bool locked = false;
};

struct Keepout {
    std::string id;
    std::vector<int> layers;
    std::vector<Pt> polygon;
};

// Fixed copper that is not a pad, track or via (copper text, graphics, and
// later zone fills): an obstacle with a net (or none), never moved or removed.
struct Copper {
    std::string id, net;
    int layer = 0;
    std::vector<Pt> polygon; // absolute; PNS treats it as its convex hull
};

struct NetClass {
    std::string name;
    int64_t clearance = 0, track_width = 0, via_diameter = 0, via_drill = 0;
};

struct Rules {
    std::map<std::string, NetClass> classes;
    std::string default_class;
    std::map<std::string, std::string> net_class; // net name -> class name
    int64_t edge_clearance = 0;
    int64_t hole_clearance = 0;
    int64_t hole_to_hole = 0;

    const NetClass& class_for(const std::string& net) const;
    int64_t max_clearance() const;
};

struct Board {
    std::vector<std::string> layers; // copper, top to bottom
    std::vector<std::vector<Pt>> outline;
    std::vector<Pad> pads;       // sorted by id
    std::vector<Track> tracks;   // sorted by id
    std::vector<Via> vias;       // sorted by id
    std::vector<Keepout> keepouts;
    std::vector<Copper> copper;  // sorted by id
    Rules rules;

    int layer_index(const std::string& name) const; // -1 if unknown
    const Pad* find_pad(const std::string& id) const;
    const Track* find_track(const std::string& id) const;
    const Via* find_via(const std::string& id) const;
    std::set<std::string> net_names() const;
    void sort_items();
    void check_unique_ids() const;
};

// Parsing: throw RequestError("invalid_board", ...) naming the offending path.
std::shared_ptr<Board> parse_board(const tj::Value& v);
Track parse_track(const tj::Value& v, const Board& b, const std::string& where);
Via parse_via(const tj::Value& v, const Board& b, const std::string& where);
Pt parse_point(const tj::Value& v, const std::string& where);

// Serialization (proposal items).
tj::Value track_json(const Track& t, const Board& b);
tj::Value via_json(const Via& v, const Board& b);

// Range check: every coordinate fits the router's 32-bit int with headroom.
int64_t check_coord(int64_t v, const std::string& where);

} // namespace tr
