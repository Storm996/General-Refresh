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
// FILE: LampList.h ///////////////////////////////////////////////////////////////////////////////
// Desc:   INI-defined per-object static lamp lights (building facade lamps, etc.)
//         Mirrors the FXListStore pattern: name-keyed store, loaded from
//         Data\INI\Default\LampList + Data\INI\LampList via initSubsystem().
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "Common/GameMemory.h"
#include "Common/NameKeyGenerator.h"
#include "Common/STLTypedefs.h"
#include "Common/SubsystemInterface.h"
#include "Lib/BaseType.h"				// GeneralsX: Real / Coord3D / RGBColor for LampDef

// FORWARD REFERENCES /////////////////////////////////////////////////////////////////////////////
class INI;

//-------------------------------------------------------------------------------------------------
/**
	One lamp: a position in the drawable's local (model) coordinate frame, plus
	its color / radius / intensity / flicker parameters. Consumed render-side only
	by W3DModelDraw's lamp driver; never touches game logic (determinism rule 7).
*/
struct LampDef
{

	LampDef();

	Coord3D			m_position;					///< local-space position relative to the model pivot
	RGBColor		m_color;						///< lamp tint
	Real				m_radius;						///< far attenuation radius (world units)
	Real				m_intensity;					///< 0..1; keep <=0.6 so explosion pulses keep priority slots
	UnsignedInt	m_flickerPeriodFrames;	///< 0 = steady light
	Real				m_flickerAmp;					///< 0..1 fraction of intensity modulated by flicker

};

//-------------------------------------------------------------------------------------------------
/**
	The set of lamps attached to one object type (ThingTemplate name).
*/
class LampSet
{

public:

	LampSet() { }
	virtual ~LampSet() { m_lamps.clear(); }

	void clear() { m_lamps.clear(); }
	void addLamp( const LampDef& def ) { m_lamps.push_back( def ); }

	Int getNumLamps() const { return (Int)m_lamps.size(); }
	const LampDef* getLamp( Int idx ) const
	{
		if (idx < 0 || idx >= (Int)m_lamps.size())
			return nullptr;
		return &m_lamps[idx];
	}

private:

	// GeneralsX @feature Storm996 16/10/2026 no per-instance copy issues: store owns these by value.
	typedef std::vector< LampDef > LampList;
	LampList m_lamps;

};

//-------------------------------------------------------------------------------------------------
/**
	The "store" used to hold all the LampSets in existence.
*/
class LampStore : public SubsystemInterface
{

public:

	LampStore();
	virtual ~LampStore() override;

	virtual void init() override { }
	virtual void reset() override { }
	virtual void update() override { }

	/**
		return the LampSet with the given namekey, or nullptr if none exists.
	*/
	const LampSet *findLampSet( const char* name ) const;

	// GeneralsX @feature PS3.0 4F 17/10/2026: global lamp brightness gain for the Graphics Options
	// slider (Options.ini LampLightIntensity, ticks 0-200, 100 = 1.0x; GENERALSX_LAMP_INT float wins
	// inside the lazy seed). The render-side consumer (W3DModelDraw::updateLamps) multiplies every
	// per-lamp INI intensity by this, so the user can dial the 4B look in-game and keep it.
	void setGlobalIntensityGain( Real gain ) { m_intensityGain = gain; }
	Real getGlobalIntensityGain() const;		///< lazy: seeds from OptionPreferences on first use

	// GeneralsX @feature Qoder 18/10/2026 PS3.0 4E: the on/off half of the 4B lamp channel is now live
	// too. It used to be a `static const Bool` latch inside W3DModelDraw::updateLamps, so the page's
	// 建筑灯光 row only spoke at start-up. The store owns the value because it is the one piece both
	// sides can reach (the options page is engine-layer, the lamps device-layer); GENERALSX_LAMPLIGHT
	// still wins outright, so a scripted A/B leg is never moved by the menu. Turning it off makes
	// updateLamps release every held lamp, i.e. the lights go out on the next frame.
	void setGlobalLampsEnabled( Bool enabled );
	Bool getGlobalLampsEnabled() const;		///< lazy: seeds from OptionPreferences on first use

	static void parseLampListDefinition(INI* ini);

private:

	// use the hashing function for Ints, same as FXListStore.
	typedef std::hash_map< NameKeyType, LampSet, rts::hash<NameKeyType>, rts::equal_to<NameKeyType>/**/> LampSetMap;

	LampSetMap m_lampmap;
	mutable Real m_intensityGain;				///< -1 = not yet seeded from the options store
	mutable Int m_lampsEnabled;					///< GeneralsX @feature Qoder 18/10/2026 -1 = not yet seeded

};

// EXTERNALS //////////////////////////////////////////////////////////////////////////////////////
extern LampStore *TheLampStore;
