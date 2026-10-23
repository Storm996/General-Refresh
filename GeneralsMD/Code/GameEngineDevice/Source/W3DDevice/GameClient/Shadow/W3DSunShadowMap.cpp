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

// FILE: W3DSunShadowMap.cpp //////////////////////////////////////////////////
//
// GeneralsX @refactor Qoder 20/10/2026: retired from the build. The T6 depth-buffered sun shadow
// map never produced a usable visible result under the pure-Vulkan backend, so the class is now an
// inline no-op in the header and this translation unit is no longer listed in
// GeneralsMD/Code/GameEngineDevice/CMakeLists.txt. Shadow duty returns to the retail decal
// projection and stencil volume managers, which W3DScene.cpp re-arms because isWanted() is false.
//
///////////////////////////////////////////////////////////////////////////////

#include "W3DDevice/GameClient/W3DSunShadowMap.h"
