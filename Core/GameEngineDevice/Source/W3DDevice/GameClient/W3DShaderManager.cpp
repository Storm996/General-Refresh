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

// FILE: W3DShaderManager.cpp ////////////////////////////////////////////////
//-----------------------------------------------------------------------------
//
//                       Westwood Studios Pacific.
//
//                       Confidential Information
//                Copyright (C) 2001 - All Rights Reserved
//
//-----------------------------------------------------------------------------
//
// Project:   RTS3
//
// File name: W3DShaderManager.cpp
//
// Created:   Mark Wilczynski, August 2001
//
// Desc:      Perform tests on currently selected WW3D/D3D device to determine
//			  which of our rendering features are supported.  The system allows
//			  setting up a few custom shaders that are selected based on video
//			  card features.
//
//			  To add a new shader to the system:
//			  0) Add your shader to the ShaderTypes enum
//			  1) Create shader using W3DShaderInterface
//			  2) Repeat step 1 for any alternate shaders
//			  3) Create list of alternate shaders sorted by order of preference.
//				 The first shader which passes hardware validation will be selected.
//			  4) Add list from step 3) to MasterShaderList[].
//
//-----------------------------------------------------------------------------

#include "WW3D2/dx8wrapper.h"
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.3: mirror the raw CPU-written flat-normal fallback
// into the Vulkan texture store (unconditional: the symbol is a no-op unless the win64vk core is up).
#include "WW3D2/Backend/VulkanTexMirror.h"
#include "WW3D2/Backend/VulkanPgm.h"	// GeneralsX @feature Qoder 18/10/2026 wedge 8: HLSL source -> program tag
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
#include "WW3D2/Backend/VulkanDraw.h"	// GeneralsX @feature Qoder 18/10/2026 step 9.H: VKDraw_Ready gate
#include "WW3D2/Backend/VKD3DStubImpl.h"	// GeneralsX @feature Qoder 19/10/2026 BUG-03: VK_NOD3D_Enabled gate
#endif
#include "WW3D2/formconv.h"	// GeneralsX @feature Qoder 18/10/2026 step 9.H: depth surface -> WW3DZFormat
#include "WWMath/wwmath.h"	// GeneralsX @feature Storm996 17/10/2026 PS3.0 haze coverage: Float_As_Int for FOGSTART/FOGEND
#include "WW3D2/assetmgr.h"
#include "Lib/BaseType.h"
#include "Common/file.h"
#include "Common/FileSystem.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DCustomScene.h"
#include "W3DDevice/GameClient/W3DSmudge.h"
#include "GameClient/View.h"
#include "GameClient/CommandXlat.h"
#include "GameClient/Display.h"
#include "GameClient/Water.h"
#include "GameClient/WeatherFX.h"	// GeneralsX @feature PS3.0 Weather W2 16/10/2026 screen-layer scalars
#include "GameLogic/GameLogic.h"
#include "Common/GlobalData.h"
#include "Common/GameLOD.h"
#include "Common/OptionPreferences.h"	// GeneralsX @feature 13/10/2026 lighting-knob getters
#include "d3dx9tex.h"
#include "WW3D2/dx8caps.h"
// GeneralsX @feature T3-0 M3/M4: dot3 runtime assembly + startup diagnostics + WW3D2-side mount module
#include "d3dx9core.h"
#include "WW3D2/dx8normalmap.h"


// Turn this on to turn off pixel shaders. jba[4/3/2003]
#define do_not_DISABLE_PIXEL_SHADERS 1

/** Interface definition for custom shaders we define in our app.  These shaders can perform more complex
	operations than those allowed in the WW3D2 shader system.
*/
class W3DShaderInterface
{
public:
	Int getNumPasses() {return m_numPasses;};	///<return number of passes needed for this shader
	virtual Int set(Int pass) {return TRUE;};		///<setup shader for the specified rendering pass.
	 ///do any custom resetting necessary to bring W3D in sync.
	virtual void reset() {
		ShaderClass::Invalidate();
		DX8Wrapper::Set_DX8_Texture(0, nullptr);
		DX8Wrapper::Set_DX8_Texture(1, nullptr);};
	virtual Int init() = 0;			///<perform any one time initialization and validation
	virtual Int shutdown() { return TRUE;};			///<release resources used by shader
protected:
	Int m_numPasses;						///<number of passes to complete shader
};

//this table will contain custom versions of each shader tuned for specific video card and user options.
static W3DFilterInterface *W3DFilters[FT_MAX];
static W3DShaderInterface *W3DShaders[W3DShaderManager::ST_MAX];
static Int W3DShadersPassCount[W3DShaderManager::ST_MAX];	//number of passes for each of the above shaders
// GeneralsX @feature PS3.0 batch7 13/10/2026 the stage table has to cover every sampler the wrapper can
// bind, otherwise setTexture(8..15) from a ps_3_0 shader writes past the array.
static_assert(SHADER_TEXTURE_STAGES == MAX_TEXTURE_STAGES,
	"W3DShaderManager stage table must match the DX8Wrapper sampler cache size");
TextureClass *W3DShaderManager::m_Textures[SHADER_TEXTURE_STAGES];
W3DShaderManager::ShaderTypes W3DShaderManager::m_currentShader;
FilterTypes W3DShaderManager::m_currentFilter=FT_NULL_FILTER; ///< Last filter that was set.
Int W3DShaderManager::m_currentShaderPass;
ChipsetType W3DShaderManager::m_currentChipset;
GraphicsVenderID W3DShaderManager::m_currentVendor;
// GeneralsX @bugfix BenderAI 13/02/2026 Int64 for cross-platform (fighter19 pattern)
Int64 W3DShaderManager::m_driverVersion;

Bool W3DShaderManager::m_renderingToTexture = false;
IDirect3DSurface9 *W3DShaderManager::m_oldRenderSurface=nullptr;	///<previous render target
IDirect3DTexture9 *W3DShaderManager::m_renderTexture=nullptr;		///<texture into which rendering will be redirected.
IDirect3DSurface9 *W3DShaderManager::m_newRenderSurface=nullptr;	///<surface the filters sample from
IDirect3DSurface9 *W3DShaderManager::m_oldDepthSurface=nullptr;	///<previous depth buffer surface
// GeneralsX @bugfix Storm996 12/10/2026 offscreen target pair bound while rendering to texture
IDirect3DSurface9 *W3DShaderManager::m_rttColorSurface=nullptr;
IDirect3DSurface9 *W3DShaderManager::m_rttDepthSurface=nullptr;
// GeneralsX @tweak PS3.0-P2-3 14/10/2026 S6: consecutive RTT bind-failure counter (retry before fuse).
Int W3DShaderManager::m_rttBindFailures=0;
// GeneralsX @feature Qoder 18/10/2026 step 9.H: the offscreen pair only becomes a recorder target when
// both keys were mirrored and opted in; until then the filters keep the D3D9 redirect.
Bool W3DShaderManager::m_vkRttReady=false;
Bool W3DShaderManager::m_vkRttPublished=false;
/*===========================================================================================*/
/*=========      Screen Shaders	=============================================================*/
/*===========================================================================================*/

/*=========  ScreenBloomFilter	=============================================================*/
///applies a bloom (glow) post-processing effect to the tactical viewport.
///The bright pass is extracted with a fixed-function threshold subtract into a
///quarter-resolution render target; drawing it back additively with linear
///filtering produces a cheap but convincing glow halo. No pixel shaders are
///required so this works on every DX8-compatible backend (including DXVK).
class ScreenBloomFilter : public W3DFilterInterface
{
public:
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Int shutdown() override;		///<release resources used by shader
	virtual Bool preRender(Bool &skipRender, CustomScenePassModes &scenePassMode) override; ///< Set up at start of render.  Only applies to screen filter shaders.
	virtual Bool postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender) override; ///< Called after render.  Only applies to screen filter shaders.
	virtual Bool setup(FilterModes mode) override {return true;} ///< Called when the filter is started, one time before the first prerender.
	// GeneralsX @feature 05/10/2026 Global toggle, kept out of GlobalData so it never touches CRC/replay state.
	static Bool IsEnabled() { return s_enabled; }
	static void SetEnabled(Bool enabled) { s_enabled = enabled; }
protected:
	virtual Int set(FilterModes mode) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	enum { BLOOM_TEX_SIZE = 256 };	///<quarter-resolution bright pass buffer (power of two).
	// GeneralsX @feature PS3.0 T4 13/10/2026 programmable bloom chain sizes.
	enum { BLOOM_MID_SIZE = 128, BLOOM_SMALL_SIZE = 64 };
	static Bool s_enabled;			///<global bloom on/off switch (not part of game state/CRC).
	IDirect3DTexture9 *m_bloomTexture;	///<quarter-resolution buffer holding the bright pass.
	IDirect3DSurface9 *m_bloomSurface;	///<render target surface of m_bloomTexture.
	// GeneralsX @feature PS3.0 T4 13/10/2026 HLSL chain: soft-knee brightpass, 9-tap tent downsample
	// (used 256->128 and 128->64), additive composite with an intensity constant.  When any part of
	// this fails to compile the filter keeps the fixed-function passes above - bloom never disappears
	// over a shader bug.
	Bool m_hlsl;						///<TRUE when all three HLSL passes compiled and the extra RTs exist.
	IDirect3DPixelShader9 *m_brightPS;	///<soft-threshold brightpass program.
	IDirect3DPixelShader9 *m_blurPS;	///<9-tap tent downsample program (one program, two calls).
	IDirect3DPixelShader9 *m_compositePS;///<intensity-scaled additive composite program.
	// GeneralsX @feature PS3.0-P3-24 14/10/2026 windowed render-time gamma copy program.
	// Optional: a compile failure leaves it null and the fixed-function scene copy stays, so
	// this can never take the frame down.
	IDirect3DPixelShader9 *m_gammaCopyPS;///<scene copy with a pow(curve) for the windowed gamma slider.
	IDirect3DTexture9 *m_midTexture;	///<128px first blur level.
	IDirect3DSurface9 *m_midSurface;
	IDirect3DTexture9 *m_smallTexture;	///<64px second blur level - the one composited back.
	IDirect3DSurface9 *m_smallSurface;
	Bool initHlslChain();				///<compile programs + create the two blur levels.
	void releaseHlslChain();			///<release everything initHlslChain owns (idempotent).
};

ScreenBloomFilter screenBloomFilter;

Bool ScreenBloomFilter::s_enabled = true;	// GeneralsX @feature 05/10/2026 Prototype default: enabled for evaluation.

///List of different bloom implementations in order of preference
W3DFilterInterface *ScreenBloomFilterList[]=
{
	&screenBloomFilter,
	nullptr
};

// GeneralsX @feature 05/10/2026 Screen-space quad used by the bloom filter passes.
struct BloomQuadVertex
{
	D3DXVECTOR4 p;
	DWORD color;   // diffuse color
	float	u;
	float	v;
};

// GeneralsX @feature 05/10/2026 Fill a screen-space quad covering the given rect/UV box.
static void BuildBloomQuad(BloomQuadVertex *v, Real x0, Real y0, Real x1, Real y1,
	Real u0, Real v0, Real u1, Real v1, DWORD color)
{
	//bottom right
	v[0].p = D3DXVECTOR4( x1-0.5f, y1-0.5f, 0.0f, 1.0f );
	v[0].u = u1;	v[0].v = v1;
	//top right
	v[1].p = D3DXVECTOR4( x1-0.5f, y0-0.5f, 0.0f, 1.0f );
	v[1].u = u1;	v[1].v = v0;
	//bottom left
	v[2].p = D3DXVECTOR4( x0-0.5f, y1-0.5f, 0.0f, 1.0f );
	v[2].u = u0;	v[2].v = v1;
	//top left
	v[3].p = D3DXVECTOR4( x0-0.5f, y0-0.5f, 0.0f, 1.0f );
	v[3].u = u0;	v[3].v = v0;
	v[0].color = v[1].color = v[2].color = v[3].color = color;
}

class ScreenDefaultFilter : public W3DFilterInterface
{
public:
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Bool preRender(Bool &skipRender, CustomScenePassModes &scenePassMode) override; ///< Set up at start of render.  Only applies to screen filter shaders.
	virtual Bool postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender) override; ///< Called after render.  Only applies to screen filter shaders.
	virtual Bool setup(FilterModes mode) override {return true;} ///< Called when the filter is started, one time before the first prerender.
protected:
	virtual Int set(FilterModes mode) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
};

ScreenDefaultFilter screenDefaultFilter;

///Default filter that just renders screen to off-screen texture and then copies it the the screen.
///Useful because we added some full-time unit effects (microwave tank smudge) to Generals MD that need access
///to the background as a texture.  This filter makes that texture always available for these effects.
W3DFilterInterface *ScreenDefaultFilterList[]=
{
	&screenDefaultFilter,
	nullptr
};

Int ScreenDefaultFilter::init()
{
	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return FALSE;
	}

	//Can render to texture, but we don't know if it can read and write to the same texture.
	//Since there is no D3D caps bit to tell you this, we will just hard-code some specific
	//cards that we know should work.

	Int res;

	if ((res=W3DShaderManager::getChipset()) != DC_UNKNOWN)
	{
		if ( res >=	DC_GEFORCE2)
		{
			//Check if their driver is newer than what we tested for this vendor
/*			if (TheGameLODManager)
			{
				if (TheGameLODManager->getTestedDriverVersion(W3DShaderManager::getCurrentVendor()) < W3DShaderManager::getCurrentDriverVersion())
					return FALSE;
			}*/
		}
	}

	W3DFilters[FT_VIEW_DEFAULT]=&screenDefaultFilter;

	return TRUE;
}

Bool ScreenDefaultFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	// TheSuperHackers @bugfix Disable Render To Texture redirection for the default filter
	// When MSAA is forced by Nvidia driver profile depth buffer is multisampled internally.
	// Rendering to non-MSAA texture with this depth buffer corrupts depth testing producing black screen
	// The smudge system has its own Copy path that works without Render To Texture.
	return FALSE;
}

Bool ScreenDefaultFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	IDirect3DTexture9 * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex) return false;
	if (!set(mode)) return false;

	struct _TRANS_LIT_TEX_VERTEX {
		D3DXVECTOR4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	DX8Wrapper::Set_DX8_Texture(0,tex);	//previously rendered frame inside this texture
	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = D3DXVECTOR4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = D3DXVECTOR4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = D3DXVECTOR4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = D3DXVECTOR4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[0].color = 0xffffffff;
	v[1].color = 0xffffffff;
	v[2].color = 0xffffffff;
	v[3].color = 0xffffffff;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANS_LIT_TEX_VERTEX));

	reset();
	return true;
}

Int ScreenDefaultFilter::set(FilterModes mode)
{
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
	DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
	DX8Wrapper::Set_Texture(0,nullptr);
	DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_ALWAYS);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,FALSE);
	DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

	return true;
}

void ScreenDefaultFilter::reset()
{
	DX8Wrapper::Set_DX8_Texture(0,nullptr);	//previously rendered frame inside this texture
	DX8Wrapper::Invalidate_Cached_Render_States();
}

/*=========  ScreenBloomFilter impl	=====================================================*/

Int ScreenBloomFilter::init()
{
	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return FALSE;
	}

	// Device reset may re-enter init; drop stale resources first.
	SAFE_RELEASE(m_bloomSurface);
	SAFE_RELEASE(m_bloomTexture);

	LPDIRECT3DDEVICE9 pDev=DX8Wrapper::_Get_D3D_Device8();
	HRESULT hr = pDev->CreateTexture(BLOOM_TEX_SIZE, BLOOM_TEX_SIZE, 1,
		D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_bloomTexture, nullptr);
	if (FAILED(hr) || m_bloomTexture == nullptr)
		return FALSE;

	hr = m_bloomTexture->GetSurfaceLevel(0, &m_bloomSurface);
	if (FAILED(hr))
		return FALSE;

	// GeneralsX @feature PS3.0 T4 13/10/2026 try the programmable chain; the fixed-function
	// passes in postRender stay as the fallback whenever this fails.
	initHlslChain();

	W3DFilters[FT_VIEW_BLOOM_FILTER]=&screenBloomFilter;
	return TRUE;
}

Int ScreenBloomFilter::shutdown()
{
	releaseHlslChain();
	SAFE_RELEASE(m_bloomSurface);
	SAFE_RELEASE(m_bloomTexture);
	return TRUE;
}

// GeneralsX @feature PS3.0 T4 13/10/2026 programmable bloom chain sources.  The fixed-function
// passes reproduce these with a hard SUBTRACT threshold, no blur of its own (the halo came from
// bilinear upscaling of the quarter-res buffer alone) and a vertex-diffuse intensity scale.
// Here the threshold gets a soft knee so lights fade into the glow instead of popping, two
// tent-blurred downsample levels give a real wide halo, and the composite intensity is a float
// constant instead of a quantised colour byte.
static const char* kBloomBrightShader =
	"sampler s0 : register(s0);\n"
	"float4 c0 : register(c0);\n"	//x=threshold, y=knee width
	"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
	"{\n"
	"    float3 c = tex2D(s0, uv).rgb;\n"
	"    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));\n"
	"    float soft = saturate((lum - c0.x) / max(c0.y, 0.0001));\n"	//linear knee: 0 below threshold, 1 past threshold+knee
	"    return float4(c * soft, 1.0);\n"
	"}\n";

static const char* kBloomBlurShader =
	"sampler s0 : register(s0);\n"
	"float4 c0 : register(c0);\n"	//xy=tent spread in source uv units
	"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
	"{\n"
	"    float3 sum = float3(0.0, 0.0, 0.0);\n"
	"    sum += tex2D(s0, uv + float2(-c0.x, -c0.y)).rgb;\n"
	"    sum += tex2D(s0, uv + float2( 0.0,  -c0.y)).rgb;\n"
	"    sum += tex2D(s0, uv + float2( c0.x,  -c0.y)).rgb;\n"
	"    sum += tex2D(s0, uv + float2(-c0.x,  0.0 )).rgb;\n"
	"    sum += tex2D(s0, uv).rgb;\n"
	"    sum += tex2D(s0, uv + float2( c0.x,  0.0 )).rgb;\n"
	"    sum += tex2D(s0, uv + float2(-c0.x,  c0.y)).rgb;\n"
	"    sum += tex2D(s0, uv + float2( 0.0,   c0.y)).rgb;\n"
	"    sum += tex2D(s0, uv + float2( c0.x,  c0.y)).rgb;\n"
	"    return float4(sum * (1.0 / 9.0), 1.0);\n"
	"}\n";

static const char* kBloomCompositeShader =
	"sampler s0 : register(s0);\n"
	"float4 c0 : register(c0);\n"	//x=intensity
	"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
	"{\n"
	"    return float4(tex2D(s0, uv).rgb * c0.x, 0.0);\n"	//device adds it onto the scene (ONE,ONE)
	"}\n";

// GeneralsX @feature PS3.0-P3-24 14/10/2026 windowed render-time gamma.  Same curve equation as
// DX8Wrapper::Set_Gamma's ramp build (out = pow(in, 1/gamma), bright/contrast unused because the
// slider only drives gamma), applied while copying the scene instead of through the device ramp.
static const char* kBloomGammaCopyShader =
	"sampler s0 : register(s0);\n"
	"float4 c0 : register(c0);\n"	//x=1/gamma
	"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
	"{\n"
	"    float3 c = max(tex2D(s0, uv).rgb, 0.0);\n"
	"    return float4(pow(c, c0.x), 0.0);\n"	//alpha 0 matches the FF copy (ALPHAOP DISABLE)
	"}\n";

// Builds the three programs and the two extra blur levels.  Anything failing simply leaves
// m_hlsl FALSE and the filter keeps its fixed-function passes, so this can never take bloom -
// or the frame - down.  Windows-only like the other inline-HLSL users: the runtime D3DCompile
// shim is the only compiler in the shipped build.
Bool ScreenBloomFilter::initHlslChain()
{
	releaseHlslChain();
#ifdef _WIN32
	if (getenv("GENERALSX_BLOOM3") && atoi(getenv("GENERALSX_BLOOM3")) == 0)
	{
		fprintf(stderr, "[BLOOM3] disabled by GENERALSX_BLOOM3=0, fixed-function chain stays\n");
		fflush(stderr);
		return FALSE;
	}

	const char* profile = nullptr;
	void* handle = nullptr;
	if (FAILED(W3DShaderManager::compileShaderAtBestProfile(kBloomBrightShader, false, "[BLOOM3]", &profile, &handle)) || handle == nullptr)
		return FALSE;
	m_brightPS = (IDirect3DPixelShader9*)handle;

	handle = nullptr;
	if (FAILED(W3DShaderManager::compileShaderAtBestProfile(kBloomBlurShader, false, "[BLOOM3]", &profile, &handle)) || handle == nullptr)
	{
		releaseHlslChain();
		return FALSE;
	}
	m_blurPS = (IDirect3DPixelShader9*)handle;

	handle = nullptr;
	if (FAILED(W3DShaderManager::compileShaderAtBestProfile(kBloomCompositeShader, false, "[BLOOM3]", &profile, &handle)) || handle == nullptr)
	{
		releaseHlslChain();
		return FALSE;
	}
	m_compositePS = (IDirect3DPixelShader9*)handle;

	// GeneralsX @feature PS3.0-P3-24 14/10/2026 optional windowed gamma copy program: a failure
	// here only leaves the gamma slider's windowed path dead (retail behavior), the chain above
	// already works, so do not release or bail.
	handle = nullptr;
	if (SUCCEEDED(W3DShaderManager::compileShaderAtBestProfile(kBloomGammaCopyShader, false, "[WINGAMMA]", &profile, &handle)) && handle != nullptr)
		m_gammaCopyPS = (IDirect3DPixelShader9*)handle;
	else
	{
		fprintf(stderr, "[WINGAMMA] gamma copy compile failed, windowed gamma stays retail-dead\n");
		fflush(stderr);
	}

	LPDIRECT3DDEVICE9 pDev = DX8Wrapper::_Get_D3D_Device8();
	if (FAILED(pDev->CreateTexture(BLOOM_MID_SIZE, BLOOM_MID_SIZE, 1, D3DUSAGE_RENDERTARGET,
			D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_midTexture, nullptr)) || m_midTexture == nullptr ||
		FAILED(m_midTexture->GetSurfaceLevel(0, &m_midSurface)) ||
		FAILED(pDev->CreateTexture(BLOOM_SMALL_SIZE, BLOOM_SMALL_SIZE, 1, D3DUSAGE_RENDERTARGET,
			D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_smallTexture, nullptr)) || m_smallTexture == nullptr ||
		FAILED(m_smallTexture->GetSurfaceLevel(0, &m_smallSurface)))
	{
		releaseHlslChain();
		return FALSE;
	}

	m_hlsl = TRUE;
	fprintf(stderr, "[BLOOM3] programmable chain installed profile=%s levels=%d/%d/%d\n",
		profile ? profile : "?", BLOOM_TEX_SIZE, BLOOM_MID_SIZE, BLOOM_SMALL_SIZE);
	fflush(stderr);
	return TRUE;
#else
	return FALSE;
#endif
}

void ScreenBloomFilter::releaseHlslChain()
{
	if (m_brightPS) { m_brightPS->Release(); m_brightPS = nullptr; }
	if (m_blurPS) { m_blurPS->Release(); m_blurPS = nullptr; }
	if (m_compositePS) { m_compositePS->Release(); m_compositePS = nullptr; }
	if (m_gammaCopyPS) { m_gammaCopyPS->Release(); m_gammaCopyPS = nullptr; }	// GeneralsX @feature PS3.0-P3-24 14/10/2026
	SAFE_RELEASE(m_midSurface);
	SAFE_RELEASE(m_midTexture);
	SAFE_RELEASE(m_smallSurface);
	SAFE_RELEASE(m_smallTexture);
	m_hlsl = FALSE;
}

Bool ScreenBloomFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	skipRender = false;
	W3DShaderManager::startRenderToTexture();
	// GeneralsX @bugfix 05/10/2026 The scene texture is never cleared by the render loop
	// (WW3D::Render passes clear=false and startRenderToTexture only clears for the soft
	// water edge path). Translucent scene elements (cloud shadows, shadow decals, particles)
	// would otherwise accumulate over previous frames and progressively darken the screen.
	if (W3DShaderManager::isRenderingToTexture())
		DX8Wrapper::Clear(true, false, Vector3( 0.0f, 0.0f, 0.0f ), 1.0f);
	return true;
}

Int ScreenBloomFilter::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{
		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		DX8Wrapper::Set_Material(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
		DX8Wrapper::Set_Texture(0,nullptr);
		DX8Wrapper::Set_Texture(1,nullptr);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_ALWAYS);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,FALSE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE,FALSE);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		return true;
	}
	return false;
}

void ScreenBloomFilter::reset()
{
	DX8Wrapper::Set_DX8_Texture(0,nullptr);	//release the bright pass texture from stage 0.
	// GeneralsX @bugfix 05/10/2026 Restore the blend state left enabled by the additive pass
	// so the following UI and next-frame rendering start from a clean state.
	DX8Wrapper::Set_DX8_Render_State( D3DRS_ALPHABLENDENABLE, FALSE );
	DX8Wrapper::Invalidate_Cached_Render_States();
}

namespace {
// GeneralsX @bugfix Qoder 18/10/2026 pure-Vulkan wedge 9: a filter whose offscreen pair was not
// published still records its scene into the scene mirror while the D3D9 target stays empty. Its first
// post quad is a fixed-function copy of that empty target, and the facade accepts it because the texture
// has a Vulkan view - the copy then replaced the whole tactical view with black while the GUI, drawn
// after the filter, stayed lit (bloom is the shipping case). Hold the facade off for the entire
// un-migrated post chain; the scene target is published again on scope exit so the GUI and the next
// frame record normally. Remove the guard for a family once its targets are published (task #33).
class ScopedD3DOnlyPostPass
{
public:
	ScopedD3DOnlyPostPass() { DX8Wrapper::VK_Suspend_Scene_Target(); }
	~ScopedD3DOnlyPostPass() { DX8Wrapper::VK_Publish_Scene_Target(); }
private:
	ScopedD3DOnlyPostPass(const ScopedD3DOnlyPostPass&);
	ScopedD3DOnlyPostPass& operator=(const ScopedD3DOnlyPostPass&);
};
}

Bool ScreenBloomFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	IDirect3DTexture9 * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex) return false;
	if (!set(mode)) return false;

	LPDIRECT3DDEVICE9 pDev=DX8Wrapper::_Get_D3D_Device8();

	BloomQuadVertex v[4];

	Int xpos, ypos, width, height;

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();
	Real dispW=(Real)TheDisplay->getWidth();
	Real dispH=(Real)TheDisplay->getHeight();

	// UV rectangle of the tactical view inside the full-screen scene texture.
	const Real u0=(Real)(xpos)/dispW;			const Real v0=(Real)(ypos)/dispH;
	const Real u1=(Real)(xpos+width)/dispW;		const Real v1=(Real)(ypos+height)/dispH;


	// Pass 0: copy the scene texture straight to the back buffer.
	// GeneralsX @feature PS3.0-P3-24 14/10/2026 windowed render-time gamma.  Retail kills the
	// gamma slider in windowed mode (W3DDisplay::setGamma early-returns so the desktop ramp is
	// left alone).  This filter already owns the scene-to-backbuffer copy, so the pow curve can
	// run there instead: windowed + programmable chain + slider actually moved + env not off.
	// Fullscreen keeps the retail device ramp; every other case keeps the fixed-function copy
	// below byte-for-byte retail.
	Bool doWindowGamma = FALSE;
	float invGamma = 1.0f;
	if (m_hlsl && m_gammaCopyPS != nullptr && TheDisplay != nullptr && TheDisplay->getWindowed() &&
		TheGlobalData != nullptr)
	{
		static int s_wgEnv = -1;
		if (s_wgEnv < 0)
		{
			const char* ev = getenv("GENERALSX_WINDOWGAMMA");
			s_wgEnv = (ev != nullptr && atoi(ev) == 0) ? 0 : 1;
		}
		if (s_wgEnv == 1)
		{
			Real g = TheGlobalData->m_displayGamma;
			if (g < 0.6f) g = 0.6f;		// same clamps DX8Wrapper::Set_Gamma applies to the ramp
			if (g > 6.0f) g = 6.0f;
			if (g < 0.999f || g > 1.001f)
			{
				doWindowGamma = TRUE;
				invGamma = 1.0f / g;
			}
		}
	}
	if (doWindowGamma)
	{
		DX8Wrapper::Set_Vertex_Shader(0);
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_gammaCopyPS));
		float gammaConst[4] = { invGamma, 0.0f, 0.0f, 0.0f };
		DX8Wrapper::Set_Pixel_Shader_Constant(0, gammaConst, 1);
		DX8Wrapper::Set_DX8_Texture(0, tex);
		DX8Wrapper::Set_DX8_Render_State( D3DRS_ALPHABLENDENABLE, FALSE );
		BuildBloomQuad(v, (Real)xpos, (Real)ypos, (Real)(xpos+width), (Real)(ypos+height), u0, v0, u1, v1, 0xffffffff);
		DX8Wrapper::Apply_Render_State_Changes();	//flush cached states before drawing directly through the device.
		DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BloomQuadVertex));
		//D18 contract: unbind before the fixed-function stages below expect the default state.
		DX8Wrapper::Set_Pixel_Shader(0);
		DX8Wrapper::Set_DX8_Texture(0, nullptr);
	}
	else
	{
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Render_State( D3DRS_ALPHABLENDENABLE, FALSE );
	DX8Wrapper::Set_DX8_Texture(0, tex);
	BuildBloomQuad(v, (Real)xpos, (Real)ypos, (Real)(xpos+width), (Real)(ypos+height), u0, v0, u1, v1, 0xffffffff);
	DX8Wrapper::Apply_Render_State_Changes();	//flush cached states before drawing directly through the device.
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BloomQuadVertex));
	}	// GeneralsX @feature PS3.0-P3-24 14/10/2026 closes the fixed-function scene-copy branch

	// Pass 1: extract bright areas into the quarter-resolution buffer.
	IDirect3DSurface9 *backSurface=nullptr;
	IDirect3DSurface9 *depthSurface=nullptr;
	pDev->GetRenderTarget(0, &backSurface);
	pDev->GetDepthStencilSurface(&depthSurface);
	// GeneralsX @bugfix 05/10/2026 Keep the depth buffer bound while switching render
	// targets. Replacing it with null here would disable the next frame's clearz and
	// leave every following scene render with a stale depth buffer.
	if (depthSurface == nullptr)
	{
		// Degraded path: pass 0 already copied the scene, just bail out before
		// touching the render target again.
		SAFE_RELEASE(backSurface);
		reset();
		return true;
	}
	pDev->SetRenderTarget(0, m_bloomSurface);
	pDev->SetDepthStencilSurface(depthSurface);

	if (m_hlsl)
	{
		// GeneralsX @feature PS3.0 T4 13/10/2026 programmable chain: soft-knee brightpass ->
		// 256->128 tent -> 128->64 tent -> additive composite of the 64px halo.  Same screen-space
		// quads as the fixed-function path; only the stage state lines are replaced by PS constants.
		// GeneralsX @feature 13/10/2026 Options.ini BloomIntensity via the options menu; env
		// GENERALSX_BLOOM_INTENSITY remains the QA override inside the getter, default stays 0.35
		// (retail byte-quantised scale was 0x33/255 ~ 0.2).  GeneralsX @feature Qoder 18/10/2026: read
		// live instead of latched, so the page's row takes effect on the next drawn frame.
		const Real bloomIntensity = W3DShaderManager::getBloomIntensity();

		// Bright pass: scene texture -> quarter-res buffer through the knee.
		float thr[4] = { 0.45f, 0.25f, 0.0f, 0.0f };
		DX8Wrapper::Set_Vertex_Shader(0);
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_brightPS));
		DX8Wrapper::Set_Pixel_Shader_Constant(0, thr, 1);
		DX8Wrapper::Set_DX8_Texture(0, tex);
		DX8Wrapper::Set_DX8_Render_State( D3DRS_ALPHABLENDENABLE, FALSE );
		BuildBloomQuad(v, u0*BLOOM_TEX_SIZE, v0*BLOOM_TEX_SIZE, u1*BLOOM_TEX_SIZE, v1*BLOOM_TEX_SIZE, u0, v0, u1, v1, 0xffffffff);
		DX8Wrapper::Apply_Render_State_Changes();	//flush cached states before drawing directly through the device.
		DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
		DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BloomQuadVertex));

		// First blur level: 256 -> 128, tent spread of two source texels.
		pDev->SetRenderTarget(0, m_midSurface);
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_blurPS));
		float spread[4] = { 2.0f / BLOOM_TEX_SIZE, 2.0f / BLOOM_TEX_SIZE, 0.0f, 0.0f };
		DX8Wrapper::Set_Pixel_Shader_Constant(0, spread, 1);
		DX8Wrapper::Set_DX8_Texture(0, m_bloomTexture);
		BuildBloomQuad(v, 0.0f, 0.0f, (Real)BLOOM_MID_SIZE, (Real)BLOOM_MID_SIZE, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffff);
		DX8Wrapper::Apply_Render_State_Changes();
		DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BloomQuadVertex));

		// Second blur level: 128 -> 64, same program, wider spread in source uv units.
		pDev->SetRenderTarget(0, m_smallSurface);
		spread[0] = 2.0f / BLOOM_MID_SIZE;
		spread[1] = 2.0f / BLOOM_MID_SIZE;
		DX8Wrapper::Set_Pixel_Shader_Constant(0, spread, 1);
		DX8Wrapper::Set_DX8_Texture(0, m_midTexture);
		BuildBloomQuad(v, 0.0f, 0.0f, (Real)BLOOM_SMALL_SIZE, (Real)BLOOM_SMALL_SIZE, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffff);
		DX8Wrapper::Apply_Render_State_Changes();
		DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BloomQuadVertex));

		// Composite: 64px halo upscaled over the tactical view, scaled by the float intensity.
		pDev->SetRenderTarget(0, backSurface);
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_compositePS));
		float inten[4] = { bloomIntensity, 0.0f, 0.0f, 0.0f };
		DX8Wrapper::Set_Pixel_Shader_Constant(0, inten, 1);
		DX8Wrapper::Set_DX8_Texture(0, m_smallTexture);
		DX8Wrapper::Set_DX8_Render_State( D3DRS_ALPHABLENDENABLE, TRUE );
		DX8Wrapper::Set_DX8_Render_State( D3DRS_SRCBLEND, D3DBLEND_ONE );
		DX8Wrapper::Set_DX8_Render_State( D3DRS_DESTBLEND, D3DBLEND_ONE );
		BuildBloomQuad(v, (Real)xpos, (Real)ypos, (Real)(xpos+width), (Real)(ypos+height), u0, v0, u1, v1, 0xffffffff);
		DX8Wrapper::Apply_Render_State_Changes();
		DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BloomQuadVertex));

		//D18 contract: whoever bound the shader unbinds it before the fixed-function next pass.
		DX8Wrapper::Set_Pixel_Shader(0);
		DX8Wrapper::Set_Vertex_Shader(0);
		backSurface->Release();
		SAFE_RELEASE(depthSurface);
		reset();
		return true;
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SUBTRACT );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_TFACTOR );
	DX8Wrapper::Set_DX8_Render_State( D3DRS_TEXTUREFACTOR, 0x007f7f7f );	// ~50% luminance threshold.
	BuildBloomQuad(v, u0*BLOOM_TEX_SIZE, v0*BLOOM_TEX_SIZE, u1*BLOOM_TEX_SIZE, v1*BLOOM_TEX_SIZE, u0, v0, u1, v1, 0xffffffff);
	DX8Wrapper::Apply_Render_State_Changes();	//flush cached states before drawing directly through the device.
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BloomQuadVertex));

	// Pass 2: add the (implicitly blurred) bright pass back on top of the scene.
	pDev->SetRenderTarget(0, backSurface);
	pDev->SetDepthStencilSurface(depthSurface);
	backSurface->Release();
	SAFE_RELEASE(depthSurface);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Render_State( D3DRS_ALPHABLENDENABLE, TRUE );
	DX8Wrapper::Set_DX8_Render_State( D3DRS_SRCBLEND, D3DBLEND_ONE );
	DX8Wrapper::Set_DX8_Render_State( D3DRS_DESTBLEND, D3DBLEND_ONE );
	DX8Wrapper::Set_DX8_Texture(0, m_bloomTexture);
	// GeneralsX @feature PS3.0 Qoder 17/10/2026: the fixed-function fallback used to hard-code the
	// 0x33 (~20%) retail glow byte, so the BloomIntensity row was silently ignored on any host that
	// did not get the programmable chain. The same live getter now feeds both legs; on the shipping
	// (HLSL) path the value is unchanged, and on this one the player's slider actually moves the glow.
	// Quantised to one byte because a fixed-function vertex diffuse has no float channel.
	const Int glowByte = (Int)(W3DShaderManager::getBloomIntensity() * 255.0f + 0.5f);
	const UnsignedInt glowColor = (glowByte < 1) ? 0x00000000u
		: (glowByte > 255 ? 0x00ffffffu : (0x00010101u * (UnsignedInt)glowByte));
	// Vertex diffuse color scales the glow intensity (0x33 was the hard-coded ~20%).
	BuildBloomQuad(v, (Real)xpos, (Real)ypos, (Real)(xpos+width), (Real)(ypos+height), u0, v0, u1, v1, glowColor);
	DX8Wrapper::Apply_Render_State_Changes();	//flush cached states before drawing directly through the device.
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BloomQuadVertex));

	reset();
	return true;
}

/*=========  ScreenBWFilter	=============================================================*/
///converts viewport to black & white.

Int ScreenBWFilter::m_fadeFrames;
Int ScreenBWFilter::m_curFadeFrame;
Real ScreenBWFilter::m_curFadeValue;
Int ScreenBWFilter::m_fadeDirection;

ScreenBWFilter screenBWFilter;
ScreenBWFilterDOT3 screenBWFilterDOT3;	//slower version for older cards without pixel shaders.

///List of different BW shader implementations in order of preference
W3DFilterInterface *ScreenBWFilterList[]=
{
	&screenBWFilter,
	&screenBWFilterDOT3,	//slower version for older cards without pixel shaders.
	nullptr
};

// GeneralsX @feature PS3.0 batch4 12/10/2026 -------------------------------------------------
// Runtime-compiled monochrome screen filter, and the pilot for the tier-gated shader path.
//
// Why the asset alone is not enough: dist/shaders/monochrome.pso is Direct3D 8 microcode - the file
// starts 01 01 FF FF, i.e. a ps_1_1 DX8 shader blob. DXVK's d3d9 backend translates those blobs, which
// is why the effect worked on Linux/macOS and under the old d3d8 shim, but a D3D9 shader object needs a
// DXBC/FXS container (see the native-d3d9 probe output probe_native.txt under the engine-rework
// work folder: only the HLSL profiles compile and create there),
// so the DX8 blob is the wrong format for the ported path. ScreenBWFilter::init() therefore kept
// loading nothing on Windows and the black & white mission effect silently dropped to
// ScreenBWFilterDOT3's two-pass fixed-function approximation. The asset path is kept first (zero
// regression where it already works) and the HLSL source below is the fallback that makes the
// pixel-shader path exist on every D3D9 device.
//
// The math reproduces both the retail effect and that fixed-function fallback: the fallback's
// D3DTA_TFACTOR 0x80A5CA8E dot3 is exactly BT.601 luma (0.294/0.584/0.114 after the [-1,1] decode),
// the fade is the lerp weight, and the tint is the per-channel multiplier. ScreenBWFilter::set() already
// feeds c0/c1/c2 in that order, so no C++-side constant changes are needed.
//
// ps_3_0 vs ps_2_0: the arithmetic is identical on purpose. What this batch validates is the plumbing
// every later batch depends on - tier-gated profile selection, compile through the CompatLib
// D3DXAssembleShader shim (d3dcompiler!D3DCompile), CreatePixelShader acceptance, and the constants
// travelling through DX8Wrapper::Set_Pixel_Shader_Constant's new budget-checked path.
//
// Windows-only, same as the dot3 and water runtime assembly paths: the D3DXAssembleShader shim is only
// linked into the Windows build, and on Linux/macOS the .pso asset path above already works through
// DXVK's d3d9 backend, so those platforms keep exactly the behaviour they have today.
//=============================================================================
// GeneralsX @feature PS3.0 batch7 13/10/2026 -------------------------------------------------------------------------
// The profile a runtime-compiled shader should be emitted for, taken from the caps-derived tier ladder
// (getShaderModelTier) so a call site cannot drift away from the -shadermodel cap or the user's setting.
// nullptr means "below ps_2_0/vs_2_0": the HLSL dialect these sources are written in is not available,
// and the caller must stay on its fixed-function or shipped-asset route.
const char* W3DShaderManager::getPixelShaderProfile()
{
	switch (getShaderModelTier())
	{
		case DC_GENERIC_PIXEL_SHADER_3_0:	return "ps_3_0";
		case DC_GENERIC_PIXEL_SHADER_2_0:	return "ps_2_0";
		default:									return nullptr;
	}
}

const char* W3DShaderManager::getVertexShaderProfile()
{
	switch (getShaderModelTier())
	{
		case DC_GENERIC_PIXEL_SHADER_3_0:	return "vs_3_0";
		case DC_GENERIC_PIXEL_SHADER_2_0:	return "vs_2_0";
		default:									return nullptr;
	}
}

// GeneralsX @feature PS3.0 batch7 13/10/2026 -------------------------------------------------------------------------
// The one place that turns HLSL text into a device shader. Before this every call site hard-coded its
// own "// d3d-target:" line, so ps_3_0 - flow control, 32 temporaries, 224 pixel constants - had to be
// re-decided (or forgotten) per shader, and a device that refused SM3 had no path back down.
//
// The source is passed without the target line and the directive is prefixed here, at the tier the caps
// report, with a single step down the ladder as the retry. Compiling the same source one profile higher
// is safe by construction (ps_2_0 features are a subset of ps_3_0), so the retry is for a runtime that
// rejects the newer profile outright, not for source that needs rewriting.
#ifdef _WIN32
HRESULT W3DShaderManager::compileShaderAtBestProfile(const char* hlslSource, Bool ShaderType,
	const char* logTag, const char** outProfile, void** pHandle)
{
	if (pHandle) *pHandle=nullptr;
	if (outProfile) *outProfile=nullptr;
	if (!hlslSource) return E_INVALIDARG;

	const char* profile=ShaderType ? getVertexShaderProfile() : getPixelShaderProfile();
	if (!profile)
	{
		fprintf(stderr,"%s no HLSL profile available below shader model 2.0\n",logTag?logTag:"[SHADER]");
		fflush(stderr);
		return E_FAIL;
	}

	// ps_3_0 -> ps_2_0, vs_3_0 -> vs_2_0. Anything else is already the floor.
	const char* retry=(profile[3]=='3') ? (ShaderType ? "vs_2_0" : "ps_2_0") : nullptr;

	// The sources are a few kilobytes; snprintf is bounded on purpose so an over-long shader reports
	// instead of being silently truncated into a compile error nobody can attribute.
	char source[16384];

	for (Int attempt=0; attempt<(retry?2:1); ++attempt)
	{
		const char* target=(attempt==0) ? profile : retry;
		Int written=snprintf(source,sizeof(source),"// d3d-target: %s\n%s",target,hlslSource);
		if (written<0 || (size_t)written>=sizeof(source))
		{
			fprintf(stderr,"%s HLSL source does not fit the compile buffer (%d bytes)\n",
				logTag?logTag:"[SHADER]",written);
			fflush(stderr);
			return E_FAIL;
		}

		LPD3DXBUFFER code=nullptr;
		LPD3DXBUFFER errors=nullptr;
		HRESULT hr=D3DXAssembleShader(source,(UINT)strlen(source),0,nullptr,&code,&errors);
		if (SUCCEEDED(hr) && code!=nullptr)
		{
			if (ShaderType)
				hr=DX8Wrapper::_Get_D3D_Device8()->CreateVertexShader(
					(const DWORD*)code->GetBufferPointer(),(IDirect3DVertexShader9**)pHandle);
			else
				hr=DX8Wrapper::_Get_D3D_Device8()->CreatePixelShader(
					(const DWORD*)code->GetBufferPointer(),(IDirect3DPixelShader9**)pHandle);
		}

		const Bool failed=FAILED(hr) || (pHandle && *pHandle==nullptr);
		if (failed)
		{
			fprintf(stderr,"%s %s compile/create failed hr=0x%08X err=%s\n",
				logTag?logTag:"[SHADER]",target,(unsigned)hr,
				(errors && errors->GetBufferPointer())?(const char*)errors->GetBufferPointer():"(none)");
			fflush(stderr);
		}

		if (errors) errors->Release();
		if (code) code->Release();

		if (!failed)
		{
			// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 8: offer the raw source to the
			// translated-program registry. The hash is over hlslSource (not the prefixed buffer), because
			// that is the bytes the exported block and scripts/pgm_hash.py describe. A source with no row
			// is logged once and simply leaves the D3D9 path in charge.
			[[maybe_unused]] const unsigned boundTag = VKPgm_Note_Compiled(pHandle ? *pHandle : nullptr, ShaderType ? 0 : 1, hlslSource, logTag);
			// GeneralsX @bugfix Qoder 19/10/2026 BUG-03: under no-d3d9 an untagged handle has no twin to
			// execute, and the draw path refuses every batch that binds one - the whole family would
			// disappear instead of staying on its fixed-function leg. Release the handle and fail the
			// compile, which routes the caller down the same branch a null answer always took. On the
			// d3d9 leg an untagged handle still runs natively, so the guard fires only there.
#if defined(SAGE_USE_VULKAN_CORE)
			if (!boundTag && pHandle && *pHandle && VKPgm_On() && VK_NOD3D_Enabled())
			{
				if (ShaderType)
					reinterpret_cast<IDirect3DVertexShader9*>(*pHandle)->Release();
				else
					reinterpret_cast<IDirect3DPixelShader9*>(*pHandle)->Release();
				*pHandle = nullptr;
				fprintf(stderr, "%s %s has no translated twin under no-d3d9; the fixed-function leg stays\n",
					logTag ? logTag : "[SHADER]", target);
				fflush(stderr);
				return E_FAIL;
			}
#endif
			if (outProfile) *outProfile=target;
			return S_OK;
		}
		if (pHandle) *pHandle=nullptr;
	}

	return E_FAIL;
}
#endif	// _WIN32

#ifdef _WIN32
static const char* kMonochromeShaderBody =
	"sampler Screen : register(s0);\n"
	"float4 LumaWeights : register(c0);\n"
	"float4 TintColor   : register(c1);\n"
	"float4 FadeLevel   : register(c2);\n"
	"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
	"{\n"
	"    float4 src = tex2D(Screen, uv);\n"
	"    float3 bw = dot(src.rgb, LumaWeights.rgb) * TintColor.rgb;\n"
	"    return float4(lerp(src.rgb, bw, FadeLevel.x), src.a);\n"
	"}\n";
#endif	// _WIN32

Int ScreenBWFilter::init()
{
	Int res;
	HRESULT hr;

	m_dwBWPixelShader = nullptr;
	m_curFadeFrame = 0;

	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return false;
	}

	if ((res=W3DShaderManager::getChipset()) != 0)
	{
		if (res >= DC_GENERIC_PIXEL_SHADER_1_1)
		{
			//this shader needs some assets that need to be loaded
			//shader decleration
			D3DVERTEXELEMENT9 Declaration[]=
			{
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 0), // Position
				D3DDECL_USAGE(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 12), // Diffuse
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, 0, 16), //  Texture Coordinates
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, 1, 24), //  Texture Coordinates
				D3DDECL_END()
			};

			// GeneralsX @bugfix BenderAI 13/02/2026 - Don't crash if monochrome.pso missing (optional effect)
			//Monochrome pixel shader.
			IDirect3DPixelShader9* shader = nullptr;
			Bool preferHlsl = false;
#ifdef _WIN32
			// GeneralsX @feature PS3.0 batch4 12/10/2026 The shipped container is ps_1_1 microcode, so taking
			// it whenever it loads leaves the upgraded source unreachable on hardware that can do better. On
			// a device that accepts ps_3_0 compile the HLSL first and keep the microcode as the fallback.
			preferHlsl = W3DShaderManager::getShaderModelTier() >= DC_GENERIC_PIXEL_SHADER_3_0;
#endif // _WIN32
			if (!preferHlsl)
			{
				hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\monochrome.pso", &Declaration[0], 0, false, (void**)&shader);
				if (FAILED(hr) || shader == nullptr)
					shader = nullptr;
			}

			if (shader == nullptr)
			{
#ifdef _WIN32
				// GeneralsX @feature PS3.0 batch4 12/10/2026 either the asset was not usable on this device
				// (missing from the archive, or a DX8 container D3D9 cannot wrap) or the device can take a
				// higher profile than the asset was built for.
				// GeneralsX @feature PS3.0 batch7 13/10/2026 the profile choice and the ps_3_0 -> ps_2_0
				// retry moved into compileShaderAtBestProfile, so this filter emits whatever the tier ladder
				// allows instead of carrying its own copy of the ladder.
				const char* profile = nullptr;
				void* handle = nullptr;
				if (SUCCEEDED(W3DShaderManager::compileShaderAtBestProfile(kMonochromeShaderBody, false,
					"[BWFILTER]", &profile, &handle)))
				{
					shader = (IDirect3DPixelShader9*)handle;
				}
				if (shader != nullptr)
				{
					// Which profile the device actually got is the whole point of the shader-model ladder,
					// so report it - and say when it was taken ahead of the shipped asset.
					fprintf(stderr, "[BWFILTER] monochrome pixel shader compiled as %s%s\n", profile,
						preferHlsl ? " (ahead of the shipped ps_1_1 asset)" : "");
					fflush(stderr);
				}
				else
				{
					// Last resort: the microcode we skipped above is still better than no filter at all.
					hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\monochrome.pso", &Declaration[0], 0, false, (void**)&shader);
					if (FAILED(hr) || shader == nullptr)
						shader = nullptr;
				}
#endif // _WIN32
			}

			if (shader == nullptr)
			{
				// Monochrome filter is optional - game can run without it
				return FALSE;
			}
			m_dwBWPixelShader = shader;

			W3DFilters[FT_VIEW_BW_FILTER]=&screenBWFilter;
			DEBUG_LOG(("W3DShaderManager: BW screen filter enabled"));

			return TRUE;
		}
	}
	return FALSE;
}

Bool ScreenBWFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	skipRender = false;
	W3DShaderManager::startRenderToTexture();
	return true;
}

Bool ScreenBWFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	IDirect3DTexture9 * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex) return false;
	if (!set(mode)) return false;

	struct _TRANS_LIT_TEX_VERTEX {
		D3DXVECTOR4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	DX8Wrapper::Set_DX8_Texture(0,tex);	//previously rendered frame inside this texture
	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = D3DXVECTOR4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = D3DXVECTOR4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = D3DXVECTOR4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = D3DXVECTOR4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[0].color = 0xffffffff;
	v[1].color = 0xffffffff;
	v[2].color = 0xffffffff;
	v[3].color = 0xffffffff;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANS_LIT_TEX_VERTEX));

	reset();
	return true;
}

Int ScreenBWFilter::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{
		//rendering a quad with redirected rendering surface tinted by pixel shader

		if (m_fadeDirection > 0)
		{
			//turning effect on
			m_curFadeFrame++;
			Int fade = m_curFadeFrame;

			if (fade<m_fadeFrames)
			{
				m_curFadeValue = (Real)fade/(Real)m_fadeFrames;
			}
			else
			{
				m_curFadeFrame = 0;
				m_curFadeValue = 1.0f;
				m_fadeDirection = 0;
			}
		}
		else
		if (m_fadeDirection < 0)
		{
			//turning effect off
			m_curFadeFrame++;
			Int fade = m_curFadeFrame;
			if (fade<m_fadeFrames)
			{
				m_curFadeValue = 1.0f - (Real)fade/(Real)m_fadeFrames;
			}
			else
			{	m_curFadeValue = 0.0f;
				TheTacticalView->setViewFilterMode(FM_NULL_MODE);
				TheTacticalView->setViewFilter(FT_NULL_FILTER);
				m_curFadeFrame = 0;
				m_fadeDirection = 0;
			}
		}

		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		DX8Wrapper::Set_Material(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
		DX8Wrapper::Set_Texture(0,nullptr);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_ALWAYS);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,FALSE);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		// GeneralsX @refactor PS3.0 batch4 12/10/2026 bind through the wrapper instead of the device so
		// the shader-handle cache and the constant cache stay coherent (see DX8Wrapper::Set_Pixel_Shader
		// and the budget guard added in batch 1). Direct device calls here used to leave Pixel_Shader
		// pointing at a stale handle, which after a device Reset is the same recycled address as some
		// other shader - the wrapper would then skip the binding the frame actually needs.
		DX8Wrapper::Set_Pixel_Shader((UINT_PTR)m_dwBWPixelShader);

		const Vector4 lumaWeights(0.3f, 0.59f, 0.11f, 1.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(0, &lumaWeights, 1);

		Vector4	color(1.0f,1.0f,1.0f,1.0f);	//multiply color

		if (mode == FM_VIEW_BW_BLACK_AND_WHITE)
		{
			//back & white mode
			color.X=1.0f;
			color.Y=1.0f;
			color.Z=1.0f;
		}
		if (mode == FM_VIEW_BW_RED_AND_WHITE)
		{
			//red is on
			color.X = 1.0f;
			color.Y = 0.0f;
			color.Z = 0.0f;
		}
		if (mode == FM_VIEW_BW_GREEN_AND_WHITE)
		{
			color.X = 0.0f;
			color.Y = 1.0f;
			color.Z = 0.0f;
		}

		const Vector4 fade(m_curFadeValue, m_curFadeValue, m_curFadeValue, 1.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(1, &color, 1);
		DX8Wrapper::Set_Pixel_Shader_Constant(2, &fade, 1);

		return true;
	}
	return false;
}

void ScreenBWFilter::reset()
{
	// GeneralsX @refactor PS3.0 batch4 12/10/2026 the texture unbind stays a device call on purpose: the
	// rendered frame is a bare IDirect3DTexture9 from endRenderToTexture(), never a TextureBaseClass, so
	// it was never in the wrapper's texture cache for this stage and there is no cached reference to
	// release. The shader unbind does go through the wrapper, which is what keeps the handle cache right.
	DX8Wrapper::Set_DX8_Texture(0,nullptr);	//previously rendered frame inside this texture
	DX8Wrapper::Set_Pixel_Shader(0);	//turn off pixel shader
	DX8Wrapper::Invalidate_Cached_Render_States();
}

Int ScreenBWFilter::shutdown()
{
	if (m_dwBWPixelShader)
		m_dwBWPixelShader->Release();

	m_dwBWPixelShader = nullptr;

	return TRUE;
}

/**Alternate version of the above filter which does not require pixel shaders - good for older cards*/
Int ScreenBWFilterDOT3::init()
{
	Int res;

	m_curFadeFrame = 0;

	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return false;
	}

	if ((res=W3DShaderManager::getChipset()) != 0)
	{
			W3DFilters[FT_VIEW_BW_FILTER]=&screenBWFilterDOT3;
			return TRUE;
	}
	return FALSE;
}

Bool ScreenBWFilterDOT3::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	skipRender = false;
	W3DShaderManager::startRenderToTexture();
	return true;
}

Bool ScreenBWFilterDOT3::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	IDirect3DTexture9 * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex) return false;
	if (!set(mode)) return false;

	struct _TRANS_LIT_TEX_VERTEX {
		D3DXVECTOR4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = D3DXVECTOR4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = D3DXVECTOR4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = D3DXVECTOR4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = D3DXVECTOR4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();

	DWORD currentFade=(((Int)((1.0f-m_curFadeValue) * 255.0f))<<24) | 0x00ffffff;	//store alpha value

	v[0].color = currentFade;
	v[1].color = currentFade;
	v[2].color = currentFade;
	v[3].color = currentFade;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

	//Draw B&W version first
	if (DX8Wrapper::Get_Current_Caps()->Support_Dot3())
	{
		//Override W3D states with customizations for grayscale
		DX8Wrapper::Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, 0x80A5CA8E);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG0, D3DTA_TFACTOR | D3DTA_ALPHAREPLICATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_TFACTOR | D3DTA_ALPHAREPLICATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP, D3DTOP_MULTIPLYADD);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_CURRENT);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
	}
	else
	{
		//doesn't have DOT3 blend mode so fake it another way.
		DX8Wrapper::Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, 0x60606060);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	}

	DX8Wrapper::Set_DX8_Texture(0,tex);	//previously rendered frame inside this texture

	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANS_LIT_TEX_VERTEX));

	//Draw normal view blended by current fade level
	ShaderClass::Invalidate();	//reset DOT3 blend from above.
	ShaderClass shader=ShaderClass::_PresetAlphaShader;
	shader.Set_Depth_Compare(ShaderClass::PASS_ALWAYS);
	DX8Wrapper::Set_Shader(shader);
	DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices
	//replace texture alpha with vertex alpha
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);

	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANS_LIT_TEX_VERTEX));

	reset();
	return true;
}

Int ScreenBWFilterDOT3::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{
		//rendering a quad with redirected rendering surface tinted by pixel shader

		if (m_fadeDirection > 0)
		{
			//turning effect on
			m_curFadeFrame++;
			Int fade = m_curFadeFrame;

			if (fade<m_fadeFrames)
			{
				m_curFadeValue = (Real)fade/(Real)m_fadeFrames;
			}
			else
			{
				m_curFadeFrame = 0;
				m_curFadeValue = 1.0f;
				m_fadeDirection = 0;
			}
		}
		else
		if (m_fadeDirection < 0)
		{
			//turning effect off
			m_curFadeFrame++;
			Int fade = m_curFadeFrame;
			if (fade<m_fadeFrames)
			{
				m_curFadeValue = 1.0f - (Real)fade/(Real)m_fadeFrames;
			}
			else
			{	m_curFadeValue = 0.0f;
				TheTacticalView->setViewFilterMode(FM_NULL_MODE);
				TheTacticalView->setViewFilter(FT_NULL_FILTER);
				m_curFadeFrame = 0;
				m_fadeDirection = 0;
			}
		}

		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		DX8Wrapper::Set_Material(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
		DX8Wrapper::Set_Texture(0,nullptr);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_ALWAYS);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,FALSE);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		return true;
	}
	return false;
}

void ScreenBWFilterDOT3::reset()
{
	DX8Wrapper::Set_DX8_Texture(0,nullptr);	//previously rendered frame inside this texture
	DX8Wrapper::Invalidate_Cached_Render_States();
}

Int ScreenBWFilterDOT3::shutdown()
{
	return TRUE;
}

/*=========  ScreenCrossFadeFilter	=============================================================*/
///Fades screen between 2 different views of the scene with both being visible at once.

Int ScreenCrossFadeFilter::m_fadeFrames;
Int ScreenCrossFadeFilter::m_curFadeFrame;
Real ScreenCrossFadeFilter::m_curFadeValue;
Int ScreenCrossFadeFilter::m_fadeDirection;
TextureClass *ScreenCrossFadeFilter::m_fadePatternTexture=nullptr;
Bool ScreenCrossFadeFilter::m_skipRender = FALSE;

ScreenCrossFadeFilter screenCrossFadeFilter;

///List of different BW shader implementations in order of preference
///@todo: Add a version that doesn't require pixel shader
W3DFilterInterface *ScreenCrossFadeFilterList[]=
{
	&screenCrossFadeFilter,
	nullptr
};

Int ScreenCrossFadeFilter::init()
{
	if (!TheDisplay)
		return FALSE;	//effect is useless without a view so no point initializing for the WB, etc.

	m_curFadeFrame = 0;

	if (!W3DShaderManager::canRenderToTexture())
		// Have to be able to render to texture.
		return FALSE;

	//Load an alpha mask texture that will mix foreground/background views.
	m_fadePatternTexture=WW3DAssetManager::Get_Instance()->Get_Texture("exmask_g.tga");
	if (!m_fadePatternTexture)
		return FALSE;
	m_fadePatternTexture->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
	m_fadePatternTexture->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
	m_fadePatternTexture->Get_Filter().Set_Mip_Mapping(TextureFilterClass::FILTER_TYPE_NONE);

	W3DFilters[FT_VIEW_CROSSFADE]=&screenCrossFadeFilter;

	return TRUE;
}

Bool ScreenCrossFadeFilter::updateFadeLevel()
{
	if (m_fadeDirection > 0)
	{
		//turning effect on
		m_curFadeFrame++;
		Int fade = m_curFadeFrame;

		if (fade<m_fadeFrames)
		{
			m_curFadeValue = (Real)fade/(Real)m_fadeFrames;
		}
		else
		{
			m_curFadeFrame = 0;
			m_curFadeValue = 1.0f;
			m_fadeDirection = 0;
			return false;
		}
	}
	else
	if (m_fadeDirection < 0)
	{
		//turning effect off
		Int fade = m_curFadeFrame;
		if (fade<m_fadeFrames)
		{
			m_curFadeValue = 1.0f - (Real)fade/(Real)m_fadeFrames;
			m_curFadeFrame++;
		}
		else
		{	m_curFadeValue = 0.0f;
			TheTacticalView->setViewFilterMode(FM_NULL_MODE);
			TheTacticalView->setViewFilter(FT_NULL_FILTER);
			m_curFadeFrame = 0;
			m_fadeDirection = 0;
			return false;
		}
	}
	return true;
}

Bool ScreenCrossFadeFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	if (updateFadeLevel())
	{
		//if fade has not completed
		W3DShaderManager::startRenderToTexture();
		scenePassMode=SCENE_PASS_ALPHA_MASK;
		skipRender = false;
		m_skipRender=true;	//tell the postRender function not to draw into framebuffer yet.
		return true;
	}
	//fade must have completed
	return true;
}

Bool ScreenCrossFadeFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	IDirect3DTexture9 * tex;

	if (m_skipRender)
	{
		//don't render anything to frame buffer because we still need to draw the new scene
		//that we're fading into.  Okay to render on the next call.
		m_skipRender = false;
		doExtraRender = TRUE;
		tex =	W3DShaderManager::endRenderToTexture();
		return true;
	}

	tex=W3DShaderManager::getRenderTexture();

	DEBUG_ASSERTCRASH(tex, ("Require last rendered texture."));
	if (!tex) return false;
	if (!set(mode)) return false;

	struct _TRANS_LIT_TEX_VERTEX {
		D3DXVECTOR4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
		float	u1;
		float	v1;
	} v[4];

	Int xpos, ypos, width, height;
	Real radius = 0.0f;

	DX8Wrapper::Set_DX8_Texture(0,tex);	//previously rendered frame inside this texture
	if (mode == FM_VIEW_CROSSFADE_CIRCLE)
	{	DX8Wrapper::Set_DX8_Texture(1,m_fadePatternTexture->Peek_D3D_Texture());
		//Use the current fade level to scale the mask texture, for other modes the texture
		//comes pre-scaled so doesn't require uv scaling.
		radius = (1.0f-m_curFadeValue)*2.0f;
		if (radius <= 0)
			radius = 0.01f;
		radius = 0.5f/radius;
	}

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

/*	Real radius = (1.0f-m_curFadeValue);
	if (radius <= 0)
		radius = 0.01f;
	radius = 25.0f-radius*24.75f;
*/
	//bottom right
	v[0].p = D3DXVECTOR4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	v[0].u1 = 0.5f+radius;	v[0].v1 = 0.5f+radius;
	//top right
	v[1].p = D3DXVECTOR4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[1].u1 = 0.5f+radius;	v[1].v1 = 0.5f-radius;
	//bottom left
	v[2].p = D3DXVECTOR4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	v[2].u1 = 0.5f-radius;	v[2].v1 = 0.5f+radius;
	//top left
	v[3].p = D3DXVECTOR4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[3].u1 = 0.5f-radius;	v[3].v1 = 0.5f-radius;

	DWORD diffuse = 0xffffffff;//((Int)((m_curFadeValue) * 255.0f) << 24) | 0x00ffffff;	//store alpha value in vertex diffuse

	v[0].color = diffuse;
	v[1].color = diffuse;
	v[2].color = diffuse;
	v[3].color = diffuse;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2);

	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANS_LIT_TEX_VERTEX));

	reset();
	return true;
}

Int ScreenCrossFadeFilter::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{
		//rendering a quad with redirected rendering surface
		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		DX8Wrapper::Set_Material(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		DX8Wrapper::Set_Shader(ShaderClass::_PresetAlphaShader);
		DX8Wrapper::Set_Texture(0,nullptr);
		DX8Wrapper::Set_Texture(1,nullptr);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

		if (mode == FM_VIEW_CROSSFADE_CIRCLE)
		{
			//cross-fading using circle mask stored in stage 1
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 1 );
			DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
			DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
		}

		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_ALWAYS);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,FALSE);

		return true;
	}
	return false;
}

void ScreenCrossFadeFilter::reset()
{
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture(0,nullptr);	//previously rendered frame inside this texture
	DX8Wrapper::Invalidate_Cached_Render_States();
}

Int ScreenCrossFadeFilter::shutdown()
{
	REF_PTR_RELEASE(m_fadePatternTexture);

	return TRUE;
}

/*=========  ScreenMotionBlurFilter	=============================================================*/
///applies motion blur to viewport.

ScreenMotionBlurFilter screenMotionBlurFilter;

Coord3D ScreenMotionBlurFilter::m_zoomToPos;
Bool ScreenMotionBlurFilter::m_zoomToValid = false;

ScreenMotionBlurFilter::ScreenMotionBlurFilter():
m_decrement(false),
m_maxCount(0),
m_lastFrame(0),
m_skipRender(false)
{
}
///List of different motion blur implementations in order of preference
W3DFilterInterface *ScreenMotionBlurFilterList[]=
{
	&screenMotionBlurFilter,
	nullptr
};

Int ScreenMotionBlurFilter::init()
{
	if (!W3DShaderManager::canRenderToTexture()) {
		// Have to be able to render to texture.
		return false;
	}
	W3DFilters[FT_VIEW_MOTION_BLUR_FILTER]=this;
	return true;
}

Bool ScreenMotionBlurFilter::preRender(Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	skipRender = m_skipRender;
	W3DShaderManager::startRenderToTexture();
	return true;
}

Bool ScreenMotionBlurFilter::postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender)
{
	IDirect3DTexture9 * tex =	W3DShaderManager::endRenderToTexture();
	DEBUG_ASSERTCRASH(tex, ("Require rendered texture."));
	if (!tex) return false;
	if (!set(mode)) return false;

	Bool continueEffect = true;
	struct _TRANS_LIT_TEX_VERTEX {
		D3DXVECTOR4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	DX8Wrapper::Set_DX8_Texture(0,tex);	//previously rendered frame inside this texture
	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = D3DXVECTOR4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = D3DXVECTOR4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = D3DXVECTOR4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = D3DXVECTOR4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[0].color = 0xffffffff;
	v[1].color = 0xffffffff;
	v[2].color = 0xffffffff;
	v[3].color = 0xffffffff;


	if (m_additive) {
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ONE);
	} else {
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
	}
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);
	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	DX8Wrapper::Apply_Render_State_Changes();
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

	Coord2D center;
	center.x = 0.5f;
	center.y = 0.5f;
	Bool pan = false;
	if (mode>=FM_VIEW_MB_PAN_ALPHA) {
		Real len = sqrt(scrollDelta.x*scrollDelta.x + scrollDelta.y*scrollDelta.y);
		//center.x += 0.5f * (scrollDelta.x/len);
		center.y -= 0.5f; // * (scrollDelta.y/len);
		m_decrement = false;
		m_maxCount = (len*200*m_panFactor/(Real)DEFAULT_PAN_FACTOR);
		if (m_maxCount<m_panFactor/2)
			m_maxCount = m_panFactor/2;
		if (m_maxCount>m_panFactor)
			m_maxCount=m_panFactor;
		pan = true;
		m_priorDelta = scrollDelta;
	} else if (mode == FM_VIEW_MB_END_PAN_ALPHA) {
		Real len = sqrt(m_priorDelta.x*m_priorDelta.x + m_priorDelta.y*m_priorDelta.y);
		center.x += 0.5f * (m_priorDelta.x/len);
		center.y -= 0.5f * (m_priorDelta.y/len);
		m_decrement = false;
		m_maxCount--;
		if (m_maxCount<2) {
			continueEffect = false;
		}
		pan = true;
	}


	m_skipRender = false;
	if (!pan && m_lastFrame != TheGameLogic->getFrame()) {
		if (m_decrement) {
			m_maxCount-=COUNT_STEP;
			if (m_maxCount<1) {
				m_decrement = false;
				continueEffect = false;
			}	else {
				m_skipRender = true;
			}
		} else {
			m_maxCount+=COUNT_STEP;
			if (m_maxCount>=MAX_COUNT) {
				m_decrement = true;
				if (m_doZoomTo && m_zoomToValid) {
					TheTacticalView->lookAt(&m_zoomToPos);
				} else {
					continueEffect = false;
				}
			}	else {
				m_skipRender = true;
			}
		}
	}
	Int	 i, j;
	if (!pan) {
		for (i=0; i<4; i++) {
			Real factor = 1.0f - (m_maxCount/(Real)MAX_COUNT)*0.90f;
			factor = sqrt(factor);
			v[i].u = ((v[i].u-center.x)*factor) + center.x;
			v[i].v = ((v[i].v-center.y)*factor) + center.y;
		}
	}
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_ALPHAARG1, D3DTA_CURRENT);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_ALPHAARG2, D3DTA_TEXTURE);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANS_LIT_TEX_VERTEX));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);

	DX8Wrapper::Apply_Render_State_Changes();
	{
		Int limit = m_maxCount;
		if (m_maxCount>30) limit = 30;
		for (j=0; j<limit; j++) {
			for (i=0; i<4; i++) {
				Real factor = 0.99f;
				if (m_additive) factor = 0.98f;
				Int alpha = 0x15;
				if (m_additive) {
					alpha = 0x09;
					if (m_maxCount>limit) {
						alpha += (m_maxCount-limit)/5;
					}
					if (m_maxCount==MAX_COUNT) alpha += 60;
				}
				v[i].color = (alpha<<24)|0x00ffffff; //
				if (pan) {
					v[i].u = ((v[i].u-center.x)*(factor+.006)) + center.x;
					v[i].v = ((v[i].v-center.y)*factor) + center.y;
				} else {
					v[i].u = ((v[i].u-center.x)*factor) + center.x;
					v[i].v = ((v[i].v-center.y)*factor) + center.y;
				}
			}
			DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANS_LIT_TEX_VERTEX));

		}
	}
	m_lastFrame = TheGameLogic->getFrame();
	if (pan){
		m_skipRender = false;
	}
	reset();
	if (!continueEffect) {
		m_zoomToValid = false;
	}
	return continueEffect;
}

Bool ScreenMotionBlurFilter::setup(FilterModes mode)
{

	m_additive = false;

	if (mode == FM_VIEW_MB_IN_AND_OUT_SATURATE ||
			mode == FM_VIEW_MB_IN_SATURATE ||
			mode == FM_VIEW_MB_OUT_SATURATE) {
		m_additive = true;
	}

	m_doZoomTo = false;
	if (mode == FM_VIEW_MB_IN_AND_OUT_SATURATE ||
			mode == FM_VIEW_MB_IN_AND_OUT_ALPHA ) {
		m_doZoomTo = true;
	}
	if (mode >= FM_VIEW_MB_PAN_ALPHA)	{
		m_panFactor = (int)mode - FM_VIEW_MB_PAN_ALPHA;
		if (m_panFactor<1) m_panFactor = DEFAULT_PAN_FACTOR;
	}
	m_skipRender = false;
	if (mode != FM_VIEW_MB_END_PAN_ALPHA)
		m_maxCount = 0;
	m_decrement = false;
	m_skipRender = false;
	switch (mode) {
		case FM_VIEW_MB_OUT_SATURATE:
		case FM_VIEW_MB_OUT_ALPHA:
			m_maxCount = MAX_COUNT;
			m_decrement = TRUE;
			break;
	}
	return true;
}

Int ScreenMotionBlurFilter::set(FilterModes mode)
{
	if (mode > FM_NULL_MODE)
	{
		//rendering a quad with redirected rendering surface motion blurred

		VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		DX8Wrapper::Set_Material(vmat);
		REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
		DX8Wrapper::Set_Texture(0,nullptr);
		DX8Wrapper::Set_Texture(1,nullptr);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices

		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_ALWAYS);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,FALSE);
		DX8Wrapper::Apply_Render_State_Changes();	//force update of view and projection matrices
	}
	return TRUE;
}

void ScreenMotionBlurFilter::reset()
{
	DX8Wrapper::Set_DX8_Texture(0,nullptr);	//previously rendered frame inside this texture
	DX8Wrapper::Invalidate_Cached_Render_States();
}

Int ScreenMotionBlurFilter::shutdown()
{
	return TRUE;
}

/*===========================================================================================*/
/*=========      Shroud Shaders	=============================================================*/
/*===========================================================================================*/

///Shroud layer rendering shader
class ShroudTextureShader : public W3DShaderInterface
{
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	Int m_stageOfSet;
} shroudTextureShader;

///List of different shroud shader implementations in order of preference
W3DShaderInterface *ShroudShaderList[]=
{
	&shroudTextureShader,
	nullptr
};

//#define SHROUD_STRETCH_FACTOR	(1.0f/MAP_XY_FACTOR)	//1 texel per heightmap cell width

Int ShroudTextureShader::init()
{
	W3DShaders[W3DShaderManager::ST_SHROUD_TEXTURE]=&shroudTextureShader;
	W3DShadersPassCount[W3DShaderManager::ST_SHROUD_TEXTURE]=1;

	return TRUE;
}

//Setup a texture projection in the given stage that applies our shroud.
Int ShroudTextureShader::set(Int stage)
{
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.
	DX8Wrapper::Set_Texture(stage, W3DShaderManager::getShaderTexture(0));	//shroud always stored in texture 0

	if (stage == 0)
	{
#if defined(RTS_DEBUG)
	if (TheGlobalData && TheGlobalData->m_fogOfWarOn)
		DX8Wrapper::Set_Shader(ShaderClass::_PresetAlphaSpriteShader);
	else
		DX8Wrapper::Set_Shader(ShaderClass::_PresetMultiplicativeSpriteShader);
#else
	DX8Wrapper::Set_Shader(ShaderClass::_PresetMultiplicativeSpriteShader);
#endif
	}
	DX8Wrapper::Apply_Render_State_Changes();

	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_EQUAL);

	//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
	//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
	W3DShroud *shroud;
	if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr && shroud->isTextureReady())
	{	///@todo: All this code really only need to be done once per camera/view.  Find a way to optimize it out.
		Matrix4x4 curView;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

		D3DXMATRIX inv;
		float det;
		D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

		D3DXMATRIX scale,offset;

		//We need to make all world coordinates be relative to the heightmap data origin since that
		//is where the shroud begins.

		float xoffset = 0;
		float yoffset = 0;
		Real width=shroud->getCellWidth();
		Real height=shroud->getCellHeight();

		if (TheTerrainRenderObject->getMap())
		{
			//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
			xoffset = -(float)shroud->getDrawOriginX() + width;
			yoffset = -(float)shroud->getDrawOriginY() + height;
		}

		D3DXMatrixTranslation(&offset, xoffset, yoffset,0);

		width = 1.0f/(width*shroud->getTextureWidth());
		height = 1.0f/(height*shroud->getTextureHeight());
		D3DXMatrixScaling(&scale, width, height, 1);
		*((D3DXMATRIX*)&curView) = (inv * offset) * scale;
		DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE )(D3DTS_TEXTURE0+stage), curView);
	}
	m_stageOfSet=stage;
	return TRUE;
}

void ShroudTextureShader::reset()
{
	DX8Wrapper::Set_Texture(m_stageOfSet,nullptr);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
	DX8Wrapper::Set_DX8_Texture_Stage_State(m_stageOfSet,  D3DTSS_TEXCOORDINDEX, m_stageOfSet);
	DX8Wrapper::Set_DX8_Texture_Stage_State(m_stageOfSet,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
}

///Shroud layer rendering shader
class FlatShroudTextureShader : public W3DShaderInterface
{
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	Int m_stageOfSet;
} flatShroudTextureShader;

///List of different shroud shader implementations in order of preference
W3DShaderInterface *FlatShroudShaderList[]=
{
	&flatShroudTextureShader,
	nullptr
};

//#define SHROUD_STRETCH_FACTOR	(1.0f/MAP_XY_FACTOR)	//1 texel per heightmap cell width

Int FlatShroudTextureShader::init()
{
	W3DShaders[W3DShaderManager::ST_FLAT_SHROUD_TEXTURE]=&flatShroudTextureShader;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_SHROUD_TEXTURE]=1;

	return TRUE;
}

//Setup a texture projection in the given stage that applies our shroud.
Int FlatShroudTextureShader::set(Int stage)
{
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	if (stage < 2)
		DX8Wrapper::Set_Texture(stage, W3DShaderManager::getShaderTexture(stage));
	else	//stages larger than 1 are not supported by W3D so set them directly
		DX8Wrapper::Set_DX8_Texture(stage, W3DShaderManager::getShaderTexture(stage)->Peek_D3D_Texture());

	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG2, D3DTA_CURRENT );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	//DX8Wrapper::Apply_Render_State_Changes();

	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

	//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
	//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
	W3DShroud *shroud;
	if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr && shroud->isTextureReady())
	{	///@todo: All this code really only need to be done once per camera/view.  Find a way to optimize it out.
		Matrix4x4 curView;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

		D3DXMATRIX inv;
		float det;
		D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

		D3DXMATRIX scale,offset;

		//We need to make all world coordinates be relative to the heightmap data origin since that
		//is where the shroud begins.

		float xoffset = 0;
		float yoffset = 0;
		Real width=shroud->getCellWidth();
		Real height=shroud->getCellHeight();

		if (TheTerrainRenderObject->getMap())
		{
			//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
			xoffset = -(float)shroud->getDrawOriginX() + width;
			yoffset = -(float)shroud->getDrawOriginY() + height;
		}

		D3DXMatrixTranslation(&offset, xoffset, yoffset,0);

		width = 1.0f/(width*shroud->getTextureWidth());
		height = 1.0f/(height*shroud->getTextureHeight());
		D3DXMatrixScaling(&scale, width, height, 1);
		*((D3DXMATRIX*)&curView) = (inv * offset) * scale;
		DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE )(D3DTS_TEXTURE0+stage), curView);
	}
	m_stageOfSet=stage;
	return TRUE;
}

void FlatShroudTextureShader::reset()
{
	if (m_stageOfSet < MAX_TEXTURE_STAGES)
		DX8Wrapper::Set_Texture(m_stageOfSet,nullptr);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
	DX8Wrapper::Set_DX8_Texture_Stage_State(m_stageOfSet,  D3DTSS_TEXCOORDINDEX, m_stageOfSet);
	DX8Wrapper::Set_DX8_Texture_Stage_State(m_stageOfSet,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
}

///Mask layer rendering shader
class MaskTextureShader : public W3DShaderInterface
{
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
} maskTextureShader;

///List of different shroud shader implementations in order of preference
W3DShaderInterface *MaskShaderList[]=
{
	&maskTextureShader,
	nullptr
};

Int MaskTextureShader::init()
{
	W3DShaders[W3DShaderManager::ST_MASK_TEXTURE]=&maskTextureShader;
	W3DShadersPassCount[W3DShaderManager::ST_MASK_TEXTURE]=1;

	return TRUE;
}

Int MaskTextureShader::set(Int pass)
{
	Real fadeLevel=ScreenCrossFadeFilter::getCurrentFadeValue();

	//Use the current fade level to scale the mask texture
	Real radius = (1.0f-fadeLevel)*2.0f;
	if (radius <= 0)
		radius = 0.01f;
	radius = 0.5f/radius;

	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.

	//For now we're always going to project the texture coming from the crossfade effect
	DX8Wrapper::Set_Texture(0, ScreenCrossFadeFilter::getCurrentMaskTexture());
	ShaderClass shader=ShaderClass::_PresetOpaqueShader;
	shader.Set_Primary_Gradient(ShaderClass::GRADIENT_DISABLE);
	DX8Wrapper::Set_Shader(shader);
	DX8Wrapper::Apply_Render_State_Changes();

	Matrix4x4 curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

	DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

	D3DXMATRIX inv;
	float det;

	//Get inverse view matrix so we can transform camera space points back to world space
	D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

	D3DXMATRIX scale,offset,offsetTextureCenter;
	Coord3D centerPos;
	centerPos.zero();

	//Find center of projection (this should be returned from some other filter, etc. but
	//for now assume terrain location at center of screen.
	if (TheTacticalView)
	{	Int xpos,ypos;

		TheTacticalView->getOrigin(&xpos,&ypos);

		ICoord2D screenPos;
		screenPos.x=(Real)TheTacticalView->getWidth()*0.5f;
		screenPos.y=(Real)TheTacticalView->getHeight()*0.5f;
		TheTacticalView->screenToTerrain(&screenPos,&centerPos);
	}

	D3DXMatrixTranslation(&offset, -centerPos.x, -centerPos.y,0);

	D3DXMatrixTranslation(&offsetTextureCenter, 0.5f, 0.5f, 0);	//shift coordinates so center of projection falls at uv 0.5,0.5

	Real worldTexelWidth=(1.0f-fadeLevel)*25.0f;	//9 worked well for circle but weird shape requires more stretch to cover.
	Real worldTexelHeight=(1.0f-fadeLevel)*25.0f;

	///@todo: Fix this to work with non 128x128 textures.
	if (worldTexelWidth != 0 && worldTexelHeight != 0)
	{
		Real widthScale = 1.0f/(worldTexelWidth*128.0f);
		Real heightScale = 1.0f/(worldTexelHeight*128.0f);
		D3DXMatrixScaling(&scale, widthScale, heightScale, 1);
		*((D3DXMATRIX*)&curView) = ((inv * offset) * scale)*offsetTextureCenter;
	}
	else
	{
		D3DXMatrixScaling(&scale, 0, 0, 1);	//scaling by 0 will set uv coordinates to 0,0
		*((D3DXMATRIX*)&curView) = ((inv * offset) * scale);
	}

	DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, curView);

	return TRUE;
}

void MaskTextureShader::reset()
{
	DX8Wrapper::Set_Texture(0,nullptr);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXCOORDINDEX, 0);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
}

/*===========================================================================================*/
/*=========      Terrain Shaders	=========================================================*/
/*===========================================================================================*/

///regular terrain shader that should work on all multi-texture video cards (slowest version)
class TerrainShader2Stage : public W3DShaderInterface
{
public:
	float m_xSlidePerSecond ;	 ///< How far the clouds move per second.
	float m_ySlidePerSecond ;	 ///< How far the clouds move per second.
	float m_xOffset;
	float m_yOffset;

	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.

	void updateCloud();
	void updateNoise1 (D3DXMATRIX *destMatrix,D3DXMATRIX *curViewInverse, Bool doUpdate=true);	///<generate the uv coordinates for Noise1 (i.e clouds)
	void updateNoise2 (D3DXMATRIX *destMatrix,D3DXMATRIX *curViewInverse, Bool doUpdate=true);	///<generate the uv coordinates for Noise2 (i.e lightmap)
} terrainShader2Stage;

///regular terrain shader that should work on all multi-texture video cards (slowest version)
class FlatTerrainShader2Stage : public W3DShaderInterface
{
public:
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
} flatTerrainShader2Stage;

///regular terrain shader that should work on all multi-texture video cards (slowest version)
class FlatTerrainShaderPixelShader : public W3DShaderInterface
{
public:
	IDirect3DPixelShader9*					m_dwBasePixelShader;	///<handle to terrain D3D pixel shader
	IDirect3DPixelShader9*					m_dwBaseNoise1PixelShader;	///<handle to terrain/single noise D3D pixel shader
	IDirect3DPixelShader9*					m_dwBaseNoise2PixelShader;	///<handle to terrain/double noise D3D pixel shader
	IDirect3DPixelShader9*					m_dwBase0PixelShader;	///<handle to terrain only pixel shader
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int shutdown() override;			///<release resources used by shader
} flatTerrainShaderPixelShader;

///8 stage terrain shader which only works on certain Nvidia cards.
class TerrainShader8Stage : public W3DShaderInterface
{
	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int init() override;			///<perform any one time initialization and validation
} terrainShader8Stage;

//Offsets into constant register pool used by vertex shader
#define CV_WORLDVIEWPROJ_0	0	//4 vectors for transform of world->clip space.

///Pixel shader based terrain shader - fastest method for the newest cards.
class TerrainShaderPixelShader : public W3DShaderInterface
{
	IDirect3DPixelShader9*					m_dwBasePixelShader;	///<handle to terrain D3D pixel shader
	IDirect3DPixelShader9*					m_dwBaseNoise1PixelShader;	///<handle to terrain/single noise D3D pixel shader
	IDirect3DPixelShader9*					m_dwBaseNoise2PixelShader;	///<handle to terrain/double noise D3D pixel shader

	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Int shutdown() override;			///<release resources used by shader
} terrainShaderPixelShader;

///List of different terrain shader implementations in order of preference
W3DShaderInterface *TerrainShaderList[]=
{
	&terrainShaderPixelShader,
	&terrainShader8Stage,
	&terrainShader2Stage,
	nullptr
};

///List of different terrain shader implementations in order of preference
W3DShaderInterface *FlatTerrainShaderList[]=
{
	&flatTerrainShaderPixelShader,
	&flatTerrainShader2Stage,
	nullptr
};

Int TerrainShader2Stage::init()
{
	//initialize settings for uv animated clouds
	m_xSlidePerSecond = -0.02f;
	m_ySlidePerSecond =  1.50f * m_xSlidePerSecond;
	m_xOffset = 0;
	m_yOffset = 0;

	//no special device validation needed - anything in our min spec should handle this.

	W3DShaders[W3DShaderManager::ST_TERRAIN_BASE]=&terrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE]=2;
	W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=&terrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=3;
	W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=&terrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=3;
	W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=&terrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=3;

	return TRUE;
}

void TerrainShader2Stage::reset()
{
	ShaderClass::Invalidate();

	//Free references to textures
	DX8Wrapper::Set_DX8_Texture(0, nullptr);
	DX8Wrapper::Set_DX8_Texture(1, nullptr);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|0);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|1);
}

void TerrainShader2Stage::updateCloud()
{
	const float frame_time = WW3D::Get_Logic_Frame_Time_Seconds();
	m_xOffset += m_xSlidePerSecond * frame_time;
	m_yOffset += m_ySlidePerSecond * frame_time;

	// This moves offsets towards zero when smaller -1.0 or larger 1.0
	m_xOffset -= (Int)m_xOffset;
	m_yOffset -= (Int)m_yOffset;
}

void TerrainShader2Stage::updateNoise1(D3DXMATRIX *destMatrix,D3DXMATRIX *curViewInverse, Bool doUpdate)
{
	#define STRETCH_FACTOR ((float)(1/(63.0*MAP_XY_FACTOR/2))) /* covers 63/2 tiles */

	D3DXMATRIX scale;

	D3DXMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	*destMatrix = *curViewInverse * scale;

	D3DXMATRIX offset;
	D3DXMatrixTranslation(&offset, m_xOffset, m_yOffset,0);
	*destMatrix *= offset;
}

void TerrainShader2Stage::updateNoise2(D3DXMATRIX *destMatrix,D3DXMATRIX *curViewInverse, Bool doUpdate)
{

	D3DXMATRIX scale;

	D3DXMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	*destMatrix = *curViewInverse * scale;
}

Int TerrainShader2Stage::set(Int pass)
{
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 8 diagnosis: the pixel-shader leg prints its
	// own path, but the fallbacks were silent, so a census with no "[TERRAIN] path=" line could not say
	// what actually drew the ground. One line per 4096 sets names the leg without touching state.
	{
		static unsigned sStage2Sets = 0u;
		if ((sStage2Sets++ & 4095u) == 0u) {
			fprintf(stderr, "[TERRAIN] path=stage2 pass=%d\n", pass);
			fflush(stderr);
		}
	}

	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	DX8Wrapper::Apply_Render_State_Changes();

	// GeneralsX @feature PS3.0-P3-5 14/10/2026 This block used to pin both terrain stages straight
	// from the retail Bilinear/TrilinearTerrainTex INI bools. Both ship FALSE, so the atlas and its
	// noise layer sampled with POINT min/mag/mip whatever the player had chosen in the options menu
	// (the else branch also left stage 1 on LINEAR while forcing stage 0 to POINT - an inconsistency
	// with no visible purpose, dropped here). The bools now act as a floor only; see
	// TextureFilterClass::Apply_Player_Filter, GENERALSX_TERRAIN_FILTER=legacy for the A/B leg.
	{
		const bool bilinearHint=TheGlobalData ? TheGlobalData->m_bilinearTerrainTex!=FALSE : false;
		const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
		TextureFilterClass::Apply_Player_Filter(0,bilinearHint,trilinearHint);
		TextureFilterClass::Apply_Player_Filter(1,bilinearHint,trilinearHint);
	}

	switch (pass)
	{
		case 0:
			// GeneralsX @bugfix Qoder 18/10/2026 Bind through the facade, not the raw device. The Vulkan
			// draw layer takes its textures from DX8Wrapper's shadow (Textures[]), so a device-direct
			// SetTexture left it sampling whatever the previous batch had bound - the terrain atlas never
			// arrived and the ground rendered as a flat untinted surface.
			DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(0));
			DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

			// Modulate the diffuse color with the texture as lighting comes from diffuse.
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);
			break;
		case 1:
			DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(1));
			DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

			// Modulate the diffuse color with the texture as lighting comes from diffuse.
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 1 );
			// Blend the result using the alpha. (came from diffuse mod texture)
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
			// Disable stage 2.
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
			break;
		case 2:
			// Noise/cloud pass
			Matrix4x4 curView;
			DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

			//these states apply to all noise/cloud combination passes
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

			DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
			// Two output coordinates are used.
			DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
			DX8Wrapper::Set_DX8_Sampler_State(0,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
			DX8Wrapper::Set_DX8_Sampler_State(0,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

			//blend into frame buffer
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);

			D3DXMATRIX inv;
			float det;
			D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

			if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_TERRAIN_BASE_NOISE12)
			{
				//setup cloud pass
				DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(2));

				updateNoise1((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
				DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, curView);
				//clouds always need bilinear filtering
				DX8Wrapper::Set_DX8_Sampler_State(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
				DX8Wrapper::Set_DX8_Sampler_State(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

				//setup noise pass
				DX8Wrapper::Set_Texture(1, W3DShaderManager::getShaderTexture(3));

				updateNoise2((D3DXMATRIX*)&curView,&inv);
				DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, curView);
				//noise always needs point/linear filtering.  Why point!?
				DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
				DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
				// Two output coordinates are used.
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

				DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
				DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
			}
			else
			{
				//only 1 noise or cloud texture
				// Now setup the texture pipeline.
				if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_TERRAIN_BASE_NOISE1)
				{
					//setup cloud pass
					DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(2));
					updateNoise1((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
					DX8Wrapper::Set_DX8_Sampler_State(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
					DX8Wrapper::Set_DX8_Sampler_State(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				}
				else
				{
					//setup noise pass
					DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(3));
					updateNoise2((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
					DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
					DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				}

				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
				DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, curView);
			}
			break;
	}

	return TRUE;
}

Int TerrainShader8Stage::init()
{
	ChipsetType res;

	//this shader will also use the 2Stage shader for some of the passes so initialize it too.
	if (terrainShader2Stage.init() && (res=W3DShaderManager::getChipset()) >= DC_TNT && res <= DC_GEFORCE2)
	{
		W3DShaders[W3DShaderManager::ST_TERRAIN_BASE]=&terrainShader8Stage;
		W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE]=1;
		W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=&terrainShader8Stage;
		W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=2;
		W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=&terrainShader8Stage;
		W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=2;
		W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=&terrainShader8Stage;
		W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=2;
		return TRUE;
	}

	return FALSE;
}

Int TerrainShader8Stage::set(Int pass)
{
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 8 diagnosis: names the 8-stage fallback leg,
	// sibling of the stage2 probe above.
	{
		static unsigned sStage8Sets = 0u;
		if ((sStage8Sets++ & 4095u) == 0u) {
			fprintf(stderr, "[TERRAIN] path=stage8 pass=%d\n", pass);
			fflush(stderr);
		}
	}

	if (pass == 0)
	{
		//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
		DX8Wrapper::Apply_Render_State_Changes();

		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

		// GeneralsX @feature PS3.0-P3-5 14/10/2026 Retail Bilinear/TrilinearTerrainTex override
		// replaced by the player's filter tier (floor semantics for the INI bools).
		{
			const bool bilinearHint=TheGlobalData ? TheGlobalData->m_bilinearTerrainTex!=FALSE : false;
			const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
			TextureFilterClass::Apply_Player_Filter(0,bilinearHint,trilinearHint);
			TextureFilterClass::Apply_Player_Filter(1,bilinearHint,trilinearHint);
		}

		DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(0));
		DX8Wrapper::Set_Texture(1, W3DShaderManager::getShaderTexture(1));

		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP, D3DTOP_MODULATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP, D3DTOP_ADD);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 1);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_DIFFUSE | D3DTA_COMPLEMENT | D3DTA_ALPHAREPLICATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_ADD);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TFACTOR | D3DTA_COMPLEMENT);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

		DX8Wrapper::Set_DX8_Texture(2, nullptr);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP, D3DTOP_MODULATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXCOORDINDEX, 2);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLORARG2, D3DTA_TEXTURE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

		DX8Wrapper::Set_DX8_Texture(3, nullptr);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXCOORDINDEX, 3);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLORARG1, D3DTA_DIFFUSE | 0 | D3DTA_ALPHAREPLICATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

		DX8Wrapper::Set_DX8_Texture(4, nullptr);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLOROP, D3DTOP_MODULATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_TEXCOORDINDEX, 4);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLORARG1, D3DTA_CURRENT);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAARG1, D3DTA_CURRENT);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

		DX8Wrapper::Set_DX8_Texture(5, nullptr);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLOROP, D3DTOP_ADD);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_TEXCOORDINDEX, 5);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAOP,   D3DTOP_ADD);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAARG1, D3DTA_TFACTOR | D3DTA_COMPLEMENT);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 5, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

		DX8Wrapper::Set_DX8_Texture(6, nullptr);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLOROP, D3DTOP_MODULATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_TEXCOORDINDEX, 6);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLORARG1, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_COLORARG2, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAOP,   D3DTOP_MODULATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 6, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);

		DX8Wrapper::Set_DX8_Texture(7, nullptr);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_TEXCOORDINDEX, 7);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLORARG1, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_COLORARG2, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 7, D3DTSS_ALPHAARG2, D3DTA_TFACTOR);
	}
	else
	{
		//setup cloud noise/pass
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLOROP, D3DTOP_DISABLE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		DX8Wrapper::Invalidate_Cached_Render_States();

		terrainShader2Stage.set(2);
	}
	return TRUE;
}

void TerrainShader8Stage::reset()
{
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP, D3DTOP_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_COLOROP, D3DTOP_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_COLOROP, D3DTOP_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 4, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

	DX8Wrapper::Set_DX8_Texture(0, nullptr);
	DX8Wrapper::Set_DX8_Texture(1, nullptr);
	DX8Wrapper::Invalidate_Cached_Render_States();
}

Int TerrainShaderPixelShader::shutdown()
{
	if (m_dwBasePixelShader)
		m_dwBasePixelShader->Release();

	if (m_dwBaseNoise1PixelShader)
		m_dwBaseNoise1PixelShader->Release();

	if (m_dwBaseNoise2PixelShader)
		m_dwBaseNoise2PixelShader->Release();

	m_dwBasePixelShader = nullptr;
	m_dwBaseNoise1PixelShader = nullptr;
	m_dwBaseNoise2PixelShader = nullptr;

	return TRUE;
}

// GeneralsX @feature Storm996 17/10/2026 PS3.0 haze user switch: the live haze setting, file-scope.
// The recipe below used to parse GENERALSX_GNFOG into a function-static on first call and never look
// again, which made the knob a startup-only QA leg: a player who found the distance wash too strong
// had no way to turn it down except by launching the game with an environment variable. These two
// hold the value the options page owns. The seed keeps every existing contract - the env still wins
// over Options.ini, and with neither set the shipped state is ON at 0.6 (what P3-14 delivered).
// -1 is the "not seeded yet" sentinel; the setters always write a clamped value, so once any of the
// three paths has run the sentinel is gone for good.
static Real s_hazeStrength = -1.0f;
static Bool s_hazeEnabled = TRUE;

static void seedDistanceHaze()
{
	if (s_hazeStrength >= 0.0f)
		return;
	// OptionPreferences owns the precedence: GENERALSX_GNFOG wins, then the DistanceHaze* keys.
	s_hazeEnabled = OptionPreferences().getDistanceHazeEnabled();
	s_hazeStrength = (Real)OptionPreferences().getDistanceHazeIntensity() * 0.01f;
	fprintf(stderr, "[HAZE] seeded enabled=%d strength=%.2f (Options.ini / GENERALSX_GNFOG)\n",
		(int)s_hazeEnabled, (double)s_hazeStrength);
	fflush(stderr);
}

void W3DShaderManager::setDistanceHaze(Bool enabled, Real strength01)
{
	if (strength01 < 0.0f) strength01 = 0.0f;
	else if (strength01 > 1.0f) strength01 = 1.0f;
	s_hazeEnabled = enabled;
	s_hazeStrength = strength01;

	// Re-push the ramp now: the GN/dot3 families only see Set_Fog when something pushes it, and the
	// install path runs a handful of times per session. Terrain, road and tree batches recompute the
	// ramp themselves per batch, so they need nothing.
	W3DShaderManager::applyWeatherToShaders();
	fprintf(stderr, "[HAZE] user set enabled=%d strength=%.2f\n", (int)enabled, (double)strength01);
	fflush(stderr);
}

Bool W3DShaderManager::isDistanceHazeEnabled()
{
	seedDistanceHaze();
	return s_hazeEnabled;
}

Real W3DShaderManager::getDistanceHazeStrength()
{
	seedDistanceHaze();
	return s_hazeStrength;
}

//-------------------------------------------------------------------------------------------------
// GeneralsX @feature Qoder 18/10/2026 PS3.0 live lighting knobs: the bloom composite gain and the sun
// shadow map darkening strength. Both used to be latched into a function-local static the first time
// their pass ran, so the two Graphics Options rows were start-up only. The seeds keep every existing
// contract (OptionPreferences resolves GENERALSX_BLOOM_INTENSITY / GENERALSX_SHADOWMAP_INT before
// Options.ini, and both factory defaults are untouched); a menu push is ignored while its env leg is
// present, so a scripted parity run can never be moved by the page. -1 is the "not seeded yet"
// sentinel, which is why a legal 0 parks the effect instead of re-reading the ini.
//-------------------------------------------------------------------------------------------------
static Real s_bloomIntensityLive = -1.0f;
static Real s_shadowStrengthLive = -1.0f;

static Bool lightingEnvPresent(const char* name)
{
	const char* ev = getenv(name);
	return ev != nullptr && *ev != 0;
}

static Real clamp01(Real value)
{
	if (value < 0.0f)
		return 0.0f;
	if (value > 1.0f)
		return 1.0f;
	return value;
}

void W3DShaderManager::setBloomIntensity(Real intensity01)
{
	// GeneralsX @feature Qoder 20/10/2026: the env override still keeps the file value out, but the
	// Vulkan knob is pushed either way - the getter is the single authority on what a frame will use.
	if (!lightingEnvPresent("GENERALSX_BLOOM_INTENSITY"))
		s_bloomIntensityLive = clamp01(intensity01);
	const Real live = getBloomIntensity();
	DX8Wrapper::VK_Set_Post_Knobs(live, DX8Wrapper::VK_Post_AA());
	fprintf(stderr, "[BLOOM3] user set intensity=%.2f\n", (double)live);
	fflush(stderr);
}

// GeneralsX @feature Qoder 20/10/2026: the anti-aliasing row of the graphics page. On the D3D path it can
// only take effect on the next device creation, but the pure-Vulkan present pass resolves it live, so the
// same slider/tick that names the level now switches the FXAA term of that pass on or off.
void W3DShaderManager::setPostAntiAliasing(Bool enabled)
{
	DX8Wrapper::VK_Set_Post_Knobs(getBloomIntensity(), enabled != FALSE);
	fprintf(stderr, "[VKPOST] anti-aliasing knob -> %s\n", enabled != FALSE ? "on" : "off");
	fflush(stderr);
}

Real W3DShaderManager::getBloomIntensity()
{
	if (s_bloomIntensityLive < 0.0f)
		s_bloomIntensityLive = OptionPreferences().getBloomIntensity();
	return s_bloomIntensityLive;
}

void W3DShaderManager::setShadowMapStrength(Real strength01)
{
	if (lightingEnvPresent("GENERALSX_SHADOWMAP_INT"))
		return;
	s_shadowStrengthLive = clamp01(strength01);
	fprintf(stderr, "[SMAP] user set strength=%.2f\n", (double)s_shadowStrengthLive);
	fflush(stderr);
}

Real W3DShaderManager::getShadowMapStrength()
{
	if (s_shadowStrengthLive < 0.0f)
		s_shadowStrengthLive = (Real)OptionPreferences().getShadowMapIntensity() * 0.01f;
	// GeneralsX @debug Qoder 19/10/2026 exaggerated-shadow QA knob: GENERALSX_SMAP_X=<mult> multiplies
	// the shadow term so a suspect-but-invisible shadow becomes visible to the eye (mult 3 with the 5x5
	// PCF saturates any pixel with one third of tap coverage to black). Unset = shipped behavior.
	const char* xm = getenv("GENERALSX_SMAP_X");
	if (xm != nullptr && *xm != 0)
		return s_shadowStrengthLive * (Real)atof(xm);
	return s_shadowStrengthLive;
}

// GeneralsX @feature PS3.0 Weather W5 16/10/2026: the haze ramp recipe in one place.
// start = 45%, length = 50% of the normal-zoom far clip, cold grey-blue haze, haze strength knob
// (PS3.0-P3-14, now user-owned - see seedDistanceHaze above). fogScale is the weather's coupling: a
// blizzard's 2.4 pulls the ramp in to 19%/21% of the far clip so the horizon closes down, and lifts
// the strength a little. scale 1.0 reproduces the install numbers exactly, which is what keeps a
// clear map byte-identical.
// Shared by the GN-family install below, the terrain/road/TREE30 c0/c1 upload and the weather refresh,
// because three copies of one knob-reading recipe is how a haze ramp drifts.
static void Build_Weather_Fog_Ramp(Real fogScale, Vector4 &colorStrength, Vector4 &ramp)
{
	// Read live: this runs per terrain batch, and two struct reads with the value already resolved
	// is cheap enough that the effect can be re-tuned mid-match (options page) instead of at launch.
	seedDistanceHaze();
	const float s_fogStrength = (s_hazeEnabled != FALSE) ? (float)s_hazeStrength : 0.0f;

	// Range covers everything the recipes publish (1.0 clear .. 2.4 blizzard) with room for an INI
	// author to write more; the clamp only keeps the ramp from collapsing to zero length.
	if (fogScale < 0.25f) fogScale = 0.25f;
	else if (fogScale > 8.0f) fogScale = 8.0f;

	// Far plane follows the W3DView::doCameraUpdate normal-zoom formula (NORMAL_DRAW_WIDTH *
	// MAP_XY_FACTOR); the live per-frame farZ varies with scroll edge and real-zoom pitch, and the
	// steady-state value is the right ramp anchor.
	const Real farClip = WorldHeightMap::NORMAL_DRAW_WIDTH * MAP_XY_FACTOR;
	Real fogStart = farClip * 0.45f / fogScale;
	const Real fogLen = farClip * 0.5f / fogScale;
	if (fogStart < 1.0f) fogStart = 1.0f;
	// Thicker weather also lifts the blend a touch: 0.85 + 0.15*scale is exactly 1.0 at scale 1.
	float strength = (float)(s_fogStrength * (0.85 + 0.15 * fogScale));
	if (strength > 1.0f) strength = 1.0f;

	colorStrength.Set(0.72f, 0.78f, 0.84f, strength);
	ramp.Set(fogStart, 1.0f / (fogLen > 1.0f ? fogLen : 1.0f), 0.0f, 0.0f);
}

// GeneralsX @feature Storm996 17/10/2026 PS3.0 haze coverage: the fixed-function half of the horizon.
// The custom ps_3_0 families blend toward the haze colour themselves, but they only take batches that
// clear the mount gates (dx8renderer.cpp:2065 / :2123 - single pass, opaque, plain UVs, pure
// directional light, no skin, no material override). Everything they decline - skinned meshes,
// two-pass composites, vertex-coloured props, lamp/spot-lit buildings, and terrain while the ps_3_0
// terrain leg is off - draws on the fixed pipeline, which knows nothing about c25/c26 and therefore
// stayed crisp. That is the "only SOME distant buildings grey out" report: a coverage hole, not a
// look. D3D's own linear fog is the fixed pipeline's only distance blend, so these two calls feed it
// from the same recipe around the 3D scene and the horizon becomes one continuous band.
// The state is per-device, and programmable pixel shaders ignore D3DRS_FOGENABLE entirely (fog is
// their own arithmetic), so a batch can never be hazed twice: mounted families answer to the shader
// constants, everything else to these render states.
static Bool s_ffHazeOn = FALSE;

void W3DShaderManager::beginFixedFunctionHaze()
{
	Vector4 haze, ramp;
	Build_Weather_Fog_Ramp(weatherRenderState().m_fogScale, haze, ramp);
	const float strength = haze.W;
	if (strength <= 0.0f)
	{
		// Switch off (or strength 0): leave the shipped fixed-function state untouched, which is fog
		// disabled - every pixel stays exactly where the no-haze build puts it.
		if (s_ffHazeOn != FALSE)
			endFixedFunctionHaze();
		return;
	}

	const float start = ramp.X;
	const float len = 1.0f / ramp.Y;
	// D3D's linear fog has no blend scalar, only a ramp that ends at full fog. Folding the strength
	// into the ramp length reproduces the shader recipe inside the band it actually works in:
	// t_ff(z) = (z-start)/(len/strength) = (z-start)/len*strength = t_shader(z) for z <= start+len.
	// Past the band the shader holds at `strength` while the ramp keeps creeping; at the far clip
	// (start = 45%, len = 50% of it) that overshoot is at most 0.1*strength, and at strength 1.0 both
	// saturate on the same pixel. Close enough to read as one horizon, and never a second look.
	const float end = start + len / strength;

	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGCOLOR,
		DX8Wrapper::Convert_Color(Vector3(haze.X, haze.Y, haze.Z), 0.0f));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGSTART, (unsigned)WWMath::Float_As_Int(start));
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGEND, (unsigned)WWMath::Float_As_Int(end));
	// Table (per-pixel, eye depth) rather than vertex: the shader blend is per-pixel too, and vertex
	// fog would band across the big terrain quads. W3D's init leaves FOGVERTEXMODE at LINEAR, so the
	// explicit NONE keeps the two terms from adding.
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_LINEAR);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE, TRUE);
	s_ffHazeOn = TRUE;

	// Forensics marker, once per session: the ramp the fixed pipeline actually got, so a screenshot can
	// be attributed to these numbers instead of to a guess. Per-frame would flood stderr.
	static Bool s_ffHazeLogged = FALSE;
	if (s_ffHazeLogged == FALSE)
	{
		s_ffHazeLogged = TRUE;
		fprintf(stderr, "[FFHAZE] armed strength=%.2f start=%.0f end=%.0f (table linear, FF batches only)\n",
			(double)strength, (double)start, (double)end);
		fflush(stderr);
	}
}

void W3DShaderManager::endFixedFunctionHaze()
{
	if (s_ffHazeOn == FALSE)
		return;
	// Back to the state every other draw in the frame assumes (WW3D resets FOGENABLE to FALSE at
	// Apply_Default_State, and the 2D scene, UI, weather screen veil and bloom passes all rely on it).
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	s_ffHazeOn = FALSE;
}

// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: shared c0/c1 upload for the ps_3_0 terrain and
// road pixel shaders. Same ramp recipe as the GN [GNFOG] block (start 45% / length 50% of the
// normal-zoom far clip, cold grey-blue haze, GENERALSX_GNFOG strength knob), kept separate from
// the GN constants (c25/c26) so the two families cannot stomp on each other's registers.
static void Upload_Terrain30_Fog_Constants(void)
{
	// GeneralsX @feature PS3.0 Weather W5: rebuilt from the published weather scale every call instead
	// of being frozen at first use, so a script-driven weather switch moves the horizon haze on the
	// same frame the lighting and the screen layer do. One builder call per terrain batch is arithmetic
	// on a struct read; no allocation and no new plumbing.
	Vector4 haze, ramp;
	Build_Weather_Fog_Ramp(weatherRenderState().m_fogScale, haze, ramp);
	DX8Wrapper::Set_Pixel_Shader_Constant(0, (const float*)&haze, 1);
	DX8Wrapper::Set_Pixel_Shader_Constant(1, (const float*)&ramp, 1);

	// GeneralsX @feature Qoder 22/10/2026 light shadows: c6..c9 = world -> light clip rows (row-vector,
	// D3D), c10 = light xyz + range with .w = 1 when this frame built the strongest light's map. The
	// receiver applies it to list index 0 only, so a missing map is one disabled uniform, not a branch.
	float lsVP[16], lsPR[4];
	if (DX8Wrapper::VK_LightShadow_Info(lsVP, lsPR))
	{
		DX8Wrapper::Set_Pixel_Shader_Constant(6, lsVP, 4);
		Vector4 pr(lsPR[0], lsPR[1], lsPR[2], 1.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(10, (const float*)&pr, 1);
	}
	else
	{
		Vector4 off(0.0f, 0.0f, 0.0f, 0.0f);
		DX8Wrapper::Set_Pixel_Shader_Constant(10, (const float*)&off, 1);
	}
}

// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: TRUE once the ps_3_0 terrain trio below has
// replaced the shipped .pso handles. set() then wires the stage-2 CAMERASPACEPOSITION COUNT3
// depth relay and uploads the fog ramp constants; the flag is never reset (the handles live for
// the process lifetime, same as the .pso pair they replace).
static Bool s_terrainHlslFog = FALSE;

// GeneralsX @feature PS3.0 L2.1 TNM 17/10/2026: the terrain normal pass switches.
// GENERALSX_TERRAIN_NM defaults ON but only ever matters while s_terrainHlslFog is TRUE (the ps_3_0
// terrain trio), so the retail fixed-function path and every TERRAIN30=0 run are untouched by it.
// GENERALSX_TERRAIN_NM_GAIN scales the tangent-space xy deviation (0 = exact no-op, 2 = embossed).
// s_nmFlatFallback is a 1x1 flat-normal texture created on first use so the NM-variant shaders can
// never sample a stage-4 that the atlas loader left unbound: white would fold the modulation, flat
// keeps it at exactly 1.0. Read once at shader init; changing it at runtime is not supported.
static Bool s_terrainNMEnv = TRUE;
// < 0 means "follow the options-menu NormalMapStrength slider"; >= 0 is a QA override
// from GENERALSX_TERRAIN_NM_GAIN.
static Real s_terrainNMGain = -1.0f;
static IDirect3DTexture9 *s_nmFlatFallback = nullptr;

// GeneralsX @feature Qoder 17/10/2026 PS3.0 T0 P4: TRUE only while the derivative macro-normal (PPL)
// trio is the installed trio (TerrainShaderPixelShader::init decides once, child of terrain30On).
// set() consults it per pass to upload the reconstruction matrix Kmat = T3^T*inv3 (PS c3..c5) built
// from the REAL stage-2 texture transform the device is about to use for the TEXCOORD2 relay.
static Bool s_terrainPPL = FALSE;

Bool W3DShaderManager::isTerrainNormalEnabled()
{
	if (!s_terrainHlslFog) return FALSE;
	// GeneralsX @feature Qoder 21/10/2026: the terrain NM layer now follows the same
	// NormalMapping option as the model/dot3 path (DX8NormalMap::Is_Enabled is updated live by
	// the options menu). GENERALSX_TERRAIN_NM remains a QA override. Missing *_nm.tga sheets
	// still fall back to flat normals per texture class, so detection stays asset-driven.
	const char* nenv = getenv("GENERALSX_TERRAIN_NM");
	if (nenv != nullptr && *nenv != 0) return (atoi(nenv) != 0) ? TRUE : FALSE;
	return DX8NormalMap::Is_Enabled() ? TRUE : FALSE;
}

// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: same contract for the road dual-noise HLSL swap.
static Bool s_roadHlslFog = FALSE;

// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: public bridge so the W3DTreeBuffer TREE30 path can
// reuse the exact haze ramp the terrain/road HLSL shaders upload, instead of duplicating the recipe.
void W3DShaderManager::uploadPerPixelFogConstants()
{
	Upload_Terrain30_Fog_Constants();
}

// GeneralsX @feature PS3.0 Weather W5 16/10/2026: the shader half of the weather coupling.
// Two values travel into the WW3D2 dot3/PBR layer, which cannot read game-side weather state itself
// (platform-isolation rule, same as the Set_Fog injection at the install below):
//   - the weather-scaled haze ramp for the GN per-pixel family (c25/c26). The shader install only
//     ever runs once, so without this refresh a script-driven storm would move the terrain, road and
//     tree haze but leave every normal-mapped unit at clear-sky fog;
//   - the rain wetness, which the PBR pixel shaders read out of the previously-unused c8.w / c11.w slot
//     and turn into slicker roughness. 0 keeps every pixel numerically identical.
// Both are derived device-side from the recipe, so neither can reach the replay CRC (Golden Rule 7).
void W3DShaderManager::applyWeatherToShaders()
{
	const WeatherRenderState &wx = weatherRenderState();
	Vector4 haze, ramp;
	Build_Weather_Fog_Ramp(wx.m_fogScale, haze, ramp);
	DX8NormalMap::Set_Fog(haze, ramp);
	DX8NormalMap::Set_Wetness(wx.m_wetness);

	// GeneralsX @bugfix PS3.0 Weather W5 16/10/2026: change-detector rather than the previous 1-in-256
	// tick - see the matching note in W3DDisplay::setTimeOfDay(). The lighting install runs a handful of
	// times per session, so a counter throttle only ever printed the state at load (fogScale=1.00 wet=0.00
	// in every weather leg) and could not show that the fog ramp and the wetness slot moved with the
	// recipe. Now: one line per distinct (type, fog, wet), plus one when it returns to neutral.
	static Int s_lastShaderType = -999;
	static Real s_lastShaderFog = -1.0f, s_lastShaderWet = -1.0f;
	// Plain signed comparisons instead of fabsf: this file has no math.h of its own and the fixed-function
	// legs build in more restricted translation units.
	const Real dFog = wx.m_fogScale - s_lastShaderFog;
	const Real dWet = wx.m_wetness - s_lastShaderWet;
	if ((Int)wx.m_type != s_lastShaderType
		|| dFog > 0.001f || dFog < -0.001f
		|| dWet > 0.001f || dWet < -0.001f)
	{
		fprintf(stderr, "[WXSHADER] type=%d fogScale=%.2f start=%.0f invLen=%.4f strength=%.2f wet=%.2f\n",
			(Int)wx.m_type, wx.m_fogScale, (double)ramp.X, (double)ramp.Y, (double)haze.W, wx.m_wetness);
		fflush(stderr);
		s_lastShaderType = (Int)wx.m_type;
		s_lastShaderFog = wx.m_fogScale;
		s_lastShaderWet = wx.m_wetness;
	}
}

// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: TRUE once the ps_3_0 terrain trio is installed, which
// also means the shared fog ramp is valid. The tree path gates its own HLSL haze on this so a device
// that never passed the SM3 terrain gate never sees a fog-only tree shader with stale constants.
Bool W3DShaderManager::arePerPixelFogShadersActive()
{
	return s_terrainHlslFog;
}

Int TerrainShaderPixelShader::init()
{
	Int res;
#ifdef DISABLE_PIXEL_SHADERS
	return false;
#endif
	// GeneralsX @bugfix Qoder 18/10/2026: init also runs after a device reset, and shutdown() does
	// not touch the file-scope HLSL switches. Clearing them here keeps set() from taking the ps_3_0
	// relay/constant path with handles that this pass may load from the retail .pso trio instead.
	s_terrainHlslFog = FALSE;
	s_terrainPPL = FALSE;
	//this shader will also use the 2Stage shader for some of the passes so initialize it too.
	if (terrainShader2Stage.init() && (res=W3DShaderManager::getChipset()) >= DC_GENERIC_PIXEL_SHADER_1_1)
	{
		if (res >= DC_GENERIC_PIXEL_SHADER_1_1)
		{
			//this shader needs some assets that need to be loaded
			//shader decleration
			D3DVERTEXELEMENT9 Declaration[]=
			{
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 0), // Position
				D3DDECL_USAGE(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 12), // Diffuse
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, 0, 16), //  Texture Coordinates
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, 1, 24), //  Texture Coordinates
				D3DDECL_END()
			};

			// GeneralsX @bugfix BenderAI 13/02/2026 - Graceful fallback when pixel shader files don't exist
			// Try to load pixel shaders, but don't fail if files are missing - just use 2-stage fallback
			Bool pixelShadersAvailable = TRUE;

			//base version which doesn't apply any noise textures.
			HRESULT hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\terrain.pso", &Declaration[0], 0, false, (void**)&m_dwBasePixelShader);
			if (FAILED(hr))
			{
				DEBUG_LOG(("W3DShaderManager: terrain.pso not found, using 2-stage fallback"));
				pixelShadersAvailable = FALSE;
			}

			//version which blends 1 noise texture.
			if (pixelShadersAvailable)
			{
				hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\terrainnoise.pso", &Declaration[0], 0, false, (void**)&m_dwBaseNoise1PixelShader);
				if (FAILED(hr))
				{
					DEBUG_LOG(("W3DShaderManager: terrainnoise.pso not found, using 2-stage fallback"));
					pixelShadersAvailable = FALSE;
				}
			}

			//version which blends 2 noise textures.
			if (pixelShadersAvailable)
			{
				hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\terrainnoise2.pso", &Declaration[0], 0, false, (void**)&m_dwBaseNoise2PixelShader);
				if (FAILED(hr))
				{
					DEBUG_LOG(("W3DShaderManager: terrainnoise2.pso not found, using 2-stage fallback"));
					pixelShadersAvailable = FALSE;
				}
			}

			// Only set pixel shader mode if ALL shaders loaded successfully
			// GeneralsX @bugfix Qoder 18/10/2026: the ps_3_0 HLSL trio no longer lives inside the
			// retail .pso gate. A Zero Hour install ships only the "f" family in ShadersZH.big, so
			// terrain.pso/terrainnoise*.pso are legitimately absent there and the experimental
			// Terrain30Shading path could never install. Retail .pso and HLSL are now two independent
			// sources for the same three handles; either one is enough to register this shader.
			Bool terrainPixelActive = pixelShadersAvailable;
			Bool terrainHlslInstalled = FALSE;

				// GeneralsX @feature PS3.0 batch8 G1 14/10/2026 ps_3_0 terrain trio with per-pixel haze.
				// The shipped .pso files are ps_1_1 ramps that cannot read eye depth, so their exact math
				// (mirrored from the D3DDisassemble dumps in logs/terrain_disasm_clean.txt) is re-emitted
				// in HLSL, and the stage-2 CAMERASPACEPOSITION generator is widened to COUNT3 with its
				// texture matrix column 2 pinned to (0,0,1,0) so TEXCOORD2.z carries camera-space depth
				// into the PS: the POSITION-as-input trick the GN fog used is rejected by fxc (X4502,
				// offline probe on both stacks 14/10) and VPOS has no z/w (X5631), which leaves this FF
				// relay as the only per-pixel depth channel for fixed-function geometry. All three
				// compiles must succeed to swap; any failure keeps the retail .pso behaviour untouched.
				// GENERALSX_TERRAIN30=0 opts out; SM3 tier hardware is the other gate (BWFILTER pattern).
				// GeneralsX @bugfix PS3.0 batch23 16/10/2026: two mirror defects found by reading the
				// retail disasm against the DX9 ISA docs. (1) Operand order: MS Learn lrp---ps states
				// "lrp dst, src0, src1, src2 -> dst = src0*src1 + (1-src0)*src2", so retail
				// "lrp r0, v0.w, t1, t0" means weight*v0.w*t1 + (1-v0.w)*t0 = lerp(t0, t1, vc.a) =
				// lerp(base, det, a). Batch8 wrote lerp(det, base, a), swapping base/detail wherever the
				// per-vertex blend weight was not 0.5 - the exact shape of the batch22 "square grey
				// patches" verdict (vertex alpha is the blend weight, quantised on the terrain patch
				// grid). (2) Fog sign: the COUNT3 relay passes camera-space z, negative in front of the
				// camera (D3D -Z convention), while c1.x is a positive distance, so the batch8 ramp
				// saturate((negative - positive) * positive) folded to zero - the haze was silently a
				// no-op, and every batch22 symptom came from defect (1). Fixed by ramping on -dep.z.
				// "// d3d-target" below is an HLSL comment, ignored by fxc; it lets the verbatim-extract
				// (extract_hlsl.py) feed the pixel probe for the <=1 LSB parity gate.
				static const char* kTerrainPS30Base =
					"// d3d-target: ps_3_0\n"
					"sampler s0 : register(s0);\n"                 // atlas (stage0, uv0, clamped)
					"sampler s1 : register(s1);\n"                 // detail (stage1, uv1, clamped)
					"float4 c0 : register(c0);\n"                  // haze rgb, w = fog strength
					"float4 c1 : register(c1);\n"                  // x = eye-depth start, y = 1/ramp length
					"struct PS_IN\n"
					"{\n"
					"    float2 uv0 : TEXCOORD0;\n"
					"    float2 uv1 : TEXCOORD1;\n"
					"    float3 dep : TEXCOORD2;\n"                // .z = camera depth (COUNT3 relay)
					"    float4 vc  : COLOR0;\n"                   // vertex diffuse (blend factor in .a)
					"};\n"
					"float4 main(PS_IN i) : COLOR\n"
					"{\n"
					"    float4 base = tex2D(s0, i.uv0);\n"
					"    float4 det  = tex2D(s1, i.uv1);\n"
					"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"   // terrain.pso: lrp r0,v0.w,t1,t0; mul r0,v0
					"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
					"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
					"}\n";
				static const char* kTerrainPS30Noise =
					"// d3d-target: ps_3_0\n"
					"sampler s0 : register(s0);\n"
					"sampler s1 : register(s1);\n"
					"sampler s2 : register(s2);\n"                 // noise / lightmap (stage2, dep.xy)
					"float4 c0 : register(c0);\n"
					"float4 c1 : register(c1);\n"
					"struct PS_IN\n"
					"{\n"
					"    float2 uv0 : TEXCOORD0;\n"
					"    float2 uv1 : TEXCOORD1;\n"
					"    float3 dep : TEXCOORD2;\n"                // .xy = projected noise uv, .z = camera depth
					"    float4 vc  : COLOR0;\n"
					"};\n"
					"float4 main(PS_IN i) : COLOR\n"
					"{\n"
					"    float4 base = tex2D(s0, i.uv0);\n"
					"    float4 det  = tex2D(s1, i.uv1);\n"
					"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"
					"    c = c * tex2D(s2, i.dep.xy);\n"               // terrainnoise.pso: mul r0,r0,t2
					"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
					"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
					"}\n";
				static const char* kTerrainPS30Noise12 =
					"// d3d-target: ps_3_0\n"
					"sampler s0 : register(s0);\n"
					"sampler s1 : register(s1);\n"
					"sampler s2 : register(s2);\n"                 // cloud noise (stage2, dep.xy)
					"sampler s3 : register(s3);\n"                 // light map (stage3, uv3)
					"float4 c0 : register(c0);\n"
					"float4 c1 : register(c1);\n"
					"struct PS_IN\n"
					"{\n"
					"    float2 uv0 : TEXCOORD0;\n"
					"    float2 uv1 : TEXCOORD1;\n"
					"    float3 dep : TEXCOORD2;\n"                // .xy = projected noise uv, .z = camera depth
					"    float2 uv3 : TEXCOORD3;\n"
					"    float4 vc  : COLOR0;\n"
					"};\n"
					"float4 main(PS_IN i) : COLOR\n"
					"{\n"
					"    float4 base = tex2D(s0, i.uv0);\n"
					"    float4 det  = tex2D(s1, i.uv1);\n"
					"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"
					"    c = c * tex2D(s2, i.dep.xy);\n"               // terrainnoise2.pso: mul r0,t2; mul r0,t3
					"    c = c * tex2D(s3, i.uv3);\n"
					"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
					"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
					"}\n";

					// GeneralsX @feature PS3.0 L2.1 TNM 17/10/2026: normal-mapped variants of the trio.
					// Identical to the plain versions above plus one sampler (s4 = the packed normal
					// atlas, sampled with the very uv0 the atlas layer already uses, so tile gutters and
					// mip chain line up by construction) and one constant (c2 = normalized sun direction
					// in .xyz, xy-deviation gain in .w). The modulation is a ratio of lambert terms:
					//   c.rgb *= saturate(dot(normalize(n), L)) / max(L.z, 0.05)
					// with n sampled around the flat tangent normal (0,0,1). A flat texel reproduces
					// the vertex-baked brightness exactly (ratio 1.0), and every deviation embosses the
					// already-shaded pixel instead of double-lighting it. The world-up approximation is
					// deliberate: the terrain vertex stream (VertexFormatXYZDUV2) carries no normal or
					// tangent, and slopes keep their macro shading in vc.rgb - s4 only adds the tile's
					// micro relief. Modulation lands before the cloud/lightmap multiplications, same
					// order as real lighting.
					static const char* kTerrainPS30BaseNM =
						"// d3d-target: ps_3_0\n"
						"sampler s0 : register(s0);\n"
						"sampler s1 : register(s1);\n"
						"sampler s4 : register(s4);\n"                 // normal atlas (stage4, uv0)
						"float4 c0 : register(c0);\n"
						"float4 c1 : register(c1);\n"
						"float4 c2 : register(c2);\n"                  // xyz = sun dir (normalized), w = deviation gain
						"struct PS_IN\n"
						"{\n"
						"    float2 uv0 : TEXCOORD0;\n"
						"    float2 uv1 : TEXCOORD1;\n"
						"    float3 dep : TEXCOORD2;\n"
						"    float4 vc  : COLOR0;\n"
						"};\n"
						"float4 main(PS_IN i) : COLOR\n"
						"{\n"
						"    float4 base = tex2D(s0, i.uv0);\n"
						"    float4 det  = tex2D(s1, i.uv1);\n"
						"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"
						"    float3 n = tex2D(s4, i.uv0).xyz * 2.0 - 1.0;\n"
						"    n.xy *= c2.w;\n"
						"    n = normalize(n);\n"
						"    c.rgb *= saturate(dot(n, c2.xyz)) / max(c2.z, 0.05);\n"
						"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
						"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
						"}\n";
					static const char* kTerrainPS30NoiseNM =
						"// d3d-target: ps_3_0\n"
						"sampler s0 : register(s0);\n"
						"sampler s1 : register(s1);\n"
						"sampler s2 : register(s2);\n"
						"sampler s4 : register(s4);\n"
						"float4 c0 : register(c0);\n"
						"float4 c1 : register(c1);\n"
						"float4 c2 : register(c2);\n"
						"struct PS_IN\n"
						"{\n"
						"    float2 uv0 : TEXCOORD0;\n"
						"    float2 uv1 : TEXCOORD1;\n"
						"    float3 dep : TEXCOORD2;\n"
						"    float4 vc  : COLOR0;\n"
						"};\n"
						"float4 main(PS_IN i) : COLOR\n"
						"{\n"
						"    float4 base = tex2D(s0, i.uv0);\n"
						"    float4 det  = tex2D(s1, i.uv1);\n"
						"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"
						"    float3 n = tex2D(s4, i.uv0).xyz * 2.0 - 1.0;\n"
						"    n.xy *= c2.w;\n"
						"    n = normalize(n);\n"
						"    c.rgb *= saturate(dot(n, c2.xyz)) / max(c2.z, 0.05);\n"
						"    c = c * tex2D(s2, i.dep.xy);\n"
						"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
						"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
						"}\n";
					static const char* kTerrainPS30Noise12NM =
						"// d3d-target: ps_3_0\n"
						"sampler s0 : register(s0);\n"
						"sampler s1 : register(s1);\n"
						"sampler s2 : register(s2);\n"
						"sampler s3 : register(s3);\n"
						"sampler s4 : register(s4);\n"
						"float4 c0 : register(c0);\n"
						"float4 c1 : register(c1);\n"
						"float4 c2 : register(c2);\n"
						"struct PS_IN\n"
						"{\n"
						"    float2 uv0 : TEXCOORD0;\n"
						"    float2 uv1 : TEXCOORD1;\n"
						"    float3 dep : TEXCOORD2;\n"
						"    float2 uv3 : TEXCOORD3;\n"
						"    float4 vc  : COLOR0;\n"
						"};\n"
						"float4 main(PS_IN i) : COLOR\n"
						"{\n"
						"    float4 base = tex2D(s0, i.uv0);\n"
						"    float4 det  = tex2D(s1, i.uv1);\n"
						"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"
						"    float3 n = tex2D(s4, i.uv0).xyz * 2.0 - 1.0;\n"
						"    n.xy *= c2.w;\n"
						"    n = normalize(n);\n"
						"    c.rgb *= saturate(dot(n, c2.xyz)) / max(c2.z, 0.05);\n"
						"    c = c * tex2D(s2, i.dep.xy);\n"
						"    c = c * tex2D(s3, i.uv3);\n"
						"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
						"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
						"}\n";

					// GeneralsX @feature Qoder 17/10/2026 PS3.0 T0 P4: derivative macro-normal variants of
					// the NM trio (逐像素光照总闸 施工调研 §4-P4, "采样/几何重建路线" inside the B5 slot).
					// Terrain vertices carry no normal channel (VertexFormatXYZDUV2), so instead of inventing
					// one the face normal is RECONSTRUCTED per pixel from screen-space derivatives of the
					// existing TEXCOORD2 relay: that relay is the fixed-function CAMERASPACEPOSITION texture
					// transform through the stage-2 matrix T (COUNT3, third column pinned to (0,0,1,0) by
					// batch8 G1), i.e. dep = p_cam * T is AFFINE in the camera-space position p_cam. The three
					// interpolated scalars of dep therefore map the triangle's screen tangents through T's
					// top-left 3x3, and the row-vector identity (uM)x(vM) = det(M)(uxv)M^-T gives
					//     w = cross(ddx(dep), ddy(dep))  =>  n_world(row) ∝ w * Kmat,  Kmat = T3^T * V3^T
					// where T3 is the 3x3 actually loaded into D3DTS_TEXTURE2 this pass (read back from the
					// device below, so noise projection / lightmap projection / the batch8 identity all use
					// their own true matrix) and V3 the D3DTS_VIEW rotation. VIEW is rigid (orthonormal), so
					// V3^T = V3^-1 = the inv the block already computes - the CPU builds Kmat = T3^T * inv3,
					// uploads its three rows as c3/c4/c5 and the PS forms w * Kmat as w.x*c3 + w.y*c4 +
					// w.z*c5. The cross product fixes the plane but not the hemisphere (det sign), so the
					// sign is resolved by folding toward world up (gm.z) - terrain faces never point below
					// the horizon under the RTS camera. The atlas micro-normal then reads as
					// a tilt AROUND the reconstructed macro normal instead of around (0,0,1):
					//     n = normalize(float3(atlas.xy * gain, 0) + atlas.z * gm)
					// On flat ground gm = (0,0,1) and the expression collapses to EXACTLY the L2.1 NM pixel
					// (bit-for-bit parity there); with no atlas (1x1 flat texel, a = (0,0,1)) the pixel gets
					// the pure face normal - that is the new per-pixel macro shading on slopes. Ratio
					// modulation and its sun division unchanged, cloud/lightmap/fog order unchanged, no
					// specular (the terrain line's T-C ruling stands).
					static const char* kTerrainPS30BaseNMPPL =
						"// d3d-target: ps_3_0\n"
						"sampler s0 : register(s0);\n"
						"sampler s1 : register(s1);\n"
						"sampler s4 : register(s4);\n"                 // normal atlas (stage4, uv0)
						"float4 c0 : register(c0);\n"
						"float4 c1 : register(c1);\n"
						"float4 c2 : register(c2);\n"                  // xyz = sun dir (normalized), w = deviation gain
						"float4 c3 : register(c3);\n"                  // P4: macro-normal matrix Kmat, row 0
						"float4 c4 : register(c4);\n"                  // P4: Kmat row 1
						"float4 c5 : register(c5);\n"                  // P4: Kmat row 2
						"struct PS_IN\n"
						"{\n"
						"    float2 uv0 : TEXCOORD0;\n"
						"    float2 uv1 : TEXCOORD1;\n"
						"    float3 dep : TEXCOORD2;\n"
						"    float4 vc  : COLOR0;\n"
						"};\n"
						"float4 main(PS_IN i) : COLOR\n"
						"{\n"
						"    float4 base = tex2D(s0, i.uv0);\n"
						"    float4 det  = tex2D(s1, i.uv1);\n"
						"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"
						"    float3 a = tex2D(s4, i.uv0).xyz * 2.0 - 1.0;\n"
						"    a.xy *= c2.w;\n"
						"    float3 wv = cross(ddx(i.dep), ddy(i.dep));\n"
						"    float3 gm = wv.x * c3.xyz + wv.y * c4.xyz + wv.z * c5.xyz;\n"
						"    float gl = length(gm);\n"
						"    gm = (gl > 1e-6) ? gm / gl : float3(0.0, 0.0, 1.0);\n"
						"    gm *= (gm.z < 0.0) ? -1.0 : 1.0;\n"
						"    float3 nn = float3(a.xy, 0.0) + a.z * gm;\n"
						"    float nl = length(nn);\n"
						"    float3 n = (nl > 1e-6) ? nn / nl : gm;\n"
						"    c.rgb *= saturate(dot(n, c2.xyz)) / max(c2.z, 0.05);\n"
						"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
						"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
						"}\n";
					static const char* kTerrainPS30NoiseNMPPL =
						"// d3d-target: ps_3_0\n"
						"sampler s0 : register(s0);\n"
						"sampler s1 : register(s1);\n"
						"sampler s2 : register(s2);\n"
						"sampler s4 : register(s4);\n"
						"float4 c0 : register(c0);\n"
						"float4 c1 : register(c1);\n"
						"float4 c2 : register(c2);\n"
						"float4 c3 : register(c3);\n"
						"float4 c4 : register(c4);\n"
						"float4 c5 : register(c5);\n"
						"struct PS_IN\n"
						"{\n"
						"    float2 uv0 : TEXCOORD0;\n"
						"    float2 uv1 : TEXCOORD1;\n"
						"    float3 dep : TEXCOORD2;\n"
						"    float4 vc  : COLOR0;\n"
						"};\n"
						"float4 main(PS_IN i) : COLOR\n"
						"{\n"
						"    float4 base = tex2D(s0, i.uv0);\n"
						"    float4 det  = tex2D(s1, i.uv1);\n"
						"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"
						"    float3 a = tex2D(s4, i.uv0).xyz * 2.0 - 1.0;\n"
						"    a.xy *= c2.w;\n"
						"    float3 wv = cross(ddx(i.dep), ddy(i.dep));\n"
						"    float3 gm = wv.x * c3.xyz + wv.y * c4.xyz + wv.z * c5.xyz;\n"
						"    float gl = length(gm);\n"
						"    gm = (gl > 1e-6) ? gm / gl : float3(0.0, 0.0, 1.0);\n"
						"    gm *= (gm.z < 0.0) ? -1.0 : 1.0;\n"
						"    float3 nn = float3(a.xy, 0.0) + a.z * gm;\n"
						"    float nl = length(nn);\n"
						"    float3 n = (nl > 1e-6) ? nn / nl : gm;\n"
						"    c.rgb *= saturate(dot(n, c2.xyz)) / max(c2.z, 0.05);\n"
						"    c = c * tex2D(s2, i.dep.xy);\n"
						"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
						"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
						"}\n";
					static const char* kTerrainPS30Noise12NMPPL =
						"// d3d-target: ps_3_0\n"
						"sampler s0 : register(s0);\n"
						"sampler s1 : register(s1);\n"
						"sampler s2 : register(s2);\n"
						"sampler s3 : register(s3);\n"
						"sampler s4 : register(s4);\n"
						"float4 c0 : register(c0);\n"
						"float4 c1 : register(c1);\n"
						"float4 c2 : register(c2);\n"
						"float4 c3 : register(c3);\n"
						"float4 c4 : register(c4);\n"
						"float4 c5 : register(c5);\n"
						"struct PS_IN\n"
						"{\n"
						"    float2 uv0 : TEXCOORD0;\n"
						"    float2 uv1 : TEXCOORD1;\n"
						"    float3 dep : TEXCOORD2;\n"
						"    float2 uv3 : TEXCOORD3;\n"
						"    float4 vc  : COLOR0;\n"
						"};\n"
						"float4 main(PS_IN i) : COLOR\n"
						"{\n"
						"    float4 base = tex2D(s0, i.uv0);\n"
						"    float4 det  = tex2D(s1, i.uv1);\n"
						"    float4 c = lerp(base, det, i.vc.a) * i.vc;\n"
						"    float3 a = tex2D(s4, i.uv0).xyz * 2.0 - 1.0;\n"
						"    a.xy *= c2.w;\n"
						"    float3 wv = cross(ddx(i.dep), ddy(i.dep));\n"
						"    float3 gm = wv.x * c3.xyz + wv.y * c4.xyz + wv.z * c5.xyz;\n"
						"    float gl = length(gm);\n"
						"    gm = (gl > 1e-6) ? gm / gl : float3(0.0, 0.0, 1.0);\n"
						"    gm *= (gm.z < 0.0) ? -1.0 : 1.0;\n"
						"    float3 nn = float3(a.xy, 0.0) + a.z * gm;\n"
						"    float nl = length(nn);\n"
						"    float3 n = (nl > 1e-6) ? nn / nl : gm;\n"
						"    c.rgb *= saturate(dot(n, c2.xyz)) / max(c2.z, 0.05);\n"
						"    c = c * tex2D(s2, i.dep.xy);\n"
						"    c = c * tex2D(s3, i.uv3);\n"
						"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"
						"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
						"}\n";

				// GeneralsX @bugfix PS3.0 batch22 14/10/2026: user on-machine verdict - the default-on
				// TERRAIN30 mirror produced blocky ground tint rings around structures and grid-sawtooth
				// terrain (the .pso disasm mirror does not reproduce the retail FF stage wiring exactly).
				// Shipping default is back to the retail fixed-function/.pso path; GENERALSX_TERRAIN30=1
				// keeps the HLSL leg alive as a QA opt-in.
				// GeneralsX @feature PS3.0 L2.2 17/10/2026: the switch had a Graphics Options page row.
				// GeneralsX @refactor Qoder 20/10/2026: that row is retired - TerrainPerPixel below is the
				// single terrain row and now arms this leg itself. The getter keeps the QA env winner and
				// the legacy ini key, so retail parity is unchanged unless the user opts in.
				// GeneralsX @bugfix Qoder 19/10/2026 BUG-03: under GENERALSX_VK_NOD3D the retail .pso trio
				// can never run - the stub device hands out identity handles with no driver behind them and
				// the draw path refuses every batch whose shader pair carries no translated twin - so the
				// HLSL trio, the only source the Vulkan program registry knows, is not an experiment there
				// but the only renderable terrain. Force it on; the batch22 opt-out stays on the d3d9 leg.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
				const bool vkNod3d = VK_NOD3D_Enabled();
#else
				const bool vkNod3d = false;
#endif
				// GeneralsX @refactor Qoder 20/10/2026: the two terrain rows were merged into one. The
				// "现代地形光影" parent and "地形逐像素光照" child were one chain, and under pure Vulkan the
				// parent was forced on anyway, so its checkbox read as a dead row. TerrainPerPixel is now
				// the single user switch and implies the ps_3_0 + NM leg; the legacy Terrain30Shading key
				// and GENERALSX_TERRAIN30 env still arm the leg alone for QA.
				const bool pplOn = OptionPreferences().getTerrainPerPixel() != FALSE;
				const bool terrain30On = pplOn
					|| (OptionPreferences().getTerrain30Shading() != FALSE) || vkNod3d;

				// GeneralsX @feature PS3.0 L2.1 TNM 17/10/2026: read the normal-pass switches once,
				// here where the terrain leg decides its shader variants. NM defaults ON but is only
				// reachable through terrain30On, so GENERALSX_TERRAIN30=0 keeps the exact batch23 pair.
				{
					const char* nenv = getenv("GENERALSX_TERRAIN_NM");
					// GeneralsX @bugfix Qoder 21/10/2026: the terrain normal atlas is packed without a
					// per-tile gutter, so bilinear sampling across a 256px tile edge blends two
					// unrelated normals and shades a dark/bright grid on every 2-cell tile
					// boundary (Alpine Assault). The reference Vulkan fork has no terrain NM
					// layer at all. Keep NM off by default; GENERALSX_TERRAIN_NM=1 remains the
					// development opt-in until the atlas carries per-tile padding.
					Bool globalNM = (TheGlobalData != nullptr && TheGlobalData->m_useNormalMapping != FALSE) ? TRUE : FALSE;
					// Same QA overrides the model/dot3 path honors, so one switch really means one switch.
					if (const char* ev = getenv("GENERALSX_NORMALMAP")) globalNM = (strcmp(ev, "0") != 0) ? TRUE : FALSE;
					if (const char* cmd = GetCommandLineA()) {
						if (strstr(cmd, "-nonormalmap") != nullptr) globalNM = FALSE;
						else if (strstr(cmd, "-normalmap") != nullptr) globalNM = TRUE;
					}
					s_terrainNMEnv = (nenv != nullptr && *nenv != 0) ? (atoi(nenv) != 0) : globalNM;
					const char* genv = getenv("GENERALSX_TERRAIN_NM_GAIN");
					if (genv != nullptr && *genv != 0) s_terrainNMGain = (Real)atof(genv);
				}
				// GeneralsX @feature Qoder 17/10/2026 PS3.0 T0 P4: the per-pixel switch arms the
				// derivative macro-normal trio below; it also forces the NM layer ON (the PPL variants
				// share the s4/c2 contract, and the 1x1 flat fallback keeps them asset-parity).
				const bool pplReq = pplOn;
				// PPL without NM is legal: s4 falls back to the flat 1x1 normal.
				// Pick the normal variants only while NM is on; if their compile fails the block below
				// falls back to the retail .pso trio exactly like before (no half-installed NM state).
				const bool useNM = (s_terrainNMEnv != FALSE);
				const char* srcBase    = pplReq ? kTerrainPS30BaseNMPPL    : (useNM ? kTerrainPS30BaseNM    : kTerrainPS30Base);
				const char* srcNoise   = pplReq ? kTerrainPS30NoiseNMPPL   : (useNM ? kTerrainPS30NoiseNM   : kTerrainPS30Noise);
				const char* srcNoise12 = pplReq ? kTerrainPS30Noise12NMPPL : (useNM ? kTerrainPS30Noise12NM : kTerrainPS30Noise12);

				if (terrain30On && W3DShaderManager::getShaderModelTier() >= DC_GENERIC_PIXEL_SHADER_3_0)
				{
					IDirect3DPixelShader9* hlslBase = nullptr;
					IDirect3DPixelShader9* hlslNoise = nullptr;
					IDirect3DPixelShader9* hlslNoise12 = nullptr;
					const char* bp = nullptr; const char* np = nullptr; const char* n12p = nullptr;
					const HRESULT bh = W3DShaderManager::compileShaderAtBestProfile(srcBase, false, "[TERRAIN30]", &bp, (void**)&hlslBase);
					const HRESULT nh = W3DShaderManager::compileShaderAtBestProfile(srcNoise, false, "[TERRAIN30]", &np, (void**)&hlslNoise);
					const HRESULT n2h = W3DShaderManager::compileShaderAtBestProfile(srcNoise12, false, "[TERRAIN30]", &n12p, (void**)&hlslNoise12);
					if (SUCCEEDED(bh) && SUCCEEDED(nh) && SUCCEEDED(n2h)
						&& hlslBase != nullptr && hlslNoise != nullptr && hlslNoise12 != nullptr)
					{
						// Swap the .pso handles for the HLSL ones; the retail pair is released here.
						if (m_dwBasePixelShader != nullptr) m_dwBasePixelShader->Release();
						if (m_dwBaseNoise1PixelShader != nullptr) m_dwBaseNoise1PixelShader->Release();
						if (m_dwBaseNoise2PixelShader != nullptr) m_dwBaseNoise2PixelShader->Release();
						m_dwBasePixelShader = hlslBase;
						m_dwBaseNoise1PixelShader = hlslNoise;
						m_dwBaseNoise2PixelShader = hlslNoise12;
						terrainHlslInstalled = TRUE;
						terrainPixelActive = TRUE;
						s_terrainHlslFog = TRUE;
						// GeneralsX @feature Qoder 17/10/2026 P4: the PPL trio is now the live trio;
						// arm the per-pass K upload in set() (and only then).
						s_terrainPPL = pplReq ? TRUE : FALSE;
						fprintf(stderr, "[TERRAIN30] HLSL terrain trio installed base=%s noise=%s noise12=%s nm=%d ppl=%d\n",
							bp ? bp : "?", np ? np : "?", n12p ? n12p : "?", useNM ? 1 : 0, pplReq ? 1 : 0);
						fflush(stderr);
					}
					else
					{
						if (hlslBase != nullptr) hlslBase->Release();
						if (hlslNoise != nullptr) hlslNoise->Release();
						if (hlslNoise12 != nullptr) hlslNoise12->Release();
						fprintf(stderr, "[TERRAIN30] HLSL compile failed, retail .pso path kept\n");
						fflush(stderr);
					}
				}
			// GeneralsX @bugfix Qoder 18/10/2026: register the pixel-shader terrain only when at
			// least one of the two sources above produced all three handles. The old unconditional
			// return TRUE registered a shader whose handles were all null on any install without
			// the retail terrain.pso family; set() then bound nothing and the whole ground
			// vanished - the Zero Hour "all terrain gone" report. FALSE hands the terrain slots to
			// the next candidate in TerrainShaderList (8Stage / 2Stage).
			// GeneralsX @bugfix Qoder 19/10/2026 BUG-03: under no-d3d9 a retail .pso trio is a trio
			// nothing can execute: its handles carry no translated twin in the Vulkan program registry,
			// so every terrain batch would be refused at draw time and the ground would vanish again
			// behind a "registered" log line. There, only the HLSL trio may register; the 2Stage
			// fixed-function fallback is the renderable leg.
			if (vkNod3d && !terrainHlslInstalled)
				terrainPixelActive = FALSE;
			if (terrainPixelActive)
			{
				W3DShaders[W3DShaderManager::ST_TERRAIN_BASE]=this;
				W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=this;
				W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=this;
				W3DShaders[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=this;
				W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE]=1;
				W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE1]=1;
				W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE2]=1;
				W3DShadersPassCount[W3DShaderManager::ST_TERRAIN_BASE_NOISE12]=1;
				DEBUG_LOG(("W3DShaderManager: terrain pixel shader registered"));
				fprintf(stderr, "[TERRAIN] pixel trio registered source=%s nm=%d ppl=%d\n",
					terrainHlslInstalled ? "hlsl" : "retail_pso", s_terrainNMEnv ? 1 : 0, s_terrainPPL ? 1 : 0);
				fflush(stderr);
				return TRUE;
			}
			fprintf(stderr, "[TERRAIN] no pixel-shader trio (pso=%d hlsl=%d); falling back to stage shaders\n",
				pixelShadersAvailable ? 1 : 0, terrainHlslInstalled ? 1 : 0);
			fflush(stderr);
			return FALSE;
		}
	}
	return FALSE;
}

Int TerrainShaderPixelShader::set(Int pass)
{
	// GeneralsX @feature Qoder 18/10/2026 read-only terrain path census: which leg actually drew, one
	// line per 4096 sets. The leg is chosen at init time, so this is the only place that can say whether
	// the retail .pso trio or an HLSL trio owns the ground this run.
	{
		static unsigned sTerrainPathSets = 0u;
		if ((sTerrainPathSets++ & 4095u) == 0u)
		{
			fprintf(stderr, "[TERRAIN] path=%s pass=%d nm=%d ppl=%d\n",
				s_terrainHlslFog ? (s_terrainPPL ? "ps30_ppl" : "ps30_nm") : "retail_pso",
				pass, s_terrainNMEnv ? 1 : 0, s_terrainPPL ? 1 : 0);
			fflush(stderr);
		}
	}

	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	DX8Wrapper::Apply_Render_State_Changes();

	if (s_terrainHlslFog)
	{
		// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: fog ramp constants for the HLSL trio, shared
		// with the road variant (re-uploaded per set() so device resets can never leave a stale pair).
		Upload_Terrain30_Fog_Constants();

		// GeneralsX @feature PS3.0 L2.1 TNM 17/10/2026: wire the packed normal atlas into the NM
		// variants. The atlas (stage slot 4) is only present when *_nm.tga sheets were found for this
		// map; the shaders are compiled for NM the whole session, so an unbound stage 4 would sample
		// white and fold the ground - the flat 1x1 fallback keeps the ratio exactly 1.0 instead.
		if (W3DShaderManager::isTerrainNormalEnabled() || s_terrainPPL)
		{
			TextureClass *nmAtlas = W3DShaderManager::isTerrainNormalEnabled() ? W3DShaderManager::getShaderTexture(4) : nullptr;
			IDirect3DTexture9 *nmTex = (nmAtlas != nullptr) ? nmAtlas->Peek_D3D_Texture() : nullptr;
			if (nmTex == nullptr)
			{
				if (s_nmFlatFallback == nullptr)
				{
					LPDIRECT3DDEVICE9 pDev = DX8Wrapper::_Get_D3D_Device8();
					if (FAILED(pDev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_nmFlatFallback, nullptr)))
						s_nmFlatFallback = nullptr;
					else
					{
						IDirect3DSurface9 *lvl = nullptr;
						if (SUCCEEDED(s_nmFlatFallback->GetSurfaceLevel(0, &lvl)))
						{
							D3DLOCKED_RECT lr;
							if (SUCCEEDED(lvl->LockRect(&lr, nullptr, 0)))
							{
								*((Int*)lr.pBits) = (Int)0xFF8080FFu;	// A255 R128 G128 B255 = tangent +Z
								lvl->UnlockRect();
							}
							lvl->Release();
							// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.3: this raw 1x1 flat-normal
							// fallback is written straight on the CPU (pDev->CreateTexture, no TextureClass),
							// so mirror it explicitly into the keyed Vulkan image once, after the fill.
							VKTex_Mirror_D3D_Texture(s_nmFlatFallback);
						}
					}
				}
				nmTex = s_nmFlatFallback;
			}
			if (nmTex != nullptr)
			{
				// GeneralsX @bugfix Qoder 18/10/2026 wedge 8: through the facade, not the device. The draw
				// layer builds descriptor slot 4 from the facade's shadow, so a device-direct bind left the
				// terrain normal atlas invisible to Vulkan while D3D9 still sampled it.
				DX8Wrapper::Set_DX8_Texture(4, nmTex);
				DX8Wrapper::Set_DX8_Sampler_State(4, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				DX8Wrapper::Set_DX8_Sampler_State(4, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
				DX8Wrapper::Set_DX8_Sampler_State(4, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
				DX8Wrapper::Set_DX8_Sampler_State(4, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				DX8Wrapper::Set_DX8_Texture_Stage_State(4, D3DTSS_TEXCOORDINDEX, 0);
			}

			// GeneralsX @bugfix Qoder 22/10/2026 Sun/gain constants must be uploaded even when the
			// 1x1 flat-normal fallback could not be created. They used to live inside the nmTex
			// branch, so a failed s4 bind left c2 = (0,0,0,0); the PPL shader then multiplied every
			// terrain pixel by dot(n, 0) / 0.05 = 0 and the ground read as if all textures vanished.
			// The sampler bind stays optional (s4 is only sampled by the NM/PPL variants).
			{
				// Sun vector: same -m_terrainLightPos[0] the CPU doTheLight loop shades the vertex
				// diffuse with (HeightMap.cpp builds lightRay from it), normalized here because the
				// shader divides by L.z so a flat texel lands on exactly 1.0. Degenerate/missing sun
				// falls back to straight up, which is neutral for the overhead RTS view.
				float sun[4];
				{
					const Coord3D &lp = TheGlobalData->m_terrainLightPos[0];
					Vector3 L(-lp.x, -lp.y, -lp.z);
					Real mag = L.Length();
					if (mag < 1e-3f) { sun[0] = 0.0f; sun[1] = 0.0f; sun[2] = 1.0f; }
					else { sun[0] = L.X / mag; sun[1] = L.Y / mag; sun[2] = L.Z / mag; }
					// GeneralsX @feature Qoder 22/10/2026: follow the same NormalMapStrength slider
					// as the model dot3 path (live). The terrain shader adds the normal-map xy to
					// the interpolated world normal without a full TBN, so full slider strength
					// reads exaggerated; map 0..2 to 0..1 (default 0.8 -> 0.4).
					sun[3] = (s_terrainNMGain >= 0.0f)
						? s_terrainNMGain
						: (DX8NormalMap::Get_Strength() * 0.25f);
				}
				DX8Wrapper::Set_Pixel_Shader_Constant(2, sun, 1);

				// GeneralsX @feature PS3.0 L2.1 TNM: one evidence line per session (change detector
				// not needed - sun is static unless a script moves it, and the binding state only
				// changes at map load). Atlas vs flat + the actual constants: the only in-game proof
				// that the modulation math is fed real numbers.
				static Bool s_tnmLogged = FALSE;
				if (!s_tnmLogged)
				{
					s_tnmLogged = TRUE;
					fprintf(stderr, "[TNM] stage4=%s sun=(%.3f,%.3f,%.3f) gain=%.2f\n",
						(nmAtlas != nullptr) ? "atlas" : "flat", sun[0], sun[1], sun[2], sun[3]);
					fflush(stderr);
				}
			}
		}
	}

	//setup base pass
	DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(0));
	DX8Wrapper::Set_Texture(1, W3DShaderManager::getShaderTexture(1));

	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

	//tell pixel shader which UV set to use for each stage
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 1 );

	// GeneralsX @feature PS3.0-P3-5 14/10/2026 THE shipping terrain path (single-pass pixel shader).
	// The retail override above this line forced D3DTEXF_POINT on the atlas and noise stages whenever
	// BilinearTerrainTex/TrilinearTerrainTex were left FALSE in the shipped INI, which is the default -
	// so terrain ignored the player's texture-filtering option and never got anisotropic sampling.
	// It now takes the same tier the rest of the world uses; the INI bools remain as a floor.
	{
		const bool bilinearHint=TheGlobalData ? TheGlobalData->m_bilinearTerrainTex!=FALSE : false;
		const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
		TextureFilterClass::Apply_Player_Filter(0,bilinearHint,trilinearHint);
		TextureFilterClass::Apply_Player_Filter(1,bilinearHint,trilinearHint);
	}

	if (W3DShaderManager::getCurrentShader() >= W3DShaderManager::ST_TERRAIN_BASE_NOISE1)
	{
		Matrix4x4 curView;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

		D3DXMATRIX inv;
		float det;
		D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

		DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		// Two output coordinates are used, three when the HLSL fog relay is live (batch8 G1: .z = camera depth).
		DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXTURETRANSFORMFLAGS,
			s_terrainHlslFog ? D3DTTFF_COUNT3 : D3DTTFF_COUNT2);

		DX8Wrapper::Set_DX8_Sampler_State(2,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		DX8Wrapper::Set_DX8_Sampler_State(2,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

		if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_TERRAIN_BASE_NOISE12)
		{
			//full shader
			DX8Wrapper::Set_DX8_Sampler_State(3,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
			DX8Wrapper::Set_DX8_Sampler_State(3,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
			DX8Wrapper::Set_Texture(2, W3DShaderManager::getShaderTexture(2));
			DX8Wrapper::Set_Texture(3, W3DShaderManager::getShaderTexture(3));
			DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_dwBaseNoise2PixelShader));

			DX8Wrapper::Set_DX8_Sampler_State(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
			DX8Wrapper::Set_DX8_Sampler_State(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

			DX8Wrapper::Set_DX8_Sampler_State(3, D3DSAMP_MINFILTER, D3DTEXF_POINT);
			DX8Wrapper::Set_DX8_Sampler_State(3, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

			terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
			if (s_terrainHlslFog)
			{
				// Batch8 G1: pin the third matrix column to (0,0,1,0) so TEXCOORD2.z passes camera depth
				// while the noise uv keeps consuming TEXCOORD2.xy exactly like the retail COUNT2 wiring did.
				// Named fields, not m[]: D3DMATRIX::m is float[4][4] in the D3D9 headers, and the flat
				// m[2]/m[6]/m[10]/m[14] form fails MSVC with C3863 (probe hit the same, 14/10).
				D3DXMATRIX* tm = (D3DXMATRIX*)&curView;
				tm->_13 = 0.0f; tm->_23 = 0.0f; tm->_33 = 1.0f; tm->_43 = 0.0f;
			}
			DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE2, curView);

			terrainShader2Stage.updateNoise2((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
			DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE3, curView);

			DX8Wrapper::Set_DX8_Texture_Stage_State(3,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
			// Two output coordinates are used.
			DX8Wrapper::Set_DX8_Texture_Stage_State(3,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
		}
		else
		{
			//single noise texture shader
			DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_dwBaseNoise1PixelShader));

			if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_TERRAIN_BASE_NOISE1)
			{
				//cloud map
				DX8Wrapper::Set_Texture(2, W3DShaderManager::getShaderTexture(2));
				terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
				DX8Wrapper::Set_DX8_Sampler_State(2, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
				DX8Wrapper::Set_DX8_Sampler_State(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			}
			else
			{
				//light map
				DX8Wrapper::Set_Texture(2, W3DShaderManager::getShaderTexture(3));
				terrainShader2Stage.updateNoise2((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
				DX8Wrapper::Set_DX8_Sampler_State(2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
				DX8Wrapper::Set_DX8_Sampler_State(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
			}
			if (s_terrainHlslFog)
			{
				// Batch8 G1: third-column pin as above; single-noise variants share the TEXCOORD2 relay.
				D3DXMATRIX* tm = (D3DXMATRIX*)&curView;
				tm->_13 = 0.0f; tm->_23 = 0.0f; tm->_33 = 1.0f; tm->_43 = 0.0f;
			}
			DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE2, curView);
		}
	}
	else
	{
		//just base texturing
		if (s_terrainHlslFog)
		{
			// Batch8 G1: identity TEXTURE2 with COUNT3 so the base variant still gets camera depth
			// in TEXCOORD2.z; nothing samples stage 2 here, the generator output is pure relay.
			DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
			DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			Matrix4x4 identM(true);
			DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE2, identM);
		}
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_dwBasePixelShader));
	}

	// GeneralsX @feature Qoder 17/10/2026 PS3.0 T0 P4: per-pass macro-normal reconstruction matrix for
	// the PPL trio (PS c3..c5 = the three rows of Kmat = T3^T * inv3, see the trio's source comment).
	// Both transforms are read back from the DEVICE so the matrix fed to the shader is exactly the one
	// that will generate this pass's TEXCOORD2 relay - noise projection, lightmap projection, or the
	// batch8 identity, all covered by one tail block. Indexed through contiguous float views because
	// D3DXMATRIX's flat m[] form hit C3863 in batch8 G1. A degenerate VIEW would leave c3..c5 stale -
	// impossible for a live camera, and worst case the shader just keeps the previous frame's fold.
	if (s_terrainPPL && s_terrainHlslFog)
	{
		Matrix4x4 pplView;
		Matrix4x4 pplTex;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, pplView);
		DX8Wrapper::_Get_DX8_Transform(D3DTS_TEXTURE2, pplTex);
		D3DXMATRIX pplInv;
		float pplDet;
		if (SUCCEEDED(D3DXMatrixInverse(&pplInv, &pplDet, (D3DXMATRIX*)&pplView)))
		{
			const float* tv = reinterpret_cast<const float*>(&pplTex);	// row-major: tv[r*4+c] = T[r][c]
			const float* iv = reinterpret_cast<const float*>(&pplInv);
			float kmat[12];
			for (int row = 0; row < 3; ++row) {
				for (int col = 0; col < 3; ++col) {
					// Kmat[row][col] = sum_m T3[m][row] * inv3[m][col]   (inv3 = V3^T, VIEW is rigid)
					kmat[row * 4 + col] = tv[row] * iv[col] + tv[4 + row] * iv[4 + col] + tv[8 + row] * iv[8 + col];
				}
				kmat[row * 4 + 3] = 0.0f;
			}
			DX8Wrapper::Set_Pixel_Shader_Constant(3, kmat, 3);
		}
	}

	return TRUE;
}

void TerrainShaderPixelShader::reset()
{
	DX8Wrapper::Set_DX8_Texture(2,nullptr);	//release reference to any texture
	DX8Wrapper::Set_DX8_Texture(3,nullptr);	//release reference to any texture
	// GeneralsX @bugfix Qoder 18/10/2026 wedge 8: release stage 4 through the facade as well, otherwise
	// the shadow keeps a dangling normal-atlas pointer after reset and the next draw binds freed memory.
	DX8Wrapper::Set_DX8_Texture(4, nullptr);	// GeneralsX @feature PS3.0 L2.1 TNM 17/10/2026 normal atlas

	DX8Wrapper::Set_Pixel_Shader(0);	//turn off pixel shader

	DX8Wrapper::Set_DX8_Texture(0, nullptr);
	DX8Wrapper::Set_DX8_Texture(1, nullptr);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|0);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|1);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|2);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|3);


	DX8Wrapper::Invalidate_Cached_Render_States();
}

///Cloud layer rendering shader - used for objects similar to terrain which only need the cloud layer.
class CloudTextureShader : public W3DShaderInterface
{
	virtual Int set(Int stage) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	Int m_stageOfSet;
} cloudTextureShader;

///List of different cloud shader implementations in order of preference
W3DShaderInterface *CloudShaderList[]=
{
	&cloudTextureShader,
	nullptr
};

Int CloudTextureShader::init()
{
	W3DShaders[W3DShaderManager::ST_CLOUD_TEXTURE]=&cloudTextureShader;
	W3DShadersPassCount[W3DShaderManager::ST_CLOUD_TEXTURE]=1;

	return TRUE;
}

/**Setup a certain texture stage to project our cloud texture*/
Int CloudTextureShader::set(Int stage)
{
	Matrix4x4 curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

	D3DXMATRIX inv;
	float det;

	D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

	//Get a texture matrix that applies the current cloud position
	terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView,&inv,false);	//update curView with texture matrix

	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
	DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE )(D3DTS_TEXTURE0+stage), curView);
	DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG2, D3DTA_CURRENT );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAARG2, D3DTA_CURRENT );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAOP,   D3DTOP_MODULATE );

	DX8Wrapper::Set_Texture(stage, W3DShaderManager::getShaderTexture(stage));

	m_stageOfSet=stage;
	return TRUE;
}

void CloudTextureShader::reset()
{
	//Free reference to texture
	DX8Wrapper::Set_DX8_Texture(m_stageOfSet, nullptr);
	//Turn off texture projection
	DX8Wrapper::Set_DX8_Texture_Stage_State( m_stageOfSet, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( m_stageOfSet, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|m_stageOfSet);

	DX8Wrapper::Set_DX8_Texture_Stage_State( m_stageOfSet, D3DTSS_COLOROP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( m_stageOfSet, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
}

/*===========================================================================================*/
/*=========      Road Shaders	=========================================================*/
/*===========================================================================================*/
class RoadShaderPixelShader : public W3DShaderInterface
{
	IDirect3DPixelShader9*					m_dwBaseNoise2PixelShader;	///<handle to road/double noise D3D pixel shader

	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Int shutdown() override;			///<release resources used by shader
} roadShaderPixelShader;

class RoadShader2Stage : public W3DShaderInterface
{	friend class RoadShaderPixelShader;	//pixel shader version uses some of the same features.

	virtual Int set(Int pass) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;
} roadShader2Stage;

///List of different terrain shader implementations in order of preference
W3DShaderInterface *RoadShaderList[]=
{
	&roadShaderPixelShader,
	&roadShader2Stage,
	nullptr
};

Int RoadShaderPixelShader::shutdown()
{
	if (m_dwBaseNoise2PixelShader)
		m_dwBaseNoise2PixelShader->Release();

	m_dwBaseNoise2PixelShader = nullptr;

	return TRUE;
}

Int RoadShaderPixelShader::init()
{
	Int res;

	//this shader will also use the 2Stage shader for some of the passes so initialize it too.
	if (roadShader2Stage.init() && (res=W3DShaderManager::getChipset()) >= DC_GENERIC_PIXEL_SHADER_1_1)
	{
		if (res >= DC_GENERIC_PIXEL_SHADER_1_1)
		{
			//this shader needs some assets that need to be loaded
			//shader declaration
			D3DVERTEXELEMENT9 Declaration[]=
			{
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 0), // Position
				D3DDECL_USAGE(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 12), // Diffuse
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, 0, 16), //  Texture Coordinates
				D3DDECL_END()
			};

			// GeneralsX @bugfix BenderAI 13/02/2026 - Graceful fallback when roadnoise2.pso doesn't exist
			//version which blends 2 noise textures.
			HRESULT hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\roadnoise2.pso", &Declaration[0], 0, false, (void**)&m_dwBaseNoise2PixelShader);
			if (FAILED(hr))
			{
				DEBUG_LOG(("W3DShaderManager: roadnoise2.pso not found, using 2-stage fallback"));
				// Fall back to 2-stage shader - don't fail init
				return TRUE;
			}

			//Only set this shader for use in dual noise mode.  The 2Stage shader will take care of
			//all the other modes.
			W3DShaders[W3DShaderManager::ST_ROAD_BASE_NOISE12]=&roadShaderPixelShader;
			W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE_NOISE12]=1;
			DEBUG_LOG(("W3DShaderManager: Using pixel shader for road dual-noise mode"));

			// GeneralsX @feature PS3.0 batch8 G1 14/10/2026 ps_3_0 road shader with per-pixel haze.
			// Mirrors roadnoise2.pso exactly (mul t0,t1; mul t2; mul v0 - logs/pso_disasm2_clean.txt)
			// and adds the GNFOG ramp blend. Eye depth rides in TEXCOORD3.z: roads keep stages 0..2 for
			// the diffuse + two noise projections, so the relay gets its own stage-3 CAMERASPACEPOSITION
			// generator with an identity matrix. Compile failure keeps the retail .pso; GENERALSX_ROAD30=0
			// opts out (same gate style as GENERALSX_TERRAIN30).
			static const char* kRoadPS30 =
				"// d3d-target: ps_3_0\n"
				"sampler s0 : register(s0);\n"                 // road atlas (stage0, uv0)
				"sampler s1 : register(s1);\n"                 // noise 1 (stage1, TC1.xy)
				"sampler s2 : register(s2);\n"                 // noise 2 (stage2, TC2.xy)
				"float4 c0 : register(c0);\n"                  // haze rgb, w = fog strength
				"float4 c1 : register(c1);\n"                  // x = eye-depth start, y = 1/ramp length
				"struct PS_IN\n"
				"{\n"
				"    float2 uv0 : TEXCOORD0;\n"
				"    float2 n1  : TEXCOORD1;\n"
				"    float2 n2  : TEXCOORD2;\n"
				"    float3 dep : TEXCOORD3;\n"                // .z = camera depth (stage-3 COUNT3 relay)
				"    float4 vc  : COLOR0;\n"
				"};\n"
				"float4 main(PS_IN i) : COLOR\n"
				"{\n"
				"    float4 c = tex2D(s0, i.uv0) * tex2D(s1, i.n1) * tex2D(s2, i.n2) * i.vc;\n"
				"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"   // batch23: camera z is negative ahead (D3D -Z); see kTerrainPS30Base note
				"    return lerp(c, float4(c0.rgb, c.a), fogf);\n"
				"}\n";

			// GeneralsX @bugfix PS3.0 batch22 14/10/2026: same user verdict as TERRAIN30 - the road
			// HLSL mirror joins the blocky per-quad ground artifacts; retail .pso is the shipping
			// default and GENERALSX_ROAD30=1 keeps the QA leg.
			bool road30On = false;
			const char* renv = getenv("GENERALSX_ROAD30");
			if (renv != nullptr && *renv != 0 && strcmp(renv, "0") != 0) road30On = true;
			if (road30On && W3DShaderManager::getShaderModelTier() >= DC_GENERIC_PIXEL_SHADER_3_0)
			{
				IDirect3DPixelShader9* hlslRoad = nullptr;
				const char* rp = nullptr;
				const HRESULT rr = W3DShaderManager::compileShaderAtBestProfile(kRoadPS30, false, "[ROAD30]", &rp, (void**)&hlslRoad);
				if (SUCCEEDED(rr) && hlslRoad != nullptr)
				{
					if (m_dwBaseNoise2PixelShader != nullptr) m_dwBaseNoise2PixelShader->Release();
					m_dwBaseNoise2PixelShader = hlslRoad;
					s_roadHlslFog = TRUE;
					fprintf(stderr, "[ROAD30] HLSL road shader installed %s\n", rp ? rp : "?");
					fflush(stderr);
				}
				else
				{
					if (hlslRoad != nullptr) hlslRoad->Release();
					fprintf(stderr, "[ROAD30] HLSL compile failed, retail .pso path kept\n");
					fflush(stderr);
				}
			}
			return TRUE;
		}
	}
	return FALSE;
}

Int RoadShaderPixelShader::set(Int pass)
{
	DX8Wrapper::Set_Texture(0,W3DShaderManager::getShaderTexture(0));
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	DX8Wrapper::Apply_Render_State_Changes();

	//tell pixel shader which UV set to use for each stage
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );

	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);

	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);	//blend roads into terrain
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

	Matrix4x4 curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

	D3DXMATRIX inv;
	float det;
	D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

	// GeneralsX @feature PS3.0-P3-5 14/10/2026 Roads pinned MIPFILTER to POINT unless the
	// TrilinearTerrainTex INI bool was set, so distant road seams kept switching mip hard whatever
	// the player chose. The mip step now comes from the same tier tables as everything else, with the
	// bool kept as a floor. GENERALSX_TERRAIN_FILTER=legacy reproduces the retail rule.
	{
		const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
		TextureFilterClass::Apply_Player_Mip_Filter(0,trilinearHint);
		TextureFilterClass::Apply_Player_Mip_Filter(1,trilinearHint);
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	// Two output coordinates are used.
	DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

	DX8Wrapper::Set_DX8_Sampler_State(1,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Sampler_State(1,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

	DX8Wrapper::Set_DX8_Sampler_State(2,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Sampler_State(2,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

	DX8Wrapper::Set_Texture(1,W3DShaderManager::getShaderTexture(1));
	DX8Wrapper::Set_Texture(2,W3DShaderManager::getShaderTexture(2));

	if (s_roadHlslFog)
	{
		// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: fog constants + the stage-3 identity
		// CAMERASPACEPOSITION generator whose COUNT3 output puts camera depth in TEXCOORD3.z.
		Upload_Terrain30_Fog_Constants();
		DX8Wrapper::Set_DX8_Texture_Stage_State(3,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		DX8Wrapper::Set_DX8_Texture_Stage_State(3,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
		Matrix4x4 identM(true);
		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE3, identM);
	}
	DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_dwBaseNoise2PixelShader));

	DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

	DX8Wrapper::Set_DX8_Sampler_State(2, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	DX8Wrapper::Set_DX8_Sampler_State(2, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

	terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView,&inv, false);	//get texture projection matrix
	DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, curView);

	terrainShader2Stage.updateNoise2((D3DXMATRIX*)&curView,&inv, false);	//get texture projection matrix
	DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE2, curView);

	DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	// Two output coordinates are used.
	DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

	return TRUE;
}

void RoadShaderPixelShader::reset()
{

	DX8Wrapper::Set_Pixel_Shader(0);	//turn off pixel shader

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|0);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|1);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|2);

	// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: the fog relay occupies stage 3 in HLSL mode,
	// so restore it here exactly like the other stages (harmless when the .pso path is live).
	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|3);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|3);


	DX8Wrapper::Invalidate_Cached_Render_States();
}

Int RoadShader2Stage::init()
{
	//no special device validation needed - anything in our min spec should handle this.
	W3DShaders[W3DShaderManager::ST_ROAD_BASE]=&roadShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE]=1;
	W3DShaders[W3DShaderManager::ST_ROAD_BASE_NOISE1]=&roadShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE_NOISE1]=1;
	W3DShaders[W3DShaderManager::ST_ROAD_BASE_NOISE2]=&roadShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE_NOISE2]=1;
	W3DShaders[W3DShaderManager::ST_ROAD_BASE_NOISE12]=&roadShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_ROAD_BASE_NOISE12]=2;

	return TRUE;
}

Int RoadShader2Stage::set(Int pass)
{
	//First stage always contains base texture.
	DX8Wrapper::Set_Texture(0,W3DShaderManager::getShaderTexture(0));
	//Force system to apply world/view transforms.
	DX8Wrapper::Apply_Render_State_Changes();

	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE,FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);

	// Modulate the diffuse color with the texture as lighting comes from diffuse.
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_MODULATE );

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);	//blend roads into terrain

	if (pass == 0)
	{
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

		if (W3DShaderManager::getCurrentShader() >= W3DShaderManager::ST_ROAD_BASE_NOISE1)
		{	//second texture unit will contain a noise pass
			Matrix4x4 curView;
			DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

			D3DXMATRIX inv;
			float det;
			D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

			// GeneralsX @feature PS3.0-P3-5 14/10/2026 Noise layer of the road pass followed the same
			// retail INI-bool rule as the base layer; it now uses the player's tier (bool as a floor).
			{
				const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
				TextureFilterClass::Apply_Player_Mip_Filter(1,trilinearHint);
			}

			DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
			// Two output coordinates are used.
			DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

			DX8Wrapper::Set_DX8_Sampler_State(1,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
			DX8Wrapper::Set_DX8_Sampler_State(1,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_MODULATE );

			if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_ROAD_BASE_NOISE12)
			{
				//full shader, apply noise 1 in pass 0.
				DX8Wrapper::Set_Texture(1,W3DShaderManager::getShaderTexture(1));
				DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
				DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

				terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView, &inv, false);	//get texture projection matrix
				DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, curView);
			}
			else
			{
				//single noise texture shader
				if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_ROAD_BASE_NOISE1)
				{
					//cloud map
					DX8Wrapper::Set_Texture(1,W3DShaderManager::getShaderTexture(1));
					terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView, &inv, false);	//update curView with texture matrix
					DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
					DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				}
				else
				{
					//light map
					DX8Wrapper::Set_Texture(1,W3DShaderManager::getShaderTexture(2));
					terrainShader2Stage.updateNoise2((D3DXMATRIX*)&curView,&inv, false);	//update curView with texture matrix
					DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
					DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				}
				DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, curView);
			}
		}
		else
		{
			//just base texturing
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
		}
	}
	else
	{	//pass 1, apply additional noise pass
		Matrix4x4 curView;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

		D3DXMATRIX inv;
		float det;
		D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

		// GeneralsX @feature PS3.0-P3-5 14/10/2026 Second-pass noise layer: mip step now follows the
		// player's tier instead of the retail TrilinearTerrainTex bool (kept as a floor). The
		// MINFILTER=POINT / MAGFILTER=LINEAR pair below is an intentional retail light-map choice
		// (it does not read the INI bools), so it is left as shipped.
		{
			const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
			TextureFilterClass::Apply_Player_Mip_Filter(1,trilinearHint);
		}

		DX8Wrapper::Set_Texture(1,W3DShaderManager::getShaderTexture(2));

		terrainShader2Stage.updateNoise2((D3DXMATRIX*)&curView, &inv, false);	//update curView with texture matrix
		DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

		DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		// Two output coordinates are used.
		DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

		DX8Wrapper::Set_DX8_Sampler_State(1,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		DX8Wrapper::Set_DX8_Sampler_State(1,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

		//Copy alpha channel into stage 1 but mask out color channel by replacing with white.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		//Force color channel to white by copying the alpha into RGB
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE|D3DTA_ALPHAREPLICATE);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_BLENDCURRENTALPHA);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		//Modulate into existing roads with clouds applied. - only apply where roads are transparent by
		//using road texture as a mask.
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_ZERO);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);

		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, curView);
	}

	return TRUE;
}

void RoadShader2Stage::reset()
{
	ShaderClass::Invalidate();

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|0);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|1);
}

/** List of all custom shader lists - each list in this list contains variations of the same
	shader to allow it to work on different hardware configurations.
*/
W3DShaderInterface **MasterShaderList[]=
{
	TerrainShaderList,
	ShroudShaderList,
	FlatShroudShaderList,
	RoadShaderList,
	MaskShaderList,
	CloudShaderList,
	FlatTerrainShaderList,
	nullptr
};

/** List of all custom filter lists - each list in this list contains variations of the same
	filter to allow it to work on different hardware configurations.
*/
W3DFilterInterface **MasterFilterList[]=
{
	ScreenDefaultFilterList,
	// GeneralsX @feature 05/10/2026 Bloom glow post-processing filter.
	ScreenBloomFilterList,
	ScreenBWFilterList,
	ScreenMotionBlurFilterList,
	ScreenCrossFadeFilterList,
	nullptr
};

// W3DShaderManager::W3DShaderManager =========================================
/** Constructor - just clears some variables */
//=============================================================================
W3DShaderManager::W3DShaderManager()
{
	m_currentShader = ST_INVALID;
	m_currentFilter = FT_NULL_FILTER;
	m_oldRenderSurface = nullptr;
	m_renderTexture = nullptr;
	m_newRenderSurface = nullptr;
	m_oldDepthSurface = nullptr;
	m_rttColorSurface = nullptr;
	m_rttDepthSurface = nullptr;
	m_rttBindFailures = 0;	// GeneralsX @tweak PS3.0-P2-3 14/10/2026 S6
	m_renderingToTexture = false;
	Int i;
	for (i=0; i<W3DShaderManager::ST_MAX; i++)
	{	W3DShaders[i]=nullptr;
		W3DShadersPassCount[i]=0;
	}
	for (i=0; i<FT_MAX; i++)
	{	W3DFilters[i]=nullptr;
	}
	for (i=0; i<SHADER_TEXTURE_STAGES; i++)
	{
		m_Textures[i]=nullptr;
	}
	m_currentShader=(W3DShaderManager::ShaderTypes)-1;
}

// GeneralsX @feature T3-0 M3/M4 -------------------------------------------------
// Runtime HLSL compilation + startup diagnostics for dot3 per-pixel normal lighting.
//   - Windows-only: the CompatLib D3DXAssembleShader shim drives D3DCompile out of
//     d3dcompiler_47.dll. D3D9 has no DX8 assembly assembler, so the sources below are HLSL and
//     W3DShaderManager::compileShaderAtBestProfile prefixes each with the "// d3d-target:" line for
//     the profile the caps ladder reports.
//   - Requires caps->Support_Dot3().
//   - Regardless of the opt-in switch, we try to compile+create VS/PS at startup and log the
//     result to stderr (proving DXVK/GPU accepts the emitted profile, vs_3_0/ps_3_0 where the device
//     reports them and vs_2_0/ps_2_0 otherwise). On success we Install into
//     the WW3D2-side module; whether it actually mounts is still gated by the DX8NormalMap
//     default-OFF switch. On failure the handles stay 0 and never mount -> zero regression.
//
// Vertex layout contract: these batches are drawn with the FVF DX8_FVF_XYZNDUV1TG3 (72 bytes:
// position, normal, diffuse, uv0(2), tangent(3), binormal(3), pad(3)), so the shader inputs are
// POSITION / NORMAL / COLOR0 / TEXCOORD0..2. HLSL has no semantics for the DX8 TANGENT/BINORMAL
// declaration usages, which is exactly why the tangent basis travels in the UV sets the format
// already reserves for it.
//=============================================================================
static void InitNormalMapShaders()
{
#ifdef _WIN32
	const DX8Caps* caps = DX8Wrapper::Get_Current_Caps();
	if (!caps || !caps->Support_Dot3()) {
		fprintf(stderr, "[DOT3] skipped: device does not report Support_Dot3\n");
		fflush(stderr);
		return;
	}

	// VS: transform position by WVP in c0..c3; pass uv and diffuse through. c4 = MODEL-SPACE light
	// direction - rotate it into tangent space with the vertex basis (tangent, binormal, normal) and
	// emit it as TEXCOORD1, so every surface gets its own light angle (per-face shading) instead of
	// one constant tangent-space light that made all faces render the same flat darkness.
	// The VS does no correcting of its own: whatever frame c4/c5/c9 arrive in is the frame that gets
	// shaded, so their world -> model rotate must be exact (see @bugfix PS3.0-#76b in dx8normalmap.cpp).
	// GeneralsX @feature PS3.0 T1 13/10/2026 Blinn-Phong: c5 = MODEL-SPACE eye direction (the camera's
	// +Z world axis, read out of the DX8 view matrix by dx8normalmap.cpp; constant across the RTS view).
	// The VS rotates it into tangent space exactly like c4 and emits the UNNORMALISED light+eye sum as
	// TEXCOORD2; the PS normalises per pixel (the half vector must be re-normalised after interpolation,
	// which is the ps_3_0 nrm/normalize win over the ps_2_0 pair).
	// GeneralsX @feature PS3.0 L1 13/10/2026 three-point lighting: c9 = MODEL-SPACE sky-fill
	// direction (dx8normalmap rotates g_FillDir world->model exactly like c4). It rides the same
	// per-vertex TBN into TEXCOORD3 so the fill shades per-face just like the key.
	static const char* kVertexShader =
		"float4 c0 : register(c0);\n"
		"float4 c1 : register(c1);\n"
		"float4 c2 : register(c2);\n"
		"float4 c3 : register(c3);\n"
		"float4 c4 : register(c4);\n"
		"float4 c5 : register(c5);\n"
		"float4 c9 : register(c9);\n"
		"struct VS_OUT\n"
		"{\n"
		"    float4 pos     : POSITION;\n"
		"    float4 diffuse : COLOR0;\n"
		"    float2 uv      : TEXCOORD0;\n"
		"    float3 light   : TEXCOORD1;\n"
		"    float3 hvec    : TEXCOORD2;\n"
		"    float3 fill    : TEXCOORD3;\n"
		"};\n"
		"VS_OUT main(float4 pos : POSITION, float3 normal : NORMAL, float4 diffuse : COLOR0,\n"
		"            float2 uv0 : TEXCOORD0, float3 tangent : TEXCOORD1, float3 binormal : TEXCOORD2)\n"
		"{\n"
		"    VS_OUT o;\n"
		"    o.pos = float4(dot(pos, c0), dot(pos, c1), dot(pos, c2), dot(pos, c3));\n"
		"    o.diffuse = diffuse;\n"
		"    o.uv = uv0;\n"
		"    o.light = float3(dot(c4.xyz, tangent), dot(c4.xyz, binormal), dot(c4.xyz, normal));\n"
		"    float3 eT = float3(dot(c5.xyz, tangent), dot(c5.xyz, binormal), dot(c5.xyz, normal));\n"
		// GeneralsX @bugfix PS3.0-#73 16/10/2026 two-sided flip. Retail W3D building assemblies mix
		// parts whose vertex normals face INTO the mesh (Lambert probe 16/10: power plant back wall
		// reads RED toward the sun, same on the barracks back fence). Single-sided dot3 then shades
		// those faces pitch black once bump/PBR mount. Flipping the tangent-space z of L/E/F by
		// sign(dot(eyeDir, normal)) reflects the shading normal through the tangent plane -
		// mathematically identical to "turn the normal toward the camera" - without touching the PS or
		// the Lambert probe. This is the per-VERTEX stand-in for D3D's per-triangle back-face normal
		// reversal: it agrees with the fixed pipeline on any face whose two-sided decision is uniform
		// across its vertices, and only an edge-on face (where the eye test flips mid-triangle) can
		// disagree. With the default D3DCULL_CW, geometry wound away from the camera never reaches the
		// PS at all, so the flip only ever fires on inward-normal faces that ARE visible.
		"    float facing = (dot(c5.xyz, normal) < 0.0) ? -1.0 : 1.0;\n"
		"    o.light.z *= facing;\n"
		"    eT.z *= facing;\n"
		"    float3 hsum = o.light + eT;\n"
		"    o.hvec = (dot(hsum, hsum) < 1e-6) ? float3(0.0, 0.0, 1.0) : hsum;\n"  // degenerate guard: PS normalize(0) would NaN the specular
		"    float3 fT = float3(dot(c9.xyz, tangent), dot(c9.xyz, binormal), dot(c9.xyz, normal));\n"
		"    fT.z *= facing;\n"
		"    o.fill = (dot(fT, fT) < 1e-6) ? float3(0.0, 0.0, 1.0) : fT;\n"        // fill off (c9 zero): unit up-vector, c7 black makes it a no-op
		"    return o;\n"
		"}\n";

	// PS: per-pixel dot3. The normal-map texel is expanded to [-1,1] (the DX8 _bx2 modifier), the
	// in-plane deviation is scaled by c3.x for bump contrast, and the per-vertex tangent-space light
	// (from the VS) makes faces shade differently. dp3 is saturated exactly like the DX8 dp3_sat
	// bump idiom; strength/ambient arrive as device constants c1/c2.
	// GeneralsX @feature PS3.0 T1 13/10/2026: n and L are re-normalised per pixel (perturbed normals
	// lose unit length after the c3 scaling; interpolated vectors lose it after rasterisation), and a
	// Blinn-Phong term rides on the interpolated half vector: spec = pow(ndh, c6.x) * c6.y gated by ndl
	// so back faces never gain a highlight. c6 defaults keep the highlight subtle; QA knobs
	// GENERALSX_NORMALMAP_SPEC / _SPECPOWER override intensity/power from the launcher.
	// GENERALSX_NORMALMAP_NOSTAGE1=1 swaps the normal-map sample for a flat (0,0,1) normal:
	// if buildings still flicker, stage1 binding/sampling is innocent.
	static const char* kPixelShader =
		"sampler s0 : register(s0);\n"                 // diffuse (stage0, uv)
		"sampler s1 : register(s1);\n"                 // tangent-space normal map (stage1, same uv)
		"float4 c1 : register(c1);\n"                  // (strength, -, -, -)
		"float4 c2 : register(c2);\n"                  // ambient rgb
		"float4 c3 : register(c3);\n"                  // (k, 0, 1, 0): xy exaggeration, z passthrough
		"float4 c6 : register(c6);\n"                  // (specPow, specIntensity, -, -)
		"float4 c7 : register(c7);\n"                  // GeneralsX L1: sky-fill color rgb (black = off)
		"struct PS_IN\n"
		"{\n"
		"    float4 diffuse : COLOR0;\n"
		"    float2 uv      : TEXCOORD0;\n"
		"    float3 light   : TEXCOORD1;\n"
		"    float3 hvec    : TEXCOORD2;\n"
		"    float3 fill    : TEXCOORD3;\n"
		"};\n"
		"float4 main(PS_IN i) : COLOR\n"
		"{\n"
		"    float4 base = tex2D(s0, i.uv);\n"
		"    base.rgb *= i.diffuse.rgb;\n"             // vertex tint (skin team colors / baked shading; white when absent)
		"    float3 n = tex2D(s1, i.uv).xyz * 2.0 - 1.0;\n"
		"    n.xy = n.xy * c3.x + c3.y;\n"
		"    n.z  = n.z  * c3.z + c3.w;\n"
		"    n = normalize(n);\n"
		"    float3 L = normalize(i.light);\n"
		"    float ndl = saturate(dot(n, L));\n"
		"    float3 lit = ndl * c1.xxx + c2.rgb;\n"
		"    float3 F = normalize(i.fill);\n"         // GeneralsX L1: second lambert lobe from the sky fill
		"    lit += saturate(dot(n, F)) * c7.rgb;\n"  // c7 black keeps the single-key look bit-for-bit
		"    float ndh = saturate(dot(n, normalize(i.hvec)));\n"
		"    float spec = pow(ndh, c6.x) * c6.y * ndl;\n"
		// GeneralsX @bugfix Qoder 16/10/2026 T3-0 white-flicker hardening: the fixed pipeline's
		// stage-0 MODULATE multiplies the texel alpha by the VERTEX (DCG) alpha too, so an
		// ALPHATEST_ENABLE material whose draw order / opacity comes through vertex alpha (single-pass
		// wire/foliage meshes) clipped differently once dot3 mounted and output the raw texel alpha.
		// Reproduce the FF chain: base.a * i.diffuse.a. Cosmetic no-op whenever DCG alpha is 255
		// (all diffuse mounts shipped today), faithful otherwise.
		"    return float4(base.rgb * lit + spec, base.a * i.diffuse.a);\n" // albedo detail always preserved (alpha = texel x vertex, FF parity)
		"}\n";

	// Diagnostic variant: flat (0,0,1) normal - isolates stage1 sampling as the flicker source.
	static const char* kPixelShaderNoStage1 =
		"sampler s0 : register(s0);\n"
		"float4 c1 : register(c1);\n"
		"float4 c2 : register(c2);\n"
		"float4 c5 : register(c5);\n"                  // flat (0,0,1) tangent normal
		"struct PS_IN\n"
		"{\n"
		"    float4 diffuse : COLOR0;\n"
		"    float2 uv      : TEXCOORD0;\n"
		"    float3 light   : TEXCOORD1;\n"
		"};\n"
		"float4 main(PS_IN i) : COLOR\n"
		"{\n"
		"    float4 base = tex2D(s0, i.uv);\n"
		"    base.rgb *= i.diffuse.rgb;\n"
		"    float3 lit = saturate(dot(c5.xyz, i.light)) * c1.xxx + c2.rgb;\n"
		"    return float4(base.rgb * lit, base.a * i.diffuse.a);\n"   // GeneralsX @bugfix Qoder 16/10/2026 FF-parity alpha (see kPixelShader)
		"}\n";

	// GeneralsX @debug T3-0 raw-visual probe: emits the sampled normal map directly. Any mesh
	// that mounts the dot3 shader renders blue-purple (flat texel = ~128,128,255) which is
	// unmistakable on screen; meshes without a companion keep their normal fixed-pipeline look.
	// Selected at device init by GENERALSX_NORMALMAP_RAW=1 (launcher switch, no rebuild).
	static const char* kPixelShaderRaw =
		"sampler s0 : register(s0);\n"                 // keep the diffuse alpha so alpha-tested surfaces still clip
		"sampler s1 : register(s1);\n"                 // normal map (stage1)
		"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
		"{\n"
		"    float4 base = tex2D(s0, uv);\n"
		"    float3 n = tex2D(s1, uv).xyz * 2.0 - 1.0;\n"
		"    return float4(n, base.a);\n"              // raw tangent-space normal visualization
		"}\n";

	// GeneralsX @debug PS3.0 15/10/2026 Lambert goniometer probe (see the LAMBERT env block below).
	// v2 (same day): v1 painted the fill lobe into G, but with the sky fill off the VS guard feeds
	// fill=(0,0,1)=tangent up, so EVERY surface read G~1 and the screen washed green over red.
	// v2 is a sign view of the KEY-LOBE only (R = faces the sun, B = faces away, near-black = edge-on,
	// G stays 0); "two-lobe" was v1's wording - the fill lobe is gone from this source.
	// PS_IN declares only COLOR0/TEXCOORD0/1: a valid subset of BOTH the dot3 VS and the PBR VS
	// outputs, so this one source swaps into either compile site.
	// GeneralsX @bugfix PS3.0-#73 16/10/2026 note: this probe reuses the shipped VS, so since the
	// two-sided flip landed it reports the FLIPPED shading normal - and only on the faces the flip
	// fires on: outward normals keep sign(dot(eye,normal) > 0) = +1 and look exactly as before, while
	// inward-normal faces (the ones facing= -1 reflects) swap RED <-> BLUE. The raw asset view survives
	// in GENERALSX_GEOMNORMAL_RAW (kPixelShaderGNRaw, untouched single-sided).
	// GeneralsX @bugfix PS3.0-#76b: the pre/flip comparisons recorded for #73 were made while the
	// model-space c4 light still carried the mesh's doubled rotation, so a swapped RED/BLUE reading
	// may have been the matrix step rather than the flip. Recapture on the fixed build to re-read.
	static const char* kPixelShaderLambertRaw =
		"sampler s0 : register(s0);\n"                 // diffuse for the alpha clip
		"sampler s1 : register(s1);\n"                 // tangent-space normal map
		"float4 c3 : register(c3);\n"                  // (k, 0, 1, 0): same bump scaling as the shipped PS
		"struct PS_IN\n"
		"{\n"
		"    float4 diffuse : COLOR0;\n"
		"    float2 uv      : TEXCOORD0;\n"
		"    float3 light   : TEXCOORD1;\n"
		"};\n"
		"float4 main(PS_IN i) : COLOR\n"
		"{\n"
		"    float4 base = tex2D(s0, i.uv);\n"
		"    float3 n = tex2D(s1, i.uv).xyz * 2.0 - 1.0;\n"
		"    n.xy = n.xy * c3.x + c3.y;\n"
		"    n.z  = n.z  * c3.z + c3.w;\n"
		"    n = normalize(n);\n"
		"    float d = dot(n, normalize(i.light));\n"
		"    return float4(saturate(d), 0.0, saturate(-d), base.a);\n"
		"}\n";

	bool nmLambertProbe = false;   // set by the LAMBERT env branch, consumed at the PBR compile site
	const char* pixelShaderSrc = kPixelShader;
	{
		const char* rawEnv = getenv("GENERALSX_NORMALMAP_RAW");
		if (rawEnv != nullptr && rawEnv[0] != 0 && strcmp(rawEnv, "0") != 0) {
			pixelShaderSrc = kPixelShaderRaw;
			fprintf(stderr, "[DOT3] raw-visual mode ON (emitting normal map directly)\n");
			fflush(stderr);
		} else {
			// GeneralsX @debug PS3.0 15/10/2026 (Qoder): Lambert goniometer probe for the "building lit
			// from its shadow side" report. The NORMALMAP_RAW probe shows the texel, which cannot answer
			// whether the mesh's TBN basis points outward; this one shows the ACTUAL lighting angle.
			// v2 coding (see kPixelShaderLambertRaw): R = the tangent-space normal faces the key light,
			// B = it faces away, G is unused - the v1 "R=key lobe, G=fill lobe" note in the ON message
			// below is left over from v1 and no longer matches what the probe paints.
			// What a RED wall means, corrected after #76b: it shows dot(bumpN, tangentSpaceLight) > 0,
			// and the light got into tangent space through the VS-side TBN and the model-space c4 upload.
			// So red-on-a-shadowed-face has TWO possible readings - an inward vertex normal, or a c4 that
			// was rotated into the wrong model frame (that was #76b, and it made the whole lobe pattern
			// ride on the mesh's placement angle). The probe cannot tell those apart on its own; only
			// the DOT3-vs-fixed-pipeline comparison can. The old wording claimed it "proved" the normal
			// was inverted, which is what sent #73 hunting an asset defect instead of the matrix step.
			// Covers both mounted pairs: swapped in here for the plain dot3 PS and at the PBR compile
			const char* lamEnv = getenv("GENERALSX_NORMALMAP_LAMBERT");
			if (lamEnv != nullptr && lamEnv[0] != 0 && strcmp(lamEnv, "0") != 0) {
				pixelShaderSrc = kPixelShaderLambertRaw;
				nmLambertProbe = true;
				fprintf(stderr, "[DOT3] lambert-goniometer mode ON (R=key lobe, G=fill lobe)\n");
				fflush(stderr);
			} else {
				const char* ns1Env = getenv("GENERALSX_NORMALMAP_NOSTAGE1");
				if (ns1Env != nullptr && ns1Env[0] != 0 && strcmp(ns1Env, "0") != 0) {
					pixelShaderSrc = kPixelShaderNoStage1;
					fprintf(stderr, "[DOT3] NOSTAGE1 diag mode ON (flat normal, no stage1 sample)\n");
					fflush(stderr);
				}
			}
		}
	}

	// GeneralsX @feature PS3.0-P4-1 15/10/2026 PBR (roughness/metalness) variant pair for the dot3
	// single-light path (光影三阶段/粗糙度引擎承接工作清单.md M1+M2).  Uniform-branched so one pair
	// covers both rollout steps; the GN merged path mounts it only when the roughness toggle is on AND the
	// diffuse carries a "<stem>_rp" companion (R=roughness, G=metalness, contract §1).
	// VS: identical to kVertexShader except it also emits the tangent-space eye vector eT as
	// TEXCOORD4 - the GGX branch needs NdotV/VdotH per pixel, and the main pair only folds eT into
	// the half vector.  c0..c5/c9 layout is unchanged, so Apply's constant uploads are reused as-is.
	static const char* kVertexShaderPBR =
		"float4 c0 : register(c0);\n"
		"float4 c1 : register(c1);\n"
		"float4 c2 : register(c2);\n"
		"float4 c3 : register(c3);\n"
		"float4 c4 : register(c4);\n"
		"float4 c5 : register(c5);\n"
		"float4 c9 : register(c9);\n"
		"struct VS_OUT\n"
		"{\n"
		"    float4 pos     : POSITION;\n"
		"    float4 diffuse : COLOR0;\n"
		"    float2 uv      : TEXCOORD0;\n"
		"    float3 light   : TEXCOORD1;\n"
		"    float3 hvec    : TEXCOORD2;\n"
		"    float3 fill    : TEXCOORD3;\n"
		"    float3 eye     : TEXCOORD4;\n"
		"};\n"
		"VS_OUT main(float4 pos : POSITION, float3 normal : NORMAL, float4 diffuse : COLOR0,\n"
		"            float2 uv0 : TEXCOORD0, float3 tangent : TEXCOORD1, float3 binormal : TEXCOORD2)\n"
		"{\n"
		"    VS_OUT o;\n"
		"    o.pos = float4(dot(pos, c0), dot(pos, c1), dot(pos, c2), dot(pos, c3));\n"
		"    o.diffuse = diffuse;\n"
		"    o.uv = uv0;\n"
		"    o.light = float3(dot(c4.xyz, tangent), dot(c4.xyz, binormal), dot(c4.xyz, normal));\n"
		"    float3 eT = float3(dot(c5.xyz, tangent), dot(c5.xyz, binormal), dot(c5.xyz, normal));\n"
		// GeneralsX @bugfix PS3.0-#73 16/10/2026 two-sided flip (mirrors kVertexShader; see the note
		// there). Flip applied BEFORE o.eye/hsum so V and H stay consistent with the reflected N.
		"    float facing = (dot(c5.xyz, normal) < 0.0) ? -1.0 : 1.0;\n"
		"    o.light.z *= facing;\n"
		"    eT.z *= facing;\n"
		"    o.eye = (dot(eT, eT) < 1e-6) ? float3(0.0, 0.0, 1.0) : eT;\n"          // degenerate guard: PS normalize(0) would NaN V
		"    float3 hsum = o.light + eT;\n"
		"    o.hvec = (dot(hsum, hsum) < 1e-6) ? float3(0.0, 0.0, 1.0) : hsum;\n"
		"    float3 fT = float3(dot(c9.xyz, tangent), dot(c9.xyz, binormal), dot(c9.xyz, normal));\n"
		"    fT.z *= facing;\n"
		"    o.fill = (dot(fT, fT) < 1e-6) ? float3(0.0, 0.0, 1.0) : fT;\n"
		"    return o;\n"
		"}\n";

	// PS: c8 = (mode, LI, ENV, wetness), mode from the env switch (DX8NormalMap::PBR_Mode):
	//   < 0.5 : the plain T1/L1 Blinn path, instruction-for-instruction (dead in practice - Apply
	//           never mounts this pair at mode 0 - kept as an exact-parity escape hatch for QA).
	//   1 (M1): roughness-driven Blinn only, per 工作清单 §2 M1: shininess = lerp(8,128,1-R),
	//           spec intensity = c6.y*(1-R); the diffuse/fill lobes stay the shipped dot3 model.
	//   2 (M2): simplified GGX metallic-roughness: F0 = lerp(0.04, albedo, M), Schlick fresnel,
	//           Smith-Schlick G, kd = (1-F)(1-M) (metal suppresses diffuse), GGX NDF in place of
	//           pow (no 4*ndl*vnd normalisation - folded into the 0.25 factor and the LI scaler);
	//           environment = constant hemisphere c8.z*F0 (no IBL, RTS scale per §2 M2).  The
	//           lambert/ambient/fill scalar "lit" is reused as the irradiance stand-in so the look
	//           stays calibrated to the shipped lighting; LI (c8.y) rebalances the darker kd.
	// Alpha is always the diffuse alpha (team-colour mask clipping contract preserved).
	static const char* kPixelShaderPBR =
		"sampler s0 : register(s0);\n"                 // diffuse (stage0, uv)
		"sampler s1 : register(s1);\n"                 // tangent-space normal map (stage1)
		"sampler s2 : register(s2);\n"                 // rp companion: R=roughness, G=metalness (stage2)
		"float4 c1 : register(c1);\n"                  // (strength, -, -, -)
		"float4 c2 : register(c2);\n"                  // ambient rgb
		"float4 c3 : register(c3);\n"                  // (k, 0, 1, 0): bump xy/z scaling
		"float4 c6 : register(c6);\n"                  // (specPow, specIntensity, -, -)
		"float4 c7 : register(c7);\n"                  // sky-fill color rgb
		"float4 c8 : register(c8);\n"                  // (mode, LI, ENV, wetness)
		"struct PS_IN\n"
		"{\n"
		"    float4 diffuse : COLOR0;\n"
		"    float2 uv      : TEXCOORD0;\n"
		"    float3 light   : TEXCOORD1;\n"
		"    float3 hvec    : TEXCOORD2;\n"
		"    float3 fill    : TEXCOORD3;\n"
		"    float3 eye     : TEXCOORD4;\n"
		"};\n"
		"float4 main(PS_IN i) : COLOR\n"
		"{\n"
		"    float4 base = tex2D(s0, i.uv);\n"
		"    base.rgb *= i.diffuse.rgb;\n"
		"    float3 n = tex2D(s1, i.uv).xyz * 2.0 - 1.0;\n"
		"    n.xy = n.xy * c3.x + c3.y;\n"
		"    n.z  = n.z  * c3.z + c3.w;\n"
		"    n = normalize(n);\n"
		"    float3 L = normalize(i.light);\n"
		"    float ndl = saturate(dot(n, L));\n"
		"    float3 lit = ndl * c1.xxx + c2.rgb;\n"
		"    lit += saturate(dot(n, normalize(i.fill))) * c7.rgb;\n"
		"    if (c8.x < 0.5)\n"
		"    {\n"
		"        float ndh0 = saturate(dot(n, normalize(i.hvec)));\n"
		"        float spec0 = pow(ndh0, c6.x) * c6.y * ndl;\n"
		"        return float4(base.rgb * lit + spec0, base.a * i.diffuse.a);\n"   // GeneralsX @bugfix Qoder 16/10/2026 FF-parity alpha (see kPixelShader)
		"    }\n"
		"    float2 rm = tex2D(s2, i.uv).xy;\n"        // R=roughness, G=metalness (0..1, packer §1)
		"    rm.x = rm.x * (1.0 - c8.w * 0.75);\n"    // GeneralsX @feature PS3.0 Weather W5: rain-slick. A wet film flattens the micro-surface, so roughness drops and every roughness-driven lobe below tightens. c8.w = 0 leaves this product numerically identical to before.
		"    if (c8.x < 1.5)\n"
		"    {\n"
		"        float shin = lerp(8.0, 128.0, 1.0 - rm.x);\n"
		"        float ndh1 = saturate(dot(n, normalize(i.hvec)));\n"
		"        float spec1 = pow(ndh1, shin) * c6.y * (1.0 - rm.x) * ndl;\n"
		"        return float4(base.rgb * lit + spec1, base.a * i.diffuse.a);\n"   // GeneralsX @bugfix Qoder 16/10/2026 FF-parity alpha (see kPixelShader)
		"    }\n"
		"    float3 F0 = lerp(float3(0.04, 0.04, 0.04), base.rgb, rm.y);\n"
		"    float3 V = normalize(i.eye);\n"
		"    float3 H = normalize(i.hvec);\n"
		"    float ndh = saturate(dot(n, H));\n"
		"    float vnd = max(dot(n, V), 0.05);\n"       // clamp: grazing V would divide out the G term
		"    float vnh = saturate(dot(V, H));\n"
		"    float3 fres = F0 + (1.0 - F0) * pow(1.0 - vnh, 5.0);\n"
		"    float3 kd = (1.0 - fres) * (1.0 - rm.y);\n"
		"    float a = rm.x * rm.x;\n"                  // alpha = rough^2
		"    float a2 = a * a;\n"
		"    float dnom = ndh * ndh * (a2 - 1.0) + 1.0;\n"
		"    float D = a2 / max(dnom * dnom, 1e-4);\n"
		"    float kk = (rm.x + 1.0) * (rm.x + 1.0) * 0.125;\n"
		"    float G = (ndl / max(ndl * (1.0 - kk) + kk, 1e-3)) * (vnd / max(vnd * (1.0 - kk) + kk, 1e-3));\n"
		"    float3 spec = D * fres * G * 0.25;\n"
		"    float3 col = base.rgb * kd * lit * c8.y + c8.z * F0 + spec * c8.y * ndl;\n"
		"    return float4(col, base.a * i.diffuse.a);\n"   // GeneralsX @bugfix Qoder 16/10/2026 FF-parity alpha (see kPixelShader)
		"}\n";

	// GeneralsX @feature PS3.0 batch7 13/10/2026 the pair is emitted through the shared ladder: on a
	// device that accepts it this compiles as vs_3_0/ps_3_0 (flow control, 32 temporaries, 224 pixel
	// constants) and drops back to the previous vs_2_0/ps_2_0 automatically if the runtime refuses the
	// newer profile. The sources above no longer carry a "// d3d-target:" line because the ladder decides.
	IDirect3DVertexShader9* vsHandle = nullptr;
	IDirect3DPixelShader9* psHandle = nullptr;
	const char* vsProfile = nullptr;
	const char* psProfile = nullptr;

	HRESULT hr = W3DShaderManager::compileShaderAtBestProfile(kVertexShader, true, "[DOT3]", &vsProfile, (void**)&vsHandle);
	fprintf(stderr, "[DOT3] vertex shader %s hr=0x%08X handle=%p\n",
		vsProfile ? vsProfile : "not compiled", (unsigned)hr, (void*)vsHandle);

	hr = W3DShaderManager::compileShaderAtBestProfile(pixelShaderSrc, false, "[DOT3]", &psProfile, (void**)&psHandle);
	fprintf(stderr, "[DOT3] pixel shader %s hr=0x%08X handle=%p\n",
		psProfile ? psProfile : "not compiled", (unsigned)hr, (void*)psHandle);
	fflush(stderr);

	if (vsHandle != nullptr && psHandle != nullptr) {
		// GeneralsX @build d3d9 port 11/10/2026 - UINT_PTR carries the interface pointer losslessly on x64.
		DX8NormalMap::Install((UINT_PTR)vsHandle, (UINT_PTR)psHandle);

		// GeneralsX @feature PS3.0-P4-1 15/10/2026 / #70 Qoder 15/10/2026: compile the PBR (roughness/
		// metalness) variant UNCONDITIONALLY.  Roughness became an independent runtime toggle (options
		// menu "RoughnessMapping" -> DX8NormalMap::Set_PBR_Mode), so the shaders must exist even when
		// the process started without GENERALSX_PBR; the menu/env decides mounting in Apply, not here.
		// Failure stays purely additive: the GN merged path simply keeps its plain (non-PBR) base.
		{
			{
				IDirect3DVertexShader9* pbrVS = nullptr;
				IDirect3DPixelShader9* pbrPS = nullptr;
				const char* pbrVSProfile = nullptr;
				const char* pbrPSProfile = nullptr;
				HRESULT pbrHR = W3DShaderManager::compileShaderAtBestProfile(kVertexShaderPBR, true, "[PBR]", &pbrVSProfile, (void**)&pbrVS);
				if (SUCCEEDED(pbrHR) && pbrVS != nullptr) {
					// Lambert goniometer probe swaps the PBR pixel shader too - roughness-equipped
					// buildings mount this pair, not the plain dot3 one (see the LAMBERT env block).
					const char* pbrPsSrc = nmLambertProbe ? kPixelShaderLambertRaw : kPixelShaderPBR;
					pbrHR = W3DShaderManager::compileShaderAtBestProfile(pbrPsSrc, false, "[PBR]", &pbrPSProfile, (void**)&pbrPS);
				}
				if (SUCCEEDED(pbrHR) && pbrVS != nullptr && pbrPS != nullptr) {
					DX8NormalMap::Install_PBR((UINT_PTR)pbrVS, (UINT_PTR)pbrPS);
					fprintf(stderr, "[PBR] variant compiled vs=%s ps=%s (mode set at option install below)\n", pbrVSProfile, pbrPSProfile);
				} else {
					if (pbrVS != nullptr) pbrVS->Release();
					if (pbrPS != nullptr) pbrPS->Release();
					fprintf(stderr, "[PBR] variant compile failed (hr=0x%08X), single-light path only\n", (unsigned)pbrHR);
				}
				fflush(stderr);
			}
		}

		// GeneralsX @feature PS3.0 L2 14/10/2026 geometric-normal per-pixel diffuse pair.
		// The GNprobe runs showed the battlefield rigid batches carry FVF 0x112 = DX8_FVF_XYZNUV1
		// (position+normal+one UV, NO vertex-color channel): they are realtime-lit and their per-mesh
		// LightEnvironment collects up to 3 directional lights (sun + two fills, from the map's
		// TerrainObjectsLighting*). A two-light "correction" cannot reproduce that, so these programs
		// instead rebuild the fixed pipeline's Gouraud sum for an ARBITRARY set of world directional
		// lights and evaluate it per-pixel: the VS transforms position + the world normal only (a custom
		// VS bypasses fixed-function lighting, so nothing else can be read from the stream); the PS sums
		//     col = ambient + sum_i (matDiffuse*lightColor_i) * saturate(dot(n, lightDir_i))
		// using the SAME Get_Equivalent_Ambient / Get_Light_Diffuse / Get_Light_Direction accessors the
		// fixed Set_Light_Environment path (dx8wrapper.cpp) feeds to D3D, so it matches retail lighting
		// exactly and only upgrades the Lambert term from per-vertex to per-pixel. Colors/ambient are
		// material-scaled on the CPU inside DX8GeomNormal::Apply, which reads the batch's own lenv.
		// Mounting is gated inside DX8GeomNormal (env switch + FVF in {XYZNUV1,XYZNUV2} + every light
		// directional and count<=4), so point/spot-lit props and vertex-colored batches stay FF.
		{
			// Two full VS sources because D3D9 input declarations must match the batch FVF
			// exactly; only the parameter list differs (NUV2 carries an extra TEXCOORD1).
			static const char* kVertexShaderGN_NUV1 =
				"float4 c0 : register(c0);\n"
				"float4 c1 : register(c1);\n"
				"float4 c2 : register(c2);\n"
				"float4 c3 : register(c3);\n"
				"float4 c4 : register(c4);\n"
				"float4 c5 : register(c5);\n"
				"float4 c6 : register(c6);\n"	// world rotation rows (object normal -> world)
				"struct VS_OUT\n"
				"{\n"
				"    float4 pos : POSITION;\n"
				"    float2 uv  : TEXCOORD0;\n"
				"    float3 wn  : TEXCOORD1;\n"
				"    float eye  : TEXCOORD2;\n"             // batch8 P0: clip w = eye depth; POSITION PS inputs are rejected by fxc (X4502)
				"};\n"
				"VS_OUT main(float3 pos : POSITION, float3 normal : NORMAL, float2 uv0 : TEXCOORD0)\n"
				"{\n"
				"    VS_OUT o;\n"
				"    float4 p = float4(pos, 1.0);\n"
				"    o.pos = float4(dot(p, c0), dot(p, c1), dot(p, c2), dot(p, c3));\n"
				"    o.uv = uv0;\n"
				"    o.wn = float3(dot(normal, c4.xyz), dot(normal, c5.xyz), dot(normal, c6.xyz));\n"
				"    o.eye = o.pos.w;\n"                    // batch8 P0: relay clip w so the PS fog sees eye depth
				"    return o;\n"
				"}\n";

			static const char* kVertexShaderGN_NUV2 =
				"float4 c0 : register(c0);\n"
				"float4 c1 : register(c1);\n"
				"float4 c2 : register(c2);\n"
				"float4 c3 : register(c3);\n"
				"float4 c4 : register(c4);\n"
				"float4 c5 : register(c5);\n"
				"float4 c6 : register(c6);\n"	// world rotation rows (object normal -> world)
				"struct VS_OUT\n"
				"{\n"
				"    float4 pos : POSITION;\n"
				"    float2 uv  : TEXCOORD0;\n"
				"    float3 wn  : TEXCOORD1;\n"
				"    float eye  : TEXCOORD2;\n"             // batch8 P0: clip w = eye depth; POSITION PS inputs are rejected by fxc (X4502)
				"};\n"
				"VS_OUT main(float3 pos : POSITION, float3 normal : NORMAL, float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1)\n"
				"{\n"
				"    VS_OUT o;\n"
				"    float4 p = float4(pos, 1.0);\n"
				"    o.pos = float4(dot(p, c0), dot(p, c1), dot(p, c2), dot(p, c3));\n"
				"    o.uv = uv0;\n"
				"    o.wn = float3(dot(normal, c4.xyz), dot(normal, c5.xyz), dot(normal, c6.xyz));\n"
				"    o.eye = o.pos.w;\n"                    // batch8 P0: relay clip w so the PS fog sees eye depth
				"    return o;\n"
				"}\n";

			// PS: per-vertex geometry normal re-light (the "flat normal" dot3 stand-in).
			// GeneralsX @bugfix Qoder 16/10/2026: this family intentionally keeps the raw texel
			// alpha (no * i.diffuse.a FF-parity chain like the dot3 pair above) because the GN
			// vertex shaders carry no COLOR0 at all - wiring vertex alpha in would change the VS
			// input/output signature and register budget for a cosmetic no-op on today's assets.
			static const char* kPixelShaderGN =
				"sampler s0 : register(s0);\n"                 // diffuse texture (bound by the category)
				"float4 c4 : register(c4);\n"                  // ambient: matAmbient*envAmbient + emissive
				"float4 c5 : register(c5);\n"                  // world light dir 0 (toward light)
				"float4 c6 : register(c6);\n"                  // world light dir 1
				"float4 c7 : register(c7);\n"                  // world light dir 2
				"float4 c8 : register(c8);\n"                  // world light dir 3
				"float4 c9 : register(c9);\n"                  // matDiffuse*lightColor 0
				"float4 c10 : register(c10);\n"                // matDiffuse*lightColor 1
				"float4 c11 : register(c11);\n"                // matDiffuse*lightColor 2
				"float4 c12 : register(c12);\n"                // matDiffuse*lightColor 3
				"float4 c13 : register(c13);\n"                // world eye dir (toward camera), w = spec power
				"float4 c14 : register(c14);\n"                // spec intensity rgb (light 0 only, T1 knobs)
				"float4 c16 : register(c16);\n"                // hemi ambient rgb (P3-12), scaled by world-up n.z (W3D is Z-up)
				"float4 c17 : register(c17);\n"                // world light dir 4 (P3-18 widened the GN family to 8 lights)
				"float4 c18 : register(c18);\n"                // world light dir 5
				"float4 c19 : register(c19);\n"                // world light dir 6
				"float4 c20 : register(c20);\n"                // world light dir 7
				"float4 c21 : register(c21);\n"                // matDiffuse*lightColor 4
				"float4 c22 : register(c22);\n"                // matDiffuse*lightColor 5
				"float4 c23 : register(c23);\n"                // matDiffuse*lightColor 6
				"float4 c24 : register(c24);\n"                // matDiffuse*lightColor 7
				"float4 c25 : register(c25);\n"                // P3-14 fog: rgb haze color, w = blend strength (0 = identity)
				"float4 c26 : register(c26);\n"                // P3-14 fog: x = eye-depth start, y = 1/ramp length
				"struct PS_IN\n"
				"{\n"
				"    float eye : TEXCOORD2;\n"                  // batch8 P0: eye depth relayed by the VS (POSITION input = X4502)
				"    float2 uv : TEXCOORD0;\n"
				"    float3 wn : TEXCOORD1;\n"
				"};\n"
				"float4 main(PS_IN i) : COLOR\n"
				"{\n"
				"    float4 base = tex2D(s0, i.uv);\n"
				"    float l = length(i.wn);\n"
				"    float3 n = (l > 1e-6) ? i.wn / l : float3(0.0, 1.0, 0.0);\n"
				// GeneralsX @bugfix PS3.0-#73 16/10/2026 two-sided flip: retail building assemblies mix
				// parts with vertex normals facing INTO the mesh (Lambert probe: power plant back wall reads
				// red toward the sun). c13.xyz is the world dir toward the camera, so reflect any normal
				// facing away from the viewer through its tangent plane before lighting. Matches the
				// dot3/PBR VS-side flip (kVertexShader) and lets every visible surface catch its lobes.
				"    n *= (dot(n, c13.xyz) < 0.0) ? -1.0 : 1.0;\n"
				"    float ndl0 = saturate(dot(n, c5.xyz));\n"
				"    float3 col = c4.rgb + n.z * c16.rgb\n"
				"        + c9.rgb  * ndl0\n"
				"        + c10.rgb * saturate(dot(n, c6.xyz))\n"
				"        + c11.rgb * saturate(dot(n, c7.xyz))\n"
				"        + c12.rgb * saturate(dot(n, c8.xyz))\n"
				"        + c21.rgb * saturate(dot(n, c17.xyz))\n"
				"        + c22.rgb * saturate(dot(n, c18.xyz))\n"
				"        + c23.rgb * saturate(dot(n, c19.xyz))\n"
				"        + c24.rgb * saturate(dot(n, c20.xyz));\n"
				"    float3 h = normalize(c5.xyz + normalize(c13.xyz));\n"
				"    float3 spec = c14.rgb * pow(saturate(dot(n, h)), c13.w) * ndl0;\n"
				"    float4 lit = float4(base.rgb * saturate(col) + spec, base.a);\n"
				"    float fogf = saturate((i.eye - c26.x) * c26.y) * c25.w;\n"   // P3-14: eye-depth haze, w=0 keeps the pixel exact (batch8 P0: VS-relayed TEXCOORD depth, POSITION inputs are X4502)
				"    return lerp(lit, float4(c25.rgb, lit.a), fogf);\n"
				"}\n";

			// GeneralsX @feature Qoder 17/10/2026 P2 skinned-batch variants of the plain GN pair
			// (光影T0阶段 P2). The CPU skinning pass submits the dynamic layout
			// (dynamic_fvf_type = D3DFVF_XYZ|NORMAL|DIFFUSE|TEX2), whose COLOR0 channel carries the
			// per-vertex tint / selection-damage flash / baked detail and the alpha the fixed pipeline
			// multiplies into the output (VertexFormatXYZNDUV2 in dx8renderer.cpp's skin Render loop).
			// The rigid VS pair declares no COLOR0, so D3D9 would silently drop it and skinned batches
			// used to stay on the fixed pipeline. This VS relays the vertex diffuse untouched; the PS
			// is the plain GN program with the same FF-parity chain the dot3 pair ships
			// (base.rgb *= i.diffuse.rgb, alpha = base.a * i.diffuse.a). The sun-shadow variants are
			// NOT duplicated for skins in P2 (DX8GeomNormal::Apply keeps skins on the plain pair;
			// the merge lands in P3).
			static const char* kVertexShaderGN_SKIN =
				"float4 c0 : register(c0);\n"
				"float4 c1 : register(c1);\n"
				"float4 c2 : register(c2);\n"
				"float4 c3 : register(c3);\n"
				"float4 c4 : register(c4);\n"
				"float4 c5 : register(c5);\n"
				"float4 c6 : register(c6);\n"	// world rotation rows (object normal -> world; identity for skins)
				"struct VS_OUT\n"
				"{\n"
				"    float4 pos : POSITION;\n"
				"    float2 uv  : TEXCOORD0;\n"
				"    float3 wn  : TEXCOORD1;\n"
				"    float eye  : TEXCOORD2;\n"             // batch8 P0: clip w = eye depth; POSITION PS inputs are rejected by fxc (X4502)
				"    float4 diffuse : COLOR0;\n"            // P2: vertex tint/flash relayed to the PS (FF diffuse channel)
				"};\n"
				"VS_OUT main(float3 pos : POSITION, float3 normal : NORMAL, float4 diffuse : COLOR0, float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1)\n"
				"{\n"
				"    VS_OUT o;\n"
				"    float4 p = float4(pos, 1.0);\n"
				"    o.pos = float4(dot(p, c0), dot(p, c1), dot(p, c2), dot(p, c3));\n"
				"    o.uv = uv0;\n"
				"    o.wn = float3(dot(normal, c4.xyz), dot(normal, c5.xyz), dot(normal, c6.xyz));\n"
				"    o.eye = o.pos.w;\n"
				"    o.diffuse = diffuse;\n"
				"    return o;\n"
				"}\n";

			static const char* kPixelShaderGNSkin =
				"sampler s0 : register(s0);\n"                 // diffuse texture (bound by the category)
				"float4 c4 : register(c4);\n"                  // ambient: matAmbient*envAmbient + emissive
				"float4 c5 : register(c5);\n"                  // world light dir 0 (toward light)
				"float4 c6 : register(c6);\n"                  // world light dir 1
				"float4 c7 : register(c7);\n"                  // world light dir 2
				"float4 c8 : register(c8);\n"                  // world light dir 3
				"float4 c9 : register(c9);\n"                  // matDiffuse*lightColor 0
				"float4 c10 : register(c10);\n"                // matDiffuse*lightColor 1
				"float4 c11 : register(c11);\n"                // matDiffuse*lightColor 2
				"float4 c12 : register(c12);\n"                // matDiffuse*lightColor 3
				"float4 c13 : register(c13);\n"                // world eye dir (toward camera), w = spec power
				"float4 c14 : register(c14);\n"                // spec intensity rgb (light 0 only, T1 knobs)
				"float4 c16 : register(c16);\n"                // hemi ambient rgb (P3-12), scaled by world-up n.z (W3D is Z-up)
				"float4 c17 : register(c17);\n"                // world light dir 4 (P3-18 widened the GN family to 8 lights)
				"float4 c18 : register(c18);\n"
				"float4 c19 : register(c19);\n"
				"float4 c20 : register(c20);\n"
				"float4 c21 : register(c21);\n"                // matDiffuse*lightColor 4
				"float4 c22 : register(c22);\n"
				"float4 c23 : register(c23);\n"
				"float4 c24 : register(c24);\n"
				"float4 c25 : register(c25);\n"                // P3-14 fog: rgb haze color, w = blend strength (0 = identity)
				"float4 c26 : register(c26);\n"                // P3-14 fog: x = eye-depth start, y = 1/ramp length
				"struct PS_IN\n"
				"{\n"
				"    float eye : TEXCOORD2;\n"                  // batch8 P0: eye depth relayed by the VS (POSITION input = X4502)
				"    float2 uv : TEXCOORD0;\n"
				"    float3 wn : TEXCOORD1;\n"
				"    float4 diffuse : COLOR0;\n"                // P2: vertex diffuse stream (white + a255 on un-tinted assets)
				"};\n"
				"float4 main(PS_IN i) : COLOR\n"
				"{\n"
				"    float4 base = tex2D(s0, i.uv);\n"
				"    base.rgb *= i.diffuse.rgb;\n"             // FF parity: vertex tint multiplies the albedo (dot3 pair precedent)
				"    float l = length(i.wn);\n"
				"    float3 n = (l > 1e-6) ? i.wn / l : float3(0.0, 1.0, 0.0);\n"
				// Two-sided flip identical to kPixelShaderGN (#73): infantry/building skins ship the
				// same mixed-facing hull parts, so the viewer-facing rule must carry over verbatim.
				"    n *= (dot(n, c13.xyz) < 0.0) ? -1.0 : 1.0;\n"
				"    float ndl0 = saturate(dot(n, c5.xyz));\n"
				"    float3 col = c4.rgb + n.z * c16.rgb\n"
				"        + c9.rgb  * ndl0\n"
				"        + c10.rgb * saturate(dot(n, c6.xyz))\n"
				"        + c11.rgb * saturate(dot(n, c7.xyz))\n"
				"        + c12.rgb * saturate(dot(n, c8.xyz))\n"
				"        + c21.rgb * saturate(dot(n, c17.xyz))\n"
				"        + c22.rgb * saturate(dot(n, c18.xyz))\n"
				"        + c23.rgb * saturate(dot(n, c19.xyz))\n"
				"        + c24.rgb * saturate(dot(n, c20.xyz));\n"
				"    float3 h = normalize(c5.xyz + normalize(c13.xyz));\n"
				"    float3 spec = c14.rgb * pow(saturate(dot(n, h)), c13.w) * ndl0;\n"
				"    float4 lit = float4(base.rgb * saturate(col) + spec, base.a * i.diffuse.a);\n"   // P2: FF-parity alpha (texel x vertex), see dot3 pair
				"    float fogf = saturate((i.eye - c26.x) * c26.y) * c25.w;\n"   // P3-14: eye-depth haze, w=0 keeps the pixel exact
				"    return lerp(lit, float4(c25.rgb, lit.a), fogf);\n"
				"}\n";

			// GeneralsX @debug PS3.0 L2 coverage probe: GENERALSX_GEOMNORMAL_RAW=1 swaps the PS for
			// one that emits the key-light lobe SIGN view - RED = normal faces light0 (sun), BLUE =
			// faces away, near-black = edge-on. v2 15/10/2026: the old R=l0/G=l1 coding washed green
			// over everything (fill lobe reads high on all surfaces), this matches the dot3 LAMBERT
			// probe so both shader families tell the same story. Also tells "GN never mounts" apart
			// from "GN mounts but the change is invisible" - unmounted meshes keep ordinary colors.
			// GeneralsX @bugfix PS3.0-#73 16/10/2026 note: this PS deliberately keeps NO two-sided flip
			// (unlike kPixelShaderGN), so it stays the raw goniometer of the asset's own vertex normal -
			// flipping in kPixelShaderGN does not hide the inward-normal cases from here.
			// GeneralsX @bugfix PS3.0-#76b 16/10/2026 correction: the "power plant back walls stay RED
			// even after the shipped shaders started flipping" reading was captured while the GN VS
			// c4..c6 upload still rotated the normal by the transposed world matrix, so every lobe angle
			// here carried the mesh's 2*theta error. Re-capture this probe before judging any asset
			// normal direction from it; on the fixed build RED means the true world normal faces the sun.
			static const char* kPixelShaderGNRaw =
				"sampler s0 : register(s0);\n"
				"float4 c5 : register(c5);\n"
				"struct PS_IN\n"
				"{\n"
				"    float2 uv : TEXCOORD0;\n"
				"    float3 wn : TEXCOORD1;\n"
				"};\n"
				"float4 main(PS_IN i) : COLOR\n"
				"{\n"
				"    float4 base = tex2D(s0, i.uv);\n"
				"    float l = length(i.wn);\n"
				"    float3 n = (l > 1e-6) ? i.wn / l : float3(0.0, 1.0, 0.0);\n"
				"    float d = dot(n, c5.xyz);\n"
				"    return float4(saturate(d), 0.0, saturate(-d), base.a);\n"
				"}\n";

			// GeneralsX @feature PS3.0 P3-21b 14/10/2026 sun-shadow reception variants of the GN pair.
			// The terrain receiver's post-scene raw draw cannot sample ANY bound texture on this runtime
			// (RECMODE 5/6 discriminator 14/10: constant-PS fine, RT map AND plain A8R8G8B8 dummy echo
			// read 0), while the GN pass - programmable VS + wrapper-bound textures - samples correctly.
			// So these variants add the sun camera mapping (VS c7..c10, uploaded per batch by
			// DX8GeomNormal::Apply) and a 3x3 PCF against the float map at stage 2 (PS sampler s2,
			// knobs c15 = (spread, spread, bias, strength)). The shadow factor multiplies ONLY the
			// light-0 diffuse lobe and its specular term, mirroring "sun shadows attenuate direct
			// sunlight only"; fill lights, ambient and the retail decal shadows are untouched. The uv
			// remap (0.5*(c+1), 0.5*(1-cy), 0.5*cz+0.5) and the (sample + bias < ownDepth) comparison
			// are the exact convention the occluder sink writes into the map (SunShadowSinkImpl::Submit).
			static const char* kVertexShaderGNS_NUV1 =
				"float4 c0 : register(c0);\n"
				"float4 c1 : register(c1);\n"
				"float4 c2 : register(c2);\n"
				"float4 c3 : register(c3);\n"
				"float4 c4 : register(c4);\n"
				"float4 c5 : register(c5);\n"
				"float4 c6 : register(c6);\n"	// world rotation rows (object normal -> world)
				"float4 c7 : register(c7);\n"	// object -> light clip rows (ortho sun camera)
				"float4 c8 : register(c8);\n"
				"float4 c9 : register(c9);\n"
				"float4 c10 : register(c10);\n"
				"struct VS_OUT\n"
				"{\n"
				"    float4 pos : POSITION;\n"
				"    float2 uv  : TEXCOORD0;\n"
				"    float3 wn  : TEXCOORD1;\n"
				"    float4 sv  : TEXCOORD2;\n"
				"    float eye  : TEXCOORD3;\n"             // batch8 P0: clip w = eye depth; POSITION PS inputs are rejected by fxc (X4502)
				"};\n"
				"VS_OUT main(float3 pos : POSITION, float3 normal : NORMAL, float2 uv0 : TEXCOORD0)\n"
				"{\n"
				"    VS_OUT o;\n"
				"    float4 p = float4(pos, 1.0);\n"
				"    o.pos = float4(dot(p, c0), dot(p, c1), dot(p, c2), dot(p, c3));\n"
				"    o.uv = uv0;\n"
				"    o.wn = float3(dot(normal, c4.xyz), dot(normal, c5.xyz), dot(normal, c6.xyz));\n"
				"    o.eye = o.pos.w;\n"                    // batch8 P0: relay clip w so the PS fog sees eye depth
				"    float4 s = float4(dot(p, c7), dot(p, c8), dot(p, c9), dot(p, c10));\n"
				"    o.sv = float4(0.5 * (s.x + 1.0), 0.5 * (1.0 - s.y), 0.5 * s.z + 0.5, 1.0);\n"
				"    return o;\n"
				"}\n";

			static const char* kVertexShaderGNS_NUV2 =
				"float4 c0 : register(c0);\n"
				"float4 c1 : register(c1);\n"
				"float4 c2 : register(c2);\n"
				"float4 c3 : register(c3);\n"
				"float4 c4 : register(c4);\n"
				"float4 c5 : register(c5);\n"
				"float4 c6 : register(c6);\n"	// world rotation rows (object normal -> world)
				"float4 c7 : register(c7);\n"	// object -> light clip rows (ortho sun camera)
				"float4 c8 : register(c8);\n"
				"float4 c9 : register(c9);\n"
				"float4 c10 : register(c10);\n"
				"struct VS_OUT\n"
				"{\n"
				"    float4 pos : POSITION;\n"
				"    float2 uv  : TEXCOORD0;\n"
				"    float3 wn  : TEXCOORD1;\n"
				"    float4 sv  : TEXCOORD2;\n"
				"    float eye  : TEXCOORD3;\n"             // batch8 P0: clip w = eye depth; POSITION PS inputs are rejected by fxc (X4502)
				"};\n"
				"VS_OUT main(float3 pos : POSITION, float3 normal : NORMAL, float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1)\n"
				"{\n"
				"    VS_OUT o;\n"
				"    float4 p = float4(pos, 1.0);\n"
				"    o.pos = float4(dot(p, c0), dot(p, c1), dot(p, c2), dot(p, c3));\n"
				"    o.uv = uv0;\n"
				"    o.wn = float3(dot(normal, c4.xyz), dot(normal, c5.xyz), dot(normal, c6.xyz));\n"
				"    o.eye = o.pos.w;\n"                    // batch8 P0: relay clip w so the PS fog sees eye depth
				"    float4 s = float4(dot(p, c7), dot(p, c8), dot(p, c9), dot(p, c10));\n"
				"    o.sv = float4(0.5 * (s.x + 1.0), 0.5 * (1.0 - s.y), 0.5 * s.z + 0.5, 1.0);\n"
				"    return o;\n"
				"}\n";

			static const char* kPixelShaderGNS =
				"sampler s0 : register(s0);\n"                 // diffuse texture (bound by the category)
				"sampler s2 : register(s2);\n"                 // sun shadow map (bound by DX8GeomNormal::Apply)
				"float4 c4 : register(c4);\n"                  // ambient: matAmbient*envAmbient + emissive
				"float4 c5 : register(c5);\n"                  // world light dir 0 (toward light)
				"float4 c6 : register(c6);\n"                  // world light dir 1
				"float4 c7 : register(c7);\n"                  // world light dir 2
				"float4 c8 : register(c8);\n"                  // world light dir 3
				"float4 c9 : register(c9);\n"                  // matDiffuse*lightColor 0
				"float4 c10 : register(c10);\n"                // matDiffuse*lightColor 1
				"float4 c11 : register(c11);\n"                // matDiffuse*lightColor 2
				"float4 c12 : register(c12);\n"                // matDiffuse*lightColor 3
				"float4 c13 : register(c13);\n"                // world eye dir (toward camera), w = spec power
				"float4 c14 : register(c14);\n"                // spec intensity rgb (light 0 only, T1 knobs)
				"float4 c15 : register(c15);\n"                // shadow: (spread, spread, bias, strength)
				"float4 c16 : register(c16);\n"                // hemi ambient rgb (P3-12), scaled by world-up n.z (W3D is Z-up)
				"float4 c17 : register(c17);\n"                // world light dir 4 (P3-18 widened the GN family to 8 lights)
				"float4 c18 : register(c18);\n"                // world light dir 5
				"float4 c19 : register(c19);\n"                // world light dir 6
				"float4 c20 : register(c20);\n"                // world light dir 7
				"float4 c21 : register(c21);\n"                // matDiffuse*lightColor 4
				"float4 c22 : register(c22);\n"                // matDiffuse*lightColor 5
				"float4 c23 : register(c23);\n"                // matDiffuse*lightColor 6
				"float4 c24 : register(c24);\n"                // matDiffuse*lightColor 7
				"float4 c25 : register(c25);\n"                // P3-14 fog: rgb haze color, w = blend strength (0 = identity)
				"float4 c26 : register(c26);\n"                // P3-14 fog: x = eye-depth start, y = 1/ramp length
				"struct PS_IN\n"
				"{\n"
				"    float eye : TEXCOORD3;\n"                  // batch8 P0: eye depth relayed by the VS (POSITION input = X4502)
				"    float2 uv : TEXCOORD0;\n"
				"    float3 wn : TEXCOORD1;\n"
				"    float4 sv : TEXCOORD2;\n"
				"};\n"
				"float4 main(PS_IN i) : COLOR\n"
				"{\n"
				"    float4 base = tex2D(s0, i.uv);\n"
				"    float l = length(i.wn);\n"
				"    float3 n = (l > 1e-6) ? i.wn / l : float3(0.0, 1.0, 0.0);\n"
				// GeneralsX @bugfix PS3.0-#73 16/10/2026 two-sided flip (same as kPixelShaderGN; the
				// shadow lookup rides on i.sv from the vertex position and is unaffected by n).
				"    n *= (dot(n, c13.xyz) < 0.0) ? -1.0 : 1.0;\n"
				"    float ndl0 = saturate(dot(n, c5.xyz));\n"
				"    float inside = (i.sv.x >= 0.0 && i.sv.x <= 1.0 && i.sv.y >= 0.0 && i.sv.y <= 1.0\n"
				"        && i.sv.z >= 0.0 && i.sv.z <= 1.0) ? 1.0 : 0.0;\n"
				"    float2 q = c15.xy;\n"
				"    float count = 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(-q.x, -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( 0.0,  -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( q.x,  -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(-q.x,  0.0)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( q.x,  0.0)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(-q.x,  q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( 0.0,  q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( q.x,  q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    float shade = 1.0 - inside * c15.w * (count / 9.0);\n"
				"    float3 col = c4.rgb + n.z * c16.rgb\n"
				"        + c9.rgb  * ndl0 * shade\n"
				"        + c10.rgb * saturate(dot(n, c6.xyz))\n"
				"        + c11.rgb * saturate(dot(n, c7.xyz))\n"
				"        + c12.rgb * saturate(dot(n, c8.xyz))\n"
				"        + c21.rgb * saturate(dot(n, c17.xyz))\n"
				"        + c22.rgb * saturate(dot(n, c18.xyz))\n"
				"        + c23.rgb * saturate(dot(n, c19.xyz))\n"
				"        + c24.rgb * saturate(dot(n, c20.xyz));\n"
				"    float3 h = normalize(c5.xyz + normalize(c13.xyz));\n"
				"    float3 spec = c14.rgb * pow(saturate(dot(n, h)), c13.w) * ndl0 * shade;\n"
				"    float4 lit = float4(base.rgb * saturate(col) + spec, base.a);\n"
				"    float fogf = saturate((i.eye - c26.x) * c26.y) * c25.w;\n"   // P3-14: eye-depth haze, w=0 keeps the pixel exact (batch8 P0: VS-relayed TEXCOORD depth, POSITION inputs are X4502)
				"    return lerp(lit, float4(c25.rgb, lit.a), fogf);\n"
				"}\n";

			// GeneralsX @feature PS3.0-P3-22 14/10/2026: 5x5 (25-tap) PCF twin of the program above -
			// identical lighting contract, wider kernel with taps at {0, q, 2q} per axis (the game layer
			// doubles the c15 spread for this tier so both kernels blur the same world width at equal
			// map size). Compiled only at best-profile like everything else; if it fails, DX8GeomNormal
			// keeps mounting the 3x3 variant no matter what GENERALSX_SMAP_PCF says.
			static const char* kPixelShaderGNS5 =
				"sampler s0 : register(s0);\n"
				"sampler s2 : register(s2);\n"
				"float4 c4 : register(c4);\n"
				"float4 c5 : register(c5);\n"
				"float4 c6 : register(c6);\n"
				"float4 c7 : register(c7);\n"
				"float4 c8 : register(c8);\n"
				"float4 c9 : register(c9);\n"
				"float4 c10 : register(c10);\n"
				"float4 c11 : register(c11);\n"
				"float4 c12 : register(c12);\n"
				"float4 c13 : register(c13);\n"
				"float4 c14 : register(c14);\n"
				"float4 c15 : register(c15);\n"
				"float4 c16 : register(c16);\n"                // hemi ambient rgb (P3-12), scaled by world-up n.z (W3D is Z-up)
				"float4 c17 : register(c17);\n"                // world light dir 4..7 (P3-18, 8-light GN family)
				"float4 c18 : register(c18);\n"
				"float4 c19 : register(c19);\n"
				"float4 c20 : register(c20);\n"
				"float4 c21 : register(c21);\n"                // matDiffuse*lightColor 4..7
				"float4 c22 : register(c22);\n"
				"float4 c23 : register(c23);\n"
				"float4 c24 : register(c24);\n"
				"float4 c25 : register(c25);\n"
				"float4 c26 : register(c26);\n"
				"struct PS_IN\n"
				"{\n"
				"    float eye : TEXCOORD3;\n"                  // batch8 P0: eye depth relayed by the VS (POSITION input = X4502)
				"    float2 uv : TEXCOORD0;\n"
				"    float3 wn : TEXCOORD1;\n"
				"    float4 sv : TEXCOORD2;\n"
				"};\n"
				"float4 main(PS_IN i) : COLOR\n"
				"{\n"
				"    float4 base = tex2D(s0, i.uv);\n"
				"    float l = length(i.wn);\n"
				"    float3 n = (l > 1e-6) ? i.wn / l : float3(0.0, 1.0, 0.0);\n"
				// GeneralsX @bugfix PS3.0-#73 16/10/2026 two-sided flip (same as kPixelShaderGN; the
				// shadow lookup rides on i.sv from the vertex position and is unaffected by n).
				"    n *= (dot(n, c13.xyz) < 0.0) ? -1.0 : 1.0;\n"
				"    float ndl0 = saturate(dot(n, c5.xyz));\n"
				"    float inside = (i.sv.x >= 0.0 && i.sv.x <= 1.0 && i.sv.y >= 0.0 && i.sv.y <= 1.0\n"
				"        && i.sv.z >= 0.0 && i.sv.z <= 1.0) ? 1.0 : 0.0;\n"
				"    float2 q = c15.xy;\n"
				"    float count = 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(-2.0*q.x, -2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( -q.x,  -2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  0.0,  -2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  q.x,  -2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(2.0*q.x, -2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(-2.0*q.x,  -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( -q.x,   -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  0.0,   -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  q.x,   -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(2.0*q.x,  -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(-2.0*q.x,  0.0)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( -q.x,    0.0)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  q.x,    0.0)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(2.0*q.x,  0.0)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(-2.0*q.x,  q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( -q.x,    q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  0.0,    q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  q.x,    q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(2.0*q.x,  q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(-2.0*q.x, 2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2( -q.x,   2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  0.0,   2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(  q.x,   2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    count += (tex2D(s2, i.sv.xy + float2(2.0*q.x, 2.0*q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
				"    float shade = 1.0 - inside * c15.w * (count / 25.0);\n"
				"    float3 col = c4.rgb + n.z * c16.rgb\n"
				"        + c9.rgb  * ndl0 * shade\n"
				"        + c10.rgb * saturate(dot(n, c6.xyz))\n"
				"        + c11.rgb * saturate(dot(n, c7.xyz))\n"
				"        + c12.rgb * saturate(dot(n, c8.xyz))\n"
				"        + c21.rgb * saturate(dot(n, c17.xyz))\n"
				"        + c22.rgb * saturate(dot(n, c18.xyz))\n"
				"        + c23.rgb * saturate(dot(n, c19.xyz))\n"
				"        + c24.rgb * saturate(dot(n, c20.xyz));\n"
				"    float3 h = normalize(c5.xyz + normalize(c13.xyz));\n"
				"    float3 spec = c14.rgb * pow(saturate(dot(n, h)), c13.w) * ndl0 * shade;\n"
				"    float4 lit = float4(base.rgb * saturate(col) + spec, base.a);\n"
				"    float fogf = saturate((i.eye - c26.x) * c26.y) * c25.w;\n"   // P3-14: eye-depth haze, w=0 keeps the pixel exact (batch8 P0: VS-relayed TEXCOORD depth, POSITION inputs are X4502)
				"    return lerp(lit, float4(c25.rgb, lit.a), fogf);\n"
				"}\n";

				// GeneralsX @feature Qoder 17/10/2026 P3 (逐像素光照总闸): the MERGED base pair for the
				// TG3 tangent-space layout (rigid bump meshes + tangent skin batches - both submit
				// DX8_FVF_XYZNDUV1TG3, and skins render identity-world so one VS serves both).
				// 底座 = the exact plain-GN lighting equation (material-scaled ambient c4, up to 8
				// world directional lights, hemi c16, T1 Blinn spec on light 0, eye-depth fog c25/c26,
				// #73 two-sided flip) lifted from kPixelShaderGN; the two detail layers are uniform
				// branches per 工作清单 §2 P3: c27.y picks the s1 bump sample (dot3 layer, c27.x carries
				// the #69 slider deviation), c27.w the s3 roughness sample (M1 Blinn / M2 GGX ported from
				// kPixelShaderPBR), c27.z the s2 sun-shadow taps lifted from kPixelShaderGNS (bump meshes
				// gain shadow reception they never had - an addition, not a regression; 5x5 PCF tier
				// stays on the rigid GN variants).  Samplers: s0 diffuse, s1 _nm, s2 sun map (GN
				// convention - the rp companion MOVES from s2 to s3 to free it), s3 _rp.  Vertex
				// diffuse rides through with the FF-parity chain (base*diffuse, alpha texel x vertex)
				// the GN/dot3/skin pairs already ship.  "缺资产＝逐位不变": both branch flags 0 makes the
				// pixel the plain GN pixel modulo the COLOR0 relay (flat fallback §7.2).
				static const char* kVertexShaderGNMerged_TG3 =
					"float4 c0 : register(c0);\n"
					"float4 c1 : register(c1);\n"
					"float4 c2 : register(c2);\n"
					"float4 c3 : register(c3);\n"
					"float4 c4 : register(c4);\n"
					"float4 c5 : register(c5);\n"
					"float4 c6 : register(c6);\n"	// world rotation rows (normal/tangent/binormal -> world)
					"float4 c7 : register(c7);\n"	// object -> light clip rows (identity when no sun shadow)
					"float4 c8 : register(c8);\n"
					"float4 c9 : register(c9);\n"
					"float4 c10 : register(c10);\n"
					"struct VS_OUT\n"
					"{\n"
					"    float4 pos     : POSITION;\n"
					"    float4 diffuse : COLOR0;\n"
					"    float2 uv      : TEXCOORD0;\n"
					"    float3 tT      : TEXCOORD1;\n"
					"    float3 tB      : TEXCOORD2;\n"
					"    float3 tN      : TEXCOORD3;\n"
					"    float4 sv      : TEXCOORD4;\n"
					"    float  eye     : TEXCOORD5;\n"   // batch8 P0: clip w = eye depth (POSITION PS input = X4502)
					"};\n"
					"VS_OUT main(float4 pos : POSITION, float3 normal : NORMAL, float4 diffuse : COLOR0,\n"
					"            float2 uv0 : TEXCOORD0, float3 tangent : TEXCOORD1, float3 binormal : TEXCOORD2)\n"
					"{\n"
					"    VS_OUT o;\n"
					"    o.pos = float4(dot(pos, c0), dot(pos, c1), dot(pos, c2), dot(pos, c3));\n"
					"    o.diffuse = diffuse;\n"
					"    o.uv = uv0;\n"
					"    o.tN = float3(dot(normal, c4.xyz), dot(normal, c5.xyz), dot(normal, c6.xyz));\n"
					"    o.tT = float3(dot(tangent, c4.xyz), dot(tangent, c5.xyz), dot(tangent, c6.xyz));\n"
					"    o.tB = float3(dot(binormal, c4.xyz), dot(binormal, c5.xyz), dot(binormal, c6.xyz));\n"
					"    o.eye = o.pos.w;\n"
					"    float4 s = float4(dot(pos, c7), dot(pos, c8), dot(pos, c9), dot(pos, c10));\n"
					"    o.sv = float4(0.5 * (s.x + 1.0), 0.5 * (1.0 - s.y), 0.5 * s.z + 0.5, 1.0);\n"
					"    return o;\n"
					"}\n";

				static const char* kPixelShaderGNMerged =
					"sampler s0 : register(s0);\n"                 // diffuse (stage0, uv)
					"sampler s1 : register(s1);\n"                 // tangent-space normal map (_nm companion)
					"sampler s2 : register(s2);\n"                 // sun shadow map (GN convention, bound only when c27.z=1)
					"sampler s3 : register(s3);\n"                 // rp companion: R=roughness, G=metalness (P3: s2 belongs to the sun map)
					"float4 c4 : register(c4);\n"                  // ambient: matAmbient*envAmbient + emissive
					"float4 c5 : register(c5);\n"                  // world light dir 0 (toward light)
					"float4 c6 : register(c6);\n"                  // world light dir 1
					"float4 c7 : register(c7);\n"                  // world light dir 2
					"float4 c8 : register(c8);\n"                  // world light dir 3
					"float4 c9 : register(c9);\n"                  // matDiffuse*lightColor 0
					"float4 c10 : register(c10);\n"                // matDiffuse*lightColor 1
					"float4 c11 : register(c11);\n"                // matDiffuse*lightColor 2
					"float4 c12 : register(c12);\n"                // matDiffuse*lightColor 3
					"float4 c13 : register(c13);\n"                // world eye dir (toward camera), w = spec power
					"float4 c14 : register(c14);\n"                // spec intensity rgb (light 0 only, T1 knobs)
					"float4 c15 : register(c15);\n"                // shadow: (spread, spread, bias, strength)
					"float4 c16 : register(c16);\n"                // hemi ambient rgb, scaled by world-up n.z (W3D Z-up)
					"float4 c17 : register(c17);\n"                // world light dir 4..7
					"float4 c18 : register(c18);\n"
					"float4 c19 : register(c19);\n"
					"float4 c20 : register(c20);\n"
					"float4 c21 : register(c21);\n"                // matDiffuse*lightColor 4..7
					"float4 c22 : register(c22);\n"
					"float4 c23 : register(c23);\n"
					"float4 c24 : register(c24);\n"
					"float4 c25 : register(c25);\n"                // fog: rgb haze color, w = blend strength (0 = identity)
					"float4 c26 : register(c26);\n"                // fog: x = eye-depth start, y = 1/ramp length
					"float4 c27 : register(c27);\n"                // layers: (bumpK, bumpOn, shadowOn, rpOn)
					"float4 c28 : register(c28);\n"                // rp contract: (mode, LI, ENV, wetness) - cf. PBR c8/c11
					"struct PS_IN\n"
					"{\n"
					"    float4 diffuse : COLOR0;\n"
					"    float2 uv      : TEXCOORD0;\n"
					"    float3 tT      : TEXCOORD1;\n"
					"    float3 tB      : TEXCOORD2;\n"
					"    float3 tN      : TEXCOORD3;\n"
					"    float4 sv      : TEXCOORD4;\n"
					"    float  eye     : TEXCOORD5;\n"
					"};\n"
					// The M2 GGX lobe, verbatim from kPixelShaderPBR (Schlick fresnel, Smith-Schlick G,
					// grazing-V clamp, the 0.25-folded NDF). kd comes back through the out param.
					"float3 ggxLobe(float3 n, float3 L, float3 E, float3 F0, float metal, float a2, float kk, float ndl, out float3 kd)\n"
					"{\n"
					"    float3 H = normalize(L + E);\n"
					"    float ndh = saturate(dot(n, H));\n"
					"    float vnd = max(dot(n, E), 0.05);\n"
					"    float vnh = saturate(dot(E, H));\n"
					"    float3 fres = F0 + (1.0 - F0) * pow(1.0 - vnh, 5.0);\n"
					"    kd = (1.0 - fres) * (1.0 - metal);\n"
					"    float dnom = ndh * ndh * (a2 - 1.0) + 1.0;\n"
					"    float D = a2 / max(dnom * dnom, 1e-4);\n"
					"    float G = (ndl / max(ndl * (1.0 - kk) + kk, 1e-3)) * (vnd / max(vnd * (1.0 - kk) + kk, 1e-3));\n"
					"    return D * fres * G * 0.25;\n"
					"}\n"
					"float4 main(PS_IN i) : COLOR\n"
					"{\n"
					"    float4 base = tex2D(s0, i.uv);\n"
					"    base.rgb *= i.diffuse.rgb;\n"           // FF parity: vertex tint multiplies the albedo (dot3/skin-pair chain)
					// --- 细节层 1: the normal. Bumped reads the s1 tangent map through the world TBN
					// (programmable recipe); flat reads the interpolated geometric normal (plain GN recipe,
					// with its degenerate-length guard).
					// GeneralsX @bugfix Qoder 22/10/2026 GNM two-sided flip keys on the GEOMETRIC normal.
					// #73 defined the rule for a pure geometric normal (kPixelShaderGN), and its dot3 twin
					// reflects the tangent-space light Z by sign(dot(eye, vertexNormal)) - bump-independent.
					// The merged PS (P3, 17/10) rebuilt n from the _nm sample first and then tested the
					// PERTURBED vector: on a bumpy roof/barrel the per-texel dot(n,eye) crosses zero, so
					// whole regions invert and the result depends on the object's yaw - two identical units
					// facing opposite directions shade differently even though their roof geometric normals
					// are the same world-up face. Keying on i.tN and reflecting only the N coefficient
					// reproduces the dot3 flip exactly:
					//   dot(tn, (Lt, Lb, Ln*facing)) == dot(tn.x*T + tn.y*B + tn.z*facing*N, L).
					"    float gl = length(i.tN);\n"
					"    float3 geoN = (gl > 1e-6) ? i.tN / gl : float3(0.0, 1.0, 0.0);\n"
					"    float facing = (dot(geoN, c13.xyz) < 0.0) ? -1.0 : 1.0;\n"
					"    float3 n;\n"
					"    if (c27.y > 0.5)\n"
					"    {\n"
					"        float3 tn = tex2D(s1, i.uv).xyz * 2.0 - 1.0;\n"
					"        tn.xy = tn.xy * c27.x;\n"           // #69 slider deviation (was dot3 c3.x)
					"        tn = normalize(tn);\n"
					"        float3 T = normalize(i.tT);\n"
					"        float3 B = normalize(i.tB);\n"
					"        float3 N = normalize(i.tN);\n"
					"        n = tn.x * T + tn.y * B + (tn.z * facing) * N;\n"
					"    }\n"
					"    else\n"
					"    {\n"
					"        n = geoN * facing;\n"
					"    }\n"
					"    n = normalize(n);\n"
					// --- sun shadow: the GNS 3x3 kernel behind a uniform branch (c27.z=0 keeps the
					// exact unshadowed pixel and never touches the unbound s2).
					"    float shade = 1.0;\n"
					"    if (c27.z > 0.5)\n"
					"    {\n"
					"        float inside = (i.sv.x >= 0.0 && i.sv.x <= 1.0 && i.sv.y >= 0.0 && i.sv.y <= 1.0\n"
					"            && i.sv.z >= 0.0 && i.sv.z <= 1.0) ? 1.0 : 0.0;\n"
					"        float2 q = c15.xy;\n"
					"        float count = 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy + float2(-q.x, -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy + float2( 0.0,  -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy + float2( q.x,  -q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy + float2(-q.x,  0.0)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy + float2( q.x,  0.0)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy + float2(-q.x,  q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy + float2( 0.0,  q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        count += (tex2D(s2, i.sv.xy + float2( q.x,  q.y)).x + c15.z < i.sv.z) ? 1.0 : 0.0;\n"
					"        shade = 1.0 - inside * c15.w * (count / 9.0);\n"
					"    }\n"
					// --- 底座 irradiance: the plain GN 8-light lambert sum, sun term shadowed.
					"    float ndl0 = saturate(dot(n, c5.xyz));\n"
					"    float3 col = c4.rgb + n.z * c16.rgb\n"
					"        + c9.rgb  * ndl0 * shade\n"
					"        + c10.rgb * saturate(dot(n, c6.xyz))\n"
					"        + c11.rgb * saturate(dot(n, c7.xyz))\n"
					"        + c12.rgb * saturate(dot(n, c8.xyz))\n"
					"        + c21.rgb * saturate(dot(n, c17.xyz))\n"
					"        + c22.rgb * saturate(dot(n, c18.xyz))\n"
					"        + c23.rgb * saturate(dot(n, c19.xyz))\n"
					"        + c24.rgb * saturate(dot(n, c20.xyz));\n"
					"    col = saturate(col);\n"
					"    float3 E = normalize(c13.xyz);\n"
					"    float3 h = normalize(c5.xyz + E);\n"
					"    float3 outc;\n"
					// --- 细节层 2: the specular/roughness layer. No rp = the plain T1 Blinn (bit-exact
					// GN term); mode 1 = roughness-driven Blinn (kPixelShaderPBR M1); mode 2 = M2 GGX
					// (Schlick F + Smith-Schlick G + metal-killed diffuse), sun shadow folded into shade,
					// env = constant hemisphere c28.z*F0 exactly as the shipped PBR contract.
					// 调研 §2: the V is the world eye CONSTANT (c13), the same approximation the dot3 path used.
					"    if (c27.w < 0.5)\n"
					"    {\n"
					"        float3 spec = c14.rgb * pow(saturate(dot(n, h)), c13.w) * ndl0 * shade;\n"
					"        outc = base.rgb * col + spec;\n"
					"    }\n"
					"    else\n"
					"    {\n"
					"        float2 rm = tex2D(s3, i.uv).xy;\n"  // R=roughness, G=metalness (packer §1)
					"        rm.x = rm.x * (1.0 - c28.w * 0.75);\n"   // Weather W5 rain-slick (same as PBR c8.w)
					"        if (c28.x < 1.5)\n"
					"        {\n"
					"            float shin = lerp(8.0, 128.0, 1.0 - rm.x);\n"
					"            float3 spec = c14.rgb * pow(saturate(dot(n, h)), shin) * (1.0 - rm.x) * ndl0 * shade;\n"
					"            outc = base.rgb * col + spec;\n"
					"        }\n"
					"        else\n"
					"        {\n"
					"            float3 F0 = lerp(float3(0.04, 0.04, 0.04), base.rgb, rm.y);\n"
					"            float a = rm.x * rm.x;\n"
					"            float a2 = a * a;\n"
					"            float kk = (rm.x + 1.0) * (rm.x + 1.0) * 0.125;\n"
					"            float3 kd;\n"
					"            float3 sp0 = ggxLobe(n, c5.xyz,  E, F0, rm.y, a2, kk, ndl0, kd) * c9.rgb  * ndl0 * shade;\n"
					"            float ndl1 = saturate(dot(n, c6.xyz));\n"
					"            float3 kdx;\n"
					"            float3 sp1 = ggxLobe(n, c6.xyz,  E, F0, rm.y, a2, kk, ndl1, kdx) * c10.rgb * ndl1;\n"
					"            float ndl2 = saturate(dot(n, c7.xyz));\n"
					"            float3 sp2 = ggxLobe(n, c7.xyz,  E, F0, rm.y, a2, kk, ndl2, kdx) * c11.rgb * ndl2;\n"
					"            float ndl3 = saturate(dot(n, c8.xyz));\n"
					"            float3 sp3 = ggxLobe(n, c8.xyz,  E, F0, rm.y, a2, kk, ndl3, kdx) * c12.rgb * ndl3;\n"
					"            float ndl4 = saturate(dot(n, c17.xyz));\n"
					"            float3 sp4 = ggxLobe(n, c17.xyz, E, F0, rm.y, a2, kk, ndl4, kdx) * c21.rgb * ndl4;\n"
					"            float ndl5 = saturate(dot(n, c18.xyz));\n"
					"            float3 sp5 = ggxLobe(n, c18.xyz, E, F0, rm.y, a2, kk, ndl5, kdx) * c22.rgb * ndl5;\n"
					"            float ndl6 = saturate(dot(n, c19.xyz));\n"
					"            float3 sp6 = ggxLobe(n, c19.xyz, E, F0, rm.y, a2, kk, ndl6, kdx) * c23.rgb * ndl6;\n"
					"            float ndl7 = saturate(dot(n, c20.xyz));\n"
					"            float3 sp7 = ggxLobe(n, c20.xyz, E, F0, rm.y, a2, kk, ndl7, kdx) * c24.rgb * ndl7;\n"
					"            float3 specSum = (sp0 + sp1 + sp2 + sp3 + sp4 + sp5 + sp6 + sp7) * c28.y;\n"
					"            outc = base.rgb * kd * col * c28.y + c28.z * F0 + specSum;\n"
					"        }\n"
					"    }\n"
					"    float4 lit = float4(outc, base.a * i.diffuse.a);\n"   // FF-parity alpha (see kPixelShader)
					"    float fogf = saturate((i.eye - c26.x) * c26.y) * c25.w;\n"
					"    return lerp(lit, float4(c25.rgb, lit.a), fogf);\n"
					"}\n";

			const char* gnPsSrc = kPixelShaderGN;
			{
				const char* rawEnv = getenv("GENERALSX_GEOMNORMAL_RAW");
				if (rawEnv != nullptr && rawEnv[0] != 0 && strcmp(rawEnv, "0") != 0) {
					gnPsSrc = kPixelShaderGNRaw;
					fprintf(stderr, "[GN] raw-visual mode ON (R=light0 term, G=light1 term)\n");
					fflush(stderr);
				}
			}

			IDirect3DVertexShader9* gnVS1 = nullptr;
			IDirect3DVertexShader9* gnVS2 = nullptr;
			IDirect3DPixelShader9* gnPS = nullptr;
			const char* gnP1 = nullptr; const char* gnP2 = nullptr; const char* gnPP = nullptr;
			HRESULT gnHR = W3DShaderManager::compileShaderAtBestProfile(kVertexShaderGN_NUV1, true, "[GN]", &gnP1, (void**)&gnVS1);
			if (SUCCEEDED(gnHR) && gnVS1 != nullptr)
				gnHR = W3DShaderManager::compileShaderAtBestProfile(kVertexShaderGN_NUV2, true, "[GN]", &gnP2, (void**)&gnVS2);
			if (SUCCEEDED(gnHR) && gnVS2 != nullptr)
				gnHR = W3DShaderManager::compileShaderAtBestProfile(gnPsSrc, false, "[GN]", &gnPP, (void**)&gnPS);
			if (gnVS1 != nullptr && gnVS2 != nullptr && gnPS != nullptr) {
				DX8GeomNormal::Install((UINT_PTR)gnVS1, (UINT_PTR)gnVS2, (UINT_PTR)gnPS);
				fprintf(stderr, "[GN] geometric-normal installed vs=%s/%s ps=%s\n", gnP1, gnP2, gnPP);
				// GeneralsX @feature Qoder 17/10/2026: seed the per-pixel lighting master switch from
				// the option layer, not from the raw getenv inside Install(), so the ini key, the
				// Graphics Options row and the renderer all read one value (the getter keeps
				// GENERALSX_GEOMNORMAL winning for scripted A/B legs).
				DX8GeomNormal::Set_Enabled(OptionPreferences().getPerPixelLighting() != FALSE);
			} else {
				if (gnVS1 != nullptr) gnVS1->Release();
				if (gnVS2 != nullptr) gnVS2->Release();
				if (gnPS != nullptr) gnPS->Release();
				fprintf(stderr, "[GN] geometric-normal compile failed, fixed pipeline only\n");
			}
			fflush(stderr);

			// GeneralsX @feature Qoder 17/10/2026 P2: compile the skinned GN variant pair as its own
			// step - a failure here can never take the plain trio down, it just leaves
			// DX8GeomNormal::Apply's DUV2 branch declining (skins stay on the fixed pipeline, exactly
			// the pre-P2 behavior). Skipped in raw-probe mode: kPixelShaderGNRaw shares no contract
			// with the skin relay, and that leg only ever runs scripted forensics.
			if (gnVS1 != nullptr && gnVS2 != nullptr && gnPS != nullptr && gnPsSrc == kPixelShaderGN)
			{
				IDirect3DVertexShader9* gnSkinVS = nullptr;
				IDirect3DPixelShader9* gnSkinPS = nullptr;
				const char* gnSkinVProfile = nullptr; const char* gnSkinPProfile = nullptr;
				HRESULT skHR = W3DShaderManager::compileShaderAtBestProfile(kVertexShaderGN_SKIN, true, "[GNSKIN]", &gnSkinVProfile, (void**)&gnSkinVS);
				if (SUCCEEDED(skHR) && gnSkinVS != nullptr)
					skHR = W3DShaderManager::compileShaderAtBestProfile(kPixelShaderGNSkin, false, "[GNSKIN]", &gnSkinPProfile, (void**)&gnSkinPS);
				if (gnSkinVS != nullptr && gnSkinPS != nullptr) {
					DX8GeomNormal::Install_Skin((UINT_PTR)gnSkinVS, (UINT_PTR)gnSkinPS);
					fprintf(stderr, "[GNSKIN] skinned GN pair installed vs=%s ps=%s\n", gnSkinVProfile, gnSkinPProfile);
				} else {
					if (gnSkinVS != nullptr) gnSkinVS->Release();
					if (gnSkinPS != nullptr) gnSkinPS->Release();
					fprintf(stderr, "[GNSKIN] skinned variant compile failed, skins stay on the fixed pipeline\n");
				}
				fflush(stderr);
			}

			// GeneralsX @feature Qoder 17/10/2026 P3 (逐像素光照总闸): compile the merged TG3 pair as its
			// own additive step (same discipline as GNSKIN: a failure here can never take the plain trio
			// or the skin pair down). Without the handles DX8GeomNormal::Apply declines every TG3 batch
			// and those meshes stay on the fixed pipeline. Skipped in raw-probe mode (scripted forensics
			// leg - the probe replaces only the plain GN pixel program).
			if (gnVS1 != nullptr && gnVS2 != nullptr && gnPS != nullptr && gnPsSrc == kPixelShaderGN)
			{
				IDirect3DVertexShader9* gnmVS = nullptr;
				IDirect3DPixelShader9* gnmPS = nullptr;
				const char* gnmVP = nullptr; const char* gnmPP = nullptr;
				HRESULT gnmHR = W3DShaderManager::compileShaderAtBestProfile(kVertexShaderGNMerged_TG3, true, "[GNM]", &gnmVP, (void**)&gnmVS);
				if (SUCCEEDED(gnmHR) && gnmVS != nullptr)
					gnmHR = W3DShaderManager::compileShaderAtBestProfile(kPixelShaderGNMerged, false, "[GNM]", &gnmPP, (void**)&gnmPS);
				if (gnmVS != nullptr && gnmPS != nullptr) {
					DX8GeomNormal::Install_Merged((UINT_PTR)gnmVS, (UINT_PTR)gnmPS);
					fprintf(stderr, "[GNM] merged TG3 pair installed vs=%s ps=%s\n", gnmVP, gnmPP);
				} else {
					if (gnmVS != nullptr) gnmVS->Release();
					if (gnmPS != nullptr) gnmPS->Release();
					fprintf(stderr, "[GNM] merged pair compile failed, bump/roughness meshes stay fixed pipeline\n");
				}
				fflush(stderr);
			}
			// (scope note PS3.0-P3-21b: the shadow-variant compile below stays inside this brace scope -
			// that is where the kVertexShaderGNS_* / kPixelShaderGNS sources are declared.)

		// GeneralsX @feature PS3.0-P3-21b 14/10/2026: compile the sun-shadow GN variants as a separate
		// step so a failure here can never take the plain GN pair down with it - the game layer then
		// keeps its receiver overlay (it asks DX8GeomNormal::Sun_Shadow_Ready before parking it).
		{
			IDirect3DVertexShader9* gnsVS1 = nullptr;
			IDirect3DVertexShader9* gnsVS2 = nullptr;
			IDirect3DPixelShader9* gnsPS = nullptr;
			const char* gnsP1 = nullptr; const char* gnsP2 = nullptr; const char* gnsPP = nullptr;
			HRESULT gnsHR = W3DShaderManager::compileShaderAtBestProfile(kVertexShaderGNS_NUV1, true, "[GN]", &gnsP1, (void**)&gnsVS1);
			if (SUCCEEDED(gnsHR) && gnsVS1 != nullptr)
				gnsHR = W3DShaderManager::compileShaderAtBestProfile(kVertexShaderGNS_NUV2, true, "[GN]", &gnsP2, (void**)&gnsVS2);
			if (SUCCEEDED(gnsHR) && gnsVS2 != nullptr)
				gnsHR = W3DShaderManager::compileShaderAtBestProfile(kPixelShaderGNS, false, "[GN]", &gnsPP, (void**)&gnsPS);
			if (gnsVS1 != nullptr && gnsVS2 != nullptr && gnsPS != nullptr) {
				DX8GeomNormal::Install_Sun_Shadow((UINT_PTR)gnsVS1, (UINT_PTR)gnsVS2, (UINT_PTR)gnsPS);

				// GeneralsX @feature PS3.0-P3-22 14/10/2026: the 5x5 PCF twin is compiled only after the
				// 3x3 trio is good (the VS pair is shared); its failure just means the env tier falls back
				// to 9 taps - DX8GeomNormal checks handle validity before mounting.
				IDirect3DPixelShader9* gnsPS5 = nullptr;
				const char* gnsP5 = nullptr;
				if (SUCCEEDED(gnsHR))
					gnsHR = W3DShaderManager::compileShaderAtBestProfile(kPixelShaderGNS5, false, "[GN]", &gnsP5, (void**)&gnsPS5);
				if (gnsPS5 != nullptr)
					DX8GeomNormal::Install_Sun_Shadow_PCF5((UINT_PTR)gnsPS5);
				else
					fprintf(stderr, "[GN] sun-shadow PCF5 compile failed, 3x3 kernel only\n");
				fflush(stderr);
			} else {
				if (gnsVS1 != nullptr) gnsVS1->Release();
				if (gnsVS2 != nullptr) gnsVS2->Release();
				if (gnsPS != nullptr) gnsPS->Release();
				fprintf(stderr, "[GN] sun-shadow variant compile failed, GN reception unavailable\n");
			}
			fflush(stderr);
		}
		}  // GeneralsX @feature PS3.0-P3-21b 14/10/2026 closes the brace scope that declared the
		       // GN + GNS shader sources (the shadow block above must stay inside it; nothing below may).

		// GeneralsX @feature 11/10/2026 Normal mapping defaults OFF (the dot3 path still has unresolved
		// bugs), driven by the user toggle in the graphics settings (GlobalData::m_useNormalMapping,
		// persisted to Options.ini); users can still turn it ON from the options menu.
		// Command line / env remain QA overrides: -nonormalmap / GENERALSX_NORMALMAP=0 force OFF,
		// -normalmap / GENERALSX_NORMALMAP=1 force ON. Strength via -normalmapstrength=<0..2> (default 0.8).
		bool enable = TheGlobalData ? (TheGlobalData->m_useNormalMapping != FALSE) : false;
		// GeneralsX @feature 13/10/2026 strength default now comes from the options-menu registry
		// (Options.ini NormalMapStrength, env-free key); the command line still overrides below.
		float strength = OptionPreferences().getNormalMapStrength();
		const char* cmd = GetCommandLineA();
		if (cmd) {
			if (strstr(cmd, "-nonormalmap") != nullptr) enable = false;
			else if (strstr(cmd, "-normalmap") != nullptr) enable = true;
			const char* sp = strstr(cmd, "-normalmapstrength=");
			if (sp) { sp += strlen("-normalmapstrength="); float v = (float)atof(sp); if (v >= 0.0f && v <= 2.0f) strength = v; }
		}
		if (const char* ev = getenv("GENERALSX_NORMALMAP")) { enable = (strcmp(ev, "0") != 0); }  // QA override: =0 force OFF, else force ON

		DX8NormalMap::Set_Strength(strength);
		DX8NormalMap::Set_Enabled(enable);

		// GeneralsX @feature PS3.0 #70 Qoder 15/10/2026: roughness (PBR) is now an INDEPENDENT toggle -
		// Options.ini "RoughnessMapping" (0 off / 1 M1 roughness Blinn / 2 M2 GGX) with the GENERALSX_PBR
		// env still winning inside OptionPreferences::getPBRMode, plus -pbrmode=<0|1|2> for scripted A/B.
		// Normal mapping can be on without roughness, roughness without normal (flat-normal PBR), or both.
		Int pbrMode = OptionPreferences().getPBRMode();
		{
			const char* pm = strstr(cmd, "-pbrmode=");
			if (pm) { pm += strlen("-pbrmode="); int v = atoi(pm); if (v >= 0 && v <= 2) pbrMode = v; }
		}
		DX8NormalMap::Set_PBR_Mode(pbrMode);

		fprintf(stderr, "[DOT3] shaders installed (enabled=%d strength=%.2f; default follows options-menu NormalMapping toggle)\n",
			(int)enable, strength);
		fprintf(stderr, "[PBR] mode=%d (0=off 1=roughness-Blinn 2=GGX; bump=%d, roughness-only mounts with flat normals)\n",
			(int)pbrMode, (int)enable);
		fflush(stderr);

		// GeneralsX @feature PS3.0-P3-14 14/10/2026: inject the eye-depth fog ramp the GN-family PS
		// (c25/c26) blend toward. WW3D2 cannot read GlobalData, so the
		// device layer computes the pair here once at shader install. Ramp derives from the camera's
		// far clip (start = 45%, length = 50% of it) so the haze sits beyond normal play distances;
		// the tint is a hand-picked cold grey-blue (true sky colour arrives with the P3-15 sky work).
		// Strength is user-owned now (Graphics Options page / Options.ini DistanceHaze*), seeded
		// lazily by seedDistanceHaze(); GENERALSX_GNFOG still wins inside the OptionPreferences getters,
		// and 0 keeps pixels bit-exact with the pre-P3-14 build. Default is the shipped 0.6.
		{
			// GeneralsX @feature PS3.0 Weather W5: the ramp now comes from the shared builder, at scale
			// 1.0 (dry). The weather refresh re-pushes it through the same function whenever the type
			// changes, so install and runtime cannot disagree.
			Vector4 colorStrength, ramp;
			Build_Weather_Fog_Ramp(1.0f, colorStrength, ramp);
			DX8NormalMap::Set_Fog(colorStrength, ramp);
			fprintf(stderr, "[GNFOG] strength=%.2f start=%.0f invLen=%.4f\n",
				(double)colorStrength.W, (double)ramp.X, (double)ramp.Y);
			fflush(stderr);
		}
	}
#else
	fprintf(stderr, "[DOT3] skipped: runtime shader assembly is Windows-only in this build\n");
	fflush(stderr);
#endif // _WIN32
}

// GeneralsX @bugfix Storm996 12/10/2026 ---------------------------------------------------------------
// init() is also called from the height-map rebuild path, and a failed SetRenderTarget switches the
// feature off at runtime, so every path has to drop exactly the same surfaces.
void W3DShaderManager::releaseRenderToTextureSurfaces()
{
	SAFE_RELEASE(m_newRenderSurface);
	SAFE_RELEASE(m_renderTexture);
	SAFE_RELEASE(m_oldRenderSurface);
	SAFE_RELEASE(m_oldDepthSurface);
	SAFE_RELEASE(m_rttColorSurface);
	SAFE_RELEASE(m_rttDepthSurface);
	// GeneralsX @feature Qoder 18/10/2026 step 9.H: the recorder target died with the surfaces. The
	// publish flag is deliberately kept: if the fuse pulls the pair out mid-redirect, the next
	// endRenderToTexture still has to hand the recorder back to the scene rather than stranding it.
	m_vkRttReady = false;
}

enum RttModes
{
	RTT_MODE_UNAVAILABLE = 0,	///<no offscreen target: every post-processing filter stays off.
	RTT_MODE_PLAIN,				///<single-sample offscreen target on a single-sample back buffer.
	RTT_MODE_OWN_DEPTH,			///<single-sample offscreen target with its own single-sample depth.
	RTT_MODE_MSAA_RESOLVE,		///<multisampled offscreen target, resolved into the sampled texture.
};

static const char* Rtt_Mode_Name(Int mode)
{
	switch (mode)
	{
		case RTT_MODE_PLAIN:			return "plain";
		case RTT_MODE_OWN_DEPTH:		return "single-sample+own-depth";
		case RTT_MODE_MSAA_RESOLVE:		return "msaa-resolve";
		default:						return "unavailable";
	}
}

// GeneralsX @bugfix Storm996 12/10/2026 Which of the routes below the device accepted is a hardware
// question, and it is the first thing to check when a post-processing effect misbehaves. Reported once per
// state change, because init() re-runs on every height-map rebuild.
static void Report_Render_To_Texture_Mode(Int mode, UINT width, UINT height, D3DMULTISAMPLE_TYPE samples, UINT quality)
{
	static Int lastMode = -1;
	static UINT lastWidth = 0, lastHeight = 0, lastSamples = 0xFFFFFFFFu, lastQuality = 0xFFFFFFFFu;
	if (mode == lastMode && width == lastWidth && height == lastHeight && samples == lastSamples && quality == lastQuality)
		return;
	lastMode = mode; lastWidth = width; lastHeight = height; lastSamples = samples; lastQuality = quality;

#ifdef _WIN32
	fprintf(stderr, "[RTT] offscreen target %s for back buffer %ux%u AA x%u q%u\n",
		Rtt_Mode_Name(mode), width, height, (UINT)samples, quality);
	fflush(stderr);
#endif // _WIN32
}

// W3DShaderManager::init =======================================================
/** Walk through all shaders and find versions suitable for current hardware */
//=============================================================================
void W3DShaderManager::init()
{
	int i,j;

	// GeneralsX @feature Qoder 20/10/2026 present-side post seed: FXAA and bloom are applied by the
	// pure-Vulkan present pass, which has no startup path of its own, so their knobs are seeded here from
	// the player's options. AA comes from the ini value and not from the device: the Vulkan path never
	// creates a multisampled swapchain, so every device-reported level would read back as none.
	{
		const Real seedBloom = getBloomIntensity();
		const Bool seedAA = OptionPreferences().getAntiAliasing() != WW3D::MULTISAMPLE_MODE_NONE;
		DX8Wrapper::VK_Set_Post_Knobs(seedBloom, seedAA != FALSE);
		fprintf(stderr, "[VKPOST] options seed: bloom=%.2f aa=%u\n", (double)seedBloom, seedAA != FALSE ? 1u : 0u);
		fflush(stderr);
	}

	D3DSURFACE_DESC desc = {};
	// For now, check & see if we are gf3 or higher on the food chain.

	ChipsetType res=DC_UNKNOWN;
	if ((res=W3DShaderManager::getChipset()) != 0)
	{
		m_currentChipset = res;	//cache the current chipset.

		//Some of our effects require an offscreen render target, so try creating it here.
		releaseRenderToTextureSurfaces();	//DEFAULT-pool surfaces do not survive a device reset
		LPDIRECT3DDEVICE9 device=DX8Wrapper::_Get_D3D_Device8();
		HRESULT hr=device->GetRenderTarget(0,&m_oldRenderSurface);

		// GeneralsX @bugfix Qoder 19/10/2026 BUG-03: a failed render-target probe must not skip the
		// shader and filter registration at the end of this function. The two early returns this
		// replaces were dead code on a real D3D9 device - GetRenderTarget(0) cannot fail right after
		// the device was created - but the Vulkan stub device answers with a null surface, the first
		// return fired, every W3DShadersPassCount entry stayed 0, and the terrain, shroud, road and
		// cloud passes (getShaderPasses()==0 draws nothing) vanished with it: the "all terrain gone"
		// report. RTT unavailability is already a reported mode and the post filters stay off without
		// it; the custom shaders do not depend on the offscreen pair and must still be built.
		Bool rttProbeOk = (hr == S_OK && m_oldRenderSurface != nullptr);
		D3DSURFACE_DESC depthDesc = {};
		if (rttProbeOk)
		{
			m_oldRenderSurface->GetDesc(&desc);

			hr = device->GetDepthStencilSurface(&m_oldDepthSurface);
			if (hr == S_OK && m_oldDepthSurface)
				m_oldDepthSurface->GetDesc(&depthDesc);
			else
			{
				releaseRenderToTextureSurfaces();
				rttProbeOk = FALSE;
			}
		}

		if (rttProbeOk)
		{
			// GeneralsX @feature Qoder 18/10/2026 step 9.H: decide once whether this init can hand the
			// offscreen pair to the recorder. The multisampled route below is a bare surface with no texture
			// container, so it can be neither followed nor sampled by Vulkan; whenever the draw path is up
			// the single-sample texture pair is the only shape that can serve both roles.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
			const Bool preferVkMirror = VKDraw_Ready();
#else
			const Bool preferVkMirror = FALSE;
#endif

			// GeneralsX @bugfix PS3.0 batch6 12/10/2026 -------------------------------------------------------
			// Redirecting the scene into a single-sample texture while the device keeps binding the back
			// buffer's multisampled depth buffer is the DX8 API violation that made the code below force a
			// creation failure as soon as the back buffer was multisampled. The cost of that shortcut was
			// silent: canRenderToTexture() went false for every player with antialiasing enabled and took the
			// whole filter family with it - default, bloom, monochrome, motion blur and cross-fade. The legal
			// shape is a color/depth pair that always shares one sample count, so the offscreen target now
			// brings its own depth buffer instead of borrowing the back buffer's.
			hr=device->CreateTexture(desc.Width,desc.Height,1,D3DUSAGE_RENDERTARGET,desc.Format,D3DPOOL_DEFAULT,&m_renderTexture,nullptr);
			if (hr == S_OK)
				hr = m_renderTexture->GetSurfaceLevel(0, &m_newRenderSurface);

			Int mode=RTT_MODE_UNAVAILABLE;
			if (hr == S_OK)
			{
				if (desc.MultiSampleType != D3DMULTISAMPLE_NONE && !preferVkMirror)
				{
					// Preferred route: keep the sample count of the scene render, then resolve it into the
					// single-sample texture the filters sample. Antialiasing survives the post pass.
					const DWORD qualities[2] = { desc.MultiSampleQuality, 0 };
					for (Int attempt=0; attempt<2 && !m_rttColorSurface; ++attempt)
					{
						if (device->CreateRenderTarget(desc.Width,desc.Height,desc.Format,desc.MultiSampleType,qualities[attempt],FALSE,&m_rttColorSurface,nullptr) != S_OK)
							continue;
						if (device->CreateDepthStencilSurface(desc.Width,desc.Height,depthDesc.Format,desc.MultiSampleType,qualities[attempt],FALSE,&m_rttDepthSurface,nullptr) == S_OK)
						{
							mode=RTT_MODE_MSAA_RESOLVE;
							break;
						}
						SAFE_RELEASE(m_rttColorSurface);
					}
				}

				if (mode == RTT_MODE_UNAVAILABLE)
				{
					// Either the back buffer needs no samples at all, or this device will not give us a
					// multisampled offscreen target: bind the single-sample texture against a single-sample
					// depth buffer of its own. The scene loses MSAA for as long as a filter is active, which
					// is still better than the filter not existing.
					if (device->CreateDepthStencilSurface(desc.Width,desc.Height,depthDesc.Format,D3DMULTISAMPLE_NONE,0,FALSE,&m_rttDepthSurface,nullptr) == S_OK)
					{
						m_rttColorSurface=m_newRenderSurface;
						m_rttColorSurface->AddRef();
						mode = desc.MultiSampleType != D3DMULTISAMPLE_NONE ? RTT_MODE_OWN_DEPTH : RTT_MODE_PLAIN;
					}
				}
			}

			// GeneralsX @feature Qoder 18/10/2026 step 9.H: give the recorder mirrors for the pair the filters
			// bind. The colour key is the texture the post passes sample, so one registration makes the target
			// followable and the sampled source valid; the depth surface has no container and is registered by
			// pointer, the same shape the sun shadow map uses. A refusal leaves every filter on the raw D3D9
			// redirect, which is the pre-9.H behaviour.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
			if (mode != RTT_MODE_UNAVAILABLE && preferVkMirror)
			{
				const WW3DZFormat zfmt = D3DFormat_To_WW3DZFormat((D3DFORMAT)depthDesc.Format);
				m_vkRttReady = DX8Wrapper::VK_Register_Render_Target(m_renderTexture,
					(unsigned)desc.Width, (unsigned)desc.Height, (int)desc.Format,
					m_rttDepthSurface, (int)zfmt);
				fprintf(stderr, "[RTT] vulkan mirror %s for %ux%u color=%p depth=%p zfmt=%d\n",
					m_vkRttReady ? "registered" : "refused", (unsigned)desc.Width, (unsigned)desc.Height,
					(void*)m_renderTexture, (void*)m_rttDepthSurface, (int)zfmt);
				fflush(stderr);
			}
#endif

			if (mode == RTT_MODE_UNAVAILABLE)
				releaseRenderToTextureSurfaces();
			Report_Render_To_Texture_Mode(mode, desc.Width, desc.Height, desc.MultiSampleType, desc.MultiSampleQuality);
		}
		else
			Report_Render_To_Texture_Mode(RTT_MODE_UNAVAILABLE, desc.Width, desc.Height, desc.MultiSampleType, desc.MultiSampleQuality);
	}

	W3DShaderInterface **shaders;

	for (i=0; MasterShaderList[i] != nullptr; i++)
	{
		shaders=MasterShaderList[i];
		for (j=0; shaders[j] != nullptr; j++)
		{
			if (shaders[j]->init())
				break;	//found a working shader
		}
	}
	W3DFilterInterface **filters;

	for (i=0; MasterFilterList[i] != nullptr; i++)
	{
		filters=MasterFilterList[i];
		for (j=0; filters[j] != nullptr; j++)
		{
			if (filters[j]->init())
				break;	//found a working shader
		}
	}

	DEBUG_LOG(("ShaderManager ChipsetID %d", res));

	// GeneralsX @feature T3-0 M3/M4: compile+create the dot3 vs_2_0/ps_2_0 pair, log to stderr whether DXVK accepts them.
	InitNormalMapShaders();
}

// W3DShaderManager::shutdown =======================================================
/** Any shaders which allocate resources will be allowed to free them */
//=============================================================================
void W3DShaderManager::shutdown()
{
	releaseRenderToTextureSurfaces();
	m_currentShader = ST_INVALID;
	m_currentFilter = FT_NULL_FILTER;
	//release any assets associated with a shader (vertex/pixel shaders, textures, etc.)
	Int i=0;
	for (; i<W3DShaderManager::ST_MAX; i++) {
		if (W3DShaders[i]) {
			W3DShaders[i]->shutdown();
		}
	}

	for (i=0; i < FT_MAX; i++)
	{
		if (W3DFilters[i])
		{
			W3DFilters[i]->shutdown();
		}
	}
}

//=============================================================================
void W3DShaderManager::updateCloud()
{
	terrainShader2Stage.updateCloud();
}

// W3DShaderManager::getShaderPasses =======================================================
/** Return number of renderig passes required in perform the desired shader on current
	hardware.  App will need to re-render the polygons this many times to complete the
	effect.
 */
//=============================================================================
Int W3DShaderManager::getShaderPasses(ShaderTypes shader)
{
	return W3DShadersPassCount[shader];
}

// W3DShaderManager::setShader =======================================================
/** Must call this method before each rendering pass in order to perform proper D3D
	setup for each shader.
 */
//=============================================================================
Int W3DShaderManager::setShader(ShaderTypes shader, Int pass)
{
	if (shader == m_currentShader && pass == m_currentShaderPass)
		return TRUE;	//shader is already set
	m_currentShader=shader;
	m_currentShaderPass = pass;
	if (W3DShaders[shader])
		return W3DShaders[shader]->set(pass);
	return FALSE;
}

// W3DShaderManager::resetShader =======================================================
/** Must call this method after all polygons and rendering passes have been submitted.
	This method allows D3D to reset itself to a default state that doesn't conflict
	with the WW3D2 Shader system.
 */
//=============================================================================
void W3DShaderManager::resetShader(ShaderTypes shader)
{
	if (m_currentShader == ST_INVALID)
		return;	//last shader is already reset.
	if (W3DShaders[shader])
		W3DShaders[shader]->reset();
	m_currentShader = ST_INVALID;
}
// W3DShaderManager::filterPreRender =======================================================
/** Call to view filter shaders before rendering starts.
 */
//=============================================================================
Bool W3DShaderManager::filterPreRender(FilterTypes filter, Bool &skipRender, CustomScenePassModes &scenePassMode)
{
	if (W3DFilters[filter])
	{
		// GeneralsX @bugfix Qoder 18/10/2026 step 9.H: m_currentFilter has to name the filter *while*
		// preRender runs. startRenderToTexture reads it for the soft-water alpha-only clear and, since
		// 9.H, for the render-target publish decision; assigning it after the call left both looking at
		// the previous filter. A filter that declines keeps the old value, as before.
		const FilterTypes previous = m_currentFilter;
		m_currentFilter = filter;
		Bool result=W3DFilters[filter]->preRender(skipRender,scenePassMode);
		if (!result)
			m_currentFilter = previous;
		return result;
	}
	return FALSE;
}

// W3DShaderManager::filterPostRender =======================================================
/** Call to view filter shaders after rendering is complete.
 */
//=============================================================================
Bool W3DShaderManager::filterPostRender(FilterTypes filter, FilterModes mode, Coord2D &scrollDelta, Bool &doExtraRender)
{
	if (W3DFilters[filter])
	{
		// GeneralsX @bugfix Qoder 18/10/2026 pure-Vulkan wedge 9: m_vkRttPublished is the verdict
		// startRenderToTexture reached for this pass. When it is false the scene never left the mirror
		// and the family's offscreen textures have no Vulkan content, so the whole post chain has to
		// stay on D3D9; letting any quad through overwrites the recorded 3D with an empty copy.
		if (!m_vkRttPublished)
		{
			ScopedD3DOnlyPostPass vkD3DOnly;
			return W3DFilters[filter]->postRender(mode, scrollDelta, doExtraRender);
		}
		return W3DFilters[filter]->postRender(mode, scrollDelta,doExtraRender);
	}

	m_currentFilter = FT_NULL_FILTER;
	return FALSE;
}

// W3DShaderManager::filterPostRender =======================================================
/** Call to view filter shaders after rendering is complete.
 */
//=============================================================================
	static Bool filterSetup(FilterTypes filter, FilterModes mode);
Bool W3DShaderManager::filterSetup(FilterTypes filter, FilterModes mode)
{
	if (W3DFilters[filter])
		return W3DFilters[filter]->setup(mode);
	return FALSE;
}

// GeneralsX @feature 05/10/2026 Bloom post-processing filter global toggle.
Bool W3DShaderManager::IsBloomFilterEnabled()
{
	return ScreenBloomFilter::IsEnabled();
}

// GeneralsX @feature 05/10/2026 Bloom post-processing filter global toggle.
void W3DShaderManager::SetBloomFilterEnabled(Bool enabled)
{
	ScreenBloomFilter::SetEnabled(enabled);
}

/*Draws 2 triangles covering the viewport given the current render states*/
void W3DShaderManager::drawViewport(Int color)
{
	struct _TRANS_LIT_TEX_VERTEX {
		D3DXVECTOR4 p;
		DWORD color;   // diffuse color
		float	u;
		float	v;
	} v[4];

	Int xpos, ypos, width, height;

	TheTacticalView->getOrigin(&xpos,&ypos);
	width=TheTacticalView->getWidth();
	height=TheTacticalView->getHeight();

	//bottom right
	v[0].p = D3DXVECTOR4( xpos+width-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[0].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[0].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top right
	v[1].p = D3DXVECTOR4( xpos+width-0.5f, ypos-0.5f, 0.0f, 1.0f );
	v[1].u = (Real)(xpos+width)/(Real)TheDisplay->getWidth();	v[1].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	//bottom left
	v[2].p = D3DXVECTOR4(  xpos-0.5f, ypos+height-0.5f, 0.0f, 1.0f );
	v[2].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[2].v = (Real)(ypos+height)/(Real)TheDisplay->getHeight();
	//top left
	v[3].p = D3DXVECTOR4(  xpos-0.5f,  ypos-0.5f, 0.0f, 1.0f );
	v[3].u = (Real)(xpos)/(Real)TheDisplay->getWidth();	v[3].v = (Real)(ypos)/(Real)TheDisplay->getHeight();
	v[0].color = color;
	v[1].color = color;
	v[2].color = color;
	v[3].color = color;

	//draw polygons like this is very inefficient but for only 2 triangles, it's
	//not worth bothering with index/vertex buffers.
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

	// GeneralsX @feature Qoder 18/10/2026 step 9.H: the offscreen pair is published to the recorder before
	// this quad arrives, so the alpha-only clear is a normal facade draw. The facade still falls back to
	// D3D9 on refusal, which keeps the non-Vulkan runs and the filters that did not opt in unchanged.
	DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, v, sizeof(_TRANS_LIT_TEX_VERTEX));
}

// GeneralsX @feature PS3.0 Weather W2 + W4 16/10/2026 ==================================================
// The screen half of the weather system: everything the recipe says the *air in front of the lens* does.
//
// Two passes over the tactical view rectangle, in this order:
//   1. cloud cover  - multiplies the finished frame by tint x darken, with a radial fall-off (the
//                     vignette) so a storm closes in from the edges instead of dimming uniformly.
//   2. veil + flash - adds the scrolling mist/snow veil and, for a thunderstorm, the white bolt frame.
//
// Why this is not a W3DFilterInterface: the filter system owns a single slot which the bloom filter
// already claims, and its render-to-texture chain cannot nest, so a second filter would silently steal
// or break the first. Blending over the completed frame needs neither the slot nor a scene sample, so
// weather and bloom simply compose. Both legs are therefore procedural: no textures are read, which
// also means nothing can go wrong with a missing asset.
//
// Ladder: ps_2_0-and-up runs one quad per pass with the constants below; the fixed-function fallback
// tessellates the same functions onto a 16x12 grid of vertex colours, so a device without programmable
// pixel shaders still gets tint, darken, vignette and veil (only the blob edges are softer). A shader
// failure can therefore never remove an effect, which is the same rule the bloom chain plays by.
//
// Source of the numbers is weatherRenderState(), which is computed in the device layer and never
// crosses into the CRC stream or a network packet (AGENTS.md Golden Rule 7).
// ======================================================================================================

/// Radial darkening factor: 1.0 through the middle 30% of the view, (1-strength) in the corners.
/// Shared by both legs so a fixed-function frame and a shader frame go grey by the same curve.
static Real weatherVignetteFactor(Real u, Real v, Real aspect, Real strength)
{
	const Real du = (u - 0.5f) * aspect;		//stretch x so the iso-lines are circles in pixel space
	const Real dv = v - 0.5f;
	const Real e2 = du * du + dv * dv;
	const Real m2 = 0.25f * aspect * aspect + 0.25f;	//squared half-diagonal: corners land on 1.0
	if (m2 <= 0.0f)
		return 1.0f;
	Real t = (e2 - 0.09f * m2) / (0.91f * m2);
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return 1.0f - strength * t;
}

/// Integer-mix hash in [0,1). The programmable leg uses the conventional frac(sin(dot())) fold instead
/// because ps_2_0 has no integer ops; the two place their blobs differently but read identically, which
/// is the whole point of procedural mist.
static Real weatherHash(Int x, Int y)
{
	UnsignedInt h = (UnsignedInt)x * 374761393u + (UnsignedInt)y * 668265263u;
	h = (h ^ (h >> 13)) * 1274126177u;
	h = h ^ (h >> 16);
	return (Real)(h & 0xffffffu) * (1.0f / 16777216.0f);
}

/// Bilinear value noise over unit cells, smooth-interpolated. Cell coordinates are never negative:
/// the scroll phase starts at 0 and only grows, so a plain cast is a floor.
static Real weatherValueNoise(Real x, Real y)
{
	const Int ix = (Int)x;
	const Int iy = (Int)y;
	Real fx = x - (Real)ix;
	Real fy = y - (Real)iy;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fy = fy * fy * (3.0f - 2.0f * fy);
	const Real a = weatherHash(ix, iy);
	const Real b = weatherHash(ix + 1, iy);
	const Real c = weatherHash(ix, iy + 1);
	const Real e = weatherHash(ix + 1, iy + 1);
	const Real ab = a + (b - a) * fx;
	const Real ce = c + (e - c) * fx;
	return ab + (ce - ab) * fy;
}

/// Two octaves, scrolling diagonally. Same cell density as kWeatherVeilPS so the two legs disagree only
/// in sharpness, never in scale. Seven cells across the height: at five the blobs were a fifth of the
/// screen each and read as separate white cushions rather than as mist on a lens.
static Real weatherVeilNoise(Real u, Real v, Real aspect, Real phase)
{
	const Real sx = 7.0f * aspect;
	const Real sy = 7.0f;
	const Real base = weatherValueNoise(u * sx + phase, v * sy + phase * 0.31f);
	const Real detail = weatherValueNoise(u * sx * 2.7f - phase * 0.53f, v * sy * 2.7f + phase * 0.11f);
	return base * 0.65f + detail * 0.35f;
}

/// 0..1 veil coverage at this point, i.e. how much mist is in front of the lens right here. The recipe's
/// density (the peak alpha this coverage is scaled by) is applied by the caller.
static Real weatherVeilAmount(Real u, Real v, Real aspect, Real phase)
{
	Real t = (weatherVeilNoise(u, v, aspect, phase) - 0.45f) * 2.2f;
	if (t < 0.0f) t = 0.0f;
	if (t > 1.0f) t = 1.0f;
	return t;
}

static DWORD Weather_Pack_Color(Real r, Real g, Real b)
{
	if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
	if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
	if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
	return (DWORD)(0xff000000u |
		((UnsignedInt)(r * 255.0f + 0.5f) << 16) |
		((UnsignedInt)(g * 255.0f + 0.5f) << 8) |
		((UnsignedInt)(b * 255.0f + 0.5f)));
}

// GeneralsX @bugfix PS3.0 Weather W2 16/10/2026: the veil leg now blends on SRCALPHA/INVSRCALPHA (see
// drawWeatherScreen), so its vertices carry a real alpha as well as a colour.
static DWORD Weather_Pack_Color_Alpha(Real r, Real g, Real b, Real a)
{
	if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
	return (Weather_Pack_Color(r, g, b) & 0x00ffffffu) | ((DWORD)(a * 255.0f + 0.5f) << 24);
}

// GeneralsX @feature PS3.0 Weather W2 16/10/2026: one lattice vertex of the veil leg. The blend
// multiplies the source by its own alpha, so the vertex carries the coverage in alpha and the
// un-premultiplied colour in rgb - the same split kWeatherVeilPS does on the programmable leg.
// w = this point's mist coverage, total = w + flash (the alpha), flash = the uniform bolt term.
static DWORD Weather_Pack_Veil(Real w, Real total, Real flash, Real veilR, Real veilG, Real veilB)
{
	const Real inv = total > 0.0001f ? 1.0f / total : 0.0f;
	const Real fw = flash * inv;
	return Weather_Pack_Color_Alpha(
		veilR * w * inv + 0.98f * fw,
		veilG * w * inv + 0.99f * fw,
		veilB * w * inv + 1.00f * fw,
		total);
}

// Transform-and-lit vertex for the fixed-function lattice: colour only, no texture coordinates, because
// neither leg samples anything.
struct WeatherScreenVertex
{
	D3DXVECTOR4 p;
	DWORD color;
};

// Grid resolution for the fixed-function legs. 16x12 quads is ~3 samples per noise cell, enough that the
// veil reads as blobs rather than as a gradient, and costs 408 vertices a frame.
enum { WEATHER_GRID_COLS = 17, WEATHER_GRID_ROWS = 13 };

#ifdef _WIN32
// c0.xyz = tint x darken, c0.w = vignette strength, c1.x = view aspect.
// Output is the multiplier itself; the blend state (SRCBLEND=DESTCOLOR) does the multiplying.
static const char kWeatherCloudPS[] =
"float4 c0 : register(c0);\n"
"float4 c1 : register(c1);\n"
"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
"{\n"
"    float2 d = uv - 0.5;\n"
"    d.x *= c1.x;\n"
"    float e2 = dot(d, d);\n"
"    float m2 = 0.25 * c1.x * c1.x + 0.25;\n"
"    float t = saturate((e2 - 0.09 * m2) / (0.91 * m2));\n"
"    return float4(c0.xyz * (1.0 - c0.w * t), 0.0);\n"
"}\n";

// c0.x = scroll phase, c0.y = density (peak veil alpha), c0.z = flash, c0.w = view aspect.
// c1.xyz = mist colour. The leg blends on SRCALPHA/INVSRCALPHA, so the return is (colour, coverage):
// the frame is pulled toward the mist rather than having light added on top of it, which is what mist
// in front of a lens does - it flattens contrast instead of glowing. A bolt rides the same alpha, and
// because its colour is white, saturating it to 1 clips the frame white exactly like the old additive
// pass did but without washing the mist patches out first.
static const char kWeatherVeilPS[] =
"float4 c0 : register(c0);\n"
"float4 c1 : register(c1);\n"
"float wnoise(float2 p)\n"
"{\n"
"    float2 i = floor(p);\n"
"    float2 f = frac(p);\n"
"    f = f * f * (3.0 - 2.0 * f);\n"
"    float a = frac(sin(dot(i, float2(12.9898, 78.233))) * 43758.5453);\n"
"    float b = frac(sin(dot(i + float2(1.0, 0.0), float2(12.9898, 78.233))) * 43758.5453);\n"
"    float c = frac(sin(dot(i + float2(0.0, 1.0), float2(12.9898, 78.233))) * 43758.5453);\n"
"    float e = frac(sin(dot(i + float2(1.0, 1.0), float2(12.9898, 78.233))) * 43758.5453);\n"
"    return lerp(lerp(a, b, f.x), lerp(c, e, f.x), f.y);\n"
"}\n"
"float4 main(float2 uv : TEXCOORD0) : COLOR\n"
"{\n"
"    float2 p = float2(uv.x * c0.w, uv.y) * 7.0;\n"
"    float n = wnoise(p + float2(c0.x, c0.x * 0.31));\n"
"    n = n * 0.65 + 0.35 * wnoise(p * 2.7 - float2(c0.x * 0.53, c0.x * -0.11));\n"
"    float w = saturate((n - 0.45) * 2.2) * c0.y;\n"
"    float a = saturate(w + c0.z);\n"
"    float3 col = (c1.xyz * w + float3(0.98, 0.99, 1.0) * c0.z) / max(a, 0.0001);\n"
"    return float4(col, a);\n"
"}\n";
#endif	// _WIN32

void W3DShaderManager::drawWeatherScreen()
{
	if (TheTacticalView == nullptr || TheDisplay == nullptr)
		return;

	const WeatherRenderState &wx = weatherRenderState();

	// Cloud multiplier, per channel. 1.0 everywhere means the weather is not touching the frame.
	Real mulR = wx.m_screenTint.red * wx.m_screenDarken;
	Real mulG = wx.m_screenTint.green * wx.m_screenDarken;
	Real mulB = wx.m_screenTint.blue * wx.m_screenDarken;
	if (mulR < 0.0f) mulR = 0.0f; else if (mulR > 1.0f) mulR = 1.0f;
	if (mulG < 0.0f) mulG = 0.0f; else if (mulG > 1.0f) mulG = 1.0f;
	if (mulB < 0.0f) mulB = 0.0f; else if (mulB > 1.0f) mulB = 1.0f;
	const Real vig = wx.m_screenVignette < 0.0f ? 0.0f : wx.m_screenVignette;

	Real flash = wx.m_flash;
	if (flash > 1.0f) flash = 1.0f;
	if (flash < 0.0f) flash = 0.0f;
	// GeneralsX @bugfix PS3.0 Weather W4 16/10/2026: this rode the veil leg at 0.92 of full alpha, and the
	// veil blends on SRCALPHA/INVSRCALPHA - so 0.92 is a lerp that puts 92% of the mist colour over the
	// frame and leaves 8% of the battlefield. That is a white screen with a screenshot on it, not a
	// thunderstorm. A bolt is meant to lift the frame, briefly, with the scene still readable through it,
	// which is what half coverage gives; the rest of the brightness is carried by the dynamic light.
	flash *= 0.50f;

	static int s_env = -1;
	if (s_env < 0)
	{
		// QA leg: GENERALSX_WEATHER_SCREEN=0 keeps the world particles and lighting coupling but takes
		// the screen layer out, which is how the A/B captures separate the two halves of the effect.
		const char *ev = getenv("GENERALSX_WEATHER_SCREEN");
		s_env = (ev != nullptr && atoi(ev) == 0) ? 0 : 1;
	}

	const Bool doCloud = (mulR < 0.999f || mulG < 0.999f || mulB < 0.999f || vig > 0.001f);
	const Bool doVeil = (wx.m_screenWispDensity > 0.001f || flash > 0.001f);
	if (!s_env || (!doCloud && !doVeil))
		return;

	static Real s_veilGain = -1.0f;
	if (s_veilGain < 0.0f)
	{
		const char *ev = getenv("GENERALSX_WEATHER_VEIL");
		s_veilGain = ev != nullptr ? (Real)atof(ev) : 1.0f;
		if (s_veilGain < 0.0f) s_veilGain = 0.0f;
		else if (s_veilGain > 4.0f) s_veilGain = 4.0f;
	}
	const Real density = wx.m_screenWispDensity * s_veilGain;

	// Mist drift. Frame-stepped rather than clock-stepped on purpose: it is cosmetic, and reading a
	// wall clock here would make the veil shape depend on machine timing for no benefit. Wrapping at
	// 1024 keeps the float far from its resolution cliff; the noise is not periodic, so the wrap
	// shuffles the blobs once after roughly 45 minutes of play - invisible, and never a gameplay event.
	static Real s_phase = 0.0f;
	if (doVeil)
	{
		s_phase += 0.006f;
		if (s_phase > 1024.0f) s_phase -= 1024.0f;
	}

	Int xpos, ypos, width, height;
	TheTacticalView->getOrigin(&xpos, &ypos);
	width = TheTacticalView->getWidth();
	height = TheTacticalView->getHeight();
	if (width <= 1 || height <= 1)
		return;
	const Real aspect = (Real)width / (Real)height;
	const Real x0 = (Real)xpos, y0 = (Real)ypos;
	const Real x1 = (Real)(xpos + width), y1 = (Real)(ypos + height);

	LPDIRECT3DDEVICE9 pDev = DX8Wrapper::_Get_D3D_Device8();
	if (pDev == nullptr)
		return;

	BloomQuadVertex vbQuad[4];	//the programmable legs each fill one screen quad with view-local uv 0..1

	// One-time shader build, on the first frame that actually has weather: a map that stays clear never
	// pays for compiling (or holds onto) the two programs.
	enum WeatherPSState { WXPS_UNTRIED = 0, WXPS_READY = 1, WXPS_FF = 2 };
	static WeatherPSState s_ps = WXPS_UNTRIED;
	static IDirect3DPixelShader9 *s_cloudPS = nullptr;
	static IDirect3DPixelShader9 *s_veilPS = nullptr;
	if (s_ps == WXPS_UNTRIED)
	{
#ifdef _WIN32
		if (getenv("GENERALSX_WEATHER3") != nullptr && atoi(getenv("GENERALSX_WEATHER3")) == 0)
		{
			s_ps = WXPS_FF;
		}
		else
		{
			const char *profile = nullptr;
			void *handle = nullptr;
			if (FAILED(W3DShaderManager::compileShaderAtBestProfile(kWeatherCloudPS, false, "[WXSCREEN]", &profile, &handle)) || handle == nullptr)
			{
				s_ps = WXPS_FF;
			}
			else
			{
				s_cloudPS = (IDirect3DPixelShader9*)handle;
				handle = nullptr;
				if (FAILED(W3DShaderManager::compileShaderAtBestProfile(kWeatherVeilPS, false, "[WXSCREEN]", &profile, &handle)) || handle == nullptr)
				{
					s_cloudPS->Release();
					s_cloudPS = nullptr;
					s_ps = WXPS_FF;
				}
				else
				{
					s_veilPS = (IDirect3DPixelShader9*)handle;
					s_ps = WXPS_READY;
					fprintf(stderr, "[WXSCREEN] cloud+veil on %s\n", profile ? profile : "?");
					fflush(stderr);
				}
			}
		}
#else
		// Runtime HLSL is Windows-only (the D3D9 shim is the only compiler in the shipped build), so the
		// Linux/macOS path runs the lattice below. Same four scalars, softer veil.
		s_ps = WXPS_FF;
#endif
	}

	// Presets go through DX8Wrapper so its state cache stays truthful, exactly like the bloom filter's
	// set() does; only after that is the device state worth snapshotting for the restore below.
	{
		VertexMaterialClass *vmat = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
		DX8Wrapper::Set_Material(vmat);
		REF_PTR_RELEASE(vmat);	//preset, no need to keep a reference
		DX8Wrapper::Set_Shader(ShaderClass::_PresetOpaqueShader);
		DX8Wrapper::Apply_Render_State_Changes();
	}

	DWORD oldBlendEnable, oldSrcBlend, oldDstBlend, oldZEnable, oldZWrite, oldZFunc;
	DWORD oldAlphaTest, oldCull, oldFog, oldLighting;
	oldBlendEnable = DX8Wrapper::VK_Read_RS(D3DRS_ALPHABLENDENABLE);
	oldSrcBlend = DX8Wrapper::VK_Read_RS(D3DRS_SRCBLEND);
	oldDstBlend = DX8Wrapper::VK_Read_RS(D3DRS_DESTBLEND);
	oldZEnable = DX8Wrapper::VK_Read_RS(D3DRS_ZENABLE);
	oldZWrite = DX8Wrapper::VK_Read_RS(D3DRS_ZWRITEENABLE);
	oldZFunc = DX8Wrapper::VK_Read_RS(D3DRS_ZFUNC);
	oldAlphaTest = DX8Wrapper::VK_Read_RS(D3DRS_ALPHATESTENABLE);
	oldCull = DX8Wrapper::VK_Read_RS(D3DRS_CULLMODE);
	oldFog = DX8Wrapper::VK_Read_RS(D3DRS_FOGENABLE);
	oldLighting = DX8Wrapper::VK_Read_RS(D3DRS_LIGHTING);

	// The fixed-function legs rewrite stage 0/1; the programmable legs ignore them, but saving and
	// restoring is cheap and keeps the cache honest either way.
	DWORD oldStage[8];
	const D3DTEXTURESTAGESTATETYPE stageKeys[8] =
	{
		D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP,
		D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2, D3DTSS_TEXCOORDINDEX, D3DTSS_TEXTURETRANSFORMFLAGS
	};
	for (Int s = 0; s < 8; ++s)
		pDev->GetTextureStageState(0, stageKeys[s], &oldStage[s]);
	DWORD oldStage1[2];
	pDev->GetTextureStageState(1, D3DTSS_COLOROP, &oldStage1[0]);
	pDev->GetTextureStageState(1, D3DTSS_ALPHAOP, &oldStage1[1]);

	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);
	// Direct3D 8 exposes no alpha-write mask (D3DRS_ALPHAWRITEENABLE is a D3D9 state), so the two blend
	// factors below do touch the back-buffer alpha: the cloud pass multiplies it, the veil pass adds to it.
	// Nothing in this engine reads destination alpha - the 2D UI blends on SRC_ALPHA/INV_SRC_ALPHA, the
	// scene clear writes a constant 1.0, and no filter samples the alpha channel - so the effect is
	// invisible by construction rather than suppressed by a state the API cannot set.

	// Veil colour: mist is lighter than the scene it dims, tinted by the same recipe that dims the sky,
	// so a blue-grey storm veil and a white blizzard veil both follow what the INI author wrote.
	Real veilR = 0.62f + 0.38f * wx.m_screenTint.red;
	Real veilG = 0.62f + 0.38f * wx.m_screenTint.green;
	Real veilB = 0.62f + 0.38f * wx.m_screenTint.blue;

	Bool usedShader = FALSE;

	if (doCloud)
	{
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, TRUE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ZERO);

		if (s_ps == WXPS_READY && s_cloudPS != nullptr)
		{
			usedShader = TRUE;
			// uv is the position across the view, 0..1, not a texture lookup: nothing is sampled.
			BuildBloomQuad(vbQuad, x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffff);
			float c0[4] = { mulR, mulG, mulB, vig };
			float c1[4] = { aspect, 0.0f, 0.0f, 0.0f };
			DX8Wrapper::Apply_Render_State_Changes();
			DX8Wrapper::Set_Vertex_Shader(0);
			DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(s_cloudPS));
			DX8Wrapper::Set_Pixel_Shader_Constant(0, c0, 1);
			DX8Wrapper::Set_Pixel_Shader_Constant(1, c1, 1);
			DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
			DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, vbQuad, sizeof(BloomQuadVertex));
			DX8Wrapper::Set_Pixel_Shader(0);
		}
		else
		{
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
			DX8Wrapper::Apply_Render_State_Changes();
			DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
			WeatherScreenVertex row[2 * WEATHER_GRID_COLS];
			for (Int r = 0; r + 1 < WEATHER_GRID_ROWS; ++r)
			{
				const Real vA = (Real)r / (Real)(WEATHER_GRID_ROWS - 1);
				const Real vB = (Real)(r + 1) / (Real)(WEATHER_GRID_ROWS - 1);
				const Real yA = y0 + (y1 - y0) * vA;
				const Real yB = y0 + (y1 - y0) * vB;
				for (Int c = 0; c < WEATHER_GRID_COLS; ++c)
				{
					const Real u = (Real)c / (Real)(WEATHER_GRID_COLS - 1);
					const Real x = x0 + (x1 - x0) * u;
					const Real fa = weatherVignetteFactor(u, vA, aspect, vig);
					const Real fb = weatherVignetteFactor(u, vB, aspect, vig);
					Int i = c * 2;
					row[i].p = D3DXVECTOR4(x, yB, 0.0f, 1.0f);
					row[i].color = Weather_Pack_Color(mulR * fb, mulG * fb, mulB * fb);
					row[i + 1].p = D3DXVECTOR4(x, yA, 0.0f, 1.0f);
					row[i + 1].color = Weather_Pack_Color(mulR * fa, mulG * fa, mulB * fa);
				}
				DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2 * (WEATHER_GRID_COLS - 1), row, sizeof(WeatherScreenVertex));
			}
		}
	}

	if (doVeil)
	{
		// GeneralsX @bugfix PS3.0 Weather W2 16/10/2026: this used to be ONE/ONE (pure add). With the
		// recipe densities of the first draft that put up to 0.57 of white on top of the frame wherever a
		// noise blob sat, so the storm read as glowing cushions of light. Alpha-blended, the same numbers
		// become a lerp toward the mist colour: bright ground gets dimmed, dark ground gets lifted, and
		// the whole screen loses a little contrast - which is what haze does.
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, TRUE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

		if (s_ps == WXPS_READY && s_veilPS != nullptr)
		{
			usedShader = TRUE;
			BuildBloomQuad(vbQuad, x0, y0, x1, y1, 0.0f, 0.0f, 1.0f, 1.0f, 0xffffffff);
			float c0[4] = { s_phase, density, flash, aspect };
			float c1[4] = { veilR, veilG, veilB, 0.0f };
			DX8Wrapper::Apply_Render_State_Changes();
			DX8Wrapper::Set_Vertex_Shader(0);
			DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(s_veilPS));
			DX8Wrapper::Set_Pixel_Shader_Constant(0, c0, 1);
			DX8Wrapper::Set_Pixel_Shader_Constant(1, c1, 1);
			DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
			DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2, vbQuad, sizeof(BloomQuadVertex));
			DX8Wrapper::Set_Pixel_Shader(0);
		}
		else
		{
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
			DX8Wrapper::Apply_Render_State_Changes();
			DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
			WeatherScreenVertex row[2 * WEATHER_GRID_COLS];
			for (Int r = 0; r + 1 < WEATHER_GRID_ROWS; ++r)
			{
				const Real vA = (Real)r / (Real)(WEATHER_GRID_ROWS - 1);
				const Real vB = (Real)(r + 1) / (Real)(WEATHER_GRID_ROWS - 1);
				const Real yA = y0 + (y1 - y0) * vA;
				const Real yB = y0 + (y1 - y0) * vB;
				for (Int c = 0; c < WEATHER_GRID_COLS; ++c)
				{
					const Real u = (Real)c / (Real)(WEATHER_GRID_COLS - 1);
					const Real x = x0 + (x1 - x0) * u;
					// A bolt is uniform, so it rides on every lattice vertex; only the veil is shaped.
					const Real wa = density * weatherVeilAmount(u, vA, aspect, s_phase);
					const Real wb = density * weatherVeilAmount(u, vB, aspect, s_phase);
					Int i = c * 2;
					row[i].p = D3DXVECTOR4(x, yB, 0.0f, 1.0f);
					row[i].color = Weather_Pack_Veil(wb, wb + flash, flash, veilR, veilG, veilB);
					row[i + 1].p = D3DXVECTOR4(x, yA, 0.0f, 1.0f);
					row[i + 1].color = Weather_Pack_Veil(wa, wa + flash, flash, veilR, veilG, veilB);
				}
				DX8Wrapper::Draw_User_Primitive(D3DPT_TRIANGLESTRIP, 2 * (WEATHER_GRID_COLS - 1), row, sizeof(WeatherScreenVertex));
			}
		}
	}

	// Put the frame back the way the scene renderer expects it: no blending, depth test on and writing,
	// and every stage state restored through DX8Wrapper so its cache matches the device again.
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, oldBlendEnable);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, oldSrcBlend);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, oldDstBlend);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZENABLE, oldZEnable);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZWRITEENABLE, oldZWrite);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ZFUNC, oldZFunc);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, oldAlphaTest);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE, oldCull);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_FOGENABLE, oldFog);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_LIGHTING, oldLighting);
	for (Int s = 0; s < 8; ++s)
		DX8Wrapper::Set_DX8_Texture_Stage_State(0, stageKeys[s], oldStage[s]);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, oldStage1[0]);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, oldStage1[1]);
	if (usedShader)
		DX8Wrapper::Set_Vertex_Shader(0);	//D18 contract: nothing raw-bound may outlive this call
	DX8Wrapper::Apply_Render_State_Changes();

	// One line per leg on the first draw of a type, so a capture can tell "weather is off" apart from
	// "the screen layer ran but the recipe asked for nothing" without reading the whole log.
	static Int s_lastLoggedType = -1;
	const Int nowType = (Int)wx.m_type;
	if (nowType != s_lastLoggedType)
	{
		s_lastLoggedType = nowType;
		fprintf(stderr, "[WXSCREEN] type=%d mul=(%.3f,%.3f,%.3f) vig=%.2f veil=%.2f flash=%.2f leg=%s rect=%dx%d\n",
			nowType, mulR, mulG, mulB, vig, density, flash,
			s_ps == WXPS_READY ? "hlsl" : "ff", width, height);
		fflush(stderr);
	}
}

// W3DShaderManager::startRenderToTexture =======================================================
/** Starts rendering to a texture.
 */
//=============================================================================
void W3DShaderManager::startRenderToTexture()
{
	DEBUG_ASSERTCRASH(!m_renderingToTexture, ("Already rendering to texture - cannot nest calls."));

	if (m_renderingToTexture || m_newRenderSurface==nullptr || m_rttColorSurface==nullptr || m_rttDepthSurface==nullptr) return;
	// GeneralsX @bugfix Storm996 12/10/2026 Bind the offscreen pair created above. The back buffer's depth
	// buffer is deliberately not touched here: it may be multisampled while the offscreen color is not.
	HRESULT hr = DX8Wrapper::_Get_D3D_Device8()->SetRenderTarget(0,m_rttColorSurface);
	if (hr == S_OK)
		hr = DX8Wrapper::_Get_D3D_Device8()->SetDepthStencilSurface(m_rttDepthSurface);

	// TheSuperHackers @bugfix If SetRenderTarget fails (e.g. due to MSAA forced by driver
	// profile causing a depth buffer mismatch that D3DSURFACE_DESC doesn't report), permanently
	// disable RTT to prevent repeated failures and accidental backbuffer clears.
	// GeneralsX @tweak PS3.0-P2-3 14/10/2026 S6: one transient failure used to kill every filter
	// for the session. Now the frame is simply skipped and retried next frame; only a run of
	// consecutive failures trips the fuse and releases the surfaces (old behavior) so genuinely
	// broken setups still stop hammering SetRenderTarget.
	if (hr != S_OK)
	{
		const Int kRttBindFuseThreshold = 30;
		if (++m_rttBindFailures < kRttBindFuseThreshold)
			return;	// skip this filter pass, keep the surfaces, retry next frame
		fprintf(stderr, "[RTT] SetRenderTarget failed %d times in a row; disabling render-to-texture filters\n",
			(int)m_rttBindFailures);
		fflush(stderr);
		releaseRenderToTextureSurfaces();
		return;
	}
	if (m_rttBindFailures != 0)
		m_rttBindFailures = 0;

	// GeneralsX @feature Qoder 18/10/2026 step 9.H: point the recorder at the offscreen pair before the
	// depth clear and the soft-water alpha quad below, so both land in the target the scene is about to
	// use. Only a family whose entire pass is facade-consumable opts in; bloom and the HLSL B&W filter
	// keep the recorder on the scene mirror, because their runtime-compiled post shaders would be refused
	// after the scene had already left that mirror.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	if (m_vkRttReady && VKDraw_Ready() && W3DFilters[m_currentFilter] &&
			W3DFilters[m_currentFilter]->canVulkanRtt())
	{
		const Bool owned = DX8Wrapper::VK_Publish_Render_Target(m_rttColorSurface, m_rttDepthSurface);
		m_vkRttPublished = true;
		// GeneralsX @feature Qoder 18/10/2026 step 9.H: one line per filter family, so a run that
		// actually reaches this pass proves the recorder followed the redirect instead of only proving
		// the mirror was registered at init time.
		{
			static unsigned reportedFilters = 0;
			const unsigned bit = 1u << (unsigned)m_currentFilter;
			if (m_currentFilter > FT_NULL_FILTER && m_currentFilter < FT_MAX && !(reportedFilters & bit))
			{
				reportedFilters |= bit;
				fprintf(stderr, "[RTT] recorder target published filter=%d owned=%d color=%p depth=%p\n",
					(int)m_currentFilter, (int)owned, (void*)m_rttColorSurface, (void*)m_rttDepthSurface);
				fflush(stderr);
			}
		}
	}
#endif

	// GeneralsX @bugfix PS3.0 batch6 13/10/2026 The offscreen depth buffer has to be cleared here. The per-frame
	// clear (WW3D::Begin_Render) ran against the back buffer before the switch, and the depth that comes with it
	// stays behind with that back buffer: the pair bound above owns a fresh depth surface, which starts at zero,
	// so every fragment failed the depth test and the whole scene was discarded. The filters then copied that
	// empty target back over the viewport, which is what made every 3D surface go black while the 2D GUI -
	// drawn afterwards, outside the filter - stayed lit. The retail filters never exposed this because with
	// multisampling on, canRenderToTexture() was false and no filter ran at all.
	DX8Wrapper::Clear(false, true, Vector3( 0.0f, 0.0f, 0.0f ), 1.0f);
	m_renderingToTexture = true;
#ifdef _WIN32
	// GeneralsX @bugfix Storm996 12/10/2026 The creation-time [RTT] line only says the target exists. This
	// one says a view actually redirected the scene through it, which is what a filter has to have to work.
	{
		static Bool redirectedReported = false;
		if (!redirectedReported)
		{
			redirectedReported = true;
			fprintf(stderr, "[RTT] scene redirected into the offscreen target (%s)\n",
				m_rttColorSurface != m_newRenderSurface ? "multisampled, resolved after the scene" : "single-sample");
			fflush(stderr);
		}
	}
#endif // _WIN32
	if (TheGlobalData->m_showSoftWaterEdge)
	{
		//Soft water edges use frame buffer destination alpha so we must clear it to a known value.
		if (m_currentFilter == FT_VIEW_MOTION_BLUR_FILTER || m_currentFilter == FT_VIEW_CROSSFADE)
		{
			//these filters rely on the previous frame being visible so we must be careful about clearing
			//frame buffer.  Only clear the alpha channel
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_ALPHA);	//only clear alpha
			ShaderClass shader=ShaderClass::_PresetOpaqueSolidShader;
			shader.Set_Depth_Compare(ShaderClass::PASS_ALWAYS);
			shader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);
			DX8Wrapper::Set_Shader(shader);

			VertexMaterialClass *vmat=VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
			DX8Wrapper::Set_Material(vmat);
			REF_PTR_RELEASE(vmat);	//no need to keep a reference since it's a preset.

			drawViewport(0x00ffffff | (((Int)(TheWaterTransparency->m_minWaterOpacity*255.0f)) <<24));
			DX8Wrapper::Set_DX8_Render_State(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE);	//disable writes to alpha
		}
		else	//normal clear that overwrites everything.
			DX8Wrapper::Clear(true, false, Vector3( 0.0f, 0.0f, 0.0f ), TheWaterTransparency->m_minWaterOpacity);
	}
}

// W3DShaderManager::startRenderToTexture =======================================================
/** Ends rendering to a texture.
 */
//=============================================================================
IDirect3DTexture9 *W3DShaderManager::endRenderToTexture()
{
	DEBUG_ASSERTCRASH(m_renderingToTexture, ("Not rendering to texture."));
	if (!m_renderingToTexture) return nullptr;
	HRESULT hr = DX8Wrapper::_Get_D3D_Device8()->SetRenderTarget(0,m_oldRenderSurface); DX8Wrapper::_Get_D3D_Device8()->SetDepthStencilSurface(m_oldDepthSurface);	//restore original render target
	DEBUG_ASSERTCRASH(hr==S_OK, ("Set target failed unexpectedly."));
	if (hr == S_OK)
	{
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 18/10/2026 step 9.H: the scene target is current again, so hand the
		// recorder back before the filter's quad arrives. That quad is what copies the offscreen image
		// into the mirror the present chain reads, which is why the family had to opt in before the switch.
		if (m_vkRttPublished)
		{
			DX8Wrapper::VK_Publish_Scene_Target();
			m_vkRttPublished = false;
		}
#endif

		// GeneralsX @bugfix Storm996 12/10/2026 A multisampled surface is not sampler-readable, so the scene
		// we just drew into m_rttColorSurface has to be resolved into the texture below. This runs after the
		// back buffer is current again because a surface bound as render target cannot be a StretchRect source.
		if (m_rttColorSurface != m_newRenderSurface)
		{
			hr = DX8Wrapper::_Get_D3D_Device8()->StretchRect(m_rttColorSurface,nullptr,m_newRenderSurface,nullptr,D3DTEXF_NONE);
			if (hr != S_OK)
			{
#ifdef _WIN32
				static Bool reported=false;
				if (!reported) {
					reported=true;
					fprintf(stderr, "[RTT] resolve into the sampled texture failed (0x%lX); post-processing keeps the un-resolved frame\n",(unsigned long)hr);
					fflush(stderr);
				}
#endif // _WIN32
				// The filters still sample whatever the previous resolve left in the texture. Failing the
				// frame here would leave m_renderingToTexture stuck and disable the feature anyway.
				hr = S_OK;
			}
			else
			{
#ifdef _WIN32
				// One line per run is enough to prove the multisampled path is not just created but used.
				static Bool resolveReported=false;
				if (!resolveReported) {
					resolveReported=true;
					fprintf(stderr, "[RTT] multisampled scene resolved into the sampled texture\n");
					fflush(stderr);
				}
#endif // _WIN32
			}
		}

		//assume render target texture will be in stage 0.  Most hardware has "conditional" support for
		//non-power-of-2 textures so we must force some required states:
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSW, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

		m_renderingToTexture = false;
	}
	return m_renderTexture;
}

/**Returns texture containing the image that was last rendered using any of the effects requiring render target
textures.  Used mostly for cross-fading effects that need an unmodified version of the view before the effect
was applied.  NOTE: This texture does not survive device reset.. so quit effect on reset!*/
IDirect3DTexture9 *W3DShaderManager::getRenderTexture()
{
	return m_renderTexture;
}

enum GraphicsVenderID CPP_11(: Int)
{
	DC_NVIDIA_VENDOR_ID	= 0x10DE,
	DC_3DFX_VENDOR_ID	= 0x121A,
	DC_ATI_VENDOR_ID	= 0x1002
};

// W3DShaderManager::ChipsetType =======================================================
/** Returns the chipset used by the currently active rendering device.  Can be useful
	for coding around specific driver bugs.
 */
//=============================================================================
// GeneralsX @feature PS3.0 batch2 12/10/2026 ---------------------------------------------------------------
// The shader-model ceiling the user asked for, as a tier. One table serves both -shadermodel and the
// GENERALSX_SHADERMODEL environment variable, so the two channels cannot drift apart. Returns DC_MAX
// for "no ceiling requested" (auto/0/empty) and sets *recognized=false for a token we do not know -
// silently ignoring a typo would change the render tier without any visible trace.
// 1.3 maps onto the 1.1 tier: there is no 1.3 tier, and a ceiling must never hand out features the
// user withheld.
static ChipsetType ShaderModelCapToTier(const char* text, Bool* recognized)
{
	if (recognized) *recognized=false;
	if (!text || !*text) {
		if (recognized) *recognized=true;
		return DC_MAX;
	}

	char lower[32];
	Int i=0;
	for (; text[i] && i<31; ++i) {
		char c=text[i];
		lower[i]=(c>='A' && c<='Z')?(char)(c+('a'-'A')):c;
	}
	lower[i]='\0';

	if (!strcmp(lower,"auto") || !strcmp(lower,"0")) {
		*recognized=true;
		return DC_MAX;
	}
	if (!strcmp(lower,"1.1") || !strcmp(lower,"11") || !strcmp(lower,"1.3") || !strcmp(lower,"13")) {
		*recognized=true;
		return DC_GENERIC_PIXEL_SHADER_1_1;
	}
	if (!strcmp(lower,"1.4") || !strcmp(lower,"14")) {
		*recognized=true;
		return DC_GENERIC_PIXEL_SHADER_1_4;
	}
	if (!strcmp(lower,"2.0") || !strcmp(lower,"2") || !strcmp(lower,"20")) {
		*recognized=true;
		return DC_GENERIC_PIXEL_SHADER_2_0;
	}
	if (!strcmp(lower,"3.0") || !strcmp(lower,"3") || !strcmp(lower,"30")) {
		*recognized=true;
		return DC_GENERIC_PIXEL_SHADER_3_0;
	}
	return DC_MAX;
}

// GeneralsX @feature PS3.0 batch2 12/10/2026 The ceiling can only lower the detected tier, never
// raise it: a request for 3.0 on a device that reports 2.0 stays on 2.0. Command line wins over the
// environment variable.
static ChipsetType applyShaderModelCap(ChipsetType detected)
{
	const char* requested=nullptr;
	static const char* s_envCap=getenv("GENERALSX_SHADERMODEL");

	if (TheGlobalData && !TheGlobalData->m_shaderModelCap.isEmpty())
		requested=TheGlobalData->m_shaderModelCap.str();
	else if (s_envCap && *s_envCap)
		requested=s_envCap;
	if (!requested)
		return detected;

	Bool recognized=false;
	ChipsetType cap=ShaderModelCapToTier(requested,&recognized);
	if (!recognized) {
		static Bool s_warned=false;
		if (!s_warned) {
			s_warned=true;
			fprintf(stderr,"[SHADERMODEL] Ignored unknown shader model '%s' "
				"(use 1.1, 1.4, 2.0, 3.0 or auto)\n",requested);
			fflush(stderr);
		}
		return detected;
	}
	if (cap==DC_MAX || detected<=cap)
		return detected;

	static ChipsetType s_reportedCap=DC_MAX;
	if (s_reportedCap!=cap) {
		s_reportedCap=cap;
		fprintf(stderr,"[SHADERMODEL] Tier capped: hardware tier %d -> requested tier %d\n",
			(int)detected,(int)cap);
		fflush(stderr);
	}
	return cap;
}

// GeneralsX @feature PS3.0 batch2 12/10/2026 ---------------------------------------------------------
// The generic capability ladder: the pixel-shader model the device reports, gated by how many texture
// stages a shader of that model can actually use. Deliberately a single function with two callers (the
// legacy chipset classifier below, and getShaderModelTier) so the two can never disagree about what
// ps_3_0 requires.
//
// Comparing the reported (major,minor) pair as integers: the code this replaced printed "1.10" through
// sprintf and read it back with %f, which turns shader model 1.10 into 1.1 and 2.10 into 2.1 - and it
// could not express 3.0 at all because the ladder stopped at DC_GENERIC_PIXEL_SHADER_2_0.
static ChipsetType ShaderModelTierFromCaps()
{
	ChipsetType chip=DC_UNKNOWN;
	const DX8Caps* caps=DX8Wrapper::Get_Current_Caps();

	if (caps)
	{
		Int maxTextures=caps->Get_Max_Simultaneous_Textures();
		Int psMajor=caps->Get_Pixel_Shader_Major_Version();
		Int psMinor=caps->Get_Pixel_Shader_Minor_Version();

		if (maxTextures >= 4)
		{	if (psMajor > 1 || (psMajor == 1 && psMinor >= 1))
				chip=DC_GENERIC_PIXEL_SHADER_1_1;
			if (psMajor > 1 || (psMajor == 1 && psMinor >= 4))
				chip=DC_GENERIC_PIXEL_SHADER_1_4;
			if (maxTextures >= 8 && psMajor >= 2)
				chip=DC_GENERIC_PIXEL_SHADER_2_0;
			// ps_3_0 unlocks flow control, 32 temporaries and 224 pixel constants. Both runtimes
			// measured in the port report 3.0 (native d3d9 and DXVK 2.6), so this is the tier the SM3
			// render paths gate on.
			if (maxTextures >= 8 && psMajor >= 3)
				chip=DC_GENERIC_PIXEL_SHADER_3_0;
		}
	}

	return chip;
}

// GeneralsX @feature PS3.0 batch2 12/10/2026 Detection split out of getChipset() so every return
// path - the vendor-id matches included - goes through the shader-model ceiling.
ChipsetType W3DShaderManager::detectChipsetTier()
{
	ChipsetType chip=DC_UNKNOWN;
	IDirect3D9* d3d8Interface=DX8Wrapper::_Get_D3D8();

	if (d3d8Interface && DX8Wrapper::_Get_D3D_Device8())
	{

		D3DADAPTER_IDENTIFIER9 did;
		::ZeroMemory(&did, sizeof(D3DADAPTER_IDENTIFIER9));
	/*	HRESULT res = */ d3d8Interface->GetAdapterIdentifier(0,0,&did);
		*((LARGE_INTEGER*)&m_driverVersion) = did.DriverVersion;

		if(did.VendorId == DC_NVIDIA_VENDOR_ID)
		{
			m_currentVendor = DC_NVIDIA_VENDOR_ID;

			if (did.DeviceId == 0x20)
				return DC_TNT;

			if (did.DeviceId >= 0x28 && did.DeviceId < 0x100)
				return DC_TNT2;

			if ( (did.DeviceId >= 0x100 && did.DeviceId <= 0x103) ||	//GeForce
				 (did.DeviceId >= 0x110 && did.DeviceId <= 0x113) ||	//GeForce2 MX
						 (did.DeviceId >= 0x150 && did.DeviceId <= 0x153) )	//GeForce2
           		return DC_GEFORCE2;

			if (did.DeviceId >= 0x200 && did.DeviceId < 0x250)
				return DC_GEFORCE3;

			if (did.DeviceId >= 0x250)
				return DC_GEFORCE4;
		}
		else
		if(did.VendorId == DC_3DFX_VENDOR_ID)
		{
			m_currentVendor = DC_3DFX_VENDOR_ID;

			if (did.DeviceId == 0x0002)
				return DC_VOODOO2;
			if (did.DeviceId == 0x0005)
				return DC_VOODOO3;
			if (did.DeviceId == 0x0008)	///@todo: Just guessing on this one - find actual Voodoo4 deviceID.
				return DC_VOODOO4;
			if (did.DeviceId == 0x0009)
				return DC_VOODOO5;
		}
		else
		if(did.VendorId == DC_ATI_VENDOR_ID)
		{
			m_currentVendor = DC_ATI_VENDOR_ID;

			if (did.DeviceId == 0x5144)
				return DC_RADEON;
			if (did.DeviceId == 0x514C)
				return DC_RADEON_8500;
			if (did.DeviceId == 0x4e44)
				return DC_RADEON_9700;
		}

		//None of the vendor specific ID's matched so use generic means to classify the card
		chip=ShaderModelTierFromCaps();
	}

	return chip;
}

// GeneralsX @feature PS3.0 batch2 12/10/2026 ---------------------------------------------------------
// One-time console record of both tier answers. Until now the engine picked them silently, so "the SM3
// paths are not running" was indistinguishable from "the tier ladder is broken" without a debugger -
// and the first run on real hardware needed exactly that distinction. Throttled by value, not by
// count: a mid-session change (a cap being applied late, a chipset override) is worth a line.
static const char* Chipset_Tier_Name(ChipsetType tier)
{
	switch (tier)
	{
		case DC_UNKNOWN:						return "unknown";
		case DC_GENERIC_PIXEL_SHADER_1_1:		return "ps_1_1";
		case DC_GENERIC_PIXEL_SHADER_1_4:		return "ps_1_4";
		case DC_GENERIC_PIXEL_SHADER_2_0:		return "ps_2_0";
		case DC_GENERIC_PIXEL_SHADER_3_0:		return "ps_3_0";
		default:								return "vendor-chipset";
	}
}

static void Report_Shader_Tiers(ChipsetType legacy, ChipsetType shaderModel)
{
	static ChipsetType s_loggedLegacy=DC_MAX, s_loggedShaderModel=DC_MAX;
	if (s_loggedLegacy==legacy && s_loggedShaderModel==shaderModel)
		return;
	s_loggedLegacy=legacy;
	s_loggedShaderModel=shaderModel;

	const DX8Caps* caps=DX8Wrapper::Get_Current_Caps();
	fprintf(stderr,"[CHIPSET_TIER] legacy=%s(%d) shadermodel=%s(%d)%s - PS v%d.%d / %d stages per pass\n",
		Chipset_Tier_Name(legacy),(int)legacy,
		Chipset_Tier_Name(shaderModel),(int)shaderModel,
		legacy!=shaderModel?" (LADGERS DISAGREE)":"",
		caps?caps->Get_Pixel_Shader_Major_Version():0,
		caps?caps->Get_Pixel_Shader_Minor_Version():0,
		caps?caps->Get_Max_Simultaneous_Textures():0);
	fflush(stderr);
}

// GeneralsX @feature PS3.0 batch2 12/10/2026 ---------------------------------------------------------
// Which shader model a render path may emit. The legacy ladder in getChipset() identifies 2001-era cards
// by vendor/device id and also feeds the LOD preset table, so it must not be repurposed: on this
// machine's NVIDIA GPU every device id >= 0x250 is classified DC_GEFORCE4 (ordinal 11), which sits
// below DC_GENERIC_PIXEL_SHADER_3_0 (16) while the device itself reports PS v3.0. Gating SM3 paths on
// getChipset() would therefore leave them permanently dead on exactly the hardware they were written
// for - and raising the legacy ladder to 16 would move the auto-selected detail level as a side effect.
ChipsetType W3DShaderManager::getShaderModelTier()
{
	// A forced chipset (the m_chipSetType override -noshaders writes) wins outright: it is the user
	// turning shader features off, and a caps-derived answer would ignore that.
	if (TheGlobalData && TheGlobalData->m_chipSetType != DC_UNKNOWN)
		return (ChipsetType)TheGlobalData->m_chipSetType;

	return applyShaderModelCap(ShaderModelTierFromCaps());
}

ChipsetType W3DShaderManager::getChipset()
{
	//check if globaldata has an override for current chipset
	if (TheGlobalData && TheGlobalData->m_chipSetType != DC_UNKNOWN)
	{
		ChipsetType override=(ChipsetType)TheGlobalData->m_chipSetType;
		Report_Shader_Tiers(override,getShaderModelTier());
		return override;
	}

	ChipsetType inForce=applyShaderModelCap(detectChipsetTier());
	Report_Shader_Tiers(inForce,getShaderModelTier());
	return inForce;
}

//=============================================================================
// WaterRenderObjClass::LoadAndCreateShader
//=============================================================================
/** Loads and creates a D3D pixel or vertex shader.*/
//=============================================================================
// GeneralsX @build d3d9 port 11/10/2026 - pDeclaration is a D3DVERTEXELEMENT9 array and pHandle
// receives an IDirect3DVertexShader9*/IDirect3DPixelShader9* (passed as void**, matching the
// CreateVertexShader/CreatePixelShader out-parameter types). DX8 DWORD handles are gone.
HRESULT W3DShaderManager::LoadAndCreateD3DShader(const char* strFilePath, const D3DVERTEXELEMENT9* pDeclaration, DWORD Usage, Bool ShaderType, void** pHandle)
{
	if (getChipset() < DC_GENERIC_PIXEL_SHADER_1_1)
		return E_FAIL;	//don't allow loading any shaders if hardware can't handle it.

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @bugfix Qoder 19/10/2026 BUG-03: a shipped .pso/.vso bytecode asset has no executable
	// representation under no-d3d9. The Vulkan program registry matches translated twins by HLSL
	// source; pre-compiled DX8 bytecode names no source, so a live handle here would be untagged and
	// the draw path would refuse every batch that binds it - the ground-vanishing pattern again.
	// Failing the load drives each caller's existing "asset not found" branch, which keeps the
	// fixed-function leg (or the HLSL twin, when the family has one) exactly like a bare ZH install.
	if (VK_NOD3D_Enabled())
	{
		fprintf(stderr, "[SHADER] bytecode asset skipped under no-d3d9: '%s'\n",
			strFilePath ? strFilePath : "(null)");
		fflush(stderr);
		return E_FAIL;
	}
#endif

	try
	{
		File *file = nullptr;
		HRESULT hr;

		// GeneralsX @bugfix BenderAI 18/02/2026 Add comprehensive asset loading diagnostics
		const char* shaderTypeStr = ShaderType ? "VERTEX" : "PIXEL";
		fprintf(stderr, "[ASSET_LOAD] Attempting to load shader (%s): file='%s'\n", 
			shaderTypeStr, strFilePath ? strFilePath : "(null)");

		// GeneralsX @bugfix BenderAI 18/02/2026 Normalize path for cross-platform compatibility (fighter19 pattern)
		// On Linux, backslashes must be converted to forward slashes for VFS path lookup
		// GeneralsX @bugfix Qoder 18/10/2026 The VFS key must stay RELATIVE. On Windows
		// LocalFileSystem::normalizePath is GetFullPathNameA, so the lookup became
		// "D:\...\shaders\terrain.pso" and ArchiveFileSystem could never match it - every
		// .pso/.vso asset in every BIG was reported missing. POSIX normalizePath already keeps
		// the path relative and only normalizes separators, so the Windows leg now skips it.
		AsciiString normalizedPath = AsciiString(strFilePath);
#ifndef _WIN32
		normalizedPath = FileSystem::normalizePath(AsciiString(strFilePath));
#endif
		if (normalizedPath.isEmpty())
		{
			// Normalization failed - path might be invalid
			fprintf(stderr, "[ASSET_FAIL] Could not normalize path: original='%s'\n", 
				strFilePath ? strFilePath : "(null)");
			char debugMsg[512];
			snprintf(debugMsg, sizeof(debugMsg), "ERROR: Could not normalize shader file path: '%s'\n", 
				strFilePath ? strFilePath : "(null)");
			OutputDebugString(debugMsg);
			return E_FAIL;
		}

		fprintf(stderr, "[ASSET_PATH] Original: '%s' -> Normalized: '%s'\n", 
			strFilePath ? strFilePath : "(null)", normalizedPath.str());

		file = TheFileSystem->openFile(normalizedPath.str(), File::READ | File::BINARY);
		if (file == nullptr)
		{
			// GeneralsX @bugfix BenderAI 13/02/2026 Log actual filename that's missing
			fprintf(stderr, "[ASSET_FAIL] File not found in VFS: normalized_path='%s'\n", 
				normalizedPath.str());
			char debugMsg[512];
			snprintf(debugMsg, sizeof(debugMsg), "ERROR: Could not find shader file: '%s' (normalized: '%s')\n", 
				strFilePath ? strFilePath : "(null)", normalizedPath.str());
			OutputDebugString(debugMsg);
			return E_FAIL;
		}

		FileInfo fileInfo;
		TheFileSystem->getFileInfo(normalizedPath, &fileInfo);
		DWORD dwFileSize = fileInfo.sizeLow;

		fprintf(stderr, "[ASSET_LOAD] File found: path='%s' size=%u bytes\n", 
			normalizedPath.str(), dwFileSize);

		// GeneralsX @bugfix BenderAI 13/02/2026 Use new[] instead of HeapAlloc (fighter19 pattern)
		const DWORD* pShader = new DWORD[dwFileSize / sizeof(DWORD)]();
		if (!pShader)
		{
			fprintf(stderr, "[ASSET_FAIL] Out of memory allocating %u bytes for shader data\n", dwFileSize);
			OutputDebugString( "Failed to allocate memory to load shader\n " );
			file->close();
			return E_FAIL;
		}

		file->read((void *)pShader, dwFileSize);
		file->close();
		file = nullptr;

		if (ShaderType) // SHADERTYPE_VERTEX
		{
			// GeneralsX @build d3d9 port 11/10/2026 - D3D9 CreateVertexShader takes only (pFunction,
			// ppShader); the legacy DX8 pDeclaration/Usage inputs are no longer consumed here.
			hr = DX8Wrapper::_Get_D3D_Device8()->CreateVertexShader(pShader, reinterpret_cast<IDirect3DVertexShader9**>(pHandle));
		}
		else // SHADERTYPE_PIXEL
		{
			hr = DX8Wrapper::_Get_D3D_Device8()->CreatePixelShader(pShader, reinterpret_cast<IDirect3DPixelShader9**>(pHandle));
		}

		// GeneralsX @bugfix Qoder 19/10/2026 BUG-03: a device that answers S_OK with a null handle did
		// not create the shader. compileShaderAtBestProfile grew the same guard on 18/10 for the same
		// reason - without it the no-d3d9 runs "succeeded" into handles that set() then bound as
		// nothing, and the whole ground vanished behind a successful load log.
		if (SUCCEEDED(hr) && pHandle != nullptr && *pHandle == nullptr)
			hr = D3DERR_INVALIDCALL;

		// GeneralsX @bugfix BenderAI 13/02/2026 Use delete[] instead of HeapFree (fighter19 pattern)
		delete[] pShader;

		if (FAILED(hr))
		{
			fprintf(stderr, "[ASSET_FAIL] Failed to create %s shader: original='%s' hr=0x%08x\n", 
				shaderTypeStr, strFilePath ? strFilePath : "(null)", hr);
			OutputDebugString( "Failed to create shader\n ");
			return E_FAIL;
		}

		fprintf(stderr, "[ASSET_OK] Successfully created %s shader: original='%s' handle=%p\n",
			shaderTypeStr, strFilePath ? strFilePath : "(null)", *pHandle);
	}
	catch(...)
	{
		fprintf(stderr, "[ASSET_FAIL] Exception loading shader: file='%s'\n", 
			strFilePath ? strFilePath : "(null)");
		OutputDebugString( "Error opening file \n" );
		return E_FAIL;
	}

	return S_OK;
}

//For the MP test, we're enforcing high min-spec requirements that need to be verified.
#define MIN_INTEL_CPU_FREQ	1300
#define MIN_AMD_CPU_FREQ	1100
#define MIN_ACCEPTED_FREQUENCY	1300
#define MIN_ACCEPTED_MEMORY	(1024*1024*256)	//256 MB
#define MIN_ACCEPTED_TEXTURE_MEMORY	(1024*1024*30)	//30 MB

/**Hack to give gameengine access to this function*/
Bool testMinimumRequirements(ChipsetType *videoChipType, CpuType *cpuType, Int *cpuFreq, MemValueType *numRAM, Real *intBenchIndex, Real *floatBenchIndex, Real *memBenchIndex)
{
	return W3DShaderManager::testMinimumRequirements(videoChipType,cpuType,cpuFreq,numRAM,intBenchIndex,floatBenchIndex,memBenchIndex);
}

Bool W3DShaderManager::testMinimumRequirements(ChipsetType *videoChipType, CpuType *cpuType, Int *cpuFreq, MemValueType *numRAM, Real *intBenchIndex, Real *floatBenchIndex, Real *memBenchIndex)
{
	if (videoChipType)
		*videoChipType = getChipset();

	if (cpuType)
	{
		*cpuType = XX;	//unknown

		//Check if it's an Athlon
		if (CPUDetectClass::Get_Processor_Manufacturer() == CPUDetectClass::MANUFACTURER_AMD &&
				CPUDetectClass::Get_AMD_Processor() >= CPUDetectClass::AMD_PROCESSOR_ATHLON_025)
				*cpuType = K7;

		//Check if it's a P3
		if (CPUDetectClass::Get_Processor_Manufacturer() == CPUDetectClass::MANUFACTURER_INTEL &&
				CPUDetectClass::Get_Intel_Processor() >= CPUDetectClass::INTEL_PROCESSOR_PENTIUM_III_MODEL_7)
				*cpuType = P3;
		//Check if it's a P4
		if (CPUDetectClass::Get_Processor_Manufacturer() == CPUDetectClass::MANUFACTURER_INTEL &&
				CPUDetectClass::Get_Intel_Processor() >= CPUDetectClass::INTEL_PROCESSOR_PENTIUM4)
				*cpuType = P4;
	}

	if (cpuFreq)
		*cpuFreq=CPUDetectClass::Get_Processor_Speed();

	if (numRAM)
		*numRAM=CPUDetectClass::Get_Total_Physical_Memory();

	if (intBenchIndex && floatBenchIndex && memBenchIndex)
	{
		// TheSuperHackers @tweak Aliendroid1 19/06/2025 Legacy benchmarking code was removed.
		// Since modern hardware always meets the minimum requirements, we preset the benchmark "results" to a high value.
		*intBenchIndex = 10.0f;
		*floatBenchIndex = 10.0f;
		*memBenchIndex = 10.0f;
	}

	return TRUE;
}

/**Try to guess how well the video card will handle the game assuming very fast CPU*/
StaticGameLODLevel W3DShaderManager::getGPUPerformanceIndex()
{
	ChipsetType	chipType;
	StaticGameLODLevel detailSetting=STATIC_GAME_LOD_LOW;	//assume lowest settings for now.

	if ((chipType=getChipset()) != DC_UNKNOWN)
	{
		//a known video card so we can make some assumptions
		if (chipType >=	DC_GEFORCE2)
			detailSetting=STATIC_GAME_LOD_LOW;	//these cards need multiple terrain passes.
		if (chipType >= DC_GENERIC_PIXEL_SHADER_1_1)	//these cards can do terrain in single pass.
			detailSetting=STATIC_GAME_LOD_VERY_HIGH;
	}

	return detailSetting;
}

/**We need a hardware independent method to compare different CPU's.  For lack of anything better, we'll
use time to calculate PIE using a slow random number algorithm.*/

/**Used to test function call overhead*/
void add(float *sum,float *addend)
{
	*sum = *sum + *addend;
}

/**Returns seconds needed to run the test*/
Real W3DShaderManager::GetCPUBenchTime()
{
	float ztot, yran, ymult, ymod, x, y, z, pi, prod;
    long int low, ixran, itot, j, iprod;

  	// GeneralsX @bugfix BenderAI 13/02/2026 Int64 and cross-platform timing (fighter19 pattern)
  	Int64 endTime64,freq64,startTime64;
#ifdef _WIN32
	QueryPerformanceFrequency((LARGE_INTEGER *)&freq64);
	QueryPerformanceCounter((LARGE_INTEGER *)&startTime64);
#else
	freq64 = 1000;
	startTime64 = 0;
#endif

    ztot = 0.0;
    low = 1;
    ixran = 1907;
    yran = 5813.0;
    ymult = 1307.0;
    ymod = 5471.0;
    itot = 560000;	//total iterations. This value ends up running at ~30 fps on our P4-2.2Ghz.

    for(j=1; j<=itot; j++)
    {
		iprod = 27611 * ixran;
		ixran = iprod - 74383*(long int)(iprod/74383);
		x = (float)ixran / 74383.0;
		prod = ymult * yran;
		yran = (prod - ymod*(long int)(prod/ymod));
		y = yran / ymod;
		z = x*x + y*y;
		add(&ztot,&z);
		if ( z <= 1.0 )
		{
		  low = low + 1;
		}
	}
	pi = 4.0 * (float)low/(float)itot;

// GeneralsX @bugfix BenderAI 13/02/2026 Cross-platform timing end (fighter19 pattern)
#ifdef _WIN32
	QueryPerformanceCounter((LARGE_INTEGER *)&endTime64);
#else
	endTime64 = 1000;
#endif
	return ((double)(endTime64-startTime64)/(double)(freq64));
}


// W3DShaderManager::setShroudTex =======================================================
/** Puts the shroud texture into a texture stage.
 */
//=============================================================================
Int W3DShaderManager::setShroudTex(Int stage)
{
	//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
	//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
	W3DShroud *shroud;
	if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr && shroud->isTextureReady())
	{
		DX8Wrapper::Set_Texture(stage, shroud->getShroudTexture());

		DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG2, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAARG2, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG2 );

		Matrix4x4 curView;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

		D3DXMATRIX inv;
		float det;
		D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

		D3DXMATRIX scale,offset;

		//We need to make all world coordinates be relative to the heightmap data origin since that
		//is where the shroud begins.

		float xoffset = 0;
		float yoffset = 0;
		Real width=shroud->getCellWidth();
		Real height=shroud->getCellHeight();

		if (TheTerrainRenderObject->getMap())
		{
			//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
			xoffset = -(float)shroud->getDrawOriginX() + width;
			yoffset = -(float)shroud->getDrawOriginY() + height;
		}

		D3DXMatrixTranslation(&offset, xoffset, yoffset,0);

		width = 1.0f/(width*shroud->getTextureWidth());
		height = 1.0f/(height*shroud->getTextureHeight());
		D3DXMatrixScaling(&scale, width, height, 1);
		*((D3DXMATRIX*)&curView) = (inv * offset) * scale;
		DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE )(D3DTS_TEXTURE0+stage), curView);
		return TRUE;
	}
	return FALSE;
}



Int FlatTerrainShader2Stage::init()
{
	//no special device validation needed - anything in our min spec should handle this.

	W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE]=&flatTerrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE]=1;
	W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1]=&flatTerrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1]=2;
	W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2]=&flatTerrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2]=2;
	W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12]=&flatTerrainShader2Stage;
	W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12]=2;

	return TRUE;
}

void FlatTerrainShader2Stage::reset()
{
	ShaderClass::Invalidate();

	//Free references to textures
	DX8Wrapper::Set_DX8_Texture(0, nullptr);
	DX8Wrapper::Set_DX8_Texture(1, nullptr);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|0);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|1);
}


Int FlatTerrainShader2Stage::set(Int pass)
{
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	DX8Wrapper::Apply_Render_State_Changes();

	// GeneralsX @feature PS3.0-P3-5 14/10/2026 Retail INI-bool override replaced by the player's
	// filter tier (the bools are a floor now); see TextureFilterClass::Apply_Player_Filter.
	{
		const bool bilinearHint=TheGlobalData ? TheGlobalData->m_bilinearTerrainTex!=FALSE : false;
		const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
		TextureFilterClass::Apply_Player_Filter(0,bilinearHint,trilinearHint);
		TextureFilterClass::Apply_Player_Filter(1,bilinearHint,trilinearHint);
	}

	switch (pass)
	{
		case 0:

			DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

			// Modulate the diffuse color with the texture as lighting comes from diffuse.
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
			if (W3DShaderManager::getShaderTexture(0)) {
				DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(0));
				DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_CURRENT );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

				DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
				DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

				//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
				//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
				W3DShroud *shroud;
				if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr)
				{
					Matrix4x4 curView;
					DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

					D3DXMATRIX inv;
					float det;
					D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

					D3DXMATRIX scale,offset;

					//We need to make all world coordinates be relative to the heightmap data origin since that
					//is where the shroud begins.

					float xoffset = 0;
					float yoffset = 0;
					Real width=shroud->getCellWidth();
					Real height=shroud->getCellHeight();

					if (TheTerrainRenderObject->getMap())
					{
						//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
						xoffset = -(float)shroud->getDrawOriginX() + width;
						yoffset = -(float)shroud->getDrawOriginY() + height;
					}

					D3DXMatrixTranslation(&offset, xoffset, yoffset,0);

					width = 1.0f/(width*shroud->getTextureWidth());
					height = 1.0f/(height*shroud->getTextureHeight());
					D3DXMatrixScaling(&scale, width, height, 1);
					*((D3DXMATRIX*)&curView) = (inv * offset) * scale;
					DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE )(D3DTS_TEXTURE0), curView);
				}
			}	else {
				DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG2 );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
			}
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

			DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
			DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

			// Modulate the diffuse color with the texture as lighting comes from diffuse.
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 0 );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|0);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);
			break;
		case 1:
			// Noise/cloud pass
			Matrix4x4 curView;
			DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

			//these states apply to all noise/cloud combination passes
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

			DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
			// Two output coordinates are used.
			DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
			DX8Wrapper::Set_DX8_Sampler_State(0,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
			DX8Wrapper::Set_DX8_Sampler_State(0,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

			//blend into frame buffer
			DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR);
			DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);

			D3DXMATRIX inv;
			float det;
			D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

			if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12)
			{
				//setup cloud pass

				terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
				DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, curView);
				//clouds always need bilinear filtering
				DX8Wrapper::Set_DX8_Sampler_State(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
				DX8Wrapper::Set_DX8_Sampler_State(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(2));

				//setup noise pass

				terrainShader2Stage.updateNoise2((D3DXMATRIX*)&curView,&inv);
				DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, curView);
				//noise always needs point/linear filtering.  Why point!?
				DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
				DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
				// Two output coordinates are used.
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

				DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
				DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
				DX8Wrapper::Set_Texture(1, W3DShaderManager::getShaderTexture(3));
			}
			else
			{
				//only 1 noise or cloud texture
				// Now setup the texture pipeline.
				if (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1)
				{
					//setup cloud pass
					DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(2));
					terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
					DX8Wrapper::Set_DX8_Sampler_State(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
					DX8Wrapper::Set_DX8_Sampler_State(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				}
				else
				{
					//setup noise pass
					DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(3));
					terrainShader2Stage.updateNoise2((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
					DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
					DX8Wrapper::Set_DX8_Sampler_State(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
				}

				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
				DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
				DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, curView);
			}
			break;
	}

	return TRUE;
}






Int FlatTerrainShaderPixelShader::shutdown()
{
	if (m_dwBasePixelShader)
		m_dwBasePixelShader->Release();

	if (m_dwBase0PixelShader)
		m_dwBase0PixelShader->Release();

	if (m_dwBaseNoise1PixelShader)
		m_dwBaseNoise1PixelShader->Release();

	if (m_dwBaseNoise2PixelShader)
		m_dwBaseNoise2PixelShader->Release();

	m_dwBasePixelShader = nullptr;
	m_dwBase0PixelShader = nullptr;
	m_dwBaseNoise1PixelShader = nullptr;
	m_dwBaseNoise2PixelShader = nullptr;

	return TRUE;
}

Int FlatTerrainShaderPixelShader::init()
{
	Int res;

#ifdef DISABLE_PIXEL_SHADERS
	return false;
#endif

	//this shader will also use the 2Stage shader for some of the passes so initialize it too.
	if ((res=W3DShaderManager::getChipset()) >= DC_GENERIC_PIXEL_SHADER_1_1)
	{
		if (res >= DC_GENERIC_PIXEL_SHADER_1_1)
		{
			//this shader needs some assets that need to be loaded
			//shader decleration
			D3DVERTEXELEMENT9 Declaration[]=
			{
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 0), // Position
				D3DDECL_USAGE(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 12), // Diffuse
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, 0, 16), //  Texture Coordinates
				D3DDECL_USAGE(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, 1, 24), //  Texture Coordinates
				D3DDECL_END()
			};

			//base version which doesn't apply any noise textures.
			HRESULT hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\fterrain.pso", &Declaration[0], 0, false, (void**)&m_dwBasePixelShader);
			if (FAILED(hr))
				return FALSE;

			//base version which doesn't apply any shroud textures.
			hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\fterrain0.pso", &Declaration[0], 0, false, (void**)&m_dwBase0PixelShader);
			if (FAILED(hr))
				return FALSE;

			//version which blends 1 noise texture.
			hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\fterrainnoise.pso", &Declaration[0], 0, false, (void**)&m_dwBaseNoise1PixelShader);
			if (FAILED(hr))
				return FALSE;

			//version which blends 2 noise textures.
			hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\fterrainnoise2.pso", &Declaration[0], 0, false, (void**)&m_dwBaseNoise2PixelShader);
			if (FAILED(hr))
				return FALSE;

			W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE]=&flatTerrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1]=&flatTerrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2]=&flatTerrainShaderPixelShader;
			W3DShaders[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12]=&flatTerrainShaderPixelShader;
			W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE]=1;
			W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1]=1;
			W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2]=1;
			W3DShadersPassCount[W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12]=1;
			return TRUE;
		}
	}
	return FALSE;
}

Int FlatTerrainShaderPixelShader::set(Int pass)
{
	// GeneralsX @feature Qoder 18/10/2026 read-only terrain path census: flat/untextured leg.
	{
		static unsigned sFlatTerrainPathSets = 0u;
		if ((sFlatTerrainPathSets++ & 4095u) == 0u)
		{
			fprintf(stderr, "[TERRAIN] path=flat pass=%d\n", pass);
			fflush(stderr);
		}
	}

	//setup base pass
	Int curStage = 1;
	// setup terrain [3/31/2003]

	DX8Wrapper::Set_DX8_Sampler_State(0,  D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Sampler_State(0,  D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_Texture(0, W3DShaderManager::getShaderTexture(2));
	DX8Wrapper::Set_Texture(1, W3DShaderManager::getShaderTexture(2));
	//force WW3D2 system to set it's states so it won't later overwrite our custom settings.
	DX8Wrapper::Apply_Render_State_Changes();




	DX8Wrapper::Set_DX8_Sampler_State( curStage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Sampler_State( curStage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	//tell pixel shader which UV set to use for each stage
	DX8Wrapper::Set_DX8_Texture_Stage_State( curStage, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Texture_Stage_State(curStage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

	// GeneralsX @feature PS3.0-P3-5 14/10/2026 Retail INI-bool override replaced by the player's
	// filter tier (the bools are a floor now); see TextureFilterClass::Apply_Player_Filter.
	{
		const bool bilinearHint=TheGlobalData ? TheGlobalData->m_bilinearTerrainTex!=FALSE : false;
		const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
		TextureFilterClass::Apply_Player_Filter(curStage,bilinearHint,trilinearHint);
	}

	curStage = 0;

	W3DShroud *shroud = TheTerrainRenderObject->getShroud();
	if (shroud) {

		DX8Wrapper::Set_DX8_Texture_Stage_State(curStage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		DX8Wrapper::Set_DX8_Texture_Stage_State(curStage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

		//We need to scale so shroud texel stretches over one full terrain cell.  Each texel
		//is 1/128 the size of full texture. (assuming 128x128 vid-mem texture).
		{
			Matrix4x4 curView;
			DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

			D3DXMATRIX inv;
			float det;
			D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

			D3DXMATRIX scale,offset;

			//We need to make all world coordinates be relative to the heightmap data origin since that
			//is where the shroud begins.

			float xoffset = 0;
			float yoffset = 0;
			Real width=shroud->getCellWidth();
			Real height=shroud->getCellHeight();

			if (TheTerrainRenderObject->getMap())
			{
				//subtract origin position from all coordinates.  Origin is shifted by 1 cell width/height to allow for unused border texels.
				xoffset = -(float)shroud->getDrawOriginX() + width;
				yoffset = -(float)shroud->getDrawOriginY() + height;
			}

			D3DXMatrixTranslation(&offset, xoffset, yoffset,0);

			width = 1.0f/(width*shroud->getTextureWidth());
			height = 1.0f/(height*shroud->getTextureHeight());
			D3DXMatrixScaling(&scale, width, height, 1);
			*((D3DXMATRIX*)&curView) = (inv * offset) * scale;
			DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE )(D3DTS_TEXTURE0+curStage), curView);
		}
		DX8Wrapper::Set_DX8_Sampler_State( curStage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( curStage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( curStage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State( curStage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_Texture(curStage, shroud->getShroudTexture());
		curStage++;
		if (curStage==1) curStage++;
	}

	Bool doNoise1 = (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE1 ||
						W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12);
	if (doNoise1) {	 // Cloud pass.
		Matrix4x4 curView;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

		D3DXMATRIX inv;
		float det;
		D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

		DX8Wrapper::Set_DX8_Texture_Stage_State(curStage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		// Two output coordinates are used.
		DX8Wrapper::Set_DX8_Texture_Stage_State(curStage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

		DX8Wrapper::Set_DX8_Sampler_State(curStage,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		DX8Wrapper::Set_DX8_Sampler_State(curStage,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		DX8Wrapper::Set_Texture(curStage, W3DShaderManager::getShaderTexture(2));
		terrainShader2Stage.updateNoise1((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
		DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE )(D3DTS_TEXTURE0+curStage), curView);
		DX8Wrapper::Set_DX8_Sampler_State(curStage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State(curStage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

		curStage++;
		if (curStage==1) curStage++;
	}

	Bool doNoise2 = (W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE2 ||
						W3DShaderManager::getCurrentShader() == W3DShaderManager::ST_FLAT_TERRAIN_BASE_NOISE12);
	if (doNoise2)
	{
		Matrix4x4 curView;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

		D3DXMATRIX inv;
		float det;
		D3DXMatrixInverse(&inv, &det, (D3DXMATRIX*)&curView);

		DX8Wrapper::Set_DX8_Texture_Stage_State(curStage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
		// Two output coordinates are used.
		DX8Wrapper::Set_DX8_Texture_Stage_State(curStage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

		DX8Wrapper::Set_DX8_Sampler_State(curStage,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		DX8Wrapper::Set_DX8_Sampler_State(curStage,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		DX8Wrapper::Set_Texture(curStage, W3DShaderManager::getShaderTexture(3));
		terrainShader2Stage.updateNoise2((D3DXMATRIX*)&curView,&inv);	//update curView with texture matrix
		DX8Wrapper::_Set_DX8_Transform((D3DTRANSFORMSTATETYPE )(D3DTS_TEXTURE0+curStage), curView);
		DX8Wrapper::Set_DX8_Sampler_State(curStage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State(curStage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

		curStage++;
		if (curStage==1) curStage++;
	}
	if (curStage<2) {
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_dwBase0PixelShader));
	}	else if (curStage==2) {
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_dwBasePixelShader));
	}	else if (curStage==3) {
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_dwBaseNoise1PixelShader));
	}else if (curStage==4) {
		DX8Wrapper::Set_Pixel_Shader(reinterpret_cast<UINT_PTR>(m_dwBaseNoise2PixelShader));
	}
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, false);
	DX8Wrapper::Apply_Render_State_Changes();
	// GeneralsX @bugfix Qoder 18/10/2026 Facade bind: the Vulkan draw layer only sees Textures[].
	DX8Wrapper::Set_Texture(curStage, W3DShaderManager::getShaderTexture(3));
	return TRUE;
}

void FlatTerrainShaderPixelShader::reset()
{
	DX8Wrapper::Set_DX8_Texture(2,nullptr);	//release reference to any texture
	DX8Wrapper::Set_DX8_Texture(3,nullptr);	//release reference to any texture

	DX8Wrapper::Set_Pixel_Shader(0);	//turn off pixel shader

	DX8Wrapper::Set_DX8_Texture(0, nullptr);
	DX8Wrapper::Set_DX8_Texture(1, nullptr);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|0);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|1);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|2);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 3, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|3);


	DX8Wrapper::Invalidate_Cached_Render_States();
}





