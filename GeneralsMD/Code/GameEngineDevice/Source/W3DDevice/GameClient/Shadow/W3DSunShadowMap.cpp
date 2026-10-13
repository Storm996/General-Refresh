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
#include <cmath>
#include <vector>

#ifdef _WIN32
// ---- generated shader programs (runtime HLSL, same boundary as the T2 blur pass) ------------

// Writes the occluder's [0,1] depth, carried in TEXCOORD0.x by the RHW sink vertex, into the red
// channel of the R32F map.
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
static const char* kPcfReceiveShader =
	"Texture2D mapTex : register(t0);\n"
	"SamplerState samp : register(s0);\n"
	"float4 c0 : register(c0);\n"	// strict mode needs the explicit constant declaration (blur shader precedent)
	"float4 main(float2 uv : TEXCOORD0, float2 md : TEXCOORD1) : COLOR\n"
	"{\n"
	"	float shade = 1.0;\n"
	"	if (uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0)\n"
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
#endif // _WIN32

// ---- module state ----------------------------------------------------------------------------

static const int kMapSizeLadder[3] = { 1024, 512, 256 };

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

#ifdef _WIN32

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
	if (!g_depthPS || !g_fillPS || !g_pcfPS)
		return false;

	// Size ladder: keep stepping down until a full RT+depth pair exists.  Matched sizes are
	// mandatory on native d3d9 (mismatch silently drops all primitives - probe Q1 lesson).
	// GeneralsX @feature 13/10/2026 the ladder starts at the user-requested edge (Options.ini
	// SunShadowMapResolution, options-menu dropdown); creation failure still steps down.
	static int s_startTier = -1;
	if (s_startTier < 0)
	{
		int requested = OptionPreferences().getSunShadowMapResolution();
		s_startTier = (requested <= 256) ? 2 : (requested <= 512) ? 1 : 0;
	}
	for (int ladder = s_startTier; ladder < 3; ++ladder)
	{
		int size = kMapSizeLadder[ladder];
		IDirect3DTexture9* tex = nullptr;
		if (FAILED(dev->CreateTexture(size, size, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex)
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
		break;
	}

	if (g_mapSize == 0)
		return false;

	if (!g_sunCam)
		g_sunCam = NEW_REF(CameraClass, ());
	if (!g_sunCam)
		return false;

	g_resourcesReady = true;
	g_initAttempted = true;
	fprintf(stderr, "[SMAP] installed size=%d profile=%s\n", g_mapSize, profile ? profile : "?");
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
		float x, y, z;			// world surface, lifted a hair off the terrain like the decal path
		float u, v;				// TEXCOORD0: sun-map uv
		float depth, pad;		// TEXCOORD1.x: this vertex's own depth in the map convention
	};
	static const DWORD kRecvFVF = D3DFVF_XYZ | D3DFVF_TEX2 |
		D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE2(1);

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
			o.x = wx;
			o.y = wy;
			o.z = hz + 0.01f * MAP_XY_FACTOR;		// same lift the shadow decals use against z-fight
			o.u = 0.5f * (cx + 1.0f);					// identical mapping to SunShadowSinkImpl::Submit
			o.v = 0.5f * (1.0f - cy);
			o.depth = 0.5f * cz + 0.5f;
			o.pad = 0.0f;
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

	// Fixed-function pipeline with the scene camera - the same approach the decal flush uses, but
	// the transforms are set explicitly so multi-view layouts use the primary view this pass was
	// planned for.
	Matrix3D view;
	sceneCamera->Get_View_Matrix(&view);
	Matrix4x4 proj;
	sceneCamera->Get_D3D_Projection_Matrix(&proj);
	Matrix3D world(true);
	DX8Wrapper::Set_Transform(D3DTS_WORLD, world);
	DX8Wrapper::Set_Transform(D3DTS_VIEW, view);
	DX8Wrapper::Set_Transform(D3DTS_PROJECTION, proj);

	dev->SetTexture(0, g_mapTex);
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

	float bias = 1.5f / zSpan;			// ~1.5 world units of acne guard, in [0,1] depth
	float spread = 1.25f / (float)g_mapSize;
	// GeneralsX @feature 13/10/2026 Options.ini ShadowMapIntensity via the options menu (env
	// GENERALSX_SHADOWMAP_INT still overrides inside the getter).  Was a per-frame getenv, now
	// cached once per process - the menu value takes effect at next start.
	static float s_intensityCached = -1.0f;
	if (s_intensityCached < 0.0f)
		s_intensityCached = (float)OptionPreferences().getShadowMapIntensity() * 0.01f;
	float strength = s_intensityCached;
	float c0[4] = { spread, spread, bias, strength };
	dev->SetPixelShaderConstantF(0, c0, 1);

	dev->SetVertexShader(nullptr);
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

	HRESULT drawHr = dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)verts.size(), (UINT)triCount,
		&indices[0], D3DFMT_INDEX16, &verts[0], sizeof(RecvV));

	static bool s_recvLogged = false;
	if (!s_recvLogged)
	{
		s_recvLogged = true;
		fprintf(stderr, "[SMAP] recv grid=%dx%d stride=%d tris=%d hr=0x%08X halfW=%.1f zSpan=%.1f cells=[%d..%d]x[%d..%d]\n",
			gw, gh, stride, triCount, (unsigned)drawHr, (double)g_sunHalfWidth, (double)zSpan,
			startX, endX, startY, endY);
		fflush(stderr);
	}

	dev->SetPixelShader(nullptr);
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
	g_sunZFarDist = 2.0f * radius + 600.0f + 100.0f;

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

	// GeneralsX @bugfix Qoder 13/10/2026 T6 diag: sampling only the first three frames missed every
	// caster - the projected-shadow lists populate well after the loadsave frame.  Re-sample early
	// AND at the same cadence as the stats line so map content and caster counts correlate.
	{
		if (g_frameCounter < 3 || (g_frameCounter % 300) == 299)
		{
			IDirect3DTexture9* staging = nullptr;
			if (SUCCEEDED(dev->CreateTexture(g_mapSize, g_mapSize, 1, 0, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &staging, nullptr)))
			{
				IDirect3DSurface9* stgSurf = nullptr;
				if (SUCCEEDED(staging->GetSurfaceLevel(0, &stgSurf)) &&
					SUCCEEDED(dev->GetRenderTargetData(g_mapSurface, stgSurf)))
				{
					D3DLOCKED_RECT lr;
					if (SUCCEEDED(stgSurf->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
					{
						const float* pixels = (const float*)lr.pBits;
						const int pitchFloats = (int)(lr.Pitch / sizeof(float));
						int below = 0; int total = 0;
						for (int yy = 0; yy < g_mapSize; yy += 16)
						{
							for (int xx = 0; xx < g_mapSize; xx += 16)
							{
								float v = pixels[yy * pitchFloats + xx];
								if (v < 0.99f) ++below;
								++total;
							}
						}
						float center = pixels[(g_mapSize / 2) * pitchFloats + g_mapSize / 2];
						stgSurf->UnlockRect();
						fprintf(stderr, "[SMAP] mapcheck@%d frac<0.99=%.3f center=%.3f casters=%d tris=%d\n",
							g_frameCounter, (float)below / (float)total, center,
							g_statCastors, g_sink.m_triTotal);
						fflush(stderr);
					}
				}
				if (stgSurf) stgSurf->Release();
				staging->Release();
			}
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
	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (!dev || dev->TestCooperativeLevel() != D3D_OK)
		return;
	renderTerrainGrid(dev, sceneCamera);
#endif // _WIN32
}

void W3DSunShadowMap::releaseResources()
{
#ifdef _WIN32
	if (g_mapSurface) { g_mapSurface->Release(); g_mapSurface = nullptr; }
	if (g_depthSurface) { g_depthSurface->Release(); g_depthSurface = nullptr; }
	if (g_mapTex) { g_mapTex->Release(); g_mapTex = nullptr; }
	if (g_depthPS) { g_depthPS->Release(); g_depthPS = nullptr; }
	if (g_fillPS) { g_fillPS->Release(); g_fillPS = nullptr; }
	if (g_pcfPS) { g_pcfPS->Release(); g_pcfPS = nullptr; }
	g_resourcesReady = false;
	g_initAttempted = false;
	g_mapSize = 0;
	g_frameActive = false;
#endif
}

void W3DSunShadowMap::reAcquireResources()
{
	// Nothing to pre-create: ensureResources() rebuilds lazily on the first frame after the
	// device reset cleared everything (releaseResources ran, or TestCooperativeLevel recovered).
}
