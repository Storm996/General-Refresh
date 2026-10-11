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

// AIObservationProvider.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 observation encoder.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameLogic/Neural/AIObservationProvider.h"

#include "GameLogic/GameLogic.h"				// TheGameLogic, getFirstObject/getNextObject/getFrame
#include "GameLogic/Object.h"					// Object traversal, controllers, templates, body
#include "GameLogic/PartitionManager.h"	// fog-scope queries (the fairness boundary)
#include "Common/Player.h"					// Player identity + money
#include "Common/Money.h"						// Money::countMoney()
#include "Common/ThingTemplate.h"		// KindOf filtering + template names
#include "GameLogic/Module/BodyModule.h"

// ---------------------------------------------------------------------------
// Stable cross-machine template id: djb2 over the template name. We deliberately hash the
// ASCII name (identical on every peer / replay) instead of the pointer or an ini list index,
// which would drift. Truncated to Int; collisions are acceptable for a feature channel.
static Int aiNeuralTemplateId( const Object* obj )
{
	const ThingTemplate* t = obj->getTemplate();
	if ( !t )
		return 0;

	const char* name = t->getName().str();
	if ( !name )
		return 0;

	UnsignedInt hash = 5381;
	for ( const char* p = name; *p; ++p )
		hash = ( ( hash << 5 ) + hash ) + (UnsignedInt)(unsigned char)*p;

	return (Int)hash;
}

// ---------------------------------------------------------------------------
// Coarse archetype channel.
static Int aiNeuralEntityKind( const Object* obj )
{
	if ( obj->isKindOf( KINDOF_INFANTRY ) )
		return AI_EK_INFANTRY;
	if ( obj->isKindOf( KINDOF_VEHICLE ) )
		return AI_EK_VEHICLE;
	if ( obj->isKindOf( KINDOF_AIRCRAFT ) )
		return AI_EK_AIRCRAFT;
	if ( obj->isKindOf( KINDOF_STRUCTURE ) )
		return AI_EK_STRUCTURE;
	return AI_EK_NONE;
}

// ---------------------------------------------------------------------------
void AIObservationProvider::buildObservation( Player* self, AIObservation& out,
	std::vector<Object*>& ownRef, std::vector<Object*>& enemyRef )
{
	out.reset();
	ownRef.clear();
	enemyRef.clear();

	if ( !self )
		return;

	out.selfPlayerIndex = (Int)self->getPlayerIndex();
	out.frame						= TheGameLogic->getFrame();

	Int totalOwnUnits		= 0;
	Int totalOwnStructs	= 0;
	Int totalSeenEnemies	= 0;
	Int ownInfantry				= 0;
	Int ownVehicles				= 0;
	Int ownAircraft				= 0;

	for ( Object* obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject() )
	{
		// Skip anything no longer in fighting condition (also covers pending destruction).
		if ( obj->isDestroyed() || obj->isEffectivelyDead() )
			continue;

		const ThingTemplate* t = obj->getTemplate();
		if ( !t )
			continue;

		Player* owner = obj->getControllingPlayer();

		if ( owner == self )
		{
			if ( obj->isKindOf( KINDOF_STRUCTURE ) )
			{
				++totalOwnStructs;
				continue;		// buildings inform scalars only, not the orderable unit list
			}

			// Orderable = something we can hand a move/attack command to this tick.
			if ( !obj->getAIUpdateInterface() )
				continue;

			++totalOwnUnits;
			Int kind = aiNeuralEntityKind( obj );
			if ( kind == AI_EK_INFANTRY )	++ownInfantry;
			else if ( kind == AI_EK_VEHICLE )	++ownVehicles;
			else if ( kind == AI_EK_AIRCRAFT )	++ownAircraft;
			else continue;	// crates, props, weird hybrids: not orderable features in P0

			if ( out.ownCount < AI_OBS_MAX_OWN )
			{
				const Coord3D* pos = obj->getPosition();
				AIEntityFeature& f = out.own[out.ownCount];
				f.templateId	= aiNeuralTemplateId( obj );
				f.kind				= kind;
				f.x							= pos ? pos->x : 0.0f;
				f.y							= pos ? pos->y : 0.0f;
				f.healthFrac		= 0.0f;
				BodyModuleInterface* body = obj->getBodyModule();
				if ( body && body->getMaxHealth() > 0.0f )
					f.healthFrac = body->getHealth() / body->getMaxHealth();

				ownRef.push_back( obj );
				++out.ownCount;
			}
		}
		else if ( owner )
		{
			// ONLY true enemies belong in the contact list. Neutral civilians / alliance-
			// partners must be excluded, otherwise the policy starts attacking neutrals (observed
			// as "mad-dog attacking civilian cars"). Use the engine's own relationship model.
			if ( self->getRelationship( obj->getTeam() ) != ENEMIES )
				continue;

			// Enemy contacts must pass OUR fog: CELLSHROUD_CLEAR for self at the entity's
			// position. Anything shadowed stays invisible - the hard fairness rule.
			const Coord3D* pos = obj->getPosition();
			if ( !pos )
				continue;
			if ( ThePartitionManager->getShroudStatusForPlayer( out.selfPlayerIndex, pos ) != CELLSHROUD_CLEAR )
				continue;

			Int kind = aiNeuralEntityKind( obj );
			if ( kind == AI_EK_NONE )
				continue;
			++totalSeenEnemies;

			if ( out.enemyCount < AI_OBS_MAX_ENEMY )
			{
				AIEntityFeature& f = out.enemy[out.enemyCount];
				f.templateId	= aiNeuralTemplateId( obj );
				f.kind				= kind;
				f.x							= pos->x;
				f.y							= pos->y;
				f.healthFrac		= 0.0f;
				BodyModuleInterface* body = obj->getBodyModule();
				if ( body && body->getMaxHealth() > 0.0f )
					f.healthFrac = body->getHealth() / body->getMaxHealth();

				enemyRef.push_back( obj );
				++out.enemyCount;
			}
		}
	}

	// ---------------------------------------------------------------------
	// Global scalar plane (schema v1 layout, keep append-only!).
	UnsignedInt cash = 0;
	if ( self->getMoney() )
		cash = self->getMoney()->countMoney();

	out.scalars[0] = (Real)cash;								// bank balance
	out.scalars[1] = (Real)totalOwnUnits;					// all own orderable units (unclipped)
	out.scalars[2] = (Real)totalOwnStructs;				// own buildings
	out.scalars[3] = (Real)totalSeenEnemies;			// fog-visible enemy contacts (unclipped)
	out.scalars[4] = (Real)out.frame;							// logical frame (net input normalization later)
	out.scalars[5] = (Real)ownInfantry;					// own composition
	out.scalars[6] = (Real)ownVehicles;
	out.scalars[7] = (Real)ownAircraft;
	out.scalarCount	= 8;
}
