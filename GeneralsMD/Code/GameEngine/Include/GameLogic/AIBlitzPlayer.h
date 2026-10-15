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
//																																						//
////////////////////////////////////////////////////////////////////////////////

// AIBlitzPlayer.h
// GeneralsX @feature ai-upgrade 07/10/2026 "Blitz" armor-rush AI persona (experimental/wild style).
// Derived from AISkirmishPlayer so the original AIPlayer / AISkirmishPlayer stay untouched.

#pragma once

#include "Common/GameMemory.h"
#include "GameLogic/AISkirmishPlayer.h"

/**
 * Blitz skirmish AI. Skips the infantry war and saves for a single armored spearhead, then
 * commits one big fast tank push aimed at breaking the enemy economy outright. Uses a fast
 * (but not all-in) team cadence to concentrate force rather than drip-feed it. Difficulty
 * remains an orthogonal numeric knob inherited from the base class.
 */
class AIBlitzPlayer : public AISkirmishPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AIBlitzPlayer, "AIBlitzPlayer" )

public:

	AIBlitzPlayer( Player *p );			///< constructor

	virtual AIStyleType getAIStyle() const override { return AI_STYLE_BLITZ; }

	virtual const char *getDriverName() const override { return "AIBlitz"; }

	// GeneralsX @feature ai-upgrade 14/10/2026 The armored spearhead commits after 40s (retail 60s).
	virtual Int getRallyTimeoutFrames() const override;

protected:
	// GeneralsX @feature ai-upgrade 07/10/2026 P1 composition bias: blitz refuses pure foot teams
	// to force mechanized / armored spearheads (see AIStylePersona.h). Base gate still runs first.
	virtual Bool isAGoodIdeaToBuildTeam( TeamPrototype *proto ) override;
};
