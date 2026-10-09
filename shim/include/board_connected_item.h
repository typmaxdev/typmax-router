// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's BOARD_CONNECTED_ITEM (pcbnew/board_connected_item.h):
// a BOARD_ITEM with a net. NODE::FindItemByParent reads the net through it.
#pragma once

#include <board_item.h>
#include <netinfo.h>

class NETCLASS;

class BOARD_CONNECTED_ITEM : public BOARD_ITEM {
public:
    BOARD_CONNECTED_ITEM(KICAD_T aType, NETINFO_ITEM* aNet, PCB_LAYER_ID aLayer = UNDEFINED_LAYER) :
            BOARD_ITEM(aType, aLayer), m_net(aNet)
    {
    }
    bool IsConnected() const override { return true; }
    NETINFO_ITEM* GetNet() const { return m_net; }
    int GetNetCode() const { return m_net ? m_net->GetNetCode() : -1; }
    // Length tuning only (the meander placers); the service does not tune.
    NETCLASS* GetEffectiveNetClass() const { return nullptr; }

protected:
    NETINFO_ITEM* m_net;
};
