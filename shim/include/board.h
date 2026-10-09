// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's BOARD (pcbnew/board.h). The router core includes it
// for the layer ids (pns_sizes_settings.cpp) and BOARD_ITEM types; it never
// holds a BOARD.
#pragma once

#include <board_item.h>
#include <layer_ids.h>
