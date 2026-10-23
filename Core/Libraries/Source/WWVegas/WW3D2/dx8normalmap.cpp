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
#include <cstring>   // GeneralsX @feature PS3.0-#75: strcmp/strncpy for the render-time sun probe
#include <cstdlib>
#include <cstring>
#include <cmath>

// GeneralsX @bugfix Qoder 18/10/2026 pure-Vulkan wedge 8: mount capability probe. While the Vulkan
// swapchain owns presentation, a batch that names a custom VS/PS the registry has not translated is
// refused by the draw facade and falls back to a D3D9 call that is never presented - the whole batch
// disappears. The mounters therefore have to decline such a pair up front and leave the batch on the
// fixed pipeline (which the Vulkan FF stages do draw). This is not a feature switch: every translated
// pair still mounts, and the decline lifts automatically as each family's translation lands.
// GeneralsX @build Qoder 22/10/2026 VulkanPgm.cpp is compiled into ww3d2 unconditionally; the
// handle->tag query below must see its declaration even when SAGE_USE_VULKAN_CORE is off.
#include "Backend/VulkanPgm.h"
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
#include "Backend/VulkanCore.h"
#include "Backend/VulkanForward.h"
#endif

namespace
{
	bool vkPresentOwnsFrame()
	{
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		return VKPgm_On() && VulkanCore_Present_Enabled();
#else
		return false;
#endif
	}

	bool vkPairTranslated(UINT_PTR vs, UINT_PTR ps, const char* family)
	{
		if (!vkPresentOwnsFrame()) return true;
		const unsigned vsTag = VKPgm_Tag_For_Handle((void*)vs, 0);
		const unsigned psTag = VKPgm_Tag_For_Handle((void*)ps, 1);
		if ((vs == 0 || vsTag != 0u) && (ps == 0 || psTag != 0u)) return true;

		static unsigned s_declined = 0;
		if ((s_declined++ & 0x3FFu) == 0u) {
			fprintf(stderr, "[VKPGM] mount declined (%s): vs tag=%u ps tag=%u - batch stays on the fixed pipeline\n",
				family != nullptr ? family : "?", vsTag, psTag);
			fflush(stderr);
		}
		return false;
	}
}

// GeneralsX @feature PS3.0-P3-12 14/10/2026: hemispherical ambient strength (master report L5).
// The engine's ambient is a flat constant; multiplying an ambient*hemiscale tint by the WORLD-up
// component of the pixel normal (W3D is Z-up - see DX8NormalMap's g_FallbackDir) lifts sky-facing
// surfaces and leaves downward faces on the plain ambient, so shadowed ground no longer reads as
// the same value as an upward-lit wall. Uploaded as the GN-family PS c16 hemi rgb.
// READ ONCE per process; GENERALSX_HEMI=0 restores the flat-ambient look bit-for-bit.
// GeneralsX @tweak Qoder 17/10/2026 P0 of the per-pixel master switch (光影T0阶段/调研/
// 2026-10-17_逐像素光照总闸_施工调研.md): 0.35 was the showcase value and, together with the
// 0.35 GN spec, whitened every light-coloured building/vehicle top under the RTS camera (the
// 14/10 verdict that pinned GENERALSX_GEOMNORMAL=0 in the user's HKCU). 0.15 keeps the
// up-vs-down-face separation without lifting shadowed tops into haze; user-confirmed value for
// the master-switch batch. GENERALSX_HEMI still wins for A/B forensics.
static float Hemi_Ambient_K()
{
	static float cached = -1.0f;
	if (cached < 0.0f) {
		cached = 0.15f;
		const char* ev = getenv("GENERALSX_HEMI");
		if (ev != nullptr && *ev != 0) cached = (float)atof(ev);
		if (cached < 0.0f) cached = 0.0f;
	}
	return cached;
}

// GeneralsX @feature PS3.0-P3-14 14/10/2026: per-pixel distance fog for the GN family
// (PS c25/c26). WW3D2 must not read the game
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

	// GeneralsX @feature PS3.0-P4-1 15/10/2026 roughness/metalness (PBR) variant for the dot3
	// single-light path (光影三阶段/粗糙度引擎承接工作清单.md M1+M2).  The pair is compiled by the
	// device layer and carries the SAME VS constant layout as g_VS/g_PS (c0..c3 WVP, c4 model-space
	// light, c5 model-space eye, c9 model-space fill); the PS adds sampler s2 (the "<stem>_rp"
	// companion: R=roughness, G=metalness) and c8=(mode, LI, ENV, -) where mode selects the branch:
	//   0 = off (this pair never mounts), 1 = M1 roughness-driven Blinn, 2 = M2 GGX metallic-roughness.
	// Mounting additionally requires the diffuse to have a companion _rp texture; without one, without
	// handles, or without GENERALSX_PBR=1/2 in the process environment, the single-light path runs
	// bit-for-bit - zero regression.
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
		// GeneralsX @feature PS3.0 probe round 6 16/10/2026: one-shot console proof of the injected
		// probe sun (was it ever called on mission load, and with which vector?). Release-safe log.
		fprintf(stderr, "[FALLBACK] dir=(%.3f,%.3f,%.3f) amb=(%.3f,%.3f,%.3f)\n",
			world_dir.X, world_dir.Y, world_dir.Z, ambient.X, ambient.Y, ambient.Z);
		fflush(stderr);
	}

	// GeneralsX @feature PS3.0-#75 16/10/2026 render-time sun probe (user-directed): print the light
	// direction and ambient each shader path ACTUALLY shades the batch with, one line per distinct
	// quantized state (path + tag + dir@0.02 + amb@0.005). DOT3 keys on the diffuse texture name,
	// GN keys on the per-batch LightEnvironment pointer + light count.
	// What the output CAN prove (corrected in #76b): both tags report a WORLD-space direction, so
	// the probe compares light SOURCES (injected global sun vs per-mesh environment) and per-asset
	// ambient. Every DOT3 line carries the same dir by construction (Lw is the single g_FallbackDir),
	// so two DOT3 lines can never "disagree" about the sun - the informative pairing is DOT3 vs GN.
	// What it CANNOT show: the #76b bug lived in the per-mesh world -> model ROTATE, so the world
	// direction stayed identical for every building and no amount of [SUNDBG] lines could expose it.
	// Capped at 400 distinct lines; release-safe stderr+fflush per AGENTS.md.
	static void log_sun_once(const char* path, const char* tag, const Vector3& dir, const Vector3& amb)
	{
		static char seen[400][128];
		static int seen_n = 0;
		if (seen_n >= 400) return;
		char key[128];
		_snprintf(key, sizeof(key), "%s|%s|%d,%d,%d|%d,%d,%d", path, (tag != nullptr) ? tag : "?",
			(int)(dir.X * 50.0f), (int)(dir.Y * 50.0f), (int)(dir.Z * 50.0f),
			(int)(amb.X * 200.0f), (int)(amb.Y * 200.0f), (int)(amb.Z * 200.0f));
		for (int i = 0; i < seen_n; ++i) {
			if (strcmp(seen[i], key) == 0) return;
		}
		strncpy(seen[seen_n], key, sizeof(seen[0]) - 1);
		seen[seen_n][sizeof(seen[0]) - 1] = 0;
		++seen_n;
		fprintf(stderr, "[SUNDBG] %s tag=%s dir=(%.3f,%.3f,%.3f) amb=(%.3f,%.3f,%.3f)\n",
			path, (tag != nullptr) ? tag : "?", dir.X, dir.Y, dir.Z, amb.X, amb.Y, amb.Z);
		fflush(stderr);
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

	// GeneralsX @feature PS3.0 Weather W5: injected wetness for the PBR roughness term (see the two
	// c8/c11 uploads below). Default 0 keeps every pixel identical to the dry shader path.
	static float g_Wetness = 0.0f;

	void Set_Wetness(float wet)
	{
		if (wet < 0.0f) wet = 0.0f;
		if (wet > 1.0f) wet = 1.0f;
		g_Wetness = wet;
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

	// GeneralsX @bugfix Qoder 16/10/2026 #87: short-side probe from the file header only (TGA:12/14
	// LE16, DDS:16/12 LE32), shared by both companion lookups. False = format not recognised, which
	// callers treat as "unknown", never as "reject". Mirrors the load-time helper in
	// MeshModelClass::Detect_And_Build_Normal_Map (GeneralsMD tree) for the render-side belt.
	static bool Peek_Image_Min_Side(const char* filename, int& minSide)
	{
		minSide = 0;
		if (filename == nullptr || filename[0] == 0) {
			return false;
		}
		FileClass* file = _TheFileFactory->Get_File(filename);
		if (file == nullptr) {
			return false;
		}
		bool ok = false;
		file->Open();
		if (file->Is_Available()) {
			unsigned char hdr[20];
			if (file->Read(hdr, 20) == 20) {
				if (hdr[0] == 'D' && hdr[1] == 'D' && hdr[2] == 'S' && hdr[3] == ' ') {
					const int h = hdr[12] | (hdr[13] << 8) | (hdr[14] << 16) | (hdr[15] << 24);
					const int w = hdr[16] | (hdr[17] << 8) | (hdr[18] << 16) | (hdr[19] << 24);
					if (w > 0 && h > 0) {
						minSide = (w < h) ? w : h;
						ok = true;
					}
				} else {
					const int w = hdr[12] | (hdr[13] << 8);
					const int h = hdr[14] | (hdr[15] << 8);
					if (w > 0 && h > 0) {
						minSide = (w < h) ? w : h;
						ok = true;
					}
				}
			}
		}
		file->Close();
		_TheFileFactory->Return_File(file);
		return ok;
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
		// GeneralsX @tweak Qoder 20/10/2026 TEMP EXPERIMENT: the #68 team-colour rejection below is
		// commented out (normally house-colour skins - names starting with "zhcd" or containing
		// "housecolor" - are refused a _nm companion so they keep the fixed pipeline). Temporary
		// local test only; the rejection is a class-level asset contract from 16/10/2026 (#68) and is
		// EXPECTED to come back - do not ship this state.
		// if (Is_TeamColor_Skin(base)) { g_NmCache.Put(raw, nullptr); return nullptr; }   // #68: house-colour skins keep the fixed pipeline
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

		// GeneralsX @bugfix Qoder 16/10/2026 #87: render-side belt of the surface detail contract
		// (dimension arm only; the polygon arm lives in the load probe where the mesh is visible).
		// Companions dropped after model load never re-run Detect_And_Build_Normal_Map, so a
		// sub-128px FX-sprite companion must never become a live stage-1 binding from here either.
		{
			int cSide = 0;
			int dSide = 0;
			if ((Peek_Image_Min_Side(found, cSide) && cSide < 128)
				|| (Peek_Image_Min_Side(raw, dSide) && dSide < 128)) {
				g_NmCache.Put(raw, nullptr);
				return nullptr;
			}
		}

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

		// GeneralsX @bugfix Qoder 16/10/2026 #87: same render-side dimension belt as
		// Find_Companion_Normal - a sub-128px _rp companion is an FX sprite's roughness map and
		// must never bind to stage 2.
		{
			int cSide = 0;
			int dSide = 0;
			if ((Peek_Image_Min_Side(found, cSide) && cSide < 128)
				|| (Peek_Image_Min_Side(raw, dSide) && dSide < 128)) {
				g_RPCache.Put(raw, nullptr);
				return nullptr;
			}
		}

		WW3DAssetManager* am = WW3DAssetManager::Get_Instance();
		if (am == nullptr) return nullptr;
		TextureClass* rp = am->Get_Texture(found, MIP_LEVELS_ALL, WW3D_FORMAT_A8R8G8B8);

		g_RPCache.Put(raw, rp);
		return rp;
	}

	// GeneralsX @feature Qoder 17/10/2026 P2 forensics: public presence probes over the cached
	// lookups, used by the renderer's one-line-per-mesh-class companion reconciliation. The caches
	// own the returned pointers, so these only compare against nullptr (no refcount handling).
	bool Has_Companion_Normal(TextureClass* diffuse)
	{
		return Find_Companion_Normal(diffuse) != nullptr;
	}

	bool Has_Companion_RP(TextureClass* diffuse)
	{
		return Find_Companion_RP(diffuse) != nullptr;
	}

	// GeneralsX @feature PS3.0-#76 Qoder 16/10/2026: targeted branch forensics for the four-plant
	// sun-consistency bench. The [PBR] mount counter caps at 8 and the case-mixed duplicate texture
	// names (ABPwrPlant.tga vs abpwrplant.tga) make the existing logs inconclusive about which
	// shading branch (or rejection) the power plant actually takes. This fires one stderr line per
	// branch visit for any diffuse whose name contains "abpwrplant" (case-insensitive), so a T1
	// recapture answers "DOT3 vs PBR vs fixed pipeline" from runtime evidence, not guesswork.
	static void Log_Bench_Path(const char* verdict, TextureClass* diffuse, TextureClass* nm, TextureClass* rp)
	{
		static int s_logged = 0;
		if (s_logged >= 12 || diffuse == nullptr) return;
		const char* dn = (const char*)diffuse->Get_Texture_Name();
		if (dn == nullptr || dn[0] == 0) return;
		char low[64];
		int i = 0;
		for (; dn[i] != 0 && i < 63; ++i) {
			const char c = dn[i];
			low[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
		}
		low[i] = 0;
		if (strstr(low, "abpwrplant") == nullptr) return;
		++s_logged;
		fprintf(stderr, "[BENCH76] %s diffuse='%s' nm=%s rp=%s\n", verdict, dn,
			nm != nullptr ? "yes" : "no", rp != nullptr ? "yes" : "no");
		fflush(stderr);
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

	// GeneralsX @feature Qoder 17/10/2026 P2: skinned-batch variant (see header). The dynamic layout
	// the CPU skinning pass submits carries a REAL COLOR0 channel (team flash, baked detail, alpha),
	// which the rigid VS pair drops - so skins get their own VS (relay) + PS (GN lighting x vertex
	// diffuse, the same FF-parity chain the dot3 pair ships). 0 handles = skins stay fixed pipeline.
	static UINT_PTR g_VS_SKIN = 0;
	static UINT_PTR g_PS_SKIN = 0;

	// GeneralsX @feature Qoder 17/10/2026 P3 (逐像素光照总闸): the MERGED pair for the TG3 tangent-space
	// layout - rigid bump meshes and tangent skin batches both submit DX8_FVF_XYZNDUV1TG3, and both are
	// served by ONE program that IS the plain GN lighting equation plus two optional sampling branches
	// (s1 _nm bump, s3 _rp roughness) and the GNS sun-shadow branch (s2). This is what retires the
	// parallel DOT3/PBR single-light families; the Vulkan GN pair owns point lights now.
	// 0 handles = every TG3 batch declines and stays on the fixed pipeline.
	static UINT_PTR g_VS_MERGED = 0;
	static UINT_PTR g_PS_MERGED = 0;
	static bool     g_mergedBoundThisBatch = false;   // Restore unbinds the s1/s3 companion stages

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
		// GeneralsX @tweak Qoder 17/10/2026: seed only. The device layer overrides this a few lines
		// later through Set_Enabled(OptionPreferences().getPerPixelLighting()), which is the same env
		// test plus the Options.ini "PerPixelLighting" row - keeping the identical semantics here means
		// a non-Windows build (no option seed) still behaves exactly as before.
		g_Enabled = (ev == nullptr || strcmp(ev, "0") != 0);   // GeneralsX @tweak PS3.0 L2 14/10/2026 default ON
		fprintf(stderr, "[GN] geometric-normal shaders installed vs_duv1=%p vs_duv2=%p ps=%p enabled=%d\n",
			(void*)vs_duv1, (void*)vs_duv2, (void*)ps_handle, (int)g_Enabled);
		fflush(stderr);
	}

	bool Ready_To_Render()
	{
		return g_Enabled && g_PS != 0 && (g_VS_DUV1 != 0 || g_VS_DUV2 != 0);
	}

	// GeneralsX @feature Qoder 17/10/2026: per-pixel lighting master switch (see header). The pass used
	// to be reachable only through GENERALSX_GEOMNORMAL read once in Install(); the Graphics Options
	// page row "逐像素光照" now owns it and can toggle it live, because dx8renderer.cpp asks
	// Ready_To_Render() per batch. Set from the device layer at startup (OptionPreferences getter, env
	// override included) and from the menu on every apply. Toggling costs one branch per batch and
	// leaves shader handles installed either way, so ON->OFF->ON is exact.
	void Set_Enabled(bool enabled)
	{
		if (g_Enabled == enabled)
			return;
		g_Enabled = enabled;
		fprintf(stderr, "[GN] per-pixel lighting master switch -> %s\n", g_Enabled ? "ON" : "OFF");
		fflush(stderr);
	}

	bool Is_Enabled()
	{
		return g_Enabled;
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

	// GeneralsX @feature Qoder 17/10/2026 P2: skinned-batch GN variant pair (see header). Install
	// failure (0 handles) is silent-safe: Apply's DUV2 branch then returns false and every skin
	// batch keeps the fixed pipeline, exactly the pre-P2 behavior.
	void Install_Skin(UINT_PTR vs_handle, UINT_PTR ps_handle)
	{
		g_VS_SKIN = vs_handle;
		g_PS_SKIN = ps_handle;
		fprintf(stderr, "[GN] skinned variant pair installed vs=%p ps=%p\n",
			(void*)vs_handle, (void*)ps_handle);
		fflush(stderr);
	}

	// GeneralsX @feature Qoder 17/10/2026 P3 (逐像素光照总闸): merged TG3 pair (see the statics block
	// above). A 0-handle install is silent-safe: Apply's TG3 branch then returns false and bump/
	// roughness meshes keep the fixed pipeline exactly like a master-switch-OFF frame does today.
	void Install_Merged(UINT_PTR vs_handle, UINT_PTR ps_handle)
	{
		g_VS_MERGED = vs_handle;
		g_PS_MERGED = ps_handle;
		fprintf(stderr, "[GNM] merged TG3 pair registered vs=%p ps=%p\n",
			(void*)vs_handle, (void*)ps_handle);
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

	bool Apply(const Matrix3D& world, const VertexMaterialClass* vm, int fvf, LightEnvironmentClass* lenv, TextureClass* diffuse)
	{
		if (!Ready_To_Render()) return false;
		UINT_PTR vs = 0;
		bool skinBatch = false;
		bool merged = false;
		TextureClass* nm = nullptr;
		TextureClass* rp = nullptr;
		if (fvf == (int)DX8_FVF_XYZNDUV1TG3) {
			// GeneralsX @feature Qoder 17/10/2026 P3 (逐像素光照总闸): the merged base mount for the
			// tangent-space layout - rigid bump meshes AND tangent skin batches share this enum
			// (dynamic_fvf_type_nm), and skins render identity-world so the same uploads serve both.
			// The two detail layers are probed with DX8NormalMap's OWN cached finders (the #68
			// team-colour and #87 sub-128px rejections live inside them and therefore carry over
			// verbatim): bump reads the _nm companion only while the 凹凸 menu row is ON, roughness
			// reads _rp whenever the PBR mode is armed - the same decoupling #70 shipped. No companion
			// at all = return false, so these batches keep the exact fixed-pipeline pixels they had
			// before P3 (the legacy single-light mount never took them either).
			if (g_VS_MERGED == 0 || g_PS_MERGED == 0) return false;
			if (diffuse == nullptr) return false;
			nm = DX8NormalMap::Is_Enabled() ? DX8NormalMap::Find_Companion_Normal(diffuse) : nullptr;
			rp = DX8NormalMap::Get_PBR_Mode() > 0 ? DX8NormalMap::Find_Companion_RP(diffuse) : nullptr;
			if (nm == nullptr && rp == nullptr) return false;
			vs = g_VS_MERGED;
			merged = true;
		}
		else if (fvf == (int)DX8_FVF_XYZNUV1) vs = g_VS_DUV1;
		else if (fvf == (int)DX8_FVF_XYZNUV2) vs = g_VS_DUV2;
		else if (fvf == (int)DX8_FVF_XYZNDUV2) {
			// GeneralsX @feature Qoder 17/10/2026 P2: skinned batch (the dynamic CPU-skinning layout).
			// NO fallback to the rigid pair when the skin pair is missing: a rigid VS declares no
			// COLOR0, so the D3D9 vertex declaration would drop the vertex tint/flash and the batch
			// would render visibly wrong. Both handles required, else the batch stays fixed pipeline.
			if (g_VS_SKIN == 0 || g_PS_SKIN == 0) return false;
			vs = g_VS_SKIN;
			skinBatch = true;
		}
		else return false;
		if (vs == 0) return false;

		// GeneralsX @feature PS3.0 P3-21b 14/10/2026: prefer the shadow-aware variants when a sun map
		// is active and a variant exists for this FVF; otherwise the plain GN pair runs unchanged.
		// GeneralsX @feature Qoder 17/10/2026 P3: the MERGED pair carries its own shadow branch
		// (c27.z), so it takes the sun map on EVERY TG3 batch - rigid bump meshes gain shadow
		// reception they never had (addition, not regression) and the P2 "skins never get shadows"
		// limit dissolves (the merged program relays COLOR0 AND builds sv from the light-clip rows).
		// The 5x5 PCF tier still ships only on the plain rigid variants (3x3 kernel in the merged PS;
		// the game layer's spread doubling for PCF5 widens the merged blur correspondingly - visible
		// only with PCF=5 + a bump mesh inside the shadow, both opt-in).
		bool useShadow = false;
		UINT_PTR psSel = merged ? g_PS_MERGED : (skinBatch ? g_PS_SKIN : g_PS);
		if (merged) {
			useShadow = sunShadowActive();
		} else if (!skinBatch && sunShadowActive()) {
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

		// GeneralsX @bugfix Qoder 18/10/2026 pure-Vulkan wedge 8: decline the mount when this exact
		// pair has no translated twin. Reached before any constant/texture upload, so returning false
		// leaves the batch untouched for the fixed pipeline.
		if (!vkPairTranslated(vs, psSel, merged ? "GNMerged" : (skinBatch ? "GNSkin" : "GN"))) return false;

		// Same WVP convention as the old dot3 path (raw DX8 transforms, transpose once);
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
		// GeneralsX @bugfix PS3.0-#76b 16/10/2026: the GN VS builds the world normal as
		// dot(normal, c4..c6), so the three uploaded rows must be the Westwood world matrix ROWS
		// (Rotate_Vector convention: object -> world). The old wmDx8 upload sent its rows, which are
		// the wm columns = Inverse_Rotate = world -> model, so rotated meshes lit with a 2-theta error.
		// Same error, seen from the other side of the dot product: Apply over-rotated the light
		// (dot(n, R*Lw)), GN under-rotated the normal (dot(R^T*n, Lw)) - the two are equal, so both
		// families put the brightest face on the world normal R^2*Lw and both disagreed with the
		// fixed pipeline on exactly the same meshes, by exactly the same amount. That is why GN was
		// never a valid "correct reference" to diff the dot3 path against.
		const float* m = reinterpret_cast<const float*>(&wm);
		DX8Wrapper::Set_Vertex_Shader_Constant(4, m, 3);             // VS c4..c6 = world matrix rows (normal -> world)
#if defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 20/10/2026 Forward: full world rows for the per-pixel point-light
		// world position. c11..c14 are free in every GN vertex twin (c7..c10 stay the shadow rows).
		DX8Wrapper::Set_Vertex_Shader_Constant(11, m, 4);            // VS c11..c14 = world rows (object -> world)
#endif

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
#if defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 20/10/2026 Forward: point lights no longer share the directional
		// slots. They are evaluated per pixel from PS c32..c48 (world center, inner radius, raw diffuse,
		// outer radius), so a lamp or explosion attenuates across the surface instead of being flattened
		// to one per-object direction. This is also what keeps the batch on the per-pixel path: the old
		// "any point light -> fixed pipeline" rule is what turned tanks vertex-lit when a muzzle flash
		// lit them and made a building lamp drop its normal map.
		Vector4 pts[16];
		int np = 0;
#endif
		Vector3 amb(0.0f, 0.0f, 0.0f);
		int n = 0;

		if (lenv != nullptr) {
			const Vector3& envAmb = lenv->Get_Equivalent_Ambient();
			amb.X = ma.X * envAmb.X + me.X;
			amb.Y = ma.Y * envAmb.Y + me.Y;
			amb.Z = ma.Z * envAmb.Z + me.Z;
			int lc = lenv->Get_Light_Count();
			if (lc > 8) lc = 8;
#if defined(SAGE_USE_VULKAN_CORE)
			for (int li = 0; li < lc; ++li) {
				if (lenv->isPointLight(li)) {
					// A rejected point light was folded into the ambient term; adding its raw diffuse
					// here would double-count it. Far lights are NOT rejected: their per-pixel falloff
					// is exactly what lets a lamp on a large building light the part it reaches even
					// when the object's center is outside the radius.
					if (np < 8 && !lenv->isPointDiffuseRejected(li)) {
						const Vector3& c = lenv->getPointCenter(li);
						const Vector3& pd = lenv->getPointDiffuse(li);   // intensity-scaled, unattenuated
						pts[np * 2]     = Vector4(c.X, c.Y, c.Z, lenv->getPointIrad(li));
						pts[np * 2 + 1] = Vector4(pd.X * md.X, pd.Y * md.Y, pd.Z * md.Z, lenv->getPointOrad(li));
						++np;
					}
					continue;
				}
				Vector3 d = lenv->Get_Light_Direction(li);   // world dir TOWARD the light
				Normalize(d);
				const Vector3& c = lenv->Get_Light_Diffuse(li);
				dirs[n] = Vector4(d.X, d.Y, d.Z, 0.0f);
				cols[n] = Vector4(c.X * md.X, c.Y * md.Y, c.Z * md.Z, 0.0f);
				++n;
			}
#else
			for (int li = 0; li < lc; ++li) {
				Vector3 d = lenv->Get_Light_Direction(li);   // world dir TOWARD the light
				Normalize(d);
				const Vector3& c = lenv->Get_Light_Diffuse(li);
				dirs[li] = Vector4(d.X, d.Y, d.Z, 0.0f);
				cols[li] = Vector4(c.X * md.X, c.Y * md.Y, c.Z * md.Z, 0.0f);
			}
			n = lc;
#endif
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

		// GeneralsX @feature PS3.0-#75 16/10/2026: render-time sun probe for this GN batch. The tag
		// carries the per-batch LightEnvironment pointer + light count, so distinct lenvs that hand out
		// different dir0 print one line each - that is what this line can prove (light-source selection).
		// Corrected in #76b: it is NOT the smoking gun for "same frame, half the base dark". GN lights
		// with WORLD-space dirs and rotates the vertex normal by the world rows, so a per-mesh rotate
		// error never shows up here; that bug only appeared in the model-space c4/c5/c9 uploads.
		{
			char tag[48];
			_snprintf(tag, sizeof(tag), "lenv=%p lc=%d", (const void*)lenv, n);
			const Vector3 d0(dirs[0].X, dirs[0].Y, dirs[0].Z);
			DX8NormalMap::log_sun_once("GN", tag, d0, amb);
		}

		Vector4 c4amb(amb.X, amb.Y, amb.Z, 1.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(4, &c4amb, 1);          // PS c4 = material-scaled ambient
		DX8Wrapper::Set_Pixel_Shader_Constant(5, &dirs[0], 4);        // PS c5..c8 = world light dirs 0..3
		DX8Wrapper::Set_Pixel_Shader_Constant(9, &cols[0], 4);        // PS c9..c12 = matDiffuse-scaled colors 0..3
		DX8Wrapper::Set_Pixel_Shader_Constant(17, &dirs[4], 4);       // PS c17..c20 = world light dirs 4..7 (P3-18)
		DX8Wrapper::Set_Pixel_Shader_Constant(21, &cols[4], 4);       // PS c21..c24 = matDiffuse-scaled colors 4..7 (P3-18)
#if defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 20/10/2026 Forward: point-light block. c32.x = count, c33..c48 =
		// 8 x (world center.xyz, inner radius) / (material-scaled diffuse.rgb, outer radius). Unused
		// pairs are zeroed so the PS loop can stop on the count alone.
		for (int pi = np; pi < 8; ++pi) {
			pts[pi * 2] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
			pts[pi * 2 + 1] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
		}
		Vector4 pcnt((float)np, 0.0f, 0.0f, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(32, &pcnt, 1);         // PS c32 = point-light count
		DX8Wrapper::Set_Pixel_Shader_Constant(33, &pts[0], 16);      // PS c33..c48 = 8 point lights
		// GeneralsX @feature Qoder 20/10/2026 Forward+: c29 = material diffuse for the global tile
		// lights (their colors are raw, unlike the per-object cols above), c31.x = tile path selector.
		// When the frame published no point lights the shader keeps the per-object c32..c48 list.
		{
			Vector4 c29(md.X, md.Y, md.Z, 0.0f);
			DX8Wrapper::Set_Pixel_Shader_Constant(29, &c29, 1);
			Vector4 c31((float)(VK_Forward_Active() ? 1 : 0), 0.0f, 0.0f, 0.0f);
			DX8Wrapper::Set_Pixel_Shader_Constant(31, &c31, 1);
			// GeneralsX @bugfix Qoder 21/10/2026 GNATEST: the GN family is mounted on NUV1/NUV2
			// vegetation (hedges, dogwood, tree drawables) whose textures are alpha-cut. A custom PS
			// has no fixed-function alpha test, so without this the whole leaf/hedge card renders as
			// an opaque square. c30 = (enable, D3DCMP ordinal, reference, 0), sampled from the same
			// states the fixed pipeline would use at this draw.
			Vector4 c30(
				(float)(DX8Wrapper::VK_Read_RS(D3DRS_ALPHATESTENABLE) ? 1 : 0),
				(float)(DX8Wrapper::VK_Read_RS(D3DRS_ALPHAFUNC) & 7u),
				(float)(DX8Wrapper::VK_Read_RS(D3DRS_ALPHAREF) & 0xFFu),
				0.0f);
			DX8Wrapper::Set_Pixel_Shader_Constant(30, &c30, 1);
		}
		// GeneralsX @debug Qoder 20/10/2026 Forward: proof that a point-lit batch stayed on the
		// per-pixel path (the old gate would have dropped it to the fixed pipeline). Bounded.
		{
			static int s_gnptLog = 0;
			if (np > 0 && s_gnptLog < 12) {
				++s_gnptLog;
				fprintf(stderr, "[GNPT] point-lit GN batch: dirs=%d pts=%d fvf=0x%08X tex=%s\n",
					n, np, (unsigned)fvf, diffuse ? (const char*)diffuse->Get_Texture_Name() : "(none)");
				fflush(stderr);
			}
		}
#endif
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
		// GENERALSX_GEOMNORMAL_SPECPOWER (default 24) / GENERALSX_GEOMNORMAL_SPEC (default 0.12).
		// GeneralsX @tweak Qoder 17/10/2026 P0 of the per-pixel master switch: the factory value was
		// walked up to 0.35 on 14/10 for the showcase and that, plus HEMI 0.35, is what made GN read
		// whitened and got the path pinned off on the user's machine (PS3.0 升级过程/PS3.0升级实施记录.md
		// §26.3 had already registered 0.12 as the retail-parity fix, awaiting the user's go-ahead -
		// given now on 17/10 as part of promoting GN to the menu's "逐像素光照" master switch). 0.12
		// equals the proven dot3 single-light spec (c6.y), so curved tops keep their retail brightness.
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
				specInt = 0.12f;   // GeneralsX @tweak Qoder 17/10/2026: retail parity (showcase was 0.35)
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

		// GeneralsX @feature Qoder 17/10/2026 P3 (逐像素光照总闸): merged-pair layer contract uploads.
		// c27 = (bumpK, bumpOn, shadowOn, rpOn) selects the detail branches; c28 = (mode, LI, ENV,
		// wetness) re-expresses the proven PBR c8 contract on this program. bumpK keeps the
		// #69 slider mapping (default 0.8 -> xy deviation 2.5) so the 凹凸强度 slider drives the merged
		// bump exactly like it drove the dot3 one. When no sun map is active the light-clip rows go
		// identity: sv is still computed but c27.z=0 branches the taps out, so the pixel never reads the
		// unbound stage 2. Companion stages are ALWAYS (re)bound - nullptr included - so the wrapper
		// cache can never hand a stale _nm/_rp to the next batch, same discipline as the P4-1/P3-21b
		// unbinds in the two Restore paths.
		if (merged) {
			if (!useShadow) {
				static const float kIdentity4[16] = {
					1.0f, 0.0f, 0.0f, 0.0f,
					0.0f, 1.0f, 0.0f, 0.0f,
					0.0f, 0.0f, 1.0f, 0.0f,
					0.0f, 0.0f, 0.0f, 1.0f };
				DX8Wrapper::Set_Vertex_Shader_Constant(7, kIdentity4, 4);   // VS c7..c10 = identity (sv unused)
				// GeneralsX @bugfix Qoder 17/10/2026 P3 (self-caught while wiring): sticky categories can
				// hand the merged pair straight after a point-lit batch and leave the _rp texture on
				// stage 2. GN's Restore only unbinds s2 when the shadow
				// flag was set, so without this the stale rp would outlive the category and leak into
				// the next multi-stage fixed-pipeline material - the exact P4-1 hazard, closed the same way.
				DX8Wrapper::Set_Texture(2, nullptr);
			}
			float bumpK = DX8NormalMap::Get_Strength() * 3.125f;
			if (bumpK < 0.0f) bumpK = 0.0f;
			const Vector4 c27(bumpK, nm != nullptr ? 1.0f : 0.0f, useShadow ? 1.0f : 0.0f, rp != nullptr ? 1.0f : 0.0f);
			DX8Wrapper::Set_Pixel_Shader_Constant(27, &c27, 1);
			const Vector4 c28((float)DX8NormalMap::Get_PBR_Mode(), DX8NormalMap::PBR_LI(), DX8NormalMap::PBR_ENV(), DX8NormalMap::g_Wetness);
			DX8Wrapper::Set_Pixel_Shader_Constant(28, &c28, 1);
			DX8Wrapper::Set_Texture(1, nm);
			DX8Wrapper::Set_Texture(3, rp);
			g_mergedBoundThisBatch = true;
			static int s_gnmMounted = 0;
			if (s_gnmMounted < 8) {
				++s_gnmMounted;
				fprintf(stderr, "[GNM] merged mounted on diffuse='%s' nm=%d rp=%d mode=%d shadow=%d\n",
					(const char*)diffuse->Get_Texture_Name(), nm != nullptr ? 1 : 0, rp != nullptr ? 1 : 0,
					DX8NormalMap::Get_PBR_Mode(), useShadow ? 1 : 0);
				fflush(stderr);
			}
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
		// GeneralsX @feature Qoder 17/10/2026 P3: the merged pair's companion stages (s1 _nm, s3 _rp)
		// ride the same unbind discipline - and only then, so plain GN batches never touch stage 1/3.
		if (g_mergedBoundThisBatch) {
			DX8Wrapper::Set_Texture(1, nullptr);
			DX8Wrapper::Set_Texture(3, nullptr);
			g_mergedBoundThisBatch = false;
		}
		DX8Wrapper::Set_Pixel_Shader(0);
		DX8Wrapper::Set_Vertex_Shader(0);
		DX8Wrapper::Set_Vertex_FVF((DWORD)fvf);
	}
}
