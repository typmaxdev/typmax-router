// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 The typmax-router authors
// wxWidgets stand-in: assertions off (the build defines NDEBUG), the wxCHECK
// family keeps its early-return behaviour as in a wxWidgets release build.
#pragma once

#define wxASSERT(c) ((void) 0)
#define wxASSERT_MSG(c, m) ((void) 0)
#define wxFAIL_MSG(m) ((void) 0)
#define wxFAIL ((void) 0)
#define wxCHECK(c, r) do { if (!(c)) return r; } while (0)
#define wxCHECK_MSG(c, r, m) do { if (!(c)) return r; } while (0)
#define wxCHECK_RET(c, m) do { if (!(c)) return; } while (0)
#define wxCHECK2(c, op) do { if (!(c)) { op; } } while (0)
#define wxCHECK2_MSG(c, op, m) do { if (!(c)) { op; } } while (0)
