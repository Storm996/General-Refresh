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
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
*/

// AICampaignPlayer.h
// GeneralsX @feature campaign-ai 22/10/2026
// Campaign-specific AI: keeps the hand-placed campaign base/build list and gives the
// ordinary AIPlayer an autonomous commander tick, so built teams do not need a
// per-mission teamOnCreateScript to receive an attack order.

#pragma once

#include "GameLogic/AIPlayer.h"

class AICampaignPlayer : public AIPlayer
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( AICampaignPlayer, "AICampaignPlayer" )

public:
	AICampaignPlayer( Player *p );
	virtual const char *getDriverName() const override { return "AICampaignPlayer"; }

	virtual void update() override;

protected:
	/// Issue attack-move orders to idle AI-built combat teams.
	void commanderTick();
	/// Find the human player that this AI is at war with.
	Player *findHumanEnemy();
	/// Pick the enemy command center, or the nearest enemy structure.
	Bool findEnemyTarget( Player *enemy, Coord3D *target );
};
