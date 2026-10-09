// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's include/units_provider.h and eda_units.h: SHOVE builds
// one on every iteration to describe the obstacle in a debug message
// (BOARD_ITEM::GetItemDescription), which nothing prints here.
#pragma once

#include <base_units.h>

enum class EDA_UNITS : int { INCH = 0, MM = 1, UNSCALED = 2, DEGREES = 3, PERCENT = 4, MILS = 5 };

class UNITS_PROVIDER {
public:
    UNITS_PROVIDER(const EDA_IU_SCALE&, EDA_UNITS) {}
};
