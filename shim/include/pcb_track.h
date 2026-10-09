// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's PCB_TRACK / PCB_ARC / PCB_VIA (pcbnew/pcb_track.h).
// NODE::FixupVirtualVias static_casts EVERY segment's parent to PCB_TRACK* and
// reads its solder mask; these members read no state, so that cast is harmless
// for a parent of any type (the board outline's included).
#pragma once

#include <optional>

#include <board_connected_item.h>
#include <pcb_track_types.h>

class PCB_TRACK : public BOARD_CONNECTED_ITEM {
public:
    using BOARD_CONNECTED_ITEM::BOARD_CONNECTED_ITEM;
    bool HasSolderMask() const { return false; }
    std::optional<int> GetLocalSolderMaskMargin() const { return std::nullopt; }
};

class PCB_ARC : public PCB_TRACK {
public:
    using PCB_TRACK::PCB_TRACK;
};

class PCB_VIA : public PCB_TRACK {
public:
    using PCB_TRACK::PCB_TRACK;
};
