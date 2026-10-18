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
//																			 		//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																			 		//
////////////////////////////////////////////////////////////////////////////////

// GeneralsX @feature Storm996 16/10/2026 PS3.0 stage 4B: static building lamp light sources.
// FILE: LampList.cpp ///////////////////////////////////////////////////////////////////////////////
// Desc:   LampList / LampSet / LampStore INI parsing, patterned on FXList.
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "GameClient/LampList.h"

#include "Common/INI.h"
#include "Common/OptionPreferences.h"	// GeneralsX @feature PS3.0 4F 17/10/2026 lazy seed of the lamp gain slider

// GeneralsX @feature Qoder 18/10/2026 getenv()/atoi() for the live lamp gate, fprintf()/fflush() for
// its one-shot seed evidence line.
#include <cstdio>
#include <cstdlib>

// DEFINES ////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
LampDef::LampDef()
{
	m_position.x = m_position.y = m_position.z = 0.0f;
	m_color.red = m_color.green = m_color.blue = 1.0f;
	m_radius = 30.0f;
	m_intensity = 0.5f;
	m_flickerPeriodFrames = 0;
	m_flickerAmp = 0.0f;
}

// PRIVATE ////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
/**
	Parse one nested "Lamp ... End" block into a LampDef and append it to
	the owning LampSet (instance). Mirrors LightPulseFXNugget::parse in FXList.cpp.
*/
//-------------------------------------------------------------------------------------------------
static void parseLampEntry(INI *ini, void *instance, void* /*store*/, const void* /*userData*/)
{
	static const FieldParse myFieldParse[] =
	{
		{ "Position",							INI::parseCoord3D,									nullptr, offsetof( LampDef, m_position ) },
		{ "Color",								INI::parseRGBColor,								nullptr, offsetof( LampDef, m_color ) },
		{ "Radius",							INI::parseReal,											nullptr, offsetof( LampDef, m_radius ) },
		{ "Intensity",						INI::parseReal,											nullptr, offsetof( LampDef, m_intensity ) },
		{ "FlickerPeriodFrames",	INI::parseDurationUnsignedInt,		nullptr, offsetof( LampDef, m_flickerPeriodFrames ) },
		{ "FlickerAmp",						INI::parseReal,											nullptr, offsetof( LampDef, m_flickerAmp ) },
		{ nullptr, nullptr, nullptr, 0 }
	};

	LampSet* lampSet = (LampSet*)instance;
	LampDef def;
	ini->initFromINI(&def, myFieldParse);
	lampSet->addLamp(def);
}

//-------------------------------------------------------------------------------------------------
/**
	FieldParse table for the body of a LampList block: repeatable Lamp entries.
*/
//-------------------------------------------------------------------------------------------------
static const FieldParse TheLampSetFieldParse[] =
{
	{ "Lamp",					parseLampEntry,						nullptr, 0			},
	{ nullptr, nullptr, nullptr, 0 }
};

// PUBLIC /////////////////////////////////////////////////////////////////////////////////////////

//-------------------------------------------------------------------------------------------------
LampStore::LampStore()
{
	m_intensityGain = -1.0f;	// GeneralsX @feature PS3.0 4F 17/10/2026 sentinel: seed lazily from the options store
	m_lampsEnabled = -1;		// GeneralsX @feature Qoder 18/10/2026 sentinel for the on/off half
}

//-------------------------------------------------------------------------------------------------
LampStore::~LampStore()
{
	m_lampmap.clear();
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature PS3.0 4F 17/10/2026: current lamp brightness gain (1.0 = the original 4B look).
// GENERALSX_LAMP_INT (float) always wins so QA can A/B without touching Options.ini; otherwise the
// gain is seeded lazily from the options store (Options.ini LampLightIntensity ticks) on the first
// lamp frame and live-updated by the Graphics Options slider apply hook afterwards.
//-------------------------------------------------------------------------------------------------
Real LampStore::getGlobalIntensityGain() const
{
	const char* env = getenv("GENERALSX_LAMP_INT");
	if (env != nullptr && *env != 0)
	{
		Real gain = (Real)atof(env);
		if (gain < 0.0f) gain = 0.0f;
		if (gain > 2.0f) gain = 2.0f;
		return gain;
	}

	if (m_intensityGain < 0.0f)
		m_intensityGain = (Real)OptionPreferences().getLampLightIntensity() * 0.01f;

	return m_intensityGain;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 18/10/2026 PS3.0 4E: the live on/off gate for the whole lamp channel
// (see LampList.h). GENERALSX_LAMPLIGHT resolves once per process into a static - the env block cannot
// change while the game runs, and a QA leg that sets it must not be overridable from the page.
// Without an env the gate seeds from Options.ini LampLightEffect on the first lamp frame and the
// Graphics Options row re-arms it from then on.
//-------------------------------------------------------------------------------------------------
static Int LampEnvState()
{
	static Int state = 0;		///< 0 = not looked up, 1 = no env, 2 = env OFF, 3 = env ON
	if (state == 0)
	{
		const char* env = getenv("GENERALSX_LAMPLIGHT");
		if (env == nullptr || *env == 0)
			state = 1;
		else
			state = (atoi(env) != 0) ? 3 : 2;
	}
	return state;
}

void LampStore::setGlobalLampsEnabled( Bool enabled )
{
	if (LampEnvState() != 1)
		return;							// the env leg owns the channel while it is set
	m_lampsEnabled = (enabled != FALSE) ? 1 : 0;
}

Bool LampStore::getGlobalLampsEnabled() const
{
	const Int envState = LampEnvState();
	if (envState == 2)
		return FALSE;
	if (envState == 3)
		return TRUE;

	if (m_lampsEnabled < 0)
	{
		m_lampsEnabled = (OptionPreferences().getLampLightEnabled() != FALSE) ? 1 : 0;
		fprintf(stderr, "[LAMP] gate seeded enabled=%d (Options.ini LampLightEffect)\n", m_lampsEnabled);
		fflush(stderr);
	}
	return m_lampsEnabled != 0;
}

//-------------------------------------------------------------------------------------------------
const LampSet *LampStore::findLampSet(const char* name) const
{
	if (!name || stricmp(name, "None") == 0)
		return nullptr;

	LampSetMap::const_iterator it = m_lampmap.find(NAMEKEY(name));
	if (it != m_lampmap.end())
	{
		return &(*it).second;
	}
	return nullptr;
}

//-------------------------------------------------------------------------------------------------
/*static */ void LampStore::parseLampListDefinition(INI *ini)
{
	// read the LampList name (a ThingTemplate object name)
	const char *c = ini->getNextToken();
	NameKeyType key = TheNameKeyGenerator->nameToKey(c);
	LampSet& ls = TheLampStore->m_lampmap[key];
	ls.clear();
	ini->initFromINI(&ls, TheLampSetFieldParse);
}

//-------------------------------------------------------------------------------------------------
/*static*/ void INI::parseLampListDefinition(INI *ini)
{
	LampStore::parseLampListDefinition(ini);
}

// EXTERNALS //////////////////////////////////////////////////////////////////////////////////////
LampStore *TheLampStore = nullptr;
