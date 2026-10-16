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

// FILE: WeatherFX.h /////////////////////////////////////////////////////////
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1/W4: the render-side half of the weather
// system.
//
// Split of responsibility, and why:
//  - The *kind* of weather is logic state (GlobalData::m_weather, set by map data or a script action).
//    It is synchronized on every peer and recorded in replays, so it may drive gameplay appearance.
//  - Everything in this file is derived from that kind inside the device layer and is deliberately
//    NOT logic: the dimming curve, the lightning schedule, the flash envelope. Thunder in particular
//    uses a private RNG seeded per run, so two peers see strikes at different instants - which is fine
//    because nothing here is ever read by GameLogic or written into the CRC state stream
//    (AGENTS.md Golden Rule 7, and the same "zero logic consumption" discipline PS3.0 4C used for the
//    TOD override debug leg).
//
// Consumers:
//  - W3DDisplay (light refill) reads the ambient/key multipliers.
//  - W3DSnow (world particles) reads the tint plus the flash lift, so rain sheets breathe with gunfire.
//  - The screen filter reads the tint/darken/wisp/vignette scalars and the flash envelope.
#pragma once

#include "Common/GameType.h"
#include "Lib/RGBColor.h"

struct WeatherRenderState
{
	Weather m_type;					///<the type this frame's numbers were built from.
	Bool m_particlesOn;			///<active recipe wants world particles drawn.

	// Scene-light coupling (multipliers on the existing PS3.0 L1c/L1k knobs; 1.0 = "weather stays out of it").
	Real m_ambientScale;
	Real m_keyScale;
	Real m_fogScale;

	// Material coupling.
	Real m_wetness;					///<0..1 roughness offset for PBR materials (rain-slick ground).

	// Screen-layer coupling.
	RGBColor m_screenTint;
	Real m_screenDarken;
	Real m_screenWispDensity;
	Real m_screenVignette;

	// Transient lightning envelope, 0..1: peaks on the strike frame and decays over the afterglow.
	// Fed to the screen filter (white flash) and to the particle tint (the sheet lighting up).
	Real m_flash;

	WeatherRenderState()
	{
		m_type = WEATHER_NORMAL;
		m_particlesOn = FALSE;
		m_ambientScale = 1.0f;
		m_keyScale = 1.0f;
		m_fogScale = 1.0f;
		m_wetness = 0.0f;
		m_screenTint.red = 1.0f;
		m_screenTint.green = 1.0f;
		m_screenTint.blue = 1.0f;
		m_screenDarken = 1.0f;
		m_screenWispDensity = 0.0f;
		m_screenVignette = 0.0f;
		m_flash = 0.0f;
	}
};

class WeatherFX
{
public:
	/// Called once per client update from SnowManager::update() (the render side already ticks there).
	void update();

	/// Reset the schedule/flash between games so a fresh map never inherits the last one's lightning.
	void reset();

	const WeatherRenderState &getState() const { return m_state; }

	/// Lightning gate for QA: GENERALSX_WEATHER_THUNDER=0 keeps the rain but silences the strikes.
	static Bool thunderEnabled();

private:
	WeatherRenderState m_state;
	Real m_nextStrikeSeconds;			///<countdown to the next strike of the current type.
	Real m_flashRemaining;				///<seconds left of the flash core (drawn at full strength).
	Real m_flashTotal;					///<length of the current flash, for the decay curve.
	Real m_gunshotAfterglow;			///<unused today; kept for the muzzle-light lift follow-up.

	void rebuildState();
	void scheduleLightning(Real secondsDelta);
};

extern WeatherFX *TheWeatherFX;

/// Cheap accessor for render code: never null inside a live device frame, and returns a neutral state
/// on the shell/preview paths so nothing has to special-case "no weather yet".
inline const WeatherRenderState &weatherRenderState()
{
	static const WeatherRenderState s_neutral;
	return TheWeatherFX != nullptr ? TheWeatherFX->getState() : s_neutral;
}
