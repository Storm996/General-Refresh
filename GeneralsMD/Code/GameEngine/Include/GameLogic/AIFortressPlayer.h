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

// AIFortressPlayer.h
// GeneralsX @feature ai-upgrade 07/10/2026 "Fortress" extreme-turtle AI persona (wild style).
// Derived from AISkirmishPlayer so the original AIPlayer / AISkirmishPlayer stay untouched.

#pragma once

#include "Common/GameMemory.h"
#include "GameLogic/AISkirmishPlayer.h"

/**
 * Fortress skirmish AI. An exaggerated defensive persona: rarely attacks, walls up with
 * dense static base defense, and stews for a late-game superweapon / elite tech payoff.
 * Uses a very long team delay so it hoards forces instead of committing them. Difficulty
 * remains an orthogonal numeric knob inherited from the base class.
 */
class AIFortressPlayer : public AISkirmishPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AIFortressPlayer, "AIFortressPlayer" )

public:

	AIFortressPlayer( Player *p );			///< constructor

	virtual AIStyleType getAIStyle() const override { return AI_STYLE_FORTRESS; }

	virtual const char *getDriverName() const override { return "AIFortress"; }

	// GeneralsX @feature ai-upgrade 14/10/2026 Fortress personality behavior: slowest commit
	// window (120s), an extra gatherer per supply center to pay for the wall of defenses it
	// grows every 60s via the base-defense ring builder.
	virtual void update() override;

	virtual Int getRallyTimeoutFrames() const override;

	virtual Int getGathererBonus() const override { return 1; }
};
