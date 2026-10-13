/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2026 TheSuperHackers
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

#ifdef PROFILER_ENABLED

#include "../../../Include/W3DDevice/GameClient/W3DProfilerFrameCapture.h"

#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/surfaceclass.h"
#include "WW3D2/texture.h"
#include "WW3D2/ww3d.h"
#include "WW3D2/ww3dformat.h"
#include "WWMath/wwmath.h"
#include <cstring>
#include <d3dx9core.h>

W3DProfilerFrameCapture::W3DProfilerFrameCapture()
{
}

W3DProfilerFrameCapture::~W3DProfilerFrameCapture()
{
	if (m_swizzleShader)
	{
		m_swizzleShader->Release();
		m_swizzleShader = nullptr;
	}
}

bool W3DProfilerFrameCapture::ShouldReuseLastCapture(UnsignedInt currentTimeMs) const
{
	return PROFILER_FRAME_IMAGE_INTERVAL_MS > 0 &&
		currentTimeMs - m_lastCaptureTimeMs < PROFILER_FRAME_IMAGE_INTERVAL_MS &&
		!m_lastCapturePixels.empty();
}

void W3DProfilerFrameCapture::Capture(UnsignedInt displayWidth, UnsignedInt displayHeight)
{
	if (!PROFILER_IS_CONNECTED)
		return;

	// the profiler expects an image every render frame. resend the last capture if we're inside the capture interval.
	const UnsignedInt currentTimeMs = WW3D::Get_Logic_Time_Milliseconds();
	if (ShouldReuseLastCapture(currentTimeMs))
	{
		PROFILER_FRAME_IMAGE(m_lastCapturePixels.data(), PROFILER_FRAME_IMAGE_SIZE, m_lastCaptureHeight, 0, false);
		return;
	}

	// compile swizzle shader convert BGRA to RGBA
	if (!m_swizzleShader)
	{
#ifdef _WIN32
		// GeneralsX @build d3d9 port 12/10/2026 - HLSL now that the D3D9 path has no DX8 assembler.
		// c0/c1/c2 are the per-channel masks set below; the sampled alpha passes through untouched.
		// GeneralsX @feature PS3.0 batch8 13/10/2026 - no "// d3d-target:" line: the shared ladder
		// prefixes the highest profile the caps report, so this is ps_3_0 where the device takes it
		// instead of a hard-coded ps_2_0, and steps back down by itself when it does not.
		const char *shader =
			"sampler s0 : register(s0);\n"
			"float4 c0 : register(c0);\n"
			"float4 c1 : register(c1);\n"
			"float4 c2 : register(c2);\n"
			"float4 main(float2 tc : TEXCOORD0) : COLOR\n"
			"{\n"
			"    float4 t = tex2D(s0, tc);\n"
			"    return float4(t.b * c0.rgb + t.g * c1.rgb + t.r * c2.rgb, t.a);\n"
			"}\n";

		void *handle = nullptr;
		// compileShaderAtBestProfile prints the failing profile, HRESULT and compiler reason itself.
		if (FAILED(W3DShaderManager::compileShaderAtBestProfile(shader, false, "[PROFILER]", nullptr, &handle)))
			return;

		m_swizzleShader = (IDirect3DPixelShader9 *)handle;
#else
		// Runtime HLSL compilation only exists on Windows, so there is no swizzle shader to run the
		// capture through. Returning here keeps the previous behaviour of a silently disabled capture.
		return;
#endif
	}

	// allocate render target
	TextureClass *renderTarget = DX8Wrapper::Create_Render_Target(PROFILER_FRAME_IMAGE_SIZE, PROFILER_FRAME_IMAGE_SIZE, WW3D_FORMAT_A8R8G8B8);
	if (!renderTarget)
		return;

	// allocate surface class
	const Real aspectRatio = (Real)displayHeight / (Real)displayWidth;
	unsigned int profilerImageHeight = min((int)WWMath::Round(PROFILER_FRAME_IMAGE_SIZE * aspectRatio), PROFILER_FRAME_IMAGE_SIZE);
	SurfaceClass *surfaceClass = NEW_REF(SurfaceClass, (PROFILER_FRAME_IMAGE_SIZE, profilerImageHeight, WW3D_FORMAT_A8R8G8B8));
	if (!surfaceClass)
	{
		REF_PTR_RELEASE(renderTarget);
		return;
	}

	// get the backbuffer
	SurfaceClass *backBuffer = DX8Wrapper::_Get_DX8_Back_Buffer();
	if (!backBuffer)
	{
		REF_PTR_RELEASE(surfaceClass);
		REF_PTR_RELEASE(renderTarget);
		return;
	}

	IDirect3DSurface9 *backBufferSurface = backBuffer->Peek_D3D_Surface();
	D3DSURFACE_DESC backBufferSurfaceDesc;
	HRESULT hr = backBufferSurface->GetDesc(&backBufferSurfaceDesc);
	if (FAILED(hr))
	{
		REF_PTR_RELEASE(backBuffer);
		REF_PTR_RELEASE(surfaceClass);
		REF_PTR_RELEASE(renderTarget);
		return;
	}

	// allocate intermediate texture
	IDirect3DTexture9 *intermediateTexture = nullptr;
	hr = DX8Wrapper::_Get_D3D_Device8()->CreateTexture(
		backBufferSurfaceDesc.Width,
		backBufferSurfaceDesc.Height,
		1,
		D3DUSAGE_RENDERTARGET,
		backBufferSurfaceDesc.Format,
		D3DPOOL_DEFAULT,
		&intermediateTexture);
	if (FAILED(hr))
	{
		REF_PTR_RELEASE(backBuffer);
		REF_PTR_RELEASE(surfaceClass);
		REF_PTR_RELEASE(renderTarget);
		return;
	}

	// draw backbuffer to intermediate texture
	IDirect3DSurface9 *intermediateTextureSurface;
	hr = intermediateTexture->GetSurfaceLevel(0, &intermediateTextureSurface);
	if (FAILED(hr))
	{
		REF_PTR_RELEASE(backBuffer);
		REF_PTR_RELEASE(surfaceClass);
		REF_PTR_RELEASE(renderTarget);
		intermediateTexture->Release();
		return;
	}
	DX8Wrapper::_Copy_DX8_Rects(backBufferSurface, nullptr, 0, intermediateTextureSurface, nullptr);
	intermediateTextureSurface->Release();
	intermediateTextureSurface = nullptr;

	// release the backbuffer
	backBufferSurface = nullptr;
	REF_PTR_RELEASE(backBuffer);

	// set render target to a small surface
	IDirect3DSurface9 *smallRenderTargetSurface = renderTarget->Get_D3D_Surface_Level();
	WWASSERT(smallRenderTargetSurface != nullptr);
	DX8Wrapper::Set_Render_Target(smallRenderTargetSurface, false);

	// set viewport
	IDirect3DDevice9 *device = DX8Wrapper::_Get_D3D_Device8();
	D3DVIEWPORT9 restoreViewport;
	device->GetViewport(&restoreViewport);

	SurfaceClass::SurfaceDescription smallRenderDesc;
	surfaceClass->Get_Description(smallRenderDesc);

	D3DVIEWPORT9 viewport;
	viewport.X = 0;
	viewport.Y = 0;
	viewport.Width = PROFILER_FRAME_IMAGE_SIZE;
	viewport.Height = smallRenderDesc.Height;
	viewport.MinZ = 0.0f;
	viewport.MaxZ = 1.0f;
	DX8Wrapper::Set_Viewport(&viewport);

	// bind swizzle shader
	DX8Wrapper::Set_Pixel_Shader(m_swizzleShader);
	static const Real kMaskR[4] = {1.0f, 0.0f, 0.0f, 0.0f};
	static const Real kMaskG[4] = {0.0f, 1.0f, 0.0f, 0.0f};
	static const Real kMaskB[4] = {0.0f, 0.0f, 1.0f, 0.0f};
	device->SetPixelShaderConstantF(0, kMaskR, 1);
	device->SetPixelShaderConstantF(1, kMaskG, 1);
	device->SetPixelShaderConstantF(2, kMaskB, 1);

	// draw texture scaled-down onto a small surface
	struct QuadVertex
	{
		Real x, y, z, rhw;
		Real u, v;
	} vtx[4];
	const Real left = -0.5f;
	const Real top = -0.5f;
	const Real right = (Real)PROFILER_FRAME_IMAGE_SIZE - 0.5f;
	const Real bottom = (Real)smallRenderDesc.Height - 0.5f;
	vtx[0] = {right, bottom, 0.0f, 1.0f, 1.0f, 1.0f};
	vtx[1] = {right, top,    0.0f, 1.0f, 1.0f, 0.0f};
	vtx[2] = {left,  bottom, 0.0f, 1.0f, 0.0f, 1.0f};
	vtx[3] = {left,  top,    0.0f, 1.0f, 0.0f, 0.0f};
	DX8Wrapper::Set_DX8_Texture(0, intermediateTexture);
	DX8Wrapper::Set_Vertex_FVF(D3DFVF_XYZRHW | D3DFVF_TEX1); // GeneralsX @bugfix d3d9 port 12/10/2026 FVF through SetFVF
	device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vtx, sizeof(QuadVertex));
	DX8Wrapper::Set_Pixel_Shader(0);
	DX8Wrapper::Set_DX8_Texture(0, nullptr);
	DX8Wrapper::Set_Viewport(&restoreViewport);
	DX8Wrapper::Set_Render_Target(static_cast<IDirect3DSurface9 *>(nullptr));

	// copy the small surface pixels from GPU to CPU
	RECT srcRect = { 0, 0, PROFILER_FRAME_IMAGE_SIZE, smallRenderDesc.Height };
	POINT dstPoint = { 0, 0 };
	DX8Wrapper::_Copy_DX8_Rects(
		smallRenderTargetSurface,
		&srcRect,
		1,
		surfaceClass->Peek_D3D_Surface(),
		&dstPoint);
	smallRenderTargetSurface->Release();

	// send pixels to the profiler backend
	int pitch = 0;
	void *bits = surfaceClass->Lock(&pitch);
	if (bits)
	{
		const size_t rowBytes = (size_t)PROFILER_FRAME_IMAGE_SIZE * 4;
		m_lastCaptureHeight = smallRenderDesc.Height;
		m_lastCapturePixels.resize(rowBytes * m_lastCaptureHeight);

		const UnsignedByte *source = static_cast<const UnsignedByte *>(bits);
		UnsignedByte *destination = m_lastCapturePixels.data();
		for (UnsignedInt row = 0; row < m_lastCaptureHeight; ++row)
		{
			std::memcpy(destination + row * rowBytes, source + row * pitch, rowBytes);
		}

		PROFILER_FRAME_IMAGE(m_lastCapturePixels.data(), PROFILER_FRAME_IMAGE_SIZE, m_lastCaptureHeight, 0, false);
		surfaceClass->Unlock();
		m_lastCaptureTimeMs = currentTimeMs;
	}

	// cleanup
	intermediateTexture->Release();
	intermediateTexture = nullptr;
	REF_PTR_RELEASE(surfaceClass);
	REF_PTR_RELEASE(renderTarget);
}

#endif // PROFILER_ENABLED
