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

// AISupportPlayer.h
// GeneralsX @feature ai-upgrade 07/10/2026 Support skirmish AI personality.
// Derived from AISkirmishPlayer so the original AIPlayer / AISkirmishPlayer stay untouched.

#pragma once

#include "Common/GameMemory.h"
#include "GameLogic/AISkirmishPlayer.h"

/**
 * Support skirmish AI. Prioritizes economy and tech growth, fielding versatile combined-force
 * teams at a moderate cadence rather than massing a single attack spearhead. Difficulty remains
 * an orthogonal numeric knob inherited from the base class.
 */
class AISupportPlayer : public AISkirmishPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AISupportPlayer, "AISupportPlayer" )

public:

	AISupportPlayer( Player *p );			///< constructor

	virtual AIStyleType getAIStyle() const override { return AI_STYLE_SUPPORT; }

	virtual const char *getDriverName() const override { return "AISupport"; }

	// GeneralsX @feature ai-upgrade 14/10/2026 Economy-first: one extra gatherer per supply
	// center on top of the difficulty INI row, funding the (expensive) support teams it prefers.
	virtual Int getGathererBonus() const override { return 1; }
};
