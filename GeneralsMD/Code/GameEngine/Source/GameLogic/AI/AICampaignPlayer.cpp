/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

// AICampaignPlayer.cpp
// GeneralsX @feature campaign-ai 22/10/2026

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GlobalData.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "Common/ThingTemplate.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/AI.h"
#include "GameLogic/AICampaignPlayer.h"
#include "GameLogic/Weapon.h"

AICampaignPlayer::AICampaignPlayer( Player *p ) : AIPlayer( p )
{
}

AICampaignPlayer::~AICampaignPlayer()
{
}

void AICampaignPlayer::update()
{
	AIPlayer::update();

	if( !TheGameLogic || !ThePlayerList || !TheAI || !m_player )
		return;

	// Give the world a couple of seconds to finish player/team setup.
	if( TheGameLogic->getFrame() < 2 * LOGICFRAMES_PER_SECOND )
		return;

	// Every five seconds, give idle AI-built combat teams an attack-move order.
	// No teamOnCreateScript is required for this; mission scripts remain optional set pieces.
	if( (TheGameLogic->getFrame() % (5 * LOGICFRAMES_PER_SECOND)) == 0 )
		commanderTick();
}

Player *AICampaignPlayer::findHumanEnemy()
{
	for( Int i = 0; i < ThePlayerList->getPlayerCount(); ++i )
	{
		Player *p = ThePlayerList->getNthPlayer( i );
		if( !p || p->getPlayerType() != PLAYER_HUMAN )
			continue;
		const Team *dt = p->getDefaultTeam();
		if( !dt )
			continue;
		if( m_player->getRelationship( dt ) == ENEMIES )
			return p;
	}
	return nullptr;
}

Bool AICampaignPlayer::findEnemyTarget( Player *enemy, Coord3D *target )
{
	Object *best = nullptr;
	Real bestDistSqr = 1.0e30f;

	for( Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		if( obj->getControllingPlayer() != enemy )
			continue;
		if( !obj->isKindOf( KINDOF_STRUCTURE ) )
			continue;

		// Prefer the command center.  It is the stable, mission-relevant target.
		if( obj->isKindOf( KINDOF_COMMANDCENTER ) )
		{
			*target = *obj->getPosition();
			return TRUE;
		}

		const Coord3D *pos = obj->getPosition();
		Real dx = pos->x - m_baseCenter.x;
		Real dy = pos->y - m_baseCenter.y;
		Real d = dx * dx + dy * dy;
		if( d < bestDistSqr )
		{
			bestDistSqr = d;
			best = obj;
		}
	}

	if( best )
	{
		*target = *best->getPosition();
		return TRUE;
	}
	return FALSE;
}

void AICampaignPlayer::commanderTick()
{
	Player *enemy = findHumanEnemy();
	if( !enemy )
		return;

	Coord3D target;
	if( !findEnemyTarget( enemy, &target ) )
		return;

	Player::PlayerTeamList::const_iterator it;
	for( it = m_player->getPlayerTeams()->begin(); it != m_player->getPlayerTeams()->end(); ++it )
	{
		TeamPrototype *proto = *it;
		if( !proto || !proto->getTemplateInfo() ||
			proto->getTemplateInfo()->m_productionPriority <= 0 )
			continue;

		for( DLINK_ITERATOR<Team> iter = proto->iterate_TeamInstanceList(); !iter.done(); iter.advance() )
		{
			Team *team = iter.cur();
			if( !team || !team->hasAnyUnits() || !team->isIdle() )
				continue;

			AIGroupPtr group = TheAI->createGroup();
			if( !group )
				continue;

#if RETAIL_COMPATIBLE_AIGROUP
			team->getTeamAsAIGroup( group );
#else
			team->getTeamAsAIGroup( group.Peek() );
#endif
			group->groupAttackMoveToPosition( &target, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI );
		}
	}
}
