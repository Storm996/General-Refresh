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
//
//  (c) 2001-2003 Electronic Arts Inc.                                              //
//
////////////////////////////////////////////////////////////////////////////////

// AIActionExecutor.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 action decoder.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameLogic/Neural/AIActionExecutor.h"

#include "Common/GameCommon.h"				// CMD_FROM_AI
#include "Common/Player.h"
#include "GameLogic/AI.h"							// AIUpdateInterface / ai* command helpers
#include "GameLogic/Module/AIUpdate.h"	// full AIUpdateInterface definition (Object.h only forward-declares)
#include "GameLogic/Object.h"
#include "GameLogic/Weapon.h"					// NO_MAX_SHOTS_LIMIT

// ---------------------------------------------------------------------------
// Slot validation: the observation was captured the same tick, but the death/decay systems of
// the frame's own update (which runs objects interleaved with AI players) may already have
// retired an entity, so nothing may be assumed. Returns a safely orderable Object* or NULL.
static Object* aiNeuralLiveOwn( const std::vector<Object*>& ownRef, Int index, Player* self )
{
	if ( index < 0 || index >= (Int)ownRef.size() )
		return NULL;

	Object* obj = ownRef[index];
	if ( !obj || obj->isDestroyed() || obj->isEffectivelyDead() )
		return NULL;

	// Ownership may have flipped (captured / hacked units) between encode and execute.
	if ( obj->getControllingPlayer() != self )
		return NULL;

	AIUpdateInterface* ai = obj->getAIUpdateInterface();
	if ( !ai )
		return NULL;

	return obj;
}

// ---------------------------------------------------------------------------
static Object* aiNeuralLiveEnemy( const std::vector<Object*>& enemyRef, Int index, Player* self )
{
	if ( index < 0 || index >= (Int)enemyRef.size() )
		return NULL;

	Object* obj = enemyRef[index];
	if ( !obj || obj->isDestroyed() || obj->isEffectivelyDead() )
		return NULL;

	// Must still NOT be ours (a just-captured former enemy becomes friendly - don't attack it).
	if ( obj->getControllingPlayer() == self )
		return NULL;

	return obj;
}

// ---------------------------------------------------------------------------
void AIActionExecutor::execute( const AIActionSink& sink, const AIObservation& /*obs*/, Player* self,
	std::vector<Object*>& ownRef, std::vector<Object*>& enemyRef )
{
	if ( !self )
		return;

	for ( Int i = 0; i < sink.count; ++i )
	{
		const AIAction& a = sink.items[i];
		if ( a.kind == AI_ACT_NONE )
			continue;

		Object* actor = aiNeuralLiveOwn( ownRef, a.ownIndex, self );
		if ( !actor )
			continue;

		AIUpdateInterface* ai = actor->getAIUpdateInterface();

		switch ( a.kind )
		{
			case AI_ACT_MOVE_TO:
			{
				Coord3D pos;
				pos.x = a.targetX;
				pos.y = a.targetY;
				pos.z = actor->getPosition() ? actor->getPosition()->z : 0.0f;
				ai->aiMoveToPosition( &pos, CMD_FROM_AI );
			}
			break;

			case AI_ACT_ATTACK_MOVE:
			{
				Coord3D pos;
				pos.x = a.targetX;
				pos.y = a.targetY;
				pos.z = actor->getPosition() ? actor->getPosition()->z : 0.0f;
				ai->aiAttackMoveToPosition( &pos, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI );
			}
			break;

			case AI_ACT_ATTACK_OBJECT:
			{
				Object* victim = aiNeuralLiveEnemy( enemyRef, a.enemyIndex, self );
				if ( victim )
					ai->aiAttackObject( victim, NO_MAX_SHOTS_LIMIT, CMD_FROM_AI );
			}
			break;

			default:
				break;
		}
	}
}
