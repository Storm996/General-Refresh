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

// NeuralPolicy.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 placeholder policies.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameLogic/Neural/NeuralPolicy.h"

#include <stdlib.h>			// getenv (setup-time policy selection only)
#include <string.h>			// strcmp

#include "GameLogic/LogicRandomValue.h"	// GameLogicRandomValue - the ONLY allowed RNG here

// ---------------------------------------------------------------------------
// RandomLegalPolicy
// Emits 0..3 actions per tick. Every referenced position/target comes from the observation
// itself, which by construction only carries (a) our own live orderable units and (b) enemy
// units inside our own fog-CLEAR view - so the policy can never "cheat" and can never name a
// target the player couldn't legally see. Legality of the command itself is still double
// checked by the engine when the executor routes it through the normal AI command channel.
void RandomLegalPolicy::act( const AIObservation& obs, Player* /*self*/, AIActionSink& sink )
{
	if ( obs.ownCount <= 0 )
		return;

	Int waves = GameLogicRandomValue( 0, 3 );
	for ( Int i = 0; i < waves; ++i )
	{
		Int actor = GameLogicRandomValue( 0, obs.ownCount - 1 );
		if ( actor < 0 || actor >= obs.ownCount )
			continue;

		AIAction a;
		a.ownIndex = actor;

		Int roll = GameLogicRandomValue( 0, 9 );

		if ( obs.enemyCount > 0 )
		{
			Int enemy = GameLogicRandomValue( 0, obs.enemyCount - 1 );
			if ( enemy >= 0 && enemy < obs.enemyCount )
			{
				if ( roll < 4 )
				{
					// Focus-fire a fog-visible enemy unit.
					a.kind					= AI_ACT_ATTACK_OBJECT;
					a.enemyIndex	= enemy;
				}
				else if ( roll < 7 )
				{
					// Attack-move toward a fog-visible enemy position.
					a.kind					= AI_ACT_ATTACK_MOVE;
					a.targetX			= obs.enemy[enemy].x;
					a.targetY			= obs.enemy[enemy].y;
				}
				else
				{
					// Plain move to that same visible point (reposition / probe).
					a.kind					= AI_ACT_MOVE_TO;
					a.targetX			= obs.enemy[enemy].x;
					a.targetY			= obs.enemy[enemy].y;
				}
			}
		}
		else
		{
			// No visible enemies: shuffle toward another own unit (rally-ish, always on-map).
			Int buddy = actor;
			if ( obs.ownCount > 1 )
			{
				buddy = GameLogicRandomValue( 0, obs.ownCount - 1 );
				if ( buddy == actor )
					buddy = ( buddy + 1 ) % obs.ownCount;
			}
			if ( buddy >= 0 && buddy < obs.ownCount )
			{
				a.kind			= AI_ACT_MOVE_TO;
				a.targetX	= obs.own[buddy].x;
				a.targetY	= obs.own[buddy].y;
			}
		}

		if ( a.kind != AI_ACT_NONE )
			sink.push( a );
	}
}

// ---------------------------------------------------------------------------
// createDefaultNeuralPolicy
// Setup-time (constructor) policy selection. Default is the zero-impact PassThroughPolicy so
// that simply mounting NeuralAIPlayer can never perturb the simulation. Setting the environment
// variable GENERALX_NEURAL_POLICY=random switches on the legal-random exerciser for Gate B.
NeuralPolicy* createDefaultNeuralPolicy()
{
	const char* sel = getenv( "GENERALX_NEURAL_POLICY" );
	if ( sel && strcmp( sel, "random" ) == 0 )
		return new RandomLegalPolicy;

	return new PassThroughPolicy;
}
