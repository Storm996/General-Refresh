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

// Snow.cpp ////////////////////////////////////////////////////////////////////////////////
// Snow Rendering implementation
// Author: Mark Wilczynski, July 2003
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"	// This must go first in EVERY cpp file in the Game
#include "GameClient/Snow.h"
#include "GameClient/View.h"
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: the render layer observes the logic-side
// weather value here (one-way read; it is never written back, so replay/CRC state is untouched).
#include "Common/GlobalData.h"
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: the type switch refills the display's
// lighting slots (setTimeOfDay) so the weather->lighting coupler re-runs the moment the weather flips.
#include "GameClient/Display.h"
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1/W4: render-side weather state owner.
#include "GameClient/WeatherFX.h"
// GeneralsX @feature Qoder 21/10/2026: weather preset + vegetation knobs persist in Options.ini.
#include "Common/OptionPreferences.h"


SnowManager *TheSnowManager=nullptr;

// GeneralsX @feature Qoder 21/10/2026: default vegetation recipe. The master switch ships ON so the
// user can evaluate the layer immediately, and turning it off skips the entire channel (scan, instance
// build and draw). Values outside the documented ranges are clamped by refreshVegetationRenderOptions.
VegetationRenderOptions g_vegetationRenderOptions =
{
	TRUE,		// enabled
	TRUE,		// grassEnabled
	TRUE,		// rocksEnabled
	55.0f,		// grassDensity (%)
	100.0f,		// grassHeightScale (%)
	100.0f,		// grassDistanceScale (%)
	100.0f		// rockDensity (%)
};

void refreshVegetationRenderOptions()
{
	OptionPreferences prefs;
	g_vegetationRenderOptions.enabled = prefs.getInt("VegetationEnabled", 1) != 0;
	g_vegetationRenderOptions.grassEnabled = prefs.getInt("GrassEnabled", 1) != 0;
	g_vegetationRenderOptions.rocksEnabled = prefs.getInt("RocksEnabled", 1) != 0;
	g_vegetationRenderOptions.grassDensity = (Real)prefs.getInt("GrassDensity", 55);
	g_vegetationRenderOptions.grassHeightScale = (Real)prefs.getInt("GrassHeight", 100);
	g_vegetationRenderOptions.grassDistanceScale = (Real)prefs.getInt("GrassDistance", 100);
	g_vegetationRenderOptions.rockDensity = (Real)prefs.getInt("RockDensity", 100);

	if (g_vegetationRenderOptions.grassDensity < 0.0f) g_vegetationRenderOptions.grassDensity = 0.0f;
	if (g_vegetationRenderOptions.grassDensity > 100.0f) g_vegetationRenderOptions.grassDensity = 100.0f;
	if (g_vegetationRenderOptions.grassHeightScale < 50.0f) g_vegetationRenderOptions.grassHeightScale = 50.0f;
	if (g_vegetationRenderOptions.grassHeightScale > 200.0f) g_vegetationRenderOptions.grassHeightScale = 200.0f;
	if (g_vegetationRenderOptions.grassDistanceScale < 50.0f) g_vegetationRenderOptions.grassDistanceScale = 50.0f;
	if (g_vegetationRenderOptions.grassDistanceScale > 200.0f) g_vegetationRenderOptions.grassDistanceScale = 200.0f;
	if (g_vegetationRenderOptions.rockDensity < 0.0f) g_vegetationRenderOptions.rockDensity = 0.0f;
	if (g_vegetationRenderOptions.rockDensity > 200.0f) g_vegetationRenderOptions.rockDensity = 200.0f;
}

// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: one profile per weather type. Index is the
// Weather enum value. Slot WEATHER_SNOWY is the retail "Weather" INI block (and every override of it);
// the other slots only exist once a "WeatherType" block authors them, or once applyTypeDefaults hands a
// map a tuned recipe for a type that shipped with no data at all.
static WeatherSetting *s_weatherProfiles[WEATHER_COUNT] = { nullptr };
static Weather s_activeWeatherType = WEATHER_NORMAL;

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: authored baseline for each new type.
// These are the "looks right on a generic map" numbers - a WeatherType block overrides any field.
// Note the particle count this generates: emitters = (BoxDimensions * BoxDensity)^2, and the
// per-particle height noise table holds SNOW_NOISE_X * SNOW_NOISE_Y entries, so a type that wants a
// denser box must stay inside that budget or the pattern repeats visibly (128x128 = 16384).
//-------------------------------------------------------------------------------------------------
void WeatherSetting::applyTypeDefaults(Weather type)
{
	m_weatherType = type;
	switch (type)
	{
		case WEATHER_RAINY:
			m_particleKind = PARTICLE_RAIN;
			m_weatherEnabled = TRUE;
			m_snowEnabled = TRUE;				//render-side gate is shared with the retail recipe.
			// GeneralsX @bugfix PS3.0 Weather W3 16/10/2026: this used to name EXRainStreak.tga, which
			// no ZH archive contains - WW3D answers a missing texture with its magenta debug surface, so
			// the whole sheet drew purple. EXRainDrop is the real ZH rain sprite (32x32, a soft-edged
			// vertical streak that fades in at the top and is brightest at the bottom tip).
			m_snowTexture = "EXRainDrop.tga";
			m_snowBoxDimensions = 110.0f;
			m_snowBoxDensity = 0.85f;
			m_snowVelocity = 42.0f;
			m_snowAmplitude = 0.6f;
			m_snowFrequencyScaleX = 0.02f;
			m_snowFrequencyScaleY = 0.02f;
			m_streakLength = 3.2f;
			// The sprite's bar is drawn across the quad by a narrow UV band, so this is now the on-screen
			// ribbon's real width in world units: 0.022 is a couple of pixels at the usual RTS zoom.
			m_streakThickness = 0.022f;
			// The sprite peaks at alpha 141/255, so opacity is the multiplier on top of that shape.
			m_opacity = 0.85f;
			m_falloffStart = 180.0f;
			m_falloffEnd = 420.0f;
			m_windX = 2.0f;
			m_windY = 1.0f;
			m_usePointSprites = FALSE;
			m_minShade = 0.55f;
			m_particleTint.red = 0.78f; m_particleTint.green = 0.85f; m_particleTint.blue = 1.0f;
			m_ambientScale = 0.85f;
			m_keyScale = 0.75f;
			m_fogScale = 1.35f;
			m_wetness = 0.45f;
			m_screenTint.red = 0.86f; m_screenTint.green = 0.90f; m_screenTint.blue = 0.98f;
			m_screenDarken = 0.88f;
			m_screenWispDensity = 0.13f;
			m_screenVignette = 0.12f;
			m_lightningChance = 0.0f;
			break;

		case WEATHER_STORM:
			m_particleKind = PARTICLE_RAIN;
			m_weatherEnabled = TRUE;
			m_snowEnabled = TRUE;
			m_snowTexture = "EXRainDrop.tga";
			m_snowBoxDimensions = 110.0f;
			m_snowBoxDensity = 1.25f;
			m_snowVelocity = 60.0f;
			m_snowAmplitude = 1.2f;
			m_swayScale = 2.2f;
			m_streakLength = 5.5f;
			m_streakThickness = 0.03f;
			m_opacity = 0.95f;
			m_falloffStart = 200.0f;
			m_falloffEnd = 460.0f;
			m_windX = 7.0f;
			m_windY = 4.0f;
			m_usePointSprites = FALSE;
			m_minShade = 0.50f;
			m_particleTint.red = 0.72f; m_particleTint.green = 0.78f; m_particleTint.blue = 0.95f;
			m_ambientScale = 0.68f;
			m_keyScale = 0.55f;
			m_fogScale = 1.7f;
			m_wetness = 0.65f;
			m_screenTint.red = 0.76f; m_screenTint.green = 0.80f; m_screenTint.blue = 0.92f;
			m_screenDarken = 0.74f;
			m_screenWispDensity = 0.20f;
			m_screenVignette = 0.26f;
			m_lightningChance = 0.0f;
			break;

		case WEATHER_THUNDER:
			applyTypeDefaults(WEATHER_STORM);
			m_weatherType = WEATHER_THUNDER;
			m_ambientScale = 0.58f;
			m_keyScale = 0.48f;
			m_screenDarken = 0.66f;
			m_screenWispDensity = 0.22f;
			m_lightningChance = 6.0f;			///<expected strikes/minute - device-side RNG only.
			m_lightningIntensity = 1.0f;
			break;

		case WEATHER_BLIZZARD:
			m_particleKind = PARTICLE_SNOW;
			m_weatherEnabled = TRUE;
			m_snowEnabled = TRUE;
			m_snowTexture = "EXSnowFlake.tga";
			m_snowBoxDimensions = 150.0f;
			m_snowBoxDensity = 0.62f;
			m_snowVelocity = 26.0f;
			m_snowAmplitude = 12.0f;
			m_swayScale = 2.6f;
			m_snowPointSize = 1.3f;
			m_snowMaxPointSize = 48.0f;
			m_usePointSprites = TRUE;
			m_windX = 16.0f;
			m_windY = 5.0f;
			m_opacity = 0.95f;
			m_falloffStart = 120.0f;
			m_falloffEnd = 330.0f;
			m_minShade = 0.60f;
			m_particleTint.red = 0.92f; m_particleTint.green = 0.95f; m_particleTint.blue = 1.0f;
			m_ambientScale = 0.80f;
			m_keyScale = 0.70f;
			m_fogScale = 2.4f;
			m_wetness = 0.0f;
			m_screenTint.red = 0.93f; m_screenTint.green = 0.96f; m_screenTint.blue = 1.0f;
			m_screenDarken = 0.92f;
			m_screenWispDensity = 0.24f;
			m_screenVignette = 0.35f;
			m_lightningChance = 0.0f;
			break;

		case WEATHER_SNOWY:
		{
			// GeneralsX @tweak PS3.0 Weather W1/W2: the retail snow recipe gains the new layer's identity
			// values here. Kept deliberately mild: falling flakes already carry the effect, so the screen
			// half only adds the overcast lift a snowy day has and no whiteout.
			m_weatherType = WEATHER_SNOWY;
			m_particleKind = PARTICLE_SNOW;
			m_minShade = 0.45f;					///<matches the literal floor the PS3.0-P3-26 tint used.
			m_ambientScale = 0.95f;
			m_keyScale = 0.92f;
			m_fogScale = 1.2f;
			m_wetness = 0.0f;						///<snow is not rain-slick; nothing to gain from a roughness offset.
			m_screenTint.red = 0.97f; m_screenTint.green = 0.99f; m_screenTint.blue = 1.0f;
			m_screenDarken = 0.97f;
			m_screenWispDensity = 0.18f;
			m_screenVignette = 0.08f;
			m_lightningChance = 0.0f;
			break;
		}

		case WEATHER_NORMAL:
		default:
			m_weatherType = WEATHER_NORMAL;
			m_weatherEnabled = FALSE;
			m_snowEnabled = FALSE;
			// GeneralsX @feature PS3.0 Weather W1/W2: state every coupling explicitly. This profile is
			// built by copying the snowy base (see getWeatherProfile), so leaving a scalar out would
			// inherit snow's numbers and a clear map would render weather-tinted - the one thing the
			// retail-parity default has to guarantee.
			m_ambientScale = 1.0f;
			m_keyScale = 1.0f;
			m_fogScale = 1.0f;
			m_wetness = 0.0f;
			m_screenTint.red = 1.0f; m_screenTint.green = 1.0f; m_screenTint.blue = 1.0f;
			m_screenDarken = 1.0f;
			m_screenWispDensity = 0.0f;
			m_screenVignette = 0.0f;
			m_lightningChance = 0.0f;
			m_lightningIntensity = 1.0f;
			break;
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: profile access + type switching.
// Every type lazily materializes its own base object, so render code can dereference a profile
// unconditionally. Recursion is avoided by never routing through this function while building one.
//-------------------------------------------------------------------------------------------------
WeatherSetting *getWeatherProfile(Weather type)
{
	const Int index = (type >= WEATHER_NORMAL && type < WEATHER_COUNT) ? (Int)type : (Int)WEATHER_NORMAL;
	if (s_weatherProfiles[index] == nullptr)
	{
		WeatherSetting *fresh = newInstance(WeatherSetting);
		WeatherSetting *snowBase = s_weatherProfiles[WEATHER_SNOWY];
		if (snowBase != nullptr)
		{
			// Inherit the system-level recipe (texture, grass toggles, point-sprite preference),
			// then let the type's own baseline decide what actually renders.
			*fresh = *snowBase;
		}
		fresh->applyTypeDefaults((Weather)index);
		s_weatherProfiles[index] = fresh;
	}
	return s_weatherProfiles[index];
}

Weather getActiveWeatherType()
{
	return s_activeWeatherType;
}

void applyWeatherType(Weather type)
{
	const Int index = (type >= WEATHER_NORMAL && type < WEATHER_COUNT) ? (Int)type : (Int)WEATHER_NORMAL;
	WeatherSetting *profile = getWeatherProfile((Weather)index);

	// The profile table always owns the base object of each chain (that is what a destructor frees);
	// TheWeatherSetting is the "active" view, i.e. the last override a map authored for that profile.
	// friend_getFinalOverride() walks to the end of the chain and returns the base when there is none.
	TheWeatherSetting = (WeatherSetting *)profile->friend_getFinalOverride();
	s_activeWeatherType = (Weather)index;

	// NOTE: deliberately does NOT call SnowManager::setWeatherType() - that function calls back into
	// this one, so a two-way link here would recurse forever. The manager is the driver (its update()
	// mirrors the logic value and its setWeatherType() publishes the recipe + refills the lighting);
	// this half only resolves the data.
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: render-layer gates. Both read the
// environment once and never touch logic state, so they are QA legs, not gameplay state:
//   GENERALSX_WEATHER=0        - kills the whole weather render layer (retail parity A/B leg).
//   GENERALSX_WEATHER_TYPE=n   - forces type n for this run (0..5); unset = follow the map/script.
//-------------------------------------------------------------------------------------------------
Bool isWeatherRenderEnabled()
{
	static const Bool s_on = [] {
		const char *ev = getenv("GENERALSX_WEATHER");
		return ev == nullptr || atoi(ev) != 0;
	}();
	return s_on;
}

Weather readMapWeatherOverride()
{
	static const Int s_cached = [] {
		const char *ev = getenv("GENERALSX_WEATHER_TYPE");
		return ev != nullptr && *ev != 0 ? atoi(ev) : -1;
	}();
	if (s_cached < 0)
	{
		return WEATHER_NORMAL;
	}
	return Weather_ClampToKnown(s_cached);
}

Bool hasWeatherTypeOverride()
{
	static const Bool s_set = getenv("GENERALSX_WEATHER_TYPE") != nullptr;
	return s_set;
}

SnowManager::SnowManager()
{
	m_time = 0;
	m_velocity = 1;
	m_isVisible = TRUE;	//default to showing if it's enabled via INI.
	m_weatherType = WEATHER_SNOWY;	//GeneralsX @feature PS3.0 Weather W1: retail only ever had snow.
	m_userWeatherType = -1;			//GeneralsX @feature Qoder 21/10/2026: follow the map until the player picks one.
}

void SnowManager::init()
{
	//starting heights of each particle
	//TODO: replace this lookup table with some kind of procedural method that takes x,y as input.
	m_startingHeights=NEW Real [ SNOW_NOISE_X * SNOW_NOISE_Y];
	m_time = 0;

	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: the retail build only ever had one
	// recipe, so "active type" used to be implicit. Materialize the snowy profile and point the
	// active view at it before anything reads TheWeatherSetting.
	applyWeatherType(WEATHER_SNOWY);
	updateIniSettings();

	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: create the render-side weather state
	// after the recipe exists, and reset it with every game the client starts (TheWeatherManager is
	// created next to TheSnowManager in GameClient::init, so this is the one shared hook).
	if (TheWeatherFX == nullptr)
	{
		TheWeatherFX = NEW WeatherFX;
	}
	TheWeatherFX->reset();

	// GeneralsX @feature Qoder 21/10/2026: seed the user preset and the vegetation knobs from Options.ini
	// once per client init, so the graphics page shows the same values the renderer will consume.
	loadWeatherUserOverride();
	refreshVegetationRenderOptions();
}

// GeneralsX @feature Qoder 21/10/2026: the options page forces a weather type for preview and the
// manager honours it until the player goes back to "follow map". Env GENERALSX_WEATHER_TYPE still wins
// in update(), so scripted A/B runs keep their authority.
void SnowManager::setUserWeatherOverride(Int type)
{
	if (type >= (Int)WEATHER_COUNT)
		type = (Int)WEATHER_COUNT - 1;
	if (type < -1)
		type = -1;
	m_userWeatherType = type;
	if (type >= 0)
	{
		m_isVisible = TRUE;
		// Publish immediately: setWeatherType() refreshes the recipe, particles, screen filter and
		// lighting on this same frame, which is what makes the combo feel like a live switch.
		setWeatherType(Weather_ClampToKnown(type));
	}
}

void SnowManager::loadWeatherUserOverride()
{
	Int saved = -1;
	{
		OptionPreferences prefs;
		saved = prefs.getInt("WeatherPreview", -1);
	}
	if (saved < -1 || saved >= (Int)WEATHER_COUNT)
		saved = -1;
	m_userWeatherType = saved;
}

void SnowManager::setWeatherType(Weather type)
{
	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: render-side mirror of the logic-side
	// weather value. Re-resolves the recipe (a map may have overridden that type's block), then
	// reloads the particle parameters so the swap is visible from the next frame.
	const Weather previous = m_weatherType;
	m_weatherType = type;
	applyWeatherType(type);
	if (type != previous)
	{
		m_time = 0;	//start the new type's fall cycle from the top of its box.
	}
	updateIniSettings();
	if (type != previous)
	{
		// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: publish the new scalars and re-run
		// the lighting install, so a live script switch (or a map whose saved weather differs from the
		// recipe loaded at init) repaints the world on the SAME frame instead of waiting for the next
		// time-of-day change. W3DDisplay::setTimeOfDay rewrites every slot light from the raw INI
		// values before applying the weather factors, so repeating it is idempotent, and it also marks
		// the static terrain lighting dirty (BaseHeightMap::staticLightingChanged) which is what
		// recolors the ground with the new coupler factors.
		if (TheWeatherFX != nullptr)
		{
			TheWeatherFX->refreshStateNow();
		}
		if (TheDisplay != nullptr && TheGlobalData != nullptr)
		{
			TheDisplay->setTimeOfDay(TheGlobalData->m_timeOfDay);
		}
		if (TheTacticalView != nullptr)
		{
			TheTacticalView->forceRedraw();
		}
	}
}

void SnowManager::update()
{
	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: mirror the logic-side weather value into
	// the render recipe once per client frame. Reading GlobalData here is one-way (render observes
	// logic, never writes it), so the value stays out of the CRC stream and replays are unaffected.
	// The compare is a single enum test when nothing changed, which is the common case.
	const Weather forced = readMapWeatherOverride();
	const Weather wanted = hasWeatherTypeOverride()
		? forced
		: (m_userWeatherType >= 0 && m_userWeatherType < (Int)WEATHER_COUNT)
			? (Weather)m_userWeatherType
			: TheGlobalData != nullptr ? TheGlobalData->m_weather : WEATHER_NORMAL;

	if (wanted != m_weatherType)
	{
		setWeatherType(Weather_ClampToKnown((Int)wanted));
	}

	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1/W4: the render-side scalars (dimming,
	// wetness, screen tint, lightning envelope) are derived here so every consumer - the light refill,
	// the particle pass and the screen filter - reads one coherent set of numbers per frame.
	if (TheWeatherFX != nullptr)
	{
		TheWeatherFX->update();
	}
}

void SnowManager::updateIniSettings()
{
	// GeneralsX @bugfix Storm996 16/10/2026 PS3.0 Weather W1: a WeatherType block can legally arrive
	// before the manager exists, and BaseHeightMap device-loss paths call in around resets.
	if (TheWeatherSetting == nullptr)
	{
		applyWeatherType(WEATHER_SNOWY);
	}
	if (m_startingHeights == nullptr)
	{
		return;
	}

	Real *dst=m_startingHeights;
	//initialize a table of random starting positions for each particle.
	Int boxDimensions = (Int)TheWeatherSetting->m_snowBoxDimensions;
	if (boxDimensions < 1)
	{
		boxDimensions = 1;	//modulo-by-zero guard for an authored SnowBoxDimensions of 0.
	}
	for (Int y=0; y<SNOW_NOISE_Y; y++)
	{
		for (Int x=0; x<SNOW_NOISE_X; x++)
		{
			*dst=(Real)(rand()%(boxDimensions));
			dst++;
		}
	}

	m_velocity = TheWeatherSetting->m_snowVelocity;
	m_frequencyScaleX = TheWeatherSetting->m_snowFrequencyScaleX;
	m_frequencyScaleY = TheWeatherSetting->m_snowFrequencyScaleY;
	m_amplitude	= TheWeatherSetting->m_snowAmplitude * TheWeatherSetting->m_swayScale;
	m_pointSize = TheWeatherSetting->m_snowPointSize;
	m_quadSize	= TheWeatherSetting->m_snowQuadSize;
	m_boxDimensions	= TheWeatherSetting->m_snowBoxDimensions;
	// GeneralsX @bugfix Storm996 16/10/2026: guard the divides. A 0 velocity/density in an authored
	// block used to produce Inf here, and Inf timing on ARM64 vs x86 is a documented desync source
	// (AGENTS.md Cross-Platform Determinism 2) as soon as anything downstream casts it.
	m_emitterSpacing = TheWeatherSetting->m_snowBoxDensity > 0.0001f
		? 1.0f/TheWeatherSetting->m_snowBoxDensity : 1.0f;
	m_maxPointSize = TheWeatherSetting->m_snowMaxPointSize;
	m_minPointSize = TheWeatherSetting->m_snowMinPointSize;

	//Time for snow flake to make it from top to bottom of rendered cube around camera.
	m_fullTimePeriod = m_velocity > 0.0001f ? m_boxDimensions/m_velocity : m_boxDimensions;
}

void SnowManager::setVisible(Bool showWeather)
{
	m_isVisible = showWeather;
}

void SnowManager::reset()
{
	m_isVisible = TRUE;	//default to showing if it's enabled via INI.
	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W4: a new game must not inherit the last
	// one's lightning schedule or a half-faded screen flash.
	if (TheWeatherFX != nullptr)
	{
		TheWeatherFX->reset();
	}
}

SnowManager::~SnowManager()
{
	delete [] m_startingHeights;
	m_startingHeights=nullptr;

	// TheSuperHackers @fix Mauller 13/04/2025 Delete the instance of the weather settings
	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: that instance is now the snowy base of
	// a table with one chain per weather type, so free every base (deleteInstance recurses into the
	// override chain) and clear the active view before any of them dangles.
	for (Int i=0; i<WEATHER_COUNT; ++i)
	{
		if (s_weatherProfiles[i] != nullptr)
		{
			deleteInstance(s_weatherProfiles[i]);
			s_weatherProfiles[i] = nullptr;
		}
	}
	TheWeatherSetting=nullptr;
	s_activeWeatherType = WEATHER_NORMAL;

	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: the render state is owned here because
	// its lifetime is exactly "a client with weather to draw"; GameClient deletes TheSnowManager last.
	if (TheWeatherFX != nullptr)
	{
		delete TheWeatherFX;
		TheWeatherFX = nullptr;
	}
}

OVERRIDE<WeatherSetting> TheWeatherSetting = nullptr;

// PRIVATE DATA ///////////////////////////////////////////////////////////////////////////////////
const FieldParse WeatherSetting::m_weatherSettingFieldParseTable[] =
{
	{ "SnowTexture",							INI::parseAsciiString,nullptr,			offsetof( WeatherSetting, m_snowTexture ) },
	{ "SnowFrequencyScaleX",					INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowFrequencyScaleX ) },
	{ "SnowFrequencyScaleY",					INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowFrequencyScaleY ) },
	{ "SnowAmplitude",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowAmplitude ) },
	{ "SnowPointSize",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowPointSize ) },
	{ "SnowMaxPointSize",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowMaxPointSize ) },
	{ "SnowMinPointSize",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowMinPointSize ) },
	{ "SnowQuadSize",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowQuadSize ) },
	{ "SnowBoxDimensions",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowBoxDimensions ) },
	{ "SnowBoxDensity",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowBoxDensity ) },
	{ "SnowVelocity",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_snowVelocity ) },
	{ "SnowPointSprites",						INI::parseBool,nullptr,			offsetof( WeatherSetting, m_usePointSprites ) },
	{ "SnowEnabled",							INI::parseBool,nullptr,			offsetof( WeatherSetting, m_snowEnabled ) },
	// GeneralsX @feature Qoder 11/10/2026 Grass layer toggles (same weather INI block).
	{ "GrassEnabled",							INI::parseBool,nullptr,			offsetof( WeatherSetting, m_grassEnabled ) },
	{ "GrassDensity",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_grassDensity ) },
	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: the world/screen/light knobs that turn
	// the single retail snow block into a per-type recipe. Defaults keep the shipped snow look.
	// m_weatherType is deliberately NOT parseable: the type comes from the block that declared it
	// ("Weather" = snowy, "WeatherType NAME" = NAME), so a map can never retarget a block by accident.
	{ "WeatherEnabled",							INI::parseBool,nullptr,			offsetof( WeatherSetting, m_weatherEnabled ) },
	{ "ParticleKind",								INI::parseIndexList,	WeatherParticleKindNames,	offsetof( WeatherSetting, m_particleKind ) },
	{ "WindX",										INI::parseReal,nullptr,			offsetof( WeatherSetting, m_windX ) },
	{ "WindY",										INI::parseReal,nullptr,			offsetof( WeatherSetting, m_windY ) },
	{ "StreakLength",								INI::parseReal,nullptr,			offsetof( WeatherSetting, m_streakLength ) },
	{ "StreakThickness",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_streakThickness ) },
	{ "ParticleOpacity",							INI::parseReal,nullptr,			offsetof( WeatherSetting, m_opacity ) },
	{ "FalloffStart",								INI::parseReal,nullptr,			offsetof( WeatherSetting, m_falloffStart ) },
	{ "FalloffEnd",									INI::parseReal,nullptr,			offsetof( WeatherSetting, m_falloffEnd ) },
	{ "SwayScale",									INI::parseReal,nullptr,			offsetof( WeatherSetting, m_swayScale ) },
	{ "MinSceneShade",								INI::parseReal,nullptr,			offsetof( WeatherSetting, m_minShade ) },
	{ "ParticleTint",								INI::parseRGBColor,nullptr,		offsetof( WeatherSetting, m_particleTint ) },
	{ "AmbientScale",								INI::parseReal,nullptr,			offsetof( WeatherSetting, m_ambientScale ) },
	{ "KeyScale",										INI::parseReal,nullptr,			offsetof( WeatherSetting, m_keyScale ) },
	{ "FogScale",										INI::parseReal,nullptr,			offsetof( WeatherSetting, m_fogScale ) },
	{ "Wetness",										INI::parseReal,nullptr,			offsetof( WeatherSetting, m_wetness ) },
	{ "ScreenTint",									INI::parseRGBColor,nullptr,		offsetof( WeatherSetting, m_screenTint ) },
	{ "ScreenDarken",								INI::parseReal,nullptr,			offsetof( WeatherSetting, m_screenDarken ) },
	{ "ScreenWispDensity",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_screenWispDensity ) },
	{ "ScreenVignette",								INI::parseReal,nullptr,			offsetof( WeatherSetting, m_screenVignette ) },
	{ "LightningStrikesPerMinute",			INI::parseReal,nullptr,			offsetof( WeatherSetting, m_lightningChance ) },
	{ "LightningIntensity",						INI::parseReal,nullptr,			offsetof( WeatherSetting, m_lightningIntensity ) },
	{ nullptr, nullptr, nullptr, 0 },
};

// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: names for the ParticleKind enum, in the
// order the enum declares (INI::parseIndexList resolves by position).
const char *const WeatherParticleKindNames[] =
{
	"SNOW",
	"RAIN",

	nullptr
};

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: shared block plumbing for the two INI
// entries. The retail "Weather" block and a "WeatherType NAME = SNOWY" block both live as one
// Overridable chain per weather type: system defaults create the base, a map's override file appends
// an override (preserving the retail copy-then-parse semantics exactly), and switching types is just
// repointing TheWeatherSetting - no re-parse, no reallocation.
//-------------------------------------------------------------------------------------------------
static WeatherSetting *openWeatherProfileForParse(INI *ini, Weather type)
{
	const Int index = (Int)type;
	WeatherSetting *base = s_weatherProfiles[index];

	if (base == nullptr)
	{
		base = s_weatherProfiles[index] = newInstance(WeatherSetting);
		base->applyTypeDefaults(type);
	}
	else if (ini->getLoadType() == INI_LOAD_CREATE_OVERRIDES)
	{
		WeatherSetting *wsOverride = newInstance(WeatherSetting);
		*wsOverride = *base;
		wsOverride->markAsOverride();
		base->friend_getFinalOverride()->setNextOverride(wsOverride);
		base = wsOverride;
	}
	else
	{
		// A second plain "Weather"/"WeatherType" block in the system pass is a data error, same as retail.
		throw INI_INVALID_DATA;
	}

	return base;
}

//-------------------------------------------------------------------------------------------------
void INI::parseWeatherDefinition( INI *ini )
{
	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: this is the retail block, so it always
	// targets the snowy profile; per-type authoring lives in "WeatherType NAME".
	WeatherSetting *target = openWeatherProfileForParse(ini, WEATHER_SNOWY);

	// parse the data
	ini->initFromINI( target, target->getFieldParse() );

	if (TheSnowManager != nullptr)
	{
		if (s_activeWeatherType == WEATHER_SNOWY)
		{
			// Re-resolve the active view: a map override only shows up at the end of the chain.
			TheSnowManager->setWeatherType(WEATHER_SNOWY);
		}
		else
		{
			TheSnowManager->updateIniSettings();
		}
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1:
//   WeatherType NAME = RAINY     ; any WeatherNames value
//     ...same keys as the Weather block...
//   End
// Lets a mod author tune rain/blizzard without touching the shipped snow recipe.
//-------------------------------------------------------------------------------------------------
void INI::parseWeatherTypeDefinition( INI *ini )
{
	// "WeatherType = RAINY" - resolved through the same name table the GameData "Weather" key uses,
	// so an authored block and a map's GameData line can never disagree about what a name means.
	Weather type = (Weather)INI::scanIndexList(ini->getNextToken(), WeatherNames);
	if (type < WEATHER_NORMAL || type >= WEATHER_COUNT)
	{
		throw INI_INVALID_DATA;
	}

	WeatherSetting *target = openWeatherProfileForParse(ini, type);
	target->m_weatherType = type;
	if (ini->getLoadType() != INI_LOAD_CREATE_OVERRIDES)
	{
		// A freshly authored type is live by default unless the block says otherwise.
		target->m_weatherEnabled = TRUE;
		target->m_snowEnabled = TRUE;
	}

	ini->initFromINI( target, target->getFieldParse() );

	if (s_activeWeatherType == type && TheSnowManager != nullptr)
	{
		TheSnowManager->setWeatherType(type);
	}
	else if (TheSnowManager)
	{
		TheSnowManager->updateIniSettings();
	}
}
