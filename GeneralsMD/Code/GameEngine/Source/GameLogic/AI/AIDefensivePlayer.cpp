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

// AIDefensivePlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 Defensive skirmish AI personality.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIDefensivePlayer.h"
#include "GameLogic/AITrace.h"
#include "GameLogic/GameLogic.h"				// TheGameLogic::getFrame()

//-------------------------------------------------------------------------------------------------
AIDefensivePlayer::AIDefensivePlayer( Player *p ) :
AISkirmishPlayer( p )
{
	// GeneralsX @feature ai-upgrade 07/10/2026 Defense consolidates first: double the
	// designer-configured team delay so it holds back forces while building up position.
	// Integer math only, to stay bit-identical across x86_64 / ARM64 (determinism red line).
	Int baseDelay = TheAI->getAiData()->m_teamSeconds;
	Int defensiveDelay = baseDelay * 2;
	if (defensiveDelay < baseDelay)		// guard against overflow on absurd INI values
		defensiveDelay = baseDelay;
	setTeamDelaySeconds( defensiveDelay );
}

//-------------------------------------------------------------------------------------------------
AIDefensivePlayer::~AIDefensivePlayer()
{
	// Base AISkirmishPlayer destructor already disbands teams and releases queued work orders.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 14/10/2026 Turtle timing: rallied teams hold 90s (retail 60s)
// so the defensive line is dug in before forces commit. Integer frame math only (determinism
// red line).
Int AIDefensivePlayer::getRallyTimeoutFrames() const
{
	return 90 * LOGICFRAMES_PER_SECOND;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 14/10/2026 Fortification loop. Retail only grew the base-defense
// ring from scripted campaign triggers, which skirmish maps never fire - this persona activates
// that dormant builder on a fixed frame cadence: every 90s of sim time it adds one structure to
// the ring, alternating front / flank arcs, capped so the ring stops at a sane size. All triggers
// are pure functions of TheGameLogic->getFrame(), so every peer replays identical decisions.
void AIDefensivePlayer::update()
{
	AISkirmishPlayer::update();

	const Int interval = 90 * LOGICFRAMES_PER_SECOND;
	const UnsignedInt frame = TheGameLogic->getFrame();
	if (frame == 0 || frame < 2 * interval || frame % interval != 0)
		return;

	if ((frame / interval) & 1)
	{
		if (m_curFlankBaseDefense < 4)
		{
			buildAIBaseDefense(true);
			aiTrace("[AIFORT]", "%s flank fortification frame=%u flankCount=%d",
				getDriverName(), frame, m_curFlankBaseDefense);
		}
	}
	else
	{
		if (m_curFrontBaseDefense < 4)
		{
			buildAIBaseDefense(false);
			aiTrace("[AIFORT]", "%s front fortification frame=%u frontCount=%d",
				getDriverName(), frame, m_curFrontBaseDefense);
		}
	}
}
