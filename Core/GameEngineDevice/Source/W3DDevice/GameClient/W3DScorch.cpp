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

#include "W3DDevice/GameClient/W3DScorch.h"

#include "Common/GameMemory.h"
#include "Common/GameType.h"
#include "Common/GlobalData.h"
#include "Common/MapObject.h"
#include "W3DDevice/GameClient/TerrainTex.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "WW3D2/dx8wrapper.h"

W3DScorch::W3DScorch(bool deduplicateScorches)
  : m_vertexScorch(nullptr)
  , m_indexScorch(nullptr)
  , m_scorchTexture(nullptr)
  , m_curNumScorchVertices(0)
  , m_curNumScorchIndices(0)
  , m_needBufferRecompute(true)
  , m_deduplicateScorches(deduplicateScorches)
{}

W3DScorch::~W3DScorch() { freeBuffers(); }

void W3DScorch::allocateBuffers()
{
	freeBuffers();
	// GeneralsX @bugfix 28/10/2026 SCORCHTEAR: updateScorches() rewrites the whole decal list every time a
	// crater is added, and craters are added by explosions - i.e. right when the frame being rendered is
	// already busy. As USAGE_DEFAULT both buffers were a single fixed region, so the rewrite landed on the
	// bytes the frames still in flight were reading and one frame drew a decal quad whose vertices came
	// from two different lists: a long dark streak dragged across the ground that is gone the next frame.
	// USAGE_DYNAMIC + D3DLOCK_DISCARD lets the Vulkan backing rotate to a fresh per-frame region instead.
	m_vertexScorch = NEW_REF(DX8VertexBufferClass, (DX8_FVF_XYZDUV1, MAX_SCORCH_VERTEX, DX8VertexBufferClass::USAGE_DYNAMIC));
	m_indexScorch = NEW_REF(DX8IndexBufferClass, (MAX_SCORCH_INDEX, DX8IndexBufferClass::USAGE_DYNAMIC));
	m_scorchTexture = NEW ScorchTextureClass;
	invalidateBuffers();
}

void W3DScorch::freeBuffers()
{
	REF_PTR_RELEASE(m_vertexScorch);
	REF_PTR_RELEASE(m_indexScorch);
	REF_PTR_RELEASE(m_scorchTexture);
}

void W3DScorch::clearAllScorches()
{
	m_scorches.clear();
	invalidateBuffers();
}

void W3DScorch::invalidateBuffers()
{
	m_needBufferRecompute = true;
	m_curNumScorchVertices = 0;
	m_curNumScorchIndices = 0;
}

void W3DScorch::invalidateTexture()
{
	if (m_scorchTexture)
	{
		m_scorchTexture->Invalidate();
	}
}

void W3DScorch::addScorch(Vector3 location, Real radius, Scorches type)
{
	TScorch scorch;
	scorch.location = location;
	scorch.radius = radius;
	if (type >= 0 && (Int)type < SCORCH_MARKS_IN_TEXTURE)
		scorch.scorchType = type;
	else
		scorch.scorchType = SCORCH_1;

	if (m_deduplicateScorches && isDuplicate(scorch))
	{
		return;
	}

	// GeneralsX @feature PS3.0-P3-31 14/10/2026 G6: the retail hard cap of 500 live scorches evicted
	// marks on busy maps; GENERALSX_SCORCH_MAX raises/lowers it (clamped 50..2000, default = the
	// retail MAX_SCORCH_MARKS so shipped behaviour is unchanged). Marker line proves the override is read.
	static int scorchMax = -1;
	if (scorchMax < 0) {
		scorchMax = MAX_SCORCH_MARKS;
		const char* ev = getenv("GENERALSX_SCORCH_MAX");
		if (ev != nullptr && *ev != 0) {
			scorchMax = atoi(ev);
			if (scorchMax < 50) scorchMax = 50;
			if (scorchMax > 2000) scorchMax = 2000;
			fprintf(stderr, "[SCORCHMAX] cap=%d\n", scorchMax);
			fflush(stderr);
		}
	}
	if ((Int)m_scorches.size() >= scorchMax)
	{
		m_scorches.pop_front();
	}
	m_scorches.push_back(scorch);

	invalidateBuffers();
}

Bool W3DScorch::isDuplicate(const TScorch& scorch) const
{
	const Real limit = scorch.radius / 4;
	for (std::deque<TScorch>::const_iterator it = m_scorches.begin(); it != m_scorches.end(); ++it)
	{
		if (it->scorchType == scorch.scorchType &&
		    fabsf(scorch.location.X - it->location.X) < limit &&
		    fabsf(scorch.location.Y - it->location.Y) < limit &&
		    fabsf(scorch.radius - it->radius) < limit)
		{
			return true;
		}
	}
	return false;
}

void W3DScorch::drawScorches(WorldHeightMap& map)
{
	updateScorches(map);
	if (m_curNumScorchIndices == 0)
	{
		return;
	}

	// GeneralsX @bugfix Qoder 22/10/2026 Never draw the MissingTexture fallback as a scorch decal:
	// its X1R5G5B5 surface has alpha = 1 everywhere, so SRCALPHA blending paints the whole quad as an
	// opaque black square. The base Generals asset path fix should make EXScorch01 load; this guard
	// keeps a future lookup failure from turning every crater into a hard square.
	if (m_scorchTexture == nullptr || m_scorchTexture->Is_Missing_Texture())
	{
		static Bool s_missingScorchLogged = FALSE;
		if (!s_missingScorchLogged)
		{
			s_missingScorchLogged = TRUE;
			fprintf(stderr, "[SCORCH] EXScorch atlas unavailable; scorch decals skipped to avoid black quads\n");
			fflush(stderr);
		}
		return;
	}

	DX8Wrapper::Set_Index_Buffer(m_indexScorch, 0);
	DX8Wrapper::Set_Vertex_Buffer(m_vertexScorch);
	DX8Wrapper::Set_Shader(ShaderClass::_PresetAlphaShader);

	DX8Wrapper::Set_Texture(0, m_scorchTexture);
	DX8Wrapper::Draw_Triangles(0, m_curNumScorchIndices / 3, 0, m_curNumScorchVertices);
}

static Real getMapHeight(WorldHeightMap& map, Int x, Int y)
{
	x += map.getBorderSizeInline();
	y += map.getBorderSizeInline();
	return map.getDataPtr()[x + y * map.getXExtent()] * MAP_HEIGHT_SCALE;
}

void W3DScorch::updateScorches(WorldHeightMap& map)
{
	if (!m_needBufferRecompute || m_scorches.empty() || !m_indexScorch || !m_vertexScorch)
	{
		return;
	}

	m_needBufferRecompute = false;
	m_curNumScorchVertices = 0;
	m_curNumScorchIndices = 0;

	// Full rewrite of both buffers (every live decal is re-emitted below), so DISCARD is exactly right:
	// the new list goes to a fresh region and the frames still in flight keep the list they recorded.
	DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexScorch, D3DLOCK_DISCARD);
	UnsignedShort* ib = lockIdxBuffer.Get_Index_Array();

	DX8VertexBufferClass::WriteLockClass lockVtxBuffer(m_vertexScorch, D3DLOCK_DISCARD);
	VertexFormatXYZDUV1* vb = (VertexFormatXYZDUV1*)lockVtxBuffer.Get_Vertex_Array();

	Real shadeR = (TheGlobalData->m_terrainAmbient[0].red + TheGlobalData->m_terrainDiffuse[0].red) / 2.0f;
	Real shadeG = (TheGlobalData->m_terrainAmbient[0].green + TheGlobalData->m_terrainDiffuse[0].green) / 2.0f;
	Real shadeB = (TheGlobalData->m_terrainAmbient[0].blue + TheGlobalData->m_terrainDiffuse[0].blue) / 2.0f;
	UnsignedInt diffuse = DX8Wrapper::Convert_Color_Clamp(Vector4(shadeR, shadeG, shadeB, 1.0f));

	// TheSuperHackers @info Scorches are written in reverse order to ensure that the last added scorches fit in the buffers.
	for (std::deque<TScorch>::reverse_iterator it = m_scorches.rbegin(); it != m_scorches.rend(); ++it)
	{
		if (writeScorchToBuffer(*it, map, diffuse,
		                        vb + m_curNumScorchVertices, ib + m_curNumScorchIndices) == SCORCH_BUFFER_FULL)
		{
			return;
		}
	}
}

W3DScorch::WriteScorchResult W3DScorch::writeScorchToBuffer(const TScorch& scorch, WorldHeightMap& map,
                                                          UnsignedInt diffuse, VertexFormatXYZDUV1* curVb,
                                                          UnsignedShort* curIb)
{
	Real radius = scorch.radius;
	Vector3 loc = scorch.location;
	Int type = scorch.scorchType;
	Real amtToFloat = MAP_HEIGHT_SCALE / 10;

	Int minX = REAL_TO_INT_FLOOR((loc.X - radius) / MAP_XY_FACTOR);
	Int minY = REAL_TO_INT_FLOOR((loc.Y - radius) / MAP_XY_FACTOR);
	if (minX < -map.getBorderSizeInline())
		minX = -map.getBorderSizeInline();
	if (minY < -map.getBorderSizeInline())
		minY = -map.getBorderSizeInline();
	Int maxX = REAL_TO_INT_CEIL((loc.X + radius) / MAP_XY_FACTOR);
	Int maxY = REAL_TO_INT_CEIL((loc.Y + radius) / MAP_XY_FACTOR);
	maxX++;
	maxY++;
	if (maxX > map.getXExtent() - map.getBorderSizeInline())
	{
		maxX = map.getXExtent() - map.getBorderSizeInline();
	}
	if (maxY > map.getYExtent() - map.getBorderSizeInline())
	{
		maxY = map.getYExtent() - map.getBorderSizeInline();
	}

	const Int vertexCountX = maxX - minX;
	const Int vertexCountY = maxY - minY;
	if (vertexCountX <= 0 || vertexCountY <= 0)
	{
		return SCORCH_SKIPPED;
	}

	const Int requiredVertices = vertexCountX * vertexCountY;
	const Int requiredIndices = 6 * (vertexCountX - 1) * (vertexCountY - 1);
	if (m_curNumScorchVertices + requiredVertices > MAX_SCORCH_VERTEX ||
	    m_curNumScorchIndices + requiredIndices > MAX_SCORCH_INDEX)
	{
		return SCORCH_BUFFER_FULL;
	}

	Int startVertex = m_curNumScorchVertices;
	Int i, j;
	for (j = minY; j < maxY; j++)
	{
		for (i = minX; i < maxX; i++)
		{
			curVb->diffuse = diffuse;
			Real theZ = amtToFloat + getMapHeight(map, i, j);
			// The scorchmarks are spaced out by 1.5 in the texture.
			Real uOffset = (type % SCORCH_PER_ROW) * 1.5f;
			Real vOffset = (type / SCORCH_PER_ROW) * 1.5f;
			Real X = i * MAP_XY_FACTOR;
			Real Y = j * MAP_XY_FACTOR;
			curVb->u1 = (uOffset + 0.5f + (X - loc.X) / (2 * radius)) / (SCORCH_PER_ROW + 1);
			curVb->v1 = (vOffset + 0.5f + (Y - loc.Y) / (2 * radius)) / (SCORCH_PER_ROW + 1);
			curVb->x = X;
			curVb->y = Y;
			curVb->z = theZ;
			curVb++;
			m_curNumScorchVertices++;
		}
	}
	Int yOffset = maxX - minX;
	for (j = 0; j < maxY - minY - 1; j++)
	{
		for (i = 0; i < maxX - minX - 1; i++)
		{
			Int xNdx = i + minX + map.getBorderSizeInline();
			Int yNdx = j + minY + map.getBorderSizeInline();
			Bool flipForBlend = map.getFlipState(xNdx, yNdx);
#if 0
			UnsignedByte alpha[4];
			float UA[4], VA[4];
			map.getAlphaUVData(xNdx, yNdx, UA, VA, alpha, &flipForBlend);
#endif
			if (flipForBlend)
			{
				*curIb++ = startVertex + j * yOffset + i + 1;
				*curIb++ = startVertex + j * yOffset + i + yOffset;
				*curIb++ = startVertex + j * yOffset + i;
				*curIb++ = startVertex + j * yOffset + i + 1;
				*curIb++ = startVertex + j * yOffset + i + 1 + yOffset;
				*curIb++ = startVertex + j * yOffset + i + yOffset;
			}
			else
			{
				*curIb++ = startVertex + j * yOffset + i;
				*curIb++ = startVertex + j * yOffset + i + 1 + yOffset;
				*curIb++ = startVertex + j * yOffset + i + yOffset;
				*curIb++ = startVertex + j * yOffset + i;
				*curIb++ = startVertex + j * yOffset + i + 1;
				*curIb++ = startVertex + j * yOffset + i + 1 + yOffset;
			}
			m_curNumScorchIndices += 6;
		}
	}

	return SCORCH_WRITTEN;
}
