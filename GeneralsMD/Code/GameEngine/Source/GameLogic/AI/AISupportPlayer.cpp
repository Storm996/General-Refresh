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
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// AISupportPlayer.cpp
// GeneralsX @feature ai-upgrade 07/10/2026 Support skirmish AI personality.

#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/GameMemory.h"
#include "Common/Player.h"
#include "GameLogic/AI.h"
#include "GameLogic/AISupportPlayer.h"

//-------------------------------------------------------------------------------------------------
AISupportPlayer::AISupportPlayer( Player *p ) :
AISkirmishPlayer( p )
{
	// GeneralsX @feature ai-upgrade 07/10/2026 Support grows economy/tech and fields combined
	// forces at a slightly relaxed cadence (~1.5x the designer-configured team delay).
	// Integer math only, to stay bit-identical across x86_64 / ARM64 (determinism red line).
	Int baseDelay = TheAI->getAiData()->m_teamSeconds;
	Int supportDelay = baseDelay + baseDelay / 2;
	if (supportDelay < baseDelay)		// guard against overflow on absurd INI values
		supportDelay = baseDelay;
	setTeamDelaySeconds( supportDelay );
}

//-------------------------------------------------------------------------------------------------
AISupportPlayer::~AISupportPlayer()
{
	// Base AISkirmishPlayer destructor already disbands teams and releases queued work orders.
}
