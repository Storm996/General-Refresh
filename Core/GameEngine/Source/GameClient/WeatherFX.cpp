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

// FILE: WeatherFX.cpp ///////////////////////////////////////////////////
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1/W4: render-side weather state.
// See WeatherFX.h for the logic/render split and the determinism argument.

#include "PreRTS.h"
#include "GameClient/WeatherFX.h"
#include "GameClient/Snow.h"
#include "Common/GlobalData.h"
#include "GameClient/Display.h"
#include "GameClient/View.h"
#include "WW3D2/ww3d.h"

WeatherFX *TheWeatherFX = nullptr;

// GeneralsX @feature PS3.0 Weather W4: a private, deliberately non-reproducible RNG. It decides when a
// bolt falls and where it lands - both cosmetic only. Using the engine's GameClientRandomVariable here
// would push weather into the network/replay random stream and desync peers over a purely visual event.
static UnsignedInt s_strikeSeed = 0;

/// Strikes handed to the light transport since load. File scope so the once-a-second [WXSTAT] trace can
/// report it next to the client frame rate, which is what makes a bolt's real duration readable.
static UnsignedInt s_boltCount = 0;

static UnsignedInt strikeRandomBits()
{
	if (s_strikeSeed == 0)
	{
		// GetTickCount() is already used unguarded elsewhere in this codebase (ReplaySimulation.cpp),
		// so the platform layer provides it on every target this file builds for.
		s_strikeSeed = (UnsignedInt)GetTickCount() ^ 0x9e3779b9u ^ (UnsignedInt)WW3D::Get_Frame_Count();
		if (s_strikeSeed == 0)
		{
			s_strikeSeed = 0x1234abcdu;
		}
	}
	// xorshift32: tiny, no allocations, no global state outside this file.
	s_strikeSeed ^= s_strikeSeed << 13;
	s_strikeSeed ^= s_strikeSeed >> 17;
	s_strikeSeed ^= s_strikeSeed << 5;
	return s_strikeSeed;
}

/// Uniform in [0,1).
static Real strikeRandomReal()
{
	return (Real)(strikeRandomBits() & 0xffffffu) * (1.0f / 16777216.0f);
}

Bool WeatherFX::thunderEnabled()
{
	static const Bool s_on = [] {
		const char *ev = getenv("GENERALSX_WEATHER_THUNDER");
		return ev == nullptr || atoi(ev) != 0;
	}();
	return s_on;
}

void WeatherFX::reset()
{
	m_nextStrikeSeconds = 0.0f;
	m_flashRemaining = 0.0f;
	m_flashTotal = 0.0f;
	rebuildState();
}

void WeatherFX::update()
{
	// Real (sync) frame delta, clamped: Get_Sync_Frame_Time() reports whole milliseconds and jumps to a
	// huge value across a load or an alt-tab, which would fire every pending bolt at once.
	Real dt = (Real)WW3D::Get_Sync_Frame_Time() * 0.001f;
	if (dt < 0.0f || dt > 0.25f)
	{
		dt = 1.0f / 30.0f;
	}
	rebuildState();
	scheduleLightning(dt);
	traceClientRate();
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature PS3.0 Weather W6 16/10/2026: once-a-second client-rate trace, printed only while a
// weather type is actually active so the retail-parity leg keeps a clean log.
// Why this exists: the bolt's dynamic light is armed in FRAMES (the retail LightPulse transport counts
// increase/decay in On_Frame_Update calls), while the flash envelope is armed in SECONDS. On a scene
// that cannot hold 30 fps those two halves of one bolt therefore disagree about how long a strike lasts
// - the light can burn for seconds of wall time while the screen flash is long gone. A capture that
// shows "half the frames are white" is exactly that symptom, and without the frame rate next to the
// strike count there is no way to tell a leaking light from a slow client.
//-------------------------------------------------------------------------------------------------
void WeatherFX::traceClientRate()
{
	if (m_state.m_type == WEATHER_NORMAL)
	{
		return;
	}

	static UnsignedInt s_frames = 0;
	static UnsignedInt s_windowStart = 0;
	++s_frames;

	const UnsignedInt now = (UnsignedInt)GetTickCount();
	if (s_windowStart == 0)
	{
		s_windowStart = now;
		return;
	}
	const UnsignedInt elapsed = now - s_windowStart;
	if (elapsed < 1000u)
	{
		return;
	}

	fprintf(stderr, "[WXSTAT] type=%d fps=%.1f flash=%.2f bolts=%u\n",
		(Int)m_state.m_type, (Real)s_frames * 1000.0f / (Real)elapsed, m_state.m_flash, s_boltCount);
	fflush(stderr);

	s_frames = 0;
	s_windowStart = now;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature PS3.0 Weather W1: fold the active recipe into the scalars every other render
// module reads. This is pure arithmetic on already-synced state, so it costs one call per client
// update and cannot diverge between peers (each peer computes the same numbers from its own weather).
//-------------------------------------------------------------------------------------------------
void WeatherFX::rebuildState()
{
	m_state.m_type = getActiveWeatherType();

	if (TheWeatherSetting != nullptr)
	{
		m_state.m_particlesOn = TheWeatherSetting->wantsParticles() &&
			(TheSnowManager == nullptr || TheSnowManager->isWeatherVisible());
		m_state.m_ambientScale = TheWeatherSetting->m_ambientScale;
		m_state.m_keyScale = TheWeatherSetting->m_keyScale;
		m_state.m_fogScale = TheWeatherSetting->m_fogScale;
		m_state.m_wetness = TheWeatherSetting->m_wetness;
		m_state.m_screenTint = TheWeatherSetting->m_screenTint;
		m_state.m_screenDarken = TheWeatherSetting->m_screenDarken;
		m_state.m_screenWispDensity = TheWeatherSetting->m_screenWispDensity;
		m_state.m_screenVignette = TheWeatherSetting->m_screenVignette;
	}
	else
	{
		m_state = WeatherRenderState();
	}

	// Lightning gate: only a type that authored a strike schedule can flash, and the QA env leg can
	// silence it without touching the rain itself.
	if (TheWeatherSetting == nullptr || TheWeatherSetting->m_lightningChance <= 0.0f || !thunderEnabled()
		|| !isWeatherRenderEnabled())
	{
		m_state.m_flash = 0.0f;
		m_nextStrikeSeconds = 0.0f;
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature PS3.0 Weather W4: the bolt. Two halves, both existing machinery:
//   1. a real dynamic light (TheDisplay->createLightPulse = the PS3.0 4A transport that explosions
//      already use), so the ground, buildings and terrain light up for a few frames;
//   2. a screen flash envelope that the weather filter adds over the frame - the "retina-burn" part
//      a dynamic light can never produce because lights don't affect the sky or the 2D scene.
// The flash envelope is also read by W3DSnow's batch tint, so the falling sheet brightens with it.
//-------------------------------------------------------------------------------------------------
void WeatherFX::scheduleLightning(Real dt)
{
	// Decay the envelope first: a strike raised it last frame and the shape must keep collapsing even
	// if the schedule is temporarily off (e.g. the map flipped to clear weather mid-afterglow).
	if (m_flashRemaining > 0.0f)
	{
		m_flashRemaining -= dt;
		if (m_flashRemaining < 0.0f)
		{
			m_flashRemaining = 0.0f;
		}
	}

	// Envelope shape. GeneralsX @bugfix PS3.0 Weather W4 16/10/2026: this used to compare the time
	// REMAINING against the core length, so the "brief pop" branch was really the whole first 0.38 s of
	// the window and the fade was the last 0.07 s - i.e. the bolt burned at full strength for six times
	// longer than it flashed. Measured against the time ELAPSED since the strike instead: a short core at
	// full brightness (the retina-burn pop), then a visible afterglow that collapses to nothing.
	const Real remaining = m_flashRemaining > 0.0f ? m_flashRemaining : 0.0f;
	const Real total = m_flashTotal > 0.0f ? m_flashTotal : 1.0f;
	const Real core = 0.066f;
	const Real elapsed = total - remaining;
	Real envelope = 0.0f;
	if (remaining > 0.0f)
	{
		if (elapsed < core)
		{
			envelope = 1.0f;
		}
		else
		{
			const Real tail = total - core > 0.0f ? total - core : total;
			envelope = (total - elapsed) / tail;
			if (envelope < 0.0f) envelope = 0.0f;
			else if (envelope > 1.0f) envelope = 1.0f;
		}
	}

	const Real scheduleChance = TheWeatherSetting != nullptr ? TheWeatherSetting->m_lightningChance : 0.0f;
	if (scheduleChance <= 0.0f || !thunderEnabled() || !isWeatherRenderEnabled())
	{
		m_state.m_flash = envelope;
		return;
	}

	const Real meanSeconds = 60.0f / (scheduleChance > 0.01f ? scheduleChance : 0.01f);

	if (m_nextStrikeSeconds <= 0.0f)
	{
		// Redraw the gap every strike rather than ticking a fixed timer, so bolts never march in
		// lockstep. A uniform 0.4..1.6x the mean is enough to read as random weather without needing
		// a second distribution here.
		m_nextStrikeSeconds = meanSeconds * (0.4f + 1.2f * strikeRandomReal());
	}
	m_nextStrikeSeconds -= dt;

	if (m_nextStrikeSeconds > 0.0f)
	{
		m_state.m_flash = envelope;
		return;
	}

	// ---- strike ----
	m_nextStrikeSeconds = 0.0f;
	m_flashTotal = 0.45f;			//~2 frames of core brightness (the white pop) plus a visible tail.
	m_flashRemaining = m_flashTotal;

	const Real intensity = TheWeatherSetting->m_lightningIntensity > 0.0f
		? TheWeatherSetting->m_lightningIntensity : 1.0f;
	m_state.m_flash = intensity;

	if (TheDisplay == nullptr || TheTacticalView == nullptr)
	{
		return;
	}

	// Land the bolt somewhere in front of the camera, a few hundred world units out, so the dynamic
	// light actually rakes the terrain the player is looking at instead of the sky behind them.
	const Coord3D camPos = TheTacticalView->get3DCameraPosition();
	Coord3D strike = camPos;
	strike.x += (strikeRandomReal() - 0.5f) * 500.0f;
	strike.y += (strikeRandomReal() - 0.5f) * 500.0f;
	strike.z -= 300.0f;				//roughly the RTS eye height: put the bolt head near the ground plane.

	RGBColor boltColor;
	// GeneralsX @bugfix PS3.0 Weather W4 16/10/2026: this was (0.85, 0.90, 1.0) - a second sun. DX8 sums
	// the dynamic light on top of the key/ambient fill, so anywhere inside the bolt's 600-unit reach the
	// pixel landed at roughly twice its lit value and clipped: the capture showed whole frames of white
	// with the battlefield still only 8% visible underneath. It is also frame-paced, not time-paced (the
	// retail LightPulse transport counts increase/decay in On_Frame_Update calls), so on a heavy weather
	// scene that clips to 10-15 fps one bolt can burn for seconds of wall time - which turns a 0.2 s
	// design intent into a strobe the player cannot dismiss. The pop belongs to the screen flash, which
	// IS time-paced; this light only has to rake the ground, so it is authored as a quarter-sun lift.
	boltColor.red = 0.22f * intensity;
	boltColor.green = 0.25f * intensity;
	boltColor.blue = 0.32f * intensity;

	// innerRadius 1 + attenuation width is the retail FXList LightPulse convention (see Core/FXList.cpp),
	// and the total has to clear W3DDisplay's "too small to matter" threshold (~41 world units) by a wide
	// margin, which a bolt does at 600.
	const Real innerRadius = 1.0f;
	const Real attenuationWidth = 600.0f * intensity;
	const UnsignedInt increaseFrames = 2;
	const UnsignedInt decayFrames = 14;

	TheDisplay->createLightPulse(&strike, &boltColor, innerRadius, attenuationWidth,
		increaseFrames, decayFrames);

	// GeneralsX @tweak PS3.0 Weather W4 16/10/2026: this used to print one line per 32 strikes, which hid
	// the strike schedule - a capture could not tell "one bolt that would not fade" apart from "seven
	// bolts on a ten second clock". Strikes are authored ~6/minute, so one line each stays bounded.
	++s_boltCount;
	fprintf(stderr, "[WXTHUNDER] type=%d strike=(%.0f,%.0f,%.0f) radius=%.0f intensity=%.2f meanGap=%.2fs n=%u\n",
		(Int)m_state.m_type, strike.x, strike.y, strike.z, innerRadius + attenuationWidth,
		intensity, meanSeconds, s_boltCount);
	fflush(stderr);
}
