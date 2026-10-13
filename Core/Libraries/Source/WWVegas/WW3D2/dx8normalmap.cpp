/***********************************************************************************************
***  GeneralsX @feature T3-0 法线贴图 dot3 引擎对接 - WW3D2 侧挂载模块实现                      ***
***                                                                                           ***
***  默认关闭 (g_Enabled=false)。仅当设备层 Install() 成功注入 VS/PS 句柄、且开关命中、且当前    ***
***  mesh 处于 TG3 顶点格式 (Uses_Normal_Map) 时, Render 循环才调用 Apply/Restore。任一前置缺失  ***
***  立即返回 false, 渲染严格走原固定管线 - 零回归。                                            ***
***********************************************************************************************/
#include "dx8normalmap.h"

#include "WWLib/always.h"
#include "dx8wrapper.h"
#include "WWMath/matrix3.h"
#include "WWMath/matrix4.h"
#include "WWMath/vector3.h"
#include "WWMath/vector4.h"
#include "lightenvironment.h"
#include "texture.h"
#include "assetmgr.h"
#include "ww3d.h"
#include "w3d_file.h"          // WW3D_FORMAT_U8V8, MIP_LEVELS_1
#include "WWLib/ffactory.h"

#include "d3d9.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace DX8NormalMap
{
	// GeneralsX  d3d9 port 11/10/2026 - D3D9 shader handles are interface pointers (UINT_PTR carries them on x64).
	static UINT_PTR g_VS = 0;
	static UINT_PTR g_PS = 0;
	static bool     g_Enabled = false;      // rule: default OFF
	static float    g_Strength = 0.8f;      // per-pixel perturbation strength

	// GeneralsX @feature PS3.0 T5 13/10/2026 multi-light pair + kill switch (env read once).
	static UINT_PTR g_VS_ML = 0;
	static UINT_PTR g_PS_ML = 0;

	void Install_ML(UINT_PTR vs_handle, UINT_PTR ps_handle)
	{
		g_VS_ML = vs_handle;
		g_PS_ML = ps_handle;
	}

	static bool ML_Enabled()
	{
		static int cached = -1;
		if (cached < 0) {
			const char* ev = getenv("GENERALSX_MULTILIGHT");
			cached = (ev != nullptr && strcmp(ev, "0") == 0) ? 0 : 1;
		}
		return cached == 1 && g_VS_ML != 0 && g_PS_ML != 0;
	}

	// GeneralsX @feature T3-0 P1: global sun fallback (direction TOWARD the light source - same
	// convention as LightEnvironmentClass::Get_Light_Direction - plus object ambient) for meshes
	// that carry no per-mesh light environment (battlefield objects). Injected by
	// W3DDisplay::setTimeOfDay; until then a neutral up-pointing sun keeps Apply functional.
	static Vector3  g_FallbackDir(0.0f, 0.0f, 1.0f);
	static Vector3  g_FallbackAmbient(0.5f, 0.5f, 0.5f);

	void Set_Global_Fallback_Light(const Vector3& world_dir, const Vector3& ambient)
	{
		g_FallbackDir = world_dir;
		g_FallbackAmbient = ambient;
	}

	// GeneralsX @tweak T3-0 showcase mode: GENERALSX_NORMALMAP_SHOWCASE=1 exaggerates the bump
	// (6x in-plane normal deviation + fixed oblique tangent-space light + low ambient) so the
	// dot3 pipeline effect is unmistakably visible for verification. Never active by default.
	static bool Showcase_On()
	{
		static int cached = -1;
		if (cached < 0) {
			const char* ev = getenv("GENERALSX_NORMALMAP_SHOWCASE");
			cached = (ev != nullptr && strcmp(ev, "0") != 0) ? 1 : 0;
		}
		return cached == 1;
	}

	// Simple single-slot cache: avoid re-deriving/re-looking-up when consecutive meshes share a diffuse.
	static const char* g_LastDiffuse = nullptr;
	static TextureClass* g_CachedNormal = nullptr;

	void Install(UINT_PTR vs_handle, UINT_PTR ps_handle)
	{
		g_VS = vs_handle;
		g_PS = ps_handle;
	}

	void Set_Enabled(bool on)      { g_Enabled = on; }
	bool Is_Enabled()              { return g_Enabled; }
	bool Ready_To_Render()         { return g_Enabled && g_VS != 0 && g_PS != 0; }

	void Set_Strength(float s)     { g_Strength = s; }
	float Get_Strength()           { return g_Strength; }

	// Derive the companion-normal name from the diffuse name and fetch the cached texture object
	// (M1 already loaded it under the same name into the asset hash).
	static TextureClass* Find_Companion_Normal(TextureClass* diffuse)
	{
		if (diffuse == nullptr) return nullptr;
		const StringClass& dn = diffuse->Get_Texture_Name();
		const char* raw = (const char*)dn;
		if (raw == nullptr || raw[0] == 0) return nullptr;

		// Cache hit?
		if (g_CachedNormal != nullptr && g_LastDiffuse != nullptr && strcmp(g_LastDiffuse, raw) == 0) {
			return g_CachedNormal;
		}

		char base[256];
		int i = 0;
		for (; raw[i] != 0 && i < 255; ++i) base[i] = raw[i];
		base[i] = 0;
		char* dot = strrchr(base, '.');
		if (dot != nullptr) *dot = 0;
		const int baseLen = (int)strlen(base);
		if (baseLen >= 3 && strcmp(base + baseLen - 3, "_nm") == 0) return nullptr;
		if (baseLen >= 6 && strcmp(base + baseLen - 6, "_nm_ai") == 0) return nullptr;

		static const char* const kSuffixes[] = { "_nm_ai", "_nm" };
		static const char* const kExts[]     = { ".tga", ".dds" };
		const char* found = nullptr;
		char candidate[300];
		for (int s = 0; s < 2 && found == nullptr; ++s) {
			for (int e = 0; e < 2 && found == nullptr; ++e) {
				snprintf(candidate, sizeof(candidate), "%s%s%s", base, kSuffixes[s], kExts[e]);
				FileClass* file = _TheFileFactory->Get_File(candidate);
				if (file != nullptr) {
					file->Open();
					bool avail = file->Is_Available();
					file->Close();
					_TheFileFactory->Return_File(file);
					if (avail) found = candidate;
				}
			}
		}
		if (found == nullptr) return nullptr;

		// Load the companion as full RGB (A8R8G8B8) so the blue Z channel survives. The earlier
		// U8V8 (2-channel bump) request dropped Z, so the shader's _bx2 read Z as -1 (backward normal),
		// making every dot3 negative -> dp3_sat 0 -> the surface rendered pure black.
		WW3DAssetManager* am = WW3DAssetManager::Get_Instance();
		if (am == nullptr) return nullptr;
		TextureClass* nm = am->Get_Texture(found, MIP_LEVELS_ALL, WW3D_FORMAT_A8R8G8B8);

		g_LastDiffuse = raw;   // the diffuse name is owned by its object, outliving this cache slot
		g_CachedNormal = nm;

		return nm;
	}

	bool Apply(const Matrix3D& world, LightEnvironmentClass* lenv, TextureClass* diffuse)
	{
		// GeneralsX @bugfix Qoder 11/10/2026 T3-0: lenv may be NULL on battlefield objects
		// (they rely on the global sun) - that is a LEGAL case handled by the fallback light
		// and ambient below. The old guard rejected it, so such meshes intermittently skipped
		// Apply frame-to-frame (NULL/non-NULL lenv alternation) and flickered between the
		// dot3 pass and the fixed pipeline - the "snow/plain texture swap" flicker the user saw.
		if (!Ready_To_Render() || diffuse == nullptr) {
			return false;
		}

		// --- bind companion normal texture to stage1 ---
		TextureClass* nm = Find_Companion_Normal(diffuse);
		if (nm == nullptr) {
			return false;
		}

		// --- WVP for the custom vertex shader ---
		// Mirror the proven water-shader convention (W3DWater.cpp): read view/proj in raw DX8
		// format via _Get_DX8_Transform, build world in the same format, multiply W*V*P, then
		// transpose once before loading into VS constants c0..c3. Using Get_Transform (Westwood
		// row-vector) here produced a WVP whose w-term lost the translation contribution and
		// flipped sign, so the perspective divide flung vertices to infinity (radial lines).
		Matrix4x4 view;
		Matrix4x4 proj;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, view);
		DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, proj);

		Matrix4x4 wm(world);                 // Westwood format (also used for lighting below)
		Matrix4x4 wmDx8 = wm.Transpose();     // convert to DX8 format to match view/proj
		Matrix4x4 wvp = wmDx8 * view;
		wvp = wvp * proj;
		wvp = wvp.Transpose();                 // VS constants need the transpose

		const float* s = reinterpret_cast<const float*>(&wvp);
		DX8Wrapper::Set_Vertex_Shader_Constant(0, s, 4);   // c0..c3 = WVP

		// --- key light direction: ALWAYS the global sun (injected by W3DDisplay::setTimeOfDay),
		// so dot3 lighting matches the projected-shadow direction for every mesh. Some units
		// carry a per-mesh light environment whose direction disagrees with the global sun
		// (the listening truck read bright on its shadowed side) - ignore it for direction,
		// keep it for ambient only.
		const Vector3& lw = g_FallbackDir;

		// GeneralsX @tweak Qoder 11/10/2026 T3-0: the light now feeds the VS as a MODEL-SPACE
		// direction (c4) and the VS rotates it per-vertex into tangent space via the TBN basis
		// (v4/v5/v1 -> oD1). Faces therefore shade differently (roof bright, walls dark) instead
		// of one constant tangent-space light making every face the same flat darkness.
		const bool showcase = Showcase_On();
		Vector3 Lw = lw;
		{
			float dl2 = sqrtf(Lw.X * Lw.X + Lw.Y * Lw.Y + Lw.Z * Lw.Z);
			if (dl2 > 1e-6f) { Lw.X /= dl2; Lw.Y /= dl2; Lw.Z /= dl2; }
		}

		// Lm = wmDx8^T * Lw (rotation only). wmDx8 is col-major (m[r][c] at flat[r + 4c]), so
		// Lm[r] = sum_c wmDx8[c][r] * Lw[c] = sum_c m[c + 4r] * Lw[c].
		const float* m = reinterpret_cast<const float*>(&wmDx8);
		Vector4 c4;
		c4.X = m[0] * Lw.X + m[4] * Lw.Y + m[8] * Lw.Z;
		c4.Y = m[1] * Lw.X + m[5] * Lw.Y + m[9] * Lw.Z;
		c4.Z = m[2] * Lw.X + m[6] * Lw.Y + m[10] * Lw.Z;
		c4.W = 1.0f;
		DX8Wrapper::Set_Vertex_Shader_Constant(4, &c4, 1);   // c4 = model-space light (VS rotates via TBN)

		// GeneralsX @feature PS3.0 T1 13/10/2026 Blinn-Phong eye vector. The camera's +Z axis in world
		// space (scene->eye; it looks down -Z) is, in the raw DX8 view format, the same "axis r lives at
		// flat[r], flat[r+4], flat[r+8]" pattern the light above uses, with r=2. Rotated into model space
		// exactly like c4 and fed to the VS as c5, where it joins the light to form the half-vector sum.
		const float* vm = reinterpret_cast<const float*>(&view);
		Vector3 Ew(vm[2], vm[6], vm[10]);
		{
			float el2 = sqrtf(Ew.X * Ew.X + Ew.Y * Ew.Y + Ew.Z * Ew.Z);
			if (el2 > 1e-6f) { Ew.X /= el2; Ew.Y /= el2; Ew.Z /= el2; }
			else { Ew = Vector3(0.0f, 0.0f, 1.0f); }   // degenerate view matrix: headlight fallback
		}
		Vector4 c5eye;
		c5eye.X = m[0] * Ew.X + m[4] * Ew.Y + m[8] * Ew.Z;
		c5eye.Y = m[1] * Ew.X + m[5] * Ew.Y + m[9] * Ew.Z;
		c5eye.Z = m[2] * Ew.X + m[6] * Ew.Y + m[10] * Ew.Z;
		c5eye.W = 1.0f;
		DX8Wrapper::Set_Vertex_Shader_Constant(5, &c5eye, 1); // c5 = model-space eye direction

		// --- PS constants: c1 = strength, c2 = ambient, c5 = flat normal for NOSTAGE1 diag ---
		Vector4 c5(0.0f, 0.0f, 1.0f, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(5, &c5, 1);

		const Vector3& amb = (lenv != nullptr) ? lenv->Get_Equivalent_Ambient() : g_FallbackAmbient;
		float ambient = (amb.X + amb.Y + amb.Z) * (1.0f / 3.0f);
		float strength = g_Strength;
		Vector3 ambColor(ambient, ambient, ambient);
		if (showcase) {
			// GeneralsX @tweak Qoder 11/10/2026 T3-0: the exaggerated showcase params are gone;
			// they caused the harsh light/dark cliff on low-poly faces (near-black backfaces +
			// 2.2x frontfaces + ultra-oblique light). The showcase flag now only bumps strength
			// slightly and keeps the REAL sun direction + REAL ambient, so rotation no longer
			// snaps faces between black and blown-out.
			strength = 0.9f;
		}
		Vector4 sc(strength, 0.0f, 0.0f, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(1, &sc, 1);          // c1 = strength
		Vector4 ac(ambColor.X, ambColor.Y, ambColor.Z, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(2, &ac, 1);          // c2 = ambient
		const float xyScale = 2.5f;
		// GeneralsX @tweak Qoder 11/10/2026 T3-0: c3 = (k, 0, 1, 0) feeds the PS exaggeration
		// (mad r3.xy = r1_bx2.xy * k ; mad r3.z = r1_bx2.z * 1). k=2.5 makes building bumps clearly
		// visible under the RTS top-down camera.
		Vector4 bs(xyScale, 0.0f, 1.0f, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(3, &bs, 1);          // c3 = xy exaggeration k, z passthrough

		// GeneralsX @feature PS3.0 T1 13/10/2026: c6 = (specPow, specIntensity, -, -) for the Blinn-Phong
		// term in the main dot3 PS. Subtle by default (a moving highlight along bump edges, not a second
		// sun); QA knobs GENERALSX_NORMALMAP_SPEC (intensity) / GENERALSX_NORMALMAP_SPECPOWER.
		{
			static float specPow = -1.0f;
			static float specInt = -1.0f;
			if (specPow < 0.0f) {
				specPow = 24.0f;
				specInt = 0.12f;
				const char* evPow = getenv("GENERALSX_NORMALMAP_SPECPOWER");
				if (evPow != nullptr && *evPow != 0) specPow = (float)atof(evPow);
				const char* evInt = getenv("GENERALSX_NORMALMAP_SPEC");
				if (evInt != nullptr && *evInt != 0) specInt = (float)atof(evInt);
				if (specPow < 1.0f) specPow = 1.0f;
				if (specInt < 0.0f) specInt = 0.0f;
			}
			Vector4 sc6(specPow, specInt, 0.0f, 0.0f);
			DX8Wrapper::Set_Pixel_Shader_Constant(6, &sc6, 1);
		}

		// --- bind textures / switch shaders ---
		// GeneralsX @feature PS3.0 T5 13/10/2026 multi-light branch: when this mesh's light
		// environment actually carries point lights (crates fire, superweapon blasts, lamp posts)
		// and the device layer installed the ML pair, bind that instead - the PS loops over up to
		// four lights per pixel with radius falloff.  No point lights (the overwhelming common
		// case) or GENERALSX_MULTILIGHT=0 keeps the T1 single-light path bit-for-bit.
		if (lenv != nullptr && ML_Enabled())
		{
			Vector4 mlLights[8];
			int mlCount = 0;
			const int total = lenv->Get_Light_Count();
			for (int i = 0; i < total && mlCount < 4; ++i)
			{
				if (!lenv->isPointLight(i)) continue;
				const Vector3& c = lenv->getPointCenter(i);
				const Vector3& d = lenv->getPointDiffuse(i);
				mlLights[mlCount * 2]     = Vector4(c.X, c.Y, c.Z, lenv->getPointIrad(i));
				mlLights[mlCount * 2 + 1] = Vector4(d.X, d.Y, d.Z, lenv->getPointOrad(i));
				++mlCount;
			}

			if (mlCount > 0)
			{
				// GeneralsX @feature PS3.0 T5 13/10/2026: one-shot proof the multi-light branch is
				// actually taken at runtime (a mesh with a point light in its environment mounted the
				// ML VS/PS pair). Silent otherwise; A/B delta only shows when lights touch NM meshes.
				static int s_mlMounted = 0;
				if (s_mlMounted < 5)
				{
					++s_mlMounted;
					fprintf(stderr, "[ML] multi-light branch mounted: %d point light(s) on this mesh\n", mlCount);
					fflush(stderr);
				}
				// VS c7..c10 = world matrix rows (wmDx8 carries the translation in .w of the
				// position rows, matching the dot(pos, row) convention the WVP upload uses).
				DX8Wrapper::Set_Vertex_Shader_Constant(7, m, 4);
				// PS c4/c5 = WORLD-space sun/eye (the single-light pair uses model-space VS copies).
				Vector4 sunWorld(Lw.X, Lw.Y, Lw.Z, 0.0f);
				DX8Wrapper::Set_Pixel_Shader_Constant(4, &sunWorld, 1);
				Vector4 eyeWorld(Ew.X, Ew.Y, Ew.Z, 0.0f);
				DX8Wrapper::Set_Pixel_Shader_Constant(5, &eyeWorld, 1);
				Vector4 cnt((float)mlCount, 0.0f, 0.0f, 0.0f);
				DX8Wrapper::Set_Pixel_Shader_Constant(16, &cnt, 1);
				DX8Wrapper::Set_Pixel_Shader_Constant(17, &mlLights[0], 8);

				DX8Wrapper::Set_Texture(1, nm);
				DX8Wrapper::Set_Vertex_Shader(g_VS_ML);
				DX8Wrapper::Set_Pixel_Shader(g_PS_ML);
				return true;
			}
		}

		DX8Wrapper::Set_Texture(1, nm);
		DX8Wrapper::Set_Vertex_Shader(g_VS);
		DX8Wrapper::Set_Pixel_Shader(g_PS);
		return true;
	}

	void Restore(UINT_PTR fvf)
	{
		DX8Wrapper::Set_Pixel_Shader(0);
		// GeneralsX @bugfix Qoder 12/10/2026 - D3D9 keeps the vertex shader bound across SetFVF
		// (measured on native d3d9 and DXVK 2.6), unlike D3D8 where the FVF/shader handle were one
		// slot. Without this the dot3 vertex shader stayed installed for the next batch, whose
		// stale c0..c3 WVP / c4 light constants distorted its vertices.
		DX8Wrapper::Set_Vertex_Shader(0);
		// GeneralsX @build d3d9 port 11/10/2026 - D3D9 splits FVF from shader binding: the
		// fixed-pipeline format must go through Set_Vertex_FVF (SetFVF), not Set_Vertex_Shader.
		DX8Wrapper::Set_Vertex_FVF((DWORD)fvf);
		DX8Wrapper::Set_Texture(1, nullptr);
	}
}
