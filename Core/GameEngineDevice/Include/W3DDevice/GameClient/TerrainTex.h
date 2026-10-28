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

// TerrainTex.h
// Class to generate texture for terrain.
// Author: John Ahlquist, April 2001

#pragma once

//#define DO_8STAGE_TERRAIN_PASS		//optimized terrain rendering for Nvidia based cards

#include "WW3D2/texture.h"
#include "WWMath/matrix3d.h"
#include "Common/AsciiString.h"

class WorldHeightMap;
#define TILE_OFFSET 8
/** ***********************************************************************
**                             TerrainTextureClass
***************************************************************************/
class TerrainTextureClass : public TextureClass
{
	W3DMPO_CODE(TerrainTextureClass)
protected:
	virtual void Apply(unsigned int stage) override;

public:
		/// Create texture for a height map.
		TerrainTextureClass(int height);

		// GeneralsX @feature 24/10/2026 P25 C8: normal-atlas variant. mipChain=false builds a single
		// level, so the mip chain cannot average normals across the gutter-less tile edges.
		TerrainTextureClass(int height, bool mipChain);

		/// Create texture for a height map.
		TerrainTextureClass(int height, int width);

		// just use default destructor. ~TerrainTextureClass();
public:
	int update(WorldHeightMap *htMap); ///< Sets the pixels, and returns the actual height of the texture.
	// GeneralsX @feature PS3.0 L2.1 TNM 17/10/2026: packs the parallel tangent-normal atlas from
	// htMap's m_nmTiles. Same tile positions as update() (the layout is owned by the diffuse tiles),
	// flat (128,128,255,255) wherever a normal tile is missing. Returns the texture height like
	// update(), or 0 when the atlas format cannot carry 8-bit channels (caller then drops it).
	int updateNM(WorldHeightMap *htMap);
	Bool updateFlat(WorldHeightMap *htMap, Int xCell, Int yCell, Int cellWidth, Int pixelsPerCell); ///< Sets the pixels.
	void setLOD(Int LOD);
};


class AlphaTerrainTextureClass : public TextureClass
{
	W3DMPO_CODE(AlphaTerrainTextureClass)
protected:
		virtual void Apply(unsigned int stage) override;
public:
		// Create texture for a height map.
		AlphaTerrainTextureClass(TextureClass *pBaseTex );

		// just use default destructor. ~TerrainTextureClass();

};

/** ***********************************************************************
**                             AlphaEdgeTextureClass
***************************************************************************/
class AlphaEdgeTextureClass : public TextureClass
{
	W3DMPO_CODE(AlphaEdgeTextureClass)
protected:
	virtual void Apply(unsigned int stage) override;
	int update256(WorldHeightMap *htMap);///< Sets the pixels, and returns the actual height of the texture.

public:
		/// Create texture for a height map.
		AlphaEdgeTextureClass(int height, MipCountType mipLevelCount = MIP_LEVELS_3 );

		// just use default destructor. ~TerrainTextureClass();
public:
	int update(WorldHeightMap *htMap); ///< Sets the pixels, and returns the actual height of the texture.

};

class LightMapTerrainTextureClass : public TextureClass
{
	W3DMPO_CODE(LightMapTerrainTextureClass)
protected:
		virtual void Apply(unsigned int stage) override;

public:
		// Create texture from a height map.
		LightMapTerrainTextureClass( AsciiString name, MipCountType mipLevelCount = MIP_LEVELS_ALL );

		// just use default destructor.
};

class ScorchTextureClass : public TextureClass
{
	W3DMPO_CODE(ScorchTextureClass)
protected:
		virtual void Apply(unsigned int stage) override;

public:
		// Create texture.
		ScorchTextureClass( MipCountType mipLevelCount = MIP_LEVELS_3 );

		// just use default destructor. ~ScorchTextureClass();
};

class CloudMapTerrainTextureClass : public TextureClass
{
	W3DMPO_CODE(CloudMapTerrainTextureClass)
protected:
		virtual void Apply(unsigned int stage) override;

protected:
		float m_xSlidePerSecond ;	 ///< How far the clouds move per second.
		float m_ySlidePerSecond ;	 ///< How far the clouds move per second.
		int	  m_curTick;
		float m_xOffset;
		float m_yOffset;


public:
		// Create texture from a height map.
		CloudMapTerrainTextureClass( MipCountType mipLevelCount = MIP_LEVELS_ALL );

		// just use default destructor. ~TerrainTextureClass();

		void restore();
};
