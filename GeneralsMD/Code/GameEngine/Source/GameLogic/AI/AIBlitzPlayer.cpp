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

// AIBlitzPlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 "Blitz" armor-rush AI persona.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIBlitzPlayer.h"
#include "GameLogic/AIStylePersona.h"

//-------------------------------------------------------------------------------------------------
AIBlitzPlayer::AIBlitzPlayer( Player *p ) :
AISkirmishPlayer( p )
{
	// GeneralsX @feature ai-upgrade 07/10/2026 Fast but concentrated push: a third of the
	// designer-configured team delay. Blitz wants fewer, heavier spearheads rather than a
	// non-stop drip, so it is slightly more restrained than pure rush. Integer math only
	// (determinism red line).
	Int baseDelay = TheAI->getAiData()->m_teamSeconds;
	Int blitzDelay = baseDelay / 3;
	if (blitzDelay < 1)
		blitzDelay = 1;
	setTeamDelaySeconds( blitzDelay );
}

//-------------------------------------------------------------------------------------------------
AIBlitzPlayer::~AIBlitzPlayer()
{
	// Base AISkirmishPlayer destructor already disbands teams and releases queued work orders.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 07/10/2026 P1 composition bias: blitz refuses pure foot teams on
// top of the base gate, pushing mechanized / armored spearheads for a single fast breakthrough.
Bool AIBlitzPlayer::isAGoodIdeaToBuildTeam( TeamPrototype *proto )
{
	if (!AISkirmishPlayer::isAGoodIdeaToBuildTeam(proto))
		return false;
	if (aiStylePersonaAcceptsTeam(AI_STYLE_BLITZ, proto, m_player))
		return true;
	// Disfavoured team: only defer it while an armor/mech alternative is actually buildable.
	// When nothing preferred can be made right now, build it anyway so we stay aggressive.
	std::function<Bool(TeamPrototype *)> baseOk =
		[this](TeamPrototype *c) { return AISkirmishPlayer::isAGoodIdeaToBuildTeam(c); };
	if (aiStyleHasPreferredAlternative(AI_STYLE_BLITZ, m_player, baseOk))
		return false;
	return true;
}
