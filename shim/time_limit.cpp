// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2013-2014 CERN; The KiCad Developers (the upstream file, KiCad 10.0.7 pcbnew/router/time_limit.cpp)
// SPDX-FileCopyrightText: 2026 The typmax-router authors (the steady_clock port, the INT_MAX and test-clock lines)
// Replaces KiCad's time_limit.cpp, which reads wxWidgets' wxGetLocalTimeMillis().
/*
 * KiRouter - a push-and-(sometimes-)shove PCB router
 *
 * Copyright (C) 2013-2014 CERN
 * Copyright The KiCad Developers, see AUTHORS.txt for contributors.
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>

#include "time_limit.h"

namespace PNS {

// typmax-router: INT_MAX milliseconds means "no wall-clock cap" (the service
// sets it for shove through ROUTING_SETTINGS' "shove_time_limit" parameter:
// SOURCE.md, review D-1). The test hook
// TYPMAX_ROUTER_TEST_CLOCK=expired makes every FINITE limit read as already
// expired, the limit case of a machine under load; with the cap off, a shove's
// answer must not move under it (tests/run_tests.py t_shove_no_wall_clock).
static bool clock_hook_expired()
{
    static const bool on = [] {
        const char* v = std::getenv( "TYPMAX_ROUTER_TEST_CLOCK" );
        return v && std::strcmp( v, "expired" ) == 0;
    }();
    return on;
}

TIME_LIMIT::TIME_LIMIT( int aMilliseconds ) :
    m_limitMs( aMilliseconds )
{
    Restart();
}


TIME_LIMIT::~TIME_LIMIT()
{}


static int64_t get_millis() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool TIME_LIMIT::Expired() const
{
    if( m_limitMs == INT_MAX )
        return false;

    if( clock_hook_expired() )
        return true;

    return ( get_millis() - m_startTics ) >= m_limitMs;
}


void TIME_LIMIT::Restart()
{
    m_startTics = get_millis();
}


void TIME_LIMIT::Set( int aMilliseconds )
{
    m_limitMs = aMilliseconds;
}

}
