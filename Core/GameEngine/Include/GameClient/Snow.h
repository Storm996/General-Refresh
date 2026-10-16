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

// FILE: Snow.h /////////////////////////////////////////////////////////

#pragma once

#include "Common/SubsystemInterface.h"
#include "Common/Overridable.h"
#include "Common/Override.h"
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: per-type tints live in the weather profile.
#include "Lib/RGBColor.h"
#include "WWMath/vector3.h"
#include "WWMath/vector4.h"

//-------------------------------------------------------------------------------------------------
/** This structure keeps the transparency and vertex settings, which are the same regardless of the
		time of day. They can be overridden on a per-map basis. */
//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: INI name table for the particle primitive
// kinds in WeatherSetting (declared here so the parse table can reference it).
extern const char *const WeatherParticleKindNames[];

// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: render-layer gates. Both read the
// environment once per process and never write logic state, so they are QA legs rather than gameplay:
//   GENERALSX_WEATHER=0        - kills the whole weather render layer (retail parity A/B leg).
//   GENERALSX_WEATHER_TYPE=n   - forces type n for this run (0..5); unset = follow the map/script.
extern Bool isWeatherRenderEnabled();
extern Bool hasWeatherTypeOverride();
extern Weather readMapWeatherOverride();

class WeatherSetting : public Overridable
{
	MEMORY_POOL_GLUE_WITH_USERLOOKUP_CREATE( WeatherSetting, "WeatherSetting"  )

	public:
		AsciiString m_snowTexture;
		Real	m_snowFrequencyScaleX;	///<used to adjust snow position.
		Real	m_snowFrequencyScaleY;	///<used to adjust snow position.
		Real	m_snowAmplitude;		///<used to adjust amount of of snow movement. (in world units)
		Real	m_snowPointSize;		///<used to control hardware point-sprite size. (in arbitrary units - see DX SDK Docs).
		Real	m_snowMaxPointSize;		///<used to control maximum size (in pixels) of point sprite.
		Real	m_snowMinPointSize;		///<used to control the minimum size (in piexels) of point sprite.
		Real	m_snowQuadSize;			///<used to control quad size when no hardware point sprites. (world width/height of quad)
		Real	m_snowBoxDimensions;	///<used to set dimensions of box surrounding camera. (world units)
		Real	m_snowBoxDensity;		///<used to control how many emitters are present per world unit
		Real	m_snowVelocity;			///<used to set speed at which snow falls (world units/sec).
		Bool    m_usePointSprites;		///<used to disable hardware point-sprite support.
		Bool	m_snowEnabled;			///<enable/disable snow on the map.
		// GeneralsX @feature Qoder 11/10/2026 Grass layer options (reuse the weather INI block).
		// GeneralsX @tweak Qoder 15/10/2026 Layer ships DISABLED by default (feature is built but
		// never field-verified). Re-enable per user/match by writing "GrassEnabled = yes" (and
		// optionally "GrassDensity = 0..100") into the weather INI block - no code change needed.
		Bool	m_grassEnabled;				///<enable/disable the 3D grass layer (off by default; INI: GrassEnabled).
		Real	m_grassDensity;				///<percent of qualifying grass cells that grow a clump (0..100).

		// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: the fields above are the retail
		// SNOW recipe and stay exactly as they were. Everything below grows that one block into a
		// per-weather-type profile so rain/storm/thunder/blizzard can be authored instead of hardcoded.
		// Defaults are chosen so the retail "Weather" block keeps producing bit-identical snow: the
		// new scalars only take effect for types whose own block sets them.
		enum ParticleKind
		{
			PARTICLE_SNOW = 0,		///<point sprites (or quads on cards without them) - the retail path.
			PARTICLE_RAIN = 1,		///<world-space line segments: streaks keep their length at any zoom.
			PARTICLE_COUNT
		};
		Weather m_weatherType;				///<which Weather enum value this profile drives.
		Bool m_weatherEnabled;			 ///<master switch for this weather type (retail block maps to SNOWY).
		ParticleKind m_particleKind;	 ///<world-layer primitive type for this weather.

		Real m_windX;							///<world units of horizontal drift per fall cycle, X axis.
		Real m_windY;							///<world units of horizontal drift per fall cycle, Y axis.
		Real m_streakLength;				///<rain streak length in world units (falls along -Z plus wind).
		Real m_streakThickness;			 ///<rain streak width in world units, quad fallback only.
		Real m_opacity;							///<0..1 alpha ceiling for a near-camera particle.
		Real m_falloffStart;				///<distance from camera where particles start fading out.
		Real m_falloffEnd;					///<distance from camera where particles vanish (sprites get no fog).
		Real m_swayScale;						 ///<scales the wave amplitude for this type (blizzard gusts).
		Real m_minShade;						///<floor for the scene-light tint so night weather stays visible.

		RGBColor m_particleTint;			///<multiplied into the scene-light tint (cool rain, warm reflections).

		Real m_ambientScale;				 ///<weather multiplier on GENERALSX_AMBIENT_SCALE (1 = no change).
		Real m_keyScale;						///<weather multiplier on GENERALSX_KEY_GAIN (1 = no change).
		Real m_fogScale;							///<weather multiplier on the scene fog slope (1 = no change).
		Real m_wetness;								///<0..1 roughness offset applied to PBR materials (rain-slick).
		RGBColor m_screenTint;				///<screen-layer color multiply (cloud cover, blue night rain).
		Real m_screenDarken;					///<screen-layer brightness multiply, 0..1 (thicker = darker sky).
		Real m_screenWispDensity;		 ///<0..1 strength of the scrolling screen-space drizzle/snow veil.
		Real m_screenVignette;				///<0..1 edge darkening (blizzard whiteout framing).
		Real m_lightningChance;			 ///<expected strikes per minute; 0 disables (device-side RNG only).
		Real m_lightningIntensity;		 ///<scale on the flash + dynamic light pulse a strike produces.

	public:
		WeatherSetting()
		{
			m_snowTexture = "EXSnowFlake.tga";
			m_snowFrequencyScaleX=0.0533f;
			m_snowFrequencyScaleY=0.0275f;
			m_snowAmplitude=5.0f;
			m_snowPointSize=1.0f;
			m_snowQuadSize=0.5f;
			m_snowBoxDimensions=200;
			m_snowBoxDensity=1;
			m_snowVelocity=4;
			m_usePointSprites=TRUE;
			m_snowEnabled=FALSE;
			// GeneralsX @tweak 15/10/2026 grass layer default OFF (enable via weather INI "GrassEnabled = yes").
			m_grassEnabled=FALSE;
			m_grassDensity=35.0f;
			m_snowMaxPointSize=64.0f;
			m_snowMinPointSize=0.0f;

			// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: this object starts life as the
			// retail SNOWY recipe - every new field is an identity/scale-one default so nothing below
			// changes how snow looks until a WeatherType block asks for it.
			m_weatherType=WEATHER_SNOWY;
			m_weatherEnabled=FALSE;
			m_particleKind=PARTICLE_SNOW;
			m_windX=0.0f;
			m_windY=0.0f;
			m_streakLength=2.5f;
			m_streakThickness=0.05f;
			m_opacity=0.85f;
			m_falloffStart=250.0f;
			m_falloffEnd=600.0f;
			m_swayScale=1.0f;
			m_minShade=0.45f;
			m_particleTint.red=1.0f;		m_particleTint.green=1.0f;		m_particleTint.blue=1.0f;
			m_ambientScale=1.0f;
			m_keyScale=1.0f;
			m_fogScale=1.0f;
			m_wetness=0.0f;
			m_screenTint.red=1.0f;		m_screenTint.green=1.0f;		m_screenTint.blue=1.0f;
			m_screenDarken=1.0f;
			m_screenWispDensity=0.0f;
			m_screenVignette=0.0f;
			m_lightningChance=0.0f;
			m_lightningIntensity=1.0f;
		}

		/// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: fill this profile with the authored
		/// defaults for a weather type. Types nobody overrides still look like a tuned retail effect.
		void applyTypeDefaults(Weather type);

		// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: the one question every consumer asks -
		// does this recipe want particles drawn? The two flags are OR'd on purpose: the retail block only
		// ever had SnowEnabled, and authored WeatherType blocks set both, so neither spelling can leave a
		// map with weather authored but nothing on screen. GENERALSX_WEATHER=0 overrides both (retail A/B leg).
		Bool wantsParticles() const { return isWeatherRenderEnabled() && (m_snowEnabled || m_weatherEnabled); }

		static const FieldParse m_weatherSettingFieldParseTable[];		///< the parse table for INI definition

		/// Get the INI parsing table for loading
		const FieldParse *getFieldParse() const { return m_weatherSettingFieldParseTable; }
};

EMPTY_DTOR(WeatherSetting)

extern OVERRIDE<WeatherSetting> TheWeatherSetting;

// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: one profile per weather type.
// TheWeatherSetting stays the "active recipe" that every render consumer already reads (retail code
// is untouched); these accessors let a map or a script swap which recipe is active without a new
// INI load, and let the screen/light layers ask for a type's tuning even when it isn't active.
extern WeatherSetting *getWeatherProfile(Weather type);			///<owned profile, never null after INI init.
extern Weather getActiveWeatherType();								///<type the active profile describes.
extern void applyWeatherType(Weather type);						///<point TheWeatherSetting at that type's profile.

class SnowManager : public SubsystemInterface
{
  public :
	  enum{
		 SNOW_NOISE_X=128,			//dimensions table holding noise function used for initial snow positions.
		 SNOW_NOISE_Y=128,			//dimensions table holding noise function used for initial snow positions.
		 // GeneralsX @tweak Storm996 16/10/2026 PS3.0 Weather W1: was 64x64. Rain wants several times the
		 // emitter count of snow, and every particle's starting height comes out of this table - at 64x64
		 // a dense rain box reused the same 4096 phases over and over and the streaks fell in visible
		 // slabs. 128x128 = 16 kB of Reals, and the retail snow recipe is unaffected (its cells keep
		 // their own index; they just stop colliding with 4096 neighbours).
	  };

	 SnowManager();
	virtual ~SnowManager() override;

	virtual void init() override;
	virtual void reset() override;
	virtual void updateIniSettings ();
	void setVisible(Bool showWeather);	///<enable/disable rendering of weather - assuming it's available on map.

	// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: switch the active recipe at runtime
	// (map data, script action or the QA env leg). Purely client-side: the enum value it mirrors is
	// owned by GlobalData/the script, so nothing here reaches the CRC state stream.
	virtual void setWeatherType(Weather type);
	Weather getWeatherType() const { return m_weatherType; }
	/// GeneralsX @feature Storm996 16/10/2026 PS3.0 Weather W1: script/map visibility gate, read by the
	/// render pass alongside the recipe's own enable flags.
	Bool isWeatherVisible() const { return m_isVisible; }

  protected :

	Real				*m_startingHeights;
	Real				m_time;	///<time elapsed since it started snowing.
	Real				m_velocity;	///<positive velocity of falling snow
	Real				m_fullTimePeriod;	///<time for snow to complete a full animation cycle.
	Real				m_frequencyScaleX;	///<used to adjust snow position.
	Real				m_frequencyScaleY;	///<used to adjust snow position.
	Real				m_amplitude;		///<used to adjust amount of of snow movement.
	Real				m_pointSize;		///<used to control hardware point-sprite size.
	Real				m_maxPointSize;		///<used to control maximum pixel size of sprites.
	Real				m_minPointSize;		///<used to control minimum pixel size of sprites.
	Real				m_quadSize;			///<used to control quad size when no hardware point sprites.
	Real				m_boxDimensions;	///<used to set dimensions of box surrounding camera.
	Real				m_emitterSpacing;		///<used to control how many emitters are present per world unit
	Bool				m_isVisible;		///<used to prevent map weather (if defined) from rendering.
	Weather			m_weatherType;		///<GeneralsX @feature PS3.0 Weather W1 16/10/2026 type the current recipe describes.
};

extern SnowManager *TheSnowManager;  ///< the ray effects singleton external
