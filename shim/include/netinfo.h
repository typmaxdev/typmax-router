// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's NETINFO_ITEM (pcbnew/netinfo.h): a net is its code and
// its name. PNS keys nets by an opaque NET_HANDLE; the service's handles are
// pointers to these.
#pragma once

#include <wx/string.h>

class NETCLASS;

class NETINFO_ITEM {
public:
    NETINFO_ITEM(int aCode, const wxString& aName) : m_code(aCode), m_name(aName) {}
    int GetNetCode() const { return m_code; }
    const wxString& GetNetname() const { return m_name; }

private:
    int m_code;
    wxString m_name;
};
