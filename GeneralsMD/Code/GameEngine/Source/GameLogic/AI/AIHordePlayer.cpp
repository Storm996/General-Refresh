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

// AIHordePlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 "Horde" attrition AI persona.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"
#include "Common/Money.h"
#include "GameLogic/AI.h"
#include "GameLogic/AIHordePlayer.h"
#include "GameLogic/AIStylePersona.h"
#include "GameLogic/AITrace.h"
#include "GameLogic/GameLogic.h"				// TheGameLogic::getFrame()

//-------------------------------------------------------------------------------------------------
AIHordePlayer::AIHordePlayer( Player *p ) :
AISkirmishPlayer( p )
{
	// GeneralsX @feature ai-upgrade 07/10/2026 Sustained attrition: halve the designer-configured
	// team delay so a constant stream of cheap units keeps flowing all game (rather than one
	// all-in push). Integer math only (determinism red line).
	Int baseDelay = TheAI->getAiData()->m_teamSeconds;
	Int hordeDelay = baseDelay / 2;
	if (hordeDelay < 1)
		hordeDelay = 1;
	setTeamDelaySeconds( hordeDelay );
}

//-------------------------------------------------------------------------------------------------
AIHordePlayer::~AIHordePlayer()
{
	// Base AISkirmishPlayer destructor already disbands teams and releases queued work orders.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 14/10/2026 Waves commit after 45s (retail 60s): pressure stays
// continuous without being a pure all-in. Integer frame math only (determinism red line).
Int AIHordePlayer::getRallyTimeoutFrames() const
{
	return 45 * LOGICFRAMES_PER_SECOND;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature ai-upgrade 14/10/2026 Sustained-attrition queue gate. Mirrors the base
// skirmish gate (production condition / instance cap / factory+money buildability) but relaxes
// the retail "one queued instance per prototype" rule to TWO concurrent queue entries: while the
// bank can pay for two waves of the same team at once, a second copy is queued so the horde never
// stops flowing. Extra queue copy only consumes regular build money through the normal flow -
// no new state, integer math throughout (determinism red line). Composition filter and the
// never-starve fallback keep the P0 semantics unchanged.
Bool AIHordePlayer::isAGoodIdeaToBuildTeam( TeamPrototype *proto )
{
	if (!proto || !proto->getTemplateInfo())
		return false;

	if (!proto->evaluateProductionCondition())
		return false;

	if (proto->countTeamInstances() >= proto->getTemplateInfo()->m_maxInstances)
		return false;

	Int queued = 0;
	for ( DLINK_ITERATOR<TeamInQueue> iter = iterate_TeamBuildQueue(); !iter.done(); iter.advance() )
	{
		TeamInQueue *team = iter.cur();
		if (team->m_team && team->m_team->getPrototype() == proto)
			++queued;
	}
	if (queued >= 2)
		return false;

	Bool needMoney;
	if (!isPossibleToBuildTeam(proto, true, needMoney))
		return false;

	if (queued == 1)
	{
		// Second concurrent wave: only when the bank can pay for two of them at once.
		// Cost estimate matches the engine's own (avg units per entry * per-unit cost),
		// done with integer division only.
		Int teamCost = 0;
		const TeamTemplateInfo *info = proto->getTemplateInfo();
		Int num = info->m_numUnitsInfo;
		if (num > TeamTemplateInfo::MAX_UNIT_TYPES)
			num = TeamTemplateInfo::MAX_UNIT_TYPES;
		for (Int i = 0; i < num; ++i)
		{
			const ThingTemplate *thing = TheThingFactory->findTemplate( info->m_unitsInfo[i].unitThingName );
			if (thing)
				teamCost += thing->calcCostToBuild(m_player) *
					((info->m_unitsInfo[i].minUnits + info->m_unitsInfo[i].maxUnits) / 2);
		}
		Int balance = m_player->getMoney()->countMoney();
		if (teamCost <= 0 || balance <= 2 * teamCost)
			return false;
		aiTrace("[AIHORDE]", "double-queue team=%s queued=%d balance=%d teamCost=%d frame=%u",
			proto->getName().str(), queued, balance, teamCost, TheGameLogic->getFrame());
	}

	// Persona composition filter with the established never-starve fallback.
	if (aiStylePersonaAcceptsTeam(AI_STYLE_HORDE, proto, m_player))
		return true;
	std::function<Bool(TeamPrototype *)> baseOk =
		[this](TeamPrototype *c) { return AISkirmishPlayer::isAGoodIdeaToBuildTeam(c); };
	if (aiStyleHasPreferredAlternative(AI_STYLE_HORDE, m_player, baseOk))
		return false;
	return true;
}
