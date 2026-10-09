// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// wxWidgets stand-in: logging compiled out. The service writes nothing to
// stderr from the engine; a log call's arguments are not evaluated.
#pragma once

#include <cstdarg>

#include <wx/debug.h>
#include <wx/string.h>

#define wxLogTrace(...) ((void) 0)
#define wxLogDebug(...) ((void) 0)
#define wxLogWarning(...) ((void) 0)
#define wxLogError(...) ((void) 0)
#define wxLogMessage(...) ((void) 0)

#ifndef wxLOG_COMPONENT
#define wxLOG_COMPONENT ""
#endif

enum { wxLOG_Debug = 5 };

class wxLog {
public:
    static bool IsLevelEnabled(int, const wxString&) { return false; }
    static bool EnableLogging(bool = true) { return false; }
};

inline void wxVLogWarning(const char*, va_list) {}
