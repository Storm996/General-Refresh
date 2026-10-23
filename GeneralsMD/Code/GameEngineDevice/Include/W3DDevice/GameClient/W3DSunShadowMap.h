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
// GeneralsX @refactor Qoder 20/10/2026: the T6 depth-buffered sun shadow map is retired from the
// build. The shared Core W3DDisplay.cpp still calls the two frame hooks, so this target links the
// same inline no-op stub the base game uses; the retail projected (decal) and volumetric (stencil)
// shadow managers own shadows again because isWanted() now reports false.
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
		static bool isReceptionActive() { return false; }
		static float receptionStrength() { return 0.0f; }

		// False = the legacy shadow managers arm their queue (W3DScene.cpp) instead of standing down.
		static bool isWanted() { return false; }
};
