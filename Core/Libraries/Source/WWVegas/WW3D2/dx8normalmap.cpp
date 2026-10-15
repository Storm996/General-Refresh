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

// GeneralsX @feature PS3.0-P3-12 14/10/2026: hemispherical ambient strength (master report L5).
// The engine's ambient is a flat constant; multiplying an ambient*hemiscale tint by the WORLD-up
// component of the pixel normal (W3D is Z-up - see DX8NormalMap's g_FallbackDir) lifts sky-facing
// surfaces and leaves downward faces on the plain ambient, so shadowed ground no longer reads as
// the same value as an upward-lit wall. Uploaded as the GN-family PS c16 / ML PS c8 hemi rgb.
// READ ONCE per process; GENERALSX_HEMI=0 restores the flat-ambient look bit-for-bit.
static float Hemi_Ambient_K()
{
	static float cached = -1.0f;
	if (cached < 0.0f) {
		cached = 0.35f;
		const char* ev = getenv("GENERALSX_HEMI");
		if (ev != nullptr && *ev != 0) cached = (float)atof(ev);
		if (cached < 0.0f) cached = 0.0f;
	}
	return cached;
}

// GeneralsX @feature PS3.0-P3-14 14/10/2026: per-pixel distance fog for the GN family
// (PS c25/c26) and the multi-light dot3 path (PS c9/c10). WW3D2 must not read the game
// layer's GlobalData, so the display layer injects the pair through DX8NormalMap::Set_Fog
// (same layering precedent as Set_Sun_Shadow). colorStrength.w == 0 keeps every pixel
// bit-for-bit identical to the pre-fog shader (lerp with t=0). File-scope so both
// namespaces' Apply paths read the same state.
static Vector4 g_FogColorStrength(0.0f, 0.0f, 0.0f, 0.0f);   // rgb haze tint, w = strength
static Vector4 g_FogRamp(0.0f, 1.0f, 0.0f, 0.0f);            // x = eye-depth start, y = 1/length

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

	// GeneralsX @feature PS3.0-P4-1 15/10/2026 roughness/metalness (PBR) variant for the dot3
	// single-light path (光影三阶段/粗糙度引擎承接工作清单.md M1+M2).  The pair is compiled by the
	// device layer and carries the SAME VS constant layout as g_VS/g_PS (c0..c3 WVP, c4 model-space
	// light, c5 model-space eye, c9 model-space fill); the PS adds sampler s2 (the "<stem>_rp"
	// companion: R=roughness, G=metalness) and c8=(mode, LI, ENV, -) where mode selects the branch:
	//   0 = off (this pair never mounts), 1 = M1 roughness-driven Blinn, 2 = M2 GGX metallic-roughness.
	// Mounting additionally requires the diffuse to have a companion _rp texture; without one, without
	// handles, or without GENERALSX_PBR=1/2 in the process environment, the single-light path runs
	// bit-for-bit - zero regression.  The ML branch keeps priority: a mesh with live point lights
	// uses g_VS_ML/g_PS_ML exactly as before (the ML shader has no rp support by design).
	static UINT_PTR g_VS_PBR = 0;
	static UINT_PTR g_PS_PBR = 0;

	// GeneralsX @feature PS3.0 #70 15/10/2026: the PBR mode became runtime state armed by the device
	// layer (OptionPreferences::getPBRMode at shader install, options menu re-arms live). The env
	// read moved into the getter, and the mode no longer requires the normal-map switch: at mode>0
	// with the bump path off, Apply still mounts the PBR pair with a flat tangent normal (roughness
	// specular on the geometric normal). Default OFF keeps the single-light path bit-for-bit.
	static int g_PBRMode = 0;
	static int PBR_Mode()
	{
		return g_PBRMode;
	}

	// Radiance scaler for the diffuse lobe (bakes the 0..1 lambert into the legacy brightness;
	// GENERALSX_PBR_LI).
	// GeneralsX @tweak PS3.0 #70 15/10/2026: retuned 3.2 -> 1.0. The old value was calibrated in the
	// lighting-preview studio against *un-baked albedo*, but in-game the diffuse stage is still the
	// retail texture with baked lighting, so the GGX lambert rides on top of the baked light and
	// 3.2 doubled the light ("everything glows" report). 1.0 keeps parity with the retail look;
	// re-evaluate upward once un-baked albedo ships. The studio mirrors this default (viewer.html).
	static float PBR_LI()
	{
		static float cached = -1.0f;
		if (cached < 0.0f) {
			cached = 1.0f;
			const char* ev = getenv("GENERALSX_PBR_LI");
			if (ev != nullptr && *ev != 0) cached = (float)atof(ev);
			if (cached < 0.0f) cached = 0.0f;
		}
		return cached;
	}

	// Constant-hemisphere environment strength for the M2 ambient term (c8.z * F0; no IBL cubemap
	// at RTS scale, see 工作清单 §2 M2). GENERALSX_PBR_ENV, default 0.06.
	static float PBR_ENV()
	{
		static float cached = -1.0f;
		if (cached < 0.0f) {
			cached = 0.06f;
			const char* ev = getenv("GENERALSX_PBR_ENV");
			if (ev != nullptr && *ev != 0) cached = (float)atof(ev);
			if (cached < 0.0f) cached = 0.0f;
		}
		return cached;
	}

	void Install_PBR(UINT_PTR vs_handle, UINT_PTR ps_handle)
	{
		g_VS_PBR = vs_handle;
		g_PS_PBR = ps_handle;
		fprintf(stderr, "[PBR] variant pair installed vs=%p ps=%p mode=%d\n",
			(void*)vs_handle, (void*)ps_handle, PBR_Mode());
		fflush(stderr);
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

	// GeneralsX @feature PS3.0 L1 13/10/2026: global sky fill for the dot3 single-light path.
	// The fixed pipeline gets fill/bounce through real LightClass slots (W3DScene collects them),
	// but dot3 meshes bypass Lights[4] entirely, so the fill must be injected here too or the
	// NM-mounted units would keep the old single-key look while everything around them fills.
	// Zero color = feature off = bit-for-bit the old single-light path.
	static Vector3  g_FillDir(0.0f, 0.0f, 0.0f);     // "toward the fill light", world space
	static Vector3  g_FillColor(0.0f, 0.0f, 0.0f);   // premultiplied fill diffuse

	void Set_Global_Fill_Light(const Vector3& world_dir, const Vector3& color)
	{
		g_FillDir = world_dir;
		g_FillColor = color;
	}

	// GeneralsX @feature PS3.0-P3-14 14/10/2026: store the injected fog pair (see file-scope g_Fog*
	// above). Called by the display layer once per setting change; both Apply paths upload it.
	void Set_Fog(const Vector4& colorStrength, const Vector4& startInvLen)
	{
		g_FogColorStrength = colorStrength;
		g_FogRamp = startInvLen;
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

		// GeneralsX @performance PS3.0 #71 15/10/2026: the old single-slot caches (g_LastDiffuse/
		// g_CachedNormal, g_LastRPDiffuse/g_CachedRP) thrashed whenever consecutive meshes used
		// different diffuses - every miss re-ran 2-4 Get_File/Open/Is_Available BIG-archive probes on
		// the render thread per frame, which is the main CPU cost behind the "GPU <40% but 100fps"
		// report. Replaced by a 256-entry direct-index hash per channel, keyed on the diffuse file
		// name and caching NEGATIVE results too (most retail diffuses have no companion: first probe
		// stores nullptr, later frames are one strcmp). Collisions evict the older entry (correct,
		// just re-probes). TextureClass* lifetime assumption is unchanged from the slot caches: the
		// asset manager owns the instance for the session.
		struct Companion_Cache {
			struct Entry { char key[256]; TextureClass* tex; bool used; };
			Entry slots[256];
			Companion_Cache() { for (int i = 0; i < 256; ++i) { slots[i].key[0] = 0; slots[i].tex = nullptr; slots[i].used = false; } }
			static unsigned Hash(const char* s) {
				unsigned h = 2166136261u;
				for (; *s; ++s) h = (h ^ (unsigned char)*s) * 16777619u;
				return h & 255u;
			}
			// Returns the cached texture (nullptr = cached miss) and sets *hit.
			TextureClass* Peek(const char* key, bool& hit) {
				const int idx = (int)Hash(key);
				Entry& e = slots[idx];
				hit = e.used && strcmp(e.key, key) == 0;
				return hit ? e.tex : nullptr;
			}
			void Put(const char* key, TextureClass* tex) {
				if (strlen(key) >= sizeof(Entry::key)) return;   // pathological name: stay uncached
				Entry& e = slots[(int)Hash(key)];
				strcpy(e.key, key);
				e.tex = tex;
				e.used = true;
			}
		};
		static Companion_Cache g_NmCache;
		static Companion_Cache g_RPCache;

	void Install(UINT_PTR vs_handle, UINT_PTR ps_handle)
	{
		g_VS = vs_handle;
		g_PS = ps_handle;
	}

	void Set_Enabled(bool on)      { g_Enabled = on; }
	bool Is_Enabled()              { return g_Enabled; }
	bool Ready_To_Render()         { return g_Enabled && g_VS != 0 && g_PS != 0; }

	// GeneralsX @feature PS3.0 #70 15/10/2026: independent roughness toggle (see header).
	void Set_PBR_Mode(int mode)    { g_PBRMode = (mode >= 1 && mode <= 2) ? mode : 0; }
	int  Get_PBR_Mode()            { return g_PBRMode; }
	bool RP_Ready()                { return g_PBRMode > 0 && g_VS_PBR != 0 && g_PS_PBR != 0; }

	void Set_Strength(float s)     { g_Strength = s; }
	float Get_Strength()           { return g_Strength; }

	// GeneralsX @bugfix PS3.0-P4 #68 15/10/2026: team-colour skins must never take the dot3/PBR path.
	// Vehicles/buildings tint their HOUSECOLOR parts through the VertexMaterialClass diffuse/ambient
	// (W3DAssetManager::Recolor_Vertex_Material clones one tinted prototype per colour). The custom
	// dot3/PBR shaders light from the injected sun/ambient constants and the vertex COLOR0 stream and
	// do NOT read that material tint, so mounting a companion normal map on a house-colour skin drops
	// the team colour (the exact "all non-infantry house colours vanished" regression the full-library
	// _nm rollout detonated). ZHCD* are the exporter-generated house-colour diffuse atlases
	// (DIFFUSE_HOUSECOLOR_TEXTURE_PREFIX); "*housecolor*" covers the retail atlas names (housecolor2…).
	// Infantry skins (ZHCa_/ZHCd_) bake the colour into the texture variant on the CPU, so they stay
	// mountable. Skipping the companion here falls the part back to the fixed pipeline, material tint
	// included, bit-for-bit retail.
	static bool Is_TeamColor_Skin(const char* base)
	{
		if (base == nullptr) return true;
		char low[256];
		int i = 0;
		for (; base[i] != 0 && i < 255; ++i) {
			const char c = base[i];
			low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
		}
		low[i] = 0;
		if (i >= 255) return true;   // pathological name: refuse to mount
		if (strncmp(low, "zhcd", 4) == 0) return true;
		return strstr(low, "housecolor") != nullptr;
	}

	// Derive the companion-normal name from the diffuse name and fetch the cached texture object
	// (M1 already loaded it under the same name into the asset hash).
	static TextureClass* Find_Companion_Normal(TextureClass* diffuse)
	{
		if (diffuse == nullptr) return nullptr;
		const StringClass& dn = diffuse->Get_Texture_Name();
		const char* raw = (const char*)dn;
		if (raw == nullptr || raw[0] == 0) return nullptr;

		// Hash-cache lookup first: a hit (including a cached negative) skips all file probes.
		bool cacheHit = false;
		TextureClass* cachedNm = g_NmCache.Peek(raw, cacheHit);
		if (cacheHit) return cachedNm;

		char base[256];
		int i = 0;
		for (; raw[i] != 0 && i < 255; ++i) base[i] = raw[i];
		base[i] = 0;
		char* dot = strrchr(base, '.');
		if (dot != nullptr) *dot = 0;
		if (Is_TeamColor_Skin(base)) { g_NmCache.Put(raw, nullptr); return nullptr; }   // #68: house-colour skins keep the fixed pipeline
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
		if (found == nullptr) { g_NmCache.Put(raw, nullptr); return nullptr; }

		// Load the companion as full RGB (A8R8G8B8) so the blue Z channel survives. The earlier
		// U8V8 (2-channel bump) request dropped Z, so the shader's _bx2 read Z as -1 (backward normal),
		// making every dot3 negative -> dp3_sat 0 -> the surface rendered pure black.
		WW3DAssetManager* am = WW3DAssetManager::Get_Instance();
		if (am == nullptr) return nullptr;
		TextureClass* nm = am->Get_Texture(found, MIP_LEVELS_ALL, WW3D_FORMAT_A8R8G8B8);

		g_NmCache.Put(raw, nm);

		return nm;
	}

	// GeneralsX @feature PS3.0-P4-1 15/10/2026: companion roughness/metalness probe, mirroring
	// Find_Companion_Normal's pattern (loose/BIG "<base>_rp" .tga then .dds, full RGB load so R and
	// G survive, 256-entry hash cache with negative results, same contract as the normal probe).
	// The asset contract (工作清单 §1): R=roughness, G=metalness, B=0, A=255, 32bpp, same size as
	// the retail diffuse.

	static TextureClass* Find_Companion_RP(TextureClass* diffuse)
	{
		if (diffuse == nullptr) return nullptr;
		const StringClass& dn = diffuse->Get_Texture_Name();
		const char* raw = (const char*)dn;
		if (raw == nullptr || raw[0] == 0) return nullptr;

		bool cacheHit = false;
		TextureClass* cachedRp = g_RPCache.Peek(raw, cacheHit);
		if (cacheHit) return cachedRp;

		char base[256];
		int i = 0;
		for (; raw[i] != 0 && i < 255; ++i) base[i] = raw[i];
		base[i] = 0;
		char* dot = strrchr(base, '.');
		if (dot != nullptr) *dot = 0;
		if (Is_TeamColor_Skin(base)) { g_RPCache.Put(raw, nullptr); return nullptr; }   // #68/#70: roughness must skip house-colour skins too
		const int baseLen = (int)strlen(base);
		if (baseLen >= 3 && strcmp(base + baseLen - 3, "_rp") == 0) return nullptr;   // never probe our own output

		static const char* const kExts[] = { ".tga", ".dds" };
		const char* found = nullptr;
		char candidate[300];
		for (int e = 0; e < 2 && found == nullptr; ++e) {
			snprintf(candidate, sizeof(candidate), "%s_rp%s", base, kExts[e]);
			FileClass* file = _TheFileFactory->Get_File(candidate);
			if (file != nullptr) {
				file->Open();
				bool avail = file->Is_Available();
				file->Close();
				_TheFileFactory->Return_File(file);
				if (avail) found = candidate;
			}
		}
		if (found == nullptr) { g_RPCache.Put(raw, nullptr); return nullptr; }

		WW3DAssetManager* am = WW3DAssetManager::Get_Instance();
		if (am == nullptr) return nullptr;
		TextureClass* rp = am->Get_Texture(found, MIP_LEVELS_ALL, WW3D_FORMAT_A8R8G8B8);

		g_RPCache.Put(raw, rp);
		return rp;
	}

	bool Apply(const Matrix3D& world, LightEnvironmentClass* lenv, TextureClass* diffuse)
	{
		// GeneralsX @bugfix Qoder 11/10/2026 T3-0: lenv may be NULL on battlefield objects
		// (they rely on the global sun) - that is a LEGAL case handled by the fallback light
		// and ambient below. The old guard rejected it, so such meshes intermittently skipped
		// Apply frame-to-frame (NULL/non-NULL lenv alternation) and flickered between the
		// dot3 pass and the fixed pipeline - the "snow/plain texture swap" flicker the user saw.
		// GeneralsX @feature PS3.0 #70 15/10/2026: the bump path needs the normal-map switch; the
		// roughness path only needs the PBR pair + an rp companion. Either arm alone (or both)
		// mounts this pass; with nm==nullptr the PBR pair runs on a FLAT tangent normal.
		if (diffuse == nullptr) {
			return false;
		}
		const bool bumpOn = Ready_To_Render();
		if (!bumpOn && !RP_Ready()) {
			return false;
		}
		TextureClass* nm = bumpOn ? Find_Companion_Normal(diffuse) : nullptr;
		TextureClass* rp = RP_Ready() ? Find_Companion_RP(diffuse) : nullptr;
		if (nm == nullptr && rp == nullptr) {
			return false;
		}
		// Roughness-only mode never had the point-light ML pair to offer: if this mesh's light
		// environment collects point lights, leave it to the fixed pipeline (which still lights
		// them) instead of mounting the single-light PBR pair and losing those lights.
		if (nm == nullptr && lenv != nullptr) {
			const int total = lenv->Get_Light_Count();
			for (int i = 0; i < total; ++i) {
				if (lenv->isPointLight(i)) return false;
			}
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

		// GeneralsX @feature PS3.0 L1 13/10/2026 sky fill for the dot3 single-light path. The fill
		// rotates world->model exactly like the key light (c4) and leaves the VS as TEXCOORD3 via
		// c9; its premultiplied color lands in PS c7. When the display layer never injected a fill
		// (color == 0), c7 is black and the extra lambert term is a numerical no-op. The ML branch
		// below keeps its own layout and is intentionally fill-free (point lights dominate there).
		Vector3 Fw = g_FillDir;
		{
			float fl2 = sqrtf(Fw.X * Fw.X + Fw.Y * Fw.Y + Fw.Z * Fw.Z);
			if (fl2 > 1e-6f) { Fw.X /= fl2; Fw.Y /= fl2; Fw.Z /= fl2; }
			else { Fw = Vector3(0.0f, 0.0f, 0.0f); }
		}
		Vector4 c9fill;
		c9fill.X = m[0] * Fw.X + m[4] * Fw.Y + m[8] * Fw.Z;
		c9fill.Y = m[1] * Fw.X + m[5] * Fw.Y + m[9] * Fw.Z;
		c9fill.Z = m[2] * Fw.X + m[6] * Fw.Y + m[10] * Fw.Z;
		c9fill.W = 1.0f;
		DX8Wrapper::Set_Vertex_Shader_Constant(9, &c9fill, 1);  // c9 = model-space fill direction

		// --- PS constants: c1 = sun gain (fixed), c2 = ambient, c3 = bump deviation, c5 = flat normal for NOSTAGE1 diag ---
		Vector4 c5(0.0f, 0.0f, 1.0f, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(5, &c5, 1);

		const Vector3& amb = (lenv != nullptr) ? lenv->Get_Equivalent_Ambient() : g_FallbackAmbient;
		float ambient = (amb.X + amb.Y + amb.Z) * (1.0f / 3.0f);
		Vector3 ambColor(ambient, ambient, ambient);
		// GeneralsX @bugfix PS3.0 #69 15/10/2026: the options-menu "normal strength" slider was wired
		// to PS c1, which the shaders multiply onto the SUN lambert term (col = base*(ndl*c1 + ambient…))
		// — so the bump slider was really a brightness knob and cranking it blew the buildings out.
		// c1 is now the fixed calibrated sun gain (0.8 = the shipped default slider value, so every A/B
		// photo stays reproducible); the slider drives the actual bump deviation c3.x instead, scaled so
		// the old default 0.8 reproduces the previous hard-coded xyScale 2.5 exactly (0 -> flat, 2 -> 6.25).
		const float lightGain = 0.8f;
		float bumpK = g_Strength * 3.125f;
		if (bumpK < 0.0f) bumpK = 0.0f;
		if (showcase) {
			// GeneralsX @tweak Qoder 11/10/2026 T3-0: the exaggerated showcase params are gone;
			// they caused the harsh light/dark cliff on low-poly faces (near-black backfaces +
			// 2.2x frontfaces + ultra-oblique light). The showcase flag now only bumps strength
			// slightly and keeps the REAL sun direction + REAL ambient, so rotation no longer
			// snaps faces between black and blown-out.
			bumpK = 0.9f * 3.125f;
		}
		Vector4 sc(lightGain, 0.0f, 0.0f, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(1, &sc, 1);          // c1 = fixed sun gain (slider no longer rides it)
		Vector4 ac(ambColor.X, ambColor.Y, ambColor.Z, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(2, &ac, 1);          // c2 = ambient

		// GeneralsX @feature PS3.0 L1 13/10/2026: c7 = fill color for the second lambert term
		// (pairs with the VS c9 -> TEXCOORD3 fill direction above). Black = feature off.
		Vector4 fc7(g_FillColor.X, g_FillColor.Y, g_FillColor.Z, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(7, &fc7, 1);         // c7 = fill color
		// GeneralsX @tweak PS3.0 #69 15/10/2026: c3.x (the tangent-space xy deviation, i.e. the REAL
		// bump strength the shader applies to the normal map) now comes from the options slider via
		// bumpK instead of the old hard-coded 2.5; default slider 0.8 maps to the same 2.5.
		Vector4 bs(bumpK, 0.0f, 1.0f, 0.0f);
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
		if (nm != nullptr && lenv != nullptr && ML_Enabled())
		{
			// GeneralsX @feature PS3.0-P3-18 14/10/2026: the collection cap follows the engine's
			// widened MAX_LIGHTS (4 -> 8); the ML PS loop and mlights[16] (c17..c32) match.
			Vector4 mlLights[16];
			int mlCount = 0;
			const int total = lenv->Get_Light_Count();
			for (int i = 0; i < total && mlCount < 8; ++i)
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
				DX8Wrapper::Set_Pixel_Shader_Constant(17, &mlLights[0], 16);   // 8 pairs, c17..c32
				// GeneralsX @feature PS3.0-P3-12 14/10/2026: ML PS reads c8 = hemi ambient rgb,
				// multiplied by the world-up component of the perturbed normal (n.z, W3D Z-up).
				// ambColor is the same scalar ambient the dot3 path already uses for c2.
				{
					const float hk = Hemi_Ambient_K();
					Vector4 hemi(ambColor.X * hk, ambColor.Y * hk, ambColor.Z * hk, 0.0f);
					DX8Wrapper::Set_Pixel_Shader_Constant(8, &hemi, 1);        // c8 = hemi ambient
				}
				// GeneralsX @feature PS3.0-P3-14 14/10/2026: c9/c10 = eye-depth fog ramp (see file-scope
				// g_Fog*). strength w=0 (the default until the display layer injects) is a shader no-op.
				DX8Wrapper::Set_Pixel_Shader_Constant(9, &g_FogColorStrength, 1);
				DX8Wrapper::Set_Pixel_Shader_Constant(10, &g_FogRamp, 1);

				DX8Wrapper::Set_Texture(1, nm);
				DX8Wrapper::Set_Vertex_Shader(g_VS_ML);
				DX8Wrapper::Set_Pixel_Shader(g_PS_ML);
				return true;
			}
		}

		// GeneralsX @feature PS3.0-P4-1 15/10/2026: PBR variant mount (see Install_PBR block above).
		// Only the single-light path reaches here (the ML branch returned first, by design).  The
		// variant VS reads the c0..c5/c9 constants uploaded above unchanged; the PS additionally
		// samples stage2 (the _rp companion) and reads c8=(mode, LI, ENV, -).
		// GeneralsX @feature PS3.0 #70 15/10/2026: rp was probed up-front, so this branch now runs
		// with or without the normal-map switch.  Roughness-only (nm == nullptr) leaves stage1
		// unbound and re-uploads c3=(0,0,0,1): the PS discards whatever the (unbound) s1 sample
		// returns and shades with the FLAT tangent normal, i.e. the interpolated geometric normal.
		if (rp != nullptr)
		{
			static int s_pbrMounted = 0;
			if (s_pbrMounted < 8)
			{
				++s_pbrMounted;
				fprintf(stderr, "[PBR] mode=%d mounted on diffuse='%s' with rp='%s' bump=%d\n",
					PBR_Mode(), (const char*)diffuse->Get_Texture_Name(), (const char*)rp->Get_Texture_Name(),
					nm != nullptr ? 1 : 0);
				fflush(stderr);
			}
			const Vector4 c8pbr((float)PBR_Mode(), PBR_LI(), PBR_ENV(), 0.0f);
			DX8Wrapper::Set_Pixel_Shader_Constant(8, &c8pbr, 1);   // c8 = (mode, LI, ENV, -)
			if (nm == nullptr) {
				const Vector4 flat(0.0f, 0.0f, 0.0f, 1.0f);
				DX8Wrapper::Set_Pixel_Shader_Constant(3, &flat, 1); // c3: tangent normal forced to (0,0,1)
			}
			DX8Wrapper::Set_Texture(1, nm);
			DX8Wrapper::Set_Texture(2, rp);
			DX8Wrapper::Set_Vertex_Shader(g_VS_PBR);
			DX8Wrapper::Set_Pixel_Shader(g_PS_PBR);
			return true;
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
		// GeneralsX @feature PS3.0-P4-1 15/10/2026: unbind the rp companion stage too (no-op when the
		// plain single-light pair ran), so the wrapper cache can never hand a stale _rp to a later
		// multi-stage material - same precedent as the GN sun-shadow stage2 unbind.
		DX8Wrapper::Set_Texture(2, nullptr);
	}
}

//=============================================================================
// GeneralsX @feature PS3.0 L2 14/10/2026 geometric-normal per-pixel diffuse.
// See dx8normalmap.h for the model. This module never samples a normal map: the PS
// re-lighting uses the interpolated geometric VERTEX normal evaluated per pixel, so it
// mounts on the plain realtime-lit XYZNUV1/XYZNUV2 rigid batches (no vertex-color channel)
// that the dot3 path skips. Apply sums the batch's own up-to-4 directional lights.
// GeneralsX @tweak PS3.0 L2 14/10/2026: promoted to DEFAULT ON after the GNspec A/B certified
// coverage (293 mount samples, zero terrain/HUD bleed). GENERALSX_GEOMNORMAL=0 in the process
// environment at startup disables (read once inside Install); enabling still needs a successful
// device-layer install.
//=============================================================================
#include "dx8fvf.h"
#include "vertmaterial.h"

namespace DX8GeomNormal
{
	static UINT_PTR g_VS_DUV1 = 0;
	static UINT_PTR g_VS_DUV2 = 0;
	static UINT_PTR g_PS      = 0;
	static bool     g_Enabled = false;   // off until Install; default ON after (kill switch: =0)

	// World-space key sun (direction TOWARD the light, same convention as DX8NormalMap's
	// fallback) and its diffuse color; the L1 sky fill mirrors DX8NormalMap's pair.
	static Vector3  g_KeyDir(0.0f, 0.0f, 1.0f);
	static Vector3  g_KeyColor(0.0f, 0.0f, 0.0f);
	static Vector3  g_FillDir(0.0f, 0.0f, 0.0f);
	static Vector3  g_FillColor(0.0f, 0.0f, 0.0f);
	// Scene ambient (the object ambient the display layer feeds Set_Ambient_Light with). The GN VS
	// multiplies it by the material ambient and adds material emissive to rebuild the fixed-pipeline
	// Gouraud base, since a custom VS bypasses fixed-function lighting (realtime meshes carry no
	// vertex-color channel - FVF 0x112).
	static Vector3  g_Ambient(0.0f, 0.0f, 0.0f);

	// GeneralsX @feature PS3.0 P3-21b 14/10/2026 sun shadow map reception state (see dx8normalmap.h).
	// The texture pointer is BORROWED - the game layer keeps the wrapper alive between Set and Clear.
	static UINT_PTR g_SunVS_DUV1 = 0;
	static UINT_PTR g_SunVS_DUV2 = 0;
	static UINT_PTR g_SunPS      = 0;
	static UINT_PTR g_SunPS5     = 0;   // GeneralsX @feature PS3.0-P3-22: optional 5x5 (25-tap) PCF variant
	static TextureClass* g_sunTex = nullptr;
	static Matrix4x4 g_sunM;                       // world -> light clip (Transform_Vector convention)
	static float  g_sunSpread = 0.0f;              // PCF tap offset in UV units
	static float  g_sunBias   = 0.0f;              // acne guard in [0,1] depth
	static float  g_sunStrength = 0.0f;            // 0 disables the variant (plain GN shaders run)
	static bool   g_sunBoundThisBatch = false;     // Restore only unbinds stage 2 after a shadow batch

	void Install(UINT_PTR vs_duv1, UINT_PTR vs_duv2, UINT_PTR ps_handle)
	{
		g_VS_DUV1 = vs_duv1;
		g_VS_DUV2 = vs_duv2;
		g_PS      = ps_handle;
		const char* ev = getenv("GENERALSX_GEOMNORMAL");
		g_Enabled = (ev == nullptr || strcmp(ev, "0") != 0);   // GeneralsX @tweak PS3.0 L2 14/10/2026 default ON
		fprintf(stderr, "[GN] geometric-normal shaders installed vs_duv1=%p vs_duv2=%p ps=%p enabled=%d\n",
			(void*)vs_duv1, (void*)vs_duv2, (void*)ps_handle, (int)g_Enabled);
		fflush(stderr);
	}

	bool Ready_To_Render()
	{
		return g_Enabled && g_PS != 0 && (g_VS_DUV1 != 0 || g_VS_DUV2 != 0);
	}

	// GeneralsX @feature PS3.0 P3-21b 14/10/2026 - sun shadow reception in the GN pass (see header).
	void Install_Sun_Shadow(UINT_PTR vs_duv1, UINT_PTR vs_duv2, UINT_PTR ps_handle)
	{
		g_SunVS_DUV1 = vs_duv1;
		g_SunVS_DUV2 = vs_duv2;
		g_SunPS      = ps_handle;
		fprintf(stderr, "[GN] sun-shadow variants installed vs1=%p vs2=%p ps=%p\n",
			(void*)vs_duv1, (void*)vs_duv2, (void*)ps_handle);
		fflush(stderr);
	}

	// GeneralsX @feature PS3.0-P3-22 14/10/2026: optional 5x5 PCF pixel shader for the shadow path.
	// When present AND GENERALSX_SMAP_PCF=5 was set at startup, Apply mounts this instead of the 3x3;
	// any other combination keeps the 9-tap program, so a compile failure here degrades softening,
	// never correctness.
	void Install_Sun_Shadow_PCF5(UINT_PTR ps_handle)
	{
		g_SunPS5 = ps_handle;
		fprintf(stderr, "[GN] sun-shadow PCF5 variant installed ps=%p\n", (void*)ps_handle);
		fflush(stderr);
	}

	void Set_Sun_Shadow(TextureClass* map, const Matrix4x4& worldToLightClip,
		float spreadTexels, float bias, float strength)
	{
		g_sunTex = map;
		g_sunM = worldToLightClip;
		g_sunSpread = spreadTexels;
		g_sunBias = bias;
		g_sunStrength = strength;
	}

	void Clear_Sun_Shadow()
	{
		g_sunTex = nullptr;
		g_sunStrength = 0.0f;
	}

	static bool sunShadowActive()
	{
		return g_Enabled && g_sunTex != nullptr && g_sunStrength > 0.0f && g_SunPS != 0;
	}

	bool Sun_Shadow_Ready()
	{
		// Reports the MODULE side only (the game layer asks this before parking its own receiver
		// overlay); whether a map is currently pushed is sunShadowActive().
		return g_Enabled && g_SunPS != 0 && (g_SunVS_DUV1 != 0 || g_SunVS_DUV2 != 0);
	}

	void Set_Global_Key_Light(const Vector3& world_dir, const Vector3& color)
	{
		g_KeyDir = world_dir;
		g_KeyColor = color;
	}

	void Set_Global_Fill_Light(const Vector3& world_dir, const Vector3& color)
	{
		g_FillDir = world_dir;
		g_FillColor = color;
	}

	void Set_Global_Ambient(const Vector3& ambient)
	{
		g_Ambient = ambient;
	}

	static void Normalize(Vector3& v)
	{
		const float l2 = v.X * v.X + v.Y * v.Y + v.Z * v.Z;
		if (l2 > 1e-12f) {
			const float inv = 1.0f / sqrtf(l2);
			v.X *= inv; v.Y *= inv; v.Z *= inv;
		}
	}

	// GeneralsX @feature PS3.0 P3-21b 14/10/2026: one object point through the two proven transforms
	// into the sun camera's clip space (ortho: w stays 1).  Used by Apply's finite-difference row solve.
	static Vector4 lightClip(const Matrix3D& world, float x, float y, float z)
	{
		Vector3 wpt;
		Matrix3D::Transform_Vector(world, Vector3(x, y, z), &wpt);
		Vector4 in(wpt.X, wpt.Y, wpt.Z, 1.0f);
		Vector4 out;
		Matrix4x4::Transform_Vector(g_sunM, in, &out);
		return out;
	}

	bool Apply(const Matrix3D& world, const VertexMaterialClass* vm, int fvf, LightEnvironmentClass* lenv)
	{
		if (!Ready_To_Render()) return false;
		UINT_PTR vs = 0;
		if (fvf == (int)DX8_FVF_XYZNUV1) vs = g_VS_DUV1;
		else if (fvf == (int)DX8_FVF_XYZNUV2) vs = g_VS_DUV2;
		else return false;
		if (vs == 0) return false;

		// GeneralsX @feature PS3.0 P3-21b 14/10/2026: prefer the shadow-aware variants when a sun map
		// is active and a variant exists for this FVF; otherwise the plain GN pair runs unchanged.
		bool useShadow = false;
		UINT_PTR psSel = g_PS;
		if (sunShadowActive()) {
			const UINT_PTR svs = (fvf == (int)DX8_FVF_XYZNUV1) ? g_SunVS_DUV1 : g_SunVS_DUV2;
			if (svs != 0) {
				vs = svs;
				psSel = g_SunPS;
				// GeneralsX @feature PS3.0-P3-22: PCF kernel tier (same env the game layer read for
				// the spread scale - both decide once at startup, so shader and constants always agree).
				static const int s_pcf5 = []() {
					const char* e = getenv("GENERALSX_SMAP_PCF");
					return e != nullptr && atoi(e) == 5;
				}();
				if (s_pcf5 && g_SunPS5 != 0) psSel = g_SunPS5;
				useShadow = true;
			}
		}

		// Same WVP convention as DX8NormalMap::Apply (raw DX8 transforms, transpose once);
		// see the water-shader precedent comment there.
		Matrix4x4 view;
		Matrix4x4 proj;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, view);
		DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, proj);

		Matrix4x4 wm(world);
		Matrix4x4 wmDx8 = wm.Transpose();
		Matrix4x4 wvp = wmDx8 * view;
		wvp = wvp * proj;
		wvp = wvp.Transpose();

		const float* s = reinterpret_cast<const float*>(&wvp);
		DX8Wrapper::Set_Vertex_Shader_Constant(0, s, 4);              // VS c0..c3 = WVP
		const float* m = reinterpret_cast<const float*>(&wmDx8);
		DX8Wrapper::Set_Vertex_Shader_Constant(4, m, 3);             // VS c4..c6 = world rotation rows (normal -> world)

		if (useShadow) {
			// object -> light-clip rows (VS c7..c10), composed by finite differences through the two
			// conventions that are each PROVEN on the device: Matrix3D::Transform_Vector(world, .) for
			// object->world (every engine transform uses it) and Matrix4x4::Transform_Vector(g_sunM, .)
			// for world->light-clip (the terrain receiver's CPU vertex math uses exactly this and lands
			// the grid on the map). The chain is affine, so four evaluations recover the four rows
			// without guessing operator* transposition conventions.
			const Vector4 L0 = lightClip(world, 0.0f, 0.0f, 0.0f);
			const Vector4 LX = lightClip(world, 1.0f, 0.0f, 0.0f);
			const Vector4 LY = lightClip(world, 0.0f, 1.0f, 0.0f);
			const Vector4 LZ = lightClip(world, 0.0f, 0.0f, 1.0f);
			float sun[16];
			sun[0]  = LX.X - L0.X; sun[1]  = LY.X - L0.X; sun[2]  = LZ.X - L0.X; sun[3]  = L0.X;
			sun[4]  = LX.Y - L0.Y; sun[5]  = LY.Y - L0.Y; sun[6]  = LZ.Y - L0.Y; sun[7]  = L0.Y;
			sun[8]  = LX.Z - L0.Z; sun[9]  = LY.Z - L0.Z; sun[10] = LZ.Z - L0.Z; sun[11] = L0.Z;
			sun[12] = LX.W - L0.W; sun[13] = LY.W - L0.W; sun[14] = LZ.W - L0.W; sun[15] = L0.W;
			DX8Wrapper::Set_Vertex_Shader_Constant(7, sun, 4);       // VS c7..c10 = object -> light clip rows

			Vector4 c15(g_sunSpread, g_sunSpread, g_sunBias, g_sunStrength);
			DX8Wrapper::Set_Pixel_Shader_Constant(15, &c15, 1);      // PS c15 = (spread, spread, bias, strength)
			DX8Wrapper::Set_Texture(2, g_sunTex);                    // PS sampler s2 = the float sun map
			g_sunBoundThisBatch = true;
			static unsigned s_gnShadowMounted = 0;
			if ((s_gnShadowMounted++ % 4096u) == 0u) {
				fprintf(stderr, "[GN] sun-shadow batch #%u spread=%.5f bias=%.5f strength=%.2f\n",
					s_gnShadowMounted, (double)g_sunSpread, (double)g_sunBias, (double)g_sunStrength);
				fflush(stderr);
			}
		}

		// The fixed pipeline multiplied every light's diffuse by the material diffuse before the
		// vertex lambert sum, and used D3DRS_AMBIENT = Get_Equivalent_Ambient() scaled by the
		// material ambient, plus material emissive. We mirror that exactly, but as a per-pixel sum
		// over the batch's OWN directional lights, so GN only differs from retail in normal
		// interpolation (per-pixel vs per-vertex), never in the light set or its scaling.
		Vector3 md(1.0f, 1.0f, 1.0f);
		Vector3 ma(0.0f, 0.0f, 0.0f);
		Vector3 me(0.0f, 0.0f, 0.0f);
		if (vm != nullptr) { vm->Get_Diffuse(&md); vm->Get_Ambient(&ma); vm->Get_Emissive(&me); }

		// GeneralsX @feature PS3.0-P3-18 14/10/2026: 4 -> 8 slots, mirroring the widened MAX_LIGHTS.
		// PS layout: dirs c5..c8 + c17..c20, matDiffuse-scaled colors c9..c12 + c21..c24 (c13..c16
		// carry eye/spec/shadow/hemi), so lights 4..7 upload in the second const pair.
		Vector4 dirs[8];
		Vector4 cols[8];
		Vector3 amb(0.0f, 0.0f, 0.0f);
		int n = 0;

		if (lenv != nullptr) {
			const Vector3& envAmb = lenv->Get_Equivalent_Ambient();
			amb.X = ma.X * envAmb.X + me.X;
			amb.Y = ma.Y * envAmb.Y + me.Y;
			amb.Z = ma.Z * envAmb.Z + me.Z;
			int lc = lenv->Get_Light_Count();
			if (lc > 8) lc = 8;
			for (int li = 0; li < lc; ++li) {
				Vector3 d = lenv->Get_Light_Direction(li);   // world dir TOWARD the light
				Normalize(d);
				const Vector3& c = lenv->Get_Light_Diffuse(li);
				dirs[li] = Vector4(d.X, d.Y, d.Z, 0.0f);
				cols[li] = Vector4(c.X * md.X, c.Y * md.Y, c.Z * md.Z, 0.0f);
			}
			n = lc;
		} else {
			// Global-lit fallback (no per-mesh environment): the injected key sun + L1 sky fill.
			amb.X = ma.X * g_Ambient.X + me.X;
			amb.Y = ma.Y * g_Ambient.Y + me.Y;
			amb.Z = ma.Z * g_Ambient.Z + me.Z;
			Vector3 key = g_KeyDir; Normalize(key);
			dirs[0] = Vector4(key.X, key.Y, key.Z, 0.0f);
			cols[0] = Vector4(g_KeyColor.X * md.X, g_KeyColor.Y * md.Y, g_KeyColor.Z * md.Z, 0.0f);
			n = 1;
			Vector3 fill = g_FillDir;
			const float fl2 = fill.X * fill.X + fill.Y * fill.Y + fill.Z * fill.Z;
			if (fl2 > 1e-12f) {
				Normalize(fill);
				dirs[1] = Vector4(fill.X, fill.Y, fill.Z, 0.0f);
				cols[1] = Vector4(g_FillColor.X * md.X, g_FillColor.Y * md.Y, g_FillColor.Z * md.Z, 0.0f);
				n = 2;
			}
		}

		// Zero the unused light slots: saturate(dot(n,0))=0 and color=0, so they contribute nothing.
		for (int li = n; li < 8; ++li) {
			dirs[li] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
			cols[li] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
		}

		Vector4 c4amb(amb.X, amb.Y, amb.Z, 1.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(4, &c4amb, 1);          // PS c4 = material-scaled ambient
		DX8Wrapper::Set_Pixel_Shader_Constant(5, &dirs[0], 4);        // PS c5..c8 = world light dirs 0..3
		DX8Wrapper::Set_Pixel_Shader_Constant(9, &cols[0], 4);        // PS c9..c12 = matDiffuse-scaled colors 0..3
		DX8Wrapper::Set_Pixel_Shader_Constant(17, &dirs[4], 4);       // PS c17..c20 = world light dirs 4..7 (P3-18)
		DX8Wrapper::Set_Pixel_Shader_Constant(21, &cols[4], 4);       // PS c21..c24 = matDiffuse-scaled colors 4..7 (P3-18)
		// GeneralsX @feature PS3.0-P3-12 14/10/2026: PS c16 = hemi ambient rgb, multiplied by the
		// world-up component (n.z) in the GN/GNS/GNS5 shaders. GENERALSX_HEMI=0 zeroes it and the
		// extra term is a numerical no-op (flat-ambient parity).
		{
			const float hk = Hemi_Ambient_K();
			Vector4 c16hemi(amb.X * hk, amb.Y * hk, amb.Z * hk, 0.0f);
			DX8Wrapper::Set_Pixel_Shader_Constant(16, &c16hemi, 1);   // PS c16 = hemi ambient
		}
		// GeneralsX @feature PS3.0-P3-14 14/10/2026: PS c25/c26 = eye-depth fog ramp for the GN
		// shaders. Injected state (DX8NormalMap::Set_Fog); w=0 is the exact pre-fog pixel.
		DX8Wrapper::Set_Pixel_Shader_Constant(25, &g_FogColorStrength, 1);
		DX8Wrapper::Set_Pixel_Shader_Constant(26, &g_FogRamp, 1);

		// GeneralsX @feature PS3.0 L2 14/10/2026: per-pixel Blinn-Phong on light 0, the one term the
		// old GN pass omitted. The FF feed (dx8wrapper.cpp Set_Light_Environment) gives light0 a white
		// D3DLIGHT.Specular, so dropping it made GN-on read visibly DARKER on curved tank/building
		// tops than retail (-2.4 RGB region mean measured 13/10). Eye vector follows the T1 dot3
		// precedent: the camera's +Z axis in world space sits at flat[2], flat[6], flat[10] of the raw
		// DX8 view matrix. GN normals and light dirs are already WORLD space (VS rotates them), so
		// unlike dot3 (model space) no inverse transform is needed here. Knobs mirror the dot3 ones:
		// GENERALSX_GEOMNORMAL_SPECPOWER (default 24) / GENERALSX_GEOMNORMAL_SPEC (default 0.35;
		// 0.35 is the showcase value measured 14/10, 0.12 was the retail-parity dot3 default).
		{
			const float* vd = reinterpret_cast<const float*>(&view);
			Vector3 Ew(vd[2], vd[6], vd[10]);
			const float el2 = Ew.X * Ew.X + Ew.Y * Ew.Y + Ew.Z * Ew.Z;
			if (el2 > 1e-6f) { const float inv = 1.0f / sqrtf(el2); Ew.X *= inv; Ew.Y *= inv; Ew.Z *= inv; }
			else { Ew = Vector3(0.0f, 0.0f, 1.0f); }   // degenerate view matrix: headlight fallback

			static float specPow = -1.0f;
			static float specInt = -1.0f;
			if (specPow < 0.0f) {
				specPow = 24.0f;
				specInt = 0.35f;   // GeneralsX @tweak PS3.0 L2 14/10/2026: showcase level (was 0.12)
				const char* evPow = getenv("GENERALSX_GEOMNORMAL_SPECPOWER");
				if (evPow != nullptr && *evPow != 0) specPow = (float)atof(evPow);
				const char* evInt = getenv("GENERALSX_GEOMNORMAL_SPEC");
				if (evInt != nullptr && *evInt != 0) specInt = (float)atof(evInt);
				if (specPow < 1.0f) specPow = 1.0f;
				if (specInt < 0.0f) specInt = 0.0f;
			}
			Vector4 c13eye(Ew.X, Ew.Y, Ew.Z, specPow);
			DX8Wrapper::Set_Pixel_Shader_Constant(13, &c13eye, 1);    // PS c13 = world eye dir, w = spec power
			Vector4 c14spec(specInt, specInt, specInt, 0.0f);
			DX8Wrapper::Set_Pixel_Shader_Constant(14, &c14spec, 1);   // PS c14 = spec intensity (0 = off)
		}

		DX8Wrapper::Set_Vertex_Shader(vs);
		DX8Wrapper::Set_Pixel_Shader(psSel);
		return true;
	}

	void Restore(UINT_PTR fvf)
	{
		// GeneralsX @feature PS3.0 P3-21b: unbind the sun map from stage 2 after a shadow batch so the
		// wrapper cache cannot hand a stale map pointer to a later multi-stage material.
		if (g_sunBoundThisBatch) {
			DX8Wrapper::Set_Texture(2, nullptr);
			g_sunBoundThisBatch = false;
		}
		DX8Wrapper::Set_Pixel_Shader(0);
		DX8Wrapper::Set_Vertex_Shader(0);
		DX8Wrapper::Set_Vertex_FVF((DWORD)fvf);
	}
}
