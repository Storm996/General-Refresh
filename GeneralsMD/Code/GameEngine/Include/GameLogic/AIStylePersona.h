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

// AIStylePersona.h
// GeneralsX @feature ai-upgrade 07/10/2026 Shared, data-driven team classifier for the
// skirmish AI personality styles. Header-only on purpose so the original AIPlayer /
// AISkirmishPlayer stay untouched; each style subclass calls aiStylePersonaAcceptsTeam()
// from its own isAGoodIdeaToBuildTeam() override.
//
// The filter is intentionally a *rejection* of teams the persona disfavours, layered on top
// of the base gate. selectTeamToBuild() still gathers base-approved candidates and picks the
// highest m_productionPriority one with a single GameLogicRandomValue() draw, so overriding
// only this predicate biases *which* teams survive without touching the RNG stream -> the
// outcome stays deterministic across x86_64 / ARM64 as long as peers run the same data/styles.
// All arithmetic below is integer-only (determinism red line); no floating point, no native
// transcendental calls. Classification uses generic attributes (KindOf + calcCostToBuild +
// team template flags), so it auto-generalises to mod units that carry the same standard data.

#pragma once

#include <functional>

#include "Common/GameCommon.h"		// AIStyleType
#include "Common/KindOf.h"			// KINDOF_INFANTRY / VEHICLE / AIRCRAFT
#include "Common/ThingTemplate.h"	// ThingTemplate::isKindOf / calcCostToBuild
#include "Common/ThingFactory.h"		// TheThingFactory->findTemplate
#include "Common/Team.h"					// TeamPrototype / TeamTemplateInfo / TCreateUnitsInfo
#include "Common/Player.h"					// Player::getPlayerTeams / PlayerTeamList

class Player;

// GeneralsX @feature ai-upgrade 07/10/2026 Aggregate profile of a candidate team.
struct AIStyleTeamProfile
{
	Int infantry;			// weighted count of KINDOF_INFANTRY members
	Int vehicle;			// weighted count of KINDOF_VEHICLE members
	Int aircraft;			// weighted count of KINDOF_AIRCRAFT members
	Int totalCount;		// total weighted member count (min+max per entry)
	Int totalCost;		// sum of cost * weight (avg cost = totalCost / totalCount)
	Int maxUnitCost;	// single most expensive member's cost
	Bool isDefense;		// base / perimeter defense team (never rejected)
	Bool avoidThreats;// team authored to sneak around threats
	Bool parsed;			// false if the prototype could not be inspected

	AIStyleTeamProfile() :
		infantry(0), vehicle(0), aircraft(0), totalCount(0), totalCost(0),
		maxUnitCost(0), isDefense(0), avoidThreats(0), parsed(0) {}
};

// GeneralsX @feature ai-upgrade 07/10/2026 Per-unit average-cost ceilings (gold). Tuned to the
// retail ZH cost curve: cheap foot ~200-500, light vehicles ~600-900, heavy armor/tech ~1000+.
enum
{
	AI_STYLE_RUSH_CHEAP_CEIL  = 600,	// Rush keeps only genuinely cheap foot swarms
	AI_STYLE_HORDE_HEAVY_CEIL = 1000,	// Horde allows cheap+light, rejects heavy tech blobs
	AI_STYLE_SNEAK_HEAVY_CEIL = 1000	// Sneak avoids the same heavy armored blobs
};

// GeneralsX @feature ai-upgrade 07/10/2026 Build a profile from a prototype's member templates.
inline AIStyleTeamProfile aiStyleClassifyTeam( const TeamPrototype *proto, const Player *player )
{
	AIStyleTeamProfile p;
	if (!proto || !player)
		return p;

	const TeamTemplateInfo *info = proto->getTemplateInfo();
	if (!info)
		return p;

	p.parsed			= 1;
	p.isDefense		= (info->m_isBaseDefense || info->m_isPerimeterDefense) ? 1 : 0;
	p.avoidThreats = info->m_avoidThreats ? 1 : 0;

	Int num = info->m_numUnitsInfo;
	if (num > TeamTemplateInfo::MAX_UNIT_TYPES)
		num = TeamTemplateInfo::MAX_UNIT_TYPES;

	for (Int i = 0; i < num; ++i)
	{
		const TCreateUnitsInfo &u = info->m_unitsInfo[i];
		const ThingTemplate *t = TheThingFactory->findTemplate( u.unitThingName );
		if (!t)
			continue;

		Int weight = u.minUnits + u.maxUnits;
		if (weight <= 0)
			continue;

		Int cost = t->calcCostToBuild( player );
		if (t->isKindOf( KINDOF_INFANTRY ))
			p.infantry += weight;
		if (t->isKindOf( KINDOF_VEHICLE ))
			p.vehicle += weight;
		if (t->isKindOf( KINDOF_AIRCRAFT ))
			p.aircraft += weight;
		p.totalCount += weight;
		p.totalCost += cost * weight;
		if (cost > p.maxUnitCost)
			p.maxUnitCost = cost;
	}

	return p;
}

// GeneralsX @feature ai-upgrade 07/10/2026 The persona gate. Returns true if the given style is
// happy to field this (already base-approved) team. Styles with no composition agenda (Offense,
// Defense, Support, Fortress) accept everything and remain cadence-only for now.
inline Bool aiStylePersonaAcceptsTeam( AIStyleType style, const TeamPrototype *proto, const Player *player )
{
	// Cadence-only personalities: never filter composition.
	if (style != AI_STYLE_RUSH && style != AI_STYLE_HORDE &&
			style != AI_STYLE_BLITZ && style != AI_STYLE_SNEAK)
		return true;

	AIStyleTeamProfile p = aiStyleClassifyTeam( proto, player );
	if (!p.parsed)
		return true;

	// Base / perimeter defense is sacred for every persona (protects the economy).
	if (p.isDefense)
		return true;

	// Nothing measurable (e.g. an all-structure or empty attack list) -> do not reject blindly.
	if (p.totalCount <= 0)
		return true;

	switch (style)
	{
		case AI_STYLE_RUSH:
			// Mad-dog: only the cheapest foot-soldier swarms; skip heavy / armor tech.
			if (p.totalCost > AI_STYLE_RUSH_CHEAP_CEIL * p.totalCount)
				return false;
			return true;

		case AI_STYLE_HORDE:
			// Human wave: cheap + light mixed; reject only truly heavy tech blobs.
			if (p.totalCost > AI_STYLE_HORDE_HEAVY_CEIL * p.totalCount)
				return false;
			return true;

		case AI_STYLE_BLITZ:
			// Lightning armor: refuse pure foot teams, push mechanized / armored spearheads.
			if (p.vehicle == 0 && p.aircraft == 0 && p.infantry > 0)
				return false;
			return true;

		case AI_STYLE_SNEAK:
			// Sneak: hit-and-run light forces; refuse heavy armored blobs.
			if (p.totalCost > AI_STYLE_SNEAK_HEAVY_CEIL * p.totalCount)
				return false;
			return true;

		default:
			return true;
	}
}

// GeneralsX @feature ai-upgrade 07/10/2026 Fallback guard. Returns true if the player currently
// has at least one *persona-preferred* team that also clears the base gate. Each style's
// isAGoodIdeaToBuildTeam() uses this so that rejecting a disfavoured team NEVER starves the AI:
// if nothing preferred is buildable right now (factories busy / instance caps hit), the AI falls
// back to building the disfavoured team instead of idling. baseOk is supplied by the caller as a
// lambda that invokes the (protected) base-class gate, so this helper needs no access to it.
inline Bool aiStyleHasPreferredAlternative(
	AIStyleType style,
	const Player *player,
	const std::function<Bool(TeamPrototype *)> &baseOk )
{
	if (!player)
		return false;

	const Player::PlayerTeamList *teams = player->getPlayerTeams();
	if (!teams)
		return false;

	for (Player::PlayerTeamList::const_iterator it = teams->begin(); it != teams->end(); ++it)
	{
		TeamPrototype *cand = *it;
		if (!cand)
			continue;
		if (!baseOk(cand))
			continue;
		if (aiStylePersonaAcceptsTeam(style, cand, player))
			return true;
	}
	return false;
}
