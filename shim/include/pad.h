// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's PAD (pcbnew/pad.h): ROUTER::StartRouting reads a
// non-routable pad's attribute to explain a refusal.
#pragma once

#include <board_connected_item.h>
#include <padstack.h>

class PAD : public BOARD_CONNECTED_ITEM {
public:
    PAD(NETINFO_ITEM* aNet, PAD_ATTRIB aAttrib) : BOARD_CONNECTED_ITEM(PCB_PAD_T, aNet), m_attrib(aAttrib) {}
    PAD_ATTRIB GetAttribute() const { return m_attrib; }

private:
    PAD_ATTRIB m_attrib;
};
