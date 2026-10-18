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

// FILE: W3DSunShadowMap.cpp //////////////////////////////////////////////////
//
// GeneralsX @feature PS3.0 T6 - real depth-buffered sun shadow map for terrain.
//
// V1 scope (deliberate, see docs/WORKDIR PS3.0 notes):
//   Casters  = every RenderObjClass registered with the shadow manager (m_shadowList + m_decalList),
//              fed through the existing Special_Render(RENDER_SHADOW) machinery via a new
//              SpecialRenderInfoClass::SunSink.  Skinned HLod parts come along for free because
//              get_deformed_screenspace_vertices already handles HTree bone links.
//   Receiver = the terrain heightmap grid around the visible window, drawn post-scene with a
//              3x3 PCF pixel shader in the same multiplicative blend the retail decal shadows use
//              (W3DProjectedShadow::flushDecals / renderProjectedTerrainShadow are the precedent).
//              Static structures and units do not receive the map shadow in V1.
//   Map      = one R32F texture (1024, stepping down 512/256 if creation fails) plus a matched-size
//              D24S8 depth surface.  Size mismatch is NOT tolerated by native d3d9 (silently drops
//              every primitive - shadow_map_probe Q1 lesson), and the depth format question was
//              settled there too: D24S8 is not sampleable on this stack, so depth lives in R.
//
// Depth conventions (pinned down during T6 recon):
//   The sun camera is ORTHO.  get_deformed_screenspace_vertices composes Get_Projection_Matrix()
//   (GL convention, z in [-1,1]) so the sink rebases z' = 0.5*z + 0.5 itself; the receiver does the
//   identical mapping on its terrain vertices, keeping producer and consumer in one convention
//   without touching the shared helper.
//   RHW vertices are viewport-clipped (probe lesson 8) - the occluder viewport is set to the full
//   map before any draw - and skip the viewport transform (lesson 9), so the sink writes pixel
//   coordinates, not clip coordinates.
//
// Off unless GENERALSX_SHADOWMAP is set to 1..3; runtime HLSL compilation is Windows-only, so the
// whole path stays inert on other platforms until the shaders ship as build-time assets.
//
///////////////////////////////////////////////////////////////////////////////

#include "W3DDevice/GameClient/W3DSunShadowMap.h"

#include "WWLib/always.h"
#include "Lib/BaseType.h"
#include "WW3D2/camera.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/texture.h"					// GeneralsX @feature PS3.0-P3-21b wrapper for the map
#include "WW3D2/dx8normalmap.h"				// GeneralsX @feature PS3.0-P3-21b DX8GeomNormal sun-shadow API
// GeneralsX @feature PS3.0-P3-3r 14/10/2026 Cached capability answers (the [CAPS4] probes) and the
// D3DFORMAT <-> WW3DFormat bridge, so the map-format ladder can ask the engine instead of re-querying
// the device for every rung.
#include "WW3D2/dx8caps.h"
#include "WW3D2/formconv.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/sunsink.h"
#include "WWMath/matrix3d.h"
#include "WWMath/matrix4.h"
#include "W3DDevice/GameClient/BaseHeightMap.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DProjectedShadow.h"
#include "Common/OptionPreferences.h"	// GeneralsX @feature 13/10/2026 ShadowMap S-family knobs
#include "W3DDevice/GameClient/W3DShadow.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "GameLogic/TerrainLogic.h"		// MAP_XY_FACTOR / MAP_HEIGHT_SCALE
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>

#ifdef _WIN32
// ---- generated shader programs (runtime HLSL, same boundary as the T2 blur pass) ------------

// Writes the occluder's [0,1] depth, carried in TEXCOORD0.x by the RHW sink vertex, into the red
// channel of the float depth map (format chosen by the RT+filter-sampleable ladder).
static const char* kDepthWriteShader =
	"float4 main(float2 dv : TEXCOORD0) : COLOR\n"
	"{\n"
	"	return float4(dv.x, 0.0, 0.0, 1.0);\n"
	"}\n";

// Far-field init for the map: red = 1.0 means "nothing closer than anything", so unoccluded
// terrain compares stored 1.0 + bias against its own depth and stays lit.
static const char* kFillFarShader =
	"float4 main() : COLOR\n"
	"{\n"
	"	return float4(1.0, 0.0, 0.0, 1.0);\n"
	"}\n";

// Terrain receiver: 3x3 PCF against the red channel.  c0 = (texelOffset, texelOffset, bias, strength).
// Multiplicative blend happens in the render states (SRCBLEND_ZERO / DSTBLEND_SRC_COLOR), so the
// shader outputs lit = 1.0 and shadow = 1-strength as plain gray.
// GeneralsX @bugfix note: strict mode (D3DCOMPILE_ENABLE_STRICTNESS, the runtime-identical flags the
// compile ladder uses) rejects the deprecated sampler2D keyword - Texture2D + SamplerState register
// syntax is required, same as the live [SMAP] failure 13/10/2026 showed.
// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: the depth axis is now guarded as well as uv.  Shell
// forensics (2026-10-14, GENERALSX_SHADOWMAP=1 at the menu): sun-space depths above 1.0 (pxbox z max
// 1.218..1.713) made every PCF comparison fail for those texels, so the multiplicative receiver shaded
// the WHOLE visible terrain (mean 13/255 ON vs 77/255 OFF, only the UI layer stayed lit).  Any fragment
// whose remapped depth lands outside [0,1] is outside the ortho slab along the sun axis and is forced
// fully lit instead of sampling clamped edge texels.
static const char* kPcfReceiveShader =
	"Texture2D mapTex : register(t0);\n"
	"SamplerState samp : register(s0);\n"
	"float4 c0 : register(c0);\n"	// strict mode needs the explicit constant declaration (blur shader precedent)
	"float4 main(float2 uv : TEXCOORD0, float2 md : TEXCOORD1) : COLOR\n"
	"{\n"
	"	float shade = 1.0;\n"
	"	if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0 && md.x >= 0.0 && md.x <= 1.0)\n"
	"	{\n"
	"		float myDepth = md.x;\n"
	"		float count = 0.0;\n"
	"		count += (mapTex.Sample(samp, uv + float2(-c0.x, -c0.x)).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		count += (mapTex.Sample(samp, uv + float2( 0.0,  -c0.x)).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		count += (mapTex.Sample(samp, uv + float2( c0.x, -c0.x)).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		count += (mapTex.Sample(samp, uv + float2(-c0.x,  0.0)).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		count += (mapTex.Sample(samp, uv).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		count += (mapTex.Sample(samp, uv + float2( c0.x,  0.0)).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		count += (mapTex.Sample(samp, uv + float2(-c0.x,  c0.x)).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		count += (mapTex.Sample(samp, uv + float2( 0.0,   c0.x)).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		count += (mapTex.Sample(samp, uv + float2( c0.x,  c0.x)).x + c0.z < myDepth) ? 1.0 : 0.0;\n"
	"		shade = 1.0 - c0.w * (count / 9.0);\n"
	"	}\n"
	"	return float4(shade, shade, shade, 1.0);\n"
	"}\n";

// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: the receiver's pixel-shader texture reads return 0 in
// the fixed-function-vertex + pixel-shader combination on this runtime - the constant-PS probe (no
// texture) dims the scene exactly x0.75 while echo probes against the map AND against a plain
// 191-filled A8R8G8B8 dummy both sample 0 (P2W0n forensics).  Every working sampling pass in the
// engine (GN/ML/bloom/dot3) uses a programmable vertex shader, so the receiver now submits through
// this pass-through VS.  GeneralsX @bugfix PS3.0-P3-21a 14/10/2026: the vertices carry the CPU-side
// clip-space product of view*proj WITHOUT the perspective divide - the divide and the near-plane clip
// belong to the rasterizer, and feeding it an already-divided triple with w=0 left the grid with no
// valid 1/w to interpolate, which is why the receiver drew nothing at all.
static const char* kRecvPassthroughVSShader =
	"struct VS_OUT\n"
	"{\n"
	"	float4 clip : POSITION;\n"
	"	float2 uv : TEXCOORD0;\n"
	"	float2 md : TEXCOORD1;\n"
	"};\n"
	"VS_OUT main(float3 pos : POSITION, float2 uv : TEXCOORD0, float2 md : TEXCOORD1)\n"
	"{\n"
	"	VS_OUT o;\n"
	"	o.clip = float4(pos, md.y);\n"	// true clip coords; md.y carries w so the rasterizer clips and divides
	"	o.uv = uv;\n"
	"	o.md = float2(md.x, 0.0);\n"		// md.x = own depth in the map convention, unchanged
	"	return o;\n"
	"}\n";
#endif // _WIN32

// ---- module state ----------------------------------------------------------------------------

// GeneralsX @feature PS3.0-P3-22 14/10/2026: 2048 joins the ladder at the top (S2 tier request);
// creation failure still steps down exactly like before.
static const int kMapSizeLadder[4] = { 2048, 1024, 512, 256 };

static Int		g_mapSize			= 0;		// edge that actually got created
static bool		g_wantMap				= false;	// env gate, read once
static bool		g_shallowSunWarned	= false;
#ifdef _WIN32
static IDirect3DTexture9*			g_mapTex			= nullptr;
static IDirect3DSurface9*			g_mapSurface		= nullptr;
static IDirect3DSurface9*			g_depthSurface		= nullptr;
static IDirect3DPixelShader9*		g_depthPS			= nullptr;
static IDirect3DPixelShader9*		g_fillPS			= nullptr;
static IDirect3DPixelShader9*		g_pcfPS			= nullptr;
static IDirect3DVertexShader9*		g_recvVS		= nullptr;	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026 pass-through VS
static D3DFORMAT			g_mapFmt		= D3DFMT_UNKNOWN;	// format that passed RT+filter-sample query and got created
// GeneralsX @feature PS3.0-P3-21b 14/10/2026: TextureClass shell that adopts g_mapTex (AlphaTerrainTextureClass
// precedent, TerrainTex.cpp) so DX8GeomNormal can bind the map through the wrapper's cached Set_Texture path -
// the raw device SetTexture the overlay used bypasses that cache and is what the runtime punishes with
// zero reads.  Borrowed pointer between Set_Sun_Shadow and Clear_Sun_Shadow.
static TextureClass*		g_mapTexWrapper	= nullptr;
#endif
static bool		g_resourcesReady	= false;
static bool		g_initAttempted		= false;

static CameraClass*			g_sunCam			= nullptr;
static Matrix4x4			g_sunM;					// GL-convention ortho proj * view; sink and receiver share it
static Real						g_sunZNearDist		= 0.0f;		// positive clip distances
static Real						g_sunZFarDist		= 0.0f;
static Real						g_sunHalfWidth		= 0.0f;		// ortho half extent in world units
static Vector3				g_sunDir;					// unit: terrain -> sun
static AABoxClass			g_sunBox;					// texel-snapped visible-terrain box
static bool						g_frameActive			= false;		// map valid for THIS frame's receiver
// GeneralsX @feature PS3.0-P3-23 14/10/2026: S4 composition flags. g_gnArmed mirrors "GN reception is
// mounted for THIS frame" and g_gnStrength carries its premultiplied darkening; both are read by the
// volumetric shadow pass (W3DVolumetricShadow) so the two screen layers compose instead of stacking.
static bool						g_gnArmed				= false;
static float					g_gnStrength			= 0.0f;

static Int						g_statCastors			= 0;
static Int						g_statTris				= 0;
static Int						g_statSkipped			= 0;
static Int						g_frameCounter			= 0;

// ---- helpers ----------------------------------------------------------------------------------

static bool sunShadowMapEnabled()
{
	// gate read once per process; the options-menu registry (Options.ini ShadowMap) is the user
	// entry, GENERALSX_SHADOWMAP stays the QA override inside the getter (it wins either way).
	static bool gateRead = false;
	if (!gateRead)
	{
		g_wantMap = (OptionPreferences().getShadowMapEnabled() != FALSE);
		gateRead = true;
	}
	return g_wantMap;
}

// GeneralsX @feature PS3.0-P3-21b 14/10/2026: GN-reception mode, default ON (GENERALSX_SMAP_GNRECV=0
// forces the old terrain overlay back for A/B runs).  The RECMODE 5/6 discriminator legs (14/10) proved
// the raw overlay draw cannot sample ANY texture on this runtime - it paints its ~75% screen grid BLACK -
// so while GN reception is mounted the overlay is parked and the map is pushed to DX8GeomNormal instead:
// buildings and units (the rigid XYZNUV1/2 batches the GN pass already lights per-pixel) now receive each
// other's sun shadows, with the light-0-only attenuation the S-line spec asks for.
static bool sunShadowGNReception()
{
	static int s_gn = -1;
	if (s_gn < 0)
	{
		const char* e = getenv("GENERALSX_SMAP_GNRECV");
		s_gn = (e != nullptr && strcmp(e, "0") == 0) ? 0 : 1;
	}
	return s_gn != 0;
}

// GeneralsX @feature PS3.0-P3-22 14/10/2026: PCF kernel tier for the GN reception variants -
// GENERALSX_SMAP_PCF=5 selects the 5x5 (25-tap) pixel shader, anything else (default) keeps the
// 3x3 (9-tap) one.  The penumbra spread handed to the shader scales with the kernel so both tiers
// blur the same world-space width at equal map size, and the 1/mapSize factor keeps softening
// proportionate when the resolution dropdown moves between 512/1024/2048 (S2 "比例软化").
static int sunShadowPCFTaps()
{
	static int s_pcf = -1;
	if (s_pcf < 0)
	{
		const char* e = getenv("GENERALSX_SMAP_PCF");
		s_pcf = (e != nullptr && atoi(e) == 5) ? 5 : 3;
	}
	return s_pcf;
}

// GeneralsX @feature PS3.0-P3-22 14/10/2026: QA-only map-size override so A/B legs can pin a tier
// without editing Options.ini; 0 = follow the user dropdown. Creation failure still steps down.
static int sunShadowMapSizeOverride()
{
	static int s_sz = -1;
	if (s_sz < 0)
	{
		const char* e = getenv("GENERALSX_SMAP_SIZE");
		s_sz = e ? atoi(e) : 0;
	}
	return s_sz;
}

#ifdef _WIN32

// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: name for the [SMAP] install line.
static const char* mapFmtName(D3DFORMAT fmt)
{
	switch ((unsigned)fmt)
	{
	case D3DFMT_R32F:				return "R32F";
	case D3DFMT_A16B16G16R16F:	return "A16B16G16R16F";
	case D3DFMT_R16F:				return "R16F";
	case D3DFMT_A2B10G10R10:		return "A2B10G10R10";
	default:							return "unknown";
	}
}

// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: shell-black forensics proved the R32F map content is
// visible to a GetRenderTargetData readback (0.75 marker test) while the PS sampler reads 0 on this
// native d3d9 runtime.  D3DFMT_R32F textures are not guaranteed sampleable/filterable by the D3D9
// capability model, so the map format is now chosen from a ladder whose members are each vetted at
// creation time for render-target + filter-sample support.
static bool rtFormatFilterSampleable(IDirect3DDevice9* dev, D3DFORMAT fmt)
{
	// The D3D9 device itself has no CheckDeviceFormat / GetPresentationParameters - query the
	// IDirect3D9 that created it, with adapter ordinal/type from GetCreationParameters and the
	// adapter format from GetDisplayMode.
	IDirect3D9* d3d = nullptr;
	if (FAILED(dev->GetDirect3D(&d3d)) || !d3d)
		return false;
	D3DDISPLAYMODE mode;
	D3DDEVICE_CREATION_PARAMETERS cp;
	const bool haveMode = SUCCEEDED(dev->GetDisplayMode(0, &mode));
	const bool haveCp = SUCCEEDED(dev->GetCreationParameters(&cp));
	bool ok = false;
	if (haveMode && haveCp)
	{
		ok = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
			D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, fmt)) ? true : false;
	}
	d3d->Release();
	return ok;
}

// GeneralsX @feature PS3.0-P3-3r 14/10/2026 I4 consumer for the cached capability answers: DX8Caps now
// keeps the render-target table (which since P3-9 includes the float formats) together with the
// R32F / A16B16G16R16F filter-sample probes, so a ladder rung that the engine already knows is
// un-sampleable is skipped without touching the driver. Rungs the caps table does not cover (R16F,
// A2B10G10R10) still fall through to the live CheckDeviceFormat probe below.
static bool mapFormatFilterSampleable(IDirect3DDevice9* dev, D3DFORMAT fmt, const char** source = nullptr)
{
	if (source) *source = "driver";
	const DX8Caps* caps=DX8Wrapper::Get_Current_Caps();
	if (caps)
	{
		const WW3DFormat ww3dFormat=D3DFormat_To_WW3DFormat(fmt);
		switch (ww3dFormat)
		{
		case WW3D_FORMAT_R32F:
		case WW3D_FORMAT_A16B16G16R16F:
			if (source) *source = "cachedCaps";
			return caps->Support_Filterable_Render_Target_Format(ww3dFormat);
		default:
			break;
		}
	}
	return rtFormatFilterSampleable(dev,fmt);
}

static bool ensureResources(IDirect3DDevice9* dev)
{
	if (g_resourcesReady)
		return true;

	const char* profile = nullptr;
	void* handle = nullptr;
	if (g_depthPS == nullptr &&
		SUCCEEDED(W3DShaderManager::compileShaderAtBestProfile(kDepthWriteShader, false, "[SMAP]", &profile, &handle)) && handle)
		g_depthPS = (IDirect3DPixelShader9*)handle;
	handle = nullptr;
	if (g_fillPS == nullptr &&
		SUCCEEDED(W3DShaderManager::compileShaderAtBestProfile(kFillFarShader, false, "[SMAP]", &profile, &handle)) && handle)
		g_fillPS = (IDirect3DPixelShader9*)handle;
	handle = nullptr;
	if (g_pcfPS == nullptr &&
		SUCCEEDED(W3DShaderManager::compileShaderAtBestProfile(kPcfReceiveShader, false, "[SMAP]", &profile, &handle)) && handle)
		g_pcfPS = (IDirect3DPixelShader9*)handle;
	handle = nullptr;
	if (g_recvVS == nullptr &&
		SUCCEEDED(W3DShaderManager::compileShaderAtBestProfile(kRecvPassthroughVSShader, true, "[SMAP]", &profile, &handle)) && handle)
		g_recvVS = (IDirect3DVertexShader9*)handle;
	if (!g_depthPS || !g_fillPS || !g_pcfPS)
		return false;

	// Size ladder: keep stepping down until a full RT+depth pair exists.  Matched sizes are
	// mandatory on native d3d9 (mismatch silently drops all primitives - probe Q1 lesson).
	// GeneralsX @feature 13/10/2026 the ladder starts at the user-requested edge (Options.ini
	// SunShadowMapResolution, options-menu dropdown); creation failure still steps down.
	static int s_startTier = -1;
	if (s_startTier < 0)
	{
		// GeneralsX @feature PS3.0-P3-22 14/10/2026: ladder now tops out at 2048; the QA env
		// GENERALSX_SMAP_SIZE pins a tier for A/B legs, the dropdown still decides the default.
		int requested = sunShadowMapSizeOverride();
		if (requested <= 0)
			requested = OptionPreferences().getSunShadowMapResolution();
		s_startTier = (requested <= 256) ? 3 : (requested <= 512) ? 2 : (requested <= 1024) ? 1 : 0;
	}
	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: format ladder.  The 0.75-marker forensics
	// (20261014-110939 leg) proved the PS cannot read this USAGE_RENDERTARGET R32F texture on the
	// native d3d9 runtime even though GetRenderTargetData sees the content - D3D9 only requires
	// drivers to filter-sample formats that pass the QUERY_FILTER capability check, and R32F
	// render targets were created here without ever passing it.  Candidates are ordered by
	// shadow-map pedigree: A16B16G16R16F (the d3d9-era standard) first, R32F only as a last
	// resort and only when the driver declares it RT+filter sampleable.
	static const D3DFORMAT kFmtLadder[4] = { D3DFMT_A16B16G16R16F, D3DFMT_R16F, D3DFMT_A2B10G10R10, D3DFMT_R32F };
	// GeneralsX @feature PS3.0-P3-3r 14/10/2026 Two passes. The first binds only formats that pass the
	// shared RT+filter-sample vetting (the cached DX8Caps answer when the engine probed the format, the
	// live CheckDeviceFormat otherwise) - that vetting used to exist for R32F alone, which let an
	// un-sampleable A16B16G16R16F map be bound and produce the same black-receiver failure. If a runtime
	// rejects every candidate, the second pass falls back to the old unvetted behaviour so the feature
	// degrades to "shadows look like before" instead of switching itself off, and says so on the console.
	for (int attempt = 0; attempt < 2 && g_mapSize == 0; ++attempt)
	{
		const bool vettedOnly = (attempt == 0);
		for (int ladder = s_startTier; ladder < 4; ++ladder)
		{
			int size = kMapSizeLadder[ladder];
			for (int fi = 0; fi < 4; ++fi)
			{
				const D3DFORMAT fmt = kFmtLadder[fi];
				if (vettedOnly && !mapFormatFilterSampleable(dev, fmt))
					continue;	// never bind an unqualified float map - that was the shell-black bug
				IDirect3DTexture9* tex = nullptr;
				if (FAILED(dev->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex)
					continue;
				IDirect3DSurface9* surface = nullptr;
				if (FAILED(tex->GetSurfaceLevel(0, &surface)) || !surface)
				{
					tex->Release();
					continue;
				}
				IDirect3DSurface9* depth = nullptr;
				if (FAILED(dev->CreateDepthStencilSurface(size, size, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &depth, nullptr)) || !depth)
				{
					surface->Release();
					tex->Release();
					continue;
				}
				g_mapTex = tex;
				g_mapSurface = surface;
				g_depthSurface = depth;
				g_mapSize = size;
				g_mapFmt = fmt;
				break;
			}
			if (g_mapSize != 0)
				break;
		}
		if (!vettedOnly && g_mapSize != 0)
		{
			fprintf(stderr, "[SMAP] no float map format passed RT+filter vetting, bound %s unvetted\n",
				mapFmtName(g_mapFmt));
			fflush(stderr);
		}
	}

	if (g_mapSize == 0)
		return false;

	if (!g_sunCam)
		g_sunCam = NEW_REF(CameraClass, ());
	if (!g_sunCam)
		return false;

	// GeneralsX @feature PS3.0-P3-21b 14/10/2026: wrapper carrying the map into DX8Wrapper::Set_Texture
	// (8x8 throwaway + adopt, the AlphaTerrainTextureClass trick - the wrapper AddRefs the RT texture,
	// so release order below is safe).
	if (g_mapTexWrapper == nullptr)
	{
		g_mapTexWrapper = NEW_REF(TextureClass, (8, 8, WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_1));
		if (g_mapTexWrapper != nullptr)
			g_mapTexWrapper->Set_D3D_Base_Texture(g_mapTex);
		if (g_mapTexWrapper == nullptr)
			fprintf(stderr, "[SMAP] PROBLEM map wrapper allocation failed, GN reception off\n");
	}

	g_resourcesReady = true;
	g_initAttempted = true;
	fprintf(stderr, "[SMAP] installed size=%d fmt=%s profile=%s recvVS=%d\n",
		g_mapSize, mapFmtName(g_mapFmt), profile ? profile : "?", g_recvVS ? 1 : 0);
	fflush(stderr);
	return true;
}

// The geometry sink handed to SpecialRenderInfoClass::SunSink.  Converts the GL-convention
// clip-space vertices produced by get_deformed_screenspace_vertices into D3D RHW pixels
// (viewport transform is skipped by XYZRHW - write pixels, not NDC) and submits the mesh as an
// indexed UP triangle list whose TEXCOORD0.x carries the [0,1] depth.
class SunShadowSinkImpl : public SunShadowSinkClass
{
public:
	virtual void Submit_Screenspace_Triangles(const Vector4 *clip_verts, int vcount,
		const unsigned short *indices, int tri_count) override
	{
		IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
		if (!dev || !g_mapSurface || tri_count <= 0)
			return;
		if (vcount > 65536)
		{
			// 16-bit UP indices; oversized meshes are rare and simply do not cast in V1.
			++g_statSkipped;
			return;
		}

		m_scratch.resize(vcount);
		const Real half = 0.5f * (Real)g_mapSize;
		for (int i = 0; i < vcount; ++i)
		{
			// Ortho sun camera: w is always 1 (see sunsink.h contract), so no divide is needed.
			Real clipX = clip_verts[i].X;
			Real clipY = clip_verts[i].Y;
			Real clipZ = clip_verts[i].Z;
			RHWV& v = m_scratch[i];
			v.x = half * (clipX + 1.0f);
			v.y = half * (1.0f - clipY);					// pixel row 0 == top of the map
			v.z = 0.5f * clipZ + 0.5f;						// GL [-1,1] -> D3D [0,1]
			v.rhw = 1.0f;
			v.depth = v.z;
			v.pad = 0.0f;
			// Per-frame pixel-space bounds (T6 diag): if nothing rasterizes, the bbox says whether
			// vertices ever land inside the 0..mapSize viewport volume at all.
			if (!m_bbValid)
			{
				m_bbMinX = m_bbMaxX = v.x;
				m_bbMinY = m_bbMaxY = v.y;
				m_bbMinZ = m_bbMaxZ = v.z;
				m_bbValid = true;
			}
			else
			{
				if (v.x < m_bbMinX) m_bbMinX = v.x;		if (v.x > m_bbMaxX) m_bbMaxX = v.x;
				if (v.y < m_bbMinY) m_bbMinY = v.y;		if (v.y > m_bbMaxY) m_bbMaxY = v.y;
				if (v.z < m_bbMinZ) m_bbMinZ = v.z;		if (v.z > m_bbMaxZ) m_bbMaxZ = v.z;
			}
		}

		static const DWORD kSinkFVF = D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE2(0);
		dev->SetFVF(kSinkFVF);
		dev->SetVertexShader(nullptr);
		if (SUCCEEDED(dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vcount, tri_count,
			&indices[0], D3DFMT_INDEX16, &m_scratch[0], sizeof(RHWV))))
		{
			++g_statTris;
			m_triTotal += tri_count;
		}
	}

	Int m_triTotal = 0;

	// T6 diag state, reset once per occluder pass.
	float m_bbMinX = 0, m_bbMinY = 0, m_bbMinZ = 0;
	float m_bbMaxX = 0, m_bbMaxY = 0, m_bbMaxZ = 0;
	bool  m_bbValid = false;

	void Reset_Frame()
	{
		m_triTotal = 0;
		m_bbValid = false;
	}

private:
	struct RHWV
	{
		float x, y, z, rhw;
		float depth, pad;
	};
	std::vector<RHWV> m_scratch;
};

static SunShadowSinkImpl g_sink;

// Renders every registered caster from the sun's ortho view into the R32F depth map.
static void renderOccluderPass(IDirect3DDevice9* dev)
{
	HRESULT beginHr = dev->BeginScene();
	const bool ownScene = SUCCEEDED(beginHr);
	// If a scene is somehow already open we keep drawing inside it; nothing we do re-enters.

	IDirect3DSurface9* prevRT = nullptr;
	IDirect3DSurface9* prevDS = nullptr;
	D3DVIEWPORT9 prevVP;
	dev->GetRenderTarget(0, &prevRT);
	dev->GetDepthStencilSurface(&prevDS);
	dev->GetViewport(&prevVP);

	dev->SetRenderTarget(0, g_mapSurface);
	dev->SetDepthStencilSurface(g_depthSurface);

	D3DVIEWPORT9 vp;
	vp.X = 0; vp.Y = 0; vp.Width = (DWORD)g_mapSize; vp.Height = (DWORD)g_mapSize;
	vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
	dev->SetViewport(&vp);

	// Common fixed-function neutralization for RHW drawing.  RHW vertices skip the transform
	dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	// GeneralsX @bugfix Qoder 13/10/2026 T6 diag: the game leaves scissor state from the previous
	// frame's UI/RT renders; a stale or empty scissor rect silently drops every occluder pixel.
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE,
		D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
	dev->SetVertexShader(nullptr);
	dev->SetPixelShader(g_fillPS);
	dev->SetFVF(D3DFVF_XYZRHW);

	// Fill the map with "far" (red = 1).  R32F Clear semantics are unreliable, so this is drawn
	// as one fullscreen RHW quad - pixel coordinates, per probe lesson 9.
	{
		struct { float x, y, z, rhw; } quad[4] =
		{
			{ 0.0f, 0.0f, 1.0f, 1.0f },
			{ (float)g_mapSize, 0.0f, 1.0f, 1.0f },
			{ 0.0f, (float)g_mapSize, 1.0f, 1.0f },
			{ (float)g_mapSize, (float)g_mapSize, 1.0f, 1.0f },
		};
		dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
	}

	// Depth clear for the occluder Z competition (the RT itself is already filled).
	dev->Clear(0, nullptr, D3DCLEAR_ZBUFFER, 0x00000000u, 1.0f, 0);

	dev->SetPixelShader(g_depthPS);
	dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESS);

	// Walk both caster lists.  Culling beyond the ortho box is free: the RHW pipeline clips
	// against the map viewport (probe lesson 8), so off-box triangles simply do not rasterize.
	W3DProjectedShadowManager* mgr = TheW3DProjectedShadowManager;
	if (mgr)
	{
		W3DProjectedShadow* listHeads[2] = { mgr->peekShadowList(), mgr->peekDecalList() };
		for (int head = 0; head < 2; ++head)
		{
			for (W3DProjectedShadow* shadow = listHeads[head]; shadow != nullptr; shadow = shadow->peekNextShadow())
			{
				RenderObjClass* robj = shadow->peekCaster();
				if (!robj)
					continue;
				SpecialRenderInfoClass ctx(*g_sunCam, SpecialRenderInfoClass::RENDER_SHADOW);
				ctx.SunSink = &g_sink;
				robj->Special_Render(ctx);
				++g_statCastors;
			}
		}
	}

	dev->SetPixelShader(nullptr);
	dev->SetRenderTarget(0, prevRT);
	dev->SetDepthStencilSurface(prevDS);
	dev->SetViewport(&prevVP);
	if (prevRT) prevRT->Release();
	if (prevDS) prevDS->Release();
	if (ownScene)
		dev->EndScene();
	// The detour overwrote raw state DX8Wrapper caches (RT, shaders, states); make it re-apply
	// everything on the next draw, same contract as the T2 blur pass.
	DX8Wrapper::Invalidate_Cached_Render_States();
}

// Builds and draws the terrain-conforming receiver grid with 3x3 PCF, multiplying the scene.
static void renderTerrainGrid(IDirect3DDevice9* dev, CameraClass* sceneCamera)
{
	WorldHeightMap* hmap = TheTerrainRenderObject ? TheTerrainRenderObject->getMap() : nullptr;
	if (!hmap)
		return;

	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: the receiver cannot run without the pass-through VS
	// (fixed-function-vertex + PS texture reads return 0 on this runtime), so disable the pass entirely
	// rather than draw a black shell.
	if (!g_recvVS)
	{
		static bool s_vsMissingWarned = false;
		if (!s_vsMissingWarned)
		{
			s_vsMissingWarned = true;
			fprintf(stderr, "[SMAP] PROBLEM pass-through VS unavailable - receiver disabled\n");
			fflush(stderr);
		}
		return;
	}

	// Terrain cell window of the snapped sun box, clamped to the map and capped at 128 cells per
	// side by widening the stride (16-bit UP indices limit the grid to 129*129 vertices).
	Int startX = (Int)floor((g_sunBox.Center.X - g_sunBox.Extent.X) / MAP_XY_FACTOR);
	Int endX   = (Int)ceil((g_sunBox.Center.X + g_sunBox.Extent.X) / MAP_XY_FACTOR);
	Int startY = (Int)floor((g_sunBox.Center.Y - g_sunBox.Extent.Y) / MAP_XY_FACTOR);
	Int endY   = (Int)ceil((g_sunBox.Center.Y + g_sunBox.Extent.Y) / MAP_XY_FACTOR);
	startX = MAX(startX, 0);				endX = MIN(endX, hmap->getXExtent() - 1);
	startY = MAX(startY, 0);				endY = MIN(endY, hmap->getYExtent() - 1);
	if (endX - startX < 1 || endY - startY < 1)
		return;

	Int stride = 1;
	while (((endX - startX) / stride) > 128 || ((endY - startY) / stride) > 128)
		++stride;

	Int gw = (endX - startX) / stride + 1;
	Int gh = (endY - startY) / stride + 1;
	if (startX + (gw - 1) * stride < endX) ++gw;
	if (startY + (gh - 1) * stride < endY) ++gh;
	gw = MIN(gw, 130);
	gh = MIN(gh, 130);

	struct RecvV
	{
		float x, y, z;			// clip xyz (pre-divide); the pass-through VS pairs it with pad as w
		float u, v;				// TEXCOORD0: sun-map uv
		float depth, pad;		// TEXCOORD1.x own depth in the map convention, .y clip.w
	};
	static const DWORD kRecvFVF = D3DFVF_XYZ | D3DFVF_TEX2 |
		D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE2(1);

	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: the draw goes through a real vertex shader, so
	// build view*proj on the CPU here (same matrices the FF path used) and pre-divide each vertex.
	Matrix3D view;
	sceneCamera->Get_View_Matrix(&view);
	Matrix4x4 proj;
	sceneCamera->Get_D3D_Projection_Matrix(&proj);
	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: engine composition order is proj*view, exactly
	// like the proven g_sunM = sunProj * sunView for the occluder pass (matrix4.h operator* with
	// Transform_Vector's row-dot application reproduces the device's row-vector v*V*P chain).
	// An initial view*proj draft transformed in the wrong order - the receiver draw then landed
	// entirely off-screen and looked "not darkening".
	Matrix4x4 vpM = proj * Matrix4x4(view);
	const float lift = 0.01f * MAP_XY_FACTOR;

	std::vector<RecvV> verts((size_t)gw * gh);
	const float zSpan = g_sunZFarDist - g_sunZNearDist;
	int n = 0;
	for (Int jj = 0; jj < gh; ++jj)
	{
		Int yCell = MIN(startY + jj * stride, endY);
		for (Int ii = 0; ii < gw; ++ii)
		{
			Int xCell = MIN(startX + ii * stride, endX);
			float wx = (float)xCell * MAP_XY_FACTOR;
			float wy = (float)yCell * MAP_XY_FACTOR;
			float hz = (float)hmap->getHeight(xCell, yCell) * MAP_HEIGHT_SCALE;

			// clip = g_sunM * (x,y,z,1); g_sunM is the GL-convention ortho proj*view.
			float cx = g_sunM[0][0]*wx + g_sunM[0][1]*wy + g_sunM[0][2]*hz + g_sunM[0][3];
			float cy = g_sunM[1][0]*wx + g_sunM[1][1]*wy + g_sunM[1][2]*hz + g_sunM[1][3];
			float cz = g_sunM[2][0]*wx + g_sunM[2][1]*wy + g_sunM[2][2]*hz + g_sunM[2][3];

			RecvV& o = verts[n++];
			// GeneralsX @bugfix PS3.0-P3-21a 14/10/2026: submit TRUE clip coordinates and hand the
			// rasterizer its w.
			//
			// The previous revision divided on the CPU and left pad=0.0f, so the pass-through VS built
			// float4(ndc, 0) - a clip vector with w==0. Every fixed point the rasterizer then computes
			// comes from a division by w, so on the native-d3d9 runtime the whole receiver grid was
			// either discarded or mapped through a degenerate 1/0, which is the third layer of the
			// "#27 receiver" failure: the echo probes proved the sampler worked and the constant-PS probe
			// proved the draw happened, yet nothing darkened, because the primitive never survived
			// clipping. Passing (clip.xyz, w) instead restores the exact hardware behaviour the retail
			// fixed-function path had, and lets vertices behind the near plane be clipped properly rather
			// than folded through the divide.
			float lz = hz + lift;						// same lift the shadow decals use against z-fight
			Vector4 worldV4(wx, wy, lz, 1.0f);
			Vector4 clipCam;
			Matrix4x4::Transform_Vector(vpM, worldV4, &clipCam);
			float ow = clipCam.W;
			if (ow < 0.001f && ow > -0.001f)
				ow = (ow < 0.0f) ? -0.001f : 0.001f;	// degenerate near-plane guard; hardware clips the row
			o.x = clipCam.X;
			o.y = clipCam.Y;
			o.z = clipCam.Z;
			o.u = 0.5f * (cx + 1.0f);					// identical mapping to SunShadowSinkImpl::Submit
			o.v = 0.5f * (1.0f - cy);
			o.depth = 0.5f * cz + 0.5f;
			o.pad = ow;									// consumed as clip.w by kRecvPassthroughVSShader
		}
	}

	std::vector<unsigned short> indices;
	indices.reserve((size_t)(gw - 1) * (gh - 1) * 6);
	for (Int jj = 0; jj + 1 < gh; ++jj)
	{
		for (Int ii = 0; ii + 1 < gw; ++ii)
		{
			unsigned short a = (unsigned short)(jj * gw + ii);
			unsigned short b = (unsigned short)(a + 1);
			unsigned short c = (unsigned short)(a + gw);
			unsigned short d = (unsigned short)(c + 1);
			// CULLMODE_NONE below, so winding only needs to cover both triangles.
			indices.push_back(a); indices.push_back(b); indices.push_back(c);
			indices.push_back(b); indices.push_back(d); indices.push_back(c);
		}
	}
	Int triCount = (Int)(indices.size() / 3);
	if (triCount <= 0)
		return;

	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: the receiver submits through a pass-through vertex
	// shader (see kRecvPassthroughVSShader).  GeneralsX @bugfix PS3.0-P3-21a 14/10/2026: what it submits
	// is now true clip xyz + w rather than a CPU post-divided NDC triple, which is what the shader's
	// float4(pos, md.y) has always expected.  The WORLD/VIEW/PROJECTION sets are kept because multi-view
	// layouts must leave the device holding the primary view this pass was planned for, same contract as
	// the old fixed-function draw.
	Matrix3D world(true);
	DX8Wrapper::Set_Transform(D3DTS_WORLD, world);
	DX8Wrapper::Set_Transform(D3DTS_VIEW, view);
	DX8Wrapper::Set_Transform(D3DTS_PROJECTION, proj);

	dev->SetTexture(0, g_mapTex);
	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: POINT taps for PCF.  The shell-black root cause was
	// the fixed-function-vertex + pixel-shader combination: its PS texture reads return 0 for ANY bound
	// texture on this runtime (P2W0n probes: constant-PS draw fine, map echo and plain-dummy echo both
	// read 0).  The receiver now runs through a pass-through vertex shader; POINT + NONE mip filtering
	// stays because the D3D9 capability model does not guarantee LINEAR for float RT formats and the
	// 3x3 taps at 1.25-texel spread do not need it.
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);	// single-level RT texture; stale LINEAR mip state must not leak in
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

	float bias = 1.5f / zSpan;			// ~1.5 world units of acne guard, in [0,1] depth
	float spread = 1.25f / (float)g_mapSize;
	// GeneralsX @feature 13/10/2026 Options.ini ShadowMapIntensity via the options menu.  GeneralsX
	// @feature Qoder 18/10/2026: the value is no longer latched per process - W3DShaderManager holds it
	// live (env GENERALSX_SHADOWMAP_INT still wins inside the seed), so the row darkens or lightens the
	// shadows on the next frame instead of the next start.
	const float strength = (float)W3DShaderManager::getShadowMapStrength();

	float c0[4] = { spread, spread, bias, strength };
	dev->SetPixelShaderConstantF(0, c0, 1);

	dev->SetVertexShader(g_recvVS);
	dev->SetPixelShader(g_pcfPS);
	dev->SetFVF(kRecvFVF);

	// Multiplicative darkening exactly like _PresetMultiplicativeShader: result = dst * src.rgb.
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
	dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE,
		D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
	dev->SetRenderState(D3DRS_ZENABLE, TRUE);
	dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: the raw DIPUP below uses whatever the DEVICE
	// actually holds, while DX8Wrapper::Set_Transform only defers WORLD/VIEW (dirty flags, applied by
	// the sorting renderer) and writes PROJECTION through - the transforms are set through the wrapper
	// above so the multi-view layouts leave the device holding the primary view this pass planned for.
	const HRESULT drawHr = dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)verts.size(), (UINT)triCount,
		&indices[0], D3DFMT_INDEX16, &verts[0], sizeof(RecvV));

	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: a once-only log hid a later-frame failure; repeat
	// the hr for the first four receiver draws.
	static int s_recvLogged = 0;
	if (s_recvLogged < 4)
	{
		++s_recvLogged;
		fprintf(stderr, "[SMAP] recv#%d grid=%dx%d stride=%d tris=%d hr=0x%08X strength=%.2f halfW=%.1f zSpan=%.1f cells=[%d..%d]x[%d..%d]\n",
			s_recvLogged, gw, gh, stride, triCount, (unsigned)drawHr, (double)strength,
			(double)g_sunHalfWidth, (double)zSpan, startX, endX, startY, endY);
		fflush(stderr);
	}

	dev->SetPixelShader(nullptr);
	dev->SetVertexShader(nullptr);
	dev->SetTexture(0, nullptr);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE,
		D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE | D3DCOLORWRITEENABLE_ALPHA);
	DX8Wrapper::Invalidate_Cached_Render_States();
}

#endif // _WIN32

// ---- public hooks -----------------------------------------------------------------------------

void W3DSunShadowMap::preSceneUpdate(CameraClass *sceneCamera)
{
#ifdef _WIN32
	g_frameActive = false;
	if (!sceneCamera || !sunShadowMapEnabled())
		return;

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev || dev->TestCooperativeLevel() != D3D_OK)
		return;

	// GeneralsX @feature PS3.0-P3-21b 14/10/2026: default the GN reception OFF every frame and re-arm
	// it at the bottom of this function only when the occluder pass actually filled the map - any of
	// the early returns above (feature off, device lost, sun too shallow, no visible terrain box) then
	// leaves meshes on the plain GN shaders rather than sampling a stale map.
	DX8GeomNormal::Clear_Sun_Shadow();
	g_gnArmed = false;   // GeneralsX @feature PS3.0-P3-23: re-armed below only on a fresh push.

	if (!ensureResources(dev))
		return;

	if (!TheW3DShadowManager || !TheTerrainRenderObject)
		return;

	// Sun direction: the shadow manager's light-0 position is a far point along the sun vector.
	Vector3 sunPos = TheW3DShadowManager->getLightPosWorld(0);
	float len2 = sunPos.X*sunPos.X + sunPos.Y*sunPos.Y + sunPos.Z*sunPos.Z;
	if (len2 < 1.0f)
		return;													// no usable sun vector
	g_sunDir.Set(sunPos.X / sqrtf(len2), sunPos.Y / sqrtf(len2), sunPos.Z / sqrtf(len2));
	if (g_sunDir.Z < 0.15f)
	{
		if (!g_shallowSunWarned)
		{
			g_shallowSunWarned = true;
			fprintf(stderr, "[SMAP] sun too low (z=%0.2f), pass suspended\n", (double)g_sunDir.Z);
			fflush(stderr);
		}
		return;
	}

	// Visible terrain window from the scene camera's frustum.
	AABoxClass box;
	const FrustumClass& frustum = sceneCamera->Get_Frustum();
	if (!TheTerrainRenderObject->getMaximumVisibleBox(frustum, &box, TRUE))
		return;

	// Ortho volume = sphere around the visible box; eye pushed beyond it along the sun vector.
	float radius = sqrtf(box.Extent.X*box.Extent.X + box.Extent.Y*box.Extent.Y + box.Extent.Z*box.Extent.Z) * 1.05f + 1.0f;
	g_sunHalfWidth = radius;
	float eyeDist = radius + 600.0f;
	g_sunZNearDist = 10.0f;
	// GeneralsX @bugfix PS3.0-T6-shell 14/10/2026: the receiver grid spans the visible box's X/Y window,
	// but terrain inside that window can sit far above/below the box's Z extent - the shell demo camera
	// produced rebased sun depths up to 1.713 (pxbox), and every PCF compare for those texels failed,
	// multiplying the whole visible terrain dark (menu mean 13/255 vs 77/255 with the pass off).  The
	// depth slab is therefore widened by the map's full height span so grid depths land inside [0,1];
	// the receiver shader guard added in the same change keeps anything still outside fully lit.
	float heightSpan = (WorldHeightMap::getMaxHeightValue() - WorldHeightMap::getMinHeightValue()) * MAP_HEIGHT_SCALE;
	g_sunZFarDist = 2.0f * radius + 600.0f + 100.0f + heightSpan;

	// Texel-snap the center onto the map grid so the projected box does not shimmer frame to
	// frame while the camera and sun move slowly.
	Matrix3D tm;
	tm.Look_At(box.Center + g_sunDir * eyeDist, box.Center, 0.0f);
	Vector3 right = tm.Get_X_Vector();
	Vector3 up = tm.Get_Y_Vector();
	float texelWorld = 2.0f * radius / (float)g_mapSize;
	float dRight = box.Center.X*right.X + box.Center.Y*right.Y + box.Center.Z*right.Z;
	float dUp    = box.Center.X*up.X   + box.Center.Y*up.Y   + box.Center.Z*up.Z;
	float sRight = floorf(dRight / texelWorld + 0.5f) * texelWorld;
	float sUp    = floorf(dUp    / texelWorld + 0.5f) * texelWorld;
	box.Center += right * (sRight - dRight) + up * (sUp - dUp);
	tm.Look_At(box.Center + g_sunDir * eyeDist, box.Center, 0.0f);
	g_sunBox = box;

	g_sunCam->Set_Projection_Type(CameraClass::ORTHO);
	g_sunCam->Set_Transform(tm);
	g_sunCam->Set_View_Plane(Vector2(-g_sunHalfWidth, -g_sunHalfWidth), Vector2(g_sunHalfWidth, g_sunHalfWidth));
	g_sunCam->Set_Clip_Planes(g_sunZNearDist, g_sunZFarDist);

	Matrix4x4 sunProj;
	g_sunCam->Get_Projection_Matrix(&sunProj);
	Matrix3D sunView;
	g_sunCam->Get_View_Matrix(&sunView);
	g_sunM = sunProj * sunView;

	g_statCastors = 0;
	g_statTris = 0;
	g_statSkipped = 0;
	g_sink.Reset_Frame();

	renderOccluderPass(dev);
	g_frameActive = true;

	// GeneralsX @feature PS3.0-P3-21b 14/10/2026: hand the freshly filled map to the GN pass.
	// Same numbers the terrain overlay used: 1.25-texel PCF spread, 1.5 world units of depth bias
	// over the ortho slab, Options.ini ShadowMapIntensity as strength (0 parks the overlay anyway
	// because Apply then keeps the plain GN shaders).
	if (sunShadowGNReception() && g_mapTexWrapper != nullptr && DX8GeomNormal::Sun_Shadow_Ready())
	{
		// GeneralsX @feature Qoder 18/10/2026: same live value as the overlay pass above (no per-process
		// latch), so one slider moves both shadow-map consumers together.
		const float gnIntensity = (float)W3DShaderManager::getShadowMapStrength();
		const float zSpan = g_sunZFarDist - g_sunZNearDist;
		// GeneralsX @feature PS3.0-P3-22 14/10/2026: proportionate softening - the kernel half-width
		// is 1.25 texels for 3x3 and 2.5 for 5x5, and dividing by the map edge keeps the world-space
		// penumbra constant when the resolution tier moves (512 -> 2048).
		const float spreadFactor = (sunShadowPCFTaps() == 5) ? 2.5f : 1.25f;
		DX8GeomNormal::Set_Sun_Shadow(g_mapTexWrapper, g_sunM,
			spreadFactor / (float)g_mapSize, (zSpan > 1.0f) ? 1.5f / zSpan : 0.0f, gnIntensity);
		g_gnArmed = true;         // GeneralsX @feature PS3.0-P3-23: composition queries see reception live
		g_gnStrength = gnIntensity;
		static bool s_gnPushedLogged = false;
		if (!s_gnPushedLogged)
		{
			s_gnPushedLogged = true;
			fprintf(stderr, "[SMAP] GN reception armed (map=%s %d, pcf=%dx%d, spread=%.5f, bias=%.5f, strength=%.2f)\n",
				mapFmtName(g_mapFmt), g_mapSize, sunShadowPCFTaps(), sunShadowPCFTaps(),
				(double)(spreadFactor / (float)g_mapSize),
				(double)((zSpan > 1.0f) ? 1.5f / zSpan : 0.0f), (double)gnIntensity);
			fflush(stderr);
		}
	}
	else if (sunShadowGNReception())
	{
		static bool s_gnMissWarned = false;
		if (!s_gnMissWarned)
		{
			s_gnMissWarned = true;
			fprintf(stderr, "[SMAP] GN reception unavailable (wrapper=%d ready=%d), overlay stays active\n",
				g_mapTexWrapper != nullptr ? 1 : 0, DX8GeomNormal::Sun_Shadow_Ready() ? 1 : 0);
			fflush(stderr);
		}
	}

	if ((++g_frameCounter % 300) == 0)
	{
		fprintf(stderr, "[SMAP] frame %d casters=%d draws=%d skipped=%d tris=%d bbox=%d\n",
			g_frameCounter, g_statCastors, 0, g_statSkipped, g_sink.m_triTotal, g_sink.m_bbValid ? 1 : 0);
		if (g_sink.m_bbValid)
		{
			fprintf(stderr, "[SMAP] pxbox x[%.0f,%.0f] y[%.0f,%.0f] z[%.3f,%.3f] map=%d\n",
				g_sink.m_bbMinX, g_sink.m_bbMaxX, g_sink.m_bbMinY, g_sink.m_bbMaxY,
				g_sink.m_bbMinZ, g_sink.m_bbMaxZ, g_mapSize);
		}
		fflush(stderr);
	}
#endif // _WIN32
}

void W3DSunShadowMap::renderTerrainReceiver(CameraClass *sceneCamera)
{
#ifdef _WIN32
	if (!g_frameActive || !sceneCamera)
		return;
	// GeneralsX @feature PS3.0-P3-21b 14/10/2026: with GN reception armed the overlay would double-darken
	// meshes AND still paint its black grid over the terrain (its texture reads return 0), so park it.
	// GENERALSX_SMAP_GNRECV=0 restores the pre-P3-21b behaviour for A/B forensics.
	if (sunShadowGNReception() && g_mapTexWrapper != nullptr && DX8GeomNormal::Sun_Shadow_Ready())
	{
		static bool s_parkedLogged = false;
		if (!s_parkedLogged)
		{
			s_parkedLogged = true;
			fprintf(stderr, "[SMAP] terrain overlay receiver parked (GN reception active)\n");
			fflush(stderr);
		}
		return;
	}
	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev || dev->TestCooperativeLevel() != D3D_OK)
		return;
	renderTerrainGrid(dev, sceneCamera);
#endif // _WIN32
}

// GeneralsX @feature PS3.0-P3-23 14/10/2026: S4 composition queries (see header).  These read the frame
// flags set in preSceneUpdate so the volumetric screen pass can halve its darkening while GN reception is
// active; on a non-Windows / feature-off build the flags are always false/0 (the whole occluder path lives
// under _WIN32), which is exactly the "no sun map, keep volumetric full-strength" behaviour.
bool W3DSunShadowMap::isReceptionActive()
{
	return g_gnArmed;
}

float W3DSunShadowMap::receptionStrength()
{
	return g_gnStrength;
}

void W3DSunShadowMap::releaseResources()
{
#ifdef _WIN32
	// GeneralsX @feature PS3.0-P3-21b: drop the GN reception state BEFORE the raw texture goes away -
	// the wrapper is a borrowed pointer to DX8GeomNormal.
	DX8GeomNormal::Clear_Sun_Shadow();
	if (g_mapTexWrapper) { REF_PTR_RELEASE(g_mapTexWrapper); }
	if (g_mapSurface) { g_mapSurface->Release(); g_mapSurface = nullptr; }
	if (g_depthSurface) { g_depthSurface->Release(); g_depthSurface = nullptr; }
	if (g_mapTex) { g_mapTex->Release(); g_mapTex = nullptr; }
	if (g_depthPS) { g_depthPS->Release(); g_depthPS = nullptr; }
	if (g_fillPS) { g_fillPS->Release(); g_fillPS = nullptr; }
	if (g_pcfPS) { g_pcfPS->Release(); g_pcfPS = nullptr; }
	if (g_recvVS) { g_recvVS->Release(); g_recvVS = nullptr; }	// GeneralsX @bugfix PS3.0-P3-23: the overlay VS was missing here - it leaks on every device reset otherwise.
	g_resourcesReady = false;
	g_initAttempted = false;
	g_mapSize = 0;
	g_mapFmt = D3DFMT_UNKNOWN;
	g_frameActive = false;
#endif
}

void W3DSunShadowMap::reAcquireResources()
{
	// Nothing to pre-create: ensureResources() rebuilds lazily on the first frame after the
	// device reset cleared everything (releaseResources ran, or TestCooperativeLevel recovered).
}
