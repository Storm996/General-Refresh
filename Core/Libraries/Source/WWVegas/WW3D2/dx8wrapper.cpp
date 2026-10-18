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

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8wrapper.cpp                         $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 170                                                         $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 * 06/27/02 KM Render to shadow buffer texture support														*
 * 06/27/02 KM Shader system updates																				*
 * 08/05/02 KM Texture class redesign
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 *   DX8Wrapper::_Update_Texture -- Copies a texture from system memory to video memory        *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//#define CREATE_DX8_MULTI_THREADED
//#define CREATE_DX8_FPU_PRESERVE
#define WW3D_DEVTYPE D3DDEVTYPE_HAL

#if !defined(WINVER) || WINVER < 0x0500
#undef WINVER
#define WINVER 0x0500 // Required to access GetMonitorInfo in VC6.
#endif

#include "dx8wrapper.h"
// GeneralsX @build PS3.0-P2-8 14/10/2026 needed for getenv/atoi in the swap-interval env override.
#include <stdlib.h>
// GeneralsX @build BenderAI 10/02/2026 - Need LoadLibrary/GetProcAddress/FreeLibrary for dynamic loading
#ifndef _WIN32
#include "module_compat.h"
#endif
// GeneralsX @build felipebraz 16/02/2026 - Need dlerror() for dlopen() error reporting on Linux
#ifndef _WIN32
#include <dlfcn.h>
#endif
// GeneralsX @build BenderAI 10/02/2026 - Embedded browser Windows-only (requires COM LPDISPATCH)
#ifdef _WIN32
#include "dx8webbrowser.h"
#endif
#include "dx8fvf.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"
#include "dx8renderer.h"
#include "dx8normalmap.h"   // GeneralsX @debug T3-0 draw-time shader readback probe
#include "ww3d.h"
#include "camera.h"
#include "WWLib/wwstring.h"
#include "WWMath/matrix4.h"
#include "vertmaterial.h"
#include "rddesc.h"
#include "lightenvironment.h"
#include "statistics.h"
#include "WWLib/registry.h"
#include "boxrobj.h"
#include "pointgr.h"
#include "render2d.h"
#include "sortingrenderer.h"
#include "shattersystem.h"
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 1: optional Vulkan device core (opt-in tree only).
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
#include "Backend/VulkanCore.h"
#include "Backend/VulkanGeo.h" // wedge 3.4: render-target/depth mirror alloc hooks
#include "Backend/VulkanDraw.h" // wedge 5: frame boundaries, render targets and the draw path
#endif
#include "light.h"
#include "assetmgr.h"
#include "textureloader.h"
#include "missingtexture.h"
#include "WWLib/thread.h"
#include <d3dx9core.h>
#include "WWMath/pot.h"
#include "WWDebug/wwprofile.h"
#include "WWLib/ffactory.h"
#include "dx8caps.h"
#include "formconv.h"
#include "dx8texman.h"
#include "WWLib/bound.h"
#include "DbgHelpGuard.h"

#include "shdlib.h"

const int DEFAULT_RESOLUTION_WIDTH = 640;
const int DEFAULT_RESOLUTION_HEIGHT = 480;
const int DEFAULT_BIT_DEPTH = 32;
const int DEFAULT_TEXTURE_BIT_DEPTH = 16;
const D3DMULTISAMPLE_TYPE DEFAULT_MSAA = D3DMULTISAMPLE_NONE;

static D3DPRESENT_PARAMETERS _PresentParameters;

// GeneralsX @feature PS3.0-P3-25a 14/10/2026 present interval (0..3) the device layer injects
// from Options.ini VSync before the device is created; -1 = nothing injected (retail default).
static int s_UserPresentInterval = -1;

// GeneralsX @bugfix Copilot 24/08/2026 Fall back through supported MSAA sample counts instead of disabling AA immediately.
static D3DMULTISAMPLE_TYPE Normalize_MSAA_Mode(D3DMULTISAMPLE_TYPE mode)
{
	if (mode >= D3DMULTISAMPLE_8_SAMPLES)
		return D3DMULTISAMPLE_8_SAMPLES;
	if (mode >= D3DMULTISAMPLE_4_SAMPLES)
		return D3DMULTISAMPLE_4_SAMPLES;
	if (mode >= D3DMULTISAMPLE_2_SAMPLES)
		return D3DMULTISAMPLE_2_SAMPLES;

	return D3DMULTISAMPLE_NONE;
}

static D3DMULTISAMPLE_TYPE Get_Lower_MSAA_Mode(D3DMULTISAMPLE_TYPE mode)
{
	switch (mode)
	{
	case D3DMULTISAMPLE_8_SAMPLES:
		return D3DMULTISAMPLE_4_SAMPLES;
	case D3DMULTISAMPLE_4_SAMPLES:
		return D3DMULTISAMPLE_2_SAMPLES;
	case D3DMULTISAMPLE_2_SAMPLES:
	default:
		return D3DMULTISAMPLE_NONE;
	}
}

static bool Is_MSAA_Mode_Supported(
	IDirect3D9 *direct3D,
	unsigned adapter,
	D3DFORMAT backBufferFormat,
	D3DFORMAT depthStencilFormat,
	BOOL windowed,
	D3DMULTISAMPLE_TYPE mode)
{
	if (mode == D3DMULTISAMPLE_NONE)
		return true;

	// GeneralsX @build d3d9 port 11/10/2026 - D3D9 CheckDeviceMultiSampleType takes an extra out-param for quality levels
	DWORD qualityLevels = 0;
	return SUCCEEDED(direct3D->CheckDeviceMultiSampleType(
		adapter,
		D3DDEVTYPE_HAL,
		backBufferFormat,
		windowed,
		mode,
		&qualityLevels)) &&
		SUCCEEDED(direct3D->CheckDeviceMultiSampleType(
			adapter,
			D3DDEVTYPE_HAL,
			depthStencilFormat,
			windowed,
			mode,
			&qualityLevels));
}

// GeneralsX @feature PS3.0-P3-1 14/10/2026 I1: the quality-levels out-param of
// CheckDeviceMultiSampleType was accepted above but never used, so MultiSampleQuality stayed at 0.
// This helper reports the tier count actually available for a mode (the smaller of the backbuffer
// and depth-format answers) so Set_Render_Device can clamp a user-requested tier to it.
static unsigned Query_MSAA_Quality_Levels(
	IDirect3D9 *direct3D,
	unsigned adapter,
	D3DFORMAT backBufferFormat,
	D3DFORMAT depthStencilFormat,
	BOOL windowed,
	D3DMULTISAMPLE_TYPE mode)
{
	if (mode == D3DMULTISAMPLE_NONE)
		return 0;

	DWORD bbLevels = 0;
	DWORD dsLevels = 0;
	if (FAILED(direct3D->CheckDeviceMultiSampleType(adapter, D3DDEVTYPE_HAL, backBufferFormat, windowed, mode, &bbLevels)))
		return 0;
	if (FAILED(direct3D->CheckDeviceMultiSampleType(adapter, D3DDEVTYPE_HAL, depthStencilFormat, windowed, mode, &dsLevels)))
		return 0;

	return (bbLevels < dsLevels) ? (unsigned)bbLevels : (unsigned)dsLevels;
}

// --- Pillarbox: render game to offscreen RT, blit centered onto backbuffer ---
// GeneralsX @feature xxorza 15/04/2026 Unified pillarbox for fullscreen and windowed
DX8Wrapper::DisplaySizeFunc DX8Wrapper::s_getNativeDisplaySize = nullptr;
DX8Wrapper::DisplaySizeFunc DX8Wrapper::s_getWindowSize = nullptr;
bool DX8Wrapper::s_pillarboxEnabled = false;
bool DX8Wrapper::s_pillarboxActive = false;
int DX8Wrapper::s_bbW = 0;
int DX8Wrapper::s_bbH = 0;
int DX8Wrapper::s_dstX = 0;
int DX8Wrapper::s_dstY = 0;
int DX8Wrapper::s_dstW = 0;
int DX8Wrapper::s_dstH = 0;
float DX8Wrapper::s_pixelDensity = 1.0f;
IDirect3DTexture9* DX8Wrapper::s_offscreenTex = nullptr;
IDirect3DSurface9* DX8Wrapper::s_offscreenSurf = nullptr;
IDirect3DSurface9* DX8Wrapper::s_depthSurf = nullptr;
IDirect3DSurface9* DX8Wrapper::s_savedBackbuffer = nullptr;
IDirect3DSurface9* DX8Wrapper::s_savedDepth = nullptr;

void DX8Wrapper::Set_Display_Size_Provider(DisplaySizeFunc nativeSize, DisplaySizeFunc windowSize)
{
	s_getNativeDisplaySize = nativeSize;
	s_getWindowSize = windowSize;
}

bool DX8Wrapper::GetNativeDisplaySize(int& outW, int& outH, float& outDensity)
{
	return s_getNativeDisplaySize && s_getNativeDisplaySize(outW, outH, outDensity);
}

bool DX8Wrapper::GetWindowSize(int& outW, int& outH, float& outDensity)
{
	return s_getWindowSize && s_getWindowSize(outW, outH, outDensity);
}

void DX8Wrapper::Pillarbox_Cleanup()
{
	if (s_savedBackbuffer) { s_savedBackbuffer->Release(); s_savedBackbuffer = nullptr; }
	if (s_savedDepth) { s_savedDepth->Release(); s_savedDepth = nullptr; }
	if (s_depthSurf) { s_depthSurf->Release(); s_depthSurf = nullptr; }
	if (s_offscreenSurf) { s_offscreenSurf->Release(); s_offscreenSurf = nullptr; }
	if (s_offscreenTex) { s_offscreenTex->Release(); s_offscreenTex = nullptr; }
	s_pillarboxEnabled = false;
	s_pillarboxActive = false;
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.B: with no pillarbox the scene mirror is the
	// window size and the end-of-frame blit covers the whole swapchain image.
	VKDraw_Set_Present_Rect(0, 0, 0, 0);
#endif
}

bool DX8Wrapper::Pillarbox_Setup(int gameW, int gameH)
{
	Pillarbox_Cleanup();

	// Determine backbuffer dimensions from the active present parameters first.
	// GeneralsX @bugfix GitHub Copilot 27/04/2026 Using native display size here can diverge from
	// the actual DXVK backbuffer during SDL fullscreen transitions and cause zoom/crop artifacts.
	int bbW = (int)_PresentParameters.BackBufferWidth;
	int bbH = (int)_PresentParameters.BackBufferHeight;
	float density = 1.0f;

	if (bbW <= 0 || bbH <= 0) {
		if (!GetWindowSize(bbW, bbH, density)) {
			if (!GetNativeDisplaySize(bbW, bbH, density)) {
				bbW = gameW;
				bbH = gameH;
				density = 1.0f;
			}
		}
	} else {
		// GeneralsX @bugfix macOS HiDPI: the backbuffer size came from the present
		// parameters (in physical pixels), so the density query above was skipped and
		// s_pixelDensity would wrongly stay 1.0. Fetch it explicitly so Pillarbox_Get_Rect
		// can convert the viewport rect back to logical points - SDL delivers mouse
		// coordinates in points, so without this, clicks land at the wrong position.
		int pointsW = 0, pointsH = 0;
		float queriedDensity = 1.0f;
		if (GetWindowSize(pointsW, pointsH, queriedDensity) && queriedDensity > 0.0f) {
			density = queriedDensity;
		}
	}

	// No pillarbox needed if backbuffer matches game resolution
	if (bbW == gameW && bbH == gameH) return false;

	// Create offscreen render target at game resolution
	HRESULT hr = D3DDevice->CreateTexture(gameW, gameH, 1, D3DUSAGE_RENDERTARGET,
		D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s_offscreenTex, nullptr);
	if (FAILED(hr)) return false;
	s_offscreenTex->GetSurfaceLevel(0, &s_offscreenSurf);

	// Create dedicated depth stencil at game resolution
	hr = D3DDevice->CreateDepthStencilSurface(gameW, gameH,
		_PresentParameters.AutoDepthStencilFormat, D3DMULTISAMPLE_NONE, 0, FALSE, &s_depthSurf, nullptr);
	if (FAILED(hr)) {
		s_offscreenSurf->Release(); s_offscreenSurf = nullptr;
		s_offscreenTex->Release(); s_offscreenTex = nullptr;
		return false;
	}

	// Cache backbuffer size and compute aspect-correct fit rect once
	s_bbW = bbW; s_bbH = bbH;
	s_pixelDensity = density;
	float gameAspect = (float)gameW / (float)gameH;
	float bbAspect = (float)bbW / (float)bbH;
	if (bbAspect > gameAspect) {
		s_dstW = (int)(bbH * gameAspect); s_dstH = bbH;
		s_dstX = (bbW - s_dstW) / 2; s_dstY = 0;
	} else {
		s_dstW = bbW; s_dstH = (int)(bbW / gameAspect);
		s_dstX = 0; s_dstY = (bbH - s_dstH) / 2;
	}
	s_pillarboxEnabled = true;
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.B: the Vulkan present chain reproduces this
	// letterbox by blitting the scene mirror into the same rectangle, so the fit rect is published to the
	// pipeline layer as soon as it is known. The D3D9 quad in Pillarbox_End is the other half of the same
	// job and is skipped once the draw path is consuming the frame (see there).
	VKDraw_Set_Present_Rect((unsigned)s_dstX, (unsigned)s_dstY, (unsigned)s_dstW, (unsigned)s_dstH);
#endif
	fprintf(stderr, "INFO: Pillarbox: game=%dx%d, backbuffer=%dx%d, present=%ux%u, windowed=%d\n",
		gameW,
		gameH,
		bbW,
		bbH,
		_PresentParameters.BackBufferWidth,
		_PresentParameters.BackBufferHeight,
		IsWindowed ? 1 : 0);
	return true;
}

void DX8Wrapper::Pillarbox_Begin()
{
	if (!s_pillarboxEnabled || !s_offscreenSurf || !s_depthSurf) return;
	// GeneralsX @bugfix GitHub Copilot 10/05/2026 Route pillarbox target switches through the DX8Wrapper cache so render-target size queries stay correct during the frame.
	Set_Render_Target(s_offscreenSurf, s_depthSurf);
	// GeneralsX @bugfix GitHub Copilot 10/05/2026 Pillarbox uses an offscreen target for presentation only and must not advertise the whole frame as an effect RTT pass.
	IsRenderToTexture = false;
	// GeneralsX @bugfix GitHub Copilot 27/04/2026 Ensure scene render uses game-resolution viewport on the offscreen RT.
	// Without this, a stale fullscreen-sized viewport can survive the target switch and crop/zoom into the top-left area.
	D3DVIEWPORT9 sceneViewport = {
		0,
		0,
		(DWORD)ResolutionWidth,
		(DWORD)ResolutionHeight,
		0.0f,
		1.0f
	};
	D3DDevice->SetViewport(&sceneViewport);
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.C: this viewport is set straight on the D3D9
	// device, so it never passes through DX8Wrapper::Set_Viewport and the submit filler would keep drawing
	// with the last viewport the game named. Publish it - with the pillarbox target it is the whole mirror,
	// which is also what the layer falls back to, but a windowed frame with a stale shadow would not be.
	VK_Shadow_Viewport(&sceneViewport);
#endif
	// Clear the offscreen RT and its depth buffer. The Begin_Render clear targeted
	// the backbuffer before we switched, so without this the offscreen would carry
	// stale depth from the previous frame.
	D3DDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL,
		0x00000000, 1.0f, 0);
	D3DDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
	D3DDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
	D3DDevice->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	s_pillarboxActive = true;
}

void DX8Wrapper::Pillarbox_End()
{
	if (!s_pillarboxActive || !s_offscreenTex) return;
	s_pillarboxActive = false;

	// Restore backbuffer (still inside active scene)
	Set_Render_Target((IDirect3DSurface9 *)nullptr);
	IsRenderToTexture = false;

	// Blit offscreen texture centered onto backbuffer (fit rect cached in Pillarbox_Setup)
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.B: this quad is the letterbox. The Vulkan
	// present chain does the same job by blitting the scene mirror into the rect published at
	// Pillarbox_Setup, and while the Vulkan present gate is up the D3D9 backbuffer is never shown - so
	// drawing the quad would be a raw-device escape that cannot reach the screen. Wedge 9 deletes the
	// block; until then it is what the gate below skips.
	if (VulkanCore_Present_Enabled()) return;
#endif
	D3DVIEWPORT9 vp = {0, 0, (DWORD)s_bbW, (DWORD)s_bbH, 0.0f, 1.0f};
	D3DDevice->SetViewport(&vp);
	D3DDevice->Clear(0, nullptr, D3DCLEAR_TARGET, 0x00000000, 1.0f, 0);

	// Use DX8Wrapper's cached state setters so the cache stays in sync - no invalidation needed.
	D3DDevice->SetTexture(0, s_offscreenTex);
	Set_DX8_Render_State(D3DRS_ZENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_ZWRITEENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);
	Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0xF);
	Set_DX8_Render_State(D3DRS_CLIPPING, FALSE);
	Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	Set_DX8_Texture_Stage_State(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	Set_DX8_Texture_Stage_State(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	DWORD filterMode = (s_dstW == ResolutionWidth && s_dstH == ResolutionHeight)
		? D3DTEXF_POINT : D3DTEXF_LINEAR;
	Set_DX8_Sampler_State(0, D3DSAMP_MINFILTER, filterMode);
	Set_DX8_Sampler_State(0, D3DSAMP_MAGFILTER, filterMode);

	// D3D8 half-pixel offset: XYZRHW vertices need -0.5 for exact texel-to-pixel alignment
	float x0 = (float)s_dstX - 0.5f, y0 = (float)s_dstY - 0.5f;
	float x1 = (float)(s_dstX + s_dstW) - 0.5f, y1 = (float)(s_dstY + s_dstH) - 0.5f;
	struct BV { float x, y, z, rhw; float u, v; };
	BV quad[4] = {
		{x0, y0, 0, 1, 0, 0}, {x1, y0, 0, 1, 1, 0},
		{x0, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1},
	};
	D3DDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
	D3DDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(BV));
	D3DDevice->SetTexture(0, nullptr);
}

bool DX8Wrapper::Pillarbox_Get_Rect(int& x, int& y, int& w, int& h)
{
	if (!s_pillarboxEnabled) return false;
	x = (int)(s_dstX / s_pixelDensity);
	y = (int)(s_dstY / s_pixelDensity);
	w = (int)(s_dstW / s_pixelDensity);
	h = (int)(s_dstH / s_pixelDensity);
	return true;
}

// GeneralsX @bugfix GitHub Copilot 27/04/2026 Keep SDL-managed fullscreen swapchains aligned to the current window size until the native fullscreen transition finishes.
static void Resolve_Present_BackBuffer_Size(int gameW, int gameH, bool isWindowed, UINT& outW, UINT& outH)
{
	outW = (UINT)gameW;
	outH = (UINT)gameH;

#ifndef _WIN32
	if (!isWindowed) {
		int windowW = gameW;
		int windowH = gameH;
		float density = 1.0f;
		if (DX8Wrapper::GetWindowSize(windowW, windowH, density)) {
			outW = (UINT)windowW;
			outH = (UINT)windowH;
			return;
		}
	}
#endif

	if (!isWindowed) {
		int nativeW = 0;
		int nativeH = 0;
		float density = 1.0f;
		if (DX8Wrapper::GetNativeDisplaySize(nativeW, nativeH, density)) {
			outW = (UINT)nativeW;
			outH = (UINT)nativeH;
		}
	}
}

DX8FrameStatistics DX8Wrapper::FrameStatistics;
static DX8FrameStatistics LastFrameStatistics;

bool DX8Wrapper_IsWindowed = true;

// FPU_PRESERVE
int DX8Wrapper_PreserveFPU = 0;

/***********************************************************************************
**
** DX8Wrapper Static Variables
**
***********************************************************************************/

static HWND						_Hwnd															= nullptr;
bool								DX8Wrapper::IsInitted									= false;
bool								DX8Wrapper::_EnableTriangleDraw						= true;

int								DX8Wrapper::CurRenderDevice							= -1;
int								DX8Wrapper::ResolutionWidth							= DEFAULT_RESOLUTION_WIDTH;
int								DX8Wrapper::ResolutionHeight							= DEFAULT_RESOLUTION_HEIGHT;
int								DX8Wrapper::BitDepth										= DEFAULT_BIT_DEPTH;
int								DX8Wrapper::TextureBitDepth							= DEFAULT_TEXTURE_BIT_DEPTH;
bool								DX8Wrapper::IsWindowed									= false;
D3DFORMAT					DX8Wrapper::DisplayFormat	= D3DFMT_UNKNOWN;
D3DMULTISAMPLE_TYPE DX8Wrapper::MultiSampleAntiAliasing	= DEFAULT_MSAA;

// shader system additions KJM v
UINT_PTR						DX8Wrapper::Vertex_Shader						= 0;
UINT_PTR						DX8Wrapper::Pixel_Shader						= 0;
IDirect3DVertexDeclaration9*		DX8Wrapper::Vertex_Declaration					= nullptr;

Vector4							DX8Wrapper::Vertex_Shader_Constants[MAX_VERTEX_SHADER_CONSTANTS];
Vector4							DX8Wrapper::Pixel_Shader_Constants[MAX_PIXEL_SHADER_CONSTANTS];
// GeneralsX @feature PS3.0 batch1 12/10/2026 start from the pre-2.0 budget so a caller that runs
// before Compute_Caps() cannot address registers the historic code could not; Compute_Caps widens
// them to what the device reports.
unsigned							DX8Wrapper::Vertex_Shader_Constant_Limit=MAX_VERTEX_SHADER_CONSTANTS_PS1X;
unsigned							DX8Wrapper::Pixel_Shader_Constant_Limit=MAX_PIXEL_SHADER_CONSTANTS_PS1X;

LightEnvironmentClass*		DX8Wrapper::Light_Environment							= nullptr;

DWORD								DX8Wrapper::Vertex_Processing_Behavior				= 0;
ZTextureClass*					DX8Wrapper::Shadow_Map[MAX_SHADOW_MAPS];

Vector3							DX8Wrapper::Ambient_Color;
// shader system additions KJM ^

bool								DX8Wrapper::world_identity;
unsigned							DX8Wrapper::RenderStates[256];
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the material the fixed-function vertex stage
// reads before the game has applied one is the one the engine itself installs for a null material
// (VertexMaterialClass::Apply_Null): white diffuse/ambient, no specular/emissive, power 1.
D3DMATERIAL9						DX8Wrapper::Material_Shadow =
{ { 1.0f, 1.0f, 1.0f, 1.0f }, { 1.0f, 1.0f, 1.0f, 1.0f },
  { 0.0f, 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, 1.0f };
bool								DX8Wrapper::Material_Shadow_Valid						= false;
unsigned							DX8Wrapper::TextureStageStates[MAX_TEXTURE_STAGES][33];
unsigned							DX8Wrapper::SamplerStates[MAX_TEXTURE_STAGES][16];
IDirect3DBaseTexture9 *		DX8Wrapper::Textures[MAX_TEXTURE_STAGES];
RenderStateStruct				DX8Wrapper::render_state;
unsigned							DX8Wrapper::render_state_changed;

// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0 (pure-Vulkan port): the render-state census.
//
// Why this exists: the facade replays state lazily (Apply_Render_State_Changes marks dirty bits, the
// next draw applies them), which is free on D3D9 but has no equivalent on Vulkan, where state is baked
// into pipeline state objects that must be enumerated up front. Guessing that set produced the classic
// port failure of "works until a rare shader walks in", so the wedge order starts by measuring: with
// GENERALSX_VKCENSUS=1 every draw digests the *live* shadow state into two keys -
//   pso key  = shader bits + FVF + VS/PS tags + light mask + touched render states
//              (deliberately WITHOUT stage state: wedge 7 replaces the combiner with one interpreted
//               pixel shader that reads stage state as uniforms, so it must not multiply pipelines)
//   full key = pso key + per-stage texture stage state + sampler state
// - and histograms both. The gap between the two counts *is* the payoff of the interpreted-shader
// design, and the token lists are the exact surface wedge 5's state table and wedge 7's switch have
// to cover. Dumps repeat every 20000 draws so a run that gets killed mid-scenario still leaves data.
// Off, the only cost is the one-byte touched marks in the setters.
unsigned char						DX8Wrapper::VK_RS_Touched[256];
unsigned char						DX8Wrapper::VK_TSS_Touched[33];
unsigned char						DX8Wrapper::VK_Samp_Touched[16];
DWORD								DX8Wrapper::Vertex_Format = 0;

#include <cstdio>
#include <cstdlib>

namespace
{
	const unsigned          VKCENSUS_ROW_SLOTS   = 8192;   // power of two, open addressing
	const unsigned          VKCENSUS_ROW_MASK    = VKCENSUS_ROW_SLOTS - 1;
	const unsigned          VKCENSUS_SET_SLOTS   = 4096;
	const unsigned          VKCENSUS_SET_MASK    = VKCENSUS_SET_SLOTS - 1;
	const unsigned          VKCENSUS_TAG_SLOTS   = 512;
	const unsigned          VKCENSUS_TAG_MASK    = 1023;   // probe table is twice the slots
	const unsigned          VKCENSUS_MAX_TOKENS  = 33;
	const unsigned long long VKCENSUS_DUMP_EVERY = 20000;
	const unsigned VKCENSUS_RS_KEY_SLOTS = 40;      // >= VKCENSUS_RS_KEY_COUNT, sized before the tables
	const unsigned VKCENSUS_RS_VALUE_SLOTS = 8;     // distinct values remembered per keying state
	const unsigned long long VKCENSUS_TOKEN_EVERY = 512;   // rebuild touched-token lists this often

	// Render states that carry a per-frame VALUE instead of selecting a pipeline. They are curated (so
	// they never show up as untracked) but deliberately excluded from the pso key: keying a pipeline on
	// fog distance would explode the cache. Wedge 5 reads this list as "these go in the UBO".
	const unsigned VKCENSUS_RS_UNIFORM[] = {
		D3DRS_AMBIENT,
		D3DRS_FOGSTART,
		D3DRS_FOGEND,
		D3DRS_FOGCOLOR,
		D3DRS_FOGDENSITY,
		D3DRS_TEXTUREFACTOR,
	};
	const unsigned VKCENSUS_RS_UNIFORM_N =
		(unsigned)(sizeof(VKCENSUS_RS_UNIFORM) / sizeof(VKCENSUS_RS_UNIFORM[0]));
	// Slots are a little wider than the table so the classification loops can never walk off the end.
	const unsigned VKCENSUS_RS_UNIFORM_COUNT = VKCENSUS_RS_UNIFORM_N + 2u;

	struct VKCensusRow
	{
		unsigned long long pso_key;
		unsigned long long full_key;
		unsigned long long draws;
		unsigned long long polys;
		unsigned           shader_bits;
		unsigned           fvf;
		unsigned           vs_tag;
		unsigned           ps_tag;
		unsigned           light_mask;
		unsigned           prim;
		unsigned short     used;
		unsigned short     pad;
	};

	// key -> count, used for the pso-key set, the stage-combination histograms and the render-target
	// set (those three must stay distinct from the row table because they collapse other fields).
	struct VKCensusSet
	{
		unsigned long long key;
		unsigned long long count;
	};

	struct VKCensusTag
	{
		UINT_PTR handle;
		unsigned tag;
	};

	struct VKCensusSnap
	{
		unsigned                 shader_bits;
		unsigned                 fvf;
		UINT_PTR                 vs_handle;
		UINT_PTR                 ps_handle;
		unsigned                 light_mask;
		unsigned long long       rt_key;
		unsigned                 prim;
		unsigned                 polys;
		unsigned                 verts;
		unsigned                 stages;          // stages a pass can actually reach
		const unsigned*          rs;              // RenderStates[256]
		const unsigned*          tss;             // TextureStageStates flattened [stages][33]
		const unsigned*          samp;            // SamplerStates flattened [stages][16]
		const unsigned char*     rs_touch;
		const unsigned char*     tss_touch;
		const unsigned char*     samp_touch;
	};

	struct VKCensusState
	{
		VKCensusRow* rows;
		VKCensusSet* pso_set;
		VKCensusSet* stage_set;
		VKCensusSet* rt_set;
		VKCensusTag  tags[VKCENSUS_TAG_MASK + 1];

		unsigned rs_tok[256];
		unsigned tss_tok[VKCENSUS_MAX_TOKENS];
		unsigned samp_tok[VKCENSUS_MAX_TOKENS];
		unsigned rs_tok_n;
		unsigned tss_tok_n;
		unsigned samp_tok_n;
		// The render states a Vulkan pipeline keys on. Keeping this as an explicit table (rather than
		// "hash every touched slot") makes the pso key stable across a run and, more importantly, makes
		// the gap between what the census hashes and what the game writes visible: anything touched but
		// not curated lands in rs_untracked, and that list has to be empty before wedge 5's state table
		// can be called complete.
		unsigned char rs_curated[256];
		unsigned char rs_untracked[256];
		unsigned rs_untracked_n;
		unsigned long long rs_untracked_hits;
		// Per keying state, the distinct values the game actually uses. This is what turns the census from
		// a hash dump into wedge 5's translation table: "ZFUNC has 2 values, COLORWRITE 3" is the real
		// multiplier on pipeline variants, and a state that saturates its 8 slots warns that the key is
		// wider than the pipeline cache should be.
		unsigned rs_key_val[VKCENSUS_RS_KEY_SLOTS][VKCENSUS_RS_VALUE_SLOTS];
		unsigned char rs_key_val_n[VKCENSUS_RS_KEY_SLOTS];
		unsigned long long rs_key_val_overflow;
		// Render states that are uniform VALUES rather than pipeline choices: fog geometry, ambient,
		// texture factor. A Vulkan pipeline must not key on them (they change per frame), but wedge 5's
		// UBO has to carry them, so they get their own curated class and their own observed-value range.
		unsigned char rs_uniform[256];
		unsigned rs_uniform_tok[VKCENSUS_RS_UNIFORM_COUNT];
		unsigned rs_uniform_n;
		unsigned long long rs_uniform_hits;
		unsigned long long rs_uniform_writes[VKCENSUS_RS_UNIFORM_COUNT];
		unsigned rs_uniform_last[VKCENSUS_RS_UNIFORM_COUNT];
		unsigned rs_uniform_changes[VKCENSUS_RS_UNIFORM_COUNT];
		unsigned char rs_drop[256];
		unsigned rs_drop_tok[VKCENSUS_MAX_TOKENS];
		unsigned rs_drop_n;
		unsigned fvf_list[64];
		unsigned fvf_n;
		unsigned light_list[256];
		unsigned light_n;
		unsigned op_list[64];
		unsigned op_n;
		unsigned arg_list[64];
		unsigned arg_n;
		unsigned tss_class[VKCENSUS_MAX_TOKENS];   // 0 other, 1 combiner op, 2 combiner operand

		unsigned long long draws;
		unsigned long long polys;
		unsigned long long verts;
		unsigned long long distinct_pso;
		unsigned long long distinct_rows;
		unsigned long long distinct_stage;
		unsigned long long distinct_rt;
		unsigned long long ff_draws;
		unsigned long long prog_draws;
		unsigned long long vs_draws;
		unsigned long long ps_draws;
		unsigned long long tag_overflow;
		unsigned long long next_dump;
		unsigned long long dumps;
		unsigned long long token_ticks;
		unsigned           tag_count;
		unsigned long long row_overflow;
		bool               checked;
		bool               on;
		bool               classified;
		char               csv[512];
	};

	VKCensusState CX;   // namespace-scope: zero initialised

	// The render states folded into the pso key. This is the honest candidate list for wedge 5's
	// pipeline-state translation, and it is deliberately a table instead of "hash every touched slot":
	// the key must be stable across a run, and any token the game writes that is NOT in this table has
	// to show up as untracked so the list can be corrected rather than silently under-counted.
	const unsigned VKCENSUS_RS_KEY[] = {
		D3DRS_LIGHTING,
		D3DRS_ZENABLE,
		D3DRS_ZWRITEENABLE,
		D3DRS_CULLMODE,
		D3DRS_ALPHABLENDENABLE,
		D3DRS_SRCBLEND,
		D3DRS_DESTBLEND,
		D3DRS_ALPHATESTENABLE,
		D3DRS_ALPHAFUNC,
		D3DRS_ALPHAREF,
		D3DRS_FOGENABLE,
		D3DRS_FOGTABLEMODE,
		D3DRS_FOGVERTEXMODE,
		D3DRS_COLORWRITEENABLE,
		D3DRS_STENCILENABLE,
		D3DRS_STENCILFUNC,
		D3DRS_SRGBWRITEENABLE,
		D3DRS_ZFUNC,
		D3DRS_FILLMODE,
		D3DRS_SHADEMODE,
		D3DRS_DIFFUSEMATERIALSOURCE,
		D3DRS_AMBIENTMATERIALSOURCE,
		D3DRS_SPECULARMATERIALSOURCE,
		D3DRS_EMISSIVEMATERIALSOURCE,
		D3DRS_CLIPPLANEENABLE,
		D3DRS_MULTISAMPLEANTIALIAS,
		D3DRS_DEPTHBIAS,
		D3DRS_SLOPESCALEDEPTHBIAS,
		D3DRS_SPECULARENABLE,
		D3DRS_COLORVERTEX,
	};
	const unsigned VKCENSUS_RS_KEY_COUNT = (unsigned)(sizeof(VKCENSUS_RS_KEY) / sizeof(VKCENSUS_RS_KEY[0]));

	// Fixed-function switches the port consciously folds away rather than reproducing: range fog becomes
	// plain vertex fog for every distance (wedge 6's decision), so it must not split a pipeline key, but it
	// still has to be accounted for. Listing it here keeps the "everything the game writes is classified"
	// property of the census instead of hiding it in an untracked bucket that would look like a bug.
	const unsigned VKCENSUS_RS_DROP[] = {
		D3DRS_RANGEFOGENABLE,
	};
	const unsigned VKCENSUS_RS_DROP_COUNT = (unsigned)(sizeof(VKCENSUS_RS_DROP) / sizeof(VKCENSUS_RS_DROP[0]));

	unsigned long long vk_census_hash(unsigned long long h, unsigned v)
	{
		const unsigned char* b = (const unsigned char*)&v;
		for (int i = 0; i < 4; ++i) {
			h ^= (unsigned long long)b[i];
			h *= 1099511628211ULL;
		}
		return h;
	}

	unsigned long long vk_census_hash64(unsigned long long h, unsigned long long v)
	{
		h = vk_census_hash(h, (unsigned)(v & 0xFFFFFFFFULL));
		return vk_census_hash(h, (unsigned)(v >> 32));
	}

	unsigned vk_census_slot(unsigned long long key)
	{
		return (unsigned)((key ^ (key >> 29) ^ (key >> 58)) & VKCENSUS_SET_MASK);
	}

	// Bump a key in a set table; returns true when the key was new.
	bool vk_census_bump(VKCensusSet* table, unsigned long long key, unsigned long long delta)
	{
		unsigned idx = vk_census_slot(key);
		for (unsigned probe = 0; probe <= VKCENSUS_SET_MASK; ++probe) {
			VKCensusSet& s = table[idx];
			if (!s.key && !s.count) {
				s.key = key;
				s.count = delta;
				return true;
			}
			if (s.key == key) { s.count += delta; return false; }
			idx = (idx + 1) & VKCENSUS_SET_MASK;
		}
		return false;   // table saturated: stop counting, the gate line reports the pressure
	}

	unsigned vk_census_tag(UINT_PTR handle)
	{
		if (!handle) return 0;
		unsigned long long h = (unsigned long long)handle;
		unsigned idx = (unsigned)((h ^ (h >> 17)) & VKCENSUS_TAG_MASK);
		for (unsigned probe = 0; probe <= VKCENSUS_TAG_MASK; ++probe) {
			VKCensusTag& t = CX.tags[idx];
			if (!t.handle) {
				if (CX.tag_count >= VKCENSUS_TAG_SLOTS) { ++CX.tag_overflow; return VKCENSUS_TAG_SLOTS + 1; }
				t.handle = handle;
				t.tag = ++CX.tag_count;
				return t.tag;
			}
			if (t.handle == handle) return t.tag;
			idx = (idx + 1) & VKCENSUS_TAG_MASK;
		}
		++CX.tag_overflow;
		return VKCENSUS_TAG_SLOTS + 1;
	}

	// Small value lists: the census wants "which distinct X did the game use", not a histogram.
	bool vk_census_add_list(unsigned* list, unsigned& count, unsigned capacity, unsigned value)
	{
		for (unsigned i = 0; i < count; ++i) if (list[i] == value) return false;
		if (count >= capacity) return false;
		list[count++] = value;
		return true;
	}

	// Fold the sentinel written by Invalidate_Cached_Render_States out of a digest: an invalidated slot
	// has no value yet, so it must not split one key into two across a device reset.
	unsigned vk_census_value(unsigned raw)
	{
		return (raw == 0x12345678u) ? 0u : raw;
	}


	void vk_census_rebuild_tokens(const VKCensusSnap& s)
	{
		CX.rs_tok_n = 0;
		CX.tss_tok_n = 0;
		CX.samp_tok_n = 0;
		CX.rs_uniform_n = 0;
		CX.rs_drop_n = 0;
		for (unsigned i = 0; i < 256; ++i) {
			if (!s.rs_touch[i]) continue;
			CX.rs_tok[CX.rs_tok_n++] = i;
			if (CX.rs_uniform[i]) {
				if (CX.rs_uniform_n < VKCENSUS_RS_UNIFORM_COUNT) CX.rs_uniform_tok[CX.rs_uniform_n++] = i;
			} else if (CX.rs_drop[i]) {
				if (CX.rs_drop_n < VKCENSUS_MAX_TOKENS) CX.rs_drop_tok[CX.rs_drop_n++] = i;
			} else if (!CX.rs_curated[i] && !CX.rs_untracked[i]) {
				CX.rs_untracked[i] = 1;
				++CX.rs_untracked_n;
			}
		}
		for (unsigned i = 0; i < VKCENSUS_MAX_TOKENS; ++i) {
			if (s.tss_touch[i]) {
				if (CX.tss_tok_n < VKCENSUS_MAX_TOKENS) CX.tss_tok[CX.tss_tok_n++] = i;
			}
			if (s.samp_touch[i] && i < 16) {
				if (CX.samp_tok_n < VKCENSUS_MAX_TOKENS) CX.samp_tok[CX.samp_tok_n++] = i;
			}
		}

		// Classify the stage-state tokens once, off the facade's own name tables (see
		// vk_census_ops_and_args for why this is name-driven rather than ordinal-driven).
		if (!CX.classified) {
			CX.classified = true;
			for (unsigned i = 0; i < VKCENSUS_MAX_TOKENS; ++i) {
				const char* name = DX8Wrapper::Get_DX8_Texture_Stage_State_Name((D3DTEXTURESTAGESTATETYPE)i);
				if (!name) continue;
				unsigned cls = 0;
				for (const char* p = name; *p; ++p) {
					if (p[0] == 'O' && p[1] == 'P' && (p[2] < '0' || p[2] > '9' || !p[2])) { cls = 1; break; }
				}
				if (!cls) {
					for (const char* p = name; *p; ++p) {
						if (p[0] == 'A' && p[1] == 'R' && p[2] == 'G') { cls = 2; break; }
					}
				}
				CX.tss_class[i] = cls;
			}
		}
	}

	// Remember the distinct values a keying render state takes (first VKCENSUS_RS_VALUE_SLOTS of them).
	void vk_census_note_key_value(unsigned slot, unsigned value)
	{
		unsigned char& n = CX.rs_key_val_n[slot];
		for (unsigned i = 0; i < n; ++i) {
			if (CX.rs_key_val[slot][i] == value) return;
		}
		if (n < VKCENSUS_RS_VALUE_SLOTS) {
			CX.rs_key_val[slot][n++] = value;
			return;
		}
		++CX.rs_key_val_overflow;
	}

	VKCensusRow* vk_census_find_row(unsigned long long full)
	{
		unsigned idx = (unsigned)((full ^ (full >> 29) ^ (full >> 58)) & VKCENSUS_ROW_MASK);
		for (unsigned probe = 0; probe <= VKCENSUS_ROW_MASK; ++probe) {
			VKCensusRow& r = CX.rows[idx];
			if (!r.used) return &r;
			if (r.full_key == full) return &r;
			idx = (idx + 1) & VKCENSUS_ROW_MASK;
		}
		return nullptr;
	}

	// Op/arg collection: stage state tokens whose value space is an enum the interpreted pixel shader
	// has to switch over. The token is classified by the name the facade's own tables give it ("...OP"
	// is a combiner operation, "...ARG" one of its operands) rather than by hard-coded D3DTSS_* ordinals:
	// the numeric space differs between the DX8 and D3D9 headers, the names do not, and those same name
	// tables are what the CSV prints, so this classification cannot drift from the report.
	void vk_census_ops_and_args(const VKCensusSnap& s)
	{
		for (unsigned stage = 0; stage < s.stages; ++stage) {
			const unsigned* row = s.tss + (size_t)stage * VKCENSUS_MAX_TOKENS;
			for (unsigned t = 0; t < CX.tss_tok_n; ++t) {
				const unsigned tok = CX.tss_tok[t];
				const unsigned cls = CX.tss_class[tok];
				if (!cls) continue;
				const unsigned v = vk_census_value(row[tok]);
				if (!v) continue;
				if (cls == 1) vk_census_add_list(CX.op_list, CX.op_n, 64, v);
				else vk_census_add_list(CX.arg_list, CX.arg_n, 64, v);
			}
		}
	}

	void vk_census_record(const VKCensusSnap& s)
	{
		++CX.draws;
		CX.polys += s.polys;
		CX.verts += s.verts;

		if (!(CX.token_ticks & (VKCENSUS_TOKEN_EVERY - 1))) vk_census_rebuild_tokens(s);
		++CX.token_ticks;

		// --- pso key: everything a pipeline needs except stage state ----------------------------
		unsigned long long pso = 1469598103934665603ULL;
		pso = vk_census_hash(pso, s.shader_bits);
		pso = vk_census_hash(pso, s.fvf);
		pso = vk_census_hash(pso, s.light_mask);
		for (unsigned t = 0; t < VKCENSUS_RS_KEY_COUNT; ++t) {
			const unsigned v = vk_census_value(s.rs[VKCENSUS_RS_KEY[t]]);
			pso = vk_census_hash(pso, v);
			if (t < VKCENSUS_RS_KEY_SLOTS) vk_census_note_key_value(t, v);
		}

		const unsigned vs_tag = vk_census_tag(s.vs_handle);
		const unsigned ps_tag = vk_census_tag(s.ps_handle);
		pso = vk_census_hash(pso, vs_tag);
		pso = vk_census_hash(pso, ps_tag);
		if (vk_census_bump(CX.pso_set, pso, 1)) ++CX.distinct_pso;

		// --- full key: plus everything stage state can still change ------------------------------
		unsigned long long full = pso;
		for (unsigned stage = 0; stage < s.stages; ++stage) {
			const unsigned* trow = s.tss + (size_t)stage * VKCENSUS_MAX_TOKENS;
			const unsigned* srow = s.samp + (size_t)stage * 16;
			unsigned long long stage_key = vk_census_hash(1469598103934665603ULL, stage);
			for (unsigned t = 0; t < CX.tss_tok_n; ++t) {
				const unsigned v = vk_census_value(trow[CX.tss_tok[t]]);
				full = vk_census_hash(full, v);
				stage_key = vk_census_hash(stage_key, v);
			}
			for (unsigned t = 0; t < CX.samp_tok_n; ++t) {
				const unsigned v = vk_census_value(srow[CX.samp_tok[t]]);
				full = vk_census_hash(full, v);
				stage_key = vk_census_hash(stage_key, v);
			}
			// The per-stage tuple on its own is wedge 7's real workload: it is what the interpreted
			// pixel shader's switch actually has to cover, stage by stage.
			if (vk_census_bump(CX.stage_set, stage_key, 1)) ++CX.distinct_stage;
		}

		vk_census_ops_and_args(s);

		// Render target sits outside both keys on purpose - a pipeline does not care which target it
		// fills - but wedge 5's framebuffer/pass cache does, so it is counted on its own.
		if (vk_census_bump(CX.rt_set, vk_census_hash64(1469598103934665603ULL, s.rt_key), 1)) ++CX.distinct_rt;

		for (unsigned t = 0; t < CX.rs_tok_n; ++t) {
			if (CX.rs_untracked[CX.rs_tok[t]]) ++CX.rs_untracked_hits;
		}
		// How often a uniform-class state actually changes value. A token that moves between draws is the
		// evidence wedge 5 needs: it cannot be a pipeline key, it has to reach the constant buffer.
		for (unsigned u = 0; u < CX.rs_uniform_n; ++u) {
			const unsigned v = vk_census_value(s.rs[CX.rs_uniform_tok[u]]);
			++CX.rs_uniform_writes[u];
			if (v != CX.rs_uniform_last[u]) { ++CX.rs_uniform_changes[u]; CX.rs_uniform_last[u] = v; }
			++CX.rs_uniform_hits;
		}

		if (vs_tag) ++CX.vs_draws;
		if (ps_tag) ++CX.ps_draws;
		if (vs_tag || ps_tag) ++CX.prog_draws;
		else ++CX.ff_draws;

		vk_census_add_list(CX.fvf_list, CX.fvf_n, 64, s.fvf);
		vk_census_add_list(CX.light_list, CX.light_n, 256, s.light_mask);

		VKCensusRow* row = vk_census_find_row(full);
		if (!row) { ++CX.row_overflow; return; }
		if (!row->used) {
			row->used = 1;
			row->full_key = full;
			row->pso_key = pso;
			row->shader_bits = s.shader_bits;
			row->fvf = s.fvf;
			row->vs_tag = vs_tag;
			row->ps_tag = ps_tag;
			row->light_mask = s.light_mask;
			row->prim = s.prim;
			++CX.distinct_rows;
		}
		++row->draws;
		row->polys += s.polys;
	}

	int vk_census_cmp_row(const void* a, const void* b)
	{
		const VKCensusRow* ra = (const VKCensusRow*)a;
		const VKCensusRow* rb = (const VKCensusRow*)b;
		if (ra->draws < rb->draws) return 1;
		if (ra->draws > rb->draws) return -1;
		return 0;
	}

	int vk_census_cmp_set(const void* a, const void* b)
	{
		const VKCensusSet* sa = (const VKCensusSet*)a;
		const VKCensusSet* sb = (const VKCensusSet*)b;
		if (sa->count < sb->count) return 1;
		if (sa->count > sb->count) return -1;
		return 0;
	}

	void vk_census_copy(char* dst, size_t cap, const char* src)
	{
		size_t i = 0;
		if (!cap) return;
		for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
		dst[i] = 0;
	}

	// This TU is compiled on every platform (DXVK serves the same D3D9 API on Linux/macOS), so the env
	// reads go through the platform's own accessor instead of assuming Win32.
	bool vk_census_env_flag(const char* name)
	{
#ifdef _WIN32
		char raw[8] = {};
		DWORD n = GetEnvironmentVariableA(name, raw, (DWORD)sizeof(raw));
		return (n != 0 && raw[0] != '0');
#else
		const char* raw = getenv(name);
		return (raw != nullptr && raw[0] != 0 && raw[0] != '0');
#endif
	}

	const char* vk_census_env_text(const char* name, char* buffer, size_t cap)
	{
#ifdef _WIN32
		DWORD n = GetEnvironmentVariableA(name, buffer, (DWORD)cap);
		return (n != 0 && n < (DWORD)cap) ? buffer : nullptr;
#else
		const char* raw = getenv(name);
		if (!raw) return nullptr;
		vk_census_copy(buffer, cap, raw);
		return buffer;
#endif
	}

	// The CSV goes to the Vulkan work hub by default so the artefact sits with the rest of the port's
	// forensics; GENERALSX_VKCENSUS_CSV overrides it. The default is CWD-relative because the port's run
	// script starts the game from its root.
	void vk_census_default_csv(char* out, size_t cap)
	{
		char env[512] = {};
		if (vk_census_env_text("GENERALSX_VKCENSUS_CSV", env, sizeof(env))) vk_census_copy(out, cap, env);
		else vk_census_copy(out, cap, "Vulkan/artifacts/census.csv");
	}

	unsigned long long vk_census_uniform_changes()
	{
		unsigned long long total = 0;
		for (unsigned u = 0; u < VKCENSUS_RS_UNIFORM_COUNT; ++u) total += CX.rs_uniform_changes[u];
		return total;
	}
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: read a shadowed render state as the value
// the device would have applied.
//
// The census (Vulkan/artifacts/census_menu.csv, 2.42M draws) is what forces this. Six of the states that
// key a pipeline are never written by the engine at all: D3DRS_ZENABLE, D3DRS_SHADEMODE,
// D3DRS_STENCILFUNC, D3DRS_CLIPPLANEENABLE, D3DRS_MULTISAMPLEANTIALIAS and D3DRS_SRGBWRITEENABLE each
// appear in the census exactly once, as the value 0x00000000, and that is not a real setting - it is
// Invalidate_Cached_Render_States' sentinel 0x12345678 folded to zero by vk_census_value above (the
// digest does that on purpose, so a device reset cannot split one key into two). Read the same way, a
// pipeline key would therefore carry depth testing off and shading flat - and a batch that never named a
// state would look like a content bug rather than a state-reading bug. A pipeline has to be built from
// what the hardware would have done, so this is that table: ordinals from
// Windows Kits/10/Include/10.0.26100.0/shared/d3d9types.h, defaults from the documented D3D9 state
// initial values, which are also what the D3D9 device this port replaces applied to those same batches.
unsigned DX8Wrapper::VK_Read_RS(unsigned token)
{
	const unsigned raw = RenderStates[token & 0xFFu];
	if (raw != 0x12345678u) return raw;
	switch (token) {
	case D3DRS_ZENABLE:                return D3DZB_TRUE;
	case D3DRS_ZWRITEENABLE:           return TRUE;
	case D3DRS_ZFUNC:                  return D3DCMP_LESSEQUAL;
	case D3DRS_FILLMODE:               return D3DFILL_SOLID;
	case D3DRS_SHADEMODE:              return D3DSHADE_GOURAUD;
	case D3DRS_CULLMODE:               return D3DCULL_CCW;
	case D3DRS_ALPHABLENDENABLE:       return FALSE;
	case D3DRS_ALPHATESTENABLE:        return FALSE;
	case D3DRS_SRCBLEND:               return D3DBLEND_ONE;
	case D3DRS_DESTBLEND:              return D3DBLEND_ZERO;
	case D3DRS_ALPHAFUNC:              return D3DCMP_GREATER;
	case D3DRS_ALPHAREF:               return 0;
	case D3DRS_FOGENABLE:              return FALSE;
	case D3DRS_FOGTABLEMODE:           return D3DFOG_NONE;
	case D3DRS_FOGVERTEXMODE:          return D3DFOG_NONE;
	case D3DRS_RANGEFOGENABLE:         return FALSE;
	case D3DRS_SPECULARENABLE:         return FALSE;
	case D3DRS_LIGHTING:               return TRUE;
	case D3DRS_COLORVERTEX:            return TRUE;
	case D3DRS_LOCALVIEWER:            return TRUE;
	case D3DRS_NORMALIZENORMALS:       return FALSE;
	case D3DRS_DIFFUSEMATERIALSOURCE:  return D3DMCS_COLOR1;
	case D3DRS_AMBIENTMATERIALSOURCE:  return D3DMCS_MATERIAL;
	case D3DRS_SPECULARMATERIALSOURCE: return D3DMCS_COLOR2;
	case D3DRS_EMISSIVEMATERIALSOURCE: return D3DMCS_MATERIAL;
	case D3DRS_AMBIENT:                return 0u;
	case D3DRS_FOGCOLOR:               return 0u;
	case D3DRS_TEXTUREFACTOR:          return 0u;
	case D3DRS_COLORWRITEENABLE:       return 0x0000000Fu;
	case D3DRS_STENCILENABLE:          return FALSE;
	case D3DRS_STENCILFUNC:            return D3DCMP_ALWAYS;
	case D3DRS_CLIPPLANEENABLE:        return 0u;
	case D3DRS_MULTISAMPLEANTIALIAS:   return FALSE;
	case D3DRS_SRGBWRITEENABLE:        return 0u;
	case D3DRS_DEPTHBIAS:              return 0u;
	case D3DRS_SLOPESCALEDEPTHBIAS:    return 0u;
	default:                           return 0u;
	}
}

// Same sentinel problem for the states that carry a float: D3D9 documents fog start 0.0, fog end 1.0
// and density 1.0, and the shadow holds the bit pattern of the float, not the float.
unsigned DX8Wrapper::VK_Read_RS_Default_bits(unsigned token, float fallback)
{
	const unsigned raw = RenderStates[token & 0xFFu];
	if (raw != 0x12345678u) return raw;
	float v = fallback;
	unsigned bits;
	memcpy(&bits, &v, sizeof(bits));
	return bits;
}

// Texture-stage shadow read: D3DTSS_TEXCOORDINDEX defaults to the stage's own coordinate set (the
// documentation says "Stage"), everything else in the stage registers defaults to zero.
unsigned DX8Wrapper::VK_Read_TSS(unsigned stage, unsigned token, unsigned fallback)
{
	const unsigned raw = TextureStageStates[stage & (MAX_TEXTURE_STAGES - 1u)][token & 0x3Fu];
	return (raw == 0x12345678u) ? fallback : raw;
}

// ----------------------------------------------------------------------------
//
// Census entry points (see the block above for what each number is for).
//
// ----------------------------------------------------------------------------

bool DX8Wrapper::VK_Census_On()
{
	if (!CX.checked) {
		CX.checked = true;
		if (!vk_census_env_flag("GENERALSX_VKCENSUS")) return false;

		CX.rows      = (VKCensusRow*)calloc(VKCENSUS_ROW_SLOTS, sizeof(VKCensusRow));
		CX.pso_set   = (VKCensusSet*)calloc(VKCENSUS_SET_SLOTS, sizeof(VKCensusSet));
		CX.stage_set = (VKCensusSet*)calloc(VKCENSUS_SET_SLOTS, sizeof(VKCensusSet));
		CX.rt_set    = (VKCensusSet*)calloc(VKCENSUS_SET_SLOTS, sizeof(VKCensusSet));

		if (!CX.rows || !CX.pso_set || !CX.stage_set || !CX.rt_set) {
			free(CX.rows); free(CX.pso_set); free(CX.stage_set); free(CX.rt_set);
			CX.rows = nullptr; CX.pso_set = nullptr; CX.stage_set = nullptr; CX.rt_set = nullptr;
			fprintf(stderr, "[VKCENSUS] enabled but tables did not allocate; census disabled\n");
			fflush(stderr);
			return false;
		}

		for (unsigned t = 0; t < VKCENSUS_RS_KEY_COUNT; ++t) CX.rs_curated[VKCENSUS_RS_KEY[t]] = 1;
		for (unsigned t = 0; t < VKCENSUS_RS_UNIFORM_N; ++t) CX.rs_uniform[VKCENSUS_RS_UNIFORM[t]] = 1;
		for (unsigned t = 0; t < VKCENSUS_RS_DROP_COUNT; ++t) CX.rs_drop[VKCENSUS_RS_DROP[t]] = 1;
		vk_census_default_csv(CX.csv, sizeof(CX.csv));
		CX.next_dump = VKCENSUS_DUMP_EVERY;
		CX.on = true;

		fprintf(stderr, "[VKCENSUS] render-state census ON, csv=%s\n", CX.csv);
		fflush(stderr);
	}
	return CX.on;
}

void DX8Wrapper::VK_Census_Draw(unsigned prim_type, unsigned short polygon_count, unsigned short vertex_count)
{
	if (!VK_Census_On()) return;

	VKCensusSnap s;
	s.shader_bits = (unsigned&)render_state.shader;
	s.fvf         = (unsigned)Vertex_Format;
	s.vs_handle   = Vertex_Shader;
	s.ps_handle   = Pixel_Shader;

	unsigned mask = 0;
	for (unsigned i = 0; i < 8; ++i) {
		if (render_state.LightEnable[i]) mask |= (1u << i);
	}
	s.light_mask = mask;

	s.rt_key = (unsigned long long)(UINT_PTR)CurrentRenderTarget;
	s.prim   = prim_type;
	s.polys  = polygon_count;
	s.verts  = vertex_count;

	s.stages = CurrentCaps ? (unsigned)CurrentCaps->Get_Max_Textures_Per_Pass() : 2u;
	if (s.stages > MAX_TEXTURE_STAGES) s.stages = MAX_TEXTURE_STAGES;

	s.rs         = RenderStates;
	s.tss        = &TextureStageStates[0][0];
	s.samp       = &SamplerStates[0][0];
	s.rs_touch   = VK_RS_Touched;
	s.tss_touch  = VK_TSS_Touched;
	s.samp_touch = VK_Samp_Touched;

	vk_census_record(s);

	if (CX.draws >= CX.next_dump) {
		CX.next_dump += VKCENSUS_DUMP_EVERY;
		VK_Census_Dump("periodic");
	}
}

void DX8Wrapper::VK_Census_Dump(const char* reason)
{
	if (!CX.on) return;
	++CX.dumps;

	VKCensusRow* sorted_rows = (VKCensusRow*)malloc(sizeof(VKCensusRow) * VKCENSUS_ROW_SLOTS);
	VKCensusSet* sorted_set  = (VKCensusSet*)malloc(sizeof(VKCensusSet) * VKCENSUS_SET_SLOTS);

	FILE* f = fopen(CX.csv, "w");
	if (!f) {
		fprintf(stderr, "[VKCENSUS] csv open FAILED path=%s\n", CX.csv);
		fflush(stderr);
	} else {
		const double collapse = CX.distinct_pso
			? (double)CX.distinct_rows / (double)CX.distinct_pso : 0.0;

		fprintf(f, "# GeneralsX pure-Vulkan wedge 0 render-state census\n");
		fprintf(f, "# reason=%s dumps=%llu\n", reason, CX.dumps);
		fprintf(f, "# summary,draws,%llu,polys,%llu,psoKeys,%llu,fullKeys,%llu,collapse,%g\n",
			CX.draws, CX.polys, CX.distinct_pso, CX.distinct_rows, collapse);
		fprintf(f, "# stageCombos,%llu,targets,%llu,fvfs,%u,lightMasks,%u,shaderTags,%u\n",
			CX.distinct_stage, CX.distinct_rt, CX.fvf_n, CX.light_n, CX.tag_count);
		fprintf(f, "# fixedFunction,%llu,programmable,%llu,vertexShader,%llu,pixelShader,%llu\n",
			CX.ff_draws, CX.prog_draws, CX.vs_draws, CX.ps_draws);
		fprintf(f, "# ops,%u,args,%u,rsTokens,%u,tssTokens,%u,sampTokens,%u\n",
			CX.op_n, CX.arg_n, CX.rs_tok_n, CX.tss_tok_n, CX.samp_tok_n);
		fprintf(f, "# untrackedRsTokens,%u,untrackedHits,%llu,rowOverflow,%llu,tagOverflow,%llu,keyValueOverflow,%llu\n",
			CX.rs_untracked_n, CX.rs_untracked_hits, CX.row_overflow, CX.tag_overflow, CX.rs_key_val_overflow);
		fprintf(f, "# uniformRsTokens,%u,uniformSamples,%llu,uniformValueChanges,%llu,droppedRsTokens,%u\n",
			CX.rs_uniform_n, CX.rs_uniform_hits, vk_census_uniform_changes(), CX.rs_drop_n);

		unsigned n = 0;
		if (sorted_rows) {
			for (unsigned i = 0; i < VKCENSUS_ROW_SLOTS; ++i) {
				if (CX.rows[i].used) sorted_rows[n++] = CX.rows[i];
			}
			qsort(sorted_rows, n, sizeof(VKCensusRow), vk_census_cmp_row);
			fprintf(f, "\nsection,pso_key,full_key,draws,polys,shader_bits,fvf,vs_tag,ps_tag,light_mask,prim\n");
			const unsigned limit = (n < 500) ? n : 500;
			for (unsigned i = 0; i < limit; ++i) {
				const VKCensusRow& r = sorted_rows[i];
				fprintf(f, "row,%llu,%llu,%llu,%llu,%u,%u,%u,%u,%u,%u\n",
					r.pso_key, r.full_key, r.draws, r.polys,
					r.shader_bits, r.fvf, r.vs_tag, r.ps_tag, r.light_mask, r.prim);
			}
		}

		// Two more sections reuse the same scratch set table (it is overwritten, never aliased).
		for (int which = 0; which < 2; ++which) {
			VKCensusSet* src = which ? CX.rt_set : CX.stage_set;
			if (!sorted_set || !src) continue;
			n = 0;
			for (unsigned i = 0; i < VKCENSUS_SET_SLOTS; ++i) {
				if (src[i].count) sorted_set[n++] = src[i];
			}
			qsort(sorted_set, n, sizeof(VKCensusSet), vk_census_cmp_set);
			const unsigned limit = (n < 300) ? n : 300;
			for (unsigned i = 0; i < limit; ++i) {
				fprintf(f, "%s,%llu,%llu\n", which ? "target" : "stagecombo",
					sorted_set[i].key, sorted_set[i].count);
			}
		}

		for (unsigned i = 0; i < CX.fvf_n; ++i) fprintf(f, "fvf,%u\n", CX.fvf_list[i]);
		for (unsigned i = 0; i < CX.light_n; ++i) fprintf(f, "lightmask,%u\n", CX.light_list[i]);

		// Token tables: the current value plus its name is what the state translator reads to build
		// its mapping, so the names come from the facade's own tables rather than a hand-written list.
		for (unsigned i = 0; i < CX.rs_tok_n; ++i) {
			const unsigned tok = CX.rs_tok[i];
			const unsigned cls = CX.rs_curated[tok] ? 1u : (CX.rs_uniform[tok] ? 2u : (CX.rs_drop[tok] ? 3u : 0u));
			fprintf(f, "rsstate,%u,%s,%u,%u\n", tok,
				Get_DX8_Render_State_Name((D3DRENDERSTATETYPE)tok),
				vk_census_value(RenderStates[tok]), cls);
		}
		// Uniform-class states with their activity: writes counted at census time, and how many times the
		// value actually moved. Nonzero changes is the machine proof wedge 5 must route these to the UBO.
		for (unsigned u = 0; u < CX.rs_uniform_n; ++u) {
			const unsigned tok = CX.rs_uniform_tok[u];
			// GeneralsX @bugfix Qoder 18/10/2026 pure-Vulkan wedge 0: rs_uniform_changes is an unsigned int, so
			// %llu read eight bytes off a four-byte slot and shifted every later column of this row - the
			// "did the value actually move" proof wedge 5 was reading off the CSV was garbage.
			fprintf(f, "rsuniform,%u,%s,%08X,%llu,%u\n", tok,
				Get_DX8_Render_State_Name((D3DRENDERSTATETYPE)tok), CX.rs_uniform_last[u],
				CX.rs_uniform_writes[u], CX.rs_uniform_changes[u]);
		}
		for (unsigned u = 0; u < CX.rs_drop_n; ++u) {
			const unsigned tok = CX.rs_drop_tok[u];
			fprintf(f, "rsdrop,%u,%s,%u\n", tok,
				Get_DX8_Render_State_Name((D3DRENDERSTATETYPE)tok), vk_census_value(RenderStates[tok]));
		}
		// The distinct-value census for each pipeline-keyed state: wedge 5's variant multiplier.
		for (unsigned t = 0; t < VKCENSUS_RS_KEY_COUNT && t < VKCENSUS_RS_KEY_SLOTS; ++t) {
			const unsigned tok = VKCENSUS_RS_KEY[t];
			const unsigned char n = CX.rs_key_val_n[t];
			char values[VKCENSUS_RS_VALUE_SLOTS * 10 + 1] = {};
			size_t used = 0;
			for (unsigned i = 0; i < n && used + 12 < sizeof(values); ++i) {
				int w = snprintf(values + used, sizeof(values) - used, i ? " %08X" : "%08X", CX.rs_key_val[t][i]);
				if (w <= 0) break;
				used += (size_t)w;
			}
			fprintf(f, "rskeyvalues,%u,%s,%u,%s\n", tok,
				Get_DX8_Render_State_Name((D3DRENDERSTATETYPE)tok), n, values);
		}

		for (unsigned i = 0; i < CX.tss_tok_n; ++i) {
			const unsigned tok = CX.tss_tok[i];
			fprintf(f, "stagestate,%u,%s,%u,%u,%u\n", tok,
				Get_DX8_Texture_Stage_State_Name((D3DTEXTURESTAGESTATETYPE)tok),
				vk_census_value(TextureStageStates[0][tok]),
				CX.tss_class[tok],
				CX.tss_class[tok] == 1u ? 1u : 0u);
		}
		for (unsigned i = 0; i < CX.samp_tok_n; ++i) {
			const unsigned tok = CX.samp_tok[i];
			fprintf(f, "samplerstate,%u,%s,%u\n", tok,
				Get_DX8_Sampler_State_Name((D3DSAMPLERSTATETYPE)tok),
				vk_census_value(SamplerStates[0][tok]));
		}
		for (unsigned i = 0; i < CX.op_n; ++i) {
			fprintf(f, "op,%u,%s\n", CX.op_list[i], Get_DX8_Texture_Op_Name(CX.op_list[i]));
		}
		for (unsigned i = 0; i < CX.arg_n; ++i) {
			fprintf(f, "arg,%u,%s\n", CX.arg_list[i], Get_DX8_Texture_Arg_Name(CX.arg_list[i]));
		}
		for (unsigned i = 0; i < CX.tag_count; ++i) {
			if (CX.tags[i].handle) fprintf(f, "shaderhandle,%llu,%u\n", (unsigned long long)CX.tags[i].handle, CX.tags[i].tag);
		}

		fclose(f);
	}

	// The gate line: everything wedge 5/6/7 commits to is bounded by these numbers.
	{
		const double collapse = CX.distinct_pso
			? (double)CX.distinct_rows / (double)CX.distinct_pso : 0.0;
		fprintf(stderr,
			"VKCENSUS reason=%s draws=%llu polys=%llu psoKeys=%llu fullKeys=%llu collapse=%.2f "
			"stageCombos=%llu targets=%llu fvfs=%u lightMasks=%u tags=%u ff=%llu prog=%llu vs=%llu ps=%llu "
			"ops=%u args=%u rsTokens=%u tssTokens=%u sampTokens=%u untrackedRs=%u untrackedHits=%llu "
			"uniformRs=%u uniformChanges=%llu droppedRs=%u rowOverflow=%llu tagOverflow=%llu "
			"keyValueOverflow=%llu\n",
			reason, CX.draws, CX.polys, CX.distinct_pso, CX.distinct_rows, collapse,
			CX.distinct_stage, CX.distinct_rt, CX.fvf_n, CX.light_n, CX.tag_count,
			CX.ff_draws, CX.prog_draws, CX.vs_draws, CX.ps_draws,
			CX.op_n, CX.arg_n, CX.rs_tok_n, CX.tss_tok_n, CX.samp_tok_n,
			CX.rs_untracked_n, CX.rs_untracked_hits,
			CX.rs_uniform_n, vk_census_uniform_changes(), CX.rs_drop_n,
			CX.row_overflow, CX.tag_overflow, CX.rs_key_val_overflow);
		fflush(stderr);
	}

	free(sorted_rows);
	free(sorted_set);
}

bool								DX8Wrapper::FogEnable									= false;
D3DCOLOR							DX8Wrapper::FogColor										= 0;

IDirect3D9 *					DX8Wrapper::D3DInterface								= nullptr;
IDirect3DDevice9 *			DX8Wrapper::D3DDevice									= nullptr;
IDirect3DSurface9 *			DX8Wrapper::CurrentRenderTarget						= nullptr;
IDirect3DSurface9 *			DX8Wrapper::CurrentDepthBuffer						= nullptr;
IDirect3DSurface9 *			DX8Wrapper::DefaultRenderTarget						= nullptr;
IDirect3DSurface9 *			DX8Wrapper::DefaultDepthBuffer						= nullptr;
bool								DX8Wrapper::IsRenderToTexture							= false;

unsigned							DX8Wrapper::_MainThreadID								= 0;
bool								DX8Wrapper::CurrentDX8LightEnables[8];	// GeneralsX @feature PS3.0-P3-18 14/10/2026: 4 -> 8 (light units)
bool								DX8Wrapper::IsDeviceLost;
int								DX8Wrapper::ZBias;
// GeneralsX @feature PS3.0 batch7 13/10/2026 hardware depth bias mirror values, see DX8Wrapper::Set_Depth_Bias
float							DX8Wrapper::DepthBias=0.0f;
float							DX8Wrapper::SlopeScaleDepthBias=0.0f;
float								DX8Wrapper::ZNear;
float								DX8Wrapper::ZFar;
// GeneralsX @refactor BenderAI 10/02/2026 - Use Matrix4x4 not D3DMATRIX (fighter19 pattern)
Matrix4x4						DX8Wrapper::ProjectionMatrix;
Matrix4x4						DX8Wrapper::DX8Transforms[D3DTS_WORLD+1];

DX8Caps*							DX8Wrapper::CurrentCaps = nullptr;

// GeneralsX @refactor PS3.0-P3-33 14/10/2026 removed the dead "too few polygons" batch-disable
// hack (DX8Wrapper::DrawPolygonLowBoundLimit): no code ever set the limit, so the counter stayed
// 0 and its early-out never culled anything.

D3DADAPTER_IDENTIFIER9		DX8Wrapper::CurrentAdapterIdentifier;

unsigned long DX8Wrapper::FrameCount = 0;

bool								_DX8SingleThreaded										= false;

static DynamicVectorClass<StringClass>					_RenderDeviceNameTable;
static DynamicVectorClass<StringClass>					_RenderDeviceShortNameTable;
static DynamicVectorClass<RenderDeviceDescClass>	_RenderDeviceDescriptionTable;


typedef IDirect3D9* (WINAPI *Direct3DCreate9Type) (UINT SDKVersion);
Direct3DCreate9Type	Direct3DCreate9Ptr = nullptr;
HINSTANCE D3D9Lib = nullptr;

DX8_CleanupHook	 *DX8Wrapper::m_pCleanupHook=nullptr;
#ifdef EXTENDED_STATS
DX8_Stats	 DX8Wrapper::stats;
#endif
// Called once per frame from W3DDisplay::draw(). Checks if the window size
// changed since last frame and reconfigures pillarbox accordingly.
void DX8Wrapper::Pillarbox_Process_Resize()
{
	if (!D3DDevice) return;

	int physW = 0, physH = 0;
	float density = 1.0f;
	if (!GetWindowSize(physW, physH, density)) {
		if (!IsWindowed) {
			// GeneralsX @bugfix GitHub Copilot 27/04/2026 SDL fullscreen transitions can change window pixel size
			// after device creation. Fall back to native display size so we can re-sync the backbuffer.
			if (!GetNativeDisplaySize(physW, physH, density)) {
				return;
			}
		}
		else {
			return;
		}
	}

	// Nothing to do if backbuffer already matches window
	if ((int)_PresentParameters.BackBufferWidth == physW &&
		(int)_PresentParameters.BackBufferHeight == physH) return;

	Pillarbox_Cleanup();
	_PresentParameters.BackBufferWidth = physW;
	_PresentParameters.BackBufferHeight = physH;

	if (!Reset_Device()) {
		_PresentParameters.BackBufferWidth = ResolutionWidth;
		_PresentParameters.BackBufferHeight = ResolutionHeight;
		Reset_Device();
		return;
	}

	Pillarbox_Setup(ResolutionWidth, ResolutionHeight);
}

/***********************************************************************************
**
** DX8Wrapper Implementation
**
***********************************************************************************/

void Log_DX8_ErrorCode(unsigned res)
{
	char tmp[256]="";

	HRESULT new_res=D3DXGetErrorStringA(
		res,
		tmp,
		sizeof(tmp));

	if (new_res==D3D_OK) {
		WWDEBUG_SAY((tmp));
	}

	WWASSERT(0);
}

void Non_Fatal_Log_DX8_ErrorCode(unsigned res,const char * file,int line)
{
	char tmp[256]="";

	HRESULT new_res=D3DXGetErrorStringA(
		res,
		tmp,
		sizeof(tmp));

	if (new_res==D3D_OK) {
		WWDEBUG_SAY(("DX8 Error: %s, File: %s, Line: %d",tmp,file,line));
	}
}

// TheSuperHackers @info helmutbuhler 14/04/2025
// Helper function that moves x and y such that the inner rect fits into the outer rect.
// If the inner rect already is in the outer rect, then this does nothing.
// If the inner rect is larger than the outer rect, then the inner rect will be aligned to the top left of the outer rect.
void MoveRectIntoOtherRect(const RECT& inner, const RECT& outer, int* x, int* y)
{
	int dx = 0;
	if (inner.right > outer.right)
		dx = outer.right-inner.right;
	if (inner.left < outer.left)
		dx = outer.left-inner.left;

	int dy = 0;
	if (inner.bottom > outer.bottom)
		dy = outer.bottom-inner.bottom;
	if (inner.top < outer.top)
		dy = outer.top-inner.top;

	*x += dx;
	*y += dy;
}


bool DX8Wrapper::Init(void * hwnd, bool lite)
{
	WWASSERT(!IsInitted);

	// zero memory
	memset(Textures,0,sizeof(IDirect3DBaseTexture9*)*MAX_TEXTURE_STAGES);
	memset(RenderStates,0,sizeof(unsigned)*256);
	// GeneralsX @build PS3.0 batch1 12/10/2026 the TSS cache row is 33 wide since D3DTSS_CONSTANT was
	// added; a hard-coded 32 left the last column carrying stale contents across init.
	memset(TextureStageStates,0,sizeof(TextureStageStates));
	memset(Vertex_Shader_Constants,0,sizeof(Vector4)*MAX_VERTEX_SHADER_CONSTANTS);
	memset(Pixel_Shader_Constants,0,sizeof(Vector4)*MAX_PIXEL_SHADER_CONSTANTS);
	memset(&render_state,0,sizeof(RenderStateStruct));
	memset(Shadow_Map,0,sizeof(ZTextureClass*)*MAX_SHADOW_MAPS);

	/*
	** Initialize all variables!
	*/
	_Hwnd = (HWND)hwnd;
	_MainThreadID=ThreadClass::_Get_Current_Thread_ID();
	WWDEBUG_SAY(("DX8Wrapper main thread: 0x%x",_MainThreadID));
	CurRenderDevice = -1;
	ResolutionWidth = DEFAULT_RESOLUTION_WIDTH;
	ResolutionHeight = DEFAULT_RESOLUTION_HEIGHT;
	// Initialize Render2DClass Screen Resolution
	Render2DClass::Set_Screen_Resolution( RectClass( 0, 0, ResolutionWidth, ResolutionHeight ) );
	BitDepth = DEFAULT_BIT_DEPTH;
	IsWindowed = false;
	DX8Wrapper_IsWindowed = false;

	for (int light=0;light<8;++light) CurrentDX8LightEnables[light]=false;	// GeneralsX @feature PS3.0-P3-18 14/10/2026: 4 -> 8

	//old_vertex_shader; TODO
	//old_sr_shader;
	//current_shader;

	//world_identity;
	//CurrentFogColor;

	D3DInterface = nullptr;
	D3DDevice = nullptr;

	WWDEBUG_SAY(("Reset DX8Wrapper statistics"));
	Reset_Statistics();

	Invalidate_Cached_Render_States();

	if (!lite) {
		// GeneralsX @build BenderAI 10/02/2026 - Platform-specific DLL/SO/DYLIB loading (Phase 5: macOS)
#ifdef _WIN32
		D3D9Lib = LoadLibrary("d3d9.dll");
#elif defined(__APPLE__)
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Loading libdxvk_d3d9.dylib (macOS)...\n");
		D3D9Lib = LoadLibrary("libdxvk_d3d9.dylib");
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - LoadLibrary result: %p\n", (void*)D3D9Lib);
		if (D3D9Lib == nullptr) {
			const char* error = dlerror();
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - dlerror(): %s\n", error ? error : "unknown");
		}
#else
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Loading libdxvk_d3d9.so (Linux)...\n");
		D3D9Lib = LoadLibrary("libdxvk_d3d9.so");
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - LoadLibrary result: %p\n", (void*)D3D9Lib);
		if (D3D9Lib == nullptr) {
			const char* error = dlerror();
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - dlerror(): %s\n", error ? error : "unknown");
		}
#endif

		if (D3D9Lib == nullptr) {
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - Failed to load D3D9 library\n");
			return false;	// Return false at this point if init failed
		}

		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Getting Direct3DCreate9 function pointer...\n");
		Direct3DCreate9Ptr = (Direct3DCreate9Type) GetProcAddress(D3D9Lib, "Direct3DCreate9");
		if (Direct3DCreate9Ptr == nullptr) {
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - Failed to get Direct3DCreate9 function\n");
			return false;
		}

		/*
		** Create the D3D interface object
		*/
		WWDEBUG_SAY(("Create Direct3D9"));
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - About to call Direct3DCreate9(D3D_SDK_VERSION=%d)\n", D3D_SDK_VERSION);
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Window handle: %p\n", hwnd);
		{
			// TheSuperHackers @bugfix xezon 13/06/2025 Front load the system dbghelp.dll to prevent
			// the graphics driver from potentially loading the old game dbghelp.dll and then crashing the game process.
			DbgHelpGuard dbgHelpGuard;

			D3DInterface = Direct3DCreate9Ptr(D3D_SDK_VERSION);		// TODO: handle failure cases...
		}
		fprintf(stderr, "DEBUG: DX8Wrapper::Init() - Direct3DCreate9 returned: %p\n", (void*)D3DInterface);
		if (D3DInterface == nullptr) {
			fprintf(stderr, "ERROR: DX8Wrapper::Init() - Direct3DCreate9 returned NULL (DXVK failed to create D3D9 interface)\n");
			return(false);
		}
		IsInitted = true;

		/*
		** Enumerate the available devices
		*/
		WWDEBUG_SAY(("Enumerate devices"));
		Enumerate_Devices();
		WWDEBUG_SAY(("DX8Wrapper Init completed"));
	}

		// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 1: env-gated co-creation of the Vulkan device
	// core on the engine HWND. D3D9 keeps presenting; this proves lifecycle coexistence only.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	if (!lite) {
		VulkanCore_Try_Init(_Hwnd);
	}
#endif

	return(true);
}

void DX8Wrapper::Shutdown()
{
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0: final census dump while the shadow state is
	// still intact (the periodic dumps mean a killed run still leaves data).
	VK_Census_Dump("shutdown");

	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 1: tear the co-created Vulkan core down first.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	VulkanCore_Shutdown();
#endif
	if (D3DDevice) {

		Set_Render_Target ((IDirect3DSurface9 *)nullptr);
		Release_Device();
	}

	if (D3DInterface) {
		D3DInterface->Release();
		D3DInterface=nullptr;

	}

	// GeneralsX @bugfix PS3.0 batch3 12/10/2026 release the whole texture cache, not just the
	// fixed-function per-pass count. Set_DX8_Texture AddRefs into Textures[] for every stage below
	// MAX_TEXTURE_STAGES, so bounding the release loop by Get_Max_Textures_Per_Pass() leaked the
	// references held at the higher stages (which programmable shader passes do use).
	for (int i = 0; i < (int)MAX_TEXTURE_STAGES; i++)
	{
		if (Textures[i])
		{
			Textures[i]->Release();
			Textures[i] = nullptr;
		}
	}

	// GeneralsX @bugfix 13/10/2026 Do NOT unload the D3D9 front-end here.
	// shutdownAll() (GameEngine.cpp) is not the last word on texture destruction: TextureClass
	// instances still alive afterwards (W3DDisplay's asset manager teardown, and anything released
	// later still, such as Drawable::killStaticImages()) run ~TextureBaseClass, which calls the COM
	// virtual D3DTexture->Release() (texture.cpp:138). Freeing this library here left that vtable
	// pointing into an unmapped image => access violation on quit, and the crash dump itself froze
	// the process for 15-21s (Full minidump), which is what the user experienced as "hangs forever
	// when leaving the game". The front-end stays loaded for the remaining lifetime of the process
	// and the OS reclaims it at exit. Note this unloaded-frontend window is inherited from the
	// imported baseline (it did the same with FreeLibrary(D3D8Lib)), not introduced by the D3D9 port.
	_RenderDeviceNameTable.Clear();		 // note - Delete_All() resizes the vector, causing a reallocation.  Clear is better. jba.
	_RenderDeviceShortNameTable.Clear();
	_RenderDeviceDescriptionTable.Clear();

	DX8Caps::Shutdown();
	IsInitted = false;		// 010803 srj
}

void DX8Wrapper::Do_Onetime_Device_Dependent_Inits()
{
	/*
	** Set Global render states (some of which depend on caps)
	*/
	Compute_Caps(D3DFormat_To_WW3DFormat(DisplayFormat));

   /*
	** Initialize any other subsystems inside of WW3D
	*/
	MissingTexture::_Init();
	TextureFilterClass::_Init_Filters(
		(TextureFilterClass::TextureFilterMode)WW3D::Get_Texture_Filter(),
		(TextureFilterClass::AnisotropicFilterMode)WW3D::Get_Anisotropy_Level()
	);
	TheDX8MeshRenderer.Init();
	SHD_INIT;
	BoxRenderObjClass::Init();
	VertexMaterialClass::Init();
	PointGroupClass::_Init(); // This needs the VertexMaterialClass to be initted
	ShatterSystem::Init();
	TextureLoader::Init();

	Set_Default_Global_Render_States();
}

inline DWORD F2DW(float f) { return *((unsigned*)&f); }
void DX8Wrapper::Set_Default_Global_Render_States()
{
	DX8_THREAD_ASSERT();
	const D3DCAPS9 &caps = Get_Current_Caps()->Get_DX8_Caps();

	Set_DX8_Render_State(D3DRS_RANGEFOGENABLE, (caps.RasterCaps & D3DPRASTERCAPS_FOGRANGE) ? TRUE : FALSE);
	Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
	Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_LINEAR);
	Set_DX8_Render_State(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_COLORVERTEX, TRUE);
	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_ZBIAS removed in D3D9; depth bias is emulated
	// in the projection matrix by Set_DX8_ZBias, whose ZBias member already defaults to 0 here.
	Set_DX8_Texture_Stage_State(1, D3DTSS_BUMPENVLSCALE, F2DW(1.0f));
	Set_DX8_Texture_Stage_State(1, D3DTSS_BUMPENVLOFFSET, F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT00,F2DW(1.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT01,F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT10,F2DW(0.0f));
	Set_DX8_Texture_Stage_State(0, D3DTSS_BUMPENVMAT11,F2DW(1.0f));

//	Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_CW);
	// Set dither mode here?
}

void DX8Wrapper::Invalidate_Cached_Render_States()
{
	render_state_changed=0;

	int a;
	for (a=0;a<sizeof(RenderStates)/sizeof(unsigned);++a) {
		RenderStates[a]=0x12345678;
	}
	for (a=0;a<MAX_TEXTURE_STAGES;++a)
	{
		for (int b=0; b<33;b++)
		{
			TextureStageStates[a][b]=0x12345678;
		}
		// GeneralsX @build d3d9 port 11/10/2026 - sampler cache must be invalidated alongside TSS cache
		for (int b=0; b<16;b++)
		{
			SamplerStates[a][b]=0x12345678;
		}
		//Need to explicitly set texture to null, otherwise app will not be able to
		//set it to null because of redundant state checker. MW
		if (_Get_D3D_Device8())
			_Get_D3D_Device8()->SetTexture(a,nullptr);
		if (Textures[a] != nullptr) {
			Textures[a]->Release();
		}
		Textures[a]=nullptr;
	}

	// GeneralsX @bugfix PS3.0 batch1 12/10/2026 the shader-handle caches were missing here. After a
	// device Reset the bindings are gone but Vertex_Shader/Pixel_Shader/Vertex_Declaration still held
	// the old values, so a shader re-bound to the same (or a recycled) address took the setters'
	// "unchanged" fast path and the device silently kept running with no shader at all. Clear the
	// cache and push the matching null so cache and device leave Reset in agreement.
	Vertex_Shader=0;
	Pixel_Shader=0;
	Vertex_Declaration=nullptr;
	if (_Get_D3D_Device8()) {
		_Get_D3D_Device8()->SetVertexShader(nullptr);
		_Get_D3D_Device8()->SetPixelShader(nullptr);
		_Get_D3D_Device8()->SetVertexDeclaration(nullptr);
	}

	ShaderClass::Invalidate();

	//Need to explicitly set render_state texture pointers to null. MW
	Release_Render_State();

	// (gth) clear the matrix shadows too
	memset(&DX8Transforms, 0, sizeof(DX8Transforms));
}

void DX8Wrapper::Set_Transform_Dirty()
{
	memset(&DX8Transforms, 0, sizeof(DX8Transforms));
	render_state_changed |= (unsigned)(WORLD_CHANGED | VIEW_CHANGED);
}

void DX8Wrapper::Do_Onetime_Device_Dependent_Shutdowns()
{
	/*
	** Shutdown ww3d systems
	*/
	int i;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		if (render_state.vertex_buffers[i]) render_state.vertex_buffers[i]->Release_Engine_Ref();
		REF_PTR_RELEASE(render_state.vertex_buffers[i]);
	}
	if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();
	REF_PTR_RELEASE(render_state.index_buffer);
	REF_PTR_RELEASE(render_state.material);
	// GeneralsX @bugfix PS3.0 batch3 12/10/2026 same reason as the Textures[] cache release above: the
	// snapshot struct has MAX_TEXTURE_STAGES slots, so releasing only Get_Max_Textures_Per_Pass() of
	// them leaks whatever a programmable shader pass left in the upper slots.
	for (i=0;i<(int)MAX_TEXTURE_STAGES;++i) REF_PTR_RELEASE(render_state.Textures[i]);


	TextureLoader::Deinit();
	SortingRendererClass::Deinit();
	DynamicVBAccessClass::_Deinit();
	DynamicIBAccessClass::_Deinit();
	ShatterSystem::Shutdown();
	PointGroupClass::_Shutdown();
	VertexMaterialClass::Shutdown();
	BoxRenderObjClass::Shutdown();
	SHD_SHUTDOWN;
	TheDX8MeshRenderer.Shutdown();
	MissingTexture::_Deinit();

	delete CurrentCaps;
	CurrentCaps=nullptr;

}


bool DX8Wrapper::Create_Device()
{
	WWASSERT(D3DDevice==nullptr);	// for now, once you've created a device, you're stuck with it!

	D3DCAPS9 caps;
	if
	(
		FAILED
		(
			D3DInterface->GetDeviceCaps
			(
				CurRenderDevice,
				WW3D_DEVTYPE,
				&caps
			)
		)
	)
	{
		return false;
	}

	::ZeroMemory(&CurrentAdapterIdentifier, sizeof(D3DADAPTER_IDENTIFIER9));

	if
	(
		FAILED
		(
			D3DInterface->GetAdapterIdentifier
			(
				CurRenderDevice,
				0,	// GeneralsX @build d3d9 port 11/10/2026 - D3DENUM_NO_WHQL_LEVEL removed; D3D9 flags param takes 0
				&CurrentAdapterIdentifier
			)
			)
	)
	{
		return false;
	}

	Vertex_Processing_Behavior=(caps.DevCaps&D3DDEVCAPS_HWTRANSFORMANDLIGHT) ?
		D3DCREATE_MIXED_VERTEXPROCESSING : D3DCREATE_SOFTWARE_VERTEXPROCESSING;

	// GeneralsX @feature PS3.0-P3-33 14/10/2026 I10 evaluation: MIXED keeps the DX8-era per-shader
	// vertex-processing switch, which shows up as pipeline boundaries under DXVK. GENERALSX_HWVP=1
	// asks for D3DCREATE_HARDWARE_VERTEXPROCESSING on devices that advertise HW T&L; the screen-space
	// RHW quads (bloom/water filter passes) are not affected by the choice. Default stays retail MIXED.
	{
		const char* hwvp = getenv("GENERALSX_HWVP");
		if ((caps.DevCaps & D3DDEVCAPS_HWTRANSFORMANDLIGHT) && hwvp != nullptr && atoi(hwvp) != 0)
		{
			Vertex_Processing_Behavior = D3DCREATE_HARDWARE_VERTEXPROCESSING;
			fprintf(stderr, "[HWVP] hardware vertex processing requested at device creation\n");
			fflush(stderr);
		}
	}

	// enable this when all 'get' dx calls are removed KJM
	/*if (caps.DevCaps&D3DDEVCAPS_PUREDEVICE)
	{
		Vertex_Processing_Behavior|=D3DCREATE_PUREDEVICE;
	}*/

#ifdef CREATE_DX8_MULTI_THREADED
	Vertex_Processing_Behavior|=D3DCREATE_MULTITHREADED;
	_DX8SingleThreaded=false;
#else
	_DX8SingleThreaded=true;
#endif

	if (DX8Wrapper_PreserveFPU)
		Vertex_Processing_Behavior |= D3DCREATE_FPU_PRESERVE;

#ifdef CREATE_DX8_FPU_PRESERVE
	Vertex_Processing_Behavior|=D3DCREATE_FPU_PRESERVE;
#endif

	// TheSuperHackers @bugfix xezon 13/06/2025 Front load the system dbghelp.dll to prevent
	// the graphics driver from potentially loading the old game dbghelp.dll and then crashing the game process.
	DbgHelpGuard dbgHelpGuard;

	HRESULT hr=D3DInterface->CreateDevice
	(
		CurRenderDevice,
		WW3D_DEVTYPE,
		_Hwnd,
		Vertex_Processing_Behavior,
		&_PresentParameters,
		&D3DDevice
	);

	if (FAILED(hr))
	{
		// The device selection may fail because the device lied that it supports 32 bit zbuffer with 16 bit
		// display. This happens at least on Voodoo2.

		if ((_PresentParameters.BackBufferFormat==D3DFMT_R5G6B5 ||
			_PresentParameters.BackBufferFormat==D3DFMT_X1R5G5B5 ||
			_PresentParameters.BackBufferFormat==D3DFMT_A1R5G5B5) &&
			(_PresentParameters.AutoDepthStencilFormat==D3DFMT_D32 ||
			_PresentParameters.AutoDepthStencilFormat==D3DFMT_D24S8 ||
			_PresentParameters.AutoDepthStencilFormat==D3DFMT_D24X8))
		{
			_PresentParameters.AutoDepthStencilFormat=D3DFMT_D16;
			hr = D3DInterface->CreateDevice
			(
				CurRenderDevice,
				WW3D_DEVTYPE,
				_Hwnd,
				Vertex_Processing_Behavior,
				&_PresentParameters,
				&D3DDevice
			);

			if (FAILED(hr))
			{
				return false;
			}
        }
		else
		{
				return false;
		}
	}

	dbgHelpGuard.deactivate();

	/*
	** Initialize all subsystems
	*/
	Do_Onetime_Device_Dependent_Inits();
	return true;
}

bool DX8Wrapper::Reset_Device(bool reload_assets)
{
	WWDEBUG_SAY(("Resetting device."));
	DX8_THREAD_ASSERT();
	if ((IsInitted) && (D3DDevice != nullptr)) {
		// Release all non-MANAGED stuff
		WW3D::_Invalidate_Textures();

		for (unsigned i=0;i<MAX_VERTEX_STREAMS;++i)
		{
			Set_Vertex_Buffer (nullptr,i);
		}
		Set_Index_Buffer (nullptr, 0);
		if (m_pCleanupHook) {
			m_pCleanupHook->ReleaseResources();
		}
		DynamicVBAccessClass::_Deinit();
		DynamicIBAccessClass::_Deinit();
		DX8TextureManagerClass::Release_Textures();
		SHD_SHUTDOWN_SHADERS;

		// Reset frame count to reflect the flipping chain being reset by Reset()
		FrameCount = 0;

		memset(Vertex_Shader_Constants,0,sizeof(Vector4)*MAX_VERTEX_SHADER_CONSTANTS);
		memset(Pixel_Shader_Constants,0,sizeof(Vector4)*MAX_PIXEL_SHADER_CONSTANTS);

		HRESULT hr=_Get_D3D_Device8()->TestCooperativeLevel();
		if (hr != D3DERR_DEVICELOST )
		{	DX8CALL_HRES(Reset(&_PresentParameters),hr)
			if (hr != D3D_OK)
				return false;	//reset failed.
		}
		else
			return false;	//device is lost and can't be reset.

		if (reload_assets)
		{
			DX8TextureManagerClass::Recreate_Textures();
			if (m_pCleanupHook) {
				m_pCleanupHook->ReAcquireResources();
			}
		}
		Invalidate_Cached_Render_States();
		Set_Default_Global_Render_States();
		SHD_INIT_SHADERS;
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 2: keep the Vulkan swapchain in sync.
		VulkanCore_Resize();
#endif
		WWDEBUG_SAY(("Device reset completed"));
		return true;
	}
	WWDEBUG_SAY(("Device reset failed"));
	return false;
}

void DX8Wrapper::Release_Device()
{
	Pillarbox_Cleanup();

	if (D3DDevice) {

		for (int a=0;a<MAX_TEXTURE_STAGES;++a)
		{
			//release references to any textures that were used in last rendering call
			DX8CALL(SetTexture(a,nullptr));
		}

		DX8CALL(SetStreamSource(0, nullptr, 0, 0));	//release reference count on last rendered vertex buffer
		DX8CALL(SetIndices(nullptr));	//release reference count on last rendered index buffer


		/*
		** Release the current vertex and index buffers
		*/
		for (unsigned i=0;i<MAX_VERTEX_STREAMS;++i)
		{
			if (render_state.vertex_buffers[i]) render_state.vertex_buffers[i]->Release_Engine_Ref();
			REF_PTR_RELEASE(render_state.vertex_buffers[i]);
		}
		if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();
		REF_PTR_RELEASE(render_state.index_buffer);

		/*
		** Shutdown all subsystems
		*/
		Do_Onetime_Device_Dependent_Shutdowns();

		/*
		** Release the device
		*/

		D3DDevice->Release();
		D3DDevice=nullptr;
	}
}

void DX8Wrapper::Enumerate_Devices()
{
	DX8_Assert();

	int adapter_count = D3DInterface->GetAdapterCount();
	for (int adapter_index=0; adapter_index<adapter_count; adapter_index++) {

		D3DADAPTER_IDENTIFIER9 id;
		::ZeroMemory(&id, sizeof(D3DADAPTER_IDENTIFIER9));
		HRESULT res = D3DInterface->GetAdapterIdentifier(adapter_index,0,&id);	// GeneralsX @build d3d9 port 11/10/2026 - D3DENUM_NO_WHQL_LEVEL removed; flags param takes 0

		if (res == D3D_OK) {

			/*
			** Set up the render device description
			** TODO: Fill in more fields of the render device description?  (need some lookup tables)
			*/
			RenderDeviceDescClass desc;
			desc.set_device_name(id.Description);
			desc.set_driver_name(id.Driver);

			char buf[64];
			sprintf(buf,"%d.%d.%d.%d", //"%04x.%04x.%04x.%04x",
				HIWORD(id.DriverVersion.HighPart),
				LOWORD(id.DriverVersion.HighPart),
				HIWORD(id.DriverVersion.LowPart),
				LOWORD(id.DriverVersion.LowPart));

			desc.set_driver_version(buf);

			D3DInterface->GetDeviceCaps(adapter_index,WW3D_DEVTYPE,&desc.Caps);
			D3DInterface->GetAdapterIdentifier(adapter_index,0,&desc.AdapterIdentifier);	// GeneralsX @build d3d9 port 11/10/2026 - D3DENUM_NO_WHQL_LEVEL removed; flags param takes 0

			DX8Caps dx8caps(D3DInterface,desc.Caps,WW3D_FORMAT_UNKNOWN,desc.AdapterIdentifier);

			/*
			** Enumerate the resolutions
			** GeneralsX @build d3d9 port 11/10/2026 - D3D9 enumerates modes per format:
			** GetAdapterModeCount(adapter, format) / EnumAdapterModes(adapter, format, index, &mode)
			*/
			desc.reset_resolution_list();
			static const D3DFORMAT enumFormats[] = { D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R8G8B8, D3DFMT_R5G6B5, D3DFMT_X1R5G5B5 };
			for (unsigned f = 0; f < sizeof(enumFormats)/sizeof(enumFormats[0]); ++f) {
				int bits = (enumFormats[f] == D3DFMT_R5G6B5 || enumFormats[f] == D3DFMT_X1R5G5B5) ? 16 : 32;
				int mode_count = D3DInterface->GetAdapterModeCount(adapter_index, enumFormats[f]);
				for (int mode_index=0; mode_index<mode_count; mode_index++) {
					D3DDISPLAYMODE d3dmode;
					::ZeroMemory(&d3dmode, sizeof(D3DDISPLAYMODE));
					HRESULT res = D3DInterface->EnumAdapterModes(adapter_index, enumFormats[f], mode_index, &d3dmode);

					if (res == D3D_OK) {
						// Some cards fail in certain modes, DX8Caps keeps list of those.
						if (!dx8caps.Is_Valid_Display_Format(d3dmode.Width,d3dmode.Height,D3DFormat_To_WW3DFormat(d3dmode.Format))) {
							bits=0;
						}

						/*
						** If we recognize the format, add it to the list
						*/
						if (bits != 0) {
							desc.add_resolution(d3dmode.Width,d3dmode.Height,bits);
						}
					}
				}
			}

			// IML: If the device has one or more valid resolutions add it to the device list.
			// NOTE: Testing has shown that there are drivers with zero resolutions.
			if (desc.Enumerate_Resolutions().Count() > 0) {

				/*
				** Set up the device name
				*/
				StringClass device_name(id.Description,true);
				_RenderDeviceNameTable.Add(device_name);
				_RenderDeviceShortNameTable.Add(device_name);	// for now, just add the same name to the "pretty name table"

				/*
				** Add the render device to our table
				*/
				_RenderDeviceDescriptionTable.Add(desc);
			}
		}
	}
}

bool DX8Wrapper::Set_Any_Render_Device()
{
	// Try fullscreen first
	int dev_number = 0;
	for (; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if (Set_Render_Device(dev_number,-1,-1,-1,0,false)) {
			return true;
		}
	}

	// Then windowed
	for (dev_number = 0; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if (Set_Render_Device(dev_number,-1,-1,-1,1,false)) {
			return true;
		}
	}

	return false;
}

bool DX8Wrapper::Set_Render_Device
(
	const char * dev_name,
	int width,
	int height,
	int bits,
	int windowed,
	bool resize_window
)
{
	for ( int dev_number = 0; dev_number < _RenderDeviceNameTable.Count(); dev_number++) {
		if ( strcmp( dev_name, _RenderDeviceNameTable[dev_number]) == 0) {
			return Set_Render_Device( dev_number, width, height, bits, windowed, resize_window );
		}

		if ( strcmp( dev_name, _RenderDeviceShortNameTable[dev_number]) == 0) {
			return Set_Render_Device( dev_number, width, height, bits, windowed, resize_window );
		}
	}
	return false;
}

void DX8Wrapper::Get_Format_Name(unsigned int format, StringClass *tex_format)
{
		*tex_format="Unknown";
		switch (format) {
		case D3DFMT_A8R8G8B8: *tex_format="D3DFMT_A8R8G8B8"; break;
		case D3DFMT_R8G8B8: *tex_format="D3DFMT_R8G8B8"; break;
		case D3DFMT_A4R4G4B4: *tex_format="D3DFMT_A4R4G4B4"; break;
		case D3DFMT_A1R5G5B5: *tex_format="D3DFMT_A1R5G5B5"; break;
		case D3DFMT_R5G6B5: *tex_format="D3DFMT_R5G6B5"; break;
		case D3DFMT_L8: *tex_format="D3DFMT_L8"; break;
		case D3DFMT_A8: *tex_format="D3DFMT_A8"; break;
		case D3DFMT_P8: *tex_format="D3DFMT_P8"; break;
		case D3DFMT_X8R8G8B8: *tex_format="D3DFMT_X8R8G8B8"; break;
		case D3DFMT_X1R5G5B5: *tex_format="D3DFMT_X1R5G5B5"; break;
		case D3DFMT_R3G3B2: *tex_format="D3DFMT_R3G3B2"; break;
		case D3DFMT_A8R3G3B2: *tex_format="D3DFMT_A8R3G3B2"; break;
		case D3DFMT_X4R4G4B4: *tex_format="D3DFMT_X4R4G4B4"; break;
		case D3DFMT_A8P8: *tex_format="D3DFMT_A8P8"; break;
		case D3DFMT_A8L8: *tex_format="D3DFMT_A8L8"; break;
		case D3DFMT_A4L4: *tex_format="D3DFMT_A4L4"; break;
		case D3DFMT_V8U8: *tex_format="D3DFMT_V8U8"; break;
		case D3DFMT_L6V5U5: *tex_format="D3DFMT_L6V5U5"; break;
		case D3DFMT_X8L8V8U8: *tex_format="D3DFMT_X8L8V8U8"; break;
		case D3DFMT_Q8W8V8U8: *tex_format="D3DFMT_Q8W8V8U8"; break;
		case D3DFMT_V16U16: *tex_format="D3DFMT_V16U16"; break;
		// GeneralsX @build d3d9 port 11/10/2026 - D3DFMT_W11V11U10 removed in D3D9
		case D3DFMT_UYVY: *tex_format="D3DFMT_UYVY"; break;
		case D3DFMT_YUY2: *tex_format="D3DFMT_YUY2"; break;
		case D3DFMT_DXT1: *tex_format="D3DFMT_DXT1"; break;
		case D3DFMT_DXT2: *tex_format="D3DFMT_DXT2"; break;
		case D3DFMT_DXT3: *tex_format="D3DFMT_DXT3"; break;
		case D3DFMT_DXT4: *tex_format="D3DFMT_DXT4"; break;
		case D3DFMT_DXT5: *tex_format="D3DFMT_DXT5"; break;
		case D3DFMT_D16_LOCKABLE: *tex_format="D3DFMT_D16_LOCKABLE"; break;
		case D3DFMT_D32: *tex_format="D3DFMT_D32"; break;
		case D3DFMT_D15S1: *tex_format="D3DFMT_D15S1"; break;
		case D3DFMT_D24S8: *tex_format="D3DFMT_D24S8"; break;
		case D3DFMT_D16: *tex_format="D3DFMT_D16"; break;
		case D3DFMT_D24X8: *tex_format="D3DFMT_D24X8"; break;
		case D3DFMT_D24X4S4: *tex_format="D3DFMT_D24X4S4"; break;
		default:	break;
		}
}

void DX8Wrapper::Resize_And_Position_Window()
{
	// Get the current dimensions of the 'render area' of the window
	RECT rect = { 0 };
	::GetClientRect (_Hwnd, &rect);

	// Is the window the correct size for this resolution?
	if ((rect.right-rect.left) != ResolutionWidth ||
			(rect.bottom-rect.top) != ResolutionHeight) {

		// Calculate what the main window's bounding rectangle should be to
		// accommodate this resolution
		rect.left = 0;
		rect.top = 0;
		rect.right = ResolutionWidth;
		rect.bottom = ResolutionHeight;
		DWORD dwstyle = ::GetWindowLong (_Hwnd, GWL_STYLE);
		AdjustWindowRect (&rect, dwstyle, FALSE);
		int width = rect.right-rect.left;
		int height = rect.bottom-rect.top;

		// Resize the window to fit this resolution
		if (!IsWindowed)
		{
			#ifdef _WIN32
			::SetWindowPos(_Hwnd, HWND_TOPMOST, 0, 0, width, height, 0);

			DEBUG_LOG(("Window resized to w:%d h:%d", width, height));
			#else
			// GeneralsX @bugfix GitHub Copilot 27/04/2026 On SDL platforms, fullscreen size is controlled by SDL display mode.
			// Forcing window size here can lock fullscreen transitions to the game resolution (for example 1024x768).
			DEBUG_LOG(("Skipping explicit fullscreen resize on SDL platform"));
			#endif
		}
		else
		{
#ifdef _WIN32
			// TheSuperHackers @feature helmutbuhler 14/04/2025
			// Center the window in the workarea of the monitor it is on.
			MONITORINFO mi = {sizeof(MONITORINFO)};
			GetMonitorInfo(MonitorFromWindow(_Hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
			int left = (mi.rcWork.left + mi.rcWork.right - width) / 2;
			int top  = (mi.rcWork.top + mi.rcWork.bottom - height) / 2;

			// TheSuperHackers @feature helmutbuhler 14/04/2025
			// Move the window to try fit it into the monitor area, if one of its dimensions is larger than the work area.
			// Otherwise align the window to the top left edges, if it is even larger than the monitor area.
			RECT rectClient;
			rectClient.left = left - rect.left;
			rectClient.top = top - rect.top;
			rectClient.right = rectClient.left + ResolutionWidth;
			rectClient.bottom = rectClient.top + ResolutionHeight;
			MoveRectIntoOtherRect(rectClient, mi.rcMonitor, &left, &top);

			::SetWindowPos (_Hwnd, nullptr, left, top, width, height, SWP_NOZORDER);

			DEBUG_LOG(("Window positioned to x:%d y:%d, resized to w:%d h:%d", left, top, width, height));
#else
			// GeneralsX @build BenderAI 10/02/2026 - SDL3 window management on Linux (no Monitor API needed)
			::SetWindowPos (_Hwnd, nullptr, 0, 0, width, height, SWP_NOZORDER);

			DEBUG_LOG(("Window resized (Linux/SDL) to w:%d h:%d", width, height));
#endif
		}
	}
}

bool DX8Wrapper::Set_Render_Device(int dev, int width, int height, int bits, int windowed,
								   bool resize_window,bool reset_device, bool restore_assets)
{
	WWASSERT(IsInitted);
	WWASSERT(dev >= -1);
	WWASSERT(dev < _RenderDeviceNameTable.Count());

	/*
	** If user has never selected a render device, start out with device 0
	*/
	if ((CurRenderDevice == -1) && (dev == -1)) {
		CurRenderDevice = 0;
	} else if (dev != -1) {
		CurRenderDevice = dev;
	}

	/*
	** If user doesn't want to change res, set the res variables to match the
	** current resolution
	*/
	if (width != -1)		ResolutionWidth = width;
	if (height != -1)		ResolutionHeight = height;

	if (bits != -1)		BitDepth = bits;
	if (windowed != -1)	IsWindowed = (windowed != 0);
	DX8Wrapper_IsWindowed = IsWindowed;

	WWDEBUG_SAY(("Attempting Set_Render_Device: name: %s (%s:%s), width: %d, height: %d, windowed: %d",
		_RenderDeviceNameTable[CurRenderDevice].str(),_RenderDeviceDescriptionTable[CurRenderDevice].Get_Driver_Name(),
		_RenderDeviceDescriptionTable[CurRenderDevice].Get_Driver_Version(),ResolutionWidth,ResolutionHeight,(IsWindowed ? 1 : 0)));

	// PWG 4/13/2000 - changed so that if you say to resize the window it resizes
	// regardless of whether its windowed or not as OpenGL resizes its self around
	// the caption and edges of the window type you provide, so its important to
	// push the client area to be the size you really want.
	// if ( resize_window && windowed ) {
	if (resize_window) {
		Resize_And_Position_Window();
	}
	//must be either resetting existing device or creating a new one.
	WWASSERT(reset_device || D3DDevice == nullptr);

	/*
	** Initialize values for D3DPRESENT_PARAMETERS members.
	*/
	::ZeroMemory(&_PresentParameters, sizeof(D3DPRESENT_PARAMETERS));

	Resolve_Present_BackBuffer_Size(
		ResolutionWidth,
		ResolutionHeight,
		IsWindowed,
		_PresentParameters.BackBufferWidth,
		_PresentParameters.BackBufferHeight);
	_PresentParameters.BackBufferCount = IsWindowed ? 1 : 2;

	//I changed this to discard all the time (even when full-screen) since that the most efficient. 07-16-03 MW:
	_PresentParameters.SwapEffect = D3DSWAPEFFECT_DISCARD;//IsWindowed ? D3DSWAPEFFECT_DISCARD : D3DSWAPEFFECT_FLIP;		// Shouldn't this be D3DSWAPEFFECT_FLIP?
	_PresentParameters.hDeviceWindow = _Hwnd;
	#ifndef _WIN32
	// GeneralsX @bugfix xorza 14/04/2026 Force DXVK to use windowed presentation mode on Linux.
	// DXVK's SDL3 WSI calls SDL_SetWindowPosition during fullscreen entry which Wayland rejects.
	// SDL3 native fullscreen is applied separately after device creation (see W3DDisplay::init).
	_PresentParameters.Windowed = TRUE;
	#else
	_PresentParameters.Windowed = IsWindowed;
	#endif

	_PresentParameters.EnableAutoDepthStencil = TRUE;				// Driver will attempt to match Z-buffer depth
	_PresentParameters.Flags=0;											// We're not going to lock the backbuffer

	_PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
	_PresentParameters.FullScreen_RefreshRateInHz = D3DPRESENT_RATE_DEFAULT;

	// GeneralsX @feature PS3.0-P2-8 14/10/2026 I8a: the present interval was fixed at device creation
	// and the engine-side Set_Swap_Interval had no caller, so vsync was not configurable without a UI
	// session. GENERALSX_SWAPINTERVAL=0..3 now selects IMMEDIATE/ONE/TWO/THREE here (0 = uncapped).
	// Applied before the device is created, so no Reset_Device() is needed. The Options panel wiring
	// is left to the settings session; the engine already consumes the value.
	// GeneralsX @feature PS3.0-P3-25a 14/10/2026 that wiring landed: the device layer injects the
	// Options.ini VSync key (env still wins) through Set_User_Present_Interval before creation, and
	// it is consumed here under the same rules.
	{
		const char* env = getenv("GENERALSX_SWAPINTERVAL");
		int v = -1;
		if (env != nullptr && env[0] != 0)
			v = atoi(env);
		else if (s_UserPresentInterval >= 0)
			v = s_UserPresentInterval;
		if (v >= 0)
		{
			switch (v)
			{
				case 0: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; break;
				case 2: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_TWO; break;
				case 3: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_THREE; break;
				case 1:
				default: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE; break;
			}
			fprintf(stderr, "[SWAP] present interval override = %d\n", v);
			fflush(stderr);
		}
	}

	/*
	** Set up the buffer formats.  Several issues here:
	** - if in windowed mode, the backbuffer must use the current display format.
	** - the depth buffer must use
	*/
	if (IsWindowed) {

		D3DDISPLAYMODE desktop_mode;
		::ZeroMemory(&desktop_mode, sizeof(D3DDISPLAYMODE));
		D3DInterface->GetAdapterDisplayMode( CurRenderDevice, &desktop_mode );

		DisplayFormat=_PresentParameters.BackBufferFormat = desktop_mode.Format;

		// In windowed mode, define the bitdepth from desktop mode (as it can't be changed)
		switch (_PresentParameters.BackBufferFormat) {
		case D3DFMT_X8R8G8B8:
		case D3DFMT_A8R8G8B8:
		case D3DFMT_R8G8B8: BitDepth=32; break;
		case D3DFMT_A4R4G4B4:
		case D3DFMT_A1R5G5B5:
		case D3DFMT_R5G6B5: BitDepth=16; break;
		case D3DFMT_L8:
		case D3DFMT_A8:
		case D3DFMT_P8: BitDepth=8; break;
		default:
			// Unknown backbuffer format probably means the device can't do windowed
			return false;
		}

		if (BitDepth==32 && D3DInterface->CheckDeviceType(0,D3DDEVTYPE_HAL,desktop_mode.Format,D3DFMT_A8R8G8B8, TRUE) == D3D_OK)
		{
			//promote 32-bit modes to include destination alpha
			_PresentParameters.BackBufferFormat = D3DFMT_A8R8G8B8;
		}

		/*
		** Find a appropriate Z buffer
		*/
		if (!Find_Z_Mode(DisplayFormat,_PresentParameters.BackBufferFormat,&_PresentParameters.AutoDepthStencilFormat))
		{
			// If opening 32 bit mode failed, try 16 bit, even if the desktop happens to be 32 bit
			if (BitDepth==32) {
				BitDepth=16;
				_PresentParameters.BackBufferFormat=D3DFMT_R5G6B5;
				if (!Find_Z_Mode(_PresentParameters.BackBufferFormat,_PresentParameters.BackBufferFormat,&_PresentParameters.AutoDepthStencilFormat)) {
					_PresentParameters.AutoDepthStencilFormat=D3DFMT_UNKNOWN;
				}
			}
			else {
				_PresentParameters.AutoDepthStencilFormat=D3DFMT_UNKNOWN;
			}
		}

	} else {

		/*
		** Try to find a mode that matches the user's desired bit-depth.
		*/
		Find_Color_And_Z_Mode(ResolutionWidth,ResolutionHeight,BitDepth,&DisplayFormat,
			&_PresentParameters.BackBufferFormat,&_PresentParameters.AutoDepthStencilFormat);
	}

	/*
	** Set default for depth stencil format if auto Z buffer failed.
	*/
	if (_PresentParameters.AutoDepthStencilFormat==D3DFMT_UNKNOWN) {
		if (BitDepth==32) {
			_PresentParameters.AutoDepthStencilFormat=D3DFMT_D32;
		}
		else {
			_PresentParameters.AutoDepthStencilFormat=D3DFMT_D16;
		}
	}

	/*
	** Check the devices support for the requested MSAA mode then setup the multi sample type
	*/
	if (MultiSampleAntiAliasing > D3DMULTISAMPLE_NONE) {
		const D3DMULTISAMPLE_TYPE requestedMode = MultiSampleAntiAliasing;
		MultiSampleAntiAliasing = Normalize_MSAA_Mode(MultiSampleAntiAliasing);

		while (!Is_MSAA_Mode_Supported(
			D3DInterface,
			CurRenderDevice,
			_PresentParameters.BackBufferFormat,
			_PresentParameters.AutoDepthStencilFormat,
			IsWindowed,
			MultiSampleAntiAliasing))
		{
			MultiSampleAntiAliasing = Get_Lower_MSAA_Mode(MultiSampleAntiAliasing);
		}

		if (MultiSampleAntiAliasing != requestedMode) {
			WWDEBUG_SAY(("Requested MSAA mode %u is unsupported; using %u",
				(unsigned)requestedMode,
				(unsigned)MultiSampleAntiAliasing));
		}
	}

	_PresentParameters.MultiSampleType = MultiSampleAntiAliasing;

	// GeneralsX @feature PS3.0-P3-1 14/10/2026 I1: GENERALSX_MSAA_QUALITY picks the MSAA quality tier
	// ("max", or a 0-based index) for the mode that survived the downgrade loop above. Default stays
	// tier 0, i.e. today's behavior; the request is clamped to the levels the adapter reports and the
	// outcome is logged once so harness runs can attribute any difference.
	if (MultiSampleAntiAliasing > D3DMULTISAMPLE_NONE) {
		const char* msaaQualEnv = getenv("GENERALSX_MSAA_QUALITY");
		if (msaaQualEnv != nullptr && msaaQualEnv[0] != 0) {
			const unsigned levels = Query_MSAA_Quality_Levels(
				D3DInterface,
				CurRenderDevice,
				_PresentParameters.BackBufferFormat,
				_PresentParameters.AutoDepthStencilFormat,
				IsWindowed,
				MultiSampleAntiAliasing);
			unsigned quality = 0;
			if (levels > 0) {
				if (msaaQualEnv[0]=='m' || msaaQualEnv[0]=='M')
					quality = levels - 1;
				else {
					quality = (unsigned)atoi(msaaQualEnv);
					if (quality > levels - 1) quality = levels - 1;
				}
			}
			_PresentParameters.MultiSampleQuality = quality;
			fprintf(stderr, "[MSAA] mode=%u levels=%u quality=%u request=%s\n",
				(unsigned)MultiSampleAntiAliasing, levels, quality, msaaQualEnv);
			fflush(stderr);
		}
	}

	/*
	** Time to actually create the device.
	*/
	StringClass displayFormat;
	StringClass backbufferFormat;

	Get_Format_Name(DisplayFormat,&displayFormat);
	Get_Format_Name(_PresentParameters.BackBufferFormat,&backbufferFormat);

	WWDEBUG_SAY(("Using Display/BackBuffer Formats: %s/%s",displayFormat.str(),backbufferFormat.str()));

	bool ret;

	if (reset_device)
	{
		WWDEBUG_SAY(("DX8Wrapper::Set_Render_Device is resetting the device."));
		ret = Reset_Device(restore_assets);	//reset device without restoring data - we're likely switching out of the app.
	}
	else
		ret = Create_Device();

	WWDEBUG_SAY(("Reset/Create_Device done, reset_device=%d, restore_assets=%d", reset_device, restore_assets));

	if (ret)
	{
		Render2DClass::Set_Screen_Resolution( RectClass( 0, 0, ResolutionWidth, ResolutionHeight ) );
		Pillarbox_Setup(ResolutionWidth, ResolutionHeight);
	}

	return ret;
}

bool DX8Wrapper::Set_Next_Render_Device()
{
	int new_dev = (CurRenderDevice + 1) % _RenderDeviceNameTable.Count();
	return Set_Render_Device(new_dev);
}

bool DX8Wrapper::Toggle_Windowed()
{
#ifdef WW3D_DX8
	// State OK?
	assert (IsInitted);
	if (IsInitted) {

		// Get information about the current render device's resolutions
		const RenderDeviceDescClass &render_device = Get_Render_Device_Desc ();
		const DynamicVectorClass<ResolutionDescClass> &resolutions = render_device.Enumerate_Resolutions ();

		// Loop through all the resolutions supported by the current device.
		// If we aren't currently running under one of these resolutions,
		// then we should probably		 to the closest resolution before
		// toggling the windowed state.
		int curr_res = -1;
		for (int res = 0;
		     (res < resolutions.Count ()) && (curr_res == -1);
			  res ++) {

			// Is this the resolution we are looking for?
			if ((resolutions[res].Width == ResolutionWidth) &&
				 (resolutions[res].Height == ResolutionHeight) &&
				 (resolutions[res].BitDepth == BitDepth)) {
				curr_res = res;
			}
		}

		if (curr_res == -1) {

			// We don't match any of the standard resolutions,
			// so set the first resolution and toggle the windowed state.
			return Set_Device_Resolution (resolutions[0].Width,
								 resolutions[0].Height,
								 resolutions[0].BitDepth,
								 !IsWindowed, true);
		} else {

			// Toggle the windowed state
			return Set_Device_Resolution (-1, -1, -1, !IsWindowed, true);
		}
	}
#endif //WW3D_DX8

	return false;
}

void DX8Wrapper::Set_Swap_Interval(int swap)
{
	switch (swap) {
		case 0: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE; break;
		case 1: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE ; break;
		case 2: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_TWO; break;
		case 3: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_THREE; break;
		default: _PresentParameters.PresentationInterval = D3DPRESENT_INTERVAL_ONE ; break;
	}

	WWDEBUG_SAY(("DX8Wrapper::Set_Swap_Interval is resetting the device."));
	Reset_Device();
}

int DX8Wrapper::Get_Swap_Interval()
{
	return _PresentParameters.PresentationInterval;
}

// GeneralsX @feature PS3.0-P3-25a 14/10/2026 store-only: Set_Render_Device's creation block
// consumes this when no GENERALSX_SWAPINTERVAL env is present. Calling it after the device exists
// has no effect until the next creation (vsync changes are a restart-to-take-effect option).
void DX8Wrapper::Set_User_Present_Interval(int swap)
{
	if (swap < -1)
		swap = -1;
	if (swap > 3)
		swap = 3;
	s_UserPresentInterval = swap;
}

bool DX8Wrapper::Has_Stencil()
{
	bool has_stencil = (_PresentParameters.AutoDepthStencilFormat == D3DFMT_D24S8 ||
						_PresentParameters.AutoDepthStencilFormat == D3DFMT_D24X4S4);
	return has_stencil;
}

int DX8Wrapper::Get_Render_Device_Count()
{
	return _RenderDeviceNameTable.Count();

}
int DX8Wrapper::Get_Render_Device()
{
	assert(IsInitted);
	return CurRenderDevice;
}

const RenderDeviceDescClass & DX8Wrapper::Get_Render_Device_Desc(int deviceidx)
{
	WWASSERT(IsInitted);

	if ((deviceidx == -1) && (CurRenderDevice == -1)) {
		CurRenderDevice = 0;
	}

	// if the device index is -1 then we want the current device
	if (deviceidx == -1) {
		WWASSERT(CurRenderDevice >= 0);
		WWASSERT(CurRenderDevice < _RenderDeviceNameTable.Count());
		return _RenderDeviceDescriptionTable[CurRenderDevice];
	}

	// We can only ask for multiple device information if the devices
	// have been detected.
	WWASSERT(deviceidx >= 0);
	WWASSERT(deviceidx < _RenderDeviceNameTable.Count());
	return _RenderDeviceDescriptionTable[deviceidx];
}

const char * DX8Wrapper::Get_Render_Device_Name(int device_index)
{
	device_index = device_index % _RenderDeviceShortNameTable.Count();
	return _RenderDeviceShortNameTable[device_index];
}

bool DX8Wrapper::Set_Device_Resolution(int width,int height,int bits,int windowed, bool resize_window)
{
	if (D3DDevice != nullptr) {

		if (width != -1) ResolutionWidth = width;
		if (height != -1) ResolutionHeight = height;

		if (resize_window)
		{
			Resize_And_Position_Window();
		}

		Resolve_Present_BackBuffer_Size(
			ResolutionWidth,
			ResolutionHeight,
			IsWindowed,
			_PresentParameters.BackBufferWidth,
			_PresentParameters.BackBufferHeight);

		// Release pillarbox RT before device reset
		Pillarbox_Cleanup();
#pragma message("TODO: support changing windowed status and changing the bit depth")
		WWDEBUG_SAY(("DX8Wrapper::Set_Device_Resolution is resetting the device."));
		bool ok = Reset_Device();
		if (ok) Pillarbox_Setup(ResolutionWidth, ResolutionHeight);
		return ok;
	} else {
		return false;
	}
}

void DX8Wrapper::Get_Device_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed)
{
	WWASSERT(IsInitted);

	set_w = ResolutionWidth;
	set_h = ResolutionHeight;
	set_bits = BitDepth;
	set_windowed = IsWindowed;
}

void DX8Wrapper::Get_Render_Target_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed)
{
	WWASSERT(IsInitted);

	if (CurrentRenderTarget != nullptr) {
		D3DSURFACE_DESC info;
		CurrentRenderTarget->GetDesc (&info);

		set_w				= info.Width;
		set_h				= info.Height;
		set_bits			= BitDepth;		// should we get the actual bit depth of the target?
		set_windowed	= IsWindowed;	// this doesn't really make sense for render targets (shouldn't matter)...

	} else {
		Get_Device_Resolution (set_w, set_h, set_bits, set_windowed);
	}
}

bool DX8Wrapper::Registry_Save_Render_Device( const char * sub_key )
{
	int	width, height, depth;
	bool	windowed;
	Get_Device_Resolution(width, height, depth, windowed);
	return Registry_Save_Render_Device(sub_key, CurRenderDevice, ResolutionWidth, ResolutionHeight, BitDepth, IsWindowed, TextureBitDepth);
}

bool DX8Wrapper::Registry_Save_Render_Device( const char *sub_key, int device, int width, int height, int depth, bool windowed, int texture_depth)
{
	RegistryClass * registry = W3DNEW RegistryClass( sub_key );
	WWASSERT( registry );

	if ( !registry->Is_Valid() ) {
		delete registry;
		WWDEBUG_SAY(( "Error getting Registry" ));
		return false;
	}

	registry->Set_String( VALUE_NAME_RENDER_DEVICE_NAME,
		_RenderDeviceShortNameTable[device] );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_WIDTH,	width );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_HEIGHT, height );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_DEPTH, depth );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_WINDOWED, windowed );
	registry->Set_Int( VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH, texture_depth );

	delete registry;
	return true;
}

bool DX8Wrapper::Registry_Load_Render_Device( const char * sub_key, bool resize_window )
{
	char	name[ 200 ];
	int	width,height,depth,windowed;

	if (	Registry_Load_Render_Device(	sub_key,
													name,
													sizeof(name),
													width,
													height,
													depth,
													windowed,
													TextureBitDepth) &&
			(*name != 0))
	{
		WWDEBUG_SAY(( "Device %s (%d X %d) %d bit windowed:%d", name,width,height,depth,windowed));

		if (TextureBitDepth==16 || TextureBitDepth==32) {
//			WWDEBUG_SAY(( "Texture depth %d", TextureBitDepth));
		} else {
			WWDEBUG_SAY(( "Invalid texture depth %d, switching to 16 bits", TextureBitDepth));
			TextureBitDepth=16;
		}

		if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) != true) {
			if (depth==16) depth=32;
			else depth=16;
			if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) == true) {
				return true;
			}
			if (depth==16) depth=32;
			else depth=16;
			// we'll test resolutions down, so if start is 640, increase to begin with...
			if (width==640) {
				width=1024;
				height=768;
			}
			for(;;) {
				if (width>2048) {
					width=2048;
					height=1536;
				}
				else if (width>1920) {
					width=1920;
					height=1440;
				}
				else if (width>1600) {
					width=1600;
					height=1200;
				}
				else if (width>1280) {
					width=1280;
					height=1024;
				}
				else if (width>1024) {
					width=1024;
					height=768;
				}
				else if (width>800) {
					width=800;
					height=600;
				}
				else if (width!=640) {
					width=640;
					height=480;
				}
				else {
					return Set_Any_Render_Device();
				}
				for (int i=0;i<2;++i) {
					if ( Set_Render_Device( name, width,height,depth,windowed, resize_window ) == true) {
						return true;
					}
					if (depth==16) depth=32;
					else depth=16;
				}
			}
		}

		return true;
	}

	WWDEBUG_SAY(( "Error getting Registry" ));

	return Set_Any_Render_Device();
}

bool DX8Wrapper::Registry_Load_Render_Device( const char * sub_key, char *device, int device_len, int &width, int &height, int &depth, int &windowed, int &texture_depth)
{
	RegistryClass registry( sub_key );

	if ( registry.Is_Valid() ) {
		registry.Get_String( VALUE_NAME_RENDER_DEVICE_NAME,
			device, device_len);

		width =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_WIDTH, -1 );
		height =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_HEIGHT, -1 );
		depth =		registry.Get_Int( VALUE_NAME_RENDER_DEVICE_DEPTH, -1 );
		windowed =	registry.Get_Int( VALUE_NAME_RENDER_DEVICE_WINDOWED, -1 );
		texture_depth = registry.Get_Int( VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH, -1 );
		return true;
	}
	*device=0;
	width=-1;
	height=-1;
	depth=-1;
	windowed=-1;
	texture_depth=-1;
	return false;
}


bool DX8Wrapper::Find_Color_And_Z_Mode(int resx,int resy,int bitdepth,D3DFORMAT * set_colorbuffer,D3DFORMAT * set_backbuffer,D3DFORMAT * set_zmode)
{
	static D3DFORMAT _formats16[] =
	{
		D3DFMT_R5G6B5,
		D3DFMT_X1R5G5B5,
		D3DFMT_A1R5G5B5
	};

	static D3DFORMAT _formats32[] =
	{
		D3DFMT_A8R8G8B8,
		D3DFMT_X8R8G8B8,
		D3DFMT_R8G8B8,
	};

	/*
	** Select the table that we're going to use to search for a valid backbuffer format
	*/
	D3DFORMAT * format_table = nullptr;
	int format_count = 0;

	if (BitDepth == 16) {
		format_table = _formats16;
		format_count = sizeof(_formats16) / sizeof(D3DFORMAT);
	} else {
		format_table = _formats32;
		format_count = sizeof(_formats32) / sizeof(D3DFORMAT);
	}

	/*
	** now search for a valid format
	*/
	bool found = false;
	unsigned int mode = 0;

	int format_index=0;
	for (; format_index < format_count; format_index++) {
		found |= Find_Color_Mode(format_table[format_index],resx,resy,&mode);
		if (found) break;
	}

	if (!found) {
		return false;
	} else {
		*set_backbuffer=*set_colorbuffer = format_table[format_index];
	}

	if (bitdepth==32 && *set_colorbuffer == D3DFMT_X8R8G8B8 && D3DInterface->CheckDeviceType(0,D3DDEVTYPE_HAL,*set_colorbuffer,D3DFMT_A8R8G8B8, TRUE) == D3D_OK)
	{
		//promote 32-bit modes to include destination alpha when supported
		*set_backbuffer = D3DFMT_A8R8G8B8;
	}

	/*
	** We found a backbuffer format, now find a zbuffer format
	*/
	return Find_Z_Mode(*set_colorbuffer,*set_backbuffer, set_zmode);
};


// find the resolution mode with at least resx,resy with the highest supported
// refresh rate
bool DX8Wrapper::Find_Color_Mode(D3DFORMAT colorbuffer, int resx, int resy, UINT *mode)
{
	UINT i,j,modemax;
	UINT rx,ry;
	D3DDISPLAYMODE dmode;
	::ZeroMemory(&dmode, sizeof(D3DDISPLAYMODE));

	rx=(unsigned int) resx;
	ry=(unsigned int) resy;

	bool found=false;

	// GeneralsX @build d3d9 port 11/10/2026 - D3D9 enumerates modes per format
	modemax=D3DInterface->GetAdapterModeCount(D3DADAPTER_DEFAULT, colorbuffer);

	i=0;

	while (i<modemax && !found)
	{
		D3DInterface->EnumAdapterModes(D3DADAPTER_DEFAULT, colorbuffer, i, &dmode);
		if (dmode.Width==rx && dmode.Height==ry) {
			WWDEBUG_SAY(("Found valid color mode.  Width = %d Height = %d Format = %d",dmode.Width,dmode.Height,dmode.Format));
			found=true;
		}
		i++;
	}

	i--; // this is the first valid mode

	// no match
	if (!found) {
		WWDEBUG_SAY(("Failed to find a valid color mode"));
		return false;
	}

	// go to the highest refresh rate in this mode
	bool stillok=true;

	j=i;
	while (j<modemax && stillok)
	{
		D3DInterface->EnumAdapterModes(D3DADAPTER_DEFAULT, colorbuffer, j, &dmode);
		if (dmode.Width==rx && dmode.Height==ry)
			stillok=true; else stillok=false;
		j++;
	}

	if (stillok==false) *mode=j-2;
	else *mode=i;

	return true;
}

// Helper function to find a Z buffer mode for the colorbuffer
// Will look for greatest Z precision
bool DX8Wrapper::Find_Z_Mode(D3DFORMAT colorbuffer,D3DFORMAT backbuffer, D3DFORMAT *zmode)
{
	//MW: Swapped the next 2 tests so that Stencil modes get tested first.
	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D24S8))
	{
		*zmode=D3DFMT_D24S8;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D24S8"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D32))
	{
		*zmode=D3DFMT_D32;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D32"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D24X8))
	{
		*zmode=D3DFMT_D24X8;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D24X8"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D24X4S4))
	{
		*zmode=D3DFMT_D24X4S4;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D24X4S4"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D16))
	{
		*zmode=D3DFMT_D16;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D16"));
		return true;
	}

	if (Test_Z_Mode(colorbuffer,backbuffer,D3DFMT_D15S1))
	{
		*zmode=D3DFMT_D15S1;
		WWDEBUG_SAY(("Found zbuffer mode D3DFMT_D15S1"));
		return true;
	}

	// can't find a match
	WWDEBUG_SAY(("Failed to find a valid zbuffer mode"));
	return false;
}

bool DX8Wrapper::Test_Z_Mode(D3DFORMAT colorbuffer,D3DFORMAT backbuffer, D3DFORMAT zmode)
{
	// See if we have this mode first
	if (FAILED(D3DInterface->CheckDeviceFormat(D3DADAPTER_DEFAULT,WW3D_DEVTYPE,
		colorbuffer,D3DUSAGE_DEPTHSTENCIL,D3DRTYPE_SURFACE,zmode)))
	{
		WWDEBUG_SAY(("CheckDeviceFormat failed.  Colorbuffer format = %d  Zbufferformat = %d",colorbuffer,zmode));
		return false;
	}

	// Then see if it matches the color buffer
	if(FAILED(D3DInterface->CheckDepthStencilMatch(D3DADAPTER_DEFAULT, WW3D_DEVTYPE,
		colorbuffer,backbuffer,zmode)))
	{
		WWDEBUG_SAY(("CheckDepthStencilMatch failed.  Colorbuffer format = %d  Backbuffer format = %d Zbufferformat = %d",colorbuffer,backbuffer,zmode));
		return false;
	}
	return true;
}


void DX8Wrapper::Reset_Statistics()
{
	FrameStatistics = DX8FrameStatistics();
	LastFrameStatistics = DX8FrameStatistics();
}

void DX8Wrapper::Begin_Statistics()
{
	FrameStatistics = DX8FrameStatistics();
}

void DX8Wrapper::End_Statistics()
{
	LastFrameStatistics = FrameStatistics;
}

const DX8FrameStatistics& DX8Wrapper::Get_Last_Frame_Statistics()
{
	return LastFrameStatistics;
}

unsigned long DX8Wrapper::Get_FrameCount() {return FrameCount;}

void DX8_Assert()
{
	WWASSERT(DX8Wrapper::_Get_D3D8());
	DX8_THREAD_ASSERT();
}

void DX8Wrapper::Begin_Scene()
{
	DX8_THREAD_ASSERT();

#if ENABLE_EMBEDDED_BROWSER
	DX8WebBrowser::Update();
#endif

	DX8CALL(BeginScene());

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 wedge 5: open the Vulkan frame that this scene's batches record
	// into. It waits the frame slot's fence, resets that slot's descriptor pool and command buffer and
	// stands the scene render target up, so it belongs here rather than in the present call.
	if (VulkanCore_Present_Enabled()) {
		// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.B: the mirror the scene is drawn into is
		// the render resolution, not the window - the two differ whenever the game runs at a resolution the
		// display does not, and the present rect published by Pillarbox_Setup puts it back in the window.
		VKDraw_Set_Scene_Extent((unsigned)ResolutionWidth, (unsigned)ResolutionHeight);
		VKDraw_Begin_Frame();
	}
#endif

	// GeneralsX @build BenderAI 10/02/2026 - Embedded browser Windows-only
#ifdef _WIN32
	DX8WebBrowser::Update();
#endif
}

void DX8Wrapper::End_Scene(bool flip_frames)
{
	DX8_THREAD_ASSERT();

	// Pillarbox: restore backbuffer + blit offscreen, all within the current scene
	if (s_pillarboxActive && s_offscreenTex) {
		Pillarbox_End();
		// GeneralsX @bugfix GitHub Copilot 10/05/2026 Re-sync cached DX8 state after the pillarbox blit mutates global render state at the end of the frame.
		Invalidate_Cached_Render_States();
	}
	DX8CALL(EndScene());

	// GeneralsX @build BenderAI 10/02/2026 - Embedded browser Windows-only
#ifdef _WIN32
	DX8WebBrowser::Render(0);
#endif

	if (flip_frames) {
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 2: Vulkan swapchain presents instead of D3D9.
		if (VulkanCore_Present_Enabled()) {
			VulkanCore_Present_Frame();
			FrameCount++;
		}
		else
#endif
		{
		DX8_Assert();
		HRESULT hr;
		{
			WWPROFILE("DX8Device::Present()");
			hr=_Get_D3D_Device8()->Present(nullptr, nullptr, nullptr, nullptr);
		}

		DX8_RECORD_DX8_CALLS();

		if (SUCCEEDED(hr)) {
#ifdef EXTENDED_STATS
			if (stats.m_sleepTime) {
				::Sleep(stats.m_sleepTime);
			}
#endif
			IsDeviceLost=false;
			FrameCount++;
		}
		else {
			IsDeviceLost=true;
		}

		// If the device was lost we need to check for cooperative level and possibly reset the device
		if (hr==D3DERR_DEVICELOST) {
			hr=_Get_D3D_Device8()->TestCooperativeLevel();
			if (hr==D3DERR_DEVICENOTRESET) {
				WWDEBUG_SAY(("DX8Wrapper::End_Scene is resetting the device."));
				Reset_Device();
			}
			else {
				// Sleep it not active
				ThreadClass::Sleep_Ms(200);
			}
		}
		else {
			DX8_ErrorCode(hr);
		}
		}
	}

	// Each frame, release all of the buffers and textures.
	Set_Vertex_Buffer(nullptr);
	Set_Index_Buffer(nullptr,0);
	for (int i=0;i<CurrentCaps->Get_Max_Textures_Per_Pass();++i) Set_Texture(i,nullptr);
	Set_Material(nullptr);
}


void DX8Wrapper::Flip_To_Primary()
{
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 2: display owned by Vulkan in present mode.
	if (VulkanCore_Present_Enabled()) {
		return;
	}
#endif
	// If we are fullscreen and the current frame is odd then we need
	// to force a page flip to ensure that the first buffer in the flipping
	// chain is the one visible.
	if (!IsWindowed) {
		DX8_Assert();

		int numBuffers = (_PresentParameters.BackBufferCount + 1);
		int visibleBuffer = (FrameCount % numBuffers);
		int flipCount = ((numBuffers - visibleBuffer) % numBuffers);
		int resetAttempts = 0;

		while ((flipCount > 0) && (resetAttempts < 3)) {
			HRESULT hr = _Get_D3D_Device8()->TestCooperativeLevel();

			if (FAILED(hr)) {
				WWDEBUG_SAY(("TestCooperativeLevel Failed!"));

				if (D3DERR_DEVICELOST == hr) {
					IsDeviceLost=true;
					WWDEBUG_SAY(("DEVICELOST: Cannot flip to primary."));
					return;
				}
				IsDeviceLost=false;

				if (D3DERR_DEVICENOTRESET == hr) {
					WWDEBUG_SAY(("DEVICENOTRESET"));
					Reset_Device();
					resetAttempts++;
				}
			} else {
				WWDEBUG_SAY(("Flipping: %ld", FrameCount));
				hr = _Get_D3D_Device8()->Present(nullptr, nullptr, nullptr, nullptr);

				if (SUCCEEDED(hr)) {
					IsDeviceLost=false;
					FrameCount++;
					WWDEBUG_SAY(("Flip to primary succeeded %ld", FrameCount));
				}
				else {
					IsDeviceLost=true;
				}
			}

			--flipCount;
		}
	}
}


//**********************************************************************************************
//! Clear current render device
/*! KM
/* 5/17/02 KM Fixed support for render to texture with depth/stencil buffers
*/
void DX8Wrapper::Clear(bool clear_color, bool clear_z_stencil, const Vector3 &color, float dest_alpha, float z, unsigned int stencil)
{
	DX8_THREAD_ASSERT();

	// If we try to clear a stencil buffer which is not there, the entire call will fail
	// KJM fixed this to get format from back buffer (incase render to texture is used)
	/*bool has_stencil = (	_PresentParameters.AutoDepthStencilFormat == D3DFMT_D15S1 ||
								_PresentParameters.AutoDepthStencilFormat == D3DFMT_D24S8 ||
								_PresentParameters.AutoDepthStencilFormat == D3DFMT_D24X4S4);*/
	bool has_stencil=false;
	IDirect3DSurface9* depthbuffer;

	_Get_D3D_Device8()->GetDepthStencilSurface(&depthbuffer);
	DX8_RECORD_DX8_CALLS();

	if (depthbuffer)
	{
		D3DSURFACE_DESC desc;
		depthbuffer->GetDesc(&desc);
		has_stencil=
		(
			desc.Format==D3DFMT_D15S1 ||
			desc.Format==D3DFMT_D24S8 ||
			desc.Format==D3DFMT_D24X4S4
		);

		// release ref
		depthbuffer->Release();
	}

	DWORD flags = 0;
	if (clear_color) flags |= D3DCLEAR_TARGET;
	if (clear_z_stencil) flags |= D3DCLEAR_ZBUFFER;
	if (clear_z_stencil && has_stencil) flags |= D3DCLEAR_STENCIL;
	if (flags)
	{
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.D: mirror the clear into the target the
		// Vulkan frame is recording. The scene pass is created with LOAD, not CLEAR, because D3D lets a
		// clear arrive after the target is set and before the first draw - vkCmdClearAttachment inside the
		// pass is the form that keeps that ordering. The colour goes over as floats because a clear value is
		// not format-packed: R,G,B,A in that order whatever the attachment's channel layout is.
		if (VKDraw_Ready()) {
			const unsigned argb = Convert_Color(color,dest_alpha);
			const float rgba[4] = {
				((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
				(argb & 0xFF) / 255.0f, ((argb >> 24) & 0xFF) / 255.0f };
			VKDraw_Clear(clear_color ? 1 : 0, clear_z_stencil ? 1 : 0, rgba, z, stencil, nullptr);
		}
#endif
		DX8CALL(Clear(0, nullptr, flags, Convert_Color(color,dest_alpha), z, stencil));
	}
}

void DX8Wrapper::Set_Viewport(CONST D3DVIEWPORT9* pViewport)
{
	DX8_THREAD_ASSERT();
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.C: Vulkan has no device viewport, so the
	// recorder has to be told what the game asked for; the shadow is read per submitted batch.
	VK_Shadow_Viewport(pViewport);
#endif
	DX8CALL(SetViewport(pViewport));
}

// ----------------------------------------------------------------------------
//
// Set vertex buffer. A reference to previous vertex buffer is released and
// this one is assigned the current vertex buffer. The DX8 vertex buffer will
// actually be set in Apply() which is called by Draw_Indexed_Triangles().
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Vertex_Buffer(const VertexBufferClass* vb, unsigned stream)
{
	render_state.vba_offset=0;
	render_state.vba_count=0;
	if (render_state.vertex_buffers[stream]) {
		render_state.vertex_buffers[stream]->Release_Engine_Ref();
	}
	REF_PTR_SET(render_state.vertex_buffers[stream],const_cast<VertexBufferClass*>(vb));
	if (vb) {
		vb->Add_Engine_Ref();
		render_state.vertex_buffer_types[stream]=vb->Type();
	}
	else {
		render_state.vertex_buffer_types[stream]=BUFFER_TYPE_INVALID;
	}
	render_state_changed|=VERTEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Set index buffer. A reference to previous index buffer is released and
// this one is assigned the current index buffer. The DX8 index buffer will
// actually be set in Apply() which is called by Draw_Indexed_Triangles().
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Index_Buffer(const IndexBufferClass* ib,unsigned short index_base_offset)
{
	render_state.iba_offset=0;
	if (render_state.index_buffer) {
		render_state.index_buffer->Release_Engine_Ref();
	}
	REF_PTR_SET(render_state.index_buffer,const_cast<IndexBufferClass*>(ib));
	render_state.index_base_offset=index_base_offset;
	if (ib) {
		ib->Add_Engine_Ref();
		render_state.index_buffer_type=ib->Type();
	}
	else {
		render_state.index_buffer_type=BUFFER_TYPE_INVALID;
	}
	render_state_changed|=INDEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Set vertex buffer using dynamic access object.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Vertex_Buffer(const DynamicVBAccessClass& vba_)
{
	// Release all streams (only one stream allowed in the legacy pipeline)
	for (int i=1;i<MAX_VERTEX_STREAMS;++i) {
		DX8Wrapper::Set_Vertex_Buffer(nullptr, i);
	}

	if (render_state.vertex_buffers[0]) render_state.vertex_buffers[0]->Release_Engine_Ref();
	DynamicVBAccessClass& vba=const_cast<DynamicVBAccessClass&>(vba_);
	render_state.vertex_buffer_types[0]=vba.Get_Type();
	render_state.vba_offset=vba.VertexBufferOffset;
	render_state.vba_count=vba.Get_Vertex_Count();
	REF_PTR_SET(render_state.vertex_buffers[0],vba.VertexBuffer);
	render_state.vertex_buffers[0]->Add_Engine_Ref();
	render_state_changed|=VERTEX_BUFFER_CHANGED;
	render_state_changed|=INDEX_BUFFER_CHANGED;		// vba_offset changes so index buffer needs to be reset as well.
}

// ----------------------------------------------------------------------------
//
// Set index buffer using dynamic access object.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Set_Index_Buffer(const DynamicIBAccessClass& iba_,unsigned short index_base_offset)
{
	if (render_state.index_buffer) render_state.index_buffer->Release_Engine_Ref();

	DynamicIBAccessClass& iba=const_cast<DynamicIBAccessClass&>(iba_);
	render_state.index_base_offset=index_base_offset;
	render_state.index_buffer_type=iba.Get_Type();
	render_state.iba_offset=iba.IndexBufferOffset;
	REF_PTR_SET(render_state.index_buffer,iba.IndexBuffer);
	render_state.index_buffer->Add_Engine_Ref();
	render_state_changed|=INDEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
//
// Pure-Vulkan wedge 5.C: the facade-side submit filler (declared in dx8wrapper.h).
//
// ----------------------------------------------------------------------------

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

namespace {

	// What the filler decided, kept separately from the pipeline layer's own counters because the two fail
	// for different reasons: these are batches the facade could not describe at all, and VKDRAW's stats
	// line reports the ones it described and the layer refused.
	struct VKDrawFillCounters {
		unsigned long long emitted;
		unsigned long long notReady;
		unsigned long long noVertexBacking;
		unsigned long long noIndexBacking;
		unsigned long long programmable;	// custom vertex/pixel shader: wedge 8 owns those
		unsigned long long refused;		// the layer itself said no
		unsigned long long materialized;	// backings copied out of the D3D9 resource on first draw
		unsigned long long lastLog;
	};
	VKDrawFillCounters VKF;

	// The engine's viewport, shadowed. D3D's SetViewport is device state with no readback worth a call per
	// draw, and on Vulkan the viewport is a per-draw dynamic value the recorder has to be handed, so the
	// facade keeps it here the moment the game sets it. DX8 has no scissor render state at all (d3d8types.h
	// stops before D3DRS_SCISSORTESTENABLE), so the scissor follows the viewport, which is what D3D8's
	// viewport clipping did for the same batches.
	struct VKViewportShadow {
		unsigned x, y, w, h;
		bool     valid;
	};
	VKViewportShadow VK_VP = { 0, 0, 0, 0, false };

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A forensics: a batch whose vertex buffer has no
	// backing falls back to D3D9, and the counters say how often - but not which buffer or why. Every WW3D
	// lock path is mirrored, so a miss is either a buffer the game never wrote through those classes or an
	// allocation that failed; the two need different fixes. Record the distinct buffer objects behind the
	// misses (bounded, one line each) so the next log run names the class instead of the count.
	struct VKMissLog {
		void*	key[32];
		unsigned used;
	};
	VKMissLog VKMiss;

	void vk_report_missing_vb(VertexBufferClass* vb)
	{
		if (VKMiss.used >= 32) return;
		for (unsigned i = 0; i < VKMiss.used; ++i) if (VKMiss.key[i] == vb) return;
		VKMiss.key[VKMiss.used++] = vb;
		fprintf(stderr, "[VKMISS] vb=%p type=%d verts=%u fvf=%08X stride=%u (miss #%u)\n",
			(void*)vb, (int)vb->Type(), vb->Get_Vertex_Count(), vb->FVF_Info().Get_FVF(),
			vb->FVF_Info().Get_FVF_Size(), VKMiss.used);
		fflush(stderr);
		// The first miss is the interesting one: everything after it is the same object drawn again.
		// Dump the table it should have been in, so the log says whether the lock never ran or the
		// backing was dropped.
		if (VKMiss.used == 1u) VKGeo_Back_Dump(vb);
	}

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A: the last resort for a buffer with no
	// backing. Every WW3D lock class is mirrored, yet the lifecycle trace (vkwp16) proved the game draws at
	// least one vertex buffer that was never locked at all - born, filled and rendered with the contents
	// living only in the D3D9 resource. Rather than lose those batches to D3D9 forever (which is fatal at
	// wedge 9, when d3d9.dll is gone), materialise the backing here: create the per-object VkBuffer, read
	// the D3D9 resource once with LOCK_READONLY and copy it in. After that the mirrored lock paths own the
	// buffer, exactly as if the first lock had happened while the layer was up.
	//
	// This is a bridge, not a fix: it also covers the ~195 places that touch the raw device behind the
	// facade's back (the plan's risk 1). Wedge 9 converts those call sites, and when nothing writes a D3D9
	// resource directly any more this copy has nothing left to read and comes out with the d3d9 dependency.
	// A buffer that fails here - a dynamic pool, which D3D9 refuses to lock readonly - is tried once and
	// then left falling back, so a permanent failure cannot cost a memcpy per draw.
	struct VKTriedLog {
		void*	key[64];
		unsigned used;
	};
	VKTriedLog VKTriedVB;
	VKTriedLog VKTriedIB;

	bool vk_backing_already_tried(VKTriedLog& tried, void* key)
	{
		for (unsigned i = 0; i < tried.used; ++i) if (tried.key[i] == key) return true;
		if (tried.used < 64u) tried.key[tried.used++] = key;
		return false;
	}

	bool vk_materialize_vertex_backing(VertexBufferClass* vb)
	{
		if (vk_backing_already_tried(VKTriedVB, vb)) return false;
		const unsigned stride = vb->FVF_Info().Get_FVF_Size();
		const unsigned bytes = vb->Get_Vertex_Count() * stride;
		if (!stride || !bytes) return false;

		IDirect3DVertexBuffer9* d3d = static_cast<DX8VertexBufferClass*>(vb)->Get_DX8_Vertex_Buffer();
		if (!d3d) return false;
		void* src = nullptr;
		if (FAILED(d3d->Lock(0, 0, &src, D3DLOCK_READONLY)) || !src) {
			if (src) d3d->Unlock();
			return false;
		}
		void* dst = VKGeo_Back_Lock(vb, bytes, stride, 0, 0);
		if (dst) memcpy(dst, src, bytes);
		d3d->Unlock();
		return dst != nullptr;
	}

	bool vk_materialize_index_backing(IndexBufferClass* ib)
	{
		if (vk_backing_already_tried(VKTriedIB, ib)) return false;
		const unsigned bytes = ib->Get_Index_Count() * sizeof(unsigned short);
		if (!bytes) return false;

		IDirect3DIndexBuffer9* d3d = static_cast<DX8IndexBufferClass*>(ib)->Get_DX8_Index_Buffer();
		if (!d3d) return false;
		void* src = nullptr;
		if (FAILED(d3d->Lock(0, 0, &src, D3DLOCK_READONLY)) || !src) {
			if (src) d3d->Unlock();
			return false;
		}
		void* dst = VKGeo_Back_Lock(ib, bytes, 0, 0, 0);
		if (dst) memcpy(dst, src, bytes);
		d3d->Unlock();
		return dst != nullptr;
	}

}

void DX8Wrapper::VK_Shadow_Viewport(const D3DVIEWPORT9* viewport)
{
	if (!viewport || !viewport->Width || !viewport->Height) return;
	VK_VP.x = (unsigned)viewport->X;
	VK_VP.y = (unsigned)viewport->Y;
	VK_VP.w = (unsigned)viewport->Width;
	VK_VP.h = (unsigned)viewport->Height;
	VK_VP.valid = true;
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the fixed-function vertex stage's input block.
//
// Everything in here comes from a shadow the facade already keeps, and the point of that is the one
// property D3D9 never gave this engine: the state a batch is drawn with is now *named* at submit time
// instead of being whatever the device happens to hold. Each group is a whole number of vec4s so the C++
// offsets equal the std140 offsets of the block in shaders/vkff.vert by construction.
void DX8Wrapper::VK_FF_Pack(VKDraw_FF_State* out, const FVFInfoClass& info)
{
	if (!out) return;
	VKDraw_FF_State& ff = *out;
	memset(&ff, 0, sizeof(ff));

	const unsigned fvf = info.Get_FVF();
	const unsigned texcount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

	// ---- the three transforms ---------------------------------------------------------------
	// Copied as the same 16 bytes DX8CALL(SetTransform) would have handed the device, in the same
	// transposed storage the facade uses (render_state.world = engine matrix transposed), so the vertex
	// stage and the D3D9 path cannot disagree about a transform even if the engine's own convention is
	// ever re-examined. The identity flags are part of render_state_changed, not the matrices, so the
	// matrix bytes behind an identity declaration are not guaranteed to be anything at all.
	{
		float identity[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };
		if (render_state_changed & (unsigned)WORLD_IDENTITY) memcpy(&ff.world[0], identity, sizeof(identity));
		else memcpy(&ff.world[0], &render_state.world[0][0], sizeof(ff.world));
		if (render_state_changed & (unsigned)VIEW_IDENTITY) memcpy(&ff.view[0], identity, sizeof(identity));
		else memcpy(&ff.view[0], &render_state.view[0][0], sizeof(ff.view));
	}

	// D3D9 dropped D3DRS_ZBIAS, so the port emulates it in the projection matrix; that emulation edits a
	// temporary on the way to the device and never reaches a shadow, so the same two lines are repeated
	// here on the same bytes. ZNear == ZFar is the degenerate 2D/UI pass the facade already skips.
	{
		Matrix4x4 proj = ProjectionMatrix;
		if (ZNear != ZFar) {
			float zb = (float)ZBias;
			zb *= (1.0f / 16.0f);
			zb *= 1.0f / (ZFar - ZNear);
			proj[2][2] -= zb * proj[3][2];
		}
		memcpy(&ff.proj[0], &proj[0][0], sizeof(ff.proj));
	}

	// ---- the eye position -------------------------------------------------------------------
	// D3DRS_LOCALVIEWER defaults to TRUE and the census measured it never written, so the eye is the
	// camera's world position for every batch that lights anything. The view matrix is the camera-space
	// transform, so eye = -R^T * t of that matrix, read straight out of the bytes above.
	{
		const float* v = &ff.view[0];
		const float tx = v[12], ty = v[13], tz = v[14];
		// R[j][i] = v[4*i+j] because the stored bytes are the engine matrix transposed and the engine
		// keeps its basis vectors in columns.
		ff.eye_position[0] = -(v[0] * tx + v[5] * ty + v[10] * tz);
		ff.eye_position[1] = -(v[1] * tx + v[6] * ty + v[11] * tz);
		ff.eye_position[2] = -(v[2] * tx + v[7] * ty + v[12] * tz);
		ff.eye_position[3] = 1.0f;
	}

	// ---- material ---------------------------------------------------------------------------
	{
		const D3DMATERIAL9& m = Peek_DX8_Material();
		memcpy(&ff.mat_diffuse[0],   &m.Diffuse,  sizeof(float) * 4);
		memcpy(&ff.mat_ambient[0],   &m.Ambient,  sizeof(float) * 4);
		memcpy(&ff.mat_specular[0],  &m.Specular, sizeof(float) * 4);
		memcpy(&ff.mat_emissive[0],  &m.Emissive, sizeof(float) * 4);
		ff.mat_power[0] = m.Power;
	}

	// ---- per-light colours, scene ambient and fog ---------------------------------------------
	{
		union { unsigned u; float f; } bs;
		const unsigned amb = VK_Read_RS(D3DRS_AMBIENT);
		ff.scene_ambient[0] = float((amb >> 16) & 0xFFu) / 255.0f;
		ff.scene_ambient[1] = float((amb >> 8) & 0xFFu) / 255.0f;
		ff.scene_ambient[2] = float(amb & 0xFFu) / 255.0f;
		ff.scene_ambient[3] = 1.0f;

		const unsigned fogc = VK_Read_RS(D3DRS_FOGCOLOR);
		ff.fog_color[0] = float((fogc >> 16) & 0xFFu) / 255.0f;
		ff.fog_color[1] = float((fogc >> 8) & 0xFFu) / 255.0f;
		ff.fog_color[2] = float(fogc & 0xFFu) / 255.0f;
		ff.fog_color[3] = 1.0f;

		bs.u = VK_Read_RS_Default_bits(D3DRS_FOGSTART, 0.0f);    ff.fog_params[0] = bs.f;
		bs.u = VK_Read_RS_Default_bits(D3DRS_FOGEND, 1.0f);      ff.fog_params[1] = bs.f;
		bs.u = VK_Read_RS_Default_bits(D3DRS_FOGDENSITY, 1.0f);  ff.fog_params[2] = bs.f;

		const unsigned tf = VK_Read_RS(D3DRS_TEXTUREFACTOR);
		ff.tex_factor[0] = float((tf >> 16) & 0xFFu) / 255.0f;
		ff.tex_factor[1] = float((tf >> 8) & 0xFFu) / 255.0f;
		ff.tex_factor[2] = float(tf & 0xFFu) / 255.0f;
		ff.tex_factor[3] = float((tf >> 24) & 0xFFu) / 255.0f;
	}

	// ---- the eight light slots ---------------------------------------------------------------
	// The engine fills D3DLIGHT9 through Set_Light(index, LightClass) and that helper is the only writer,
	// so the field meanings below are its own: Direction is the vector the light *travels* along (the
	// engine negates Get_Light_Direction, which points at the light), Falloff carries Spot_Exponent and
	// Theta and Phi both carry Spot_Angle. The D3D9 cone formula divides by cos(Theta)-cos(Phi), which is
	// zero for that pair; the vertex stage treats the degenerate cone as the hard edge the same numbers
	// describe (see vkff.vert), which is also what hardware did with a zero-width ramp.
	unsigned lightMask = 0;
	for (unsigned i = 0; i < 8; ++i) {
		if (!render_state.LightEnable[i]) continue;
		lightMask |= (1u << i);
		const D3DLIGHT9& l = render_state.Lights[i];
		VKDraw_FF_Light& dl = ff.lights[i];
		dl.type_att[0] = (float)l.Type;
		dl.type_att[1] = l.Attenuation0;
		dl.type_att[2] = l.Attenuation1;
		dl.type_att[3] = l.Attenuation2;
		dl.pos_range[0] = l.Position.x;
		dl.pos_range[1] = l.Position.y;
		dl.pos_range[2] = l.Position.z;
		dl.pos_range[3] = l.Range;
		dl.dir_theta[0] = l.Direction.x;
		dl.dir_theta[1] = l.Direction.y;
		dl.dir_theta[2] = l.Direction.z;
		dl.dir_theta[3] = l.Theta;
		dl.phi_falloff[0] = l.Phi;
		dl.phi_falloff[1] = l.Falloff;
		memcpy(&dl.ambient[0],  &l.Ambient,  sizeof(float) * 4);
		memcpy(&dl.diffuse[0],  &l.Diffuse,  sizeof(float) * 4);
		memcpy(&dl.specular[0], &l.Specular, sizeof(float) * 4);
	}

	// ---- the flags ---------------------------------------------------------------------------
	unsigned fx = 0;
	if (VK_Read_RS(D3DRS_LIGHTING))         fx |= VKFF_FLAG_LIGHTING;
	if (VK_Read_RS(D3DRS_COLORVERTEX))      fx |= VKFF_FLAG_COLORVERTEX;
	if (VK_Read_RS(D3DRS_SPECULARENABLE))   fx |= VKFF_FLAG_SPECULAR_ENABLE;
	if (VK_Read_RS(D3DRS_NORMALIZENORMALS)) fx |= VKFF_FLAG_NORMALIZE;
	if (VK_Read_RS(D3DRS_FOGENABLE))        fx |= VKFF_FLAG_FOG_ENABLE;
	if (fvf & D3DFVF_NORMAL)   fx |= VKFF_FLAG_HAS_NORMAL;
	if (fvf & D3DFVF_DIFFUSE)  fx |= VKFF_FLAG_HAS_DIFFUSE;
	if (fvf & D3DFVF_SPECULAR) fx |= VKFF_FLAG_HAS_SPECULAR;
	if (fvf & D3DFVF_XYZRHW)   fx |= VKFF_FLAG_RHW;
	if ((fvf & D3DFVF_XYZ) && !(fvf & (D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEXCOUNT_MASK)))
		fx |= VKFF_FLAG_POSITION_ONLY;

	unsigned fy = lightMask;
	fy |= (texcount & 7u) << 8;
	fy |= (VK_Read_RS(D3DRS_FOGVERTEXMODE) & 7u) << 11;
	fy |= (VK_Read_RS(D3DRS_FOGTABLEMODE) & 7u) << 14;
	fy |= (VK_Read_RS(D3DRS_AMBIENTMATERIALSOURCE) & 3u) << 17;
	fy |= (VK_Read_RS(D3DRS_DIFFUSEMATERIALSOURCE) & 3u) << 19;
	fy |= (VK_Read_RS(D3DRS_SPECULARMATERIALSOURCE) & 3u) << 21;
	fy |= (VK_Read_RS(D3DRS_EMISSIVEMATERIALSOURCE) & 3u) << 23;
	ff.flags[0] = fx;
	ff.flags[1] = fy;

	// ---- the per-stage coordinate source ------------------------------------------------------
	// The whole fixed-function texture surface the census measured is these two registers plus the
	// combiner state wedge 7 reads: nothing in the engine ever writes D3DTS_TEXTURE0..7, so there is no
	// texture matrix stack to carry, and D3DTTFF_PROJECTED is the only transform flag the FVF_XYZRHW
	// path can reach.
	for (unsigned s = 0; s < VKFF_MAX_STAGES; ++s) {
		const unsigned tci  = VK_Read_TSS(s, D3DTSS_TEXCOORDINDEX, s);
		const unsigned ttff = VK_Read_TSS(s, D3DTSS_TEXTURETRANSFORMFLAGS, 0);
		ff.stage[s].tci = tci;
		ff.stage[s].ttff = ttff;
		unsigned set = (tci >> 16) ? 0u : (tci & 0xFFFFu);
		if (texcount == 0u) { ff.stage[s].set = 0u; ff.stage[s].components = 0u; continue; }
		if (set >= texcount) set = texcount - 1u;
		const unsigned bits = (fvf >> (16u + 2u * set)) & 3u;
		ff.stage[s].set = set;
		ff.stage[s].components = (bits == D3DFVF_TEXTUREFORMAT2) ? 2u
			: (bits == D3DFVF_TEXTUREFORMAT1) ? 1u
			: (bits == D3DFVF_TEXTUREFORMAT3) ? 3u : 4u;
	}

	// ---- the viewport ------------------------------------------------------------------------
	// The RHW path arrives in this space already, and only this path can say which rectangle was current.
	unsigned tw = 0, th = 0;
	VKDraw_Get_Targets(nullptr, nullptr, &tw, &th);
	if (VK_VP.valid) {
		ff.viewport[0] = (float)VK_VP.x; ff.viewport[1] = (float)VK_VP.y;
		ff.viewport[2] = (float)VK_VP.w; ff.viewport[3] = (float)VK_VP.h;
	} else {
		ff.viewport[2] = (float)tw;      ff.viewport[3] = (float)th;
	}
}

bool DX8Wrapper::VK_Emit_Indexed_Draw(void* vb_, void* ib_, unsigned primitive_type,
	unsigned short first_index, unsigned short polygon_count,
	unsigned short min_vertex_index, unsigned short vertex_count, unsigned long long base_vertex)
{
	(void)min_vertex_index;	// a range hint for D3D9, not something Vulkan binds against
	if (!VKDraw_Ready()) { ++VKF.notReady; return false; }

	VertexBufferClass* vb = static_cast<VertexBufferClass*>(vb_);
	IndexBufferClass*  ib = static_cast<IndexBufferClass*>(ib_);
	if (!vb || !ib || !polygon_count) return false;

	// A batch with a custom shader is a wedge-8 pipeline variant. The mechanism vertex/pixel stages this
	// wedge stands up know nothing about those programs, so such a batch is reported as not consumable and
	// the D3D9 call still runs - which is what keeps every intermediate state of this port playable.
	if (Vertex_Shader || Pixel_Shader) { ++VKF.programmable; return false; }

	void* vbBuf = nullptr; void* ibBuf = nullptr;
	unsigned long long vbOff = 0, ibOff = 0;
	unsigned vbBytes = 0, ibBytes = 0;
	if (!VKGeo_Back_Bind(vb, &vbBuf, &vbOff, nullptr, &vbBytes) || !vbBuf) {
		if (!vk_materialize_vertex_backing(vb) ||
			!VKGeo_Back_Bind(vb, &vbBuf, &vbOff, nullptr, &vbBytes) || !vbBuf) {
			++VKF.noVertexBacking;
			vk_report_missing_vb(vb);
			return false;
		}
		++VKF.materialized;
	}
	if (!VKGeo_Back_Bind(ib, &ibBuf, &ibOff, nullptr, &ibBytes) || !ibBuf) {
		if (!vk_materialize_index_backing(ib) ||
			!VKGeo_Back_Bind(ib, &ibBuf, &ibOff, nullptr, &ibBytes) || !ibBuf) {
			++VKF.noIndexBacking;
			return false;
		}
		++VKF.materialized;
	}

	// The vertex layout comes from the buffer, exactly like the D3D9 path takes it from the buffer:
	// Apply_Render_State_Changes re-asserts SetFVF(vb->FVF_Info().Get_FVF()) on every buffer change, so
	// this is the format the batch was drawn with, not merely the last one the game named.
	const FVFInfoClass& info = vb->FVF_Info();
	const unsigned fvf = info.Get_FVF();
	const unsigned stride = info.Get_FVF_Size();
	if (!fvf || !stride) { ++VKF.refused; return false; }

	VKDraw_Attr attrs[VKDRAW_MAX_ATTRS];
	unsigned attr_count = 0;
	// Location 0 is the position, the only input the mechanism stages declare; the rest are declared so a
	// wedge-6 fixed-function variant finds them at the offset the FVF says, and extra attributes that no
	// stage reads cost nothing in Vulkan.
	if (fvf & D3DFVF_XYZRHW) attrs[attr_count++] = { 0u, info.Get_Location_Offset(), 4u, 0u };
	else                     attrs[attr_count++] = { 0u, info.Get_Location_Offset(), 3u, 0u };
	// The position field of an FVF is one value from the mask 0x00E: 0x2 XYZ, 0x4 XYZRHW, then 0x6/0x8/0xA/
	// 0xC/0xE for the blend-weight layouts. Testing ">= 0x6" is what separates them; the earlier "!= 0" form
	// was true for plain XYZ as well, which silently dropped NORMAL from every lit mesh. It cost nothing
	// while the wedge-5 stage only read position, and would cost the whole scene now that the fixed-function
	// stage lights faces from location 1.
	if ((fvf & 0x00E) >= 0x006) {
		// Blend weights (D3DFVF_XYZB1..5) only mean something to a custom vertex shader, which was refused
		// above, so there is nothing to describe here - and a skinning layout with five weights plus a
		// matrix palette index set is wedge 6's and wedge 8's business, not this table's.
	} else {
		if (fvf & D3DFVF_NORMAL)   attrs[attr_count++] = { 1u, info.Get_Normal_Offset(),  3u, 0u };
		// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: D3DFVF_PSIZE is deliberately not
		// described. Point rendering with a per-vertex size is D3DRS_POINTSCALE territory, which the
		// approved plan drops, and the census measured no FVF with the bit set - every FVF it saw is
		// accounted for in the case table of Vulkan/artifacts/census_menu.csv. It also has to be dropped
		// for a hard reason: the wedge-6 fixed-function vertex stage numbers its texture coordinates from
		// location 4, so a one-float point size at location 5 would collide with the second UV set and
		// build a pipeline that reads the wrong bytes.
	}
	if (fvf & D3DFVF_DIFFUSE)  attrs[attr_count++] = { 2u, info.Get_Diffuse_Offset(),  4u, 1u };
	if (fvf & D3DFVF_SPECULAR) attrs[attr_count++] = { 3u, info.Get_Specular_Offset(), 4u, 1u };
	const unsigned texcount = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
	for (unsigned t = 0; t < texcount && attr_count < VKDRAW_MAX_ATTRS; ++t) {
		// D3DFVF_TEXTUREFORMAT2 = 0 (two floats), 3 = one, 1 = three, 2 = four - two bits per set from bit 16.
		const unsigned bits = (fvf >> (16u + 2u * t)) & 3u;
		const unsigned comps = (bits == D3DFVF_TEXTUREFORMAT2) ? 2u
			: (bits == D3DFVF_TEXTUREFORMAT1) ? 1u
			: (bits == D3DFVF_TEXTUREFORMAT3) ? 3u : 4u;
		attrs[attr_count++] = { 4u + t, info.Get_Tex_Offset(t), comps, 0u };
	}
	if (!attr_count) { ++VKF.refused; return false; }

	VKDraw_Submit s = {};
	s.key.shader_bits = (unsigned long long)(unsigned&)render_state.shader;
	s.key.fvf         = fvf;
	unsigned mask = 0;
	for (unsigned i = 0; i < 8; ++i) if (render_state.LightEnable[i]) mask |= (1u << i);
	s.key.light_mask  = mask;
	s.key.vs_tag = 0u; s.key.ps_tag = 0u;	// wedge 8 replaces these with registry indices
	s.key.topology = primitive_type;
	// The attachment formats are filled in by the recorder from the images the pass is built on, so the
	// facade does not have to know which mirror it is drawing into.
	s.key.rt_format = 0u; s.key.depth_format = 0u;
	s.key.sample_count = 1u;
	s.key.cull_mode   = VK_Read_RS(D3DRS_CULLMODE);
	// DX8 has no D3DRS_FRONTFACE, so the front-face declaration is a fixed part of this port's contract with
	// the rasterizer rather than a shadowed state; see vk_cull() for what the pair means.
	s.key.front_ccw   = 0u;
	s.key.fill_mode   = VK_Read_RS(D3DRS_FILLMODE);
	s.key.depth_test  = VK_Read_RS(D3DRS_ZENABLE)  != D3DZB_FALSE ? 1u : 0u;
	s.key.depth_write = VK_Read_RS(D3DRS_ZWRITEENABLE) ? 1u : 0u;
	s.key.depth_cmp   = VK_Read_RS(D3DRS_ZFUNC);
	s.key.stencil_test = VK_Read_RS(D3DRS_STENCILENABLE) ? 1u : 0u;
	s.key.blend_enable = VK_Read_RS(D3DRS_ALPHABLENDENABLE) ? 1u : 0u;
	s.key.src_blend   = VK_Read_RS(D3DRS_SRCBLEND);
	s.key.dst_blend   = VK_Read_RS(D3DRS_DESTBLEND);
	// No separate-alpha blending in DX8: the two alpha fields stay 0 and the layer reuses the colour pair.
	s.key.src_blend_alpha = 0u; s.key.dst_blend_alpha = 0u;
	s.key.write_mask  = VK_Read_RS(D3DRS_COLORWRITEENABLE) & 0xFu;
	s.key.alpha_test_enable = VK_Read_RS(D3DRS_ALPHATESTENABLE) ? 1u : 0u;
	s.key.alpha_ref   = VK_Read_RS(D3DRS_ALPHAREF) & 0xFFu;
	s.key.alpha_cmp   = VK_Read_RS(D3DRS_ALPHAFUNC);
	s.key.scissor_test = 0u;	// likewise absent from DX8; the recorder always sets one scissor

	s.stream_count = 1;
	s.streams[0].vk_buffer   = vbBuf;
	s.streams[0].offset      = vbOff;
	s.streams[0].stride      = stride;
	s.streams[0].vertex_count = vb->Get_Vertex_Count();

	for (unsigned i = 0; i < attr_count; ++i) s.attrs[i] = attrs[i];
	s.attr_count = attr_count;

	s.vk_index_buffer = ibBuf;
	s.index_offset    = ibOff;
	s.start_index     = first_index;
	// How many indices the backing actually holds from here: D3D9 guarded the same read, and the layer
	// refuses a batch that would run past it instead of reading another object's geometry.
	s.index_count     = (ib->Get_Index_Count() > (unsigned)first_index)
		? ib->Get_Index_Count() - (unsigned)first_index : 0u;
	s.polygon_count   = polygon_count;
	s.base_vertex     = (unsigned)base_vertex;

	// Textures the D3D path had bound are handed over as views so the descriptor set is the real one; an
	// unmirrored or empty slot falls to the layer's built-in white, which is the passthrough the D3D
	// combiners used there. Samplers wait for wedge 7, which reads them out of the stage state.
	for (unsigned t = 0; t < VKDRAW_MAX_TEX_SLOTS; ++t) {
		VKTex_Info ti;
		if (Textures[t] && VKTex_Query((void*)Textures[t], &ti) && ti.view) s.texture_views[t] = ti.view;
	}

	s.vertex_constants = (const float*)&Vertex_Shader_Constants[0];
	s.pixel_constants  = (const float*)&Pixel_Shader_Constants[0];
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the fixed-function block travels with every
	// batch, not only the ones the vertex stage is switched on for. Packing it unconditionally is what
	// makes the gate a shader choice rather than a state choice - the same submit either way, so flipping
	// GENERALSX_VKFFVERT cannot silently change what the pipeline was told to draw.
	VKDraw_FF_State ffState;
	VK_FF_Pack(&ffState, info);
	s.ff = &ffState;
	s.vertex_constant_vec4s = MAX_VERTEX_SHADER_CONSTANTS;
	s.pixel_constant_vec4s  = MAX_PIXEL_SHADER_CONSTANTS;

	unsigned tw = 0, th = 0;
	void* ck = nullptr; void* dk = nullptr;
	VKDraw_Get_Targets(&ck, &dk, &tw, &th);
	if (VK_VP.valid && VK_VP.w <= tw && VK_VP.h <= th) {
		s.viewport[0] = VK_VP.x; s.viewport[1] = VK_VP.y;
		s.viewport[2] = VK_VP.w; s.viewport[3] = VK_VP.h;
		s.scissor[0] = VK_VP.x; s.scissor[1] = VK_VP.y;
		s.scissor[2] = VK_VP.x + VK_VP.w; s.scissor[3] = VK_VP.y + VK_VP.h;
	} else {
		s.viewport[2] = tw; s.viewport[3] = th;
		s.scissor[2] = tw; s.scissor[3] = th;
	}

	if (!VK_Draw_Submit(&s)) { ++VKF.refused; return false; }
	++VKF.emitted;
	if ((VKF.emitted & 8191ull) == 0ull) {
		// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A: the plan's hard gate for this wedge is
		// "no D3D9 draw is left", and that is measurable, not deferred - a batch the emitter takes stops
		// before DX8_RECORD_DRAW_CALLS(), so this frame's counter is the number the Vulkan path did NOT
		// describe. dx8_calls is the same story one level up: the raw device calls the facade still makes.
		// Both are per-frame (reset in Begin_Pass), so they read as the steady-state frame, not a total.
		fprintf(stderr, "[VKDRAW] facade submits=%llu noVb=%llu noIb=%llu programmable=%llu refused=%llu materialized=%llu frameD3dDraws=%u frameDx8Calls=%u\n",
			VKF.emitted, VKF.noVertexBacking, VKF.noIndexBacking, VKF.programmable, VKF.refused, VKF.materialized,
			FrameStatistics.draw_calls, FrameStatistics.dx8_calls);
		fflush(stderr);
	}
	return true;
}

#endif // _WIN32 && SAGE_USE_VULKAN_CORE

// ----------------------------------------------------------------------------
//
// Private function for the special case of rendering polygons from sorting
// index and vertex buffers.
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Sorting_IB_VB(
	unsigned primitive_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	WWASSERT(render_state.vertex_buffer_types[0]==BUFFER_TYPE_SORTING || render_state.vertex_buffer_types[0]==BUFFER_TYPE_DYNAMIC_SORTING);
	WWASSERT(render_state.index_buffer_type==BUFFER_TYPE_SORTING || render_state.index_buffer_type==BUFFER_TYPE_DYNAMIC_SORTING);

	// Fill dynamic vertex buffer with sorting vertex buffer vertices
	DynamicVBAccessClass dyn_vb_access(BUFFER_TYPE_DYNAMIC_DX8,dynamic_fvf_type,vertex_count);
	{
		DynamicVBAccessClass::WriteLockClass lock(&dyn_vb_access);
		VertexFormatXYZNDUV2* src = static_cast<SortingVertexBufferClass*>(render_state.vertex_buffers[0])->VertexBuffer;
		VertexFormatXYZNDUV2* dest= lock.Get_Formatted_Vertex_Array();
		src += render_state.vba_offset + render_state.index_base_offset + min_vertex_index;
		unsigned  size = dyn_vb_access.FVF_Info().Get_FVF_Size()*vertex_count/sizeof(unsigned);
		unsigned *dest_u =(unsigned*) dest;
		unsigned *src_u = (unsigned*) src;

		for (unsigned i=0;i<size;++i) {
			*dest_u++=*src_u++;
		}
	}

	DX8CALL(SetStreamSource(
		0,
		static_cast<DX8VertexBufferClass*>(dyn_vb_access.VertexBuffer)->Get_DX8_Vertex_Buffer(),
		0,
		dyn_vb_access.FVF_Info().Get_FVF_Size()));
	// If using FVF format VB, set the FVF (may not be needed here KM)
	unsigned fvf=dyn_vb_access.FVF_Info().Get_FVF();
	if (fvf!=0) {
		DX8CALL(SetFVF(fvf));
	}
	DX8_RECORD_VERTEX_BUFFER_CHANGE();

	unsigned index_count=0;
	switch (primitive_type) {
	case D3DPT_TRIANGLELIST: index_count=polygon_count*3; break;
	case D3DPT_TRIANGLESTRIP: index_count=polygon_count+2; break;
	case D3DPT_TRIANGLEFAN: index_count=polygon_count+2; break;
	default: WWASSERT(0); break; // Unsupported primitive type
	}

	// Fill dynamic index buffer with sorting index buffer vertices
	DynamicIBAccessClass dyn_ib_access(BUFFER_TYPE_DYNAMIC_DX8,index_count);
	{
		DynamicIBAccessClass::WriteLockClass lock(&dyn_ib_access);
		unsigned short* dest=lock.Get_Index_Array();
		unsigned short* src=nullptr;
		src=static_cast<SortingIndexBufferClass*>(render_state.index_buffer)->index_buffer;
		src+=render_state.iba_offset+start_index;

		for (unsigned short i=0;i<index_count;++i) {
			unsigned short index=*src++;
			index-=min_vertex_index;
			WWASSERT(index<vertex_count);
			*dest++=index;
		}
	}

	// GeneralsX @build d3d9 port 11/10/2026 - D3D9 drops SetIndices' vertex-buffer offset; it moves
	// to DrawIndexedPrimitive's BaseVertexIndex parameter.
	DX8CALL(SetIndices(
		static_cast<DX8IndexBufferClass*>(dyn_ib_access.IndexBuffer)->Get_DX8_Index_Buffer()));
	DX8_RECORD_INDEX_BUFFER_CHANGE();

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.C: the sorting pass. The two dynamic
	// buffers filled just above are the shared per-frame pools, so the batch is described at the pool
	// backing plus the same offsets D3D9 was handed: IndexBufferOffset as the start index,
	// VertexBufferOffset as BaseVertexIndex. A consumed batch must not also reach the device - wedge 9
	// gates on the D3D9 draw counter going to zero.
	if (VK_Emit_Indexed_Draw(dyn_vb_access.VertexBuffer,dyn_ib_access.IndexBuffer,
		D3DPT_TRIANGLELIST,
		(unsigned short)dyn_ib_access.IndexBufferOffset,
		polygon_count,
		0,
		vertex_count,
		(unsigned long long)dyn_vb_access.VertexBufferOffset)) {
		DX8_RECORD_RENDER(polygon_count,vertex_count,render_state.shader);
		return;
	}
#endif
	DX8_RECORD_DRAW_CALLS();
	DX8CALL(DrawIndexedPrimitive(
		D3DPT_TRIANGLELIST,
		dyn_vb_access.VertexBufferOffset,	// BaseVertexIndex (was SetIndices offset in DX8)
		0,		// start vertex
		vertex_count,
		dyn_ib_access.IndexBufferOffset,
		polygon_count));

	DX8_RECORD_RENDER(polygon_count,vertex_count,render_state.shader);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw(
	unsigned primitive_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	// GeneralsX @refactor PS3.0-P3-33 14/10/2026 the DrawPolygonLowBoundLimit cull was removed:
	// its only setter had no caller in either game tree, so the counter was permanently 0 and the
	// early-out dead code.

	DX8_THREAD_ASSERT();
	SNAPSHOT_SAY(("DX8 - draw"));

	Apply_Render_State_Changes();

	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0: measure the state tuple this draw is about to
	// commit to, before any early-out, because the early-out still costs a state apply.
	VK_Census_Draw(primitive_type, polygon_count, vertex_count);

	// Debug feature to disable triangle drawing...
	if (!_Is_Triangle_Draw_Enabled()) return;

#ifdef DEBUG_LOGGING
	if (WW3D::Is_Snapshot_Activated()) {
		DWORD passes=0;		// GeneralsX @build BenderAI 10/02/2026 - Use DWORD (32-bit) instead of unsigned long (platform-dependent)
		SNAPSHOT_SAY(("ValidateDevice:"));
		HRESULT res=D3DDevice->ValidateDevice(&passes);
		switch (res) {
		case D3D_OK:
			SNAPSHOT_SAY(("OK"));
			break;

		case D3DERR_CONFLICTINGTEXTUREFILTER:
			SNAPSHOT_SAY(("D3DERR_CONFLICTINGTEXTUREFILTER"));
			break;
		case D3DERR_CONFLICTINGTEXTUREPALETTE:
			SNAPSHOT_SAY(("D3DERR_CONFLICTINGTEXTUREPALETTE"));
			break;
		case D3DERR_DEVICELOST:
			SNAPSHOT_SAY(("D3DERR_DEVICELOST"));
			break;
		case D3DERR_TOOMANYOPERATIONS:
			SNAPSHOT_SAY(("D3DERR_TOOMANYOPERATIONS"));
			break;
		case D3DERR_UNSUPPORTEDALPHAARG:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDALPHAARG"));
			break;
		case D3DERR_UNSUPPORTEDALPHAOPERATION:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDALPHAOPERATION"));
			break;
		case D3DERR_UNSUPPORTEDCOLORARG:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDCOLORARG"));
			break;
		case D3DERR_UNSUPPORTEDCOLOROPERATION:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDCOLOROPERATION"));
			break;
		case D3DERR_UNSUPPORTEDFACTORVALUE:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDFACTORVALUE"));
			break;
		case D3DERR_UNSUPPORTEDTEXTUREFILTER:
			SNAPSHOT_SAY(("D3DERR_UNSUPPORTEDTEXTUREFILTER"));
			break;
		case D3DERR_WRONGTEXTUREFORMAT:
			SNAPSHOT_SAY(("D3DERR_WRONGTEXTUREFORMAT"));
			break;
		default:
			SNAPSHOT_SAY(("UNKNOWN Error"));
			break;
		}
	}

	SNAPSHOT_SAY(("DX8 - draw %d polygons (%d vertices)",polygon_count,vertex_count));
#endif

	if (vertex_count<3) {
		min_vertex_index=0;
		switch (render_state.vertex_buffer_types[0]) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_SORTING:
			vertex_count=render_state.vertex_buffers[0]->Get_Vertex_Count()-render_state.index_base_offset-render_state.vba_offset-min_vertex_index;
			break;
		case BUFFER_TYPE_DYNAMIC_DX8:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			vertex_count=render_state.vba_count;
			break;
		}
	}

	switch (render_state.vertex_buffer_types[0]) {
	case BUFFER_TYPE_DX8:
	case BUFFER_TYPE_DYNAMIC_DX8:
		switch (render_state.index_buffer_type) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_DYNAMIC_DX8:
			{
/*				if ((start_index+render_state.iba_offset+polygon_count*3) > render_state.index_buffer->Get_Index_Count())
				{	WWASSERT_PRINT(0,"OVERFLOWING INDEX BUFFER");
					///@todo: MUST FIND OUT WHY THIS HAPPENS WITH LOTS OF PARTICLES ON BIG FIGHT!  -MW
					break;
				}*/
				DX8_RECORD_RENDER(polygon_count,vertex_count,render_state.shader);
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
				// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.C: the primary draw site. The
				// filler describes the shadow state as a Vulkan batch and returns true when the pipeline
				// layer took it, in which case no D3D9 draw is recorded and none is issued. Whatever it
				// cannot describe - no backing, a custom shader, a refused submit - still runs the D3D9
				// call below, which is what keeps the intermediate states of this port playable.
				if (VK_Emit_Indexed_Draw(render_state.vertex_buffers[0],render_state.index_buffer,
					primitive_type,
					(unsigned short)(start_index+render_state.iba_offset),
					polygon_count,
					min_vertex_index,
					vertex_count,
					(unsigned long long)(render_state.index_base_offset+render_state.vba_offset))) {
					break;
				}
#endif
				DX8_RECORD_DRAW_CALLS();
				DX8CALL(DrawIndexedPrimitive(
					(D3DPRIMITIVETYPE)primitive_type,
					// GeneralsX @build d3d9 port 11/10/2026 - DX8 SetIndices offset becomes D3D9 BaseVertexIndex
					(render_state.index_base_offset+render_state.vba_offset),
					min_vertex_index,
					vertex_count,
					start_index+render_state.iba_offset,
					polygon_count));
			}
			break;
		case BUFFER_TYPE_SORTING:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			WWASSERT_PRINT(0,"VB and IB must of same type (sorting or dx8)");
			break;
		case BUFFER_TYPE_INVALID:
			WWASSERT(0);
			break;
		}
		break;
	case BUFFER_TYPE_SORTING:
	case BUFFER_TYPE_DYNAMIC_SORTING:
		switch (render_state.index_buffer_type) {
		case BUFFER_TYPE_DX8:
		case BUFFER_TYPE_DYNAMIC_DX8:
			WWASSERT_PRINT(0,"VB and IB must of same type (sorting or dx8)");
			break;
		case BUFFER_TYPE_SORTING:
		case BUFFER_TYPE_DYNAMIC_SORTING:
			Draw_Sorting_IB_VB(primitive_type,start_index,polygon_count,min_vertex_index,vertex_count);
			break;
		case BUFFER_TYPE_INVALID:
			WWASSERT(0);
			break;
		}
		break;
	case BUFFER_TYPE_INVALID:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Triangles(
	unsigned buffer_type,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	if (buffer_type==BUFFER_TYPE_SORTING || buffer_type==BUFFER_TYPE_DYNAMIC_SORTING) {
		SortingRendererClass::Insert_Triangles(start_index,polygon_count,min_vertex_index,vertex_count);
	}
	else {
		Draw(D3DPT_TRIANGLELIST,start_index,polygon_count,min_vertex_index,vertex_count);
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Triangles(
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	Draw(D3DPT_TRIANGLELIST,start_index,polygon_count,min_vertex_index,vertex_count);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Draw_Strip(
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	Draw(D3DPT_TRIANGLESTRIP,start_index,polygon_count,min_vertex_index,vertex_count);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8Wrapper::Apply_Render_State_Changes()
{
	SNAPSHOT_SAY(("DX8Wrapper::Apply_Render_State_Changes()"));

	if (!render_state_changed) return;
	if (render_state_changed&SHADER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply shader"));
		render_state.shader.Apply();
	}

	unsigned mask=TEXTURE0_CHANGED;
	int i=0;
	for (;i<CurrentCaps->Get_Max_Textures_Per_Pass();++i,mask<<=1)
	{
		if (render_state_changed&mask)
		{
			SNAPSHOT_SAY(("DX8 - apply texture %d (%s)",i,render_state.Textures[i] ? render_state.Textures[i]->Get_Full_Path().str() : "null"));

			if (render_state.Textures[i])
			{
				render_state.Textures[i]->Apply(i);
			}
			else
			{
				TextureBaseClass::Apply_Null(i);
			}
		}
	}

	if (render_state_changed&MATERIAL_CHANGED)
	{
		SNAPSHOT_SAY(("DX8 - apply material"));
		VertexMaterialClass* material=const_cast<VertexMaterialClass*>(render_state.material);
		if (material)
		{
			material->Apply();
		}
		else VertexMaterialClass::Apply_Null();
	}

	if (render_state_changed&LIGHTS_CHANGED)
	{
		// GeneralsX @feature PS3.0-P3-18 14/10/2026: 4 -> 8 units; the per-unit bit comes from the
		// class lookup table because a plain LIGHT0_CHANGED<<index shift would land on the
		// TEXTURE0..3 change bits for index 4..7.
		for (unsigned index=0;index<8;++index) {
			if (render_state_changed&Get_Light_Changed_Bit(index)) {
				SNAPSHOT_SAY(("DX8 - apply light %d",index));
				if (render_state.LightEnable[index]) {
#if defined(DEBUG_CRASHING) || defined(DEBUG_LOGGING)
					if ( WW3D::Is_Snapshot_Activated() ) {
						D3DLIGHT9 * light = &(render_state.Lights[index]);
						static const char * _light_types[] = { "Unknown", "Point","Spot", "Directional" };
						WWASSERT((light->Type >= 0) && (light->Type <= 3));

						SNAPSHOT_SAY((" type = %s amb = %4.2f,%4.2f,%4.2f  diff = %4.2f,%4.2f,%4.2f spec = %4.2f, %4.2f, %4.2f",
							_light_types[light->Type],
							light->Ambient.r,light->Ambient.g,light->Ambient.b,
							light->Diffuse.r,light->Diffuse.g,light->Diffuse.b,
							light->Specular.r,light->Specular.g,light->Specular.b ));
						SNAPSHOT_SAY((" pos = %f, %f, %f  dir = %f, %f, %f",
							light->Position.x, light->Position.y, light->Position.z,
							light->Direction.x, light->Direction.y, light->Direction.z ));
					}
#endif

					Set_DX8_Light(index,&render_state.Lights[index]);
				}
				else {
					Set_DX8_Light(index,nullptr);
					SNAPSHOT_SAY((" clearing light to null"));
				}
			}
		}
	}

	if (render_state_changed&WORLD_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply world matrix"));
		// GeneralsX @refactor BenderAI 10/02/2026 - pass Matrix4x4 directly (no D3DMATRIX conversion)
		_Set_DX8_Transform(D3DTS_WORLD, render_state.world);
	}
	if (render_state_changed&VIEW_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply view matrix"));
		// GeneralsX @refactor BenderAI 10/02/2026 - pass Matrix4x4 directly (no D3DMATRIX conversion)
		_Set_DX8_Transform(D3DTS_VIEW, render_state.view);
	}
	if (render_state_changed&VERTEX_BUFFER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply vb change"));
		for (i=0;i<MAX_VERTEX_STREAMS;++i) {
			if (render_state.vertex_buffers[i]) {
				switch (render_state.vertex_buffer_types[i]) {
					//->Type()) {
				case BUFFER_TYPE_DX8:
				case BUFFER_TYPE_DYNAMIC_DX8:
					DX8CALL(SetStreamSource(
						i,
						static_cast<DX8VertexBufferClass*>(render_state.vertex_buffers[i])->Get_DX8_Vertex_Buffer(),
						0,
						render_state.vertex_buffers[i]->FVF_Info().Get_FVF_Size()));
					DX8_RECORD_VERTEX_BUFFER_CHANGE();
					{
						// If the VB format is FVF, set the FVF
						unsigned fvf=render_state.vertex_buffers[i]->FVF_Info().Get_FVF();
						if (fvf!=0) {
							Set_Vertex_FVF(fvf);
						}
					}
					break;
				case BUFFER_TYPE_SORTING:
				case BUFFER_TYPE_DYNAMIC_SORTING:
					break;
				default:
					WWASSERT(0);
				}
			} else {
				DX8CALL(SetStreamSource(i,nullptr,0,0));
				DX8_RECORD_VERTEX_BUFFER_CHANGE();
			}
		}
	}
	if (render_state_changed&INDEX_BUFFER_CHANGED) {
		SNAPSHOT_SAY(("DX8 - apply ib change"));
		if (render_state.index_buffer) {
			switch (render_state.index_buffer_type) {
				//->Type()) {
			case BUFFER_TYPE_DX8:
			case BUFFER_TYPE_DYNAMIC_DX8:
				// GeneralsX @build d3d9 port 11/10/2026 - SetIndices lost its offset parameter;
				// the offset (index_base_offset+vba_offset) is applied as BaseVertexIndex at DrawIndexedPrimitive.
				DX8CALL(SetIndices(
					static_cast<DX8IndexBufferClass*>(render_state.index_buffer)->Get_DX8_Index_Buffer()));
				DX8_RECORD_INDEX_BUFFER_CHANGE();
				break;
			case BUFFER_TYPE_SORTING:
			case BUFFER_TYPE_DYNAMIC_SORTING:
				break;
			default:
				WWASSERT(0);
			}
		}
		else {
			DX8CALL(SetIndices(
				nullptr));
			DX8_RECORD_INDEX_BUFFER_CHANGE();
		}
	}

	render_state_changed&=((unsigned)WORLD_IDENTITY|(unsigned)VIEW_IDENTITY);

	SNAPSHOT_SAY(("DX8Wrapper::Apply_Render_State_Changes() - finished"));
}

IDirect3DTexture9 * DX8Wrapper::_Create_DX8_Texture
(
	unsigned int width,
	unsigned int height,
	WW3DFormat format,
	MipCountType mip_level_count,
	D3DPOOL pool,
	bool rendertarget
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DTexture9 *texture = nullptr;
	IDirect3DDevice9 *d3d_device = DX8Wrapper::_Get_D3D_Device8();

	// GeneralsX @bugfix fbraz 04/05/2026 Avoid null device calls during headless replay texture requests.
	if (d3d_device == nullptr)
	{
		return nullptr;
	}

	// Paletted textures not supported!
	WWASSERT(format!=D3DFMT_P8);

	// NOTE: If 'format' is not supported as a texture format, this function will find the closest
	// format that is supported and use that instead.

	// Render target may return NOTAVAILABLE, in
	// which case we return null.
	if (rendertarget) {
		unsigned ret=D3DXCreateTexture(
			d3d_device,
			width,
			height,
			mip_level_count,
			D3DUSAGE_RENDERTARGET,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture);

		if (ret==D3DERR_NOTAVAILABLE) {
			Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
			return nullptr;
		}

		// If ran out of texture ram, try invalidating some textures and mesh cache.
		if (ret==D3DERR_OUTOFVIDEOMEMORY) {
			WWDEBUG_SAY(("Error: Out of memory while creating render target. Trying to release assets..."));
			// Free all textures that haven't been used in the last 5 seconds
			TextureClass::Invalidate_Old_Unused_Textures(5000);

			// Invalidate the mesh cache
			WW3D::_Invalidate_Mesh_Cache();

			ret=D3DXCreateTexture(
				d3d_device,
				width,
				height,
				mip_level_count,
				D3DUSAGE_RENDERTARGET,
				WW3DFormat_To_D3DFormat(format),
				pool,
				&texture);

			if (SUCCEEDED(ret)) {
				WWDEBUG_SAY(("...Render target creation successful."));
			}
			else {
				WWDEBUG_SAY(("...Render target creation failed."));
			}
			if (ret==D3DERR_OUTOFVIDEOMEMORY) {
				Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
				return nullptr;
			}
		}

		DX8_ErrorCode(ret);
		// Just return the texture, no reduction
		// allowed for render targets.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.4: stand up the Vulkan-side render-target
		// mirror (device-local COLOR_ATTACHMENT image + layout state) for the engine's color RTs.
		// Content still comes from D3D9 until wedge 5; this only proves the shape + machinery exist.
		VKRT_Alloc_Color(texture, width, height, (int)format);
#endif
		return texture;
	}

	// We should never run out of video memory when allocating a non-rendertarget texture.
	// However, it seems to happen sometimes when there are a lot of textures in memory and so
	// if it happens we'll release assets and try again (anything is better than crashing).
	unsigned ret=D3DXCreateTexture(
		d3d_device,
		width,
		height,
		mip_level_count,
		0,
		WW3DFormat_To_D3DFormat(format),
		pool,
		&texture);

	// If ran out of texture ram, try invalidating some textures and mesh cache.
	if (ret==D3DERR_OUTOFVIDEOMEMORY) {
		WWDEBUG_SAY(("Error: Out of memory while creating texture. Trying to release assets..."));
		// Free all textures that haven't been used in the last 5 seconds
		TextureClass::Invalidate_Old_Unused_Textures(5000);

		// Invalidate the mesh cache
		WW3D::_Invalidate_Mesh_Cache();

		ret=D3DXCreateTexture(
			d3d_device,
			width,
			height,
			mip_level_count,
			0,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture);
		if (SUCCEEDED(ret)) {
			WWDEBUG_SAY(("...Texture creation successful."));
		}
		else {
			StringClass format_name(0,true);
			Get_WW3D_Format_Name(format, format_name);
			WWDEBUG_SAY(("...Texture creation failed. (%d x %d, format: %s, mips: %d",width,height,format_name.str(),mip_level_count));
		}

	}
	DX8_ErrorCode(ret);

	// GeneralsX @bugfix felipebraz 10/06/2025 On non-Windows builds DX8_ErrorCode is a silent no-op
	// in non-debug builds (WWASSERT expands to nothing). Log failures to stderr so crashes can be
	// diagnosed without a debugger attached.
#if !defined(_WIN32) || defined(_WIN64)
	if (!SUCCEEDED(ret)) {
		StringClass format_name(0, true);
		Get_WW3D_Format_Name(static_cast<WW3DFormat>(format), format_name);
		fprintf(stderr, "[GeneralsX] _Create_DX8_Texture FAILED: hresult=0x%08X %dx%d fmt=%s mips=%d pool=%d rendertarget=%d\n",
			ret, (int)width, (int)height, format_name.str(), (int)mip_level_count, (int)pool, (int)rendertarget);
		fflush(stderr);
	}
#endif

	return texture;
}

IDirect3DTexture9 * DX8Wrapper::_Create_DX8_Texture
(
	const char *filename,
	MipCountType mip_level_count
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DTexture9 *texture = nullptr;

	// NOTE: If the original image format is not supported as a texture format, it will
	// automatically be converted to an appropriate format.
	// NOTE: It is possible to get the size and format of the original image file from this
	// function as well, so if we later want to second-guess D3DX's format conversion decisions
	// we can do so after this function is called..
	unsigned result = D3DXCreateTextureFromFileExA(
		_Get_D3D_Device8(),
		filename,
		D3DX_DEFAULT,
		D3DX_DEFAULT,
		mip_level_count,//create_mipmaps ? 0 : 1,
		0,
		D3DFMT_UNKNOWN,
		D3DPOOL_MANAGED,
		D3DX_FILTER_BOX,
		D3DX_FILTER_BOX,
		0,
		nullptr,
		nullptr,
		&texture);

	if (result != D3D_OK) {
		return MissingTexture::_Get_Missing_Texture();
	}

	// Make sure texture wasn't paletted!
	D3DSURFACE_DESC desc;
	texture->GetLevelDesc(0,&desc);
	if (desc.Format==D3DFMT_P8) {
		texture->Release();
		return MissingTexture::_Get_Missing_Texture();
	}
	return texture;
}

IDirect3DTexture9 * DX8Wrapper::_Create_DX8_Texture
(
	IDirect3DSurface9 *surface,
	MipCountType mip_level_count
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DTexture9 *texture = nullptr;

	D3DSURFACE_DESC surface_desc;
	::ZeroMemory(&surface_desc, sizeof(D3DSURFACE_DESC));
	surface->GetDesc(&surface_desc);

	// This function will create a texture with a different (but similar) format if the surface is
	// not in a supported texture format.
	WW3DFormat format=D3DFormat_To_WW3DFormat(surface_desc.Format);
	texture = _Create_DX8_Texture(surface_desc.Width, surface_desc.Height, format, mip_level_count);

	// Copy the surface to the texture
	IDirect3DSurface9 *tex_surface = nullptr;
	texture->GetSurfaceLevel(0, &tex_surface);
	DX8_ErrorCode(D3DXLoadSurfaceFromSurface(tex_surface, nullptr, nullptr, surface, nullptr, nullptr, D3DX_FILTER_BOX, 0));
	tex_surface->Release();

	// Create mipmaps if needed
	if (mip_level_count!=MIP_LEVELS_1)
	{
		DX8_ErrorCode(D3DXFilterTexture(texture, nullptr, 0, D3DX_FILTER_BOX));
	}

	return texture;

}

/*!
 * KJM create depth stencil texture
 */
IDirect3DTexture9 * DX8Wrapper::_Create_DX8_ZTexture
(
	unsigned int width,
	unsigned int height,
	WW3DZFormat zformat,
	MipCountType mip_level_count,
	D3DPOOL pool
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DTexture9* texture = nullptr;

	D3DFORMAT zfmt=WW3DZFormat_To_D3DFormat(zformat);

	unsigned ret=DX8Wrapper::_Get_D3D_Device8()->CreateTexture
	(
		width,
		height,
		mip_level_count,
		D3DUSAGE_DEPTHSTENCIL,
		zfmt,
		pool,
		&texture,
		nullptr
	);

	if (ret==D3DERR_NOTAVAILABLE)
	{
		Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
		return nullptr;
	}

	// If ran out of texture ram, try invalidating some textures and mesh cache.
	if (ret==D3DERR_OUTOFVIDEOMEMORY)
	{
		WWDEBUG_SAY(("Error: Out of memory while creating render target. Trying to release assets..."));
		// Free all textures that haven't been used in the last 5 seconds
		TextureClass::Invalidate_Old_Unused_Textures(5000);

		// Invalidate the mesh cache
		WW3D::_Invalidate_Mesh_Cache();

		ret=DX8Wrapper::_Get_D3D_Device8()->CreateTexture
		(
			width,
			height,
			mip_level_count,
			D3DUSAGE_DEPTHSTENCIL,
			zfmt,
			pool,
			&texture,
			nullptr
		);

		if (SUCCEEDED(ret))
		{
			WWDEBUG_SAY(("...Render target creation successful."));
		}
		else
		{
			WWDEBUG_SAY(("...Render target creation failed."));
		}
		if (ret==D3DERR_OUTOFVIDEOMEMORY)
		{
			Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
			return nullptr;
		}
	}

	DX8_ErrorCode(ret);

	texture->AddRef(); // don't release this texture

	// Just return the texture, no reduction
	// allowed for render targets.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.4: depth-texture mirror (device-local
	// DEPTH_STENCIL_ATTACHMENT image) for ZTextureClass instances. Shape/machinery only until wedge 5.
	VKRT_Alloc_Depth(texture, width, height, (int)zformat);
#endif

	return texture;
}

/*!
 * KJM create cube map texture
 */
IDirect3DCubeTexture9* DX8Wrapper::_Create_DX8_Cube_Texture
(
	unsigned int width,
	unsigned int height,
	WW3DFormat format,
	MipCountType mip_level_count,
	D3DPOOL pool,
	bool rendertarget
)
{
	WWASSERT(width==height);
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DCubeTexture9* texture=nullptr;

	// Paletted textures not supported!
	WWASSERT(format!=D3DFMT_P8);

	// NOTE: If 'format' is not supported as a texture format, this function will find the closest
	// format that is supported and use that instead.

	// Render target may return NOTAVAILABLE, in
	// which case we return null.
	if (rendertarget)
	{
		unsigned ret=D3DXCreateCubeTexture
		(
			DX8Wrapper::_Get_D3D_Device8(),
			width,
			mip_level_count,
			D3DUSAGE_RENDERTARGET,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture
		);

		if (ret==D3DERR_NOTAVAILABLE)
		{
			Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
			return nullptr;
		}

		// If ran out of texture ram, try invalidating some textures and mesh cache.
		if (ret==D3DERR_OUTOFVIDEOMEMORY)
		{
			WWDEBUG_SAY(("Error: Out of memory while creating render target. Trying to release assets..."));
			// Free all textures that haven't been used in the last 5 seconds
			TextureClass::Invalidate_Old_Unused_Textures(5000);

			// Invalidate the mesh cache
			WW3D::_Invalidate_Mesh_Cache();

			ret=D3DXCreateCubeTexture
			(
				DX8Wrapper::_Get_D3D_Device8(),
				width,
				mip_level_count,
				D3DUSAGE_RENDERTARGET,
				WW3DFormat_To_D3DFormat(format),
				pool,
				&texture
			);

			if (SUCCEEDED(ret))
			{
				WWDEBUG_SAY(("...Render target creation successful."));
			}
			else
			{
				WWDEBUG_SAY(("...Render target creation failed."));
			}
			if (ret==D3DERR_OUTOFVIDEOMEMORY)
			{
				Non_Fatal_Log_DX8_ErrorCode(ret,__FILE__,__LINE__);
				return nullptr;
			}
		}

		DX8_ErrorCode(ret);
		// Just return the texture, no reduction
		// allowed for render targets.
		return texture;
	}

	// We should never run out of video memory when allocating a non-rendertarget texture.
	// However, it seems to happen sometimes when there are a lot of textures in memory and so
	// if it happens we'll release assets and try again (anything is better than crashing).
	unsigned ret=D3DXCreateCubeTexture
	(
		DX8Wrapper::_Get_D3D_Device8(),
		width,
		mip_level_count,
		0,
		WW3DFormat_To_D3DFormat(format),
		pool,
		&texture
	);

	// If ran out of texture ram, try invalidating some textures and mesh cache.
	if (ret==D3DERR_OUTOFVIDEOMEMORY)
	{
		WWDEBUG_SAY(("Error: Out of memory while creating texture. Trying to release assets..."));
		// Free all textures that haven't been used in the last 5 seconds
		TextureClass::Invalidate_Old_Unused_Textures(5000);

		// Invalidate the mesh cache
		WW3D::_Invalidate_Mesh_Cache();

		ret=D3DXCreateCubeTexture
		(
			DX8Wrapper::_Get_D3D_Device8(),
			width,
			mip_level_count,
			0,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture
		);
		if (SUCCEEDED(ret))
		{
			WWDEBUG_SAY(("...Texture creation successful."));
		}
		else
		{
			StringClass format_name(0,true);
			Get_WW3D_Format_Name(format, format_name);
			WWDEBUG_SAY(("...Texture creation failed. (%d x %d, format: %s, mips: %d",width,height,format_name.str(),mip_level_count));
		}

	}
	DX8_ErrorCode(ret);

	return texture;
}

/*!
 * KJM create volume texture
 */
IDirect3DVolumeTexture9* DX8Wrapper::_Create_DX8_Volume_Texture
(
	unsigned int width,
	unsigned int height,
	unsigned int depth,
	WW3DFormat format,
	MipCountType mip_level_count,
	D3DPOOL pool
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	IDirect3DVolumeTexture9* texture=nullptr;

	// Paletted textures not supported!
	WWASSERT(format!=D3DFMT_P8);

	// NOTE: If 'format' is not supported as a texture format, this function will find the closest
	// format that is supported and use that instead.


	// We should never run out of video memory when allocating a non-rendertarget texture.
	// However, it seems to happen sometimes when there are a lot of textures in memory and so
	// if it happens we'll release assets and try again (anything is better than crashing).
	unsigned ret=D3DXCreateVolumeTexture
	(
		DX8Wrapper::_Get_D3D_Device8(),
		width,
		height,
		depth,
		mip_level_count,
		0,
		WW3DFormat_To_D3DFormat(format),
		pool,
		&texture
	);

	// If ran out of texture ram, try invalidating some textures and mesh cache.
	if (ret==D3DERR_OUTOFVIDEOMEMORY)
	{
		WWDEBUG_SAY(("Error: Out of memory while creating texture. Trying to release assets..."));
		// Free all textures that haven't been used in the last 5 seconds
		TextureClass::Invalidate_Old_Unused_Textures(5000);

		// Invalidate the mesh cache
		WW3D::_Invalidate_Mesh_Cache();

		ret=D3DXCreateVolumeTexture
		(
			DX8Wrapper::_Get_D3D_Device8(),
			width,
			height,
			depth,
			mip_level_count,
			0,
			WW3DFormat_To_D3DFormat(format),
			pool,
			&texture
		);
		if (SUCCEEDED(ret))
		{
			WWDEBUG_SAY(("...Texture creation successful."));
		}
		else
		{
			StringClass format_name(0,true);
			Get_WW3D_Format_Name(format, format_name);
			WWDEBUG_SAY(("...Texture creation failed. (%d x %d, format: %s, mips: %d",width,height,format_name.str(),mip_level_count));
		}

	}
	DX8_ErrorCode(ret);

	return texture;
}


IDirect3DSurface9 * DX8Wrapper::_Create_DX8_Surface(unsigned int width, unsigned int height, WW3DFormat format)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();

	IDirect3DSurface9 *surface = nullptr;

	// Paletted surfaces not supported!
	WWASSERT(format!=D3DFMT_P8);

	DX8CALL(CreateOffscreenPlainSurface(width, height, WW3DFormat_To_D3DFormat(format), D3DPOOL_SYSTEMMEM, &surface, nullptr));

	return surface;
}

IDirect3DSurface9 * DX8Wrapper::_Create_DX8_Surface(const char *filename_)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();

	// Note: Since there is no "D3DXCreateSurfaceFromFile" and no "GetSurfaceInfoFromFile" (the
	// latter is supposed to be added to D3DX in a future version), we create a texture from the
	// file (w/o mipmaps), check that its surface is equal to the original file data (which it
	// will not be if the file is not in a texture-supported format or size). If so, copy its
	// surface (we might be able to just get its surface and add a ref to it but I'm not sure so
	// I'm not going to risk it) and release the texture. If not, create a surface according to
	// the file data and use D3DXLoadSurfaceFromFile. This is a horrible hack, but it saves us
	// having to write file loaders. Will fix this when D3DX provides us with the right functions.
	// Create a surface the size of the file image data
	IDirect3DSurface9 *surface = nullptr;

	{

		file_auto_ptr myfile(_TheFileFactory,filename_);
		// If file not found, create a surface with missing texture in it

		if (!myfile->Is_Available()) {
			// If file not found, try the dds format
			// else create a surface with missing texture in it
			char compressed_name[200];
			strlcpy(compressed_name,filename_, sizeof(compressed_name));
			char *ext = strstr(compressed_name, ".");
			if ( ext && (strlen(ext)==4) &&
				  ( (ext[1] == 't') || (ext[1] == 'T') ) &&
				  ( (ext[2] == 'g') || (ext[2] == 'G') ) &&
				  ( (ext[3] == 'a') || (ext[3] == 'A') ) ) {
				ext[1]='d';
				ext[2]='d';
				ext[3]='s';
			}
			file_auto_ptr myfile2(_TheFileFactory,compressed_name);
			if (!myfile2->Is_Available())
				return MissingTexture::_Create_Missing_Surface();
		}
	}

	StringClass filename_string(filename_,true);
	surface=TextureLoader::Load_Surface_Immediate(
		filename_string,
		WW3D_FORMAT_UNKNOWN,
		true);
	return surface;
}


/***********************************************************************************************
 * DX8Wrapper::_Update_Texture -- Copies a texture from system memory to video memory          *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 *                                                                                             *
 * INPUT:                                                                                      *
 *                                                                                             *
 * OUTPUT:                                                                                     *
 *                                                                                             *
 * WARNINGS:                                                                                   *
 *                                                                                             *
 * HISTORY:                                                                                    *
 *   4/26/2001  hy : Created.                                                                  *
 *=============================================================================================*/
void DX8Wrapper::_Update_Texture(TextureClass *system, TextureClass *video)
{
	WWASSERT(system);
	WWASSERT(video);
	WWASSERT(system->Get_Pool()==TextureClass::POOL_SYSTEMMEM);
	WWASSERT(video->Get_Pool()==TextureClass::POOL_DEFAULT);
	DX8CALL(UpdateTexture(system->Peek_D3D_Base_Texture(),video->Peek_D3D_Base_Texture()));
}

// GeneralsX @feature PS3.0-P2-7 14/10/2026 I6 - Capability probe results used to be log-only, so
// no consumer could act on them. Compute_Caps now stores the depth-bias behavior check and the
// pixel-shader caps word here; the accessors in dx8wrapper.h expose them for auto-degrade paths.
static bool s_depthBiasAvailable=false;
static unsigned s_ps20CapsFlags=0;

bool DX8Wrapper::Is_Depth_Bias_Available()
{
	return s_depthBiasAvailable;
}

unsigned DX8Wrapper::Get_PS20_Caps_Flags()
{
	return s_ps20CapsFlags;
}

void DX8Wrapper::Compute_Caps(WW3DFormat display_format)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	delete CurrentCaps;
	CurrentCaps=new DX8Caps(_Get_D3D8(),D3DDevice,display_format,Get_Current_Adapter_Identifier());

	// GeneralsX @feature PS3.0 batch1 12/10/2026 derive the addressable shader constant budgets from
	// the caps report. Pixel: DX8Caps already maps the shader model onto the spec count
	// (ps_1_x 8, ps_2_x 32, ps_3_0 224) - D3DCAPS9 has no pixel field of its own - and the historic
	// 8-register budget is kept as the floor so nothing that works today starts getting refused.
	// Vertex: use the device count, floored at the legacy 96 for the same reason and clamped to the
	// cache size.
	unsigned pixelBudget=CurrentCaps->Get_Max_Pixel_Shader_Constants();
	if (pixelBudget<MAX_PIXEL_SHADER_CONSTANTS_PS1X) pixelBudget=MAX_PIXEL_SHADER_CONSTANTS_PS1X;
	if (pixelBudget>MAX_PIXEL_SHADER_CONSTANTS) pixelBudget=MAX_PIXEL_SHADER_CONSTANTS;
	Pixel_Shader_Constant_Limit=pixelBudget;

	unsigned vertexBudget=CurrentCaps->Get_Max_Vertex_Shader_Constants();
	if (vertexBudget<MAX_VERTEX_SHADER_CONSTANTS_PS1X) vertexBudget=MAX_VERTEX_SHADER_CONSTANTS_PS1X;
	if (vertexBudget>MAX_VERTEX_SHADER_CONSTANTS) vertexBudget=MAX_VERTEX_SHADER_CONSTANTS;
	Vertex_Shader_Constant_Limit=vertexBudget;

	// GeneralsX @feature PS3.0 batch1 12/10/2026 one-time console record: these are the numbers that
	// decide which shader-model permutation a render path may take, and they had never been printed.
	{
		static bool s_budgetLogged=false;
		if (!s_budgetLogged) {
			s_budgetLogged=true;
			fprintf(stderr,"[SHADER_BUDGET] PS v%d.%d VS v%d.%d -> pixel constants %u (device %d), "
				"vertex constants %u (device %d), samplers %d\n",
				CurrentCaps->Get_Pixel_Shader_Major_Version(),CurrentCaps->Get_Pixel_Shader_Minor_Version(),
				CurrentCaps->Get_Vertex_Shader_Major_Version(),CurrentCaps->Get_Vertex_Shader_Minor_Version(),
				Pixel_Shader_Constant_Limit,CurrentCaps->Get_Max_Pixel_Shader_Constants(),
				Vertex_Shader_Constant_Limit,CurrentCaps->Get_Max_Vertex_Shader_Constants(),
				CurrentCaps->Get_Max_Simultaneous_Textures());
			fflush(stderr);
		}
	}

	// GeneralsX @feature PS3.0 batch7 13/10/2026 the rest of the ps_3_0 capability picture, recorded once
	// because a later content batch has to stay inside these numbers:
	//   flow-control depth   - how deep an if/loop nest the shader may use (the spec ceiling is 24)
	//   temps                - temporary registers; ps_2_0 gives 2 usable, the spec ceiling is 32
	//   instruction slots    - the static instruction budget
	//   PS20Caps option bits - GRADIENTINSTRUCTIONS is what lets a shader take ddx/ddy (tangent-space and
	//                          screen-space work), NODEPENDENTREADLIMIT is what lets a PCF shadow loop
	//                          issue unbounded dependent texture reads, ARBITRARYSWIZZLE matters for the
	//                          single-channel depth reads a shadow map does.
	// DXVK and native d3d9 report these as zero-filled on some drivers, which is itself the answer to
	// "is this capability advertised", so the raw numbers are printed rather than a pass/fail.
	{
		static bool s_sm3Logged=false;
		if (!s_sm3Logged) {
			s_sm3Logged=true;
			const D3DCAPS9& c=CurrentCaps->Get_DX8_Caps();
			unsigned psCaps=c.PS20Caps.Caps;
			// GeneralsX @feature PS3.0-P2-7 14/10/2026 I6: publish the probe for callers (Get_PS20_Caps_Flags).
			s_ps20CapsFlags=psCaps;
			fprintf(stderr,"[SM3_CAPS] PS flow(dynamic %d, static %d) temps %d slots %d | "
				"VS flow(dynamic %d, static %d) temps %d | SM3 slots PS %d VS %d | "
				"PS options%s%s%s%s%s\n",
				c.PS20Caps.DynamicFlowControlDepth,c.PS20Caps.StaticFlowControlDepth,
				c.PS20Caps.NumTemps,c.PS20Caps.NumInstructionSlots,
				c.VS20Caps.DynamicFlowControlDepth,c.VS20Caps.StaticFlowControlDepth,c.VS20Caps.NumTemps,
				(int)c.MaxPixelShader30InstructionSlots,(int)c.MaxVertexShader30InstructionSlots,
				(psCaps & D3DPS20CAPS_ARBITRARYSWIZZLE)?" swizzle":"",
				(psCaps & D3DPS20CAPS_GRADIENTINSTRUCTIONS)?" gradients":"",
				(psCaps & D3DPS20CAPS_PREDICATION)?" predication":"",
				(psCaps & D3DPS20CAPS_NODEPENDENTREADLIMIT)?" unboundedTextureReads":"",
				(psCaps & D3DPS20CAPS_NOTEXINSTRUCTIONLIMIT)?" unboundedTexInstructions":"");
			fflush(stderr);
		}
	}

	// GeneralsX @feature PS3.0 batch7 13/10/2026 depth bias is the capability a shadow-map pass needs and
	// the one the DX8 API never had (D3DRS_ZBIAS is gone in d3d9, see Set_DX8_ZBias). Ask the device for
	// both float bias states and read them straight back from the device, not from the wrapper cache, so
	// this line proves the runtime accepts them; the cached setters keep RenderStates[] and the mirror
	// members in sync on the way in and back out to zero.
	{
		static bool s_depthBiasLogged=false;
		if (!s_depthBiasLogged) {
			s_depthBiasLogged=true;
			const float testSlope=1.5f;
			const float testConstant=-8.0f;
			Set_Slope_Scale_Depth_Bias(testSlope);
			Set_Depth_Bias(testConstant);

			DWORD echoSlope=0, echoConstant=0;
			DX8CALL(GetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,&echoSlope));
			DX8CALL(GetRenderState(D3DRS_DEPTHBIAS,&echoConstant));
			const float* asFloatSlope=(const float*)&echoSlope;
			const float* asFloatConstant=(const float*)&echoConstant;

			fprintf(stderr,"[DEPTH_BIAS] asked slope %.3f / constant %.3f, device echoes %.3f / %.3f -> %s\n",
				testSlope,testConstant,*asFloatSlope,*asFloatConstant,
				(*asFloatSlope==testSlope && *asFloatConstant==testConstant)?"accepted":"NOT echoed - treat as unavailable");
			// GeneralsX @feature PS3.0-P2-7 14/10/2026 I6: publish the probe for callers (Is_Depth_Bias_Available).
			s_depthBiasAvailable=(*asFloatSlope==testSlope && *asFloatConstant==testConstant);

			Set_Slope_Scale_Depth_Bias(0.0f);
			Set_Depth_Bias(0.0f);
			fflush(stderr);
		}
	}
}

// GeneralsX @feature PS3.0 batch1 12/10/2026 throttled so a shader that keeps asking for a register
// outside the budget cannot flood the console; the first offenders are what needs fixing.
void DX8Wrapper::Report_Shader_Constant_Range(const char* setter,int reg,int count)
{
	static int s_reportCount=0;
	if (s_reportCount<16) {
		++s_reportCount;
		fprintf(stderr,"[SHADER_CONST_RANGE] %s rejected reg=%d count=%d (pixel limit %u, vertex limit %u)\n",
			setter?setter:"?",reg,count,Pixel_Shader_Constant_Limit,Vertex_Shader_Constant_Limit);
		fflush(stderr);
	}
}


void DX8Wrapper::Set_Light(unsigned index, const D3DLIGHT9* light)
{
	// GeneralsX @feature PS3.0-P3-18 14/10/2026: guard the widened slot range (the old code relied on
	// every caller staying below 4; now MAX_LIGHTS=8 flows here straight from LightEnvironment).
	if (index >= 8) {
		WWASSERT(index < 8);
		return;
	}
	if (light) {
		render_state.Lights[index]=*light;
		render_state.LightEnable[index]=true;
	}
	else {
		render_state.LightEnable[index]=false;
	}
	render_state_changed|=Get_Light_Changed_Bit(index);
}

void DX8Wrapper::Set_Light(unsigned index,const LightClass &light)
{
	D3DLIGHT9 dlight;
	Vector3 temp;
	memset(&dlight,0,sizeof(D3DLIGHT9));

	switch (light.Get_Type())
	{
	case LightClass::POINT:
		{
			dlight.Type=D3DLIGHT_POINT;
		}
		break;
	case LightClass::DIRECTIONAL:
		{
			dlight.Type=D3DLIGHT_DIRECTIONAL;
		}
		break;
	case LightClass::SPOT:
		{
			dlight.Type=D3DLIGHT_SPOT;
		}
		break;
	}

	light.Get_Diffuse(&temp);
	temp*=light.Get_Intensity();
	dlight.Diffuse.r=temp.X;
	dlight.Diffuse.g=temp.Y;
	dlight.Diffuse.b=temp.Z;
	dlight.Diffuse.a=1.0f;

	light.Get_Specular(&temp);
	temp*=light.Get_Intensity();
	dlight.Specular.r=temp.X;
	dlight.Specular.g=temp.Y;
	dlight.Specular.b=temp.Z;
	dlight.Specular.a=1.0f;

	light.Get_Ambient(&temp);
	temp*=light.Get_Intensity();
	dlight.Ambient.r=temp.X;
	dlight.Ambient.g=temp.Y;
	dlight.Ambient.b=temp.Z;
	dlight.Ambient.a=1.0f;

	temp=light.Get_Position();
	dlight.Position=*(D3DVECTOR*) &temp;

	light.Get_Spot_Direction(temp);
	dlight.Direction=*(D3DVECTOR*) &temp;

	dlight.Range=light.Get_Attenuation_Range();
	dlight.Falloff=light.Get_Spot_Exponent();
	dlight.Theta=light.Get_Spot_Angle();
	dlight.Phi=light.Get_Spot_Angle();

	// Inverse linear light 1/(1+D)
	double a,b;
	light.Get_Far_Attenuation_Range(a,b);
	dlight.Attenuation0=1.0f;
	if (fabs(a-b)<1e-5)
		// if the attenuation range is too small assume uniform with cutoff
		dlight.Attenuation1=0.0f;
	else
		// this will cause the light to drop to half intensity at the first far attenuation
		dlight.Attenuation1=(float) 1.0/a;
	dlight.Attenuation2=0.0f;

	Set_Light(index,&dlight);
}

//**********************************************************************************************
//! Set the light environment. This is a lighting model which used up to four
//! directional lights to produce the lighting.
/*! 5/27/02 KJM Added shader light environment support
*/
void DX8Wrapper::Set_Light_Environment(LightEnvironmentClass* light_env)
{
	// Shader light environment support															*
	Light_Environment=light_env;

	if (light_env)
	{
		// GeneralsX @feature PS3.0-P3-18 14/10/2026: LightEnvironment can now hand us up to 8 lights.
		// The fixed-function path stays inside what the device advertises: MaxActiveLights is read once
		// and cached (4 if the caps query ever fails). Anything above the cap keeps retail's fate under
		// the old 4-slot world: its ambient is already inside D3DRS_AMBIENT via Get_Equivalent_Ambient,
		// only the per-unit diffuse term is dropped, and lenv's sort keeps that for the strongest ones.
		static int s_maxFFLights = -1;
		if (s_maxFFLights < 0) {
			s_maxFFLights = 4;
			IDirect3DDevice9* dev = _Get_D3D_Device8();
			if (dev != nullptr) {
				D3DCAPS9 caps;
				if (SUCCEEDED(dev->GetDeviceCaps(&caps))) {
					s_maxFFLights = (caps.MaxActiveLights >= 8) ? 8 : ((caps.MaxActiveLights >= 4) ? (int)caps.MaxActiveLights : 4);
				}
			}
			fprintf(stderr, "[FFLIGHT] fixed-function light unit cap = %d\n", s_maxFFLights);
			fflush(stderr);
		}
		int light_count = light_env->Get_Light_Count();
		if (light_count > s_maxFFLights) light_count = s_maxFFLights;
		unsigned int color=Convert_Color(light_env->Get_Equivalent_Ambient(),0.0f);
		if (RenderStates[D3DRS_AMBIENT]!=color)
		{
			Set_DX8_Render_State(D3DRS_AMBIENT,color);
//buggy Radeon 9700 driver doesn't apply new ambient unless the material also changes.
#if 1
			render_state_changed|=MATERIAL_CHANGED;
#endif
		}

		D3DLIGHT9 light;
		int l=0;
		for (;l<light_count;++l) {

			::ZeroMemory(&light, sizeof(D3DLIGHT9));

			light.Type=D3DLIGHT_DIRECTIONAL;
			(Vector3&)light.Diffuse=light_env->Get_Light_Diffuse(l);
			Vector3 dir=-light_env->Get_Light_Direction(l);
			light.Direction=(const D3DVECTOR&)(dir);

			// (gth) TODO: put specular into LightEnvironment?  Much work to be done on lights :-)'
			if (l==0) {
				light.Specular.r = light.Specular.g = light.Specular.b = 1.0f;
			}

			if (light_env->isPointLight(l)) {
				light.Type = D3DLIGHT_POINT;
				(Vector3&)light.Diffuse=light_env->getPointDiffuse(l);
				(Vector3&)light.Ambient=light_env->getPointAmbient(l);
				light.Position = (const D3DVECTOR&)light_env->getPointCenter(l);
				light.Range = light_env->getPointOrad(l);

				// Inverse linear light 1/(1+D)
				double a,b;
				b = light_env->getPointOrad(l);
				a = light_env->getPointIrad(l);

//(gth) CNC3 Generals code for the attenuation factors is causing the lights to over-brighten
//I'm changing the Attenuation0 parameter to 1.0 to avoid this problem.
#if 0
				light.Attenuation0=0.01f;
#else
				light.Attenuation0=1.0f;
#endif
				if (fabs(a-b)<1e-5)
					// if the attenuation range is too small assume uniform with cutoff
					light.Attenuation1=0.0f;
				else
					// this will cause the light to drop to half intensity at the first far attenuation
					light.Attenuation1=(float) 0.1/a;

				light.Attenuation2=8.0f/(b*b);
			}

			Set_Light(l,&light);
		}

		for (;l<8;++l) {	// GeneralsX @feature PS3.0-P3-18 14/10/2026: disable the widened unit range
			Set_Light(l,nullptr);
		}
	}
/*	else {
		for (int l=0;l<4;++l) {
			Set_Light(l,nullptr);
		}
	}
*/
}

IDirect3DSurface9 * DX8Wrapper::_Get_DX8_Front_Buffer()
{
	DX8_THREAD_ASSERT();
	D3DDISPLAYMODE mode;

	DX8CALL(GetDisplayMode(0,&mode));

	IDirect3DSurface9 * fb=nullptr;

	DX8CALL(CreateOffscreenPlainSurface(mode.Width,mode.Height,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&fb,nullptr));

	// GeneralsX @build d3d9 port 11/10/2026 - GetFrontBuffer replaced by GetFrontBufferData(swapchain, dest)
	DX8CALL(GetFrontBufferData(0,fb));
	return fb;
}

SurfaceClass * DX8Wrapper::_Get_DX8_Back_Buffer(unsigned int num)
{
	DX8_THREAD_ASSERT();

	IDirect3DSurface9 * bb;
	SurfaceClass *surf=nullptr;
	DX8CALL(GetBackBuffer(0,num,D3DBACKBUFFER_TYPE_MONO,&bb));
	if (bb)
	{
		surf=NEW_REF(SurfaceClass,(bb));
		bb->Release();
	}

	return surf;
}


// GeneralsX @feature PS3.0-P3-4 14/10/2026 Both Create_Render_Target overloads used to run every
// request through Find_POT and then collapse width and height onto that single value, so a 1680x1050
// shadow map or an off-res post-process target silently became a 1024x1024 square - the caller kept
// projecting into the box it asked for and got a cropped, rescaled result with no diagnostic. Modern
// D3D9 runtimes (DXVK included) place no such requirement on render targets: the retail RTT
// post-process and the back-buffer copy in this very file already create native-size, non-power-of-two
// targets directly through the device. The clamp is now driven by what the device actually declares:
//   - D3DPTEXTURECAPS_POW2      : every texture, RT included, must be power-of-two
//   - D3DPTEXTURECAPS_SQUAREONLY: RTs must additionally be square
// and the dimension cap per axis is applied separately instead of through the shared square value.
// GENERALSX_RT_POT=legacy restores the retail clamp for one process, which is how the A/B evidence
// legs in 光影改造二阶段/实测对比 are produced.
static void Apply_Render_Target_Size_Clamp(int& width,int& height)
{
	const D3DCAPS9& dx8caps=DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps();

	static bool legacyModeQueried=false;
	static bool legacyMode=false;
	if (!legacyModeQueried)
	{
		legacyModeQueried=true;
		const char* env=getenv("GENERALSX_RT_POT");
		legacyMode=(env!=nullptr && stricmp(env,"legacy")==0);
		fprintf(stderr,"[RTPOT] render target sizing: %s (texCaps=0x%08x pot=%d square=%d max=%ux%u)\n",
			legacyMode?"retail square POT clamp":"device-declared (non-POT allowed)",
			(unsigned)dx8caps.TextureCaps,
			(dx8caps.TextureCaps&D3DPTEXTURECAPS_POW2)?1:0,
			(dx8caps.TextureCaps&D3DPTEXTURECAPS_SQUAREONLY)?1:0,
			(unsigned)dx8caps.MaxTextureWidth,(unsigned)dx8caps.MaxTextureHeight);
		fflush(stderr);
	}

	const bool potRequired=legacyMode || !!(dx8caps.TextureCaps&D3DPTEXTURECAPS_POW2);
	const bool squareRequired=legacyMode || !!(dx8caps.TextureCaps&D3DPTEXTURECAPS_SQUAREONLY);

	if (potRequired || squareRequired)
	{
		// Keep the retail behaviour: shrink to the smallest axis, then to a power of two when asked.
		float poweroftwosize=width;
		if (height>0 && height<width) poweroftwosize=height;
		if (potRequired) poweroftwosize=::Find_POT(poweroftwosize);
		width=height=(int)poweroftwosize;
	}

	if (width>dx8caps.MaxTextureWidth) width=dx8caps.MaxTextureWidth;
	if (height>dx8caps.MaxTextureHeight) height=dx8caps.MaxTextureHeight;
	if (width<1) width=1;
	if (height<1) height=1;
}

TextureClass *
DX8Wrapper::Create_Render_Target (int width, int height, WW3DFormat format)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	DX8_RECORD_DX8_CALLS();

	// Use the current display format if format isn't specified
	if (format==WW3D_FORMAT_UNKNOWN) {
		D3DDISPLAYMODE mode;
		DX8CALL(GetDisplayMode(0,&mode));
		format=D3DFormat_To_WW3DFormat(mode.Format);
	}

	// If render target format isn't supported return null
	if (!Get_Current_Caps()->Support_Render_To_Texture_Format(format)) {
		WWDEBUG_SAY(("DX8Wrapper - Render target format is not supported"));
		return nullptr;
	}

	//
	//	Note: sized through the device-declared render-target constraints (see the helper).
	//
	Apply_Render_Target_Size_Clamp(width,height);	//
	//	Attempt to create the render target
	//
	TextureClass * tex = NEW_REF(TextureClass,(width,height,format,MIP_LEVELS_1,TextureClass::POOL_DEFAULT,true));

	// 3dfx drivers are lying in the CheckDeviceFormat call and claiming
	// that they support render targets!
	if (tex->Peek_D3D_Base_Texture() == nullptr)
	{
		WWDEBUG_SAY(("DX8Wrapper - Render target creation failed!"));
		REF_PTR_RELEASE(tex);
	}

	return tex;
}

//**********************************************************************************************
//! Create render target with associated depth stencil buffer
/*! KJM
*/
void DX8Wrapper::Create_Render_Target
(
	int width,
	int height,
	WW3DFormat format,
	WW3DZFormat zformat,
	TextureClass** target,
	ZTextureClass** depth_buffer
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();
	DX8_RECORD_DX8_CALLS();

	// Use the current display format if format isn't specified
	if (format==WW3D_FORMAT_UNKNOWN)
	{
		*target=nullptr;
		*depth_buffer=nullptr;
		return;
/*		D3DDISPLAYMODE mode;
		DX8CALL(GetDisplayMode(0,&mode));
		format=D3DFormat_To_WW3DFormat(mode.Format);*/
	}

	// If render target format isn't supported return null
	if (!Get_Current_Caps()->Support_Render_To_Texture_Format(format) ||
		 !Get_Current_Caps()->Support_Depth_Stencil_Format(zformat))
	{
		WWDEBUG_SAY(("DX8Wrapper - Render target with depth format is not supported"));
		return;
	}

	//	Note: sized through the device-declared render-target constraints (see the helper above).
	Apply_Render_Target_Size_Clamp(width,height);

	//	Attempt to create the render target
	TextureClass* tex=NEW_REF(TextureClass,(width,height,format,MIP_LEVELS_1,TextureClass::POOL_DEFAULT,true));

	// 3dfx drivers are lying in the CheckDeviceFormat call and claiming
	// that they support render targets!
	if (tex->Peek_D3D_Base_Texture() == nullptr)
	{
		WWDEBUG_SAY(("DX8Wrapper - Render target creation failed!"));
		REF_PTR_RELEASE(tex);
	}

	*target=tex;

	// attempt to create the depth stencil buffer
	*depth_buffer=NEW_REF
	(
		ZTextureClass,
		(
			width,
			height,
			zformat,
			MIP_LEVELS_1,
			TextureClass::POOL_DEFAULT
		)
	);
}

/*!
 * Set render target
 * KM Added optional custom z target
 */
void DX8Wrapper::Set_Render_Target_With_Z
(
	TextureClass* texture,
	ZTextureClass* ztexture
)
{
	WWASSERT(texture!=nullptr);
	IDirect3DSurface9 * d3d_surf = texture->Get_D3D_Surface_Level();
	WWASSERT(d3d_surf != nullptr);

	IDirect3DSurface9* d3d_zbuf=nullptr;
	if (ztexture!=nullptr)
	{

		d3d_zbuf=ztexture->Get_D3D_Surface_Level();
		WWASSERT(d3d_zbuf!=nullptr);
		Set_Render_Target(d3d_surf,d3d_zbuf);
		d3d_zbuf->Release();
	}
	else
	{
		Set_Render_Target(d3d_surf,true);
	}
	d3d_surf->Release();

	IsRenderToTexture = true;
}

void
DX8Wrapper::Set_Render_Target(IDirect3DSwapChain9 *swap_chain)
{
	DX8_THREAD_ASSERT();
	WWASSERT (swap_chain != nullptr);

	//
	//	Get the back buffer for the swap chain
	//
	LPDIRECT3DSURFACE9 render_target = nullptr;
	swap_chain->GetBackBuffer (0, D3DBACKBUFFER_TYPE_MONO, &render_target);

	//
	//	Set this back buffer as the render target
	//
	Set_Render_Target (render_target, true);

	//
	//	Release our hold on the back buffer
	//
	if (render_target != nullptr) {
		render_target->Release ();
		render_target = nullptr;
	}

	IsRenderToTexture = false;
}

void
DX8Wrapper::Set_Render_Target(IDirect3DSurface9 *render_target, bool use_default_depth_buffer)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();

	//
	//	Should we restore the default render target set a new one?
	//
	if (render_target == nullptr || render_target == DefaultRenderTarget)
	{
		// If there is currently a custom render target, default must NOT be null.
		if (CurrentRenderTarget)
		{
			WWASSERT(DefaultRenderTarget!=nullptr);
		}

		//
		//	Restore the default render target
		//
		if (DefaultRenderTarget != nullptr)
		{
			DX8CALL(SetRenderTarget (0, DefaultRenderTarget));
			DX8CALL(SetDepthStencilSurface(DefaultDepthBuffer));
			DefaultRenderTarget->Release ();
			DefaultRenderTarget = nullptr;
			if (DefaultDepthBuffer)
			{
				DefaultDepthBuffer->Release ();
				DefaultDepthBuffer = nullptr;
			}
		}

		//
		//	Release our hold on the "current" render target
		//
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->Release ();
			CurrentRenderTarget = nullptr;
		}

		if (CurrentDepthBuffer!=nullptr)
		{
			CurrentDepthBuffer->Release();
			CurrentDepthBuffer=nullptr;
		}

	}
	else if (render_target != CurrentRenderTarget)
	{
		WWASSERT(DefaultRenderTarget==nullptr);

		//
		//	We'll need the depth buffer later...
		//
		if (DefaultDepthBuffer == nullptr)
		{
//		IDirect3DSurface9 *depth_buffer = nullptr;
			DX8CALL(GetDepthStencilSurface (&DefaultDepthBuffer));
		}

		//
		//	Get a pointer to the default render target (if necessary)
		//
		if (DefaultRenderTarget == nullptr)
		{
			DX8CALL(GetRenderTarget (0,&DefaultRenderTarget));
		}

		//
		//	Release our hold on the old "current" render target
		//
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->Release ();
			CurrentRenderTarget = nullptr;
		}

		if (CurrentDepthBuffer!=nullptr)
		{
			CurrentDepthBuffer->Release();
			CurrentDepthBuffer=nullptr;
		}

		//
		//	Keep a copy of the current render target (for housekeeping)
		//
		CurrentRenderTarget = render_target;
		WWASSERT (CurrentRenderTarget != nullptr);
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->AddRef ();

			//
			//	Switch render targets
			//
			if (use_default_depth_buffer)
			{
				DX8CALL(SetRenderTarget (0, CurrentRenderTarget));
			DX8CALL(SetDepthStencilSurface(DefaultDepthBuffer));
			}
			else
			{
				DX8CALL(SetRenderTarget (0, CurrentRenderTarget));
			DX8CALL(SetDepthStencilSurface(nullptr));
			}
		}
	}

	//
	//	Free our hold on the depth buffer
	//
//	if (depth_buffer != nullptr) {
//		depth_buffer->Release ();
//		depth_buffer = nullptr;
//	}

	IsRenderToTexture = false;
}


//**********************************************************************************************
//! Set render target with depth stencil buffer
/*! KJM
*/
void DX8Wrapper::Set_Render_Target
(
	IDirect3DSurface9* render_target,
	IDirect3DSurface9* depth_buffer
)
{
	DX8_THREAD_ASSERT();
	DX8_Assert();

	//
	//	Should we restore the default render target set a new one?
	//
	if (render_target == nullptr || render_target == DefaultRenderTarget)
	{
		// If there is currently a custom render target, default must NOT be null.
		if (CurrentRenderTarget)
		{
			WWASSERT(DefaultRenderTarget!=nullptr);
		}

		//
		//	Restore the default render target
		//
		if (DefaultRenderTarget != nullptr)
		{
			DX8CALL(SetRenderTarget (0, DefaultRenderTarget));
			DX8CALL(SetDepthStencilSurface(DefaultDepthBuffer));
			DefaultRenderTarget->Release ();
			DefaultRenderTarget = nullptr;
			if (DefaultDepthBuffer)
			{
				DefaultDepthBuffer->Release ();
				DefaultDepthBuffer = nullptr;
			}
		}

		//
		//	Release our hold on the "current" render target
		//
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->Release ();
			CurrentRenderTarget = nullptr;
		}

		if (CurrentDepthBuffer!=nullptr)
		{
			CurrentDepthBuffer->Release();
			CurrentDepthBuffer=nullptr;
		}
	}
	else if (render_target != CurrentRenderTarget)
	{
		WWASSERT(DefaultRenderTarget==nullptr);

		//
		//	We'll need the depth buffer later...
		//
		if (DefaultDepthBuffer == nullptr)
		{
//		IDirect3DSurface9 *depth_buffer = nullptr;
			DX8CALL(GetDepthStencilSurface (&DefaultDepthBuffer));
		}

		//
		//	Get a pointer to the default render target (if necessary)
		//
		if (DefaultRenderTarget == nullptr)
		{
			DX8CALL(GetRenderTarget (0,&DefaultRenderTarget));
		}

		//
		//	Release our hold on the old "current" render target
		//
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->Release ();
			CurrentRenderTarget = nullptr;
		}

		if (CurrentDepthBuffer!=nullptr)
		{
			CurrentDepthBuffer->Release();
			CurrentDepthBuffer=nullptr;
		}

		//
		//	Keep a copy of the current render target (for housekeeping)
		//
		CurrentRenderTarget = render_target;
		CurrentDepthBuffer = depth_buffer;
		WWASSERT (CurrentRenderTarget != nullptr);
		if (CurrentRenderTarget != nullptr)
		{
			CurrentRenderTarget->AddRef ();
			CurrentDepthBuffer->AddRef();

			//
			//	Switch render targets
			//
			DX8CALL(SetRenderTarget (0, CurrentRenderTarget));
			DX8CALL(SetDepthStencilSurface(CurrentDepthBuffer));
		}
	}

	IsRenderToTexture=true;
}


IDirect3DSwapChain9 *
DX8Wrapper::Create_Additional_Swap_Chain (HWND render_window)
{
	DX8_Assert();

	//
	//	Configure the presentation parameters for a windowed render target
	//
	D3DPRESENT_PARAMETERS params				= { 0 };
	params.BackBufferFormat						= _PresentParameters.BackBufferFormat;
	params.BackBufferCount						= 1;
	params.MultiSampleType						= D3DMULTISAMPLE_NONE;
	params.SwapEffect								= D3DSWAPEFFECT_COPY;	// GeneralsX @build d3d9 port 11/10/2026 - D3DSWAPEFFECT_COPY_VSYNC removed in D3D9
	params.hDeviceWindow							= render_window;
	params.Windowed								= TRUE;
	params.EnableAutoDepthStencil				= TRUE;
	params.AutoDepthStencilFormat				= _PresentParameters.AutoDepthStencilFormat;
	params.Flags									= 0;
	params.FullScreen_RefreshRateInHz		= D3DPRESENT_RATE_DEFAULT;
	params.PresentationInterval	= D3DPRESENT_INTERVAL_DEFAULT;

	//
	//	Create the swap chain
	//
	IDirect3DSwapChain9 *swap_chain = nullptr;
	DX8CALL(CreateAdditionalSwapChain(&params, &swap_chain));
	return swap_chain;
}

void DX8Wrapper::Flush_DX8_Resource_Manager(unsigned int bytes)
{
	DX8_Assert();
	// GeneralsX @build d3d9 port 11/10/2026 - IDirect3DResourceManager9 was removed in D3D9;
	// the runtime implicitly restores/discards D3DPOOL_DEFAULT resources across device Reset.
	// Callers (e.g. buffer-creation retry paths) still invalidate the mesh cache themselves.
	(void)bytes;
}

unsigned int DX8Wrapper::Get_Free_Texture_RAM()
{
	DX8_Assert();
	DX8_RECORD_DX8_CALLS();
	return DX8Wrapper::_Get_D3D_Device8()->GetAvailableTextureMem();
}

// Converts a linear gamma ramp to one that is controlled by:
// Gamma - controls the curvature of the middle of the curve
// Bright - controls the minimum value of the curve
// Contrast - controls the difference between the maximum and the minimum of the curve
void DX8Wrapper::Set_Gamma(float gamma,float bright,float contrast,bool calibrate,bool uselimit)
{
	gamma=Bound(gamma,0.6f,6.0f);
	bright=Bound(bright,-0.5f,0.5f);
	contrast=Bound(contrast,0.5f,2.0f);
	float oo_gamma=1.0f/gamma;

	DX8_Assert();
	DX8_RECORD_DX8_CALLS();

	DWORD flag=(calibrate?D3DSGR_CALIBRATE:D3DSGR_NO_CALIBRATION);

	D3DGAMMARAMP ramp;
	float			 limit;

	// IML: I'm not really sure what the intent of the 'limit' variable is. It does not produce useful results for my purposes.
	if (uselimit) {
		limit=(contrast-1)/2*contrast;
	} else {
		limit = 0.0f;
	}

	// HY - arrived at this equation after much trial and error.
	for (int i=0; i<256; i++) {
		float in,out;
		in=i/256.0f;
		float x=in-limit;
		x=Bound(x,0.0f,1.0f);
		x=powf(x,oo_gamma);
		out=contrast*x+bright;
		out=Bound(out,0.0f,1.0f);
		ramp.red[i]=(WORD) (out*65535);
		ramp.green[i]=(WORD) (out*65535);
		ramp.blue[i]=(WORD) (out*65535);
	}

	if (Get_Current_Caps()->Support_Gamma())	{
		DX8Wrapper::_Get_D3D_Device8()->SetGammaRamp(0,flag,&ramp);
	}
	// GeneralsX @build fbraz 10/02/2026 GDI gamma fallback Windows-only
	// Linux: Gamma adjustment via device only (no GDI fallback)
#ifdef _WIN32
	else {
		HWND hwnd = GetDesktopWindow();
		HDC hdc = GetDC(hwnd);
		if (hdc)
		{
			SetDeviceGammaRamp (hdc, &ramp);
			ReleaseDC (hwnd, hdc);
		}
	}
#endif
}

namespace wrapper
{
void D3DMatrixIdentity(D3DMATRIX* dxm)
{
	memset(dxm, 0, sizeof(*dxm));
	dxm->_11 = 1.0f;
	dxm->_22 = 1.0f;
	dxm->_33 = 1.0f;
	dxm->_44 = 1.0f;
}
} // namespace wrapper

void DX8Wrapper::Set_World_Identity()
{
	if (render_state_changed&(unsigned)WORLD_IDENTITY)
		return;
	// GeneralsX @build fbraz 10/02/2026 Use Matrix4x4 native method
	render_state.world.Make_Identity();
	render_state_changed|=(unsigned)WORLD_CHANGED|(unsigned)WORLD_IDENTITY;
}

void DX8Wrapper::Set_View_Identity()
{
	if (render_state_changed&(unsigned)VIEW_IDENTITY)
		return;
	// GeneralsX @build fbraz 10/02/2026 Use Matrix4x4 native method
	render_state.view.Make_Identity();
	render_state_changed|=(unsigned)VIEW_CHANGED|(unsigned)VIEW_IDENTITY;
}

//**********************************************************************************************
//! Resets render device to default state
/*!
*/
void DX8Wrapper::Apply_Default_State()
{
	SNAPSHOT_SAY(("DX8Wrapper::Apply_Default_State()"));

	// only set states used in game
	Set_DX8_Render_State(D3DRS_ZENABLE, TRUE);
//	Set_DX8_Render_State(D3DRS_FILLMODE, D3DFILL_SOLID);
	Set_DX8_Render_State(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
	//Set_DX8_Render_State(D3DRS_LINEPATTERN, 0);
	Set_DX8_Render_State(D3DRS_ZWRITEENABLE, TRUE);
	Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_LASTPIXEL, FALSE);
	Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_ONE);
	Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_CW);
	Set_DX8_Render_State(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
	Set_DX8_Render_State(D3DRS_ALPHAREF, 0);
	Set_DX8_Render_State(D3DRS_ALPHAFUNC, D3DCMP_LESSEQUAL);
	// GeneralsX @feature PS3.0-P3-1 14/10/2026 I1: dither was hard-off. D3D only applies it when a
	// render target below 8 bits/channel is written, so on the usual 32-bit backbuffer this is a
	// no-op, while any 16-bit target (legacy D16 depth paths, r5g5b5 experiments) gets banding relief
	// for free. GENERALSX_DITHER=0 restores the legacy OFF state.
	{
		const char* ditherEnv = getenv("GENERALSX_DITHER");
		const DWORD ditherOn = (ditherEnv != nullptr && ditherEnv[0]=='0') ? FALSE : TRUE;
		Set_DX8_Render_State(D3DRS_DITHERENABLE, ditherOn);
	}
	// GeneralsX @feature PS3.0-P3-1 14/10/2026 I1: alpha-to-coverage was tried here and is NOT possible
	// through this stack: D3DRS_ALPHATOCONVERENABLE is a D3D8-era render state that the D3D9 headers
	// (Windows Kits 10.0.26100 d3d9types.h, verified by grep) no longer declare at all, so the enum
	// itself does not compile. D3D9 only reaches coverage-modulated alpha through per-sample masks or
	// SSAA, which is I2's territory. Recorded as skipped in the phase-2 ledger; MSAA quality tier and
	// dither above remain the landed half of I1.
	Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_FOGENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_SPECULARENABLE, FALSE);
//	Set_DX8_Render_State(D3DRS_ZVISIBLE, FALSE);
//	Set_DX8_Render_State(D3DRS_FOGCOLOR, 0);
//	Set_DX8_Render_State(D3DRS_FOGTABLEMODE, D3DFOG_NONE);
//	Set_DX8_Render_State(D3DRS_FOGSTART, 0);

//	Set_DX8_Render_State(D3DRS_FOGEND, WWMath::Float_As_Int(1.0f));
//	Set_DX8_Render_State(D3DRS_FOGDENSITY, WWMath::Float_As_Int(1.0f));

	//Set_DX8_Render_State(D3DRS_EDGEANTIALIAS, FALSE);
	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_ZBIAS removed in D3D9; default ZBias=0 is the no-op state
	// GeneralsX @feature PS3.0 batch7 13/10/2026 reset the hardware depth bias with the other states, so a
	// shadow pass that left a bias behind cannot leak it into whatever draws next. Pushed through the
	// cached setter and the mirror members are updated to match, otherwise the two would disagree and a
	// later Set_Depth_Bias(0.0f) would be swallowed as a no-op.
	{
		float biasZero=0.0f;
		Set_DX8_Render_State(D3DRS_DEPTHBIAS,*(DWORD *)(&biasZero));
		Set_DX8_Render_State(D3DRS_SLOPESCALEDEPTHBIAS,*(DWORD *)(&biasZero));
		DepthBias=biasZero;
		SlopeScaleDepthBias=biasZero;
	}
//	Set_DX8_Render_State(D3DRS_RANGEFOGENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_STENCILENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_STENCILFAIL, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
	Set_DX8_Render_State(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
	Set_DX8_Render_State(D3DRS_STENCILREF, 0);
	Set_DX8_Render_State(D3DRS_STENCILMASK, 0xffffffff);
	Set_DX8_Render_State(D3DRS_STENCILWRITEMASK, 0xffffffff);
	Set_DX8_Render_State(D3DRS_TEXTUREFACTOR, 0);
/*	Set_DX8_Render_State(D3DRS_WRAP0, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP1, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP2, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP3, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP4, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP5, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP6, D3DWRAP_U| D3DWRAP_V);
	Set_DX8_Render_State(D3DRS_WRAP7, D3DWRAP_U| D3DWRAP_V);*/
	Set_DX8_Render_State(D3DRS_CLIPPING, TRUE);
	Set_DX8_Render_State(D3DRS_LIGHTING, FALSE);
	//Set_DX8_Render_State(D3DRS_AMBIENT, 0);
//	Set_DX8_Render_State(D3DRS_FOGVERTEXMODE, D3DFOG_NONE);
	Set_DX8_Render_State(D3DRS_COLORVERTEX, TRUE);
/*	Set_DX8_Render_State(D3DRS_LOCALVIEWER, TRUE);
	Set_DX8_Render_State(D3DRS_NORMALIZENORMALS, FALSE);
	Set_DX8_Render_State(D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1);
	Set_DX8_Render_State(D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2);
	Set_DX8_Render_State(D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL);
	Set_DX8_Render_State(D3DRS_VERTEXBLEND, D3DVBF_DISABLE);*/
	//Set_DX8_Render_State(D3DRS_CLIPPLANEENABLE, 0);
	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_SOFTWAREVERTEXPROCESSING removed in D3D9 (behavior chosen at CreateDevice)
	//Set_DX8_Render_State(D3DRS_POINTSIZE, 0x3f800000);
	//Set_DX8_Render_State(D3DRS_POINTSIZE_MIN, 0);
	//Set_DX8_Render_State(D3DRS_POINTSPRITEENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_POINTSCALEENABLE, FALSE);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_A, 0);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_B, 0);
	//Set_DX8_Render_State(D3DRS_POINTSCALE_C, 0);
	//Set_DX8_Render_State(D3DRS_MULTISAMPLEANTIALIAS, TRUE);
	//Set_DX8_Render_State(D3DRS_MULTISAMPLEMASK, 0xffffffff);
	//Set_DX8_Render_State(D3DRS_PATCHEDGESTYLE, D3DPATCHEDGE_DISCRETE);
	//Set_DX8_Render_State(D3DRS_PATCHSEGMENTS, 0x3f800000);
	//Set_DX8_Render_State(D3DRS_DEBUGMONITORTOKEN, D3DDMT_ENABLE);
	//Set_DX8_Render_State(D3DRS_POINTSIZE_MAX, Float_At_Int(64.0f));
	//Set_DX8_Render_State(D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE);
	Set_DX8_Render_State(D3DRS_COLORWRITEENABLE, 0x0000000f);
	//Set_DX8_Render_State(D3DRS_TWEENFACTOR, 0);
	Set_DX8_Render_State(D3DRS_BLENDOP, D3DBLENDOP_ADD);
	//Set_DX8_Render_State(D3DRS_POSITIONORDER, D3DORDER_CUBIC);
	//Set_DX8_Render_State(D3DRS_NORMALORDER, D3DORDER_LINEAR);

	// disable TSS stages
	int i;
	for (i=0; i<CurrentCaps->Get_Max_Textures_Per_Pass(); i++)
	{
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
		Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

		/*Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT00, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT01, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT10, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVMAT11, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVLSCALE, 0);
		Set_DX8_Texture_Stage_State(i, D3DTSS_BUMPENVLOFFSET, 0);*/

		Set_DX8_Texture_Stage_State(i, D3DTSS_TEXCOORDINDEX, i);


		// GeneralsX @build d3d9 port 11/10/2026 - sampler states must go through SetSamplerState in D3D9
		Set_DX8_Sampler_State(i, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		Set_DX8_Sampler_State(i, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		Set_DX8_Sampler_State(i, D3DSAMP_BORDERCOLOR, 0);
//		Set_DX8_Sampler_State(i, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Sampler_State(i, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Sampler_State(i, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MIPMAPLODBIAS, 0);
//		Set_DX8_Texture_Stage_State(i, D3DTSS_MAXMIPLEVEL, 0);
//		Set_DX8_Sampler_State(i, D3DSAMP_MAXANISOTROPY, 1);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_ADDRESSW, D3DTADDRESS_WRAP);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_COLORARG0, D3DTA_CURRENT);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_ALPHAARG0, D3DTA_CURRENT);
		//Set_DX8_Texture_Stage_State(i, D3DTSS_RESULTARG, D3DTA_CURRENT);

		Set_DX8_Texture_Stage_State(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		Set_Texture(i,nullptr);
	}

//	DX8Wrapper::Set_Material(nullptr);
	VertexMaterialClass::Apply_Null();

	for (unsigned index=0;index<4;++index) {
		SNAPSHOT_SAY(("Clearing light %d to null",index));
		Set_DX8_Light(index,nullptr);
	}

	// set up simple default TSS
	// GeneralsX @feature PS3.0 batch1 12/10/2026 the clearing pattern is all zeros forever, so it
	// lives in a static instead of putting 4KB (+3.5KB once the pixel budget was the SM3 one) on the
	// stack in a function the resolution-change path runs. One buffer serves both stages because the
	// vertex budget (256) is the larger of the two caches. Only the registers this device addresses
	// are pushed, so the setter's budget guard stays quiet.
	static Vector4 s_zeroShaderConstants[MAX_VERTEX_SHADER_CONSTANTS];
	static bool s_zeroShaderConstantsInit=false;
	if (!s_zeroShaderConstantsInit) {
		memset(s_zeroShaderConstants,0,sizeof(s_zeroShaderConstants));
		s_zeroShaderConstantsInit=true;
	}
	Set_Vertex_Shader_Constant(0,s_zeroShaderConstants,(int)Vertex_Shader_Constant_Limit);
	Set_Pixel_Shader_Constant(0,s_zeroShaderConstants,(int)Pixel_Shader_Constant_Limit);

	Set_Vertex_FVF(DX8_FVF_XYZNDUV2);
	Set_Pixel_Shader(0);

	ShaderClass::Invalidate();
}

const char* DX8Wrapper::Get_DX8_Render_State_Name(D3DRENDERSTATETYPE state)
{
	switch (state) {
	case D3DRS_ZENABLE                       : return "D3DRS_ZENABLE";
	case D3DRS_FILLMODE                      : return "D3DRS_FILLMODE";
	case D3DRS_SHADEMODE                     : return "D3DRS_SHADEMODE";
	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_LINEPATTERN/ZVISIBLE/EDGEANTIALIAS/ZBIAS/SOFTWAREVERTEXPROCESSING/PATCHSEGMENTS removed in D3D9
	case D3DRS_ZWRITEENABLE                  : return "D3DRS_ZWRITEENABLE";
	case D3DRS_ALPHATESTENABLE               : return "D3DRS_ALPHATESTENABLE";
	case D3DRS_LASTPIXEL                     : return "D3DRS_LASTPIXEL";
	case D3DRS_SRCBLEND                      : return "D3DRS_SRCBLEND";
	case D3DRS_DESTBLEND                     : return "D3DRS_DESTBLEND";
	case D3DRS_CULLMODE                      : return "D3DRS_CULLMODE";
	case D3DRS_ZFUNC                         : return "D3DRS_ZFUNC";
	case D3DRS_ALPHAREF                      : return "D3DRS_ALPHAREF";
	case D3DRS_ALPHAFUNC                     : return "D3DRS_ALPHAFUNC";
	case D3DRS_DITHERENABLE                  : return "D3DRS_DITHERENABLE";
	case D3DRS_ALPHABLENDENABLE              : return "D3DRS_ALPHABLENDENABLE";
	case D3DRS_FOGENABLE                     : return "D3DRS_FOGENABLE";
	case D3DRS_SPECULARENABLE                : return "D3DRS_SPECULARENABLE";
	case D3DRS_FOGCOLOR                      : return "D3DRS_FOGCOLOR";
	case D3DRS_FOGTABLEMODE                  : return "D3DRS_FOGTABLEMODE";
	case D3DRS_FOGSTART                      : return "D3DRS_FOGSTART";
	case D3DRS_FOGEND                        : return "D3DRS_FOGEND";
	case D3DRS_FOGDENSITY                    : return "D3DRS_FOGDENSITY";
	case D3DRS_RANGEFOGENABLE                : return "D3DRS_RANGEFOGENABLE";
	case D3DRS_STENCILENABLE                 : return "D3DRS_STENCILENABLE";
	case D3DRS_STENCILFAIL                   : return "D3DRS_STENCILFAIL";
	case D3DRS_STENCILZFAIL                  : return "D3DRS_STENCILZFAIL";
	case D3DRS_STENCILPASS                   : return "D3DRS_STENCILPASS";
	case D3DRS_STENCILFUNC                   : return "D3DRS_STENCILFUNC";
	case D3DRS_STENCILREF                    : return "D3DRS_STENCILREF";
	case D3DRS_STENCILMASK                   : return "D3DRS_STENCILMASK";
	case D3DRS_STENCILWRITEMASK              : return "D3DRS_STENCILWRITEMASK";
	case D3DRS_TEXTUREFACTOR                 : return "D3DRS_TEXTUREFACTOR";
	case D3DRS_WRAP0                         : return "D3DRS_WRAP0";
	case D3DRS_WRAP1                         : return "D3DRS_WRAP1";
	case D3DRS_WRAP2                         : return "D3DRS_WRAP2";
	case D3DRS_WRAP3                         : return "D3DRS_WRAP3";
	case D3DRS_WRAP4                         : return "D3DRS_WRAP4";
	case D3DRS_WRAP5                         : return "D3DRS_WRAP5";
	case D3DRS_WRAP6                         : return "D3DRS_WRAP6";
	case D3DRS_WRAP7                         : return "D3DRS_WRAP7";
	case D3DRS_CLIPPING                      : return "D3DRS_CLIPPING";
	case D3DRS_LIGHTING                      : return "D3DRS_LIGHTING";
	case D3DRS_AMBIENT                       : return "D3DRS_AMBIENT";
	case D3DRS_FOGVERTEXMODE                 : return "D3DRS_FOGVERTEXMODE";
	case D3DRS_COLORVERTEX                   : return "D3DRS_COLORVERTEX";
	case D3DRS_LOCALVIEWER                   : return "D3DRS_LOCALVIEWER";
	case D3DRS_NORMALIZENORMALS              : return "D3DRS_NORMALIZENORMALS";
	case D3DRS_DIFFUSEMATERIALSOURCE         : return "D3DRS_DIFFUSEMATERIALSOURCE";
	case D3DRS_SPECULARMATERIALSOURCE        : return "D3DRS_SPECULARMATERIALSOURCE";
	case D3DRS_AMBIENTMATERIALSOURCE         : return "D3DRS_AMBIENTMATERIALSOURCE";
	case D3DRS_EMISSIVEMATERIALSOURCE        : return "D3DRS_EMISSIVEMATERIALSOURCE";
	case D3DRS_VERTEXBLEND                   : return "D3DRS_VERTEXBLEND";
	case D3DRS_CLIPPLANEENABLE               : return "D3DRS_CLIPPLANEENABLE";
	case D3DRS_POINTSIZE                     : return "D3DRS_POINTSIZE";
	case D3DRS_POINTSIZE_MIN                 : return "D3DRS_POINTSIZE_MIN";
	case D3DRS_POINTSPRITEENABLE             : return "D3DRS_POINTSPRITEENABLE";
	case D3DRS_POINTSCALEENABLE              : return "D3DRS_POINTSCALEENABLE";
	case D3DRS_POINTSCALE_A                  : return "D3DRS_POINTSCALE_A";
	case D3DRS_POINTSCALE_B                  : return "D3DRS_POINTSCALE_B";
	case D3DRS_POINTSCALE_C                  : return "D3DRS_POINTSCALE_C";
	case D3DRS_MULTISAMPLEANTIALIAS          : return "D3DRS_MULTISAMPLEANTIALIAS";
	case D3DRS_MULTISAMPLEMASK               : return "D3DRS_MULTISAMPLEMASK";
	case D3DRS_PATCHEDGESTYLE                : return "D3DRS_PATCHEDGESTYLE";
	case D3DRS_DEBUGMONITORTOKEN             : return "D3DRS_DEBUGMONITORTOKEN";
	case D3DRS_POINTSIZE_MAX                 : return "D3DRS_POINTSIZE_MAX";
	case D3DRS_INDEXEDVERTEXBLENDENABLE      : return "D3DRS_INDEXEDVERTEXBLENDENABLE";
	case D3DRS_COLORWRITEENABLE              : return "D3DRS_COLORWRITEENABLE";
	case D3DRS_TWEENFACTOR                   : return "D3DRS_TWEENFACTOR";
	case D3DRS_BLENDOP                       : return "D3DRS_BLENDOP";
//	case D3DRS_POSITIONORDER                 : return "D3DRS_POSITIONORDER";
//	case D3DRS_NORMALORDER                   : return "D3DRS_NORMALORDER";
	default											  : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Stage_State_Name(D3DTEXTURESTAGESTATETYPE state)
{
	// GeneralsX @build d3d9 port 11/10/2026 - D3D9 split sampler states (D3DSAMP_*) out of the TSS enum.
	// D3DSAMP_* values overlap D3DTSS_* numerically, so sampler states must not appear as cases here
	// (duplicate case values). They are recorded via Get_DX8_Sampler_State_Name instead.
	switch (state) {
	case D3DTSS_COLOROP                   : return "D3DTSS_COLOROP";
	case D3DTSS_COLORARG1                 : return "D3DTSS_COLORARG1";
	case D3DTSS_COLORARG2                 : return "D3DTSS_COLORARG2";
	case D3DTSS_ALPHAOP                   : return "D3DTSS_ALPHAOP";
	case D3DTSS_ALPHAARG1                 : return "D3DTSS_ALPHAARG1";
	case D3DTSS_ALPHAARG2                 : return "D3DTSS_ALPHAARG2";
	case D3DTSS_BUMPENVMAT00              : return "D3DTSS_BUMPENVMAT00";
	case D3DTSS_BUMPENVMAT01              : return "D3DTSS_BUMPENVMAT01";
	case D3DTSS_BUMPENVMAT10              : return "D3DTSS_BUMPENVMAT10";
	case D3DTSS_BUMPENVMAT11              : return "D3DTSS_BUMPENVMAT11";
	case D3DTSS_TEXCOORDINDEX             : return "D3DTSS_TEXCOORDINDEX";
	case D3DTSS_BUMPENVLSCALE             : return "D3DTSS_BUMPENVLSCALE";
	case D3DTSS_BUMPENVLOFFSET            : return "D3DTSS_BUMPENVLOFFSET";
	case D3DTSS_TEXTURETRANSFORMFLAGS     : return "D3DTSS_TEXTURETRANSFORMFLAGS";
	case D3DTSS_COLORARG0                 : return "D3DTSS_COLORARG0";
	case D3DTSS_ALPHAARG0                 : return "D3DTSS_ALPHAARG0";
	case D3DTSS_RESULTARG                 : return "D3DTSS_RESULTARG";
	case D3DTSS_CONSTANT                  : return "D3DTSS_CONSTANT";
	default										  : return "UNKNOWN";
	}
}

// GeneralsX @feature d3d9 port 11/10/2026 - sampler state names for the D3DSAMP_* split-out states
const char* DX8Wrapper::Get_DX8_Sampler_State_Name(D3DSAMPLERSTATETYPE state)
{
	switch (state) {
	case D3DSAMP_MINFILTER                : return "D3DSAMP_MINFILTER";
	case D3DSAMP_MAGFILTER                : return "D3DSAMP_MAGFILTER";
	case D3DSAMP_MIPFILTER                : return "D3DSAMP_MIPFILTER";
	case D3DSAMP_MIPMAPLODBIAS            : return "D3DSAMP_MIPMAPLODBIAS";
	case D3DSAMP_MAXMIPLEVEL              : return "D3DSAMP_MAXMIPLEVEL";
	case D3DSAMP_ADDRESSU                 : return "D3DSAMP_ADDRESSU";
	case D3DSAMP_ADDRESSV                 : return "D3DSAMP_ADDRESSV";
	case D3DSAMP_BORDERCOLOR              : return "D3DSAMP_BORDERCOLOR";
	case D3DSAMP_MAXANISOTROPY            : return "D3DSAMP_MAXANISOTROPY";
	case D3DSAMP_ADDRESSW                 : return "D3DSAMP_ADDRESSW";
	case D3DSAMP_SRGBTEXTURE              : return "D3DSAMP_SRGBTEXTURE";
	// GeneralsX @build d3d9 port 11/10/2026 - D3DSAMP_ELEMENTWISEMAD only exists in the d3d9ex header
	default										  : return "UNKNOWN";
	}
}

void DX8Wrapper::Get_DX8_Render_State_Value_Name(StringClass& name, D3DRENDERSTATETYPE state, unsigned value)
{
	switch (state) {
	case D3DRS_ZENABLE:
		name=Get_DX8_ZBuffer_Type_Name(value);
		break;

	case D3DRS_FILLMODE:
		name=Get_DX8_Fill_Mode_Name(value);
		break;

	case D3DRS_SHADEMODE:
		name=Get_DX8_Shade_Mode_Name(value);
		break;

	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_LINEPATTERN removed in D3D9
	case D3DRS_FOGCOLOR:
	case D3DRS_ALPHAREF:
	case D3DRS_STENCILMASK:
	case D3DRS_STENCILWRITEMASK:
	case D3DRS_TEXTUREFACTOR:
	case D3DRS_AMBIENT:
	case D3DRS_CLIPPLANEENABLE:
	case D3DRS_MULTISAMPLEMASK:
		name.Format("0x%x",value);
		break;

	case D3DRS_ZWRITEENABLE:
	case D3DRS_ALPHATESTENABLE:
	case D3DRS_LASTPIXEL:
	case D3DRS_DITHERENABLE:
	case D3DRS_ALPHABLENDENABLE:
	case D3DRS_FOGENABLE:
	case D3DRS_SPECULARENABLE:
	case D3DRS_STENCILENABLE:
	case D3DRS_RANGEFOGENABLE:
	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_EDGEANTIALIAS removed in D3D9
	case D3DRS_CLIPPING:
	case D3DRS_LIGHTING:
	case D3DRS_COLORVERTEX:
	case D3DRS_LOCALVIEWER:
	case D3DRS_NORMALIZENORMALS:
	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_SOFTWAREVERTEXPROCESSING removed in D3D9
	case D3DRS_POINTSPRITEENABLE:
	case D3DRS_POINTSCALEENABLE:
	case D3DRS_MULTISAMPLEANTIALIAS:
	case D3DRS_INDEXEDVERTEXBLENDENABLE:
		name=value ? "TRUE" : "FALSE";
		break;

	case D3DRS_SRCBLEND:
	case D3DRS_DESTBLEND:
		name=Get_DX8_Blend_Name(value);
		break;

	case D3DRS_CULLMODE:
		name=Get_DX8_Cull_Mode_Name(value);
		break;

	case D3DRS_ZFUNC:
	case D3DRS_ALPHAFUNC:
	case D3DRS_STENCILFUNC:
		name=Get_DX8_Cmp_Func_Name(value);
		break;

	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_ZVISIBLE removed in D3D9
	case D3DRS_FOGTABLEMODE:
	case D3DRS_FOGVERTEXMODE:
		name=Get_DX8_Fog_Mode_Name(value);
		break;

	case D3DRS_FOGSTART:
	case D3DRS_FOGEND:
	case D3DRS_FOGDENSITY:
	case D3DRS_POINTSIZE:
	case D3DRS_POINTSIZE_MIN:
	case D3DRS_POINTSCALE_A:
	case D3DRS_POINTSCALE_B:
	case D3DRS_POINTSCALE_C:
	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_PATCHSEGMENTS removed in D3D9
	case D3DRS_POINTSIZE_MAX:
	case D3DRS_TWEENFACTOR:
		name.Format("%f",*(float*)&value);
		break;

	// GeneralsX @build d3d9 port 11/10/2026 - D3DRS_ZBIAS removed in D3D9
	case D3DRS_STENCILREF:
		name.Format("%d",value);
		break;

	case D3DRS_STENCILFAIL:
	case D3DRS_STENCILZFAIL:
	case D3DRS_STENCILPASS:
		name=Get_DX8_Stencil_Op_Name(value);
		break;

	case D3DRS_WRAP0:
	case D3DRS_WRAP1:
	case D3DRS_WRAP2:
	case D3DRS_WRAP3:
	case D3DRS_WRAP4:
	case D3DRS_WRAP5:
	case D3DRS_WRAP6:
	case D3DRS_WRAP7:
		name="0";
		if (value&D3DWRAP_U) name+="|D3DWRAP_U";
		if (value&D3DWRAP_V) name+="|D3DWRAP_V";
		if (value&D3DWRAP_W) name+="|D3DWRAP_W";
		break;

	case D3DRS_DIFFUSEMATERIALSOURCE:
	case D3DRS_SPECULARMATERIALSOURCE:
	case D3DRS_AMBIENTMATERIALSOURCE:
	case D3DRS_EMISSIVEMATERIALSOURCE:
		name=Get_DX8_Material_Source_Name(value);
		break;

	case D3DRS_VERTEXBLEND:
		name=Get_DX8_Vertex_Blend_Flag_Name(value);
		break;

	case D3DRS_PATCHEDGESTYLE:
		name=Get_DX8_Patch_Edge_Style_Name(value);
		break;

	case D3DRS_DEBUGMONITORTOKEN:
		name=Get_DX8_Debug_Monitor_Token_Name(value);
		break;

	case D3DRS_COLORWRITEENABLE:
		name="0";
		if (value&D3DCOLORWRITEENABLE_RED) name+="|D3DCOLORWRITEENABLE_RED";
		if (value&D3DCOLORWRITEENABLE_GREEN) name+="|D3DCOLORWRITEENABLE_GREEN";
		if (value&D3DCOLORWRITEENABLE_BLUE) name+="|D3DCOLORWRITEENABLE_BLUE";
		if (value&D3DCOLORWRITEENABLE_ALPHA) name+="|D3DCOLORWRITEENABLE_ALPHA";
		break;
	case D3DRS_BLENDOP:
		name=Get_DX8_Blend_Op_Name(value);
		break;
	default:
		name.Format("UNKNOWN (%d)",value);
		break;
	}
}

void DX8Wrapper::Get_DX8_Texture_Stage_State_Value_Name(StringClass& name, D3DTEXTURESTAGESTATETYPE state, unsigned value)
{
	switch (state) {
	case D3DTSS_COLOROP:
	case D3DTSS_ALPHAOP:
		name=Get_DX8_Texture_Op_Name(value);
		break;

	case D3DTSS_COLORARG0:
	case D3DTSS_COLORARG1:
	case D3DTSS_COLORARG2:
	case D3DTSS_ALPHAARG0:
	case D3DTSS_ALPHAARG1:
	case D3DTSS_ALPHAARG2:
	case D3DTSS_RESULTARG:
		name=Get_DX8_Texture_Arg_Name(value);
		break;

	// GeneralsX @build d3d9 port 11/10/2026 - sampler state value names handled by Get_DX8_Sampler_State_Value_Name
	case D3DTSS_TEXTURETRANSFORMFLAGS:
		name=Get_DX8_Texture_Transform_Flag_Name(value);
		break;

	// Floating point values
	case D3DTSS_BUMPENVMAT00:
	case D3DTSS_BUMPENVMAT01:
	case D3DTSS_BUMPENVMAT10:
	case D3DTSS_BUMPENVMAT11:
	case D3DTSS_BUMPENVLSCALE:
	case D3DTSS_BUMPENVLOFFSET:
		name.Format("%f",*(float*)&value);
		break;

	case D3DTSS_TEXCOORDINDEX:
		if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACENORMAL) {
			name.Format("D3DTSS_TCI_CAMERASPACENORMAL|%d",value&0xffff);
		}
		else if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACEPOSITION) {
			name.Format("D3DTSS_TCI_CAMERASPACEPOSITION|%d",value&0xffff);
		}
		else if ((value&0xffff0000)==D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR) {
			name.Format("D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR|%d",value&0xffff);
		}
		else {
			name.Format("%d",value);
		}
		break;

	// GeneralsX @build d3d9 port 11/10/2026 - D3DTSS_MAXMIPLEVEL/BORDERCOLOR became D3DSAMP_* in D3D9
	default:
		name.Format("UNKNOWN (%d)",value);
		break;
	}
}

// GeneralsX @feature d3d9 port 11/10/2026 - value names for the D3DSAMP_* sampler states
void DX8Wrapper::Get_DX8_Sampler_State_Value_Name(StringClass& name, D3DSAMPLERSTATETYPE state, unsigned value)
{
	switch (state) {
	case D3DSAMP_ADDRESSU:
	case D3DSAMP_ADDRESSV:
	case D3DSAMP_ADDRESSW:
		name=Get_DX8_Texture_Address_Name(value);
		break;

	case D3DSAMP_MINFILTER:
	case D3DSAMP_MAGFILTER:
	case D3DSAMP_MIPFILTER:
		name=Get_DX8_Texture_Filter_Name(value);
		break;

	case D3DSAMP_MIPMAPLODBIAS:
		name.Format("%f",*(float*)&value);
		break;

	case D3DSAMP_MAXMIPLEVEL:
	case D3DSAMP_MAXANISOTROPY:
		name.Format("%d",value);
		break;

	case D3DSAMP_BORDERCOLOR:
		name.Format("0x%x",value);
		break;

	default:
		name.Format("UNKNOWN (%d)",value);
		break;
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Op_Name(unsigned value)
{
	switch (value) {
	case D3DTOP_DISABLE                      : return "D3DTOP_DISABLE";
	case D3DTOP_SELECTARG1                   : return "D3DTOP_SELECTARG1";
	case D3DTOP_SELECTARG2                   : return "D3DTOP_SELECTARG2";
	case D3DTOP_MODULATE                     : return "D3DTOP_MODULATE";
	case D3DTOP_MODULATE2X                   : return "D3DTOP_MODULATE2X";
	case D3DTOP_MODULATE4X                   : return "D3DTOP_MODULATE4X";
	case D3DTOP_ADD                          : return "D3DTOP_ADD";
	case D3DTOP_ADDSIGNED                    : return "D3DTOP_ADDSIGNED";
	case D3DTOP_ADDSIGNED2X                  : return "D3DTOP_ADDSIGNED2X";
	case D3DTOP_SUBTRACT                     : return "D3DTOP_SUBTRACT";
	case D3DTOP_ADDSMOOTH                    : return "D3DTOP_ADDSMOOTH";
	case D3DTOP_BLENDDIFFUSEALPHA            : return "D3DTOP_BLENDDIFFUSEALPHA";
	case D3DTOP_BLENDTEXTUREALPHA            : return "D3DTOP_BLENDTEXTUREALPHA";
	case D3DTOP_BLENDFACTORALPHA             : return "D3DTOP_BLENDFACTORALPHA";
	case D3DTOP_BLENDTEXTUREALPHAPM          : return "D3DTOP_BLENDTEXTUREALPHAPM";
	case D3DTOP_BLENDCURRENTALPHA            : return "D3DTOP_BLENDCURRENTALPHA";
	case D3DTOP_PREMODULATE                  : return "D3DTOP_PREMODULATE";
	case D3DTOP_MODULATEALPHA_ADDCOLOR       : return "D3DTOP_MODULATEALPHA_ADDCOLOR";
	case D3DTOP_MODULATECOLOR_ADDALPHA       : return "D3DTOP_MODULATECOLOR_ADDALPHA";
	case D3DTOP_MODULATEINVALPHA_ADDCOLOR    : return "D3DTOP_MODULATEINVALPHA_ADDCOLOR";
	case D3DTOP_MODULATEINVCOLOR_ADDALPHA    : return "D3DTOP_MODULATEINVCOLOR_ADDALPHA";
	case D3DTOP_BUMPENVMAP                   : return "D3DTOP_BUMPENVMAP";
	case D3DTOP_BUMPENVMAPLUMINANCE          : return "D3DTOP_BUMPENVMAPLUMINANCE";
	case D3DTOP_DOTPRODUCT3                  : return "D3DTOP_DOTPRODUCT3";
	case D3DTOP_MULTIPLYADD                  : return "D3DTOP_MULTIPLYADD";
	case D3DTOP_LERP                         : return "D3DTOP_LERP";
	default										     : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Arg_Name(unsigned value)
{
	switch (value) {
	case D3DTA_CURRENT			: return "D3DTA_CURRENT";
	case D3DTA_DIFFUSE			: return "D3DTA_DIFFUSE";
	case D3DTA_SELECTMASK		: return "D3DTA_SELECTMASK";
	case D3DTA_SPECULAR			: return "D3DTA_SPECULAR";
	case D3DTA_TEMP				: return "D3DTA_TEMP";
	case D3DTA_TEXTURE			: return "D3DTA_TEXTURE";
	case D3DTA_TFACTOR			: return "D3DTA_TFACTOR";
	case D3DTA_ALPHAREPLICATE	: return "D3DTA_ALPHAREPLICATE";
	case D3DTA_COMPLEMENT		: return "D3DTA_COMPLEMENT";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Filter_Name(unsigned value)
{
	switch (value) {
	case D3DTEXF_NONE				: return "D3DTEXF_NONE";
	case D3DTEXF_POINT			: return "D3DTEXF_POINT";
	case D3DTEXF_LINEAR			: return "D3DTEXF_LINEAR";
	case D3DTEXF_ANISOTROPIC	: return "D3DTEXF_ANISOTROPIC";
	// GeneralsX @build d3d9 port 11/10/2026 - D3DTEXF_FLATCUBIC/GAUSSIANCUBIC removed in D3D9
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Address_Name(unsigned value)
{
	switch (value) {
	case D3DTADDRESS_WRAP		: return "D3DTADDRESS_WRAP";
	case D3DTADDRESS_MIRROR		: return "D3DTADDRESS_MIRROR";
	case D3DTADDRESS_CLAMP		: return "D3DTADDRESS_CLAMP";
	case D3DTADDRESS_BORDER		: return "D3DTADDRESS_BORDER";
	case D3DTADDRESS_MIRRORONCE: return "D3DTADDRESS_MIRRORONCE";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Texture_Transform_Flag_Name(unsigned value)
{
	switch (value) {
	case D3DTTFF_DISABLE			: return "D3DTTFF_DISABLE";
	case D3DTTFF_COUNT1			: return "D3DTTFF_COUNT1";
	case D3DTTFF_COUNT2			: return "D3DTTFF_COUNT2";
	case D3DTTFF_COUNT3			: return "D3DTTFF_COUNT3";
	case D3DTTFF_COUNT4			: return "D3DTTFF_COUNT4";
	case D3DTTFF_PROJECTED		: return "D3DTTFF_PROJECTED";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_ZBuffer_Type_Name(unsigned value)
{
	switch (value) {
	case D3DZB_FALSE				: return "D3DZB_FALSE";
	case D3DZB_TRUE				: return "D3DZB_TRUE";
	case D3DZB_USEW				: return "D3DZB_USEW";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Fill_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DFILL_POINT			: return "D3DFILL_POINT";
	case D3DFILL_WIREFRAME		: return "D3DFILL_WIREFRAME";
	case D3DFILL_SOLID			: return "D3DFILL_SOLID";
	default					      : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Shade_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DSHADE_FLAT			: return "D3DSHADE_FLAT";
	case D3DSHADE_GOURAUD		: return "D3DSHADE_GOURAUD";
	case D3DSHADE_PHONG			: return "D3DSHADE_PHONG";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Blend_Name(unsigned value)
{
	switch (value) {
	case D3DBLEND_ZERO                : return "D3DBLEND_ZERO";
	case D3DBLEND_ONE                 : return "D3DBLEND_ONE";
	case D3DBLEND_SRCCOLOR            : return "D3DBLEND_SRCCOLOR";
	case D3DBLEND_INVSRCCOLOR         : return "D3DBLEND_INVSRCCOLOR";
	case D3DBLEND_SRCALPHA            : return "D3DBLEND_SRCALPHA";
	case D3DBLEND_INVSRCALPHA         : return "D3DBLEND_INVSRCALPHA";
	case D3DBLEND_DESTALPHA           : return "D3DBLEND_DESTALPHA";
	case D3DBLEND_INVDESTALPHA        : return "D3DBLEND_INVDESTALPHA";
	case D3DBLEND_DESTCOLOR           : return "D3DBLEND_DESTCOLOR";
	case D3DBLEND_INVDESTCOLOR        : return "D3DBLEND_INVDESTCOLOR";
	case D3DBLEND_SRCALPHASAT         : return "D3DBLEND_SRCALPHASAT";
	case D3DBLEND_BOTHSRCALPHA        : return "D3DBLEND_BOTHSRCALPHA";
	case D3DBLEND_BOTHINVSRCALPHA     : return "D3DBLEND_BOTHINVSRCALPHA";
	default									 : return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Cull_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DCULL_NONE				: return "D3DCULL_NONE";
	case D3DCULL_CW				: return "D3DCULL_CW";
	case D3DCULL_CCW				: return "D3DCULL_CCW";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Cmp_Func_Name(unsigned value)
{
	switch (value) {
	case D3DCMP_NEVER          : return "D3DCMP_NEVER";
	case D3DCMP_LESS           : return "D3DCMP_LESS";
	case D3DCMP_EQUAL          : return "D3DCMP_EQUAL";
	case D3DCMP_LESSEQUAL      : return "D3DCMP_LESSEQUAL";
	case D3DCMP_GREATER        : return "D3DCMP_GREATER";
	case D3DCMP_NOTEQUAL       : return "D3DCMP_NOTEQUAL";
	case D3DCMP_GREATEREQUAL   : return "D3DCMP_GREATEREQUAL";
	case D3DCMP_ALWAYS         : return "D3DCMP_ALWAYS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Fog_Mode_Name(unsigned value)
{
	switch (value) {
	case D3DFOG_NONE				: return "D3DFOG_NONE";
	case D3DFOG_EXP				: return "D3DFOG_EXP";
	case D3DFOG_EXP2				: return "D3DFOG_EXP2";
	case D3DFOG_LINEAR			: return "D3DFOG_LINEAR";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Stencil_Op_Name(unsigned value)
{
	switch (value) {
	case D3DSTENCILOP_KEEP		: return "D3DSTENCILOP_KEEP";
	case D3DSTENCILOP_ZERO		: return "D3DSTENCILOP_ZERO";
	case D3DSTENCILOP_REPLACE	: return "D3DSTENCILOP_REPLACE";
	case D3DSTENCILOP_INCRSAT	: return "D3DSTENCILOP_INCRSAT";
	case D3DSTENCILOP_DECRSAT	: return "D3DSTENCILOP_DECRSAT";
	case D3DSTENCILOP_INVERT	: return "D3DSTENCILOP_INVERT";
	case D3DSTENCILOP_INCR		: return "D3DSTENCILOP_INCR";
	case D3DSTENCILOP_DECR		: return "D3DSTENCILOP_DECR";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Material_Source_Name(unsigned value)
{
	switch (value) {
	case D3DMCS_MATERIAL			: return "D3DMCS_MATERIAL";
	case D3DMCS_COLOR1			: return "D3DMCS_COLOR1";
	case D3DMCS_COLOR2			: return "D3DMCS_COLOR2";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Vertex_Blend_Flag_Name(unsigned value)
{
	switch (value) {
	case D3DVBF_DISABLE			: return "D3DVBF_DISABLE";
	case D3DVBF_1WEIGHTS			: return "D3DVBF_1WEIGHTS";
	case D3DVBF_2WEIGHTS			: return "D3DVBF_2WEIGHTS";
	case D3DVBF_3WEIGHTS			: return "D3DVBF_3WEIGHTS";
	case D3DVBF_TWEENING			: return "D3DVBF_TWEENING";
	case D3DVBF_0WEIGHTS			: return "D3DVBF_0WEIGHTS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Patch_Edge_Style_Name(unsigned value)
{
	switch (value) {
	case D3DPATCHEDGE_DISCRETE	: return "D3DPATCHEDGE_DISCRETE";
   case D3DPATCHEDGE_CONTINUOUS:return "D3DPATCHEDGE_CONTINUOUS";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Debug_Monitor_Token_Name(unsigned value)
{
	switch (value) {
	case D3DDMT_ENABLE			: return "D3DDMT_ENABLE";
	case D3DDMT_DISABLE			: return "D3DDMT_DISABLE";
	default							: return "UNKNOWN";
	}
}

const char* DX8Wrapper::Get_DX8_Blend_Op_Name(unsigned value)
{
	switch (value) {
	case D3DBLENDOP_ADD			: return "D3DBLENDOP_ADD";
	case D3DBLENDOP_SUBTRACT	: return "D3DBLENDOP_SUBTRACT";
	case D3DBLENDOP_REVSUBTRACT: return "D3DBLENDOP_REVSUBTRACT";
	case D3DBLENDOP_MIN			: return "D3DBLENDOP_MIN";
	case D3DBLENDOP_MAX			: return "D3DBLENDOP_MAX";
	default							: return "UNKNOWN";
	}
}


//============================================================================
// DX8Wrapper::getBackBufferFormat
//============================================================================

WW3DFormat	DX8Wrapper::getBackBufferFormat()
{
	return D3DFormat_To_WW3DFormat( _PresentParameters.BackBufferFormat );
}
