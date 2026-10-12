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
// M1: terrain-driven distribution. Grass clumps are scattered only on cells
// whose terrain class is TERRAIN_GRASS, merged into one static index buffer and
// rebuilt (camera-centered) when the camera travels past a threshold - never per
// frame - so the CPU cost of placement sampling stays amortized (see 真实草丛系统
// GrassBuffer实施方案.md, sections 3-B / 3-C / 8.7).

#pragma once

#include "Lib/BaseType.h"
#include "WWMath/vector3.h"
#include "WWMath/matrix3d.h"			// Matrix3D::Transform_Vector (world -> view space grass verts)

#include <vector>

class DX8IndexBufferClass;
class RenderInfoClass;
class TextureClass;

class W3DGrassBuffer
{
public:
	// One placed grass clump. The base quad axis is stored pre-normalized so the
	// per-frame vertex expansion is pure arithmetic (no trig), keeping the frame
	// path cheap while all placement/hash work happens only on rebuild.
	struct GrassClump
	{
		Real x, y, z;          // world-space base position (root, on the terrain)
		Int cellX, cellY;      // source grid indices (step-sized cells; LOD stride sampling)
		Real ax, ay;           // unit half-width axis for quad A; quad B uses (-ay, ax)
		Real halfWidth;        // quad half width in world units
		Real height;           // quad tip height above the base
		unsigned long colorBase;   // AO-darkened root vertex color (per-clump tint baked in)
		unsigned long colorTip;    // lit tip vertex color
	};

	W3DGrassBuffer();
	virtual ~W3DGrassBuffer();

	void init();
	void reset();
	void update();
	void render(RenderInfoClass &rinfo);

	/// Device lifecycle (mirrors W3DSnowManager / W3DTerrainTracks).
	void ReleaseResources();
	Bool ReAcquireResources();

	/// INI / options hook (M1): re-read density + enabled toggle.
	void updateIniSettings();

	void setEnabled(Bool on) { m_enabled = on; }
	Bool isIdle(void) const { return m_clumps.empty(); }

protected:
	/// GeneralsX @build win64 11/10/2026 Camera-windowed placement pass (M1 path actually used by
	/// W3DGrassBuffer.cpp): regenerates m_clumps around the look-at pivot.
	void rebuildGrassField();
	/// Scan the WHOLE map once (map-change gated) into the full candidate set.
	void rebuildFullField();
	/// Per-frame: extract the visible, distance-LOD'd subset from m_allClumps.
	void filterVisible();
	/// Push one clump (2 crossed quads worth of vertices) into the vertex stream. Corners are
	/// transformed world -> view space with the camera view matrix (W3DSnow convention).
	void emitClumpQuads(struct VertexFormatXYZNDUV2 *&verts, const Matrix3D &view) const;

	DX8IndexBufferClass *m_indexBuffer;      // pre-sized shared quad indices
	TextureClass *m_texture;                 // grass alpha sheet (null => solid fallback)

	std::vector<GrassClump> m_clumps;        // per-frame visible subset (built from m_allClumps)
	std::vector<GrassClump> m_allClumps;     // full-map candidate set (scanned once per map)
	Int  m_builtSig;                         // map signature m_allClumps was scanned for (-1 = none)
	Int  m_quadCount;                        // quads currently cached (m_clumps * 2)
	Bool m_enabled;
	Bool m_hasResources;
	Bool m_dirty;                            // placement window stale (map change / first acquire)
	Real m_centerX;                          // window center = camera look-at pivot at last rebuild
	Real m_centerY;

	// Tunables (density is baked from INI in M1 later; constants for now).
	Int  m_densityPercent;                   // 0..100, chance a qualifying cell gets a clump
};

// extern global (owned by device layer, created lazily in W3DParticleSys render).
extern W3DGrassBuffer *TheGrassBuffer;
