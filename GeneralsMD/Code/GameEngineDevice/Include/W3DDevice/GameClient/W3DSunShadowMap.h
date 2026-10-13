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
// GeneralsX @feature PS3.0 T6 - real depth-buffered sun shadow map.
//
// Two hooks per frame, both driven from W3DDisplay::draw():
//   preSceneUpdate()      renders every registered shadow caster from the sun's ortho view into
//                         an R32F depth map (occluder pass through SpecialRenderInfoClass::SunSink).
//   renderTerrainReceiver() draws a terrain-conforming grid with a 3x3 PCF shader that multiplies
//                         the already-drawn scene where the map says the caster geometry occludes it.
//
// Off by default; GENERALSX_SHADOWMAP=1 turns it on (Windows/DXVK V1 scope, same runtime-HLSL
// boundary as the T2 blur pass).  The base game links an inline no-op stub of this class so the
// shared Core W3DDisplay.cpp compiles unchanged for both targets.
//
///////////////////////////////////////////////////////////////////////////////

#pragma once

class CameraClass;

class W3DSunShadowMap
{
	public:
		// sceneCamera: the primary view's camera, used for the visible-terrain window.
		static void preSceneUpdate(CameraClass *sceneCamera);
		static void renderTerrainReceiver(CameraClass *sceneCamera);

		// Device-reset plumbing. Called from W3DProjectedShadowManager's ReleaseResources /
		// ReAcquireResources so the map's POOL_DEFAULT textures never outlive the device.
		static void releaseResources();
		static void reAcquireResources();
};
