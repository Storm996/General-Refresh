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

// NeuralTypes.h
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 scaffolding: the fixed-schema data types
// shared by the observation encoder, the (pluggable) policy and the action executor. Everything
// here is a plain POD with fixed-size arrays and a schema version so that recorded training data
// stays well-defined. No engine state is mutated by these types; they are pure value carriers.

#pragma once

#include "Common/GameCommon.h"	// Int / Real / Bool / UnsignedInt

class Object;

// ---------------------------------------------------------------------------
// Entity archetype (coarse classifier used as a network feature channel).
enum AIEntityKind
{
	AI_EK_NONE = 0,
	AI_EK_INFANTRY,
	AI_EK_VEHICLE,
	AI_EK_AIRCRAFT,
	AI_EK_STRUCTURE
};

// ---------------------------------------------------------------------------
// Fixed capacities. Baked into the schema; changing them bumps AI_OBS_SCHEMA_VERSION.
enum
{
	AI_OBS_SCHEMA_VERSION	= 1,
	AI_OBS_MAX_OWN			= 64,	// max of our own orderable units sampled into the observation
	AI_OBS_MAX_ENEMY			= 64,	// max fog-visible enemy units sampled
	AI_OBS_SCALAR_COUNT		= 16,	// global scalar feature plane (economy, counts, ...)
	AI_MAX_ACTIONS			= 64	// max actions a policy may emit per decision tick
};

// ---------------------------------------------------------------------------
// One entity (own or enemy) reduced to a small, comparable feature vector.
struct AIEntityFeature
{
	AIEntityFeature() : templateId(0), kind(AI_EK_NONE), x(0), y(0), healthFrac(0) {}

	Int		templateId;		// stable djb2 hash of ThingTemplate name (peers agree on it)
	Int		kind;			// AIEntityKind
	Real		x, y;			// world position
	Real		healthFrac;	// current/max health, clamped to [0,1]
};

// ---------------------------------------------------------------------------
// Full observation snapshot handed to the policy. Built fresh each decision tick; the parallel
// Object* reference lists live outside this struct (kept in NeuralAIPlayer) because the policy
// must never dereference raw engine objects itself, only address them by slot index.
struct AIObservation
{
	AIObservation() { reset(); }

	void reset()
	{
		schemaVersion = AI_OBS_SCHEMA_VERSION;
		frame = 0;
		selfPlayerIndex = -1;
		scalarCount = 0;
		ownCount = 0;
		enemyCount = 0;
		for (Int i = 0; i < AI_OBS_SCALAR_COUNT; ++i)
			scalars[i] = 0.0f;
	}

	Int					schemaVersion;
	UnsignedInt		frame;				// TheGameLogic frame the obs was captured on
	Int					selfPlayerIndex;	// player index we are observing as (fog scope)

	Real					scalars[AI_OBS_SCALAR_COUNT];
	Int					scalarCount;

	Int					ownCount;
	AIEntityFeature		own[AI_OBS_MAX_OWN];

	Int					enemyCount;
	AIEntityFeature		enemy[AI_OBS_MAX_ENEMY];
};

// ---------------------------------------------------------------------------
// Layered action space (low-level micro for P0; higher-level build orders come in P1). A move /
// attack-move addresses a world point; attack-object addresses a fog-visible enemy slot.
enum AIActionKind
{
	AI_ACT_NONE = 0,
	AI_ACT_MOVE_TO,
	AI_ACT_ATTACK_MOVE,
	AI_ACT_ATTACK_OBJECT
};

struct AIAction
{
	AIAction() : kind(AI_ACT_NONE), ownIndex(-1), enemyIndex(-1), targetX(0), targetY(0) {}

	AIActionKind	kind;
	Int					ownIndex;		// slot into obs.own[] / ownRef selecting the acting unit
	Int					enemyIndex;		// slot into obs.enemy[] / enemyRef (attack-object only)
	Real					targetX, targetY;	// world target for move / attack-move
};

// ---------------------------------------------------------------------------
// Output buffer a policy fills with actions. Fixed-capacity; overflow entries are dropped.
struct AIActionSink
{
	AIActionSink() : count(0) {}

	void reset() { count = 0; }
	void push(const AIAction& a) { if (count < AI_MAX_ACTIONS) items[count++] = a; }

	Int			count;
	AIAction	items[AI_MAX_ACTIONS];
};
