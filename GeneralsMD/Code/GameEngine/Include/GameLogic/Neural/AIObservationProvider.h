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

// AIObservationProvider.h
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 observation encoder: turns live GameLogic
// state into a fixed-schema AIObservation, scoped to what this player is legitimately allowed to
// see (own units + fog-CLEAR enemy contacts only - reading the god-view would be cheating and
// would break replay CRC by definition).

#pragma once

#include "GameLogic/Neural/NeuralTypes.h"

#include <vector>

class Player;

class AIObservationProvider
{
public:

	AIObservationProvider() {}

	/**
	 * Build a fresh observation for self.
	 *
	 * ownRef / enemyRef are parallel output vectors: slot i of obs.own[] refers to ownRef[i],
	 * obs.enemy[] to enemyRef[i]. The policy only ever addresses these by index; the executor
	 * (and nobody else) dereferences the raw Object pointers, re-validating them first because
	 * the game simulation may have killed/desynced an entity between encode and execute within
	 * the same tick only via this single-threaded call chain, so pointers stay stable.
	 */
	void buildObservation( Player* self, AIObservation& out,
		std::vector<Object*>& ownRef, std::vector<Object*>& enemyRef );
};
