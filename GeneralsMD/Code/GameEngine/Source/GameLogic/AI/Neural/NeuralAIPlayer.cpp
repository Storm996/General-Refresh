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

// NeuralAIPlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 Neural-AI P0 host class implementation.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"	// engine convention: TeamPrototype / SpecialPowerTemplate decls precede the AI headers
#include "GameLogic/AI.h"
#include "GameLogic/Neural/NeuralAIPlayer.h"

#include <stdio.h>
#include <stdlib.h>			// getenv (setup-time trace gate only, never inside decisions)

#include "GameLogic/GameLogic.h"				// TheGameLogic::getFrame()
#include "GameLogic/Neural/NeuralPolicy.h"

//-------------------------------------------------------------------------------------------------
NeuralAIPlayer::NeuralAIPlayer( Player *p ) :
	AISkirmishPlayer( p ),
	m_policy( NULL ),
	m_trace( false )
{
	// P0 placeholder policy; a real (learned) network mounts via setPolicy() in P1.
	m_policy = createDefaultNeuralPolicy();

	// Observation-loop trace for Gate A/B verification. Read once at construction: the value
	// never feeds a decision, it only enables stderr logging, so it cannot break determinism.
	m_trace = getenv( "GENERALX_NEURAL_TRACE" ) != NULL;
}

//-------------------------------------------------------------------------------------------------
NeuralAIPlayer::~NeuralAIPlayer()
{
	// The base AISkirmishPlayer destructor keeps handling teams/work orders; we only own the
	// policy object, which is a plain (non-pooled) helper.
	if ( m_policy )
	{
		delete m_policy;
		m_policy = NULL;
	}
}

//-------------------------------------------------------------------------------------------------
void NeuralAIPlayer::setPolicy( NeuralPolicy* policy )
{
	if ( m_policy && m_policy != policy )
		delete m_policy;
	m_policy = policy;
}

//-------------------------------------------------------------------------------------------------
// The learned-agent loop, layered on the unchanged heuristic macro game:
//   1. AISkirmishPlayer::update()  - base build/economy/team logic runs exactly as the default AI.
//   2. every AI_DECIMATION_FRAMES: observe -> policy -> execute (+ optional training dump).
void NeuralAIPlayer::update()
{
	AISkirmishPlayer::update();

	if ( !m_policy )
		return;

	const UnsignedInt frame = TheGameLogic->getFrame();
	if ( frame % AI_DECIMATION_FRAMES != 0 )
		return;

	// --- observe: strictly fog-scoped, player-view features -------------------------------
	m_obsProvider.buildObservation( m_player, m_obs, m_ownRef, m_enemyRef );

	// --- decide: pure obs -> actions, no engine access inside the policy ------------------
	m_sink.reset();
	m_policy->act( m_obs, m_player, m_sink );

	// --- act: revalidated slot addressing on the regular CMD_FROM_AI channel --------------
	const Int applied = m_executor.execute( m_sink, m_obs, m_player, m_ownRef, m_enemyRef );

#ifdef AI_TRAINING_DUMP
	// Dev-only sidecar (NOT part of the shipped simulation path, never reaches release):
	// append the raw fixed-schema records so batch headless runs can produce training data.
	// AIObservation / AIActionSink are pointer-free PODs by schema contract.
	static FILE* s_dump = NULL;
	if ( !s_dump )
		s_dump = fopen( "neural_dump.bin", "ab" );
	if ( s_dump )
	{
		UnsignedInt marker = AI_OBS_SCHEMA_VERSION;
		fwrite( &marker, sizeof(marker), 1, s_dump );
		fwrite( &m_obs, sizeof(m_obs), 1, s_dump );
		fwrite( &m_sink, sizeof(m_sink), 1, s_dump );
		fflush( s_dump );
	}
#endif

	if ( m_trace )
	{
		// Gate evidence line; logging only, zero simulation side effects.
		// applied < actions means the decoder's commands were dropped by slot revalidation
		// (Gate C signal); applied == actions with legal moves proves the injection channel works.
		fprintf( stderr, "[NEURAL] frame=%u policy=%s own=%d enemy=%d scalars=%d actions=%d applied=%d\n",
			frame, m_policy->getName(), m_obs.ownCount, m_obs.enemyCount, m_obs.scalarCount, m_sink.count, applied );
	}
}
