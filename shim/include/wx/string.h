// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// wxWidgets stand-in: wxString as a std::string with the handful of members
// the router core and kimath call. Format() is printf-style over UTF-8 bytes.
#pragma once

// Real wxWidgets headers pull these in; upstream files rely on it.
#include <map>
#include <set>
#include <vector>

#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <type_traits>
#include <utility>

#include <wx/debug.h>

// wxString::c_str()'s result: converts to const char* and has AsChar().
struct wxCStrData {
    const char* p;
    operator const char*() const { return p; }
    const char* AsChar() const { return p; }
};

class wxString : public std::string {
public:
    wxString() = default;
    wxString(const char* s) : std::string(s ? s : "") {}
    wxString(const std::string& s) : std::string(s) {}
    wxString(std::string&& s) : std::string(std::move(s)) {}
    wxString(size_t n, char c) : std::string(n, c) {}

    bool IsEmpty() const { return empty(); }
    bool empty() const { return std::string::empty(); }
    size_t Len() const { return size(); }
    size_t Length() const { return size(); }
    wxCStrData c_str() const { return wxCStrData{std::string::c_str()}; }
    const char* mb_str() const { return std::string::c_str(); }
    const char* fn_str() const { return std::string::c_str(); }
    std::string ToStdString() const { return *this; }
    std::string ToUTF8() const { return *this; }
    wxString& RemoveLast(size_t n = 1) { erase(size() - std::min(n, size())); return *this; }
    wxString& Append(const wxString& s) { append(s); return *this; }
    void Printf(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        *this = vformat(fmt, ap);
        va_end(ap);
    }
    static wxString FromAscii(const char* s) { return wxString(s); }
    static wxString FromUTF8(const char* s) { return wxString(s); }

    template <typename... A>
    static wxString Format(const wxString& fmt, A&&... a)
    {
        return format_c(fmt.std::string::c_str(), arg(std::forward<A>(a))...);
    }

private:
    template <typename T>
    static auto arg(T&& v)
    {
        using D = std::decay_t<T>;
        if constexpr (std::is_base_of_v<std::string, D>)
            return v.std::string::c_str();
        else if constexpr (std::is_same_v<D, wxCStrData>)
            return v.p;
        else
            return v;
    }
    static wxString format_c(const char* fmt, ...)
    {
        va_list ap;
        va_start(ap, fmt);
        wxString r = vformat(fmt, ap);
        va_end(ap);
        return r;
    }
    static wxString vformat(const char* fmt, va_list ap)
    {
        va_list aq;
        va_copy(aq, ap);
        int n = std::vsnprintf(nullptr, 0, fmt, aq);
        va_end(aq);
        if (n <= 0)
            return wxString();
        std::string s(static_cast<size_t>(n) + 1, '\0');
        std::vsnprintf(&s[0], s.size(), fmt, ap);
        s.resize(static_cast<size_t>(n));
        return wxString(std::move(s));
    }
};

typedef char wxUniChar;

#define wxT(s) s
#define wxS(s) s
#ifndef wxASCII_STR
#define wxASCII_STR(s) wxString(s)
#endif

inline const wxString wxEmptyString;

inline int wxAtoi(const wxString& s) { return std::atoi(s.c_str()); }
