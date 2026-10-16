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
}

//-------------------------------------------------------------------------------------------------
LampStore::~LampStore()
{
	m_lampmap.clear();
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
