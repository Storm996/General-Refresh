/*
**	Command & Conquer Generals(tm)
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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : ww3d  (GeneralsX PS3.0 T6)                                  *
 *                                                                                             *
 *                     SunShadowSinkClass - geometry sink for the sun shadow-map occluder pass  *
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * The shadow-caster walk (SpecialRenderInfoClass RENDER_SHADOW with SunSink set) funnels every *
 * mesh through Submit_Screenspace_Triangles with clip-space vertices produced by               *
 * MeshGeometryClass::get_deformed_screenspace_vertices, so skinning, HLod parts and terrain    *
 * conform logic all come for free from the existing Special_Render machinery.                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "WWMath/matrix4.h"

// GeneralsX @feature Qoder 13/10/2026 PS3.0 T6 sun shadow map: abstract sink so the WW3D2 layer
// (meshmdl.cpp) never needs to know about the device-layer implementation that rasterizes the
// occluder geometry into the R32F depth map.
class SunShadowSinkClass
{
public:
	virtual ~SunShadowSinkClass() {}

	// clip_verts are homogeneous clip-space vertices in GL convention: x,y in [-1,1], z in [-1,1],
	// w == 1 for the ortho sun camera. The sink performs the D3D rebasing (z' = 0.5*z + 0.5) and
	// the viewport mapping itself.
	// indices are 16-bit triangle-list triplets (the TriIndex/Vector3i16 layout of MeshGeometryClass::Poly).
	virtual void Submit_Screenspace_Triangles(const Vector4 *clip_verts, int vcount,
		const unsigned short *indices, int tri_count) = 0;
};
