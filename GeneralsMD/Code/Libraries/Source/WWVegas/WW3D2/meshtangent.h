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

// GeneralsX @feature Qoder 10/10/2026 T3-0 M2 (段D)
// Per-vertex tangent-space basis (Lengyel's method) used by the dot3 normal-map path.
// Pure render-side helper: it only reads existing mesh geometry (position / normal / UV /
// triangle indices) and writes freshly allocated tangent & binormal arrays. It never
// touches GameLogic or WWMath simulation state.

#pragma once

#include "WWMath/vector3.h"

class MeshModelClass;

// Compute a per-vertex tangent (S) and binormal (T) basis for the given rigid mesh using
// the classic Lengyel accumulation: for every triangle the UV-edge system is solved to
// obtain a face tangent, which is then accumulated onto its three vertices and finally
// normalised. Seam handling relies on the W3D convention that vertices split across a UV
// seam already carry distinct indices (each with its own UV), so index-space accumulation
// naturally yields a per-(position,uv) tangent without an extra split pass.
//
// The output arrays are allocated here (W3DNEWARRAY) with one Vector3 per mesh vertex and
// are owned by the caller (MeshModelClass), which is responsible for releasing them.
// Vertices that belong to no UV-mapped triangle fall back to an orthonormal basis derived
// from the vertex normal so that the tangent space is never degenerate.
//
// Returns true on success (arrays filled). Returns false and leaves the output pointers
// null when the mesh lacks any of the required source arrays.
bool Compute_Mesh_Tangents(
	MeshModelClass* mesh,
	Vector3** out_tangent,
	Vector3** out_binormal);
