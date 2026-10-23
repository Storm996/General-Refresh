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

///////////////////////////////////////////////////////////////////////////////////////
// FILE: OptionPreferences.h
// Author: Matthew D. Campbell, April 2002
// Description: Saving/Loading of option preferences
///////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "WW3D2/ww3d.h"
#include "WW3D2/texturefilter.h"

#include "Common/UserPreferences.h"

typedef UnsignedInt CursorCaptureMode;
typedef UnsignedInt ScreenEdgeScrollMode;

//-----------------------------------------------------------------------------
// OptionsPreferences options menu class
//-----------------------------------------------------------------------------
class OptionPreferences : public UserPreferences
{
public:
	OptionPreferences();
	virtual ~OptionPreferences() override;

	enum AntiAliasingMode CPP_11(: Int)
	{
		AntiAliasingMode_OFF = 0,
		AntiAliasingMode_MSAA_2X,
		AntiAliasingMode_MSAA_4X,
		AntiAliasingMode_MSAA_8X,
		AntiAliasingMode_Count
	};

	Bool loadFromIniFile();

	WW3D::MultiSampleModeEnum getAntiAliasing() const;
	TextureFilterClass::TextureFilterMode getTextureFilterMode() const;
	TextureFilterClass::AnisotropicFilterMode getTextureAnisotropyLevel() const;
	UnsignedInt getLANIPAddress();
	UnsignedInt getOnlineIPAddress();
	void setLANIPAddress(AsciiString IP);
	void setOnlineIPAddress(AsciiString IP);
	void setLANIPAddress(UnsignedInt IP);
	void setOnlineIPAddress(UnsignedInt IP);
	Bool getArchiveReplaysEnabled() const;
	Bool getAlternateMouseModeEnabled();
	Bool getRightMouseScrollWithAlternateMouseEnabled() const;
	Bool getRetaliationModeEnabled();
	Bool getDoubleClickAttackMoveEnabled();
	Int getJpegQuality() const;
	Real getScrollFactor();
	Bool getDrawScrollAnchor();
	Bool getMoveScrollAnchor();
	Bool getCursorCaptureEnabledInWindowedGame() const;
	Bool getCursorCaptureEnabledInWindowedMenu() const;
	Bool getCursorCaptureEnabledInFullscreenGame() const;
	Bool getCursorCaptureEnabledInFullscreenMenu() const;
	CursorCaptureMode getCursorCaptureMode() const;
	Bool getScreenEdgeScrollEnabledInWindowedApp() const;
	Bool getScreenEdgeScrollEnabledInFullscreenApp() const;
	ScreenEdgeScrollMode getScreenEdgeScrollMode() const;
	Int getFirewallBehavior();
	Short getFirewallPortAllocationDelta();
	UnsignedShort getFirewallPortOverride();
	Bool getFirewallNeedToRefresh();
	Bool usesSystemMapDir();
	AsciiString getPreferred3DProvider();
	AsciiString getSpeakerType();
	Real getSoundVolume();
	Real get3DSoundVolume();
	Real getSpeechVolume();
	Real getMusicVolume();
	Real getMoneyTransactionVolume() const;
	Bool saveCameraInReplays();
	Bool useCameraInReplays();
	Bool getPlayerObserverEnabled() const;
	Int getStaticGameDetail();
	Int getIdealStaticGameDetail();
	Real getGammaValue();
	Int getTextureReduction();
	void getResolution(Int *xres, Int *yres);
	Bool get3DShadowsEnabled();
	Bool get2DShadowsEnabled();
	Bool getCloudShadowsEnabled();
	Bool getLightmapEnabled();
	Bool getSmoothWaterEnabled();
	Bool getTreesEnabled();
	Bool getExtraAnimationsDisabled();
	Bool getUseHeatEffects();
	Bool getDynamicLODEnabled();
	Bool getFPSLimitEnabled();
	Bool getBuildingOcclusionEnabled();
	Int getParticleCap();

	Int getCampaignDifficulty();
	void setCampaignDifficulty(Int diff);

	Int getNetworkLatencyFontSize();
	Int getRenderFpsFontSize();
	Int getSystemTimeFontSize();
	Int getGameTimeFontSize();
	Int getPlayerInfoListFontSize();

	Real getResolutionFontAdjustment();

	Bool getShowMoneyPerMinute() const;

	Real getMaxCameraHeight() const;
	Real getMinCameraHeight() const;
	Real getCameraPitch() const;
	Real getTerrainDrawDistanceScale() const;
	Real getGameWindowTransitionSpeedMultiplier() const;

	// GeneralsX @feature 10/10/2026 Shadow system S1/S2 knobs.  These live only in the per-user
	// Options.ini, so they are never folded into m_iniCRC and cannot break replays or LAN matching;
	// missing keys fall back to the engine defaults.
	Real getShadowProjectedIntensity() const;					///< 0-100 percent, native sun-shadow darkening (default 25)
	Real getShadowVolumetricIntensity() const;				///< 0-100 percent, native sun-shadow penumbra scale (default 22)
	Int getShadowMapResolution() const;							///< shadow map edge length, one of 512/768/1024/2048, default 768
	// GeneralsX @feature 13/10/2026 PS3.0 lighting knobs from 引擎改造/设置参数交接说明.md.  All of
	// them persist as integer ticks in the Options.ini S-family (never folded into m_iniCRC, so
	// replays and LAN matching are safe); the GENERALSX_* environment variables keep overriding
	// them inside these getters so scripted A/B runs never need to touch the user's file.
	Real getNormalMapStrength() const;			///< tick 0-200 stored, returned 0.0-2.0 (default 0.8). #69: drives bump deviation, not sun gain.
	// GeneralsX @feature PS3.0 #70 15/10/2026: independent roughness (PBR) toggle, ini key
	// RoughnessMapping, 0 = off (default) / 1 = M1 roughness Blinn / 2 = M2 GGX; the GENERALSX_PBR
	// env still wins outright so scripted A/B runs never touch the user's file.
	Int getPBRMode() const;
	Int getForwardPlusMode() const;		///< 0 off, 1 2D tiles, 2 3D clusters (env GENERALSX_VK_FORWARDPLUS_MODE wins)
	Int getForwardLightBudget() const;	///< 32..256 global point-light cap (env GENERALSX_VK_FP_BUDGET wins)
	Real getBloomIntensity() const;				///< tick 0-100 stored, returned 0.0-1.0; env GENERALSX_BLOOM_INTENSITY wins outright (default 0.35)
	Bool getShadowMapEnabled() const;			///< options-page "real shadow" switch: ON = native Vulkan sun shadow (retail decal/stencil muted), OFF = retail fallback; env GENERALSX_SHADOWMAP overrides, else ini ShadowMap (default ON)
	Int getShadowMapIntensity() const;			///< 0-100 percent; env GENERALSX_SHADOWMAP_INT wins when set (default 55)
	Int getSunShadowMapResolution() const;		///< one of 256/512/1024, snap-up (default 1024)
	Int getPresentInterval() const;				///< GeneralsX @feature PS3.0-P3-25a vsync ticks 0..3; env GENERALSX_SWAPINTERVAL wins, else ini VSync (default 1 = on). Restart to take effect.
	// GeneralsX @feature PS3.0 4E 17/10/2026 lighting stage-four switches for the Graphics Options page.
	// Same S-family rules: Options.ini only, never folded into m_iniCRC (replay/LAN safe), and the
	// GENERALSX_* QA env overrides keep winning inside these getters so A/B scripts never touch the file.
	Bool getLightPulseEnabled() const;			///< 4A explosion/muzzle dynamic light pulses; env GENERALSX_LIGHTPULSE wins, else ini LightPulseEffect (default ON)
	Bool getLampLightEnabled() const;			///< 4B building lamps; env GENERALSX_LAMPLIGHT wins, else ini LampLightEffect (default ON)
	Int getLampLightIntensity() const;			///< 4F lamp brightness gain; tick 0-200 stored, 100 = 1.0x; env GENERALSX_LAMP_INT (float) wins (default 60 = 0.6x)
	// GeneralsX @feature Storm996 17/10/2026 PS3.0 haze user switch: the P3-14 distance haze gets its
	// own page rows. ini DistanceHaze (0/1, default ON) + DistanceHazeIntensity (ticks 0-100 = the
	// 0.0-1.0 blend strength, default 60); env GENERALSX_GNFOG (float 0..1) wins over both so QA legs
	// never edit the user's file, and 0 there means "off AND strength 0" for a bit-exact parity run.
	Bool getDistanceHazeEnabled() const;
	Int getDistanceHazeIntensity() const;		///< 0-100 percent haze blend at the far end of the ramp.
	Int getTodPreview() const;					///< 4C time-of-day preview 0 = follow the map, 1..4 force a slot; env GENERALSX_TOD_OVERRIDE wins, else ini TimeOfDayPreview
	// GeneralsX @feature PS3.0 L2.2 17/10/2026: HLSL terrain trio (TERRAIN30). Since the 20/10/2026 merge
	// this is no longer a page row; env GENERALSX_TERRAIN30 keeps winning for QA legs, else the legacy ini
	// key Terrain30Shading (default OFF = retail FF/.pso parity, the batch22 verdict).
	Bool getTerrain30Shading() const;
	// GeneralsX @feature Qoder 17/10/2026 PS3.0 T0 P4: terrain per-pixel macro-normal switch, now the single
	// terrain row on the Graphics Options page. env GENERALSX_TERRAIN_PPL wins for QA legs, else ini
	// TerrainPerPixel (factory default OFF = the L2.1 NM trio parity). Turning it on also arms the ps_3_0/NM
	// leg that the retired "现代地形光影" parent row used to carry, so it never needs another switch.
	// Restart (terrain shader re-init) to take effect.
	Bool getTerrainPerPixel() const;
	// GeneralsX @feature Qoder 17/10/2026: per-pixel lighting master switch (the DX8GeomNormal pass)
	// from the Graphics Options page row "逐像素光照". env GENERALSX_GEOMNORMAL keeps winning for QA legs
	// ("0" = off, anything else = on), else ini PerPixelLighting (default ON = the 14/10 shipping state).
	// Live: the renderer asks DX8GeomNormal::Ready_To_Render() per batch, so no restart is needed.
	// Orthogonal to UseNormalMapping / RoughnessMapping / NormalMapStrength - those stay companion
	// layers mounted per asset, this one only decides fixed-function vs per-pixel lighting.
	Bool getPerPixelLighting() const;
	// GeneralsX @feature felipebraz 17/09/2026 Skirmish simulation tick rate configuration (#281)
	Int getSkirmishTickRate() const;
};
