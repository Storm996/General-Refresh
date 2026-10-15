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

// AIFortressPlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 "Fortress" extreme-turtle AI persona.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIFortressPlayer.h"
#include "GameLogic/AITrace.h"
#include "GameLogic/GameLogic.h"				// TheGameLogic::getFrame()

//-------------------------------------------------------------------------------------------------
AIFortressPlayer::AIFortressPlayer( Player *p ) :
AISkirmishPlayer( p )
{
	// GeneralsX @feature ai-upgrade 07/10/2026 Extreme turtle: triple the designer-configured
	// team delay so it hoards forces and lets dense static defense do the work, stewing toward
	// a late-game tech/superweapon payoff. Integer math only (determinism red line).
	Int baseDelay = TheAI->getAiData()->m_teamSeconds;
	Int fortressDelay = baseDelay * 3;
	if (fortressDelay < baseDelay)		// guard against overflow on absurd INI values
		fortressDelay = baseDelay;
	setTeamDelaySeconds( fortressDelay );
}

//-------------------------------------------------------------------------------------------------
AIFortressPlayer::~AIFortressPlayer()
{
	// Base AISkirmishPlayer destructor already disbands teams and releases queued work orders.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 14/10/2026 Extreme turtle timing: teams hold 120s at the rally
// (retail 60s) - the fortress wants its static defense line fully manned before anything moves.
Int AIFortressPlayer::getRallyTimeoutFrames() const
{
	return 120 * LOGICFRAMES_PER_SECOND;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 14/10/2026 Fortress wall-building loop: same dormant base-defense
// ring builder the Defensive persona activated, but on a 60s cadence and with larger caps, so the
// turtle literally walls up over time. Frame-modulo trigger only (no new serialized state), so
// x86_64 / ARM64 peers replay bit-identical decisions.
void AIFortressPlayer::update()
{
	AISkirmishPlayer::update();

	const Int interval = 60 * LOGICFRAMES_PER_SECOND;
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
		if (m_curFrontBaseDefense < 8)
		{
			buildAIBaseDefense(false);
			aiTrace("[AIFORT]", "%s front fortification frame=%u frontCount=%d",
				getDriverName(), frame, m_curFrontBaseDefense);
		}
	}
}
