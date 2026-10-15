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

// FILE: OptionsMenu.cpp //////////////////////////////////////////////////////////////////////////
// Author: Colin Day, October 2001
// Description: options menu window callbacks
///////////////////////////////////////////////////////////////////////////////////////////////////

// INCLUDES ///////////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "gamespy/ghttp/ghttp.h"

#include "Common/AudioAffect.h"
#include "Common/AudioSettings.h"
#include "Common/GameAudio.h"
#include "Common/GameEngine.h"
#include "Common/FramePacer.h"
#include "Common/OptionPreferences.h"
#include "Common/GameLOD.h"
#include "Common/Recorder.h"
#include "Common/Registry.h"
#include "Common/version.h"

// GeneralsX @feature Qoder 13/10/2026 getenv() for the shader-model override precedence check.
#include <cstdlib>

#include "GameClient/ClientInstance.h"
#include "GameClient/GameClient.h"
#include "GameClient/InGameUI.h"
#include "GameClient/LookAtXlat.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetCheckBox.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetTextEntry.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetRadioButton.h"
#include "GameClient/GadgetSlider.h"
#include "GameClient/GadgetPushButton.h" // GeneralsX @feature Qoder 13/10/2026 GadgetButtonSetText for the lighting subpage labels
#include "GameClient/HeaderTemplate.h"
// GeneralsX @feature Qoder 13/10/2026 Graphics options page: @pop back button dispatch.
#include "GameClient/MenuCommands.h"
#include "GameClient/Shell.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Mouse.h"
#include "GameClient/GameText.h"
#include "GameClient/Display.h"
#include "GameClient/IMEManager.h"
#include "GameClient/ShellHooks.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GlobalLanguage.h"
#include "GameNetwork/FirewallHelper.h"
#include "GameNetwork/IPEnumeration.h"
#include "GameNetwork/GameSpyOverlay.h"
#include "GameNetwork/GameSpy/PeerDefs.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/ScriptEngine.h"
#include "WWDownload/Registry.h"
#include "GameClient/MessageBox.h"

#include "WW3D2/ww3d.h"
#include "WW3D2/texturefilter.h"
#include "WW3D2/dx8normalmap.h"	// GeneralsX @feature 11/10/2026 dot3 normal-mapping toggle

// This is for non-RC builds only!!!
#define VERBOSE_VERSION L"Release"



static NameKeyType		comboBoxOnlineIPID	= NAMEKEY_INVALID;
static GameWindow *		comboBoxOnlineIP		= nullptr;

static NameKeyType		comboBoxLANIPID	= NAMEKEY_INVALID;
static GameWindow *		comboBoxLANIP		= nullptr;

static NameKeyType    comboBoxResolutionID      = NAMEKEY_INVALID;
static GameWindow *   comboBoxResolution       = nullptr;

static NameKeyType    comboBoxDetailID      = NAMEKEY_INVALID;
static GameWindow *   comboBoxDetail        = nullptr;

static NameKeyType		checkAlternateMouseID	= NAMEKEY_INVALID;
static GameWindow *		checkAlternateMouse		= nullptr;

static NameKeyType		checkRetaliationID	= NAMEKEY_INVALID;
static GameWindow *		checkRetaliation		= nullptr;

static NameKeyType		checkDoubleClickAttackMoveID	= NAMEKEY_INVALID;
static GameWindow *		checkDoubleClickAttackMove		= nullptr;

static NameKeyType		sliderScrollSpeedID	= NAMEKEY_INVALID;
static GameWindow *		sliderScrollSpeed		= nullptr;

static NameKeyType    checkLanguageFilterID = NAMEKEY_INVALID;
static GameWindow *   checkLanguageFilter   = nullptr;

static NameKeyType		checkUseCameraID		= NAMEKEY_INVALID;
static GameWindow *		checkUseCamera			= nullptr;

static NameKeyType		checkSaveCameraID		= NAMEKEY_INVALID;
static GameWindow *		checkSaveCamera			= nullptr;

static NameKeyType		checkDrawAnchorID		= NAMEKEY_INVALID;
static GameWindow *		checkDrawAnchor			= nullptr;

static NameKeyType		checkMoveAnchorID		= NAMEKEY_INVALID;
static GameWindow *		checkMoveAnchor			= nullptr;

static NameKeyType		buttonFirewallRefreshID	= NAMEKEY_INVALID;
static GameWindow *		buttonFirewallRefresh		= nullptr;
//
//static NameKeyType    checkAudioHardwareID = NAMEKEY_INVALID;
//static GameWindow *   checkAudioHardware   = nullptr;
//
//static NameKeyType    checkAudioSurroundID = NAMEKEY_INVALID;
//static GameWindow *   checkAudioSurround   = nullptr;
////volume controls
//
static NameKeyType    sliderMusicVolumeID = NAMEKEY_INVALID;
static GameWindow *   sliderMusicVolume   = nullptr;

static NameKeyType    sliderSFXVolumeID = NAMEKEY_INVALID;
static GameWindow *   sliderSFXVolume   = nullptr;

static NameKeyType    sliderVoiceVolumeID = NAMEKEY_INVALID;
static GameWindow *   sliderVoiceVolume   = nullptr;

static NameKeyType    sliderGammaID = NAMEKEY_INVALID;
static GameWindow *   sliderGamma = nullptr;

//Advanced Options Screen
static NameKeyType    WinAdvancedDisplayID      = NAMEKEY_INVALID;
static GameWindow *   WinAdvancedDisplay				= nullptr;

static NameKeyType    ButtonAdvancedAcceptID      = NAMEKEY_INVALID;
static GameWindow *   ButtonAdvancedAccept				= nullptr;

static NameKeyType    ButtonAdvancedCancelID      = NAMEKEY_INVALID;
static GameWindow *   ButtonAdvancedCancel				= nullptr;

// GeneralsX @refactor Qoder 13/10/2026 The lighting subpage popup was retired after the crash
// bisect; the seven lighting controls now live directly on the options page and are driven by
// s_gameOptionRegistry only, so no popup gadgets are tracked here any more.

static NameKeyType    sliderTextureResolutionID = NAMEKEY_INVALID;
static GameWindow *   sliderTextureResolution = nullptr;

static NameKeyType    sliderParticleCapID = NAMEKEY_INVALID;
static GameWindow *   sliderParticleCap = nullptr;

static NameKeyType    check3DShadowsID = NAMEKEY_INVALID;
static GameWindow *   check3DShadows   = nullptr;

static NameKeyType    check2DShadowsID = NAMEKEY_INVALID;
static GameWindow *   check2DShadows   = nullptr;

static NameKeyType    checkCloudShadowsID = NAMEKEY_INVALID;
static GameWindow *   checkCloudShadows   = nullptr;

static NameKeyType    checkGroundLightingID = NAMEKEY_INVALID;
static GameWindow *   checkGroundLighting   = nullptr;

static NameKeyType    checkSmoothWaterID = NAMEKEY_INVALID;
static GameWindow *   checkSmoothWater   = nullptr;

static NameKeyType    checkBuildingOcclusionID = NAMEKEY_INVALID;
static GameWindow *   checkBuildingOcclusion   = nullptr;

static NameKeyType    checkPropsID = NAMEKEY_INVALID;
static GameWindow *   checkProps   = nullptr;

static NameKeyType    checkExtraAnimationsID = NAMEKEY_INVALID;
static GameWindow *   checkExtraAnimations   = nullptr;

static NameKeyType    checkNoDynamicLodID = NAMEKEY_INVALID;
static GameWindow *   checkNoDynamicLod   = nullptr;

static NameKeyType    checkUnlockFpsID = NAMEKEY_INVALID;
static GameWindow *   checkUnlockFps   = nullptr;

// GeneralsX @refactor Qoder 12/10/2026 Max FPS limit and normal mapping moved to the options registry below.

static NameKeyType    checkHeatEffectsID = NAMEKEY_INVALID;
static GameWindow *   checkHeatEffects   = nullptr;

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 12/10/2026 Declarative options registry (menu rework plan S1).
// One user-facing option = one row in s_gameOptionRegistry. The row binds: the Options.ini key
// (single source of truth), the layout gadget, the display label, the value domain and the runtime
// side effect. Control lookup, entry building, refill on open, save on accept and startup
// application are done by three generic loops below, so adding a new option no longer needs
// dedicated static pointers, init blocks, saveOptions branches or GameEngine.cpp overrides.
enum GameOptionType
{
	GAME_OPTION_BOOL,   // CheckBox gadget, stored as 0/1
	GAME_OPTION_COMBO,  // ComboBox gadget, stored as the selected value from comboValues
	GAME_OPTION_SLIDER, // HorzSlider gadget, stored as the raw slider position (integer ticks)
};

struct GameOptionDesc
{
	GameOptionType type;
	const char *iniKey;               // Options.ini key
	const char *controlName;          // interactive gadget name in the layout
	const char *labelControlName;     // optional static text to relabel; nullptr = label the gadget itself
	const wchar_t *labelText;         // optional label text set from code (\u escapes keep the layout free of localized strings)
	const Int *comboValues;           // GAME_OPTION_COMBO: one stored value per entry
	const wchar_t *const *comboTexts; // optional entry display text; nullptr = show the number
	Int comboCount;
	Int sliderMin;                    // GAME_OPTION_SLIDER: min position applied to the gadget at init
	Int sliderMax;                    // GAME_OPTION_SLIDER: max position applied to the gadget at init
	Int (*globalFallback)();          // effective value shown when the key is absent from Options.ini
	void (*apply)(Int value);         // runtime side effect; called on save, and at startup only when the key exists
};

static Int gameOptionFallback_FpsLimit()
{
	return TheGlobalData ? TheGlobalData->m_framesPerSecondLimit : 60;
}

static Int gameOptionFallback_NormalMapping()
{
	return TheGlobalData ? (TheGlobalData->m_useNormalMapping != FALSE) : FALSE;
}

static void gameOptionApply_FpsLimit(Int fpsLimit)
{
	TheWritableGlobalData->m_framesPerSecondLimit = fpsLimit;
	TheWritableGlobalData->m_useFpsLimit = TRUE;
	if (TheFramePacer)
	{
		TheFramePacer->setFramesPerSecondLimit(fpsLimit);
	}
}

static void gameOptionApply_NormalMapping(Int enabled)
{
	TheWritableGlobalData->m_useNormalMapping = (enabled != 0) ? TRUE : FALSE;
	DX8NormalMap::Set_Enabled(enabled != 0);
}

static Int gameOptionFallback_AntiAliasing()
{
	return TheGlobalData ? TheGlobalData->m_antiAliasLevel : 0;
}

// The stored value is the WW3D multisample level (0/2/4/8), same domain the retail chain wrote,
// so the startup read in GlobalData.cpp and this apply agree on the Options.ini encoding.
static void gameOptionApply_AntiAliasing(Int level)
{
	TheWritableGlobalData->m_antiAliasLevel = level;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @tweak Qoder 13/10/2026 The options-menu pixel-shader ceiling row was removed at the
// user's request: on SM3-capable hardware the auto tier already is 3.0, so the row was a no-op in
// normal use. The QA channels stay - -shadermodel (CommandLine.cpp writes m_shaderModelCap) and
// GENERALSX_SHADERMODEL (W3DShaderManager::applyShaderModelCap) still cap the emitted tier for
// A/B forensics without any menu plumbing.

static const Int s_fpsLimitValues[] = { 30, 60, 120, 1000 };
static const wchar_t *const s_fpsLimitTexts[] = { L"30 FPS", L"60 FPS", L"120 FPS", L"No Limit" };

// GeneralsX @feature Qoder 12/10/2026 Anti aliasing moved onto the registry; the retail combo
// lived hidden in VideoParent with CSF-built entries, now the gadget is relabeled/refilled here.
static const Int s_antiAliasingValues[] = { 0, 2, 4, 8 };
static const wchar_t *const s_antiAliasingTexts[] = { L"Off", L"2X", L"4X", L"8X" };

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 13/10/2026 PS3.0 lighting parameters on the registry (引擎改造/设置参数交接说明 §8).
// All values persist as integer ticks in Options.ini through the generic setInt loop; the
// OptionPreferences getters own the units conversion and keep the GENERALSX_* QA env overrides
// authoritative, so these fallbacks show exactly what the renderer will consume when the ini key
// is absent. Sliders store raw ticks: 0-200 = percent x100 for the 0.0-2.0 strength range, 0-100
// elsewhere. Only the normal-map strength has a live apply hook (DX8NormalMap re-arms every frame);
// bloom, multi-light and the sun shadow map are compiled/created once per process and need a restart.

static Int gameOptionFallback_NormalStrength()
{
	return (Int)(OptionPreferences().getNormalMapStrength() * 100.0f + 0.5f);
}

static Int gameOptionFallback_Bloom()
{
	return (Int)(OptionPreferences().getBloomIntensity() * 100.0f + 0.5f);
}

static Int gameOptionFallback_ShadowDensity()
{
	return OptionPreferences().getShadowMapIntensity();
}

static Int gameOptionFallback_ShadowSoftening()
{
	return OptionPreferences().getShadowSoftening();
}

static Int gameOptionFallback_SunShadowRes()
{
	return OptionPreferences().getSunShadowMapResolution();
}

static Int gameOptionFallback_MultiLight()
{
	return OptionPreferences().getMultiLightEnabled() != FALSE;
}

static Int gameOptionFallback_ShadowMap()
{
	return OptionPreferences().getShadowMapEnabled() != FALSE;
}

static void gameOptionApply_NormalStrength(Int tick)
{
	DX8NormalMap::Set_Strength((Real)tick * 0.01f);
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature PS3.0 #70 15/10/2026: roughness (PBR) independent of the normal-map toggle.
// Stored value is the mode itself (0/1/2) so Options.ini "RoughnessMapping" matches the env domain
// GENERALSX_PBR and the startup read in W3DShaderManager consumes the same encoding.
//-------------------------------------------------------------------------------------------------
static Int gameOptionFallback_Roughness()
{
	return OptionPreferences().getPBRMode();
}

static void gameOptionApply_Roughness(Int mode)
{
	DX8NormalMap::Set_PBR_Mode(mode);
}

static const Int s_roughnessValues[] = { 0, 1, 2 };
static const wchar_t *const s_roughnessTexts[] = { L"\u5173\u95ed", L"\u57fa\u7840\u9ad8\u5149", L"\u5b8c\u6574 PBR" };

static const Int s_shadowSofteningValues[] = { 0, 1, 2, 3 };
static const wchar_t *const s_shadowSofteningTexts[] = { L"\u5173", L"\u4f4e", L"\u4e2d", L"\u9ad8" };

// GeneralsX @feature PS3.0-P3-22 14/10/2026: 2048 joins the sun-map resolution dropdown (S2 tiers).
static const Int s_sunShadowResValues[] = { 256, 512, 1024, 2048 };
static const wchar_t *const s_sunShadowResTexts[] = { L"256", L"512", L"1024", L"2048" };

// GeneralsX @feature Qoder 13/10/2026 Control names are the bare gadget names (no layout prefix):
// the init loop formats "<layoutPrefix>:<name>", so the SAME rows drive both the retail
// OptionsMenu.wnd gadgets and the clones on the new standalone GraphicsOptions.wnd page.
static const GameOptionDesc s_gameOptionRegistry[] =
{
	{
		GAME_OPTION_COMBO, "MaxFPSLimit", "ComboBoxMaxFpsLimit",
		"StaticTextMaxFpsLimit", L"\u6700\u5927\u5e27\u7387",
		s_fpsLimitValues, s_fpsLimitTexts, (Int)ARRAY_SIZE(s_fpsLimitValues), 0, 0,
		gameOptionFallback_FpsLimit, gameOptionApply_FpsLimit,
	},
	{
		GAME_OPTION_BOOL, "UseNormalMapping", "CheckNormalMapping",
		nullptr, L"\u6cd5\u7ebf\u8d34\u56fe",
		nullptr, nullptr, 0, 0, 0,
		gameOptionFallback_NormalMapping, gameOptionApply_NormalMapping,
	},
	// GeneralsX @feature PS3.0 #70 15/10/2026: independent roughness toggle (0 off / 1 M1 Blinn /
	// 2 M2 GGX). Works with normal mapping on OR off; roughness-only mounts the PBR pair with flat
	// normals, so the two features are orthogonal as the user requested.
	{
		GAME_OPTION_COMBO, "RoughnessMapping", "ComboBoxRoughness",
		"StaticTextRoughness", L"\u7c97\u7cd9\u5ea6\u8d34\u56fe",
		s_roughnessValues, s_roughnessTexts, (Int)ARRAY_SIZE(s_roughnessValues), 0, 0,
		gameOptionFallback_Roughness, gameOptionApply_Roughness,
	},
	{
		GAME_OPTION_COMBO, "AntiAliasing", "ComboBoxAntiAliasing",
		"StaticTextAntiAliasing", L"\u6297\u952f\u9f7f",
		s_antiAliasingValues, s_antiAliasingTexts, (Int)ARRAY_SIZE(s_antiAliasingValues), 0, 0,
		gameOptionFallback_AntiAliasing, gameOptionApply_AntiAliasing,
	},
	// GeneralsX @feature Qoder 13/10/2026 Lighting option rows. After the popup crash bisect the
	// seven gadgets live directly on the options page (inline layout in OptionsMenu.wnd); the
	// generic init/save loops below drive them, so each row stays one table entry.
	{
		GAME_OPTION_SLIDER, "NormalMapStrength", "SliderNormalStrength",
		"StaticTextNormalStrength", L"\u6cd5\u7ebf\u5f3a\u5ea6",
		nullptr, nullptr, 0, 0, 200,
		gameOptionFallback_NormalStrength, gameOptionApply_NormalStrength,
	},
	{
		GAME_OPTION_SLIDER, "BloomIntensity", "SliderBloom",
		"StaticTextBloom", L"\u6cdb\u5149\u5f3a\u5ea6",
		nullptr, nullptr, 0, 0, 100,
		gameOptionFallback_Bloom, nullptr,
	},
	{
		GAME_OPTION_SLIDER, "ShadowMapIntensity", "SliderShadowDensity",
		"StaticTextShadowDensity", L"\u9634\u5f71\u6d53\u5ea6",
		nullptr, nullptr, 0, 0, 100,
		gameOptionFallback_ShadowDensity, nullptr,
	},
	{
		GAME_OPTION_COMBO, "ShadowSoftening", "ComboBoxShadowSoftening",
		"StaticTextShadowSoftening", L"\u9634\u5f71\u67d4\u5316",
		s_shadowSofteningValues, s_shadowSofteningTexts, (Int)ARRAY_SIZE(s_shadowSofteningValues), 0, 0,
		gameOptionFallback_ShadowSoftening, nullptr,
	},
	{
		GAME_OPTION_COMBO, "SunShadowMapResolution", "ComboBoxSunShadowRes",
		"StaticTextSunShadowRes", L"\u9634\u5f71\u5206\u8fa8\u7387",
		s_sunShadowResValues, s_sunShadowResTexts, (Int)ARRAY_SIZE(s_sunShadowResValues), 0, 0,
		gameOptionFallback_SunShadowRes, nullptr,
	},
	{
		GAME_OPTION_BOOL, "MultiLightEffect", "CheckMultiLight",
		nullptr, L"\u591a\u5149\u6e90\u6548\u679c",
		nullptr, nullptr, 0, 0, 0,
		gameOptionFallback_MultiLight, nullptr,
	},
	{
		GAME_OPTION_BOOL, "ShadowMap", "CheckShadowMap",
		nullptr, L"\u771f\u5b9e\u9634\u5f71\uff08\u5b9e\u9a8c\uff09",
		nullptr, nullptr, 0, 0, 0,
		gameOptionFallback_ShadowMap, nullptr,
	},
};

#define GAME_OPTION_COUNT (Int)ARRAY_SIZE(s_gameOptionRegistry)

// GeneralsX @feature Qoder 13/10/2026 Slot set for the standalone GraphicsOptions.wnd page. The
// graphics rows moved out of the retail options menu (.wnd blocks removed at the user's request),
// so this is the only page driving the registry gadgets any more; the retail page keeps its own
// hand-written controls untouched.
static GameWindow *s_graphicsOptionControls[GAME_OPTION_COUNT] = { nullptr };

static void initGameOptionControls( const OptionPreferences *optionsPref,
	const GameOptionDesc *registry, Int count, GameWindow **slots, const char *layoutPrefix )
{
	for (Int i = 0; i < count; ++i)
	{
		const GameOptionDesc &desc = registry[i];
		AsciiString fullName;
		fullName.format( "%s:%s", layoutPrefix, desc.controlName );
		GameWindow *control = TheWindowManager->winGetWindowFromId( nullptr,
			TheNameKeyGenerator->nameToKey( fullName ) );
		slots[i] = control;
		if (!control)
			continue; // layout variant without this gadget: safe no-op

		if (desc.labelText)
		{
			if (desc.labelControlName)
			{
				AsciiString labelName;
				labelName.format( "%s:%s", layoutPrefix, desc.labelControlName );
				GameWindow *label = TheWindowManager->winGetWindowFromId( nullptr,
					TheNameKeyGenerator->nameToKey( labelName ) );
				if (label)
					GadgetStaticTextSetText( label, UnicodeString(desc.labelText) );
			}
			else if (desc.type == GAME_OPTION_BOOL)
			{
				GadgetCheckBoxSetText( control, UnicodeString(desc.labelText) );
			}
		}

		const Int value = optionsPref->getInt( desc.iniKey, desc.globalFallback() );

		if (desc.type == GAME_OPTION_COMBO)
		{
			GadgetComboBoxReset( control );
			for (Int e = 0; e < desc.comboCount; ++e)
			{
				UnicodeString text;
				if (desc.comboTexts && desc.comboTexts[e])
					text = UnicodeString(desc.comboTexts[e]);
				else
					text.format( L"%d", desc.comboValues[e] );
				GadgetComboBoxAddEntry( control, text, GameMakeColor(255,255,255,255) );
			}
			// Snap the selection to the first entry that meets or exceeds the stored value.
			Int sel = desc.comboCount - 1;
			for (Int e = 0; e < desc.comboCount; ++e)
			{
				if (desc.comboValues[e] >= value)
				{
					sel = e;
					break;
				}
			}
			GadgetComboBoxSetSelectedPos( control, sel );
		}
		else if (desc.type == GAME_OPTION_BOOL)
		{
			GadgetCheckBoxSetChecked( control, value != 0 );
		}
		else if (desc.type == GAME_OPTION_SLIDER)
		{
			GadgetSliderSetMinMax( control, desc.sliderMin, desc.sliderMax );
			GadgetSliderSetPosition( control, value );
		}
	}
}

static void saveGameOptionControls( OptionPreferences *optionsPref,
	const GameOptionDesc *registry, Int count, GameWindow **slots )
{
	for (Int i = 0; i < count; ++i)
	{
		const GameOptionDesc &desc = registry[i];
		const GameWindow *control = slots[i];
		slots[i] = nullptr; // consumed for this page session; init repopulates on the next open
		if (!control)
			continue;

		Int value = 0;
		if (desc.type == GAME_OPTION_COMBO)
		{
			Int pos = 0;
			GadgetComboBoxGetSelectedPos( (GameWindow *)control, &pos );
			pos = clamp( 0, pos, desc.comboCount - 1 );
			value = desc.comboValues[pos];
		}
		else if (desc.type == GAME_OPTION_SLIDER)
		{
			value = GadgetSliderGetPosition( (GameWindow *)control );
		}
		else
		{
			value = GadgetCheckBoxIsChecked( (GameWindow *)control ) ? 1 : 0;
		}

		optionsPref->setInt( desc.iniKey, value );
		if (desc.apply)
			desc.apply( value );
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 12/10/2026 Apply registry options that the user actually saved.
// Called once at engine startup (see GameEngine.cpp) before the frame pacer and the shell
// map/shader installation consume the values. Keys absent from Options.ini keep the values
// loaded from Generals.ini/SagePatch.ini, so retail defaults and QA overrides are untouched.
void ApplyGameOptionRegistryAtStartup()
{
	OptionPreferences optionsPref;
	for (Int i = 0; i < (Int)ARRAY_SIZE(s_gameOptionRegistry); ++i)
	{
		const GameOptionDesc &desc = s_gameOptionRegistry[i];
		if (!desc.apply)
			continue;
		if (optionsPref.getAsciiString( desc.iniKey, AsciiString() ).isEmpty() )
			continue;
		desc.apply( optionsPref.getInt( desc.iniKey, desc.globalFallback() ) );
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 13/10/2026 Standalone graphics options page (menu rework plan S1b).
// Shared preferences handle (declared here, further down this file, for the graphics page
// callbacks below; definition moved up because the new callbacks run the registry save loop).
static OptionPreferences *pref = nullptr;

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 13/10/2026 The in-game pause menu opens the graphics page as a persistent
// overlay - exactly like the retail options page (getOptionsLayout + bringForward) - and NOT as a
// shell-stack push. Inside a live match the shell stack still has the shell main menu linked under it,
// so a "@push"/"@pop" there popped back onto that screen and flung the player out to the main menu. The
// overlay is stacked over the pause menu and merely destroyed on close, so we return to whatever was
// underneath. The main-menu entry keeps the declarative @push/@pop stack path (unchanged).
static WindowLayout *s_graphicsLayout = nullptr;

void OpenGraphicsOptionsLayout()
{
	if (!s_graphicsLayout)
		s_graphicsLayout = TheWindowManager->winCreateLayout( "Menus/GraphicsOptions.wnd" );
	if (!s_graphicsLayout)
		return;

	s_graphicsLayout->runInit();
	s_graphicsLayout->hide( FALSE );
	s_graphicsLayout->bringForward();

	GameWindow *root = s_graphicsLayout->getFirstWindow();
	if (root)
	{
		TheWindowManager->winSetModal( root );
		TheWindowManager->winSetFocus( root );
	}
}

static void CloseGraphicsOptionsLayout( Bool save )
{
	if (save)
	{
		saveGameOptionControls( pref, s_gameOptionRegistry, GAME_OPTION_COUNT, s_graphicsOptionControls );
		if (pref)
		{
			pref->write();
			delete pref;
			pref = nullptr;
		}
	}
	else if (pref)
	{
		delete pref;
		pref = nullptr;
	}

	if (s_graphicsLayout)
	{
		s_graphicsLayout->destroyWindows();
		deleteInstance( s_graphicsLayout );
		s_graphicsLayout = nullptr;
	}

	// the gadget pointers dangled the moment the windows were freed
	for (Int i = 0; i < GAME_OPTION_COUNT; ++i)
		s_graphicsOptionControls[i] = nullptr;
}

// A second entry over the SAME registry rows, opened from the new main-menu button via the
// declarative "@push Menus/GraphicsOptions.wnd" verb. The layout clones every graphics gadget with
// the GraphicsOptions.wnd: prefix, so init/save just re-run the generic loops against a second
// slot array; both pages write identical Options.ini keys and stay consistent because Shell
// push/pop unloads the page below, and each reopen re-reads the ini through its own init.
void GraphicsOptionsMenuInit( WindowLayout *layout, void *userData )
{
	if (TheGameEngine->getQuitting())
		return;

	if (!pref)
		pref = NEW OptionPreferences;

	initGameOptionControls( pref, s_gameOptionRegistry, GAME_OPTION_COUNT, s_graphicsOptionControls,
		"GraphicsOptions.wnd" );

	// Title label comes from code: the localized string table lives inside a BIG, so the loose
	// layout keeps TEXT = "[None]" (same pattern as the main-menu Chinese button labels).
	GameWindow *title = TheWindowManager->winGetWindowFromId( nullptr,
		TheNameKeyGenerator->nameToKey( "GraphicsOptions.wnd:StaticTextTitle" ) );
	if (title)
		GadgetStaticTextSetText( title, UnicodeString( L"\u56fe\u5f62\u9009\u9879" ) );
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @bugfix Qoder 13/10/2026 Shell::pop() only completes when the layout's shutdown
// callback reports back via Shell::shutdownComplete(); the retail options page routes its back
// button through DestroyOptionsLayout instead, so the generated graphics page (pure @pop/@push
// navigation) hung with every exit button dead until this LAYOUTSHUTDOWN existed.
void GraphicsOptionsMenuShutdown( WindowLayout *layout, void *userData )
{
	// the gadgets die with the layout: drop the cached pointers before the windows are freed
	for (Int i = 0; i < GAME_OPTION_COUNT; ++i)
		s_graphicsOptionControls[i] = nullptr;

	// the preferences handle is only persisted on Accept; any other exit discards it
	if (pref)
	{
		delete pref;
		pref = nullptr;
	}

	layout->hide( TRUE );
	TheShell->shutdownComplete( layout );
}

//-------------------------------------------------------------------------------------------------
/** graphics options menu window input callback - Esc routes to the @pop back button */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType GraphicsOptionsMenuInput( GameWindow *window, UnsignedInt msg,
																				 WindowMsgData mData1, WindowMsgData mData2 )
{
	if (msg == GWM_CHAR && mData1 == KEY_ESC)
	{
		if (BitIsSet((Byte)mData2, KEY_STATE_UP))
		{
			// GeneralsX @bugfix Qoder 13/10/2026 In-game the page is a persistent overlay, so Esc
			// destroys it (back to the pause menu). Only the main-menu shell entry routes Esc to the
			// "@pop" back button, because there the page really is on the shell stack.
			if (TheGameLogic && TheGameLogic->isInGame()
				&& TheGameLogic->getGameMode() != GAME_SHELL)
			{
				CloseGraphicsOptionsLayout( FALSE );
				return MSG_HANDLED;
			}

			NameKeyType buttonID = TheNameKeyGenerator->nameToKey( "GraphicsOptions.wnd:ButtonBack@pop" );
			GameWindow *button = TheWindowManager->winGetWindowFromId( nullptr, buttonID );
			TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
				(WindowMsgData)button, buttonID );
		}
		return MSG_HANDLED;
	}
	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
/** graphics options menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType GraphicsOptionsMenuSystem( GameWindow *window, UnsignedInt msg,
																					WindowMsgData mData1, WindowMsgData mData2 )
{
	static NameKeyType buttonGraphicsAccept = NAMEKEY_INVALID;
	static NameKeyType buttonGraphicsBack = NAMEKEY_INVALID;

	switch( msg )
	{
		case GWM_CREATE:
			buttonGraphicsAccept = TheNameKeyGenerator->nameToKey( "GraphicsOptions.wnd:ButtonGraphicsAccept" );
			buttonGraphicsBack = TheNameKeyGenerator->nameToKey( "GraphicsOptions.wnd:ButtonBack@pop" );
			break;

		case GWM_INPUT_FOCUS:
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;
			return MSG_HANDLED;

		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			if( !control )
				break;

			// GeneralsX @bugfix Qoder 13/10/2026 In-game the page is a persistent overlay over the
			// pause menu, so Accept/Back destroy that overlay instead of popping the shell stack (a
			// shell pop here would expose the shell main menu still linked underneath and kick the
			// player out to it). Only the main-menu (shell) entry uses the @pop / TheShell->pop path.
			if (TheGameLogic && TheGameLogic->isInGame()
				&& TheGameLogic->getGameMode() != GAME_SHELL)
			{
				Int inGameControlID = control->winGetWindowId();
				if (inGameControlID == buttonGraphicsAccept)
				{
					CloseGraphicsOptionsLayout( TRUE );
					return MSG_HANDLED;
				}
				if (inGameControlID == buttonGraphicsBack)
				{
					CloseGraphicsOptionsLayout( FALSE );
					return MSG_HANDLED;
				}
				// combos / checkboxes / sliders keep their own gadget handling
				return MSG_IGNORED;
			}

			// Declarative "@pop" back button (framework verb, no per-button C++ needed).
			if( MenuCommandRegistry::dispatchControl( control ) )
				return MSG_HANDLED;

			if( control->winGetWindowId() == buttonGraphicsAccept )
			{
				saveGameOptionControls( pref, s_gameOptionRegistry, GAME_OPTION_COUNT, s_graphicsOptionControls );
				if (pref)
				{
					pref->write();
					delete pref;
					pref = nullptr;
				}
				TheShell->pop();
				return MSG_HANDLED;
			}
			break;
		}

		default:
			break;
	}

	return MSG_IGNORED;
}


/*

static NameKeyType    radioHighID = NAMEKEY_INVALID;
static GameWindow *   radioHigh   = nullptr;
static NameKeyType    radioMediumID = NAMEKEY_INVALID;
static GameWindow *   radioMedium   = nullptr;
static NameKeyType    radioLowID = NAMEKEY_INVALID;
static GameWindow *   radioLow   = nullptr;

*/

DisplaySettings oldDispSettings, newDispSettings;
Bool dispChanged = FALSE;
extern Int timer;
extern void DoResolutionDialog();
static Bool ignoreSelected = FALSE;
WindowLayout *OptionsLayout = nullptr;

static void setDefaults()
{
	constexpr const Bool ModifyDisplaySettings = FALSE;

	//-------------------------------------------------------------------------------------------------
	// provider type
//	GadgetCheckBoxSetChecked(checkAudioHardware, FALSE);

	//-------------------------------------------------------------------------------------------------
	// speaker type
//	GadgetCheckBoxSetChecked(checkAudioSurround, FALSE);

	//-------------------------------------------------------------------------------------------------
	// language filter
	GadgetCheckBoxSetChecked( checkLanguageFilter, TRUE );

	if constexpr (ModifyDisplaySettings)
	{
	//-------------------------------------------------------------------------------------------------
	// LOD
	if ((TheGameLogic->isInGame() == FALSE) || (TheGameLogic->isInShellGame() == TRUE))
	{
		GadgetComboBoxSetSelectedPos(comboBoxDetail, (Int)TheGameLODManager->getRecommendedStaticLODLevel());
	}

	//-------------------------------------------------------------------------------------------------
	// Resolution
	//Find index of 800x600 mode.
	if ((TheGameLogic->isInGame() == FALSE || TheGameLogic->isInShellGame() == TRUE) && !TheGameSpyInfo) {
		Int numResolutions = TheDisplay->getDisplayModeCount();
		Int defaultResIndex=0;
		for( Int i = 0; i < numResolutions; ++i )
		{	Int xres,yres,bitDepth;
			TheDisplay->getDisplayModeDescription(i,&xres,&yres,&bitDepth);
			if (xres == DEFAULT_DISPLAY_WIDTH && yres == DEFAULT_DISPLAY_HEIGHT)	//keep track of default mode in case we need it.
			{	defaultResIndex=i;
				break;
			}
		}
		GadgetComboBoxSetSelectedPos( comboBoxResolution, defaultResIndex );	//should be 800x600 (our lowest supported mode)
	}
	}

	//-------------------------------------------------------------------------------------------------
	// Mouse Mode
	GadgetCheckBoxSetChecked(checkAlternateMouse, FALSE);
	GadgetCheckBoxSetChecked(checkRetaliation, TRUE );
	GadgetCheckBoxSetChecked( checkDoubleClickAttackMove, FALSE );

	//-------------------------------------------------------------------------------------------------
//	// scroll speed val
	Int scrollPos = (Int)(TheGlobalData->m_keyboardDefaultScrollFactor*100.0f);
	GadgetSliderSetPosition( sliderScrollSpeed, scrollPos );


	Int valMin, valMax;

	//-------------------------------------------------------------------------------------------------
	// slider music volume
	GadgetSliderGetMinMax(sliderMusicVolume,&valMin, &valMax);
	GadgetSliderSetPosition(sliderMusicVolume,REAL_TO_INT(TheAudio->getAudioSettings()->m_defaultMusicVolume * 100.0f));

	//-------------------------------------------------------------------------------------------------
	// slider SFX volume
	GadgetSliderGetMinMax(sliderSFXVolume,&valMin, &valMax);
	Real maxVolume = MAX( TheAudio->getAudioSettings()->m_defaultSoundVolume, TheAudio->getAudioSettings()->m_default3DSoundVolume );
	GadgetSliderSetPosition( sliderSFXVolume, REAL_TO_INT( maxVolume * 100.0f ) );

	//-------------------------------------------------------------------------------------------------
	// slider Voice volume
	GadgetSliderGetMinMax(sliderVoiceVolume,&valMin, &valMax);
	GadgetSliderSetPosition(sliderVoiceVolume, REAL_TO_INT(TheAudio->getAudioSettings()->m_defaultSpeechVolume * 100.0f));

	//-------------------------------------------------------------------------------------------------
 	// slider Gamma
 	GadgetSliderGetMinMax(sliderGamma,&valMin, &valMax);
 	GadgetSliderSetPosition(sliderGamma, ((valMax - valMin) / 2 + valMin));

	if constexpr (ModifyDisplaySettings)
	{
	if ((TheGameLogic->isInGame() == FALSE) || (TheGameLogic->isInShellGame() == TRUE))
	{
		//-------------------------------------------------------------------------------------------------
		// Texture resolution slider
		//
		Int	txtFact=TheGameLODManager->getRecommendedTextureReduction();

		GadgetSliderSetPosition( sliderTextureResolution, 2-txtFact);

		//-------------------------------------------------------------------------------------------------
 		// 3D Shadows checkbox
		//
		GadgetCheckBoxSetChecked( check3DShadows, TheGlobalData->m_useShadowVolumes);

		//-------------------------------------------------------------------------------------------------
 		// 2D Shadows checkbox
		//
		GadgetCheckBoxSetChecked( check2DShadows, TheGlobalData->m_useShadowDecals);

		//-------------------------------------------------------------------------------------------------
 		// Cloud Shadows checkbox
		//
		GadgetCheckBoxSetChecked( checkCloudShadows, TheGlobalData->m_useCloudMap);

		//-------------------------------------------------------------------------------------------------
 		// Ground Lighting (lightmap) checkbox
		//
		GadgetCheckBoxSetChecked( checkGroundLighting, TheGlobalData->m_useLightMap);

		//-------------------------------------------------------------------------------------------------
 		// Smooth Water Border checkbox
		//
		GadgetCheckBoxSetChecked( checkSmoothWater, TheGlobalData->m_showSoftWaterEdge);

		//-------------------------------------------------------------------------------------------------
 		// Extra Animations (tree sway and buildups) checkbox
		//
		GadgetCheckBoxSetChecked( checkExtraAnimations, !TheGlobalData->m_useDrawModuleLOD);

		//-------------------------------------------------------------------------------------------------
 		// DisableDynamicLOD
		//
		GadgetCheckBoxSetChecked( checkNoDynamicLod, !TheGlobalData->m_enableDynamicLOD);

		//-------------------------------------------------------------------------------------------------
 		// Disable FPS Limit
		//
		GadgetCheckBoxSetChecked( checkUnlockFps, !TheGlobalData->m_useFpsLimit);

		//-------------------------------------------------------------------------------------------------
 		// Heat Effects
		//
		GadgetCheckBoxSetChecked( checkHeatEffects, TheGlobalData->m_useHeatEffects);

		//-------------------------------------------------------------------------------------------------
 		// Building Occlusion checkbox
		//
		GadgetCheckBoxSetChecked( checkBuildingOcclusion, TheGlobalData->m_enableBehindBuildingMarkers);

		//-------------------------------------------------------------------------------------------------
 		// Particle Cap slider
		//
		GadgetSliderSetPosition( sliderParticleCap, TheGlobalData->m_maxParticleCount);

		//-------------------------------------------------------------------------------------------------
 		// Trees and Shrubs
		//
		GadgetCheckBoxSetChecked( checkProps, TheGlobalData->m_useTrees);
	}
	}
}

static void saveOptions()
{
	Int index;
	Int val;
	//-------------------------------------------------------------------------------------------------
//	// provider type
//	Bool isChecked = GadgetCheckBoxIsChecked(checkAudioHardware);
//	TheAudio->setHardwareAccelerated(isChecked);
//	(*pref)["3DAudioProvider"] = TheAudio->getProviderName(TheAudio->getSelectedProvider());
//
	//-------------------------------------------------------------------------------------------------
//	// speaker type
	//	isChecked = GadgetCheckBoxIsChecked(checkAudioSurround);
	//	TheAudio->setSpeakerSurround(isChecked);
	//	(*pref)["SpeakerType"] = TheAudio->translateUnsignedIntToSpeakerType(TheAudio->getSpeakerType());
	//
	//-------------------------------------------------------------------------------------------------
	// language filter
	if( GadgetCheckBoxIsChecked( checkLanguageFilter ) )
	{
			//GadgetCheckBoxSetChecked( checkLanguageFilter, true);
			TheWritableGlobalData->m_languageFilterPref = true;
			(*pref)["LanguageFilter"] = "true";
	}
	else
	{
			//GadgetCheckBoxSetChecked( checkLanguageFilter, false);
			TheWritableGlobalData->m_languageFilterPref = false;
			(*pref)["LanguageFilter"] = "false";
	}


	//-------------------------------------------------------------------------------------------------
	// Custom game detail settings.
	GadgetComboBoxGetSelectedPos( comboBoxDetail, &index );
	if (index == STATIC_GAME_LOD_CUSTOM)
	{
 		//-------------------------------------------------------------------------------------------------
 		// Texture resolution slider
		{
		 		val = 2 - GadgetSliderGetPosition(sliderTextureResolution);

				AsciiString prefString;
				prefString.format("%d",val);
				(*pref)["TextureReduction"] = prefString;

				TheWritableGlobalData->m_textureReductionFactor = val;
				TheGameClient->setTextureLOD(val);
		}

		TheWritableGlobalData->m_useShadowVolumes = GadgetCheckBoxIsChecked( check3DShadows );
		(*pref)["UseShadowVolumes"] = TheWritableGlobalData->m_useShadowVolumes ? "yes" : "no";

		TheWritableGlobalData->m_useShadowDecals = GadgetCheckBoxIsChecked( check2DShadows );
		(*pref)["UseShadowDecals"] = TheWritableGlobalData->m_useShadowDecals ? "yes" : "no";

		TheWritableGlobalData->m_useCloudMap = GadgetCheckBoxIsChecked( checkCloudShadows );
		(*pref)["UseCloudMap"] = TheGlobalData->m_useCloudMap ? "yes" : "no";

		TheWritableGlobalData->m_useLightMap = GadgetCheckBoxIsChecked( checkGroundLighting );
		(*pref)["UseLightMap"] = TheGlobalData->m_useLightMap ? "yes" : "no";

		TheWritableGlobalData->m_showSoftWaterEdge = GadgetCheckBoxIsChecked( checkSmoothWater );
		(*pref)["ShowSoftWaterEdge"] = TheGlobalData->m_showSoftWaterEdge ? "yes" : "no";

		TheWritableGlobalData->m_useDrawModuleLOD = !GadgetCheckBoxIsChecked( checkExtraAnimations );
		TheWritableGlobalData->m_useTreeSway = !TheWritableGlobalData->m_useDrawModuleLOD;	//borrow same setting.
		(*pref)["ExtraAnimations"] = TheGlobalData->m_useDrawModuleLOD ? "no" : "yes";

		TheWritableGlobalData->m_enableDynamicLOD = !GadgetCheckBoxIsChecked( checkNoDynamicLod );
		(*pref)["DynamicLOD"] = TheGlobalData->m_enableDynamicLOD ? "yes" : "no";

		TheWritableGlobalData->m_useHeatEffects = GadgetCheckBoxIsChecked( checkHeatEffects );
		(*pref)["HeatEffects"] = TheGlobalData->m_useHeatEffects ? "yes" : "no";

		// Never write this out
		//TheWritableGlobalData->m_useFpsLimit = !GadgetCheckBoxIsChecked( checkUnlockFps );
		//(*pref)["FPSLimit"] = TheGlobalData->m_useFpsLimit ? "yes" : "no";

		TheWritableGlobalData->m_enableBehindBuildingMarkers = GadgetCheckBoxIsChecked( checkBuildingOcclusion );
		(*pref)["BuildingOcclusion"] = TheWritableGlobalData->m_enableBehindBuildingMarkers ? "yes" : "no";

		TheWritableGlobalData->m_useTrees = GadgetCheckBoxIsChecked( checkProps);
		(*pref)["ShowTrees"] = TheWritableGlobalData->m_useTrees ? "yes" : "no";

 		//-------------------------------------------------------------------------------------------------
		// Particle Cap slider
		{
				AsciiString prefString;

		 		val = GadgetSliderGetPosition(sliderParticleCap);

				prefString.format("%d",val);
				(*pref)["MaxParticleCount"] = prefString;

				TheWritableGlobalData->m_maxParticleCount = val;
		}
	}

	//-------------------------------------------------------------------------------------------------
	// GeneralsX @refactor Qoder 13/10/2026 Registry saving now belongs to the standalone
	// GraphicsOptions.wnd page only; its .wnd gadgets were removed from this layout, and the
	// startup application of saved keys (ApplyGameOptionRegistryAtStartup) is unchanged.

	//-------------------------------------------------------------------------------------------------
	// LOD
	if (comboBoxDetail && comboBoxDetail->winGetEnabled())
	{
		GadgetComboBoxGetSelectedPos( comboBoxDetail, &index );
		const Bool levelChanged = TheGameLODManager->setStaticLODLevel((StaticGameLODLevel)index);

		if (levelChanged)
			(*pref)["StaticGameLOD"] = TheGameLODManager->getStaticGameLODLevelName(TheGameLODManager->getStaticLODLevel());
	}

	//-------------------------------------------------------------------------------------------------
	// IP address
	if (comboBoxLANIP && comboBoxLANIP->winGetEnabled())
	{
		UnsignedInt ip;
		GadgetComboBoxGetSelectedPos(comboBoxLANIP, &index);
		if (index>=0 && TheGlobalData)
		{
			// GeneralsX @bugfix BenderAI 12/02/2026 - Cast via uintptr_t for 64-bit
			ip = static_cast<UnsignedInt>(reinterpret_cast<uintptr_t>(GadgetComboBoxGetItemData(comboBoxLANIP, index)));
			TheWritableGlobalData->m_defaultIP = ip;
			pref->setLANIPAddress(ip);
		}
	}

	if (comboBoxOnlineIP && comboBoxOnlineIP->winGetEnabled())
	{
		UnsignedInt ip;
		GadgetComboBoxGetSelectedPos(comboBoxOnlineIP, &index);
		if (index>=0)
		{
			// GeneralsX @bugfix BenderAI 12/02/2026 - Cast via uintptr_t for 64-bit
			ip = static_cast<UnsignedInt>(reinterpret_cast<uintptr_t>(GadgetComboBoxGetItemData(comboBoxOnlineIP, index)));
			pref->setOnlineIPAddress(ip);
		}
	}

	//-------------------------------------------------------------------------------------------------
	// Firewall Port Override
	GameWindow *textEntryFirewallPortOverride = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("OptionsMenu.wnd:TextEntryFirewallPortOverride"));
	if (textEntryFirewallPortOverride && textEntryFirewallPortOverride->winGetEnabled())
	{
		UnicodeString uStr = GadgetTextEntryGetText(textEntryFirewallPortOverride);
		AsciiString aStr;
		aStr.translate(uStr);
		Int portOverride = atoi(aStr.str());
		if (portOverride < 0 || portOverride > 65535)
			portOverride = 0;
		if (TheGlobalData->m_firewallPortOverride != portOverride)
		{	TheWritableGlobalData->m_firewallPortOverride = portOverride;
		    aStr.format("%d", portOverride);
			(*pref)["FirewallPortOverride"] = aStr;
		}
	}

	//-------------------------------------------------------------------------------------------------
	// texture filter mode
	val = pref->getTextureFilterMode();
	if (val >= 0)
	{
		val = clamp((int)TextureFilterClass::TEXTURE_FILTER_NONE, val, (int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC);

		TheWritableGlobalData->m_textureFilteringMode = val;
		AsciiString prefString;
		prefString = TextureFilterClass::TextureFilterModeString[val];
		(*pref)["TextureFilter"] = prefString;
	}

	//-------------------------------------------------------------------------------------------------
	// anisotropy level
	val = pref->getTextureAnisotropyLevel();
	if (val >= 0)
	{
		val = clamp((int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_2X, val, (int)TextureFilterClass::TEXTURE_FILTER_ANISOTROPIC_16X);

		TheWritableGlobalData->m_textureAnisotropyLevel = val;
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["AnisotropyLevel"] = prefString;
	}

	//-------------------------------------------------------------------------------------------------
	// mouse mode
	TheWritableGlobalData->m_useAlternateMouse = GadgetCheckBoxIsChecked(checkAlternateMouse);
	(*pref)["UseAlternateMouse"] = TheWritableGlobalData->m_useAlternateMouse ? "yes" : "no";

	// TheSuperHackers @todo Add check box ?
	{
		Bool useRightMouseScrollWithAlternateMouse = pref->getRightMouseScrollWithAlternateMouseEnabled();
		(*pref)["UseRightMouseScrollWithAlternateMouse"] = useRightMouseScrollWithAlternateMouse ? "yes" : "no";
		TheWritableGlobalData->m_useRightMouseScrollWithAlternateMouse = useRightMouseScrollWithAlternateMouse;
	}

	TheWritableGlobalData->m_clientRetaliationModeEnabled = GadgetCheckBoxIsChecked(checkRetaliation);
	(*pref)["Retaliation"] = TheWritableGlobalData->m_clientRetaliationModeEnabled? "yes" : "no";

	TheWritableGlobalData->m_doubleClickAttackMove = GadgetCheckBoxIsChecked( checkDoubleClickAttackMove );
	(*pref)["UseDoubleClickAttackMove"] = TheWritableGlobalData->m_doubleClickAttackMove ? "yes" : "no";

	// TheSuperHackers @todo Add combo box ?
	{
		CursorCaptureMode mode = pref->getCursorCaptureMode();
		(*pref)["CursorCaptureEnabledInWindowedGame"] = (mode & CursorCaptureMode_EnabledInWindowedGame) ? "yes" : "no";
		(*pref)["CursorCaptureEnabledInWindowedMenu"] = (mode & CursorCaptureMode_EnabledInWindowedMenu) ? "yes" : "no";
		(*pref)["CursorCaptureEnabledInFullscreenGame"] = (mode & CursorCaptureMode_EnabledInFullscreenGame) ? "yes" : "no";
		(*pref)["CursorCaptureEnabledInFullscreenMenu"] = (mode & CursorCaptureMode_EnabledInFullscreenMenu) ? "yes" : "no";
		TheMouse->setCursorCaptureMode(mode);
	}

	// TheSuperHackers @todo Add combo box ?
	{
		ScreenEdgeScrollMode mode = pref->getScreenEdgeScrollMode();
		(*pref)["ScreenEdgeScrollEnabledInWindowedApp"] = (mode & ScreenEdgeScrollMode_EnabledInWindowedApp) ? "yes" : "no";
		(*pref)["ScreenEdgeScrollEnabledInFullscreenApp"] = (mode & ScreenEdgeScrollMode_EnabledInFullscreenApp) ? "yes" : "no";
		TheLookAtTranslator->setScreenEdgeScrollMode(mode);
	}

	// TheSuperHackers @todo Add checkbox ?
	{
		Bool enabled = pref->getPlayerObserverEnabled();
		(*pref)["PlayerObserverEnabled"] = enabled ? "yes" : "no";
		TheWritableGlobalData->m_enablePlayerObserver = enabled;
	}

	// TheSuperHackers @todo Add checkbox ?
	{
		Bool enabled = pref->getArchiveReplaysEnabled();
		(*pref)["ArchiveReplays"] = enabled ? "yes" : "no";
		TheRecorder->setArchiveEnabled(enabled);
	}

	//-------------------------------------------------------------------------------------------------
	// scroll speed val
	val = GadgetSliderGetPosition(sliderScrollSpeed);
	if(val > 0)
	{
		TheWritableGlobalData->m_keyboardScrollFactor = val/100.0f;
		DEBUG_LOG(("Scroll Speed val %d, keyboard scroll factor %f", val, TheGlobalData->m_keyboardScrollFactor));
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["ScrollFactor"] = prefString;
	}

	//-------------------------------------------------------------------------------------------------
	// draw scroll anchor
	{
		if( TheInGameUI->getDrawRMBScrollAnchor() )
				(*pref)["DrawScrollAnchor"] = "yes";
		else
				(*pref)["DrawScrollAnchor"] = "no";
	}

	//-------------------------------------------------------------------------------------------------
	// move scroll anchor
	{
		if( TheInGameUI->getMoveRMBScrollAnchor() )
				(*pref)["MoveScrollAnchor"] = "yes";
		else
				(*pref)["MoveScrollAnchor"] = "no";
	}

	//-------------------------------------------------------------------------------------------------
	// slider music volume
	val = GadgetSliderGetPosition(sliderMusicVolume);
	if(val != -1)
	{
    AsciiString prefString;
    prefString.format("%d", val);
    (*pref)["MusicVolume"] = prefString;
    TheAudio->setVolume(val / 100.0f, (AudioAffect) (AudioAffect_Music | AudioAffect_SystemSetting));
	}

	//-------------------------------------------------------------------------------------------------
	// slider SFX volume
	val = GadgetSliderGetPosition(sliderSFXVolume);
	if(val != -1)
	{
		//Both 2D and 3D sound effects are sharing the same slider. However, there is a
		//relative slider that gets applied to one of these values to lower that sound volume.
		Real sound2DVolume = val / 100.0f;
		Real sound3DVolume = val / 100.0f;
		Real relative2DVolume = TheAudio->getAudioSettings()->m_relative2DVolume;
		relative2DVolume = MIN( 1.0f, MAX( -1.0, relative2DVolume ) );
		if( relative2DVolume < 0.0f )
		{
			//Lower the 2D volume
			sound2DVolume *= 1.0f + relative2DVolume;
		}
		else
		{
			//Lower the 3D volume
			sound3DVolume *= 1.0f - relative2DVolume;
		}

		//Apply the sound volumes in the audio system now.
    TheAudio->setVolume( sound2DVolume, (AudioAffect) (AudioAffect_Sound | AudioAffect_SystemSetting) );
		TheAudio->setVolume( sound3DVolume, (AudioAffect) (AudioAffect_Sound3D | AudioAffect_SystemSetting) );

		//Save the settings in the options.ini.
    AsciiString prefString;
    prefString.format("%d", REAL_TO_INT( sound2DVolume * 100.0f ) );
    (*pref)["SFXVolume"] = prefString;
    prefString.format("%d", REAL_TO_INT( sound3DVolume * 100.0f ) );
		(*pref)["SFX3DVolume"] = prefString;
	}

	//-------------------------------------------------------------------------------------------------
	// slider Voice volume
	val = GadgetSliderGetPosition(sliderVoiceVolume);
	if(val != -1)
	{
    AsciiString prefString;
    prefString.format("%d", val);
    (*pref)["VoiceVolume"] = prefString;
    TheAudio->setVolume(val / 100.0f, (AudioAffect) (AudioAffect_Speech | AudioAffect_SystemSetting));
	}

	//-------------------------------------------------------------------------------------------------
	// Money tick volume
	// TheSuperHackers @todo Add options slider ?
	{
		val = pref->getMoneyTransactionVolume();
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["MoneyTransactionVolume"] = prefString;
		TheAudio->friend_getAudioSettings()->m_preferredMoneyTransactionVolume = val / 100.0f;
	}

 	//-------------------------------------------------------------------------------------------------
 	// slider Gamma
 	val = GadgetSliderGetPosition(sliderGamma);
 	if(val != -1)
 	{
		Real gammaval=1.0f;
		//generate a value between 0.6 and 2.0.
		if (val < 50)
		{
			//darker gamma
			if (val <= 0)
				gammaval = 0.6f;
			else
				gammaval=1.0f-(0.4f) * (Real)(50-val)/50.0f;
		}
		else
		if (val > 50)
			gammaval=1.0f+(1.0f) * (Real)(val-50)/50.0f;

 		AsciiString prefString;
 		prefString.format("%d", val);
 		(*pref)["Gamma"] = prefString;

		if (TheGlobalData->m_displayGamma != gammaval)
		{	TheWritableGlobalData->m_displayGamma = gammaval;
			TheDisplay->setGamma(TheGlobalData->m_displayGamma,0.0f, 1.0f, FALSE);
		}
 	}

	//-------------------------------------------------------------------------------------------------
	// Set Network Latency Font Size
	val = pref->getNetworkLatencyFontSize();
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["NetworkLatencyFontSize"] = prefString;
		TheInGameUI->refreshNetworkLatencyResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set Render FPS Font Size
	val = pref->getRenderFpsFontSize();
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["RenderFpsFontSize"] = prefString;
		TheInGameUI->refreshRenderFpsResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set System Time Font Size
	val = pref->getSystemTimeFontSize(); // TheSuperHackers @todo replace with options input when applicable
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["SystemTimeFontSize"] = prefString;
		TheInGameUI->refreshSystemTimeResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set Game Time Font Size
	val = pref->getGameTimeFontSize(); // TheSuperHackers @todo replace with options input when applicable
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["GameTimeFontSize"] = prefString;
		TheInGameUI->refreshGameTimeResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set Player Info List Font Size
	val = pref->getPlayerInfoListFontSize();
	if (val >= 0)
	{
		AsciiString prefString;
		prefString.format("%d", val);
		(*pref)["PlayerInfoListFontSize"] = prefString;
		TheInGameUI->refreshPlayerInfoListResources();
	}

	//-------------------------------------------------------------------------------------------------
	// Set User Font Scaling Percentage
	val = pref->getResolutionFontAdjustment() * 100.0f; // TheSuperHackers @todo replace with options input when applicable
	if (val >= 0 || val == -100)
	{
		AsciiString prefString;
		prefString.format("%d", REAL_TO_INT( val ) );
		(*pref)["ResolutionFontAdjustment"] = prefString;
		TheGlobalLanguageData->m_userResolutionFontSizeAdjustment = (Real)val / 100.0f;
	}

	//-------------------------------------------------------------------------------------------------
	// Set Money Per Minute
	{
		Bool show = pref->getShowMoneyPerMinute();
		AsciiString prefString;
		prefString = show ? "yes" : "no";
		(*pref)["ShowMoneyPerMinute"] = prefString;
		TheWritableGlobalData->m_showMoneyPerMinute = show;
	}

	//-------------------------------------------------------------------------------------------------
	// Set Game Window Transition Speed Multiplier
	{
		Real speed = pref->getGameWindowTransitionSpeedMultiplier();
		AsciiString prefString;
		prefString.format("%g", speed);
		(*pref)["GameWindowTransitionSpeedMultiplier"] = prefString;
		TheWritableGlobalData->m_gameWindowTransitionSpeedMultiplier = speed;
	}

	//-------------------------------------------------------------------------------------------------
	// Set JPEG screenshot quality
	{
		Int quality = pref->getJpegQuality();
		AsciiString prefString;
		prefString.format("%d", quality);
		(*pref)["JpegQuality"] = prefString;
		TheWritableGlobalData->m_jpegQuality = quality;
	}

	//-------------------------------------------------------------------------------------------------
	// Resolution
	//
	// TheSuperHackers @bugfix xezon 12/06/2025 Now performs the resolution change at the very end of
	// processing all the options. This is necessary, because recreating the Shell will destroy the
	// Options Menu and therefore prevent any further ui gadget interactions afterwards.

	GadgetComboBoxGetSelectedPos( comboBoxResolution, &index );
	Int xres, yres, bitDepth;

	oldDispSettings.xRes = TheDisplay->getWidth();
	oldDispSettings.yRes = TheDisplay->getHeight();
	oldDispSettings.bitDepth = TheDisplay->getBitDepth();
	oldDispSettings.windowed = TheDisplay->getWindowed();

	if (comboBoxResolution && comboBoxResolution->winGetEnabled() && index < TheDisplay->getDisplayModeCount() && index >= 0)
	{
		TheDisplay->getDisplayModeDescription(index,&xres,&yres,&bitDepth);
		if (TheGlobalData->m_xResolution != xres || TheGlobalData->m_yResolution != yres)
		{
			if (TheDisplay->setDisplayMode(xres,yres,bitDepth,TheDisplay->getWindowed()))
			{
				dispChanged = TRUE;
				TheWritableGlobalData->m_xResolution = xres;
				TheWritableGlobalData->m_yResolution = yres;

				TheHeaderTemplateManager->onResolutionChanged();
				TheMouse->onResolutionChanged();

				//Save new settings for a dialog box confirmation after options are accepted
				newDispSettings.xRes = xres;
				newDispSettings.yRes = yres;
				newDispSettings.bitDepth = bitDepth;
				newDispSettings.windowed = TheDisplay->getWindowed();

				AsciiString prefString;
				prefString.format("%d %d", xres, yres );
				(*pref)["Resolution"] = prefString;

				TheShell->recreateWindowLayouts();

				TheInGameUI->recreateControlBar();
				TheInGameUI->refreshCustomUiResources();

				// TheSuperHackers @info Only update the camera limits and set the zoom to max to not interfere with the scripted camera on the shellmap
				// The tactical view gets reset at game start, this is here so the shell map looks correct once the resolution is adjusted
				TheTacticalView->setCameraHeightAboveGroundLimitsToDefault();
				TheTacticalView->setZoomToMax();
			}
		}
	}

	// MUST NEVER ADD ANOTHER OPTION HERE AT THE END !
}

static void DestroyOptionsLayout() {

	SignalUIInteraction(SHELL_SCRIPT_HOOK_OPTIONS_CLOSED);

	TheShell->destroyOptionsLayout();
	OptionsLayout = nullptr;
}

static void showAdvancedOptions()
{
	WinAdvancedDisplay->winHide(FALSE);
}

static void acceptAdvancedOptions()
{
	WinAdvancedDisplay->winHide(TRUE);
}

static void cancelAdvancedOptions()
{
	//restore the detail selection back to initial state
	GadgetComboBoxSetSelectedPos(comboBoxDetail, (Int)TheGameLODManager->getStaticLODLevel());

	WinAdvancedDisplay->winHide(TRUE);
}

// TheSuperHackers @tweak Now prints additional version information in the version label.
static void initLabelVersion()
{
	NameKeyType versionID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:LabelVersion" );
	GameWindow *labelVersion = TheWindowManager->winGetWindowFromId( nullptr, versionID );

	if (labelVersion)
	{
		if (TheVersion && TheGlobalData)
		{
			UnicodeString text = TheVersion->getUnicodeProductVersionHashString();
			GadgetStaticTextSetText( labelVersion, text );
		}
		else
		{
			labelVersion->winHide( TRUE );
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** Initialize the options menu */
//-------------------------------------------------------------------------------------------------
void OptionsMenuInit( WindowLayout *layout, void *userData )
{
	ignoreSelected = TRUE;
	if (TheGameEngine->getQuitting())
		return;
	OptionsLayout = layout;
	if (!pref)
	{
		pref = NEW OptionPreferences;
	}

	SignalUIInteraction(SHELL_SCRIPT_HOOK_OPTIONS_OPENED);

	comboBoxLANIPID				 = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxIP" );
	comboBoxLANIP					 = TheWindowManager->winGetWindowFromId( nullptr,  comboBoxLANIPID);
	comboBoxOnlineIPID		 = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxOnlineIP" );
	comboBoxOnlineIP			 = TheWindowManager->winGetWindowFromId( nullptr,  comboBoxOnlineIPID);
	checkAlternateMouseID  = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckAlternateMouse" );
	checkAlternateMouse	   = TheWindowManager->winGetWindowFromId( nullptr, checkAlternateMouseID);
	checkRetaliationID		 = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:Retaliation" );
	checkRetaliation	     = TheWindowManager->winGetWindowFromId( nullptr, checkRetaliationID);
	checkDoubleClickAttackMoveID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckDoubleClickAttackMove" );
	checkDoubleClickAttackMove   = TheWindowManager->winGetWindowFromId( nullptr, checkDoubleClickAttackMoveID );
	sliderScrollSpeedID	   = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderScrollSpeed" );
	sliderScrollSpeed		   = TheWindowManager->winGetWindowFromId( nullptr,  sliderScrollSpeedID);
	comboBoxResolutionID   = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxResolution" );
	comboBoxResolution     = TheWindowManager->winGetWindowFromId( nullptr, comboBoxResolutionID );
	comboBoxDetailID			 = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ComboBoxDetail" );
	comboBoxDetail		   = TheWindowManager->winGetWindowFromId( nullptr, comboBoxDetailID );

	checkLanguageFilterID  = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckLanguageFilter" );
	checkLanguageFilter    = TheWindowManager->winGetWindowFromId( nullptr, checkLanguageFilterID );
	buttonFirewallRefreshID	= TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonFirewallRefresh" );
	buttonFirewallRefresh		= TheWindowManager->winGetWindowFromId( nullptr, buttonFirewallRefreshID);

#if ENABLE_GUI_HACKS
	// TheSuperHackers @tweak 26/07/2026 The Send Delay feature was obsoleted because it only worked around
	// a source port remapping bug in early 2000s Netgear firewalls. Hide the obsoleted UI element accordingly.
	GameWindow *checkSendDelay = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("OptionsMenu.wnd:CheckSendDelay"));
	if (checkSendDelay)
		checkSendDelay->winHide(TRUE);
#endif

	checkDrawAnchorID       = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBoxDrawAnchor" );
	checkDrawAnchor				 = TheWindowManager->winGetWindowFromId( nullptr, checkDrawAnchorID);
	checkMoveAnchorID       = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBoxMoveAnchor" );
	checkMoveAnchor				 = TheWindowManager->winGetWindowFromId( nullptr, checkMoveAnchorID);

	// Replay camera
	checkSaveCameraID      = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBoxSaveCamera" );
	checkSaveCamera        = TheWindowManager->winGetWindowFromId( nullptr, checkSaveCameraID );
	checkUseCameraID       = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBoxUseCamera" );
	checkUseCamera         = TheWindowManager->winGetWindowFromId( nullptr, checkUseCameraID );

//	// Speakers and 3-D Audio
//	checkAudioSurroundID   = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckAudioSurround" );
//	checkAudioSurround     = TheWindowManager->winGetWindowFromId( nullptr, checkAudioSurroundID );
//	checkAudioHardwareID   = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckAudioHardware" );
//	checkAudioHardware     = TheWindowManager->winGetWindowFromId( nullptr, checkAudioHardwareID );
//
	// Volume Controls
	sliderMusicVolumeID    = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderMusicVolume" );
	sliderMusicVolume      = TheWindowManager->winGetWindowFromId( nullptr, sliderMusicVolumeID );
	sliderSFXVolumeID      = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderSFXVolume" );
	sliderSFXVolume        = TheWindowManager->winGetWindowFromId( nullptr, sliderSFXVolumeID );
	sliderVoiceVolumeID    = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderVoiceVolume" );
	sliderVoiceVolume      = TheWindowManager->winGetWindowFromId( nullptr, sliderVoiceVolumeID );
 	sliderGammaID    = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:SliderGamma" );
 	sliderGamma      = TheWindowManager->winGetWindowFromId( nullptr, sliderGammaID );

//	checkBoxLowTextureDetailID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckLowTextureDetail" );
//	checkBoxLowTextureDetail      = TheWindowManager->winGetWindowFromId( nullptr, checkBoxLowTextureDetailID );

	WinAdvancedDisplayID		= TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:WinAdvancedDisplayOptions" );
	WinAdvancedDisplay      = TheWindowManager->winGetWindowFromId( nullptr, WinAdvancedDisplayID );

	ButtonAdvancedAcceptID		= TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonAdvanceAccept" );
	ButtonAdvancedAccept      = TheWindowManager->winGetWindowFromId( nullptr, ButtonAdvancedAcceptID );

	ButtonAdvancedCancelID		= TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonAdvanceBack" );
	ButtonAdvancedCancel      = TheWindowManager->winGetWindowFromId( nullptr, ButtonAdvancedCancelID );

	sliderTextureResolutionID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:LowResSlider" );
	sliderTextureResolution = TheWindowManager->winGetWindowFromId( nullptr, sliderTextureResolutionID );

	check3DShadowsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:Check3DShadows" );
	check3DShadows   = TheWindowManager->winGetWindowFromId( nullptr, check3DShadowsID);

	check2DShadowsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:Check2DShadows" );
	check2DShadows   = TheWindowManager->winGetWindowFromId( nullptr, check2DShadowsID);

	checkCloudShadowsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckCloudShadows" );
	checkCloudShadows   = TheWindowManager->winGetWindowFromId( nullptr, checkCloudShadowsID);

	checkGroundLightingID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckGroundLighting" );
	checkGroundLighting   = TheWindowManager->winGetWindowFromId( nullptr, checkGroundLightingID);

	checkSmoothWaterID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckSmoothWater" );
	checkSmoothWater   = TheWindowManager->winGetWindowFromId( nullptr, checkSmoothWaterID);

	checkExtraAnimationsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckExtraAnimations" );
	checkExtraAnimations   = TheWindowManager->winGetWindowFromId( nullptr, checkExtraAnimationsID);

	checkNoDynamicLodID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckNoDynamicLOD" );
	checkNoDynamicLod   = TheWindowManager->winGetWindowFromId( nullptr, checkNoDynamicLodID);

	checkHeatEffectsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckHeatEffects" );
	checkHeatEffects   = TheWindowManager->winGetWindowFromId( nullptr, checkHeatEffectsID);

	checkUnlockFpsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckUnlockFPS" );
	checkUnlockFps   = TheWindowManager->winGetWindowFromId( nullptr, checkUnlockFpsID);

	// GeneralsX @refactor Qoder 13/10/2026 The graphics registry rows moved to the standalone
	// GraphicsOptions.wnd page; the retail options menu keeps only its original hand-written
	// controls and no longer runs the registry gadget loops (their .wnd gadgets were removed).

	checkBuildingOcclusionID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckBehindBuilding" );
	checkBuildingOcclusion   = TheWindowManager->winGetWindowFromId( nullptr, checkBuildingOcclusionID);

	checkPropsID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:CheckShowProps" );
	checkProps   = TheWindowManager->winGetWindowFromId( nullptr, checkPropsID);

	sliderParticleCapID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ParticleCapSlider" );
  sliderParticleCap = TheWindowManager->winGetWindowFromId( nullptr, sliderParticleCapID );

	WinAdvancedDisplay->winHide(TRUE);

	Color color =  GameMakeColor(255,255,255,255);

	initLabelVersion();

	// Choose an IP address, then initialize the IP combo box
	UnsignedInt selectedIP = pref->getLANIPAddress();

	UnicodeString str;
	IPEnumeration IPs;
	EnumeratedIP *IPlist = IPs.getAddresses();
	Int index;
	Int selectedIndex = -1;
	Int count = 0;
	GadgetComboBoxReset(comboBoxLANIP);
	while (IPlist)
	{
		count++;
		str.translate(IPlist->getIPstring());
		index = GadgetComboBoxAddEntry(comboBoxLANIP, str, color);
		GadgetComboBoxSetItemData(comboBoxLANIP, index, (void *)(IPlist->getIP()));
		if (selectedIP == IPlist->getIP())
		{
			selectedIndex = index;
		}
		IPlist = IPlist->getNext();
	}
	if (selectedIndex >= 0)
	{
		GadgetComboBoxSetSelectedPos(comboBoxLANIP, selectedIndex);
	}
	else
	{
		GadgetComboBoxSetSelectedPos(comboBoxLANIP, 0);
		if (IPs.getAddresses())
		{
			pref->setLANIPAddress(IPs.getAddresses()->getIPstring());
		}
	}

	// And now the GameSpy one
	if (comboBoxOnlineIP)
	{
		UnsignedInt selectedIP = pref->getOnlineIPAddress();
		UnicodeString str;
		IPEnumeration IPs;
		EnumeratedIP *IPlist = IPs.getAddresses();
		Int index;
		Int selectedIndex = -1;
		Int count = 0;
		GadgetComboBoxReset(comboBoxOnlineIP);
		while (IPlist)
		{
			count++;
			str.translate(IPlist->getIPstring());
			index = GadgetComboBoxAddEntry(comboBoxOnlineIP, str, color);
			GadgetComboBoxSetItemData(comboBoxOnlineIP, index, (void *)(IPlist->getIP()));
			if (selectedIP == IPlist->getIP())
			{
				selectedIndex = index;
			}
			IPlist = IPlist->getNext();
		}
		if (selectedIndex >= 0)
		{
			GadgetComboBoxSetSelectedPos(comboBoxOnlineIP, selectedIndex);
		}
		else
		{
			GadgetComboBoxSetSelectedPos(comboBoxOnlineIP, 0);
			if (IPs.getAddresses())
			{
				pref->setOnlineIPAddress(IPs.getAddresses()->getIPstring());
			}
		}
	}

#if ENABLE_GUI_HACKS
	// TheSuperHackers @tweak 26/07/2026 The http proxy feature was obsoleted because it did nothing for the UDP game traffic or match sockets.
	// Hide the relevant obsoleted UI elements accordingly.
	NameKeyType textEntryHTTPProxyID = TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:TextEntryHTTPProxy");
	GameWindow *textEntryHTTPProxy = TheWindowManager->winGetWindowFromId(nullptr, textEntryHTTPProxyID);
	if (textEntryHTTPProxy)
		textEntryHTTPProxy->winHide(TRUE);

	NameKeyType staticTextHTTPProxyID = TheNameKeyGenerator->nameToKey("OptionsMenu.wnd:StaticTextHTTPProxy");
	GameWindow *staticTextHTTPProxy = TheWindowManager->winGetWindowFromId(nullptr, staticTextHTTPProxyID);
	if (staticTextHTTPProxy)
		staticTextHTTPProxy->winHide(TRUE);
#endif

	// Firewall Port Override
	GameWindow *textEntryFirewallPortOverride = TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("OptionsMenu.wnd:TextEntryFirewallPortOverride"));
	if (textEntryFirewallPortOverride)
	{
			UnicodeString uStr;
			if (TheGlobalData->m_firewallPortOverride != 0)
			{	AsciiString aStr;
				aStr.format("%d",TheGlobalData->m_firewallPortOverride);
				uStr.translate(aStr);
			}
			GadgetTextEntrySetText(textEntryFirewallPortOverride,uStr);
	}

	// GeneralsX @refactor Qoder 12/10/2026 Anti aliasing entry building and refill now happens in
	// initGameOptionControls via the options registry.

	// get resolution from saved preferences file
	AsciiString selectedResolution = (*pref) ["Resolution"];
	Int selectedXRes=DEFAULT_DISPLAY_WIDTH;
	Int selectedYRes=DEFAULT_DISPLAY_HEIGHT;
	Int selectedResIndex=-1;
	if (!selectedResolution.isEmpty())
	{
		//try to parse 2 integers out of string
		if (sscanf(selectedResolution.str(),"%d%d", &selectedXRes, &selectedYRes) != 2)
		{
			selectedXRes=DEFAULT_DISPLAY_WIDTH;
			selectedYRes=DEFAULT_DISPLAY_HEIGHT;
		}
	}

	// populate resolution modes
	Int i = 0; // GeneralsX @refactor Qoder 12/10/2026 was declared in the removed anti aliasing block
	GadgetComboBoxReset(comboBoxResolution);
	Int numResolutions = TheDisplay->getDisplayModeCount();
	UnsignedInt displayWidth = TheDisplay->getWidth();
	UnsignedInt displayHeight = TheDisplay->getHeight();

	for( i = 0; i < numResolutions; ++i )
	{	Int xres,yres,bitDepth;
		TheDisplay->getDisplayModeDescription(i,&xres,&yres,&bitDepth);
		str.format(L"%d x %d",xres,yres);
		GadgetComboBoxAddEntry( comboBoxResolution, str, color);
		// TheSuperHackers @bugfix xezon 12/06/2025 Now makes a selection with the active display resolution
		// instead of the resolution read from the Option Preferences, because the active display resolution
		// is the most relevant to make a selection with and the Option Preferences could be wrong.
		if ( xres == displayWidth && yres == displayHeight )
			selectedResIndex=i;
	}

	if (selectedResIndex == -1)
	{
		// TheSuperHackers @bugfix xezon 08/06/2025 Now adds the current resolution instead of defaulting to 800 x 600.
		// This avoids force changing the resolution when the user has set a custom resolution in the Option Preferences.
		Int xres = displayWidth;
		Int yres = displayHeight;
		str.format(L"%d x %d",xres,yres);
		GadgetComboBoxAddEntry( comboBoxResolution, str, color );
		selectedResIndex = GadgetComboBoxGetLength( comboBoxResolution ) - 1;
	}

	GadgetComboBoxSetSelectedPos( comboBoxResolution, selectedResIndex );

	// set the display detail
	// TheSuperHackers @tweak xezon 24/09/2025 The Detail Combo Box now has the same value order as StaticGameLODLevel for simplicity.
	// TheSuperHackers @feature xezon 24/09/2025 The Detail Combo Box now has a new options for "Very High".
	GadgetComboBoxReset(comboBoxDetail);
#if ENABLE_GUI_HACKS
	// TheSuperHackers @tweak xezon 24/09/2025 Show max 4 rows because with the original layout it cannot possibly show 5.
	GadgetComboBoxSetMaxDisplay(comboBoxDetail, 4);
#endif
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->fetch("GUI:Low"), color);
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->fetch("GUI:Medium"), color);
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->fetch("GUI:High"), color);
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->FETCH_OR_SUBSTITUTE("GUI:VeryHigh", L"Very High"), color);
	GadgetComboBoxAddEntry(comboBoxDetail, TheGameText->fetch("GUI:Custom"), color);
	static_assert(STATIC_GAME_LOD_COUNT == 5, "Wrong combo box count");

	//Check if level was never set and default to setting most suitable for system.
	if (TheGameLODManager->getStaticLODLevel() == STATIC_GAME_LOD_UNKNOWN)
	{
		TheGameLODManager->setStaticLODLevel(TheGameLODManager->getRecommendedStaticLODLevel());
	}

	GadgetComboBoxSetSelectedPos(comboBoxDetail, (Int)TheGameLODManager->getStaticLODLevel());

	GadgetSliderSetPosition( sliderTextureResolution, 2-WW3D::Get_Texture_Reduction());

	GadgetCheckBoxSetChecked( check3DShadows, TheGlobalData->m_useShadowVolumes);

	GadgetCheckBoxSetChecked( check2DShadows, TheGlobalData->m_useShadowDecals);

	GadgetCheckBoxSetChecked( checkCloudShadows, TheGlobalData->m_useCloudMap);

	GadgetCheckBoxSetChecked( checkGroundLighting, TheGlobalData->m_useLightMap);

	GadgetCheckBoxSetChecked( checkSmoothWater, TheGlobalData->m_showSoftWaterEdge);

	GadgetCheckBoxSetChecked( checkExtraAnimations, !TheGlobalData->m_useDrawModuleLOD);

	GadgetCheckBoxSetChecked( checkNoDynamicLod, !TheGlobalData->m_enableDynamicLOD);

	GadgetCheckBoxSetChecked( checkHeatEffects, TheGlobalData->m_useHeatEffects);

	GadgetCheckBoxSetChecked( checkUnlockFps, !TheGlobalData->m_useFpsLimit);

	// GeneralsX @refactor Qoder 12/10/2026 Normal mapping / max FPS refill handled by initGameOptionControls().

	GadgetCheckBoxSetChecked( checkBuildingOcclusion, TheGlobalData->m_enableBehindBuildingMarkers);

	GadgetCheckBoxSetChecked( checkProps, TheGlobalData->m_useTrees);
	//checkProps->winEnable(false);	//gray out the option for now.

	GadgetSliderSetPosition( sliderParticleCap, TheGlobalData->m_maxParticleCount );

	//set language filter
	AsciiString languageFilter = (*pref)["LanguageFilter"];
	if (languageFilter == "true" )
	{
		GadgetCheckBoxSetChecked( checkLanguageFilter, true);
		TheWritableGlobalData->m_languageFilterPref = true;
	}
	else
	{
		GadgetCheckBoxSetChecked( checkLanguageFilter, false);
		TheWritableGlobalData->m_languageFilterPref = false;
	}

	//set replay camera
	if (pref->saveCameraInReplays())
	{
		GadgetCheckBoxSetChecked( checkSaveCamera, true);
		TheWritableGlobalData->m_saveCameraInReplay = true;
	}
	else
	{
		GadgetCheckBoxSetChecked( checkSaveCamera, false);
		TheWritableGlobalData->m_saveCameraInReplay = false;
	}
	if (pref->useCameraInReplays())
	{
		GadgetCheckBoxSetChecked( checkUseCamera, true);
		TheWritableGlobalData->m_useCameraInReplay = true;
	}
	else
	{
		GadgetCheckBoxSetChecked( checkUseCamera, false);
		TheWritableGlobalData->m_useCameraInReplay = false;
	}

	//set scroll options
	AsciiString test = (*pref)["DrawScrollAnchor"];
	DEBUG_LOG(("DrawScrollAnchor == [%s]", test.str()));
	if (test == "yes" || (test.isEmpty() && TheInGameUI->getDrawRMBScrollAnchor()))
	{
		GadgetCheckBoxSetChecked( checkDrawAnchor, true);
		TheInGameUI->setDrawRMBScrollAnchor(true);
	}
	else
	{
		GadgetCheckBoxSetChecked( checkDrawAnchor, false);
		TheInGameUI->setDrawRMBScrollAnchor(false);
	}
	test = (*pref)["MoveScrollAnchor"];
	DEBUG_LOG(("MoveScrollAnchor == [%s]", test.str()));
	if (test == "yes" || (test.isEmpty() && TheInGameUI->getMoveRMBScrollAnchor()))
	{
		GadgetCheckBoxSetChecked( checkMoveAnchor, true);
		TheInGameUI->setMoveRMBScrollAnchor(true);
	}
	else
	{
		GadgetCheckBoxSetChecked( checkMoveAnchor, false);
		TheInGameUI->setMoveRMBScrollAnchor(false);
	}

//	// Audio Init shiznat
//	GadgetCheckBoxSetChecked(checkAudioHardware, TheAudio->getHardwareAccelerated());
//	GadgetCheckBoxSetChecked(checkAudioSurround, TheAudio->getSpeakerSurround());

	// set the mouse mode
	GadgetCheckBoxSetChecked(checkAlternateMouse, TheGlobalData->m_useAlternateMouse);
	GadgetCheckBoxSetChecked(checkRetaliation, TheGlobalData->m_clientRetaliationModeEnabled);
	GadgetCheckBoxSetChecked( checkDoubleClickAttackMove, TheGlobalData->m_doubleClickAttackMove );

	// set scroll speed slider
	// TheSuperHackers @tweak xezon 11/07/2025 No longer sets the slider position if the user setting
	// is set beyond the slider limits. This gives the user more freedom to customize the scroll
	// speed. The slider value remains 0.
	Int scrollPos = (Int)(TheGlobalData->m_keyboardScrollFactor*100.0f);
	Int scrollMin, scrollMax;
	GadgetSliderGetMinMax( sliderScrollSpeed, &scrollMin, &scrollMax );
	if (scrollPos >= scrollMin && scrollPos <= scrollMax)
	{
		GadgetSliderSetPosition( sliderScrollSpeed, scrollPos );
	}
	DEBUG_LOG(("Scroll Speed %d", scrollPos));

 	// set volume sliders

	// set music volume slider
	GadgetSliderSetPosition( sliderMusicVolume, REAL_TO_INT(pref->getMusicVolume()) );

	//set SFX volume slider
	Real maxVolume = MAX( pref->getSoundVolume(), pref->get3DSoundVolume() );
	GadgetSliderSetPosition( sliderSFXVolume, REAL_TO_INT( maxVolume ) );

	//set voice volume slider
	GadgetSliderSetPosition( sliderVoiceVolume, REAL_TO_INT(pref->getSpeechVolume()) );

	// set the gamma slider
 	GadgetSliderSetPosition( sliderGamma, REAL_TO_INT(pref->getGammaValue()) );

	// show menu
	layout->hide( FALSE );

	// set keyboard focus to main parent
	NameKeyType parentID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:OptionsMenuParent" );
	GameWindow *parent = TheWindowManager->winGetWindowFromId( nullptr, parentID );
	TheWindowManager->winSetFocus( parent );

	// GeneralsX @bugfix fbraz3 12/09/2026 TheGameSpyInfo is a persistent singleton in GeneralsX,
	// so checking '|| TheGameSpyInfo' previously caused resolution and detail options to always
	// be disabled, even in the main menu. Only disable them when actually in-game or in an online staging room.
	Bool inGame = (TheGameLogic->isInGame() && TheGameLogic->getGameMode() != GAME_SHELL);
	Bool inOnlineStaging = (TheGameSpyInfo && TheGameSpyInfo->getCurrentStagingRoom() != nullptr);

	if( inGame || inOnlineStaging )
	{
		// disable controls that you can't change the options for in game
		comboBoxLANIP->winEnable(FALSE);

		if (comboBoxOnlineIP)
			comboBoxOnlineIP->winEnable(FALSE);

		buttonFirewallRefresh->winEnable(FALSE);

		if (comboBoxDetail)
			comboBoxDetail->winEnable(FALSE);

		if (comboBoxResolution)
			comboBoxResolution->winEnable(FALSE);

		if (textEntryFirewallPortOverride)
			textEntryFirewallPortOverride->winEnable(FALSE);

//		if (checkAudioSurround)
//			checkAudioSurround->winEnable(FALSE);
//
//		if (checkAudioHardware)
//			checkAudioHardware->winEnable(FALSE);
	}


	TheWindowManager->winSetModal(parent);
	ignoreSelected = FALSE;
}

//-------------------------------------------------------------------------------------------------
/** options menu shutdown method */
//-------------------------------------------------------------------------------------------------
void OptionsMenuShutdown( WindowLayout *layout, void *userData )
{
/* moved into the back button stuff
	if (pref)
	{
		pref->write();
		delete pref;
		pref = nullptr;
	}

	comboBoxIP = nullptr;

	// hide menu
	layout->hide( TRUE );

	// our shutdown is complete
	TheShell->shutdownComplete( layout );
*/

}

//-------------------------------------------------------------------------------------------------
/** options menu update method */
//-------------------------------------------------------------------------------------------------
void OptionsMenuUpdate( WindowLayout *layout, void *userData )
{

}

//-------------------------------------------------------------------------------------------------
/** Options menu input callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType OptionsMenuInput( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

			switch( key )
			{

				// ----------------------------------------------------------------------------------------
				case KEY_ESC:
				{

					//
					// send a simulated selected event to the parent window of the
					// back/exit button
					//
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						NameKeyType buttonID = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonBack" );
						GameWindow *button = TheWindowManager->winGetWindowFromId( window, buttonID );

						TheWindowManager->winSendSystemMsg( window, GBM_SELECTED,
																								(WindowMsgData)button, buttonID );

					}

					// don't let key fall through anywhere else
					return MSG_HANDLED;

				}

			}

		}

	}

	return MSG_IGNORED;

}

//-------------------------------------------------------------------------------------------------
/** options menu window system callback */
//-------------------------------------------------------------------------------------------------
WindowMsgHandledType OptionsMenuSystem( GameWindow *window, UnsignedInt msg,
																				WindowMsgData mData1, WindowMsgData mData2 )
{
	static NameKeyType buttonBack = NAMEKEY_INVALID;
	static NameKeyType buttonDefaults = NAMEKEY_INVALID;
	static NameKeyType buttonAccept = NAMEKEY_INVALID;
	static NameKeyType buttonReplayMenu = NAMEKEY_INVALID;
	static NameKeyType buttonKeyboardOptionsMenu = NAMEKEY_INVALID;

	switch( msg )
	{

		// --------------------------------------------------------------------------------------------
		case GWM_CREATE:
		{

			// get ids for our children controls
			buttonBack = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonBack" );
			buttonDefaults = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonDefaults" );
			buttonAccept = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonAccept" );
			buttonKeyboardOptionsMenu = TheNameKeyGenerator->nameToKey( "OptionsMenu.wnd:ButtonKeyboardOptions" );

			break;

		}

		//---------------------------------------------------------------------------------------------
		case GWM_DESTROY:
		{

			break;

		}

		// --------------------------------------------------------------------------------------------
		case GWM_INPUT_FOCUS:
		{

			// if we're givin the opportunity to take the keyboard focus we must say we want it
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;

			return MSG_HANDLED;

		}

		//---------------------------------------------------------------------------------------------
		case GCM_SELECTED:
			{
				if(ignoreSelected)
					break;
				GameWindow *control = (GameWindow *)mData1;
				Int controlID = control->winGetWindowId();

				if (controlID == comboBoxDetailID)
				{
					Int index;
					GadgetComboBoxGetSelectedPos( comboBoxDetail, &index );
					if(index != STATIC_GAME_LOD_CUSTOM)
						break;

					showAdvancedOptions();
				}
			break;
		}

		//---------------------------------------------------------------------------------------------
		case GBM_SELECTED:
		{
			if(ignoreSelected)
				break;
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control->winGetWindowId();

			if( controlID == buttonBack )
			{
				// go back one screen
				//TheShell->pop();

				delete pref;
				pref = nullptr;

				comboBoxLANIP = nullptr;
				comboBoxOnlineIP = nullptr;

				if(GameSpyIsOverlayOpen(GSOVERLAY_OPTIONS))
					GameSpyCloseOverlay(GSOVERLAY_OPTIONS);
				else
				{
					DestroyOptionsLayout();
				}

			}
			else if (controlID == buttonAccept )
			{
				saveOptions();

				if (pref)
				{
					pref->write();
					delete pref;
					pref = nullptr;
				}

				comboBoxLANIP = nullptr;
				comboBoxOnlineIP = nullptr;

				if(!TheGameLogic->isInGame() || TheGameLogic->isInShellGame())
					destroyQuitMenu(); // if we're in a game, the change res then enter the same kind of game, we nee the quit menu to be gone.


				if(GameSpyIsOverlayOpen(GSOVERLAY_OPTIONS))
					GameSpyCloseOverlay(GSOVERLAY_OPTIONS);
				else
				{
					DestroyOptionsLayout();
					if (dispChanged)
					{
						DoResolutionDialog();
					}
				}

			}
			else if (controlID == buttonDefaults )
			{
				setDefaults();
			}
			else if (controlID == ButtonAdvancedAcceptID )
			{
				acceptAdvancedOptions();

			}
			else if (controlID == ButtonAdvancedCancelID )
			{
				cancelAdvancedOptions();
			}
			else if ( controlID == buttonKeyboardOptionsMenu )
			{
				TheShell->push( "Menus/KeyboardOptionsMenu.wnd" );
			}
			else if(controlID == checkDrawAnchorID )
      {
        if( GadgetCheckBoxIsChecked( control ) )
        {
          	TheInGameUI->setDrawRMBScrollAnchor(true);
          	(*pref)["DrawScrollAnchor"] = "yes";
        }
				else
        {
          	TheInGameUI->setDrawRMBScrollAnchor(false);
          	(*pref)["DrawScrollAnchor"] = "no";
        }
      }
			else if(controlID == checkMoveAnchorID )
      {
        if( GadgetCheckBoxIsChecked( control ) )
        {
          	TheInGameUI->setMoveRMBScrollAnchor(true);
          	(*pref)["MoveScrollAnchor"] = "yes";
        }
				else
        {
          	TheInGameUI->setMoveRMBScrollAnchor(false);
          	(*pref)["MoveScrollAnchor"] = "no";
        }
      }
			else if(controlID == checkSaveCameraID )
      {
        if( GadgetCheckBoxIsChecked( control ) )
        {
          	TheWritableGlobalData->m_saveCameraInReplay = true;
          	(*pref)["SaveCameraInReplays"] = "yes";
        }
				else
        {
          	TheWritableGlobalData->m_saveCameraInReplay = false;
          	(*pref)["SaveCameraInReplays"] = "no";
        }
      }
			else if(controlID == checkUseCameraID )
      {
        if( GadgetCheckBoxIsChecked( control ) )
        {
          	TheWritableGlobalData->m_useCameraInReplay = true;
          	(*pref)["UseCameraInReplays"] = "yes";
        }
				else
        {
          	TheWritableGlobalData->m_useCameraInReplay = false;
          	(*pref)["UseCameraInReplays"] = "no";
        }
      }
			else if (controlID == buttonFirewallRefreshID)
			{
				// setting the behavior to unknown will force the firewall helper to detect the firewall behavior
				// the next time we log into gamespy/WOL/whatever.
				char num[16];
				num[0] = 0;
				TheWritableGlobalData->m_firewallBehavior = FirewallHelperClass::FIREWALL_TYPE_UNKNOWN;
				itoa(TheGlobalData->m_firewallBehavior, num, 10);
				AsciiString numstr;
				numstr = num;
				(*pref)["FirewallBehavior"] = numstr;
			}
			break;

		}

		default:
			return MSG_IGNORED;

	}

	return MSG_HANDLED;

}
