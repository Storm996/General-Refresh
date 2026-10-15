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

// NeuralAIPlayer.h
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 host class. Derives from
// AISkirmishPlayer so the macro game (build/economy/difficulty) keeps running exactly as the
// heuristic baseline does; on top of that, every decimated tick it runs the learned-agent loop:
//   observe (fog-scoped)  ->  policy.act()  ->  execute (CMD_FROM_AI channel).
// P0 mounts a placeholder policy only (see NeuralPolicy.h); a real network arrives as another
// NeuralPolicy implementation in P1 without touching this class.

#pragma once

#include "Common/GameMemory.h"
#include "GameLogic/AISkirmishPlayer.h"

#include "GameLogic/Neural/NeuralTypes.h"
#include "GameLogic/Neural/AIObservationProvider.h"
#include "GameLogic/Neural/AIActionExecutor.h"

#include <vector>

class NeuralPolicy;

/**
 * Skirmish AI slot that hosts a pluggable (eventually learned) decision policy.
 */
class NeuralAIPlayer : public AISkirmishPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( NeuralAIPlayer, "NeuralAIPlayer" )

public:

	NeuralAIPlayer( Player *p );			///< constructor

	virtual AIStyleType getAIStyle() const override { return AI_STYLE_NEURAL; }

	// GeneralsX @feature ai-upgrade 14/10/2026 Forensic driver name for [AIPLAYER]/[AITEAM] lines.
	virtual const char *getDriverName() const override { return "NeuralAI"; }

	///< Swap in a different policy (takes ownership; used by P1 to mount an OnnxPolicy).
	void setPolicy( NeuralPolicy* policy );

protected:

	// Note: the MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE macro above already declares
	// "protected: virtual ~NeuralAIPlayer()"; the definition in the cpp releases m_policy.

	virtual void update() override;			///< base macro AI + decimated neural decision loop

private:

	// Decision cadence: one observation/action cycle every N logic frames. Decimation keeps the
	// action space and (future) inference cost sane and matches typical RTS macro-action rates.
	enum { AI_DECIMATION_FRAMES = 30 };

	NeuralPolicy*				m_policy;			///< owned; placeholder in P0, learned net in P1
	AIObservationProvider	m_obsProvider;	///< stateless fog-scoped encoder
	AIActionExecutor			m_executor;			///< stateless command injector

	// Scratch reused per tick (avoids allocations inside the update loop).
	AIObservation					m_obs;
	AIActionSink					m_sink;
	std::vector<Object*>		m_ownRef;			// slot -> own objects, parallel to m_obs.own[]
	std::vector<Object*>		m_enemyRef;		// slot -> enemy objects, parallel to m_obs.enemy[]

	Bool							m_trace;				///< setup-time env gate for [NEURAL] log lines (no sim impact)
};
