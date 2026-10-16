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
// GeneralsX @feature Qoder 11/10/2026 Real 3D grass layer - M1 (terrain-driven).
// Scatters 3D cross-quad grass clumps only on terrain cells whose class is
// TERRAIN_GRASS, inside a camera-centered window. Placement is regenerated only
// when the camera travels past a threshold or the map changes (never per frame);
// the per-frame path just expands the cached clumps into a dynamic vertex buffer
// and issues a single merged draw. Render-side only - it never writes to logic.

#include "W3DDevice/GameClient/W3DGrassBuffer.h"

#include "GameClient/View.h"					// TheTacticalView (camera anchor)
#include "GameClient/TerrainVisual.h"		// TheTerrainVisual->getTerrainTile (render-side terrain class)
#include "GameLogic/TerrainLogic.h"			// TheTerrainLogic->getGroundHeight (render-side height read)
#include "GameLogic/GameLogic.h"				// TheGameLogic->isInGame()/isInShellGame() (mid-game gate)
#include "GameLogic/Object.h"				// Object iteration + KINDOF_STRUCTURE footprint keep-out
#include "Common/TerrainTypes.h"				// TERRAIN_GRASS / TerrainClass
#include "GameClient/Snow.h"					// TheWeatherSetting (GrassEnabled/GrassDensity INI)
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8vertexbuffer.h"
#include "WW3D2/dx8indexbuffer.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/camera.h"
#include "WW3D2/shader.h"
#include "WW3D2/vertmaterial.h"
#include "WW3D2/texture.h"
#include "WW3D2/assetmgr.h"

#include "d3d9.h"
#include <cstdio>

// Singleton owned by the W3D device layer (created lazily, never by game logic).
W3DGrassBuffer *TheGrassBuffer = nullptr;

// --- Distribution tuning ---------------------------------------------------
#define GRASS_STEP				5.0f			// world units between candidate cells
#define GRASS_NEAR_RADIUS		250.0f		// view cull radius at zoom 1 (grows with camera zoom)
#define GRASS_MAX_CLUMPS		1000			// per-frame draw budget (index buffer is sized for this)
#define GRASS_MAX_QUADS			(GRASS_MAX_CLUMPS * 2)	// two crossed quads per clump
#define GRASS_DEFAULT_DENSITY	40			// percent of qualifying cells kept by the per-frame filter
#define GRASS_STRUCTURE_MARGIN	5.0f			// extra keep-out ring around building footprints

// --- Per-clump geometry tuning --------------------------------------------
#define GRASS_HEIGHT_MIN		6.0f
#define GRASS_HEIGHT_SPAN		6.0f			// height = MIN + [0..SPAN] by hash
#define GRASS_HALFWIDTH_MIN		2.5f
#define GRASS_HALFWIDTH_SPAN	2.0f

// --- Colors ---------------------------------------------------------------
static const unsigned long GRASS_COLOR_BASE = 0xFF2A3D1E;	// dim green rooted base (baked AO)
static const unsigned long GRASS_COLOR_TIP  = 0xFF4E6B32;	// brighter lit tip

// Grass alpha texture file, resolved through the W3D asset manager / BIG archives.
// Must carry the file extension AND a real alpha channel: pmtmbweed02.dds is a 64x64
// DXT5 grass/weed billboard, so alpha-test cuts out blade silhouettes. (ztgrass.dds is
// an opaque DXT1 ground tile - it has no alpha, so the quads render as solid slabs the
// same colour as the terrain and are effectively invisible.)
#define GRASS_TEXTURE_NAME "pmtmbweed02.dds"

// Deterministic integer hash so a given map always scatters grass identically
// (rebuild is camera-window based, but the pattern per world cell is stable).
static unsigned int grassHash(Int x, Int y)
{
	unsigned int h = (unsigned int)x * 73856093u;
	h ^= (unsigned int)y * 19349663u;
	h ^= 0x9E3779B9u;
	h ^= h >> 13;
	h *= 1274126177u;
	h ^= h >> 16;
	return h;
}

// Linear ARGB tint helper (percent in -100..100), clamped per channel. A file-static
// free function so filterVisible() can call it before any class member is involved.
static void ScaleColor(unsigned long &outColor, unsigned long src, Int percent)
{
	Int a = (Int)((src >> 24) & 0xff);
	Int r = (Int)((src >> 16) & 0xff);
	Int g = (Int)((src >> 8) & 0xff);
	Int b = (Int)(src & 0xff);
	r = r + r * percent / 100;
	g = g + g * percent / 100;
	b = b + b * percent / 100;
	if (r < 0) r = 0; if (r > 255) r = 255;
	if (g < 0) g = 0; if (g > 255) g = 255;
	if (b < 0) b = 0; if (b > 255) b = 255;
	outColor = ((unsigned long)a << 24) | ((unsigned long)r << 16) | ((unsigned long)g << 8) | (unsigned long)b;
}

W3DGrassBuffer::W3DGrassBuffer()
{
	m_indexBuffer = nullptr;
	m_texture = nullptr;
	m_builtSig = -1;
	m_quadCount = 0;
	// GeneralsX @tweak Qoder 15/10/2026 Default OFF until the layer passes field
	// acceptance. updateIniSettings() re-reads TheWeatherSetting every frame, so an
	// INI "GrassEnabled = yes" re-activates the whole layer without a code change.
	m_enabled = false;
	m_hasResources = false;
	m_densityPercent = GRASS_DEFAULT_DENSITY;
	m_clumps.reserve(GRASS_MAX_CLUMPS);
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
	m_clumps.clear();
	m_allClumps.clear();
	m_builtSig = -1;	// force a full-map rescan on the next render
	m_quadCount = 0;
}

/** Release all D3D assets before a device reset (mirrors W3DSnowManager). */
void W3DGrassBuffer::ReleaseResources()
{
	REF_PTR_RELEASE(m_indexBuffer);
	REF_PTR_RELEASE(m_texture);
	m_hasResources = false;
}

/** (Re)allocate the shared index buffer + load the grass texture after a reset. */
Bool W3DGrassBuffer::ReAcquireResources()
{
	ReleaseResources();

	if (!m_enabled)
		return true;	// nothing to allocate when the layer is off.

	// Pre-fill a shared index buffer covering the maximum window: two triangles
	// per quad, vertex order tl,bl,br,tr (same layout W3DSnowManager uses).
	m_indexBuffer = NEW_REF(DX8IndexBufferClass, (GRASS_MAX_QUADS * 6));
	Bool ibOk = false;
	{
		DX8IndexBufferClass::WriteLockClass lock(m_indexBuffer);
		unsigned short *ib = lock.Get_Index_Array();
		ibOk = (ib != nullptr);
		if (ibOk)
		{
			for (Int q = 0; q < GRASS_MAX_CLUMPS * 2; ++q)
			{
				Int base = q * 4;
				ib[0] = (unsigned short)(base + 3);
				ib[1] = (unsigned short)(base + 0);
				ib[2] = (unsigned short)(base + 2);
				ib[3] = (unsigned short)(base + 2);
				ib[4] = (unsigned short)(base + 0);
				ib[5] = (unsigned short)(base + 1);
				ib += 6;
			}
		}
	}
	if (!ibOk)
	{
		// Index buffer could not be locked (device lost / low memory). Writing a null index
		// array is exactly what crashed save-loading, so drop grass for this session instead.
		fprintf(stderr, "[GRASS] WARN: index buffer lock failed (low memory) - grass disabled.\n");
		fflush(stderr);
		ReleaseResources();
		return false;
	}

	// Load the grass alpha texture through the W3D asset manager (BIG archives).
	m_texture = WW3DAssetManager::Get_Instance()->Get_Texture(GRASS_TEXTURE_NAME);
	if (!m_texture)
	{
		// Missing texture is survivable: render() falls back to a solid (null-texture) draw.
		fprintf(stderr, "[GRASS] WARN: texture '%s' failed to load - grass will be solid.\n", GRASS_TEXTURE_NAME);
		fflush(stderr);
	}

	m_hasResources = true;

	fprintf(stderr, "[GRASS] W3DGrassBuffer acquired (texture %s). texture=%p\n",
		GRASS_TEXTURE_NAME, (void *)m_texture);
	fflush(stderr);
	return true;
}

void W3DGrassBuffer::update()
{
	// M1 has no per-frame CPU animation; wind/crush vs constants arrive in S2/M2.
}

void W3DGrassBuffer::updateIniSettings()
{
	// Reflect the GrassEnabled / GrassDensity weather INI toggles (Options fallback).
	if (!TheWeatherSetting)
		return;	// weather INI not loaded yet - keep the constructor defaults.

	Bool enabled = TheWeatherSetting->m_grassEnabled;
	Int density = (Int)(TheWeatherSetting->m_grassDensity + 0.5f);
	if (density < 0) density = 0;
	if (density > 100) density = 100;

	if (enabled != m_enabled)
	{
		m_enabled = enabled;
		if (!m_enabled)
			ReleaseResources();	// turn the layer off: drop D3D assets, zero residue.
	}
	if (density != m_densityPercent)
	{
		m_densityPercent = density;
		// density is applied by the per-frame filter now - no rescan needed.
	}
}

// A terrain cell can host grass if it belongs to the grass family. ZH maps classify green
// ground under several terrain classes, not just the base TERRAIN_GRASS, so matching only
// that one leaves grassHits==0 on most maps. Cover the family; the sampler below reports
// the real class ints so the set can be tuned per map vocabulary.
static bool isGrassyTerrainClass(TerrainClass c)
{
	switch (c)
	{
		case TERRAIN_GRASS:
		case TERRAIN_ACCENT_GRASS:
		case TERRAIN_COBBLESTONE_GRASS:
		case TERRAIN_FIELD:
			return true;
		default:
			return false;
	}
}

// Stable per-map signature from the loaded map filename: changes exactly when a new map
// loads (TerrainLogic exposes no public width getter we could key off instead).
static Int grassMapSignature()
{
	AsciiString mapName = TheTerrainLogic->getSourceFilename();
	Int sig = (Int)mapName.getLength();
	for (Int i = 0; i < mapName.getLength(); ++i)
		sig = sig * 31 + (Int)(UnsignedByte)mapName.str()[i];
	return sig;
}

// Scan the WHOLE map once into the candidate set. The old design only scanned a fixed
// window around the look-at pivot, so grass was a small disc glued to the view center:
// edge grass never grew and the disc followed the camera everywhere. One full scan per
// map load removes that class of problems; what remains per frame is a cheap filter.
void W3DGrassBuffer::rebuildFullField()
{
	m_allClumps.clear();
	m_builtSig = -1;

	// Defensive: only scan once the terrain subsystems exist (mid-game).
	if (!TheTerrainVisual || !TheTerrainLogic)
		return;

	m_builtSig = grassMapSignature();

	// Full playable extent, border included (W3DTerrainLogic overrides the base stub).
	Region3D ext;
	TheTerrainLogic->getExtentIncludingBorder(&ext);

	Int cellsScanned = 0, tileNull = 0, notGrass = 0;

	// Vocabulary census keyed by terrain name (deduped, counted): reveals EVERY terrain the
	// scan meets with its class - including roads. This is the data needed to settle the
	// "grass growing on roads" report (either a road tile matches the grass family, or
	// roads are painted over grass-class cells and need a separate keep-out).
	struct VocabRow { AsciiString name; Int cls; Int cells; };
	std::vector<VocabRow> vocab;

	for (Real wy = ext.lo.y; wy < ext.hi.y; wy += GRASS_STEP)
	{
		for (Real wx = ext.lo.x; wx < ext.hi.x; wx += GRASS_STEP)
		{
			++cellsScanned;
			const Int ci = (Int)(wx / GRASS_STEP);
			const Int cj = (Int)(wy / GRASS_STEP);
			const unsigned int h = grassHash(ci, cj);

			// Jitter within the cell so the grid does not read as a checkerboard.
			const Real jx = wx + ((Real)((h >> 8) & 0xff) / 255.0f - 0.5f) * GRASS_STEP;
			const Real jy = wy + ((Real)((h >> 16) & 0xff) / 255.0f - 0.5f) * GRASS_STEP;

			TerrainType *tile = TheTerrainVisual->getTerrainTile(jx, jy);
			if (!tile) { ++tileNull; continue; }

			// Census before the gate: non-grass vocabulary (roads/dirt/etc.) must be logged too.
			AsciiString tn = tile->getName();
			Int vi;
			for (vi = 0; vi < (Int)vocab.size(); ++vi)
				if (vocab[vi].name == tn)
					break;
			if (vi == (Int)vocab.size())
			{
				VocabRow row; row.name = tn; row.cls = (Int)tile->getClass(); row.cells = 0;
				vocab.push_back(row);
			}
			vocab[vi].cells++;

			// Only grow grass on genuine grass cells (roads/dirt/snow stay bare).
			if (!isGrassyTerrainClass(tile->getClass())) { ++notGrass; continue; }

			GrassClump c;
			c.x = jx;
			c.y = jy;
			c.z = TheTerrainLogic->getGroundHeight(jx, jy);

			c.cellX = ci;
			c.cellY = cj;

			// Pre-normalized random yaw for the crossed quads (computed once here).
			Real ang = ((Real)(h & 0xff) / 255.0f) * 3.14159265f;	// 0..180 deg is enough (quads are double-sided)
			c.ax = WWMath::Fast_Cos(ang);
			c.ay = WWMath::Fast_Sin(ang);

			c.halfWidth = GRASS_HALFWIDTH_MIN + ((Real)((h >> 4) & 0x3f) / 63.0f) * GRASS_HALFWIDTH_SPAN;
			c.height    = GRASS_HEIGHT_MIN    + ((Real)((h >> 10) & 0x3f) / 63.0f) * GRASS_HEIGHT_SPAN;

			// Bake a per-clump brightness tint into the AO colors for variety.
			Int tint = -18 + (Int)((h >> 20) % 41u);	// -18..+22 percent (wider per-clump variation)
			ScaleColor(c.colorBase, GRASS_COLOR_BASE, tint);
			ScaleColor(c.colorTip, GRASS_COLOR_TIP, tint);

			m_allClumps.push_back(c);
		}
	}

	// One census printout per scan: the full terrain vocabulary this map actually uses.
	AsciiString mapName = TheTerrainLogic->getSourceFilename();
	fprintf(stderr, "[GRASS] scan '%s': cells=%d tileNull=%d notGrass=%d candidates=%d\n",
		mapName.str(), cellsScanned, tileNull, notGrass, (Int)m_allClumps.size());
	for (Int v = 0; v < (Int)vocab.size(); ++v)
	{
		fprintf(stderr, "[GRASS] vocab: '%s' class=%d grassy=%d cells=%d\n",
			vocab[v].name.str(), vocab[v].cls,
			isGrassyTerrainClass((TerrainClass)vocab[v].cls) ? 1 : 0, vocab[v].cells);
	}
	fflush(stderr);
}

// Per-frame visibility slice of the candidate set: cull to the ACTUAL view footprint (the
// radius grows with camera zoom so grass always reaches the screen edges), thin far cells
// with a stride LOD, apply the INI density, keep out building footprints (refreshed every
// pass so structures built mid-game also repel grass), and cap the draw budget.
void W3DGrassBuffer::filterVisible()
{
	m_clumps.clear();
	m_quadCount = 0;

	if (!TheTacticalView || m_allClumps.empty())
		return;

	// Center on the look-at pivot (ground center of view), never the eye position.
	const Coord3D cam = TheTacticalView->getPosition();

	// The visible ground footprint scales with zoom; grow the cull radius to match so the
	// field reaches the screen edges at every zoom instead of a fixed disc at the center.
	Real zoom = TheTacticalView->getZoom();
	if (zoom < 1.0f)
		zoom = 1.0f;
	const Real radius = GRASS_NEAR_RADIUS * zoom;
	const Real rr = radius * radius;

	// Start from the LOD stride implied by zoom; the budget loop below may double it.
	Int stride = (zoom >= 2.2f) ? 4 : ((zoom >= 1.5f) ? 2 : 1);

	Int density = m_densityPercent;
	if (density < 0) density = 0;
	if (density > 100) density = 100;

	// Building footprints: collected per pass (cheap, tens of structures) so new
	// construction immediately stops grass from poking through walls.
	std::vector<Real> occX, occY, occR;
	if (TheGameLogic)
	{
		for (Object *obj = TheGameLogic->getFirstObject(); obj; obj = obj->getNextObject())
		{
			if (!obj->isKindOf(KINDOF_STRUCTURE))
				continue;
			const GeometryInfo &gi = obj->getGeometryInfo();
			Real r = gi.getMajorRadius();
			if (gi.getGeomType() == GEOMETRY_BOX && gi.getMinorRadius() > r)
				r = gi.getMinorRadius();	// conservative: cover the whole footprint
			const Coord3D *op = obj->getPosition();
			if (!op)
				continue;
			occX.push_back(op->x);
			occY.push_back(op->y);
			occR.push_back(r + GRASS_STRUCTURE_MARGIN);
		}
	}

	// Filter + budget loop: if a pass overflows the vertex budget, double the stride and
	// redo so the thinning stays spatially EVEN (a plain break would bias the field toward
	// the scan start). Bounded: at most a few retries.
	Bool overflow;
	do
	{
		m_clumps.clear();
		overflow = false;
		for (size_t i = 0; i < m_allClumps.size(); ++i)
		{
			const GrassClump &c = m_allClumps[i];

			const Real dx = c.x - cam.x, dy = c.y - cam.y;
			if (dx * dx + dy * dy > rr)
				continue;

			if (stride > 1 &&
				((((c.cellX % stride) + stride) % stride) != 0 ||
				 (((c.cellY % stride) + stride) % stride) != 0))
				continue;

			const unsigned int h = grassHash(c.cellX, c.cellY);
			if ((Int)(h % 100u) >= density)
				continue;

			Bool inStructure = false;
			for (Int k = 0; k < (Int)occX.size(); ++k)
			{
				const Real sx = c.x - occX[k], sy = c.y - occY[k];
				if (sx * sx + sy * sy < occR[k] * occR[k]) { inStructure = true; break; }
			}
			if (inStructure)
				continue;

			m_clumps.push_back(c);
			if ((Int)m_clumps.size() >= GRASS_MAX_CLUMPS)
			{
				overflow = true;
				break;
			}
		}

		if (overflow)
			stride *= 2;
	} while (overflow && stride < 32);

	m_quadCount = (Int)m_clumps.size() * 2;

	// Throttled trace so coverage/budget behavior is checkable from the log.
	static Int s_filterCount = 0;
	if ((++s_filterCount % 60) == 0)
	{
		fprintf(stderr, "[GRASS] filter cam(%.0f,%.0f) zoom %.2f r=%.0f stride=%d drawn=%d/%d\n",
			cam.x, cam.y, zoom, radius, stride, (Int)m_clumps.size(), (Int)m_allClumps.size());
		fflush(stderr);
	}
}

// Expand one clump into two crossed quads (8 vertices) at the current write cursor.
// Pure arithmetic: the axis and colors were baked at rebuild time.
void W3DGrassBuffer::emitClumpQuads(VertexFormatXYZNDUV2 *&verts, const Matrix3D &view) const
{
	// Shared UV/color layout for both quads: tl(tip), bl(base), br(base), tr(tip).
	static const Real GRASS_U[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	static const Real GRASS_V[4] = { 0.0f, 1.0f, 1.0f, 0.0f };

	for (std::vector<GrassClump>::const_iterator it = m_clumps.begin(); it != m_clumps.end(); ++it)
	{
		const GrassClump &c = *it;
		// Quad A along (ax,ay); quad B along the perpendicular (-ay,ax).
		for (int q = 0; q < 2; ++q)
		{
			Real ux = (q == 0) ? c.ax : -c.ay;
			Real uy = (q == 0) ? c.ay :  c.ax;
			Real ox = ux * c.halfWidth;
			Real oy = uy * c.halfWidth;
			Real topZ = c.z + c.height;

			// World-space corners, then transformed into VIEW space on the CPU (the W3DSnow
			// convention): the draw uses an identity VIEW + the ambient scene projection.
			Vector3 world[4];
			world[0].Set(c.x - ox, c.y - oy, topZ);	// tl (tip)
			world[1].Set(c.x - ox, c.y - oy, c.z);		// bl (base)
			world[2].Set(c.x + ox, c.y + oy, c.z);		// br (base)
			world[3].Set(c.x + ox, c.y + oy, topZ);	// tr (tip)

			for (int k = 0; k < 4; ++k)
			{
				Vector3 eye;
				Matrix3D::Transform_Vector(view, world[k], &eye);
				verts[k].x = eye.X; verts[k].y = eye.Y; verts[k].z = eye.Z;
				verts[k].diffuse = (k == 0 || k == 3) ? c.colorTip : c.colorBase;
				verts[k].u1 = GRASS_U[k]; verts[k].v1 = GRASS_V[k];
				verts[k].nx = 0.0f; verts[k].ny = 0.0f; verts[k].nz = 0.0f;	// keep AGP write-combining active
				verts[k].u2 = 0.0f; verts[k].v2 = 0.0f;
			}
			verts += 4;
		}
	}
}

void W3DGrassBuffer::render(RenderInfoClass &rinfo)
{
	updateIniSettings();	// re-read GrassEnabled / GrassDensity before deciding to draw.

	if (!m_enabled)
		return;

	// Only meaningful once the terrain and camera subsystems exist (mid-game).
	if (!TheTerrainLogic || !TheTacticalView)
		return;

	// Grass is a mid-game decoration only. The main-menu / shell background runs this same
	// weather render pass with a fast fly-by camera; without this gate the whole placement
	// window would be rebuilt every menu frame (wasteful and rough on low-end PCs). Restrict
	// to a real match: game logic present, in a game, and NOT the shell (menu) game.
	if (!TheGameLogic || !TheGameLogic->isInGame() || TheGameLogic->isInShellGame())
	{
		if (m_hasResources)
			ReleaseResources();	// left the match: drop GPU assets, zero residue.
		return;
	}

	if (!m_hasResources)
		ReAcquireResources();
	if (!m_indexBuffer)
		return;	// device object unavailable - skip this frame.

	// Scan the full map once per map load; placement no longer depends on camera position,
	// so there is nothing else to rebuild while playing.
	const Int mapSig = grassMapSignature();
	if (mapSig != m_builtSig)
		rebuildFullField();

	// Per-frame visibility slice: cull to the actual view footprint (radius scales with
	// zoom), thin far cells with the LOD stride, apply density + keep-out, cap the budget.
	filterVisible();

	if (m_clumps.empty())
		return;	// no grass in this window (all roads/dirt/snow) - draw nothing.

	// Camera view matrix: grass corners are transformed world -> view space on the CPU, then
	// drawn with an identity VIEW transform (see the W3DSnow convention note below).
	Matrix3D viewMatrix;
	rinfo.Camera.Get_View_Matrix(&viewMatrix);

	Int vertexCount = m_quadCount * 4;

	// Fixed-pipeline path (identical FVF to the S0/S1 verification, proven to build).
	DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, dynamic_fvf_type, vertexCount);
	{
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2 *verts = lock.Get_Formatted_Vertex_Array();
		if (!verts)
		{
			// VB lock failed (low memory / device lost). Skip the frame instead of writing
			// through a null pointer; grass degrades to "off" under memory pressure.
			static Int s_vbFail = 0;
			if (s_vbFail++ < 3)
			{
				fprintf(stderr, "[GRASS] WARN: VB lock failed (low memory) - skipping grass this frame.\n");
				fflush(stderr);
			}
			return;
		}
		emitClumpQuads(verts, viewMatrix);
	}

	// W3DSnow convention: emitClumpQuads already baked the camera view into every vertex
	// (world -> view space on the CPU), so the VIEW transform must be identity here and the
	// PROJECTION is left untouched (the scene camera projection is already bound for this
	// pass). Setting a real VIEW + PROJECTION on top of view-space verts double-transforms
	// them under DXVK, which is what slid the whole grass field off-screen as the camera zoomed.
	Matrix4x4 identity(true);
	DX8Wrapper::Set_Transform(D3DTS_WORLD, identity);
	DX8Wrapper::Set_Transform(D3DTS_VIEW, identity);

	// Alpha-tested, double-sided, vertex-color-tinted grass. The ATest sprite preset
	// enables TEXTURING + ALPHATEST + DEPTH_WRITE; relaxed to two-sided and the AO
	// gradient kept so base/tip vertex colors tint the texture. With PRELIT_DIFFUSE the
	// pipeline modulates vertex ARGB by the texture on color and alpha, so the texture's
	// alpha drives the cutoff (vertex alpha is opaque 0xFF). Null texture => solid fallback.
	ShaderClass shader = ShaderClass::_PresetATestSpriteShader;
	shader.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);
	shader.Set_Primary_Gradient(ShaderClass::GRADIENT_MODULATE);
	DX8Wrapper::Set_Shader(shader);

	VertexMaterialClass *vmat = VertexMaterialClass::Get_Preset(VertexMaterialClass::PRELIT_DIFFUSE);
	DX8Wrapper::Set_Material(vmat);
	REF_PTR_RELEASE(vmat);

	DX8Wrapper::Set_Texture(0, m_texture);

	DX8Wrapper::Set_Index_Buffer(m_indexBuffer, 0);
	DX8Wrapper::Set_Vertex_Buffer(vb_access);
	DX8Wrapper::Draw_Triangles(0, m_quadCount * 2, 0, vertexCount);

	DX8Wrapper::Set_Texture(0, nullptr);
}
