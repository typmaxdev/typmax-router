// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Request params: the checks every method shares (each throws invalid_params
// naming the param), and the parsers of the route_pair and tune params.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "board.hpp"
#include "engine.hpp"
#include "json.hpp"

namespace tr {
namespace params {

[[noreturn]] void bad_params(const std::string& m);
// the param, or nullptr when absent or null
const tj::Value* opt(const tj::Value& p, const char* k);
std::string req_str(const tj::Value& p, const char* k);
bool opt_bool(const tj::Value& p, const char* k, bool d);
// a size: an integer in 1..1e8 nm
std::optional<int64_t> opt_size(const tj::Value& p, const char* k);
Pt point_param(const tj::Value& v, const std::string& where);
int layer_param(const tj::Value& v, const Board& b, const std::string& where);
Mode mode_param(const tj::Value& p, Mode d);
Anchor anchor_param(const tj::Value& v, const char* which);
// `waypoints`: absent = none; up to 1000 {point, via?, layer?}
std::vector<Waypoint> waypoints_param(const tj::Value& p, const Board& b);

// route_pair and tune (requests.cpp)
PairRequest parse_route_pair(const tj::Value& p, const Board& b);
TuneRequest parse_tune(const tj::Value& p);

} // namespace params
} // namespace tr
