// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's ADVANCED_CFG (include/advanced_config.h,
// common/advanced_config.cpp): only the members the router core and kimath
// read, with KiCad's defaults, except the two WALL-CLOCK limits, which are off.
// A new KiCad release that reads another member fails to compile here, which
// is the signal to look at its default (tools/update_kicad.sh stops there).
#pragma once

#include <climits>

struct ADVANCED_CFG {
    // KiCad defaults (common/advanced_config.cpp)
    double m_MaxTangentAngleDeviation = 1.0;
    double m_MaxTrackLengthToKeep = 0.0005;
    bool m_EnableRouterDump = false;
    int m_TriangulateSimplificationLevel = 50;
    int m_TriangulateMinimumArea = 1000;
    bool m_EnableCacheFriendlyFracture = true;

    // KiCad: 100 ms (walkaround cluster) and 500 ms (topology branch walk).
    // Both are wall-clock caps, so an answer would depend on the machine's
    // load; the service needs the same answer every time, and bounds a request
    // with its own time limit instead (SOURCE.md, "Determinism").
    int m_PNSProcessClusterTimeout = INT_MAX;
    int m_FollowBranchTimeout = INT_MAX;

    static const ADVANCED_CFG& GetCfg()
    {
        static const ADVANCED_CFG cfg;
        return cfg;
    }
};
