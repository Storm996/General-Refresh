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

// AIOffensivePlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 Aggressive skirmish AI personality.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIOffensivePlayer.h"

//-------------------------------------------------------------------------------------------------
AIOffensivePlayer::AIOffensivePlayer( Player *p ) :
AISkirmishPlayer( p )
{
	// GeneralsX @feature ai-upgrade 07/10/2026 Offense pressures early: roughly halve the
	// designer-configured team delay so attack waves commit faster than the neutral AI.
	// Integer math only, to stay bit-identical across x86_64 / ARM64 (determinism red line).
	Int baseDelay = TheAI->getAiData()->m_teamSeconds;
	Int aggressiveDelay = baseDelay / 2;
	if (aggressiveDelay < 1)
		aggressiveDelay = 1;
	setTeamDelaySeconds( aggressiveDelay );
}

//-------------------------------------------------------------------------------------------------
AIOffensivePlayer::~AIOffensivePlayer()
{
	// Base AISkirmishPlayer destructor already disbands teams and releases queued work orders.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 14/10/2026 Sustained pressure: rallied teams commit after 40s
// instead of the retail 60s. Integer frame math only (determinism red line).
Int AIOffensivePlayer::getRallyTimeoutFrames() const
{
	return 40 * LOGICFRAMES_PER_SECOND;
}
