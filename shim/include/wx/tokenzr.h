// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// wxWidgets stand-in: wxStringTokenizer over whitespace (the router's debug
// logger parses its own dump format with it; the dump is off).
#pragma once

#include <wx/string.h>

class wxStringTokenizer {
public:
    explicit wxStringTokenizer(const wxString& s, const wxString& delims = " \t\r\n") : m_s(s), m_delims(delims) {}
    bool HasMoreTokens() const { return m_s.find_first_not_of(m_delims, m_pos) != std::string::npos; }
    wxString GetNextToken()
    {
        size_t a = m_s.find_first_not_of(m_delims, m_pos);
        if (a == std::string::npos) {
            m_pos = m_s.size();
            return wxString();
        }
        size_t b = m_s.find_first_of(m_delims, a);
        if (b == std::string::npos)
            b = m_s.size();
        m_pos = b;
        return wxString(m_s.substr(a, b - a));
    }

private:
    std::string m_s, m_delims;
    size_t m_pos = 0;
};
