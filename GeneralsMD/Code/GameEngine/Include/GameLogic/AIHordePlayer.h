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

// AIHordePlayer.h
// GeneralsX @feature ai-upgrade 07/10/2026 "Horde" attrition AI persona (experimental/wild style).
// Derived from AISkirmishPlayer so the original AIPlayer / AISkirmishPlayer stay untouched.

#pragma once

#include "Common/GameMemory.h"
#include "GameLogic/AISkirmishPlayer.h"

/**
 * Horde skirmish AI. Wins by volume rather than quality: keeps a high-tempo stream of
 * cheap expendable units flowing at all times. Unlike rush (which all-ins early), horde
 * sustains endless waves throughout the game. Difficulty is an orthogonal knob.
 */
class AIHordePlayer : public AISkirmishPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AIHordePlayer, "AIHordePlayer" )

public:

	AIHordePlayer( Player *p );			///< constructor

	virtual AIStyleType getAIStyle() const override { return AI_STYLE_HORDE; }

protected:
	// GeneralsX @feature ai-upgrade 07/10/2026 P1 composition bias: horde keeps cheap + light
	// forces and rejects heavy tech blobs for endless attrition waves (see AIStylePersona.h).
	virtual Bool isAGoodIdeaToBuildTeam( TeamPrototype *proto ) override;
};
