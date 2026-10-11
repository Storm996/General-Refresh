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

// NeuralPolicy.h
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 scaffolding: the pluggable decision
// interface. A policy is a pure observer: it consumes an AIObservation and emits AIActions into
// an AIActionSink. It must never touch engine state directly (no Object* dereferencing, no
// commands) - that is the executor's job - and must never use time()/rand()/CWD so a future
// learned network stays replay-deterministic.
//
// P0 ships two deterministic placeholder policies (no real network anywhere):
//   PassThroughPolicy - emits nothing; validates the observe/hook plumbing with zero risk.
//   RandomLegalPolicy - emits legal micro actions via the GameLogic RNG stream only.

#pragma once

#include "GameLogic/Neural/NeuralTypes.h"

class Player;

// ---------------------------------------------------------------------------
// Abstract decision maker. act() is called once per decimated decision tick.
class NeuralPolicy
{
public:
	virtual ~NeuralPolicy() {}

	///< Decide actions for one tick. May append 0..AI_MAX_ACTIONS items to sink.
	virtual void act( const AIObservation& obs, Player* self, AIActionSink& sink ) = 0;

	///< Debug / log name.
	virtual const char* getName() const = 0;
};

// ---------------------------------------------------------------------------
// Does nothing. Used as the P0 default so the mere presence of NeuralAIPlayer cannot change
// the simulation (zero-regression guarantee for Gate A).
class PassThroughPolicy : public NeuralPolicy
{
public:
	virtual void act( const AIObservation& /*obs*/, Player* /*self*/, AIActionSink& /*sink*/ ) override
	{
		// Intentionally produces no actions.
	}
	virtual const char* getName() const override { return "PassThrough"; }
};

// ---------------------------------------------------------------------------
// Emits a small number of legal low-level actions per tick using ONLY the GameLogic random
// stream (GameLogicRandomValue), so replays and lockstep CRC stay intact. Targets are taken
// from the observation itself (unit slots / fog-visible enemy slots), which guarantees the
// referenced positions exist and are legally observable by this player.
class RandomLegalPolicy : public NeuralPolicy
{
public:
	RandomLegalPolicy() {}
	virtual void act( const AIObservation& obs, Player* self, AIActionSink& sink ) override;
	virtual const char* getName() const override { return "RandomLegal"; }
};

// ---------------------------------------------------------------------------
// Factory for the placeholder policy used in NeuralAIPlayer's constructor. Reads the optional
// GENERALX_NEURAL_POLICY environment variable ("random" -> RandomLegalPolicy, anything else or
// unset -> PassThroughPolicy). The env read happens once at player construction (a setup-time
// input, like local INI overrides), never inside the simulation step itself.
// Caller owns the returned instance.
extern NeuralPolicy* createDefaultNeuralPolicy();
