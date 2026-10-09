// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's include/gal/color4d.h: the colour type and the legacy
// colour names the router's debug calls pass (PNS_DBG, compiled but never run
// without a debug decorator). Enum values as upstream.
#pragma once

enum EDA_COLOR_T {
    UNSPECIFIED_COLOR = -1, BLACK = 0, DARKDARKGRAY, DARKGRAY, LIGHTGRAY, WHITE, LIGHTYELLOW, DARKBLUE,
    DARKGREEN, DARKCYAN, DARKRED, DARKMAGENTA, DARKBROWN, BLUE, GREEN, CYAN, RED, MAGENTA, BROWN,
    LIGHTBLUE, LIGHTGREEN, LIGHTCYAN, LIGHTRED, LIGHTMAGENTA, YELLOW, PUREBLUE, PUREGREEN, PURECYAN,
    PURERED, PUREMAGENTA, PUREYELLOW, LIGHTERORANGE, DARKORANGE, ORANGE, LIGHTORANGE, PUREORANGE, NBCOLORS
};

namespace KIGFX {
class COLOR4D {
public:
    constexpr COLOR4D() = default;
    constexpr COLOR4D(double r, double g, double b, double a) : r(r), g(g), b(b), a(a) {}
    constexpr COLOR4D(EDA_COLOR_T) {}
    COLOR4D WithAlpha(double aAlpha) const { return COLOR4D(r, g, b, aAlpha); }
    double r = 0, g = 0, b = 0, a = 1;
    static const COLOR4D UNSPECIFIED;
    static const COLOR4D WHITE;
    static const COLOR4D BLACK;
    static const COLOR4D CLEAR;
};
inline const COLOR4D COLOR4D::UNSPECIFIED(0, 0, 0, 0);
inline const COLOR4D COLOR4D::WHITE(1, 1, 1, 1);
inline const COLOR4D COLOR4D::BLACK(0, 0, 0, 1);
inline const COLOR4D COLOR4D::CLEAR(1, 0, 1, 0);
} // namespace KIGFX
