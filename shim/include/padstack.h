// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's pcbnew/padstack.h: the enums the router core names
// (PNS::VIA keeps a hole's post-machining and its unconnected-layer mode).
// Values as upstream.
#pragma once

enum class PAD_ATTRIB { PTH, SMD, CONN, NPTH };

enum class PAD_DRILL_POST_MACHINING_MODE { UNKNOWN, NOT_POST_MACHINED, COUNTERBORE, COUNTERSINK };

enum class UNCONNECTED_LAYER_MODE { KEEP_ALL, START_END_ONLY, REMOVE_ALL, REMOVE_EXCEPT_START_AND_END };
