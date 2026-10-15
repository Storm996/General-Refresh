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

// FILE: W3DSunShadowMap.h ////////////////////////////////////////////////////
//
// GeneralsX @feature PS3.0 T6 sun shadow map - base-game stub.
//
// The shared Core W3DDisplay.cpp calls the two frame hooks; the real depth-map pass ships with
// Zero Hour only, so this target links inline no-ops and the retail shadow path is untouched.
//
///////////////////////////////////////////////////////////////////////////////

#pragma once

class CameraClass;

class W3DSunShadowMap
{
	public:
		static void preSceneUpdate(CameraClass *) {}
		static void renderTerrainReceiver(CameraClass *) {}
		static void releaseResources() {}
		static void reAcquireResources() {}
		// GeneralsX @feature PS3.0-P3-23 14/10/2026: no-op stubs for the S4 composition queries
		// (base game never runs the sun-map pass, so reception is always inactive).
		static bool isReceptionActive() { return false; }
		static float receptionStrength() { return 0.0f; }
};
