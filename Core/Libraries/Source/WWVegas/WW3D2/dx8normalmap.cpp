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

#include "d3d8.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

namespace DX8NormalMap
{
	static unsigned g_VS = 0;
	static unsigned g_PS = 0;
	static bool     g_Enabled = false;      // rule: default OFF
	static float    g_Strength = 0.8f;      // per-pixel perturbation strength

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

	void Install(unsigned vs_handle, unsigned ps_handle)
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

		// --- bind textures / switch shaders ---
		DX8Wrapper::Set_Texture(1, nm);
		DX8Wrapper::Set_Vertex_Shader(g_VS);
		DX8Wrapper::Set_Pixel_Shader(g_PS);
		return true;
	}

	void Restore(unsigned fvf)
	{
		DX8Wrapper::Set_Pixel_Shader(0);
		DX8Wrapper::Set_Vertex_Shader(fvf);
		DX8Wrapper::Set_Texture(1, nullptr);
	}
}
