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

// FILE: MenuCommands.h //////////////////////////////////////////////////////////////////////////////
// GeneralsX @feature Qoder 07/10/2026 Declarative menu command framework (plan B).
//   A .wnd control may embed its action in its decorated NAME using the grammar
//       NAME = "<layout>.wnd:<label>@<verb> <args>"
//   (a normal widget name never contains '@'). MenuCommandRegistry parses that suffix and runs the
//   matching verb, so common menu operations (open a layout, pop the stack, ...) need no dedicated
//   C++ callback per button and no FunctionLexicon edits. See 界面升级/02 for the design rationale.
///////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

class GameWindow;

///
//-------------------------------------------------------------------------------------------------
struct MenuCommandRegistry
{
	/// If the control's decorated name carries an "@verb args" command, run it and return TRUE
	/// (the message is claimed). Return FALSE when there is no command so the caller's normal
	/// per-button handling proceeds untouched.
	static Bool dispatchControl( GameWindow *control );
};
