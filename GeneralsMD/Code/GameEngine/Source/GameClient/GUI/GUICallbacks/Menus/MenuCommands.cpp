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

// FILE: MenuCommands.cpp ///////////////////////////////////////////////////////////////////////////
// GeneralsX @feature Qoder 07/10/2026 Declarative menu command framework (plan B).
//   One reusable dispatcher turns a control's embedded "@verb args" name into an action, so
//   adding or removing a menu button becomes pure .wnd data. New verbs are added to s_verbs once
//   and every layout shares them; no per-button C++ and no FunctionLexicon edits are required.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"

#include "Common/AsciiString.h"
#include "Common/NameKeyGenerator.h"
#include "GameClient/GameWindow.h"
#include "GameClient/Gadget.h"
#include "GameClient/Shell.h"
#include "GameClient/GUICallbacks.h"
#include "GameClient/MenuCommands.h"

namespace
{
	// Command marker embedded in a control's decorated NAME. A normal widget name never contains
	// '@', so its absence means "not a declarative button" and the owning layout keeps handling it.
	const char kCmdMarker = '@';

	// A single command verb: a name plus the handler it maps to.
	struct MenuVerb
	{
		const char *name;
		void (*run)( const AsciiString &args );
	};

	// ---- built-in verbs -------------------------------------------------------------------------

	// push <layout path> : open a shell layout on top, e.g. NAME "...@push Menus/MissionSelect.wnd".
	void verbPush( const AsciiString &args )
	{
		if( !args.isEmpty() )
			TheShell->push( args );
	}

	// pop               : close the top layout (back / cancel buttons). NAME "...@pop".
	void verbPop( const AsciiString & /*args*/ )
	{
		TheShell->pop();
	}

	// The verb table. Extend here (one entry) to expose a new action to every menu at once.
	const MenuVerb s_verbs[] =
	{
		{ "push", verbPush },
		{ "pop",  verbPop  },
	};

	//-------------------------------------------------------------------------------------------------
	// GeneralsX @feature Qoder 07/10/2026 Split a decorated name into its "@verb args" payload.
	// Returns FALSE when the name carries no command marker.
	Bool extractCommand( GameWindow *control, AsciiString &verb, AsciiString &args )
	{
		if( !control )
			return FALSE;

		NameKeyType id = (NameKeyType)control->winGetWindowId();
		AsciiString fullName = TheNameKeyGenerator->keyToName( id );

		const char *at = fullName.find( kCmdMarker );
		if( !at )
			return FALSE;

		// Body is everything after '@'. Verb = up to the first space; args = the trimmed remainder.
		AsciiString body;
		body.set( at + 1 );

		const char *sp = body.find( ' ' );
		if( sp )
		{
			verb.set( body.str(), (Int)(sp - body.str()) );
			args.set( sp + 1 );
			args.trim();
		}
		else
		{
			verb = body;
			args.clear();
		}
		return !verb.isEmpty();
	}
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026
Bool MenuCommandRegistry::dispatchControl( GameWindow *control )
{
	AsciiString verb, args;
	if( !extractCommand( control, verb, args ) )
		return FALSE;

	for( Int i = 0; i < ARRAY_SIZE( s_verbs ); ++i )
	{
		if( verb.compareNoCase( s_verbs[i].name ) == 0 )
		{
			s_verbs[i].run( args );
			return TRUE;
		}
	}

	// A marker was present but the verb is unknown: report it visibly (DEBUG_LOG is compiled out in
	// release) and still claim the message, so a typo never silently falls through to unrelated
	// button handling.
	fprintf( stderr, "[MenuCommands] unknown command verb '%s' on control, ignoring\n", verb.str() );
	fflush( stderr );
	return TRUE;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 07/10/2026 Standalone system callback for fully data-driven layouts:
// set SYSTEMCALLBACK = "MenuButtonSystem" on a layout root and give each button an "@verb args"
// name; clicks are forwarded to the shared registry and focus is grabbed, so a simple new screen
// needs no bespoke C++ at all.
WindowMsgHandledType MenuButtonSystem( GameWindow *window, UnsignedInt msg,
																			 WindowMsgData mData1, WindowMsgData mData2 )
{
	switch( msg )
	{
		case GWM_INPUT_FOCUS:
		{
			if( mData1 == TRUE )
				*(Bool *)mData2 = TRUE;
			return MSG_HANDLED;
		}

		case GBM_SELECTED:
		{
			GameWindow *control = (GameWindow *)mData1;
			if( MenuCommandRegistry::dispatchControl( control ) )
				return MSG_HANDLED;
			break;
		}

		default:
			break;
	}

	return MSG_IGNORED;
}
