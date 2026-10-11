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

// AISneakPlayer.h
// GeneralsX @feature ai-upgrade 07/10/2026 "Sneak" hit-and-run AI persona (experimental/wild style).
// Derived from AISkirmishPlayer so the original AIPlayer / AISkirmishPlayer stay untouched.

#pragma once

#include "Common/GameMemory.h"
#include "GameLogic/AISkirmishPlayer.h"

/**
 * Sneak skirmish AI. Avoids pitched battles in favor of economic harassment: prefers
 * stealth / fast / infiltration-style units, snipes supply and dozers, and disengages
 * before the enemy can bring its army to bear. Uses a normal team cadence (its edge is
 * shot selection, not tempo). Difficulty remains an orthogonal numeric knob.
 */
class AISneakPlayer : public AISkirmishPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AISneakPlayer, "AISneakPlayer" )

public:

	AISneakPlayer( Player *p );			///< constructor

	virtual AIStyleType getAIStyle() const override { return AI_STYLE_SNEAK; }

protected:
	// GeneralsX @feature ai-upgrade 07/10/2026 P1 composition bias: sneak avoids heavy armored
	// blobs and sticks to light hit-and-run forces (see AIStylePersona.h). Base gate runs first.
	virtual Bool isAGoodIdeaToBuildTeam( TeamPrototype *proto ) override;
};
