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

// FILE: MissionSelect.cpp ///////////////////////////////////////////////////////////////////////
// GeneralsX @feature Qoder 07/10/2026 Modern "jump to any campaign mission" selector.
//   Vertical grouped list: the three main campaigns (USA / China / GLA) each show a
//   header row followed by their missions, top-to-bottom. Picking a mission paints its
//   map preview on the right; a difficulty combo + Start/Back sit along the lower-right.
//   The actual launch reuses the proven MainMenu campaign-start path (setupGameStart).
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "GameClient/GameClient.h"
#include "Common/GlobalData.h"
#include "Common/OptionPreferences.h"
#include "Common/NameKeyGenerator.h"
#include "Common/AsciiString.h"
#include "Common/UnicodeString.h"
#include "GameClient/Shell.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetComboBox.h"
#include "GameClient/GadgetListBox.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/KeyDefs.h"
#include "GameClient/WindowLayout.h"
#include "GameClient/Color.h"
#include "GameClient/GameText.h"
#include "GameClient/CampaignManager.h"
#include "GameClient/MapUtil.h"
#include "GameClient/Image.h"
#include "GameLogic/ScriptEngine.h"

// GeneralsX @feature Qoder 07/10/2026 Non-static globals defined in MainMenu.cpp; reused verbatim
// so we do not duplicate the (delicate) campaign start sequence. See DifficultySelect.cpp for the
// same forward-declaration pattern.
extern void prepareCampaignGame( GameDifficulty diff );

// Widget IDs
static NameKeyType listboxMissionsID = NAMEKEY_INVALID;
static NameKeyType winMapPreviewID = NAMEKEY_INVALID;
static NameKeyType comboBoxDifficultyID = NAMEKEY_INVALID;
static NameKeyType buttonStartID = NAMEKEY_INVALID;
static NameKeyType buttonBackID = NAMEKEY_INVALID;

// Widget pointers
static GameWindow *listboxMissions = nullptr;
static GameWindow *winMapPreview = nullptr;
static GameWindow *comboBoxDifficulty = nullptr;

// Row whose preview is currently shown, so a re-select of the same row is a no-op.
static Int s_lastPreviewRow = -1;

// Item-data marker for a non-selectable campaign header row.
static const Int PACKED_HEADER = -1;

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026 Pack a (campaign, mission) pair into a single int we can
// stash in a list row's item-data. Campaign indices stay tiny (well under 2^15) so the sign bit
// never collides with the header marker.
static Int packCampMission( Int campIdx, Int misIdx )
{
	return (campIdx << 16) | (misIdx & 0xFFFF);
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026 Only the three flagship campaigns are offered;
// challenge maps and training missions are intentionally excluded to match the request.
// Note: the engine lower-cases INI block names at parse time, so compare case-insensitively.
// GeneralsX @feature Qoder 07/10/2026 Also surface the migrated original Generals campaigns
// (loose Data\INI\Campaign\GeneralsCampaigns.ini), which are namespaced with a GEN_ prefix to
// avoid colliding with ZH's own USA/GLA/China/TRAINING campaigns.
static Bool isMainCampaign( const Campaign *camp )
{
	if( !camp )
		return FALSE;

	return camp->m_name.compareNoCase( "USA" ) == 0
		|| camp->m_name.compareNoCase( "China" ) == 0
		|| camp->m_name.compareNoCase( "GLA" ) == 0
		|| camp->m_name.compareNoCase( "GEN_USA" ) == 0
		|| camp->m_name.compareNoCase( "GEN_GLA" ) == 0
		|| camp->m_name.compareNoCase( "GEN_China" ) == 0
		|| camp->m_name.compareNoCase( "GEN_TRAINING" ) == 0;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026 Human-readable label for a campaign. Prefer the
// localized string-manager label, fall back to the raw faction name when it is missing.
static UnicodeString campaignDisplayName( const Campaign *camp )
{
	UnicodeString disp;
	Bool exists = FALSE;
	if( !camp->m_campaignNameLabel.isEmpty() )
		disp = TheGameText->fetch( camp->m_campaignNameLabel, &exists );
	if( !exists || disp.isEmpty() )
		disp.translate( camp->m_name );

	// GeneralsX @feature Qoder 07/10/2026 Tag the migrated Generals campaigns so they read as
	// "【将军原版】<name>" and stay visually distinct from the native ZH campaigns in the list.
	// The label itself is shared with ZH (CAMPAIGN:USA etc.), so the prefix is applied here.
	// Wide-char escapes keep this source free of code-page-dependent literals.
	if( camp->m_name.startsWithNoCase( "GEN_" ) )
	{
		UnicodeString prefixed( L"\u3010\u5c06\u519b\u539f\u7248\u3011" );
		prefixed.concat( disp );
		return prefixed;
	}

	return disp;
}

//-------------------------------------------------------------------------------------------------
static void populateDifficultyCombo( void )
{
	if( !comboBoxDifficulty )
		return;

	const Color white = GameMakeColor( 255, 255, 255, 255 );
	GadgetComboBoxReset( comboBoxDifficulty );

	// Order matters: index maps 1:1 onto GameDifficulty (EASY=0, NORMAL=1, HARD=2).
	GadgetComboBoxAddEntry( comboBoxDifficulty, TheGameText->FETCH_OR_SUBSTITUTE( "GUI:DifficultyEasy", L"Easy" ), white );
	GadgetComboBoxAddEntry( comboBoxDifficulty, TheGameText->FETCH_OR_SUBSTITUTE( "GUI:DifficultyNormal", L"Normal" ), white );
	GadgetComboBoxAddEntry( comboBoxDifficulty, TheGameText->FETCH_OR_SUBSTITUTE( "GUI:DifficultyHard", L"Hard" ), white );

	GadgetComboBoxSetSelectedPos( comboBoxDifficulty, DIFFICULTY_NORMAL );
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026 Build the vertical grouped list: for each main campaign a
// colored header row (marked PACKED_HEADER, never launchable) followed by its numbered missions.
// Mission text uses common CJK glyphs ("第N关") that the retail font atlas is known to carry;
// the header reuses the already-localized campaign name from the string manager.
static void populateMissionList( void )
{
	if( !listboxMissions )
		return;

	const Color headerColor = GameMakeColor( 255, 210, 90, 255 );
	const Color missionColor = GameMakeColor( 255, 255, 255, 255 );

	GadgetListBoxReset( listboxMissions );

	Int count = TheCampaignManager ? TheCampaignManager->getCampaignCount() : -1;
	for( Int i = 0; i < count; ++i )
	{
		Campaign *camp = TheCampaignManager->getCampaignByIndex( i );
		if( !isMainCampaign( camp ) )
			continue;

		Int hrow = GadgetListBoxAddEntryText( listboxMissions, campaignDisplayName( camp ), headerColor, -1, 0 );
		GadgetListBoxSetItemData( listboxMissions, (void *)(intptr_t)PACKED_HEADER, hrow, 0 );

		Int n = camp->getMissionCount();
		for( Int m = 0; m < n; ++m )
		{
			Mission *mis = camp->getMissionByIndex( m );
			if( !mis )
				continue;

			UnicodeString label;
			label.format( L"第%d关", m + 1 );
			Int mrow = GadgetListBoxAddEntryText( listboxMissions, label, missionColor, -1, 0 );
			GadgetListBoxSetItemData( listboxMissions, (void *)(intptr_t)packCampMission( i, m ), mrow, 0 );
		}
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026 Paint the preview for the given mission. We deliberately
// never set MapMetaData user-data on the window, so its W3DDrawMapPreview callback falls through
// to the default image draw and simply renders the enabled image we install here. Missing or
// unresolved previews fall back to the stock "UnknownMap" placeholder.
static void showMissionPreview( Mission *mis )
{
	if( !winMapPreview )
		return;

	const Image *image = nullptr;
	if( mis )
		image = getMapPreviewImage( mis->m_mapName );
	if( !image && TheMappedImageCollection )
		image = TheMappedImageCollection->findImageByName( "UnknownMap" );

	if( image )
	{
		winMapPreview->winSetStatus( WIN_STATUS_IMAGE );
		winMapPreview->winSetEnabledImage( 0, image );
	}
	else
	{
		winMapPreview->winClearStatus( WIN_STATUS_IMAGE );
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026 Return the packed (campaign,mission) stored on the current
// selection, or PACKED_HEADER when nothing launchable is selected.
static Int selectedPackedMission( void )
{
	if( !listboxMissions )
		return PACKED_HEADER;

	Int sel = -1;
	GadgetListBoxGetSelected( listboxMissions, &sel );
	if( sel < 0 )
		return PACKED_HEADER;

	return (Int)(intptr_t)GadgetListBoxGetItemData( listboxMissions, sel, 0 );
}

//-------------------------------------------------------------------------------------------------
void MissionSelectInit( WindowLayout *layout, void *userData )
{
	listboxMissionsID = TheNameKeyGenerator->nameToKey( "MissionSelect.wnd:ListMissions" );
	winMapPreviewID = TheNameKeyGenerator->nameToKey( "MissionSelect.wnd:WinMapPreview" );
	comboBoxDifficultyID = TheNameKeyGenerator->nameToKey( "MissionSelect.wnd:ComboBoxDifficulty" );

	listboxMissions = TheWindowManager->winGetWindowFromId( nullptr, listboxMissionsID );
	winMapPreview = TheWindowManager->winGetWindowFromId( nullptr, winMapPreviewID );
	comboBoxDifficulty = TheWindowManager->winGetWindowFromId( nullptr, comboBoxDifficultyID );

	s_lastPreviewRow = -1;

	populateMissionList();
	populateDifficultyCombo();
}

//-------------------------------------------------------------------------------------------------
void MissionSelectUpdate( WindowLayout *layout, void *userData )
{
	// The mission list is static for the session; nothing to refresh per tick.
}

//-------------------------------------------------------------------------------------------------
void MissionSelectShutdown( WindowLayout *layout, void *userData )
{
	listboxMissions = nullptr;
	winMapPreview = nullptr;
	comboBoxDifficulty = nullptr;
	s_lastPreviewRow = -1;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026 Boot into the chosen mission. Reuses the exact fresh
// campaign-start sequence that MainMenu's own difficulty buttons run (prepareCampaignGame):
// persist difficulty to prefs, publish it to the script engine, then trigger setupGameStart.
// Because Shell::update() ticks every layout on the stack, MainMenuUpdate stays alive underneath
// us and consumes the startGame flag to fire doGameStart().
static void startSelectedMission( void )
{
	Int packed = selectedPackedMission();
	if( packed == PACKED_HEADER )
		return;

	Int campIdx = packed >> 16;
	Int misIdx = packed & 0xFFFF;

	Campaign *camp = TheCampaignManager->getCampaignByIndex( campIdx );
	if( !camp )
		return;
	Mission *mis = camp->getMissionByIndex( misIdx );
	if( !mis )
		return;

	Int diffSel = -1;
	if( comboBoxDifficulty )
		GadgetComboBoxGetSelectedPos( comboBoxDifficulty, &diffSel );
	GameDifficulty diff = ( diffSel >= 0 && diffSel <= DIFFICULTY_HARD )
		? (GameDifficulty)diffSel : DIFFICULTY_NORMAL;

	TheCampaignManager->setCampaignAndMission( camp->m_name, mis->m_name );

	OptionPreferences pref;
	pref.setCampaignDifficulty( diff );
	pref.write();
	TheScriptEngine->setGlobalDifficulty( diff );

	// GeneralsX @feature Qoder 07/10/2026 Pop this layout off the shell stack first, then reuse
	// MainMenu's proven campaign-start sequence (identical to what its difficulty buttons run).
	// Leaving this layout on the stack is what made the list overlay the running game and turn
	// the post-game menu into a dead black screen.
	TheShell->popImmediate();
	prepareCampaignGame( diff );
}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType MissionSelectSystem( GameWindow *window, UnsignedInt msg,
																					WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		case GWM_CREATE:
		{
			buttonStartID = TheNameKeyGenerator->nameToKey( "MissionSelect.wnd:ButtonStart" );
			buttonBackID = TheNameKeyGenerator->nameToKey( "MissionSelect.wnd:ButtonBack" );
			listboxMissionsID = TheNameKeyGenerator->nameToKey( "MissionSelect.wnd:ListMissions" );
			break;
		}

		case GWM_DESTROY:
			break;

		case GWM_INPUT_FOCUS:
		{
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;
			return MSG_HANDLED;
		}

		// GeneralsX @feature Qoder 07/10/2026 List rows bubble their selection here (listbox
		// SYSTEMCALLBACK is [None]); mData2 is the selected row index.
		case GLM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			if( control && control->winGetWindowId() == listboxMissionsID )
			{
				Int row = (Int)mData2;
				if( row < 0 || row == s_lastPreviewRow )
					break;

				Int packed = (Int)(intptr_t)GadgetListBoxGetItemData( listboxMissions, row, 0 );
				if( packed == PACKED_HEADER )
					break;	// header row: keep the current preview untouched

				Mission *mis = nullptr;
				Campaign *camp = TheCampaignManager->getCampaignByIndex( packed >> 16 );
				if( camp )
					mis = camp->getMissionByIndex( packed & 0xFFFF );

				showMissionPreview( mis );
				s_lastPreviewRow = row;
			}
			break;
		}

		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			Int controlID = control ? control->winGetWindowId() : -1;

			if( controlID == buttonBackID )
			{
				TheShell->popImmediate();
			}
			else if( controlID == buttonStartID )
			{
				startSelectedMission();
			}
			break;
		}

		default:
			break;
	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType MissionSelectInput( GameWindow *window, UnsignedInt msg,
																				 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{

		case GWM_CHAR:
		{
			UnsignedByte key = mData1;
			UnsignedByte state = mData2;

			switch( key )
			{

				case KEY_ESC:
				{
					if( BitIsSet( state, KEY_STATE_UP ) )
					{
						TheShell->pop();
					}
					return MSG_HANDLED;
				}

			}
		}

	}

	return MSG_IGNORED;
}
