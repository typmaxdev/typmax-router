// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's pcbnew/length_delay_calculation: the pad/via clipping
// helpers TOPOLOGY calls when it assembles a TUNING path (length/skew meanders).
// The service never tunes, so they leave the line as it is.
#pragma once

#include <geometry/shape_line_chain.h>
#include <layer_ids.h>
#include <math/vector2d.h>

class PAD;
class PCB_VIA;

class LENGTH_DELAY_CALCULATION {
public:
    static void OptimiseTraceInPad(SHAPE_LINE_CHAIN&, const PAD*, PCB_LAYER_ID) {}
    static bool IsPointInsideViaPad(const PCB_VIA*, const VECTOR2I&, PCB_LAYER_ID) { return false; }
    static void OptimiseTraceInVia(SHAPE_LINE_CHAIN&, const PCB_VIA*, PCB_LAYER_ID) {}
};
