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
// Lengyel per-vertex tangent-space basis generation for the dot3 normal-map path.

#include "meshtangent.h"

#include <cmath>
#include "meshmdl.h"
#include "meshgeometry.h"
#include "WWDebug/wwdebug.h"
#include "WWLib/always.h"

// Local cross-product helper: the 2-argument Vector3::Cross_Product is only compiled when
// ALLOW_TEMPORARIES is defined, which this build does not enable, so wrap the available
// 3-argument (out-pointer) overload.
static inline Vector3 XP(const Vector3& a, const Vector3& b)
{
	Vector3 r;
	Vector3::Cross_Product(a, b, &r);
	return r;
}

// Build an arbitrary unit vector orthogonal to 'normal' (used as a fallback tangent when a
// vertex is not touched by any UV-mapped triangle, so the tangent space stays non-degenerate).
static void Make_Orthogonal_Basis(const Vector3& normal, Vector3& out_tangent, Vector3& out_binormal)
{
	Vector3 n = normal;
	if (n.Length2() < 1e-8f) {
		// Degenerate normal: fall back to +Z up.
		n.Set(0.0f, 0.0f, 1.0f);
	} else {
		n.Normalize();
	}

	// Pick the world axis least aligned with n to cross against, for a stable tangent.
	Vector3 ref;
	if (::fabs(n.X) <= ::fabs(n.Y) && ::fabs(n.X) <= ::fabs(n.Z)) {
		ref.Set(1.0f, 0.0f, 0.0f);
	} else if (::fabs(n.Y) <= ::fabs(n.Z)) {
		ref.Set(0.0f, 1.0f, 0.0f);
	} else {
		ref.Set(0.0f, 0.0f, 1.0f);
	}

	Vector3 t = XP(n, ref);
	if (t.Length2() < 1e-8f) {
		// ref was parallel to n (shouldn't happen given the least-aligned pick) - try another axis.
		ref.Set(0.0f, 0.0f, 1.0f);
		t = XP(n, ref);
	}
	t.Normalize();

	// Binormal = n x t so (t, b, n) is right-handed.
	out_tangent = t;
	out_binormal = XP(n, t);
	out_binormal.Normalize();
}

bool Compute_Mesh_Tangents(MeshModelClass* mesh, Vector3** out_tangent, Vector3** out_binormal)
{
	if (out_tangent) *out_tangent = nullptr;
	if (out_binormal) *out_binormal = nullptr;
	if (!mesh) return false;

	const int vertCount = mesh->Get_Vertex_Count();
	const int polyCount = mesh->Get_Polygon_Count();
	if (vertCount <= 0 || polyCount <= 0) return false;

	const Vector3* verts = mesh->Get_Vertex_Array();
	const Vector3* norms = mesh->Get_Vertex_Normal_Array();
	const TriIndex* polys = mesh->Get_Polygon_Array();
	const Vector2* uvs = mesh->Get_UV_Array_By_Index(0);
	if (!verts || !polys) return false;

	// A mesh without a stage-0 UV layer cannot contribute meaningful tangents.
	if (!uvs || !norms) return false;

	Vector3* tanAccum = W3DNEWARRAY Vector3[vertCount];
	Vector3* binAccum = W3DNEWARRAY Vector3[vertCount];
	unsigned char* touched = W3DNEWARRAY unsigned char[vertCount];
	if (!tanAccum || !binAccum || !touched) {
		delete[] tanAccum;
		delete[] binAccum;
		delete[] touched;
		return false;
	}

	for (int v = 0; v < vertCount; ++v) {
		tanAccum[v].Set(0.0f, 0.0f, 0.0f);
		binAccum[v].Set(0.0f, 0.0f, 0.0f);
		touched[v] = 0;
	}

	// Accumulate per-face tangents/binormals onto their vertices (Lengyel 2001).
	for (int p = 0; p < polyCount; ++p) {
		const int i0 = polys[p][0];
		const int i1 = polys[p][1];
		const int i2 = polys[p][2];
		if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= vertCount || i1 >= vertCount || i2 >= vertCount) {
			continue;
		}

		const Vector3& v0 = verts[i0];
		const Vector3& v1 = verts[i1];
		const Vector3& v2 = verts[i2];

		const Vector2& uv0 = uvs[i0];
		const Vector2& uv1 = uvs[i1];
		const Vector2& uv2 = uvs[i2];

		Vector3 edge1 = v1 - v0;
		Vector3 edge2 = v2 - v0;

		const float du1 = uv1.X - uv0.X;
		const float dv1 = uv1.Y - uv0.Y;
		const float du2 = uv2.X - uv0.X;
		const float dv2 = uv2.Y - uv0.Y;

		const float denom = du1 * dv2 - du2 * dv1;
		if (denom > -1e-12f && denom < 1e-12f) {
			// Degenerate UV triangle (zero area in texture space): skip, fallback covers it.
			continue;
		}
		const float r = 1.0f / denom;

		Vector3 tangent;
		tangent.X = r * (dv2 * edge1.X - dv1 * edge2.X);
		tangent.Y = r * (dv2 * edge1.Y - dv1 * edge2.Y);
		tangent.Z = r * (dv2 * edge1.Z - dv1 * edge2.Z);

		Vector3 binormal;
		binormal.X = r * (-du2 * edge1.X + du1 * edge2.X);
		binormal.Y = r * (-du2 * edge1.Y + du1 * edge2.Y);
		binormal.Z = r * (-du2 * edge1.Z + du1 * edge2.Z);

		tanAccum[i0] += tangent;
		tanAccum[i1] += tangent;
		tanAccum[i2] += tangent;
		binAccum[i0] += binormal;
		binAccum[i1] += binormal;
		binAccum[i2] += binormal;
		touched[i0] = 1;
		touched[i1] = 1;
		touched[i2] = 1;
	}

	// Gram-Schmidt re-orthogonalise the accumulated tangent against the vertex normal so the
	// stored basis stays consistent with the (possibly smoothed) normals actually rendered.
	Vector3* outT = W3DNEWARRAY Vector3[vertCount];
	Vector3* outB = W3DNEWARRAY Vector3[vertCount];
	if (!outT || !outB) {
		delete[] outT;
		delete[] outB;
		delete[] tanAccum;
		delete[] binAccum;
		delete[] touched;
		return false;
	}

	for (int v = 0; v < vertCount; ++v) {
		if (!touched[v] || tanAccum[v].Length2() < 1e-12f) {
			Make_Orthogonal_Basis(norms[v], outT[v], outB[v]);
			continue;
		}

		Vector3 n = norms[v];
		if (n.Length2() < 1e-8f) n.Set(0.0f, 0.0f, 1.0f);
		else n.Normalize();

		Vector3 t = tanAccum[v];
		// t = normalize(t - n * dot(t, n))
		t -= n * Vector3::Dot_Product(t, n);
		if (t.Length2() < 1e-12f) {
			Make_Orthogonal_Basis(n, outT[v], outB[v]);
			continue;
		}
		t.Normalize();

		// GeneralsX @bugfix PS3.0-#76 16/10/2026 preserve the UV chart handedness. The old code
		// rebuilt every binormal as the right-handed n x t, silently discarding the accumulated
		// (signed) bitangent. On a mirrored (negative-determinant) UV chart the true bitangent is
		// -(n x t), so shipping n x t there puts the second tangent axis backwards: the tangent-space
		// light keeps its T and N components but its B component flips sign, i.e. the green channel of
		// the normal map mirrors across those faces and the bump relief (and the Blinn/PBR specular
		// lobe that rides on the same basis) reads as lit from the opposite side of the bump. A LOCAL
		// detail error on the affected faces - which is all this fix claims to repair.
		// Scope note, corrected after #76b: this defect CANNOT be the "building lit by a light from
		// another direction" report. It leaves dot(L,N) untouched, so it cannot swing the macro
		// diffuse with the mesh placement angle, and it cannot affect roughness-only batches at all
		// (nm == nullptr runs with a flat (0,0,1) tangent normal, so the B row is never read). The
		// rotating-light symptom - reproduced by both normal maps and roughness - was the world ->
		// model rotate in the old dot3 path; see @bugfix PS3.0-#76b in Core/.../dx8normalmap.cpp.
		// The earlier draft of this comment quoted mirror_chart_probe.py negative-triangle counts and
		// a lighting-preview-studio A/B as evidence for the rotating-light symptom. Neither was
		// reproduced here, so both citations are dropped; the handedness fix stands on the derivation
		// above alone and matches what the skinned path already did in dx8renderer.cpp.
		// side = sign(dot(n x t, binAccum)) is the studio formula expressed in file-V UV space
		// (the studio flips V at load; dot(N x T, B_file) == dot(T x N, B_flipV) keeps them equal).
		// side == 0 (mixed-hand seam vertices) falls back to +1 exactly like the studio.
		Vector3 b = XP(n, t);
		if (Vector3::Dot_Product(b, binAccum[v]) < 0.0f) {
			b = -b;	// mirrored chart: keep the authored left-handed basis
		}
		b.Normalize();

		outT[v] = t;
		outB[v] = b;
	}

	delete[] tanAccum;
	delete[] binAccum;
	delete[] touched;

	*out_tangent = outT;
	*out_binormal = outB;
	return true;
}
