/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
*/

// FILE: W3DGrassBuffer.h ///////////////////////////////////////////////////
// GeneralsX @feature Qoder 11/10/2026 Real 3D grass layer (GrassBuffer).
// Procedural ground-decoration layer modeled after W3DSnowManager: a static,
// camera-anchored clump rendered through the fixed pipeline in the Spike stage
// (S0/S1), later extended with a vs_1_1 wind shader (S2/S3) and terrain-driven
// distribution + crush animation (M1/M2). Purely a render-side effect - it must
// never touch game logic or the CRC.

#pragma once

#include "WWLib/always.h"
#include "WW3D2/vector3.h"
#include "WW3D2/vector2.h"

class DX8IndexBufferClass;
class RenderInfoClass;
class TextureClass;
struct IDirect3DVertexBuffer8;

// One grass vertex carried in the merged vertex buffer. World-space position so
// the vertex shader can derive per-clump phase directly from X/Z (no CPU update).
// The layout MUST stay byte-for-byte in sync with DX8GRASS_FVF (stride guard - S2).
#define DX8GRASS_FVF (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE2(0))
struct GRASSVERTEX
{
	Vector3	pos;			// world position (root verts sit on the ground plane)
	unsigned long	diffuse;	// ARGB vertex color: baked AO + distance fade alpha
	float		u, v;			// grass atlas UV
	float		rootWeight;	// 0 at the rooted base, 1 at the tip (wind amplitude scale)
};

class W3DGrassBuffer
{
public:
	W3DGrassBuffer();
	virtual ~W3DGrassBuffer();

	void	init();						// load textures, build clump geometry + shared IB
	void	reset();					// new map: rebuild geometry
	void	update();				// per-frame: advance wind/crush vs constants (no VB rebuild)
	void	render(RenderInfoClass &rinfo);	// per-frame draw

	void	ReleaseResources();		// device reset out
	Bool	ReAcquireResources();	// device reset in

	void	updateIniSettings();
	void	setEnabled(Bool on)	{ m_enabled = on; }
	Bool	isEnabled() const		{ return m_enabled; }

private:
	// Spike (S0/S1): draw one demo clump anchored under the tactical camera.
	void	buildSpikeClump();

	DX8IndexBufferClass	*m_indexBuffer;		// shared quad indices
	IDirect3DVertexBuffer8* m_VertexBufferD3D;	// dynamic per-frame VB (fixed pipeline)
	Int			m_quadCount;			// quads per clump (4 for the S1 cross)
	Vector3	m_anchor;				// world position of the demo clump base
	Bool		m_enabled;				// Options master switch (default ON)
	Bool		m_hasResources;			// guard against double acquire/release
};

extern W3DGrassBuffer *TheGrassBuffer;	// render-side singleton (owned by the W3D device layer)
