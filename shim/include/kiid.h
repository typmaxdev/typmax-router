// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// Stands in for KiCad's KIID (include/kiid.h). Only the router's debug logger
// (off: ADVANCED_CFG::m_EnableRouterDump) reads it; a uuid is its string.
#pragma once

#include <string>

#include <wx/string.h>

class KIID {
public:
    KIID() = default;
    explicit KIID(const std::string& aString) : m_s(aString) {}
    wxString AsString() const { return m_s; }
    bool operator<(const KIID& o) const { return m_s < o.m_s; }
    bool operator==(const KIID& o) const { return m_s == o.m_s; }

private:
    std::string m_s;
};
