// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's ZONE (pcbnew/zone.h): ROUTER::StartRouting reads a
// keepout's name to explain a refusal.
#pragma once

#include <board_connected_item.h>
#include <wx/string.h>

class ZONE : public BOARD_CONNECTED_ITEM {
public:
    ZONE(NETINFO_ITEM* aNet, bool aKeepout, const wxString& aName) :
            BOARD_CONNECTED_ITEM(PCB_ZONE_T, aNet), m_keepout(aKeepout), m_name(aName)
    {
    }
    bool HasKeepoutParametersSet() const { return m_keepout; }
    const wxString& GetZoneName() const { return m_name; }

private:
    bool m_keepout;
    wxString m_name;
};
