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

// AISneakPlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 "Sneak" hit-and-run AI persona.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"
#include "GameLogic/AI.h"
#include "GameLogic/AISneakPlayer.h"
#include "GameLogic/AIStylePersona.h"

//-------------------------------------------------------------------------------------------------
AISneakPlayer::AISneakPlayer( Player *p ) :
AISkirmishPlayer( p )
{
	// GeneralsX @feature ai-upgrade 07/10/2026 Harassment, not tempo: a slightly relaxed team
	// delay (~1.25x) reflects hit-and-run raids rather than spearhead pushes. The persona's real
	// edge is unit/target selection (P1 hooks). Integer math only (determinism red line).
	Int baseDelay = TheAI->getAiData()->m_teamSeconds;
	Int sneakDelay = baseDelay + baseDelay / 4;
	if (sneakDelay < baseDelay)		// guard against overflow on absurd INI values
		sneakDelay = baseDelay;
	setTeamDelaySeconds( sneakDelay );
}

//-------------------------------------------------------------------------------------------------
AISneakPlayer::~AISneakPlayer()
{
	// Base AISkirmishPlayer destructor already disbands teams and releases queued work orders.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 07/10/2026 P1 composition bias: sneak rejects heavy armored blobs
// on top of the base gate, staying light for hit-and-run / economic harassment.
Bool AISneakPlayer::isAGoodIdeaToBuildTeam( TeamPrototype *proto )
{
	if (!AISkirmishPlayer::isAGoodIdeaToBuildTeam(proto))
		return false;
	if (aiStylePersonaAcceptsTeam(AI_STYLE_SNEAK, proto, m_player))
		return true;
	// Disfavoured team: only defer it while a light hit-and-run alternative is buildable.
	// When nothing preferred can be made right now, build it anyway so we stay aggressive.
	std::function<Bool(TeamPrototype *)> baseOk =
		[this](TeamPrototype *c) { return AISkirmishPlayer::isAGoodIdeaToBuildTeam(c); };
	if (aiStyleHasPreferredAlternative(AI_STYLE_SNEAK, m_player, baseOk))
		return false;
	return true;
}
