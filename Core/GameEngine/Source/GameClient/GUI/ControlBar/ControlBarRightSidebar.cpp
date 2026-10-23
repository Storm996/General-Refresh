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

//////// ControlBarRightSidebar.cpp ////////////////////////////////////////////////////////////////
// GeneralsX @feature Qoder 22/10/2026 RA3-style right-side production sidebar.
// Additive overlay: the retail bottom control bar and its context-sensitive command windows stay
// untouched and fully functional. The sidebar mirrors those command buttons on the right edge and
// gives the player BUILD / INFANTRY / VEHICLES tabs that switch the active producer.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include <algorithm>
#include <cstdlib>

#include "Common/KindOf.h"
#include "Common/NameKeyGenerator.h"
#include "Common/ObjectStatusTypes.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "Common/Team.h"
#include "Common/ThingTemplate.h"
#include "Common/UnicodeString.h"

#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"

#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/Gadget.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"
#include "GameClient/RightSidebar.h"

namespace {

enum SidebarCategory
{
	SIDEBAR_CATEGORY_BUILD = 0,		///< dozer / worker construction commands
	SIDEBAR_CATEGORY_INFANTRY,		///< barracks
	SIDEBAR_CATEGORY_VEHICLES,		///< war factory
	SIDEBAR_CATEGORY_COUNT
};

enum
{
	SIDEBAR_TAB_COUNT = SIDEBAR_CATEGORY_COUNT,
	SIDEBAR_FACILITY_SLOTS = 6,
	SIDEBAR_COMMAND_SLOTS = 15,
	SIDEBAR_MAX_FACILITIES = 8
};

struct FacilityBuckets
{
	ObjectID ids[ SIDEBAR_CATEGORY_COUNT ][ SIDEBAR_MAX_FACILITIES ];
	Int counts[ SIDEBAR_CATEGORY_COUNT ];
};

GameWindow *s_root = nullptr;
GameWindow *s_tabs[ SIDEBAR_TAB_COUNT ] = { nullptr, nullptr, nullptr };
GameWindow *s_facilityButtons[ SIDEBAR_FACILITY_SLOTS ] = { nullptr };
GameWindow *s_commandButtons[ SIDEBAR_COMMAND_SLOTS ] = { nullptr };

FacilityBuckets s_buckets;
Int s_activeTab = SIDEBAR_CATEGORY_BUILD;
Bool s_visible = FALSE;

//-------------------------------------------------------------------------------------------------
NameKeyType sidebarKey( const char *name )
{
	return TheNameKeyGenerator->nameToKey( name );
}

//-------------------------------------------------------------------------------------------------
GameWindow *sidebarWindow( const char *name )
{
	if( !TheWindowManager )
		return nullptr;

	return TheWindowManager->winGetWindowFromId( nullptr, sidebarKey( name ) );
}

//-------------------------------------------------------------------------------------------------
void hideCommands()
{
	for( Int i = 0; i < SIDEBAR_COMMAND_SLOTS; ++i )
	{
		if( s_commandButtons[ i ] )
			s_commandButtons[ i ]->winHide( TRUE );
	}
}

//-------------------------------------------------------------------------------------------------
void hideFacilities()
{
	for( Int i = 0; i < SIDEBAR_FACILITY_SLOTS; ++i )
	{
		if( s_facilityButtons[ i ] )
			s_facilityButtons[ i ]->winHide( TRUE );
	}
}

//-------------------------------------------------------------------------------------------------
void gatherFacilities( Object *obj, void *userData )
{
	FacilityBuckets *buckets = static_cast<FacilityBuckets *>(userData);
	if( !obj || !buckets )
		return;

	if( obj->isEffectivelyDead() || obj->testStatus( OBJECT_STATUS_UNDER_CONSTRUCTION ) )
		return;

	Int category = -1;
	if( obj->isKindOf( KINDOF_DOZER ) )
		category = SIDEBAR_CATEGORY_BUILD;
	else if( obj->isKindOf( KINDOF_FS_BARRACKS ) )
		category = SIDEBAR_CATEGORY_INFANTRY;
	else if( obj->isKindOf( KINDOF_FS_WARFACTORY ) )
		category = SIDEBAR_CATEGORY_VEHICLES;

	if( category < 0 )
		return;

	if( buckets->counts[ category ] >= SIDEBAR_MAX_FACILITIES )
		return;

	buckets->ids[ category ][ buckets->counts[ category ]++ ] = obj->getID();
}

//-------------------------------------------------------------------------------------------------
void refreshFacilityBuckets( Player *player )
{
	for( Int c = 0; c < SIDEBAR_CATEGORY_COUNT; ++c )
		s_buckets.counts[ c ] = 0;

	if( !player )
		return;

	player->iterateObjects( gatherFacilities, &s_buckets );

	for( Int c = 0; c < SIDEBAR_CATEGORY_COUNT; ++c )
	{
		if( s_buckets.counts[ c ] > 1 )
			std::sort( s_buckets.ids[ c ], s_buckets.ids[ c ] + s_buckets.counts[ c ] );
	}
}

//-------------------------------------------------------------------------------------------------
Int findFacilityIndex( Int category, ObjectID objectID )
{
	if( category < 0 || category >= SIDEBAR_CATEGORY_COUNT )
		return -1;

	for( Int i = 0; i < s_buckets.counts[ category ]; ++i )
	{
		if( s_buckets.ids[ category ][ i ] == objectID )
			return i;
	}

	return -1;
}

//-------------------------------------------------------------------------------------------------
void activateTab( Int tab )
{
	if( tab < 0 || tab >= SIDEBAR_TAB_COUNT )
		return;

	if( s_buckets.counts[ tab ] <= 0 )
		return;

	ObjectID current = TheControlBar ? TheControlBar->getProductionFocusObjectID() : INVALID_ID;
	Int index = findFacilityIndex( tab, current );

	if( tab == s_activeTab && index >= 0 )
		index = ( index + 1 ) % s_buckets.counts[ tab ];
	else if( index < 0 )
		index = 0;

	s_activeTab = tab;

	if( TheControlBar )
		TheControlBar->setProductionFocus( s_buckets.ids[ tab ][ index ] );
}

//-------------------------------------------------------------------------------------------------
void activateFacility( Int slot )
{
	if( s_activeTab < 0 || s_activeTab >= SIDEBAR_TAB_COUNT )
		return;

	if( slot < 0 || slot >= s_buckets.counts[ s_activeTab ] )
		return;

	if( TheControlBar )
		TheControlBar->setProductionFocus( s_buckets.ids[ s_activeTab ][ slot ] );
}

//-------------------------------------------------------------------------------------------------
void mirrorCommands()
{
	Int slot = 0;

	for( Int i = 1; i < MAX_COMMANDS_PER_SET && slot < SIDEBAR_COMMAND_SLOTS; ++i )
	{
		GameWindow *source = TheControlBar ? TheControlBar->getCommandWindowForSidebar( i ) : nullptr;
		if( !source || source->winIsHidden() )
			continue;

		if( source->winGetInputFunc() != GadgetPushButtonInput )
			continue;

		const CommandButton *command = static_cast<const CommandButton *>( GadgetButtonGetData( source ) );
		if( !command )
			continue;

		GameWindow *mirror = s_commandButtons[ slot ];
		if( !mirror )
			break;

		if( static_cast<const CommandButton *>( GadgetButtonGetData( mirror ) ) != command )
		{
			// Do not route through ControlBar::setControlCommand(): it also registers the command
			// hotkey, which is already owned by the original bottom-bar button (debug builds would
			// trip on the duplicate). Mirrors only need the cameo image and the command payload.
			GadgetButtonEnableCheckLike( mirror, BitIsSet( command->getOptions(), CHECK_LIKE ), FALSE );
			if( const Image *image = command->getButtonImage() )
				GadgetButtonSetEnabledImage( mirror, image );
			GadgetButtonSetData( mirror, const_cast<CommandButton *>( command ) );
		}

		mirror->winHide( FALSE );
		mirror->winEnable( BitIsSet( source->winGetStatus(), WIN_STATUS_ENABLED ) );
		++slot;
	}

	for( ; slot < SIDEBAR_COMMAND_SLOTS; ++slot )
	{
		if( s_commandButtons[ slot ] )
			s_commandButtons[ slot ]->winHide( TRUE );
	}
}

//-------------------------------------------------------------------------------------------------
WindowMsgHandledType rightSidebarSystem( GameWindow *window, UnsignedInt msg,
																				 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{
		case GBM_SELECTED:
		case GBM_SELECTED_RIGHT:
		{
			GameWindow *control = reinterpret_cast<GameWindow *>( mData1 );
			if( !control )
				break;

			Int controlID = control->winGetWindowId();

			for( Int i = 0; i < SIDEBAR_TAB_COUNT; ++i )
			{
				if( s_tabs[ i ] && controlID == s_tabs[ i ]->winGetWindowId() )
				{
					activateTab( i );
					return MSG_HANDLED;
				}
			}

			for( Int i = 0; i < SIDEBAR_FACILITY_SLOTS; ++i )
			{
				if( s_facilityButtons[ i ] && controlID == s_facilityButtons[ i ]->winGetWindowId() )
				{
					activateFacility( i );
					return MSG_HANDLED;
				}
			}

			for( Int i = 0; i < SIDEBAR_COMMAND_SLOTS; ++i )
			{
				if( s_commandButtons[ i ] && controlID == s_commandButtons[ i ]->winGetWindowId() )
				{
					if( TheControlBar )
						TheControlBar->processContextSensitiveButtonClick( control, (GadgetGameMessage)msg );
					return MSG_HANDLED;
				}
			}

			break;
		}

		case GBM_MOUSE_ENTERING:
		case GBM_MOUSE_LEAVING:
		{
			GameWindow *control = reinterpret_cast<GameWindow *>( mData1 );
			if( !control )
				break;

			for( Int i = 0; i < SIDEBAR_COMMAND_SLOTS; ++i )
			{
				if( s_commandButtons[ i ] && control == s_commandButtons[ i ] )
				{
					if( TheControlBar )
						TheControlBar->processContextSensitiveButtonTransition( control, (GadgetGameMessage)msg );
					return MSG_HANDLED;
				}
			}

			break;
		}
	}

	return MSG_IGNORED;
}

//-------------------------------------------------------------------------------------------------
void updateTabVisuals()
{
	for( Int i = 0; i < SIDEBAR_TAB_COUNT; ++i )
	{
		GameWindow *tab = s_tabs[ i ];
		if( !tab )
			continue;

		Bool enabled = ( s_buckets.counts[ i ] > 0 );
		tab->winEnable( enabled );
		GadgetCheckLikeButtonSetVisualCheck( tab, enabled && i == s_activeTab );
	}
}

//-------------------------------------------------------------------------------------------------
void updateFacilityVisuals( ObjectID focusObjectID )
{
	Int count = ( s_activeTab >= 0 && s_activeTab < SIDEBAR_CATEGORY_COUNT )
		? s_buckets.counts[ s_activeTab ] : 0;

	for( Int i = 0; i < SIDEBAR_FACILITY_SLOTS; ++i )
	{
		GameWindow *button = s_facilityButtons[ i ];
		if( !button )
			continue;

		if( i >= count )
		{
			button->winHide( TRUE );
			continue;
		}

		ObjectID objectID = s_buckets.ids[ s_activeTab ][ i ];
		Object *obj = TheGameLogic ? TheGameLogic->findObjectByID( objectID ) : nullptr;

		UnicodeString label;
		label.format( L"%d", i + 1 );
		GadgetButtonSetText( button, label );

		if( obj && obj->getTemplate() )
			button->winSetTooltip( obj->getTemplate()->getDisplayName() );

		button->winHide( FALSE );
		button->winEnable( TRUE );
		GadgetCheckLikeButtonSetVisualCheck( button, objectID == focusObjectID );
	}
}

} // namespace

//-------------------------------------------------------------------------------------------------
void RightSidebarInit()
{
	RightSidebarShutdown();

	// GeneralsX @feature Qoder 22/10/2026 Escape hatch while the overlay is being validated:
	// GENERALSX_RIGHT_SIDEBAR=0 keeps the retail bottom bar as the only UI.
	const char *enabled = getenv( "GENERALSX_RIGHT_SIDEBAR" );
	if( enabled && atoi( enabled ) == 0 )
		return;

	if( !TheWindowManager )
		return;

	s_root = TheWindowManager->winCreateFromScript( "RightSidebar.wnd" );
	if( !s_root )
	{
		DEBUG_LOG( ( "RightSidebar: RightSidebar.wnd not found; right-side bar disabled.\n" ) );
		return;
	}

	s_root->winSetSystemFunc( rightSidebarSystem );

	s_tabs[ SIDEBAR_CATEGORY_BUILD ]			= sidebarWindow( "RightSidebar.wnd:TabBuild" );
	s_tabs[ SIDEBAR_CATEGORY_INFANTRY ]		= sidebarWindow( "RightSidebar.wnd:TabInfantry" );
	s_tabs[ SIDEBAR_CATEGORY_VEHICLES ]		= sidebarWindow( "RightSidebar.wnd:TabVehicles" );

	for( Int i = 0; i < SIDEBAR_TAB_COUNT; ++i )
	{
		if( s_tabs[ i ] )
			GadgetButtonEnableCheckLike( s_tabs[ i ], TRUE, FALSE );
	}

	for( Int i = 0; i < SIDEBAR_FACILITY_SLOTS; ++i )
	{
		AsciiString name;
		name.format( "RightSidebar.wnd:Facility%02d", i + 1 );
		s_facilityButtons[ i ] = sidebarWindow( name.str() );
		if( s_facilityButtons[ i ] )
		{
			GadgetButtonEnableCheckLike( s_facilityButtons[ i ], TRUE, FALSE );
			s_facilityButtons[ i ]->winHide( TRUE );
		}
	}

	for( Int i = 0; i < SIDEBAR_COMMAND_SLOTS; ++i )
	{
		AsciiString name;
		name.format( "RightSidebar.wnd:Command%02d", i + 1 );
		s_commandButtons[ i ] = sidebarWindow( name.str() );
		if( s_commandButtons[ i ] )
			s_commandButtons[ i ]->winHide( TRUE );
	}

	for( Int c = 0; c < SIDEBAR_CATEGORY_COUNT; ++c )
		s_buckets.counts[ c ] = 0;

	s_activeTab = SIDEBAR_CATEGORY_BUILD;
	s_visible = FALSE;
	s_root->winHide( TRUE );
}

//-------------------------------------------------------------------------------------------------
void RightSidebarShutdown()
{
	if( s_root && TheWindowManager )
		TheWindowManager->winDestroy( s_root );

	s_root = nullptr;
	s_visible = FALSE;

	for( Int i = 0; i < SIDEBAR_TAB_COUNT; ++i )
		s_tabs[ i ] = nullptr;

	for( Int i = 0; i < SIDEBAR_FACILITY_SLOTS; ++i )
		s_facilityButtons[ i ] = nullptr;

	for( Int i = 0; i < SIDEBAR_COMMAND_SLOTS; ++i )
		s_commandButtons[ i ] = nullptr;

	for( Int c = 0; c < SIDEBAR_CATEGORY_COUNT; ++c )
		s_buckets.counts[ c ] = 0;
}

//-------------------------------------------------------------------------------------------------
void RightSidebarSetVisible( Bool visible )
{
	s_visible = visible;

	if( s_root )
		s_root->winHide( !visible );
}

//-------------------------------------------------------------------------------------------------
Bool RightSidebarIsVisible()
{
	return s_visible && s_root != nullptr;
}

//-------------------------------------------------------------------------------------------------
void RightSidebarUpdate()
{
	if( !s_root || s_root->winIsHidden() )
		return;

	if( !TheControlBar || !ThePlayerList || !TheGameLogic )
	{
		hideCommands();
		return;
	}

	Player *localPlayer = ThePlayerList->getLocalPlayer();
	if( !localPlayer || !localPlayer->isPlayerActive() )
	{
		hideCommands();
		hideFacilities();
		for( Int i = 0; i < SIDEBAR_TAB_COUNT; ++i )
		{
			if( s_tabs[ i ] )
				s_tabs[ i ]->winEnable( FALSE );
		}
		return;
	}

	refreshFacilityBuckets( localPlayer );

	// Keep a sensible active tab when the current one has no producer.
	if( s_buckets.counts[ s_activeTab ] <= 0 )
	{
		for( Int i = 0; i < SIDEBAR_TAB_COUNT; ++i )
		{
			if( s_buckets.counts[ i ] > 0 )
			{
				s_activeTab = i;
				break;
			}
		}
	}

	updateTabVisuals();

	ObjectID focusObjectID = TheControlBar->getProductionFocusObjectID();
	updateFacilityVisuals( focusObjectID );

	if( focusObjectID == INVALID_ID || findFacilityIndex( s_activeTab, focusObjectID ) < 0 )
	{
		hideCommands();
		return;
	}

	mirrorCommands();
}
