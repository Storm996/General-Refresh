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

// FILE: ControlBarMultiSelect.cpp ////////////////////////////////////////////////////////////////
// Author: Colin Day, March 2002
// Desc:   Context-sensitive GUI for when you select multiple objects.  What we do is show
//				 the commands that you can use between them all
///////////////////////////////////////////////////////////////////////////////////////////////////

// USER INCLUDES //////////////////////////////////////////////////////////////////////////////////
#include "PreRTS.h"	// This must go first in EVERY cpp file in the GameEngine

#include "Common/ThingTemplate.h"
#include "Common/UnicodeString.h"
#include "GameClient/ControlBar.h"
#include "GameClient/Drawable.h"
#include "GameClient/GameClient.h"
#include "GameClient/GadgetPushButton.h"
#include "GameClient/GadgetStaticText.h"
#include "GameClient/Gadget.h"					// GeneralsX @feature w1semannn 11/10/2026 TextData for the composition count labels
#include "GameClient/WinInstanceData.h"	// GeneralsX @feature w1semannn 11/10/2026 WinInstanceData for building the composition panel windows
#include "GameClient/GameWindow.h"
#include "GameClient/GameWindowManager.h"	// GeneralsX @feature w1semannn 11/10/2026 runtime window creation (winCreate / gogoGadget*)
#include "GameClient/GameFont.h"				// GeneralsX @feature w1semannn 11/10/2026 GameFont for the count labels
#include "GameClient/Display.h"				// GeneralsX @feature w1semannn 11/10/2026 TheDisplay for screen-anchored layout
#include "GameClient/InGameUI.h"
#include "GameLogic/Object.h"

// GeneralsX @feature w1semannn 11/10/2026 File-local system callback for the composition panel container.
static WindowMsgHandledType compositionPanelSystem( GameWindow *window, UnsignedInt msg, WindowMsgData mData1, WindowMsgData mData2 );

// GeneralsX @feature w1semannn 11/10/2026 Layout constants for the bottom-left composition panel. Cells lay out left-to-right in rows of up to COMP_PER_ROW; the whole block re-anchors just above the command bar every frame so it tracks HUD collapse.
static const Int COMP_BTN_SIZE  = 44;   // cameo button edge
static const Int COMP_LBL_W     = 46;   // "x N" label width (right of the cameo)
static const Int COMP_CELL_GAP  = 10;   // horizontal gap between cells
static const Int COMP_CELL_STEP = COMP_BTN_SIZE + COMP_LBL_W + COMP_CELL_GAP;
static const Int COMP_ROW_STEP  = 48;   // vertical distance between rows
static const Int COMP_PER_ROW   = 10;   // cells per row before wrapping to a second row
static const Int COMP_PANEL_PAD = 6;    // inner padding around the panel content
static const Int COMP_EDGE_X    = 8;    // left screen margin
static const Int COMP_GAP_Y     = 4;    // vertical gap between the panel bottom and the command-bar top




//-------------------------------------------------------------------------------------------------
/** Reset the common command data */
//-------------------------------------------------------------------------------------------------
void ControlBar::resetCommonCommandData()
{
	Int i;

	for( i = 0; i < MAX_COMMANDS_PER_SET; i++ )
	{
		m_commonCommands[ i ] = nullptr;
		//Clear out any remnant overlays.
		GadgetButtonDrawOverlayImage( m_commandWindows[ i ], nullptr );
	}

}

//-------------------------------------------------------------------------------------------------
/** add the common commands of this drawable to the common command set */
//-------------------------------------------------------------------------------------------------
void ControlBar::addCommonCommands( Drawable *draw, Bool firstDrawable )
{
	Int i;
	const CommandButton *command;

	// sanity
	if( draw == nullptr )
		return;

	Object* obj = draw->getObject();
	if (!obj)
		return;

	if (obj->isKindOf(KINDOF_IGNORED_IN_GUI)) // ignore these guys
		return;

	// get the command set of this drawable
	const CommandSet *commandSet = findCommandSet( obj->getCommandSetString() );
	if( commandSet == nullptr )
	{

		//
		// if there is no command set for this drawable, none of the selected drawables
		// can possibly have matching commands so we'll get rid of them all
		//
		for( i = 0; i < MAX_COMMANDS_PER_SET; i++ )
		{

			m_commonCommands[ i ] = nullptr;
			if (m_commandWindows[ i ])
			{
				m_commandWindows[ i ]->winHide( TRUE );
			}
			// After Every change to the m_commandWIndows, we need to show fill in the missing blanks with the images
	// removed from multiplayer branch
			//showCommandMarkers();

		}

		return;

	}


	//
	// easy case, if we're adding the first drawable we simply just add any of the commands
	// in its set that can be multi-select commands to the common command set
	//
	if( firstDrawable == TRUE )
	{

		// just add each command that is classified as a common command
		for( i = 0; i < MAX_COMMANDS_PER_SET; i++ )
		{
			// our implementation doesn't necessarily make use of the max possible command buttons
			if (! m_commandWindows[ i ]) continue;

			// get command
			command = commandSet->getCommandButton(i);

			// add if present and can be used in a multi select
			if( command && BitIsSet( command->getOptions(), OK_FOR_MULTI_SELECT ) == TRUE )
			{

				// put it in the common command set
				m_commonCommands[ i ] = command;

				// show and enable this control
				m_commandWindows[ i ]->winHide( FALSE );
				m_commandWindows[ i ]->winEnable( TRUE );

				// set the command into the control
				setControlCommand( m_commandWindows[ i ], command );

			}

		}

	}
	else
	{

		// go through each command one by one
		for( i = 0; i < MAX_COMMANDS_PER_SET; i++ )
		{

			// our implementation doesn't necessarily make use of the max possible command buttons
			if (! m_commandWindows[ i ]) continue;

			// get the command
			command = commandSet->getCommandButton(i);

			Bool attackMove = (command && command->getCommandType() == GUI_COMMAND_ATTACK_MOVE) ||
												(m_commonCommands[ i ] && m_commonCommands[ i ]->getCommandType() == GUI_COMMAND_ATTACK_MOVE);

			// Kris: When any units have attack move, they all get it. This is to allow
			// combat units to be selected with the odd dozer or pilot and still retain that ability.
			if( attackMove && !m_commonCommands[ i ] )
			{
				// put it in the common command set
				m_commonCommands[ i ] = command;

				// show and enable this control
				m_commandWindows[ i ]->winHide( FALSE );
				m_commandWindows[ i ]->winEnable( TRUE );

				// set the command into the control
				setControlCommand( m_commandWindows[ i ], command );
			}
			else if( command != m_commonCommands[ i ] && !attackMove )
			{
				//
				// if this command does not match the command that is in the common command set then
				// *neither* this command OR the command in the common command set are really common
				// commands, so we will remove the one that has been stored in the common set
				//

				// remove the common command
				m_commonCommands[ i ] = nullptr;

				//
				// hide the window control cause it should have been made visible from a command
				// that was placed in this common 'slot' earlier
				//
				m_commandWindows[ i ]->winHide( TRUE );
			}

		}

	}

	// After Every change to the m_commandWIndows, we need to show fill in the missing blanks with the images
	// removed from multiplayer branch
	//showCommandMarkers();

}

//-------------------------------------------------------------------------------------------------
/** Populate the visible command bar with commands that are common to all the objects
	* that are selected in the UI */
//-------------------------------------------------------------------------------------------------
void ControlBar::populateMultiSelect()
{
	Drawable *draw;
	Bool firstDrawable = TRUE;
	Bool portraitSet = FALSE;
	const Image *portrait = nullptr;
	Object *portraitObj = nullptr;

	// first reset the common command data
	resetCommonCommandData();

	// by default, hide all the controls in the command section
	for( Int i = 0; i < MAX_COMMANDS_PER_SET; i++ )
	{
		if (m_commandWindows[ i ])
		{
			m_commandWindows[ i ]->winHide( TRUE );
		}
	}

	// sanity
	DEBUG_ASSERTCRASH( TheInGameUI->getSelectCount() > 1,
										 ("populateMultiSelect: Can't populate multiselect context cause there are only '%d' things selected",
										  TheInGameUI->getSelectCount()) );

	// get the list of drawable IDs from the in game UI
	const DrawableList *selectedDrawables = TheInGameUI->getAllSelectedDrawables();

	// sanity
	DEBUG_ASSERTCRASH( selectedDrawables->empty() == FALSE, ("populateMultiSelect: Drawable list is empty") );

	// loop through all the selected drawables
	for( DrawableListCIt it = selectedDrawables->begin();
			 it != selectedDrawables->end(); ++it )
	{

		// get the drawable
		draw = *it;


		if (draw->getObject()->isKindOf(KINDOF_IGNORED_IN_GUI)) // ignore these guys
			continue;


		//
		// add command for this drawable, note that we also sanity check to make sure the
		// drawable has an object as all interesting drawables that we can select should
		// actually have an object underneath it so that we can do interesting things with
		// it ... otherwise we should have never selected it.
		// NOTE that we're not considering objects that are currently in the process of
		// being sold as those objects can't be issued anymore commands
		//
		if( draw && draw->getObject() &&
				!draw->getObject()->getStatusBits().test( OBJECT_STATUS_SOLD ) )
		{

			// add the common commands of this drawable to the common command set
			addCommonCommands( draw, firstDrawable );

			// not adding the first drawable anymore
			firstDrawable = FALSE;

			//
			// keep track of the portrait images, if all units selected have the same portrait
			// we will display it in the right HUD, otherwise we won't
			//
			if( portraitSet == FALSE )
			{

				portrait = draw->getTemplate()->getSelectedPortraitImage();
				portraitObj = draw->getObject();
				portraitSet = TRUE;

			}
			else if( draw->getTemplate()->getSelectedPortraitImage() != portrait )
				portrait = nullptr;

		}

	}

	// set the portrait image
	setPortraitByObject( portraitObj );

	// GeneralsX @feature w1semannn 11/10/2026 Fill the top-left composition panel (one cameo + "x N" per distinct unit type). This no longer touches the bottom command bar.
	populateMultiSelectComposition();

}

//-------------------------------------------------------------------------------------------------
/** Reset the multi-select composition slot data */
//-------------------------------------------------------------------------------------------------
void ControlBar::resetMultiSelectData()
{
	for( Int i = 0; i < MAX_COMMANDS_PER_SET; i++ )
	{
		m_multiSelectData[ i ].control = nullptr;
		m_multiSelectData[ i ].thingTemplate = nullptr;
		m_multiSelectData[ i ].count = 0;
	}
}

//-------------------------------------------------------------------------------------------------
/** Populate the top-left composition panel with one cameo button per distinct unit type in the
	* current selection plus a readable "x N" count label. The bottom command bar is left untouched
	* so its normal multi-select commands (attack-move / guard / stop ...) stay available.
	* Clicking a cameo reselects every selected unit of that type (routed via compositionPanelSystem). */
//-------------------------------------------------------------------------------------------------
void ControlBar::populateMultiSelectComposition()
{
	resetMultiSelectData();

	// only meaningful with more than one thing selected
	if( TheInGameUI->getSelectCount() <= 1 )
	{
		hideCompositionPanel();
		return;
	}

	ensureCompositionPanel();
	if( m_compPanel == nullptr )
		return;

	const DrawableList *selectedDrawables = TheInGameUI->getAllSelectedDrawables();
	if( selectedDrawables == nullptr || selectedDrawables->empty() )
	{
		hideCompositionPanel();
		return;
	}

	// group the selection by thing template; each distinct type claims one panel row (slot)
	Int slot = 0;
	for( DrawableListCIt it = selectedDrawables->begin(); it != selectedDrawables->end(); ++it )
	{
		Drawable *draw = *it;
		if( draw == nullptr )
			continue;

		Object *obj = draw->getObject();
		if( obj == nullptr )
			continue;

		if( obj->isKindOf( KINDOF_IGNORED_IN_GUI ) )
			continue;

		const ThingTemplate *t = obj->getTemplate();
		if( t == nullptr )
			continue;

		// find a row already assigned to this type
		Int found = -1;
		for( Int s = 0; s < slot; s++ )
		{
			if( m_multiSelectData[ s ].thingTemplate
				&& m_multiSelectData[ s ].thingTemplate->isEquivalentTo( t ) )
			{
				found = s;
				break;
			}
		}

		// new type: claim the next row if a physical button still exists
		if( found < 0 )
		{
			if( slot >= MAX_COMMANDS_PER_SET || m_compButton[ slot ] == nullptr )
				continue; // pool exhausted; remaining distinct types fold into nothing shown

			found = slot;
			slot++;

			m_multiSelectData[ found ].control = m_compButton[ found ];
			m_multiSelectData[ found ].thingTemplate = t;
			m_multiSelectData[ found ].count = 0;

			// cameo image for the unit type
			GadgetButtonSetEnabledImage( m_compButton[ found ], t->getButtonImage() );
		}

		m_multiSelectData[ found ].count++;
	}

	// nothing to show (e.g. only ignored kinds selected)
	if( slot <= 0 )
	{
		hideCompositionPanel();
		return;
	}

	// layout: horizontal rows of up to COMP_PER_ROW [ cameo ][ xN ] cells, wrapping to a new row when full
	for( Int i = 0; i < MAX_COMMANDS_PER_SET; i++ )
	{
		GameWindow *btn = m_compButton[ i ];
		GameWindow *lbl = m_compCount[ i ];
		if( btn == nullptr || lbl == nullptr )
			continue;

		if( i < slot )
		{
			Int col = i % COMP_PER_ROW;
			Int row = i / COMP_PER_ROW;
			btn->winSetPosition( col * COMP_CELL_STEP, row * COMP_ROW_STEP );
			lbl->winSetPosition( col * COMP_CELL_STEP + COMP_BTN_SIZE, row * COMP_ROW_STEP );

			btn->winHide( FALSE );
			btn->winEnable( TRUE );
			btn->winClearStatus( WIN_STATUS_NOT_READY );
			btn->winClearStatus( WIN_STATUS_ALWAYS_COLOR );
			GadgetButtonSetText( btn, UnicodeString::TheEmptyString );

			UnicodeString unibuffer;
			unibuffer.format( L"\u00D7%d", m_multiSelectData[ i ].count );
			GadgetStaticTextSetText( lbl, unibuffer );
			lbl->winHide( FALSE );
		}
		else
		{
			btn->winHide( TRUE );
			lbl->winHide( TRUE );
		}
	}

	// cache the content size (widest row) so the position re-anchor can bottom-align the block
	Int rows = (slot + COMP_PER_ROW - 1) / COMP_PER_ROW;
	Int cols = (slot > COMP_PER_ROW) ? COMP_PER_ROW : slot;
	m_compPanelW = cols * COMP_CELL_STEP - COMP_CELL_GAP + COMP_PANEL_PAD;
	m_compPanelH = rows * COMP_ROW_STEP + COMP_PANEL_PAD;
	m_compPanel->winSetSize( m_compPanelW, m_compPanelH );
	m_compPanel->winHide( FALSE );

	updateCompositionPanelPosition();
}

//-------------------------------------------------------------------------------------------------
/** Re-anchor the composition panel just above the command bar. Reads the live position of the
	* command-bar master window (which slides down when the player squishes the HUD) so the panel
	* follows it and always stays over the map, bottom-left. Called each frame while multi-select is on. */
//-------------------------------------------------------------------------------------------------
void ControlBar::updateCompositionPanelPosition()
{
	if( m_compPanel == nullptr || m_compPanel->winIsHidden() )
		return;

	// default: bottom edge of the screen
	Int barTopY = TheDisplay ? TheDisplay->getHeight() : 0;

	// track the command bar master window when visible (its top edge is where the panel sits above)
	GameWindow *master = m_contextParent[ CP_MASTER ];
	if( master && !master->winIsHidden() )
	{
		Int mx = 0, my = 0;
		master->winGetPosition( &mx, &my );
		barTopY = my;
	}

	Int py = barTopY - m_compPanelH - COMP_GAP_Y;
	if( py < 0 )
		py = 0;

	m_compPanel->winSetPosition( COMP_EDGE_X, py );
}

//-------------------------------------------------------------------------------------------------
/** Lazily build the composition panel: a plain transparent container window (which also
	* acts as the click "owner" for its buttons) plus one cameo button and one count label per slot. */
//-------------------------------------------------------------------------------------------------
void ControlBar::ensureCompositionPanel()
{
	if( m_compPanel != nullptr )
		return;

	if( TheWindowManager == nullptr )
		return;

	const Int BTN_SIZE = 48;
	const Int ROW_STEP = 52;
	const Int LBL_X   = BTN_SIZE + 8;
	const Int LBL_W   = 84;

	// container is a top-level window so its coordinates are absolute screen pixels (top-left)
	WinInstanceData instData;
	instData.init();
	m_compPanel = TheWindowManager->winCreate( nullptr,
													WIN_STATUS_ENABLED,
													8, 8,
													LBL_X + LBL_W, MAX_COMMANDS_PER_SET * ROW_STEP,
													compositionPanelSystem,
													&instData );
	if( m_compPanel == nullptr )
		return;

	m_compPanel->winHide( TRUE );

	// find one readable font for all count labels
	GameFont *countFont = TheWindowManager->winFindFont( "Arial", 22, TRUE );

	for( Int i = 0; i < MAX_COMMANDS_PER_SET; i++ )
	{
		// cameo button (image only, no default chrome so it just shows the unit icon)
		WinInstanceData btnData;
		btnData.init();
		BitSet( btnData.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK );
		m_compButton[ i ] = TheWindowManager->gogoGadgetPushButton( m_compPanel,
																	WIN_STATUS_ENABLED | WIN_STATUS_IMAGE,
																	0, i * ROW_STEP,
																	BTN_SIZE, BTN_SIZE,
																	&btnData, nullptr, FALSE );
		if( m_compButton[ i ] )
			m_compButton[ i ]->winHide( TRUE );

		// count label
		WinInstanceData lblData;
		TextData textData;
		lblData.init();
		memset( &textData, 0, sizeof( textData ) );
		textData.centered = 0;
		lblData.m_style = GWS_STATIC_TEXT;
		m_compCount[ i ] = TheWindowManager->gogoGadgetStaticText( m_compPanel,
																WIN_STATUS_ENABLED,
																LBL_X, i * ROW_STEP,
																LBL_W, BTN_SIZE,
																&lblData, &textData, nullptr, FALSE );
		if( m_compCount[ i ] )
		{
			if( countFont )
				m_compCount[ i ]->winSetFont( countFont );
			// bright gold text with a solid black border for maximum readability over the battlefield
			m_compCount[ i ]->winSetEnabledTextColors( GameMakeColor( 255, 235, 90, 255 ),
																GameMakeColor( 0, 0, 0, 255 ) );
			m_compCount[ i ]->winHide( TRUE );
		}
	}
}

//-------------------------------------------------------------------------------------------------
/** Hide the composition panel and clear its slot data (called when leaving the multi-select context). */
//-------------------------------------------------------------------------------------------------
void ControlBar::hideCompositionPanel()
{
	for( Int i = 0; i < MAX_COMMANDS_PER_SET; i++ )
	{
		if( m_compButton[ i ] )
			m_compButton[ i ]->winHide( TRUE );
		if( m_compCount[ i ] )
			m_compCount[ i ]->winHide( TRUE );
	}

	if( m_compPanel )
		m_compPanel->winHide( TRUE );

	resetMultiSelectData();
}

//-------------------------------------------------------------------------------------------------
/** System callback for the composition container. Push button clicks are delivered here (the
	* buttons were created with this container as their parent/owner) as GBM_SELECTED. */
//-------------------------------------------------------------------------------------------------
static WindowMsgHandledType compositionPanelSystem( GameWindow *window, UnsignedInt msg,
															WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{
		case GBM_SELECTED:
		case GBM_SELECTED_RIGHT:
		{
			GameWindow *btn = (GameWindow *)mData1;
			if( btn && TheControlBar )
				TheControlBar->handleMultiSelectCompositionClick( btn );
			return MSG_HANDLED;
		}

		default:
			return MSG_IGNORED;
	}
}

//-------------------------------------------------------------------------------------------------
/** Update logic for the multi select context sensitive GUI */
//-------------------------------------------------------------------------------------------------
void ControlBar::updateContextMultiSelect()
{
	Drawable *draw;
	Object *obj;
	const CommandButton *command;
	GameWindow *win;
	Int objectsThatCanDoCommand[ MAX_COMMANDS_PER_SET ];
	Int i;

	// zero the array that counts how many objects can do each command
	memset( objectsThatCanDoCommand, 0, sizeof( objectsThatCanDoCommand ) );

	// sanity
	DEBUG_ASSERTCRASH( TheInGameUI->getSelectCount() > 1,
										 ("updateContextMultiSelect: TheInGameUI only has '%d' things selected",
										  TheInGameUI->getSelectCount()) );

	// get the list of drawable IDs from the in game UI
	const DrawableList *selectedDrawables = TheInGameUI->getAllSelectedDrawables();

	// sanity
	DEBUG_ASSERTCRASH( selectedDrawables->empty() == FALSE, ("populateMultiSelect: Drawable list is empty") );

	// loop through all the selected drawable IDs
	for( DrawableListCIt it = selectedDrawables->begin();
			 it != selectedDrawables->end(); ++it )
	{

		// get the drawable from the ID
		draw = *it;

		if (draw->getObject()->isKindOf(KINDOF_IGNORED_IN_GUI)) // ignore these guys
			continue;


		// get the object
		obj = draw->getObject();

		// sanity
		if( obj == nullptr )
			continue;

		// for each of the visible command windows make sure the object can execute the command
		for( i = 0; i < MAX_COMMANDS_PER_SET; i++ )
		{

			// get the control window
			win = m_commandWindows[ i ];

			// our implementation doesn't necessarily make use of the max possible command buttons
			if (!win) continue;

			// don't consider hidden windows
			if( win->winIsHidden() == TRUE )
				continue;

			// get the command
			command = (const CommandButton *)GadgetButtonGetData(win);
			if( command == nullptr )
				continue;

			// can we do the command
			CommandAvailability availability = getCommandAvailability( command, obj, win );

			win->winClearStatus( WIN_STATUS_NOT_READY );
			win->winClearStatus( WIN_STATUS_ALWAYS_COLOR );

			// enable/disable the window control
			switch( availability )
			{
				case COMMAND_HIDDEN:
					win->winHide( TRUE );
					break;
				case COMMAND_RESTRICTED:
					win->winEnable( FALSE );
					break;
				case COMMAND_NOT_READY:
					win->winEnable( FALSE );
					win->winSetStatus( WIN_STATUS_NOT_READY );
					break;
				case COMMAND_CANT_AFFORD:
					win->winEnable( FALSE );
					win->winSetStatus( WIN_STATUS_ALWAYS_COLOR );
					break;
				default:
					win->winEnable( TRUE );
					break;
			}

			//If button is a CHECK_LIKE, then update it's status now.
			if( BitIsSet( command->getOptions(), CHECK_LIKE ) )
			{
				GadgetCheckLikeButtonSetVisualCheck( win, availability == COMMAND_ACTIVE );
			}

			if( availability == COMMAND_AVAILABLE || availability == COMMAND_ACTIVE )
					objectsThatCanDoCommand[ i ]++;

		}

	}

	//
	// for each command, if any objects can do the command we enable the window, otherwise
	// we disable it
	//
	for( i = 0; i < MAX_COMMANDS_PER_SET; i++ )
	{
		// our implementation doesn't necessarily make use of the max possible command buttons
		if (! m_commandWindows[ i ]) continue;

		// don't consider hidden commands
		if( m_commandWindows[ i ]->winIsHidden() == TRUE )
			continue;

		// don't consider slots that don't have commands
		if( m_commonCommands[ i ] == nullptr )
			continue;

		// check the count of objects that can do the command and enable/disable the control,
		if( objectsThatCanDoCommand[ i ] > 0 )
			m_commandWindows[ i ]->winEnable( TRUE );
		else
			m_commandWindows[ i ]->winEnable( FALSE );

	}

	// After Every change to the m_commandWIndows, we need to show fill in the missing blanks with the images
	// removed from multiplayer branch
	//showCommandMarkers();

	// GeneralsX @feature w1semannn 11/10/2026 Keep the composition panel glued above the command bar every frame so it slides down with the HUD when the player squishes the control bar.
	updateCompositionPanelPosition();

}
