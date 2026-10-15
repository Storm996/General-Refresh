/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
////////////////////////////////////////////////////////////////////////////////

// AITrace.h
// GeneralsX @feature ai-upgrade 14/10/2026 Runtime forensic logging channel for the AI upgrade
// workstream. When the environment variable GENERALX_AI_TRACE is set (any value), the AI
// subsystem emits human-readable [AIPLAYER]/[AISTYLE]/[AITEAM]/[QASKIRM] lines to stderr so that
// the automated match-entry scripts can PROVE, from the log alone, which AI driver class was
// instantiated, which persona decisions fired, and which teams were queued/activated.
//
// Determinism contract (red line):
//   * The flag is read once, lazily, and cached; reading the environment never happens inside a
//     numeric decision path and never feeds simulation state.
//   * aiTrace() ONLY writes to stderr. It mutates no game state, consumes no GameLogicRandom,
//     and is therefore byte-identical-safe for CRC/sync purposes on any platform combination
//     (peers may differ in whether they log; the simulation is unaffected).
//   * With the variable unset (the shipped default) every call compiles to a single static-bool
//     test and produces zero output.

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// GeneralsX @feature ai-upgrade 14/10/2026 True when GENERALX_AI_TRACE is set in the process env.
inline Bool aiTraceEnabled()
{
	static Int s_state = -1;
	if (s_state < 0)
		s_state = (getenv("GENERALX_AI_TRACE") != NULL) ? 1 : 0;
	return s_state == 1;
}

// GeneralsX @feature ai-upgrade 14/10/2026 Emit one forensic line. 'tag' should start with the
// marker bracket so log scrapers can grep it (e.g. "[AISTYLE]").
inline void aiTrace(const char *tag, const char *fmt, ...)
{
	if (!aiTraceEnabled())
		return;
	va_list args;
	va_start(args, fmt);
	fprintf(stderr, "%s ", tag);
	vfprintf(stderr, fmt, args);
	fprintf(stderr, "\n");
	fflush(stderr);
	va_end(args);
}
