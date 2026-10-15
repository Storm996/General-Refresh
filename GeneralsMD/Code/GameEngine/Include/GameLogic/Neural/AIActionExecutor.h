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

// AIActionExecutor.h
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 action decoder: the ONLY place in the
// neural pipeline that touches (read: commands) live engine objects. Actions are routed through
// the exact same AICommandInterface channel the built-in AI uses (CMD_FROM_AI), so they enter the
// recorded/synced command stream automatically - replayable and lockstep-safe.

#pragma once

#include "GameLogic/Neural/NeuralTypes.h"

#include <vector>

class Player;

class AIActionExecutor
{
public:

	AIActionExecutor() {}

	/**
	 * Execute sink's actions against the slot reference lists produced alongside obs by
	 * AIObservationProvider. Every slot is re-validated (range / alive / still ours / still
	 * orderable) before its command is issued; stale or illegal actions are silently dropped.
	 * Returns the number of actions that actually reached the command channel, so the caller
	 * can log applied vs scheduled (Gate C forensics: applied==0 with scheduled>0 means the
	 * decoder is issuing commands the engine refuses).
	 */
	Int execute( const AIActionSink& sink, const AIObservation& obs, Player* self,
		std::vector<Object*>& ownRef, std::vector<Object*>& enemyRef );
};
