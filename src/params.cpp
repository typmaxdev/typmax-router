// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// The param checks every method shares (params.hpp).
#include "params.hpp"

namespace tr {
namespace params {

[[noreturn]] void bad_params(const std::string& m) { throw RequestError("invalid_params", m); }

const tj::Value* opt(const tj::Value& p, const char* k)
{
    const tj::Value* v = p.find(k);
    return (v && !v->is_null()) ? v : nullptr;
}

std::string req_str(const tj::Value& p, const char* k)
{
    const tj::Value* v = opt(p, k);
    if (!v || !v->is_string())
        bad_params(std::string("\"") + k + "\" must be a string");
    return v->as_string();
}

bool opt_bool(const tj::Value& p, const char* k, bool d)
{
    const tj::Value* v = opt(p, k);
    if (!v)
        return d;
    if (!v->is_bool())
        bad_params(std::string("\"") + k + "\" must be a boolean");
    return v->as_bool();
}

std::optional<int64_t> opt_size(const tj::Value& p, const char* k)
{
    const tj::Value* v = opt(p, k);
    if (!v)
        return std::nullopt;
    if (!v->is_int() || v->as_int() <= 0 || v->as_int() > 100000000)
        bad_params(std::string("\"") + k + "\" must be a positive integer (nm, <= 100 mm)");
    return v->as_int();
}

Pt point_param(const tj::Value& v, const std::string& where)
{
    try {
        return parse_point(v, where);
    }
    catch (const RequestError& e) {
        bad_params(e.what());
    }
}

int layer_param(const tj::Value& v, const Board& b, const std::string& where)
{
    if (!v.is_string())
        bad_params(where + " must be a layer name");
    int l = b.layer_index(v.as_string());
    if (l < 0)
        bad_params(where + ": unknown copper layer \"" + v.as_string() + "\"");
    return l;
}

Mode mode_param(const tj::Value& p, Mode d)
{
    const tj::Value* v = opt(p, "mode");
    if (!v)
        return d;
    if (!v->is_string())
        bad_params("\"mode\" must be walkaround, shove or mark_obstacles");
    const std::string& m = v->as_string();
    if (m == "walkaround")
        return Mode::Walkaround;
    if (m == "shove")
        return Mode::Shove;
    if (m == "mark_obstacles")
        return Mode::MarkObstacles;
    bad_params("\"mode\" must be walkaround, shove or mark_obstacles; got \"" + m + "\"");
}

Anchor anchor_param(const tj::Value& v, const char* which)
{
    Anchor a;
    if (!v.is_object())
        bad_params(std::string(which) + " must be {\"pad\": id} or {\"point\": [x, y]}");
    const tj::Value* pad = opt(v, "pad");
    const tj::Value* pnt = opt(v, "point");
    if ((pad != nullptr) == (pnt != nullptr))
        bad_params(std::string(which) + " must name exactly one of \"pad\" or \"point\"");
    if (pad) {
        if (!pad->is_string())
            bad_params(std::string(which) + ".pad must be a pad id");
        a.is_pad = true;
        a.pad_id = pad->as_string();
    }
    else {
        a.point = point_param(*pnt, std::string(which) + ".point");
    }
    return a;
}

std::vector<Waypoint> waypoints_param(const tj::Value& p, const Board& b)
{
    std::vector<Waypoint> out;
    if (const tj::Value* w = opt(p, "waypoints")) {
        if (!w->is_array() || w->as_array().size() > 1000)
            bad_params("\"waypoints\" must be an array of at most 1000 waypoints");
        for (size_t i = 0; i < w->as_array().size(); i++) {
            const tj::Value& x = w->as_array()[i];
            std::string where = "waypoints[" + std::to_string(i) + "]";
            if (!x.is_object() || !opt(x, "point"))
                bad_params(where + " must be {\"point\": [x, y], \"via\"?: bool, \"layer\"?: name}");
            Waypoint wp;
            wp.point = point_param(*opt(x, "point"), where + ".point");
            wp.via = opt_bool(x, "via", false);
            if (wp.via && b.layers.size() < 2) // review R-6: index 0 is both F_Cu and B_Cu there
                bad_params(where + ": a via needs a board with at least two copper layers");
            if (wp.via) {
                const tj::Value* l = opt(x, "layer");
                if (!l)
                    bad_params(where + ": a via waypoint names the \"layer\" to continue on");
                wp.layer_after = layer_param(*l, b, where + ".layer");
            }
            out.push_back(wp);
        }
    }
    return out;
}

} // namespace params
} // namespace tr
