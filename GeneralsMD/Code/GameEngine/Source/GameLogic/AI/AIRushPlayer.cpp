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

// AIRushPlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 "Mad-dog" rush AI persona.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIRushPlayer.h"
#include "GameLogic/AIStylePersona.h"

//-------------------------------------------------------------------------------------------------
AIRushPlayer::AIRushPlayer( Player *p ) :
AISkirmishPlayer( p )
{
	// GeneralsX @feature ai-upgrade 07/10/2026 All-in rush: quarter the designer-configured team
	// delay so waves leave the base almost non-stop from the opening. Integer math only, to stay
	// bit-identical across x86_64 / ARM64 (determinism red line).
	Int baseDelay = TheAI->getAiData()->m_teamSeconds;
	Int rushDelay = baseDelay / 4;
	if (rushDelay < 1)
		rushDelay = 1;
	setTeamDelaySeconds( rushDelay );
}

//-------------------------------------------------------------------------------------------------
AIRushPlayer::~AIRushPlayer()
{
	// Base AISkirmishPlayer destructor already disbands teams and releases queued work orders.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 14/10/2026 Mad-dog timing: the first waves leave the rally
// point after only 30s (retail 60s), so the early game is a continuous drumbeat.
Int AIRushPlayer::getRallyTimeoutFrames() const
{
	return 30 * LOGICFRAMES_PER_SECOND;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 07/10/2026 P1 composition bias: the mad-dog layers a cheap-foot
// filter on top of the base gate, so it spams low-cost infantry instead of saving for heavy tech.
Bool AIRushPlayer::isAGoodIdeaToBuildTeam( TeamPrototype *proto )
{
	if (!AISkirmishPlayer::isAGoodIdeaToBuildTeam(proto))
		return false;
	if (aiStylePersonaAcceptsTeam(AI_STYLE_RUSH, proto, m_player))
		return true;
	// Disfavoured team: only defer it while a cheap infantry alternative is actually buildable.
	// When nothing preferred can be made right now, build it anyway so we stay aggressive.
	std::function<Bool(TeamPrototype *)> baseOk =
		[this](TeamPrototype *c) { return AISkirmishPlayer::isAGoodIdeaToBuildTeam(c); };
	if (aiStyleHasPreferredAlternative(AI_STYLE_RUSH, m_player, baseOk))
		return false;
	return true;
}
