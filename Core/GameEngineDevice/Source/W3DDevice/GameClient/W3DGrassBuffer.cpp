/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

// FILE: W3DGrassBuffer.cpp /////////////////////////////////////////////////
// GeneralsX @feature Qoder 11/10/2026 Real 3D grass layer - Spike (S0).
// Renders one camera-anchored cross of four vertical quads through the fixed
// pipeline (vertex-colored, no texture, no vertex shader yet). This proves the
// render-side mount, the device lifecycle hooks and the merged-geometry path
// before S1 adds alpha textures and S2 gates the custom vs_1_1 shader.

#include "W3DDevice/GameClient/W3DGrassBuffer.h"

#include "W3DDevice/GameClient/HeightMap.h"		// TheTerrainRenderObject
#include "GameClient/View.h"					// TheTacticalView (camera anchor)
#include "GameLogic/TerrainLogic.h"				// TheTerrainLogic::getGroundHeight
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/shader.h"
#include "WW3D2/vertmaterial.h"
#include "WW3D2/texture.h"
#include "WW3D2/assetmgr.h"

#include "d3d8.h"
#include <cstdio>

// Singleton owned by the W3D device layer (created lazily, never by game logic).
W3DGrassBuffer *TheGrassBuffer = nullptr;

// Spike demo clump parameters.
#define GRASS_SPIKE_QUADS		4		// 4 quads -> a dense enough cross to read as "a clump"
#define GRASS_SPIKE_HEIGHT		12.0f	// world units tall so it is unmistakably visible
#define GRASS_SPIKE_HALFWIDTH	6.0f	// half the quad width

// ARGB vertex colors: darker at the rooted base (baked AO), brighter at the tip.
static const unsigned long GRASS_COLOR_BASE = 0xFF20601E;	// dim green base (AO)
static const unsigned long GRASS_COLOR_TIP  = 0xFF66C244;	// bright green tip

// Grass alpha texture name, resolved through the W3D asset manager / BIG archives.
// ztgrass is the ZH textured-grass alpha sheet; swap for any alpha-bearing grass.
#define GRASS_TEXTURE_NAME "ztgrass"

W3DGrassBuffer::W3DGrassBuffer()
{
	m_indexBuffer = nullptr;
	m_VertexBufferD3D = nullptr;
	m_quadCount = GRASS_SPIKE_QUADS;
	m_anchor.Set(0.0f, 0.0f, 0.0f);
	m_enabled = true;
	m_hasResources = false;
	m_texture = nullptr;
}

W3DGrassBuffer::~W3DGrassBuffer()
{
	ReleaseResources();
}

void W3DGrassBuffer::init()
{
	ReAcquireResources();
}

void W3DGrassBuffer::reset()
{
	// New map: rebuild geometry on next render (resources survive device state).
	m_anchor.Set(0.0f, 0.0f, 0.0f);
}

/** Release all D3D assets before a device reset (mirrors W3DSnowManager). */
void W3DGrassBuffer::ReleaseResources()
{
	if (m_VertexBufferD3D)
	{
		m_VertexBufferD3D->Release();
		m_VertexBufferD3D = nullptr;
	}
	REF_PTR_RELEASE(m_indexBuffer);
	REF_PTR_RELEASE(m_texture);
	m_hasResources = false;
}

/** (Re)allocate D3D assets after a device reset (mirrors W3DSnowManager). */
Bool W3DGrassBuffer::ReAcquireResources()
{
	ReleaseResources();

	if (!m_enabled)
		return true;	// nothing to allocate when the layer is off.

	// Shared index buffer: two triangles per quad, vertex order tl,bl,br,tr
	// (same layout W3DSnowManager::renderAsQuads uses).
	m_indexBuffer = NEW_REF(DX8IndexBufferClass, (m_quadCount * 6));
	{
		DX8IndexBufferClass::WriteLockClass lock(m_indexBuffer);
		unsigned short *ib = lock.Get_Index_Array();
		for (Int i = 0; i < m_quadCount; ++i)
		{
			Int base = i * 4;
			ib[0] = (unsigned short)(base + 3);
			ib[1] = (unsigned short)(base + 0);
			ib[2] = (unsigned short)(base + 2);
			ib[3] = (unsigned short)(base + 2);
			ib[4] = (unsigned short)(base + 0);
			ib[5] = (unsigned short)(base + 1);
			ib += 6;
		}
	}

	m_hasResources = true;

	// Load the grass alpha texture through the W3D asset manager (BIG archives).
	m_texture = WW3DAssetManager::Get_Instance()->Get_Texture(GRASS_TEXTURE_NAME);

	// GeneralsX @feature Qoder 11/10/2026 Spike mount verification log (stderr so it
	// is visible in release builds, per AGENTS.md console-logging convention).
	fprintf(stderr, "[GRASS] W3DGrassBuffer resources acquired, quads=%d\n", m_quadCount);
	fflush(stderr);
	return true;
}

void W3DGrassBuffer::update()
{
	// Spike stage has no per-frame CPU work; wind/crush constants arrive in S3/M2.
}

void W3DGrassBuffer::updateIniSettings()
{
	// M1 will read GrassEnabled / GrassDensity INI here.
}

// Anchor the demo clump on the ground directly under the tactical camera so it is
// always inside the view frustum during the Spike. Deterministic read of logic
// height only - it never writes back to the simulation.
void W3DGrassBuffer::buildSpikeClump()
{
	const Coord3D &cam = TheTacticalView->get3DCameraPosition();
	Real gx = TheTerrainLogic->getGroundHeight(cam.x, cam.y);
	m_anchor.Set(cam.x, cam.y, gx);
}

void W3DGrassBuffer::render(RenderInfoClass &rinfo)
{
	if (!m_enabled)
		return;

	if (!m_hasResources)
		ReAcquireResources();
	if (!m_indexBuffer)
		return;	// device object unavailable - skip this frame.

	buildSpikeClump();

	Int vertexCount = m_quadCount * 4;

	// Dynamic vertex buffer, rebuilt each frame (fixed pipeline path from W3DSnow).
	DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, dynamic_fvf_type, vertexCount);
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2 *verts = lock.Get_Formatted_Vertex_Array();

		for (Int i = 0; i < m_quadCount; ++i)
		{
			Real ang = (Real)i * (3.14159265f / 4.0f);	// 45 degree steps -> 4 crossed quads
			Real dx = WWMath::Fast_Cos(ang);
			Real dy = WWMath::Fast_Sin(ang);

			// Corner order must match the index buffer: tl, bl, br, tr.
			// tl (top-left, base side of the width axis, tip height)
			verts[0].x = m_anchor.X - dx * GRASS_SPIKE_HALFWIDTH;
			verts[0].y = m_anchor.Y - dy * GRASS_SPIKE_HALFWIDTH;
			verts[0].z = m_anchor.Z + GRASS_SPIKE_HEIGHT;
			verts[0].diffuse = GRASS_COLOR_TIP;
			verts[0].u1 = 0.0f; verts[0].v1 = 0.0f;

			// bl (base)
			verts[1].x = m_anchor.X - dx * GRASS_SPIKE_HALFWIDTH;
			verts[1].y = m_anchor.Y - dy * GRASS_SPIKE_HALFWIDTH;
			verts[1].z = m_anchor.Z;
			verts[1].diffuse = GRASS_COLOR_BASE;
			verts[1].u1 = 0.0f; verts[1].v1 = 1.0f;

			// br (base)
			verts[2].x = m_anchor.X + dx * GRASS_SPIKE_HALFWIDTH;
			verts[2].y = m_anchor.Y + dy * GRASS_SPIKE_HALFWIDTH;
			verts[2].z = m_anchor.Z;
			verts[2].diffuse = GRASS_COLOR_BASE;
			verts[2].u1 = 1.0f; verts[2].v1 = 1.0f;

			// tr (tip)
			verts[3].x = m_anchor.X + dx * GRASS_SPIKE_HALFWIDTH;
			verts[3].y = m_anchor.Y + dy * GRASS_SPIKE_HALFWIDTH;
			verts[3].z = m_anchor.Z + GRASS_SPIKE_HEIGHT;
			verts[3].diffuse = GRASS_COLOR_TIP;
			verts[3].u1 = 1.0f; verts[3].v1 = 0.0f;

			for (Int k = 0; k < 4; ++k)
			{
				verts[k].nx = 0.0f; verts[k].ny = 0.0f; verts[k].nz = 0.0f;	// keep AGP write-combining
				verts[k].u2 = 0.0f; verts[k].v2 = 0.0f;
			}
			verts += 4;
		}
	}

	// World transform is identity: the clump is authored in world space and the
	// active view/proj (camera) come from the render pass, exactly like the snow
	// point-sprite path (see W3DSnowManager::render).
	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_WORLD, identity);

	// S1: alpha-tested, double-sided, vertex-color-tinted grass. The ATest sprite
	// preset enables TEXTURING + ALPHATEST + DEPTH_WRITE; we relax it to two-sided
	// (CULL_MODE_DISABLE) and keep the per-vertex AO gradient (GRADIENT_MODULATE) so the
	// base/tip vertex colors tint the texture. With PRELIT_DIFFUSE the fixed pipeline
	// modulates vertex ARGB by the texture on both color and alpha, so the texture's own
	// alpha channel drives the alpha-test cutoff (vertex alpha is opaque 0xFF). If the
	// asset is missing, m_texture is null and the quads fall back to solid vertex color.
	ShaderClass shader = ShaderClass::_PresetATestSpriteShader;
	shader.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);
	shader.Set_Primary_Gradient(ShaderClass::GRADIENT_MODULATE);
	DX8Wrapper::Set_Shader(shader);

	VertexMaterialClass *vmat = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);

	DX8Wrapper::Set_Texture(0, m_texture);	// grass alpha sheet (null => solid-color fallback)

	DX8Wrapper::Set_Index_Buffer(m_indexBuffer, 0);
	DX8Wrapper::Set_Vertex_Buffer(vb_access);
	DX8Wrapper::Draw_Triangles(0, m_quadCount * 2, 0, vertexCount);

	DX8Wrapper::Set_Texture(0, nullptr);	// unbind so the texture never leaks into other passes
}
