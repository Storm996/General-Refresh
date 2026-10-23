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

// FILE: TerrainTex.cpp ////////////////////////////////////////////////
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
// File name: TerrainTex.cpp
//
// Created:   John Ahlquist, April 2001
//
// Desc:      TextureClass overrides to perform custom texturing for the terrain.
//
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "W3DDevice/GameClient/TerrainTex.h"
#include "W3DDevice/GameClient/WorldHeightMap.h"
#include "W3DDevice/GameClient/TileData.h"
#include "Common/GlobalData.h"
#include "WW3D2/dx8wrapper.h"
#include "d3dx9tex.h"
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.3: converge CPU-written terrain atlases into the
// Vulkan texture mirror. Always included: VKTex_Mirror_D3D_Texture exists on every platform (no-op
// unless the win64vk Vulkan core is up), and z_gameenginedevice is not built with SAGE_USE_VULKAN_CORE.
#include "WW3D2/Backend/VulkanTexMirror.h"

/******************************************************************************
						TerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

// GeneralsX @feature PS3.0-P3-6 14/10/2026 atlas precision is now a measured choice instead of a
// hard-coded revert.  Retail-then-our-10/07 build uploaded the terrain atlas as A8R8G8B8; the
// 10/10 revert to A1R5G5B5 was justified by a 32-bit-process device-memory wall (two 8192x4096
// atlases with mip chains ~170 MiB each pushed the battlefield past the ~1.5 GiB DXVK/NVIDIA
// budget and a 128 MiB vkAllocateMemory was refused).  This binary is x64 with a 16 GiB card, so
// the wall is gone - but 5-bit green/blue banding in dark gradients (snow, dusk terrain) is not
// gone, so pick the highest precision this device can actually hold:
//   * GENERALSX_TERRAIN_ATLAS=16 / =32 are QA overrides that skip the measurement entirely.
//   * default (auto) requires BOTH an allocation probe of the exact size/level-count in
//     D3DPOOL_DEFAULT *and* a device-reported headroom test (GetAvailableTextureMem must cover the
//     extra bytes of the 32-bit chain plus a 256 MiB safety margin for everything allocated after
//     this point - the historical failure was a LATER allocation, not the atlas itself).
// A rejected probe or thin headroom leaves the retail-10/10 16-bit behaviour byte-for-byte intact.
enum TerrainAtlasPrecision
{
	TERRAIN_ATLAS_AUTO = 0,	///<measure: 32-bit if it allocates and the device can hold it.
	TERRAIN_ATLAS_16 = 1,	///<forced A1R5G5B5 (legacy / QA A-B leg).
	TERRAIN_ATLAS_32 = 2,	///<forced A8R8G8B8 (QA A-B leg, no measurement).
};

static Int Terrain_Atlas_Precision_Mode()
{
	static Int mode = -1;
	if (mode < 0)
	{
		mode = TERRAIN_ATLAS_AUTO;
		const char* env = getenv("GENERALSX_TERRAIN_ATLAS");
		if (env != nullptr)
		{
			if (strcmp(env, "16") == 0) mode = TERRAIN_ATLAS_16;
			else if (strcmp(env, "32") == 0) mode = TERRAIN_ATLAS_32;
		}
		fprintf(stderr, "[TERRAIN] atlas precision mode: %s\n",
			mode == TERRAIN_ATLAS_16 ? "forced 16-bit (legacy)"
			: mode == TERRAIN_ATLAS_32 ? "forced 32-bit (QA override)"
			: "auto (measure per atlas)");
		fflush(stderr);
	}
	return mode;
}

// Full mip-chain byte cost of a w x h texture at the given bytes per texel (power-of-two chain
// down to 1x1; both atlases here are POT in each dimension).
static unsigned Terrain_Atlas_Bytes(Int width, Int height, Int bytesPerPixel)
{
	unsigned bytes = 0;
	Int w = width, h = height;
	while (w > 0 && h > 0)
	{
		bytes += (unsigned)w * (unsigned)h * (unsigned)bytesPerPixel;
		if (w == 1 && h == 1) break;
		if (w > 1) w >>= 1;
		if (h > 1) h >>= 1;
	}
	return bytes;
}

static Int Terrain_Atlas_Mip_Levels(Int width, Int height)
{
	Int levels = 1;
	while (width > 1 || height > 1)
	{
		if (width > 1) width >>= 1;
		if (height > 1) height >>= 1;
		++levels;
	}
	return levels;
}

static WW3DFormat Choose_Terrain_Atlas_Format(Int width, Int height, const char* which)
{
	const Int mode = Terrain_Atlas_Precision_Mode();
	if (mode == TERRAIN_ATLAS_16) return WW3D_FORMAT_A1R5G5B5;
	if (mode == TERRAIN_ATLAS_32) return WW3D_FORMAT_A8R8G8B8;

	IDirect3DDevice9* dev = DX8Wrapper::_Get_D3D_Device8();
	if (dev == nullptr) return WW3D_FORMAT_A1R5G5B5;

	const Int levels = Terrain_Atlas_Mip_Levels(width, height);
	IDirect3DTexture9* probe = nullptr;
	HRESULT hr = dev->CreateTexture(width, height, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &probe, nullptr);
	if (FAILED(hr) || probe == nullptr)
	{
		if (probe != nullptr) probe->Release();
		fprintf(stderr, "[TERRAIN] atlas %s %dx%d A8R8G8B8 probe REFUSED hr=0x%08X -> 16-bit\n",
			which, width, height, (unsigned)hr);
		fflush(stderr);
		return WW3D_FORMAT_A1R5G5B5;
	}
	probe->Release();

	// Headroom: the 32-bit chain costs this much more than the 16-bit one, and everything allocated
	// after the terrain textures still has to fit (that is what broke on 10/07).  A device that
	// reports 0 (or an absurdly large value it cannot honour) gets no benefit of the doubt beyond the
	// probe itself; GetAvailableTextureMem is documented in KB.
	const unsigned delta = Terrain_Atlas_Bytes(width, height, 4) - Terrain_Atlas_Bytes(width, height, 2);
	const unsigned margin = 256u * 1024u * 1024u;
	unsigned availBytes = (unsigned)dev->GetAvailableTextureMem() * 1024u;
	if (availBytes != 0 && availBytes < delta + margin)
	{
		fprintf(stderr, "[TERRAIN] atlas %s %dx%d 32-bit allocates but headroom is thin (%u MiB free, %u MiB needed + margin) -> 16-bit\n",
			which, width, height, availBytes / (1024u * 1024u), delta / (1024u * 1024u));
		fflush(stderr);
		return WW3D_FORMAT_A1R5G5B5;
	}
	fprintf(stderr, "[TERRAIN] atlas %s %dx%d x%d mips: 32-bit A8R8G8B8 accepted (probe ok, %u MiB free)\n",
		which, width, height, levels, availBytes / (1024u * 1024u));
	fflush(stderr);
	return WW3D_FORMAT_A8R8G8B8;
}

//=============================================================================
// TerrainTextureClass::TerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 32 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
// GeneralsX @bugfix Qoder 10/10/2026 Revert the 10/07 32-bit atlas upgrade:
// two 8192x4096 A8R8G8B8 atlases (~170 MiB each with mip chains) plus the
// D3DXFilterTexture scratch peak pushed the battlefield past the ~1.5 GiB
// device-memory ceiling the NVIDIA driver enforces on this 32-bit build
// (vkAllocateMemory 128 MiB refused with only 1.4 GiB allocated). 16-bit
// A1R5G5B5 halves the atlas footprint while keeping the mip chain for
// distant terrain sampling.
// GeneralsX @feature PS3.0-P3-6 supersedes that fixed choice with the measured ladder above.
TerrainTextureClass::TerrainTextureClass(int height) :
	TextureClass(TEXTURE_WIDTH, height,
		Choose_Terrain_Atlas_Format(TEXTURE_WIDTH, height, "blend"), MIP_LEVELS_ALL )
{
	// GeneralsX @bugfix PS3.0-P3-6 14/10/2026 the old line reported "A8R8G8B8" unconditionally and
	// was wrong since the 10/10 revert; it now states the format this atlas actually got.
	fprintf(stderr, "[TERRAIN] ctor: %s atlas %d x %d\n",
		Get_Texture_Format() == WW3D_FORMAT_A8R8G8B8 ? "A8R8G8B8" : "A1R5G5B5",
		TEXTURE_WIDTH, height);
	fflush(stderr);
}

//=============================================================================
// TerrainTextureClass::TerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 32 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
// GeneralsX @bugfix Qoder 10/10/2026 Revert the 10/07 32-bit atlas upgrade
// (see the (height) ctor note above): keeps flat/background terrain textures
// inside the 32-bit build's device-memory budget.
// GeneralsX @feature PS3.0-P3-6 14/10/2026 same measured ladder as the blend atlas.
TerrainTextureClass::TerrainTextureClass(int height, int width) :
	TextureClass(width, height,
		Choose_Terrain_Atlas_Format(width, height, "flat"), MIP_LEVELS_ALL )
{
}


//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
int TerrainTextureClass::update(WorldHeightMap *htMap)
{
	// D3DTexture is our texture;

	IDirect3DSurface9 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));
	if (surface_desc.Width < TEXTURE_WIDTH) {
		surface_level->Release();
		return 0;
	}

	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));

	Int tilePixelExtent = TILE_PIXEL_EXTENT;
	Int tilesPerRow = surface_desc.Width/(2*TILE_PIXEL_EXTENT+TILE_OFFSET);
	tilesPerRow *= 2;
//	Int numRows = surface_desc.Height/(tilePixelExtent+TILE_OFFSET);
#ifdef RTS_DEBUG
	//DEBUG_ASSERTCRASH(tilesPerRow*numRows >= htMap->m_numBitmapTiles, ("Too many tiles."));
	DEBUG_ASSERTCRASH((Int)surface_desc.Width >= tilePixelExtent*tilesPerRow, ("Bitmap too small."));
#endif
	Int pixelBytes = 0;
	if (surface_desc.Format == D3DFMT_A1R5G5B5) {
		pixelBytes = 2;
#if 0
		UnsignedInt cellX, cellY;
		for (cellX = 0; cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(cellY*surface_desc.Width+cellX)*2;
				*((Short*)pBGR) = (((255-2*cellY)>>3)<<10) + ((4*cellX)>>4);
			}
		}
#endif
		Int tileNdx;
		for (tileNdx=0; tileNdx < htMap->m_numBitmapTiles; tileNdx++) {
			TileData *pTile = htMap->getSourceTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real tile offsets start at 2.  jba.

			Int i,j;
			for (j=0; j<tilePixelExtent; j++) {
				UnsignedByte *pBGR = pTile->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				Int row = position.y+j;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;

				Int column = position.x;
				pBGRX += column*pixelBytes;
				for (i=0; i<tilePixelExtent; i++) {
					*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
					pBGRX +=pixelBytes;
					pBGR +=TILE_BYTES_PER_PIXEL;
				}
			}
		}
	} else if (surface_desc.Format == D3DFMT_A8R8G8B8) {
		// GeneralsX @performance Qoder 10/07/2026 Upload path for the new
		// 32-bit atlas format: BGRA tile data is packed verbatim.
		pixelBytes = 4;
		Int tileNdx;
		for (tileNdx=0; tileNdx < htMap->m_numBitmapTiles; tileNdx++) {
			TileData *pTile = htMap->getSourceTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real tile offsets start at 2.  jba.

			Int i,j;
			for (j=0; j<tilePixelExtent; j++) {
				UnsignedByte *pBGR = pTile->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				Int row = position.y+j;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;

				Int column = position.x;
				pBGRX += column*pixelBytes;
				for (i=0; i<tilePixelExtent; i++) {
					*((Int*)pBGRX) = (Int)(0xFF000000u + ((Int)pBGR[2]<<16) + ((Int)pBGR[1]<<8) + pBGR[0]);
					pBGRX +=pixelBytes;
					pBGR +=TILE_BYTES_PER_PIXEL;
				}
			}
		}
	}
	// GeneralsX @bugfix Qoder 10/07/2026 Runtime diagnostic for the atlas
	// upload path.
	fprintf(stderr, "[TERRAIN] update: fmt=0x%08X w=%d h=%d pixelBytes=%d tiles=%d reduction=%d\n",
		(unsigned)surface_desc.Format, surface_desc.Width, surface_desc.Height,
		pixelBytes, htMap->m_numBitmapTiles, WW3D::Get_Texture_Reduction());
	fflush(stderr);
	if (pixelBytes) {
		// Now draw the 4 pixel border around each tile class.
		Int texClass;
		for (texClass=0; texClass<htMap->m_numTextureClasses; texClass++) {
			Int width = htMap->m_textureClasses[texClass].width;
			ICoord2D origin = htMap->m_textureClasses[texClass].positionInTexture;
			if (origin.x<=0) continue;
			width *= TILE_PIXEL_EXTENT;
			// Duplicate 4 columns of pixels before and after.
			Int j;
			for (j=0; j<width; j++) {
				Int row = origin.y+j;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;

				Int column = origin.x;
				pBGRX += column*pixelBytes;
				// copy before
				memcpy(pBGRX-(4)*pixelBytes, pBGRX+(width-4)*pixelBytes, 4*pixelBytes);
				// copy after
				memcpy(pBGRX+(width*pixelBytes), pBGRX, 4*pixelBytes);
			}

			// Duplicate 4 rows of pixels before and after.
			for (j=0; j<4; j++) {
				// copy before.
				Int row = origin.y-j-1;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;
				UnsignedByte *target = pBGRX+(origin.x-4)*pixelBytes;
				memcpy(target, target+width*surface_desc.Width*pixelBytes, (width+8)*pixelBytes);
				// copy after.
				row = origin.y+j;
				pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;
				target = pBGRX+(origin.x-4)*pixelBytes;
				memcpy(target+width*surface_desc.Width*pixelBytes, target, (width+8)*pixelBytes);
			}

		}

	}
	// GeneralsX @bugfix Qoder 19/10/2026 BUG-03 layer4: one-shot red-garbage scan of the packed
	// atlas before it is mirrored to the Vulkan side. The terrain shader multiplies u_tex[0..3]
	// atlas samples, so a never-written cell (position.x<=0 skip above) holding red backing-store
	// garbage would burn red patches at terrain-tile granularity. First pass used a loose
	// r>g+40&&r>b+40 rule and hit 850k pixels: that is just desert dirt (R>G>B is the map palette,
	// e.g. 219,178,109). This pass separates truly saturated red garbage (r>=100 with g and b
	// both under a third of r) from zeroed never-written cells (all four channels 0), each with
	// its own budget so a clean atlas can be stated as fact rather than assumed.
	{
		static bool s_scanned = false;
		if (!s_scanned && pixelBytes == 4) {
			s_scanned = true;
			const UnsignedByte* bits = (const UnsignedByte*)locked_rect.pBits;
			long long hardRedTotal = 0, zeroTotal = 0;
			Int reports = 0;
			for (UnsignedInt y = 0; y < surface_desc.Height; y++) {
				const UnsignedByte* rowPtr = bits + (size_t)y * surface_desc.Width * 4;
				for (UnsignedInt x = 0; x < surface_desc.Width; x++) {
					const Int bv = rowPtr[x*4+0], gv = rowPtr[x*4+1], rv = rowPtr[x*4+2];
					const Int av = rowPtr[x*4+3];
					if (rv >= 100 && gv*3 <= rv && bv*3 <= rv) {
						++hardRedTotal;
						if (reports < 12) { ++reports;
							fprintf(stderr, "[TERRAIN] hardred at (%u,%u) rgba=(%d,%d,%d,%d)\n",
								(unsigned)x, (unsigned)y, rv, gv, bv, av);
						}
					} else if ((rv | gv | bv | av) == 0) {
						++zeroTotal;
					}
				}
			}
			fprintf(stderr, "[TERRAIN] atlas scan v2: hardred=%lld zero=%lld of %ux%u\n",
				hardRedTotal, zeroTotal, (unsigned)surface_desc.Width, (unsigned)surface_desc.Height);
			fflush(stderr);
		}
	}

	// GeneralsX @bugfix Qoder 21/10/2026: same background fill as the 16-bit path below. The diffuse
	// atlas is 32-bit in the shipped build; ~60% of it is unwritten transparent black, so every mip
	// level below the first averages black into the ground colour. Cells whose UV jumps at a base-tile
	// boundary make the sampler dive to those levels, which drew a black grid line over grass.
	if (pixelBytes == 4 &&
		(surface_desc.Format == D3DFMT_A8R8G8B8 || surface_desc.Format == D3DFMT_X8R8G8B8)) {
		UnsignedByte *bits = (UnsignedByte*)locked_rect.pBits;
		unsigned long long sr = 0, sg = 0, sb = 0, n = 0;
		for (UnsignedInt fy = 0; fy < surface_desc.Height; ++fy) {
			const UnsignedByte *p = bits + (size_t)fy * locked_rect.Pitch;
			for (UnsignedInt fx = 0; fx < surface_desc.Width; ++fx, p += 4) {
				if ((p[0] | p[1] | p[2] | p[3]) == 0) continue;
				sb += p[0]; sg += p[1]; sr += p[2]; ++n;
			}
		}
		if (n > 0) {
			const UnsignedByte fr = (UnsignedByte)(sr / n);
			const UnsignedByte fg = (UnsignedByte)(sg / n);
			const UnsignedByte fb = (UnsignedByte)(sb / n);
			unsigned long long filled = 0;
			for (UnsignedInt fy = 0; fy < surface_desc.Height; ++fy) {
				UnsignedByte *p = bits + (size_t)fy * locked_rect.Pitch;
				for (UnsignedInt fx = 0; fx < surface_desc.Width; ++fx, p += 4) {
					if ((p[0] | p[1] | p[2] | p[3]) != 0) continue;
					p[0] = fb; p[1] = fg; p[2] = fr; p[3] = 255; ++filled;
				}
			}
			fprintf(stderr, "[TERRAIN] atlas background fill: rgb=(%u,%u,%u) texels=%llu of %ux%u\n",
				(unsigned)fr, (unsigned)fg, (unsigned)fb, filled,
				(unsigned)surface_desc.Width, (unsigned)surface_desc.Height);
			fflush(stderr);
		}
	}

	surface_level->UnlockRect();
	surface_level->Release();
	// GeneralsX @performance Qoder 10/07/2026 Triangle instead of box filtering:
	// box-averaging the mips destroys high-frequency tile detail; triangle
	// weights keep more contrast in the lower mip levels.
	HRESULT filter_hr = D3DXFilterTexture(Peek_D3D_Texture(), nullptr, 0, D3DX_FILTER_TRIANGLE);
	DX8_ErrorCode(filter_hr);
	fprintf(stderr, "[TERRAIN] D3DXFilterTexture(TRIANGLE) hr=0x%08X\n", (unsigned)filter_hr);
	fflush(stderr);
	// GeneralsX @bugfix Qoder 19/10/2026 BUG-03 identity probe: the diffuse atlas mirror answers with
	// a silent false when the facade handle is null or the tex layer is not ready; name the outcome.
	{
		static bool s_once = false;
		const bool mirrored = VKTex_Mirror_D3D_Texture(Peek_D3D_Texture());
		if (!s_once) { s_once = true;
			fprintf(stderr, "[TERRAIN] diffuse atlas mirror: d3d=%p ok=%d\n",
				(void*)Peek_D3D_Texture(), mirrored ? 1 : 0);
			fflush(stderr);
		}
	}
	if (WW3D::Get_Texture_Reduction()) {
		Peek_D3D_Texture()->SetLOD(WW3D::Get_Texture_Reduction());
	}
	return(surface_desc.Height);
}

//=============================================================================
// TerrainTextureClass::updateNM
//=============================================================================
// GeneralsX @feature PS3.0 L2.1 TNM 17/10/2026: pack the parallel tangent-normal atlas.
//
// Layout is not recomputed here: the tile rectangles live on the diffuse tiles
// (m_sourceTiles[i]->m_tileLocationInTexture, assigned by updateTileTexturePositions), and this
// walks the very same loop reading m_nmTiles pixels instead. Any tile without a normal sheet stays
// at the pre-filled flat value (128,128,255 = tangent-space +Z), so a map with no *_nm.tga assets
// packs an all-flat atlas whose shader modulation is exactly 1.0 everywhere.
//
// Returns 0 (caller drops the atlas) when the chosen diffuse atlas format is the 16-bit fallback:
// packing signed 8-bit offsets into 5/6-bit channels would quantize the normal sign axes to steps
// of 8 and visibly band the ground.
int TerrainTextureClass::updateNM(WorldHeightMap *htMap)
{
	IDirect3DSurface9 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));
	if (surface_desc.Width < TEXTURE_WIDTH || surface_desc.Format != D3DFMT_A8R8G8B8) {
		surface_level->Release();
		return 0;
	}

	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));

	const Int tilePixelExtent = TILE_PIXEL_EXTENT;
	Int nmWritten = 0, nmFlat = 0;

	// Pre-fill the whole level flat (A=255 R=128 G=128 B=255), then overwrite real tiles.
	{
		Int *flatRow = (Int*)locked_rect.pBits;
		for (Int r = 0; r < (Int)surface_desc.Height; r++) {
			Int *pixel = flatRow + (r * surface_desc.Width);
			for (Int c = 0; c < (Int)surface_desc.Width; c++) {
				pixel[c] = (Int)0xFF8080FFu;
			}
		}
	}

	for (Int tileNdx = 0; tileNdx < htMap->m_numBitmapTiles; tileNdx++) {
		TileData *pBase = htMap->getSourceTile(tileNdx);
		if (!pBase) continue;
		ICoord2D position = pBase->m_tileLocationInTexture;
		if (position.x <= 0) continue;	// all real tile offsets start at 2, same rule as update().

		TileData *pNM = htMap->getNMTile(tileNdx);
		if (!pNM) { nmFlat++; continue; }	// stays at the pre-filled flat normal.
		nmWritten++;

		for (Int j = 0; j < tilePixelExtent; j++) {
			UnsignedByte *pBGR = pNM->getRGBDataForWidth(tilePixelExtent);
			pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match the diffuse packing.
			Int row = position.y+j;
			UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) + (row)*surface_desc.Width*4;
			pBGRX += position.x*4;
			for (Int i = 0; i < tilePixelExtent; i++) {
				*((Int*)pBGRX) = (Int)(0xFF000000u + ((Int)pBGR[2]<<16) + ((Int)pBGR[1]<<8) + pBGR[0]);
				pBGRX += 4;
				pBGR += TILE_BYTES_PER_PIXEL;
			}
		}
	}

	// Same 4-pixel duplicated border per texture-class block as update(), so bilinear sampling at
	// tile edges blends normals with the sheet's own wrap instead of atlas background.
	Int texClass;
	for (texClass=0; texClass<htMap->m_numTextureClasses; texClass++) {
		Int width = htMap->m_textureClasses[texClass].width;
		ICoord2D origin = htMap->m_textureClasses[texClass].positionInTexture;
		if (origin.x<=0) continue;
		width *= TILE_PIXEL_EXTENT;
		Int j;
		for (j=0; j<width; j++) {
			Int row = origin.y+j;
			UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) + (row)*surface_desc.Width*4;
			Int column = origin.x;
			pBGRX += column*4;
			memcpy(pBGRX-(4)*4, pBGRX+(width-4)*4, 4*4);
			memcpy(pBGRX+(width*4), pBGRX, 4*4);
		}
		for (j=0; j<4; j++) {
			Int row = origin.y-j-1;
			UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) + (row)*surface_desc.Width*4;
			UnsignedByte *target = pBGRX+(origin.x-4)*4;
			memcpy(target, target+width*surface_desc.Width*4, (width+8)*4);
			row = origin.y+j;
			pBGRX = ((UnsignedByte*)locked_rect.pBits) + (row)*surface_desc.Width*4;
			target = pBGRX+(origin.x-4)*4;
			memcpy(target+width*surface_desc.Width*4, target, (width+8)*4);
		}
	}

	surface_level->UnlockRect();
	surface_level->Release();

	// Mips the same way the diffuse atlas does; an all-flat atlas mips to all-flat.
	DX8_ErrorCode(D3DXFilterTexture(Peek_D3D_Texture(), nullptr, 0, D3DX_FILTER_TRIANGLE));
	// GeneralsX @bugfix Qoder 19/10/2026 BUG-03 identity probe (same contract as the diffuse atlas).
	{
		static bool s_onceNM = false;
		const bool mirrored = VKTex_Mirror_D3D_Texture(Peek_D3D_Texture());
		if (!s_onceNM) { s_onceNM = true;
			fprintf(stderr, "[TNM] nm atlas mirror: d3d=%p ok=%d\n",
				(void*)Peek_D3D_Texture(), mirrored ? 1 : 0);
			fflush(stderr);
		}
	}
	if (WW3D::Get_Texture_Reduction()) {
		Peek_D3D_Texture()->SetLOD(WW3D::Get_Texture_Reduction());
	}

	fprintf(stderr, "[TNM] atlas packed: nm_tiles=%d flat_tiles=%d size=%dx%d\n",
		nmWritten, nmFlat, surface_desc.Width, surface_desc.Height);
	fflush(stderr);
	return(surface_desc.Height);
}

#if 0 // old version.
//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
int TerrainTextureClass::update(WorldHeightMap *htMap)
{
	// D3DTexture is our texture;

	IDirect3DSurface9 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(D3DTexture->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));
	if (surface_desc.Width < TEXTURE_WIDTH) {
		surface_level->Release();
		if (surface_desc.Width == 256) {
			return update256(htMap);
		}
		return false;
	}

	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));

	Int tilePixelExtent = TILE_PIXEL_EXTENT;
	Int tilesPerRow = surface_desc.Width/(2*TILE_PIXEL_EXTENT+TILE_OFFSET);
	tilesPerRow *= 2;
	Int numRows = surface_desc.Height/(tilePixelExtent+TILE_OFFSET);
#ifdef RTS_DEBUG
	assert(tilesPerRow*numRows >= htMap->m_numBitmapTiles);
	assert((Int)surface_desc.Width >= tilePixelExtent*tilesPerRow);
#endif
	if (surface_desc.Format == D3DFMT_A1R5G5B5) {
		Int cellX, cellY;
#if 0
		for (cellX = 0; cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(cellY*surface_desc.Width+cellX)*2;
				*((Short*)pBGR) = (((255-2*cellY)>>3)<<10) + ((4*cellX)>>4);
			}
		}
#endif
		Int pixelBytes = 2;
		for (cellY = 0; cellY < numRows; cellY++) {
			for (cellX = 0; cellX < tilesPerRow; cellX++) {
				Int tileNdx = cellX/2 + (tilesPerRow/2)*(cellY/2);
				tileNdx *=4;
				if (cellX&1) tileNdx++;
				if (!(cellY&1)) tileNdx += 2;
#define ADD_EXTRA_TILES 1
#if ADD_EXTRA_TILES // Fills in an extra 2 columns and 1 row of tiles if there is room.
				if (!htMap->getSourceTile(tileNdx) && htMap->getSourceTile(tileNdx-4)) {
					tileNdx -= 4;
				}
				if (!htMap->getSourceTile(tileNdx) && htMap->getSourceTile(tileNdx-8)) {
					tileNdx -= 8;
				}
				if (!htMap->getSourceTile(tileNdx) && htMap->getSourceTile(tileNdx-2*tilesPerRow)) {
					tileNdx -= 2*tilesPerRow;
				}
#endif
				if (htMap->getSourceTile(tileNdx)) {
					Int i,j;
					for (j=0; j<tilePixelExtent; j++) {
						UnsignedByte *pBGR = htMap->getSourceTile(tileNdx)->getRGBDataForWidth(tilePixelExtent);
						pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
						Int row = cellY*tilePixelExtent+j;
						row += TILE_OFFSET/2;
						row += TILE_OFFSET*(cellY/2);
						UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
									(row)*surface_desc.Width*pixelBytes;

						Int column = cellX*tilePixelExtent;
						column += TILE_OFFSET*(cellX/2);
						pBGRX += column*pixelBytes;
						pBGRX += (TILE_OFFSET/2)*pixelBytes;
						for (i=0; i<tilePixelExtent; i++) {
							*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
							pBGRX +=pixelBytes;
							pBGR +=TILE_BYTES_PER_PIXEL;
						}
					}

				}
			}

		}


		for (cellY = 0; cellY < numRows; cellY++) {
			for (cellX = 0; cellX < tilesPerRow; cellX++) {
			// Duplicate 4 rows of pixels before and after.
				Int j;
				for (j=0; j<tilePixelExtent; j++) {
					Int row = cellY*tilePixelExtent+j;
					row += TILE_OFFSET/2;
					row += TILE_OFFSET*(cellY/2);
					UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
								(row)*surface_desc.Width*pixelBytes;

					Int column = cellX*tilePixelExtent;
					column += TILE_OFFSET*(cellX/2);
					pBGRX += column*pixelBytes;
					if (cellX&1) {
						Int bytesToCopy = 4*pixelBytes;
						if (cellX == tilesPerRow-1) {
							// last cell, so fill to the end of the texture.
							Int usedWidth = tilesPerRow*tilePixelExtent + TILE_OFFSET*(tilesPerRow/2);
							usedWidth *= pixelBytes;
							bytesToCopy += surface_desc.Width*pixelBytes - usedWidth;
						}
						// Copy after.
						pBGRX -= tilePixelExtent*pixelBytes;
						pBGRX += (TILE_OFFSET/2)*pixelBytes;
						memcpy(pBGRX+(2*tilePixelExtent*pixelBytes), pBGRX, bytesToCopy);
					} else {
						// copy before
						memcpy(pBGRX, pBGRX+(2*tilePixelExtent)*pixelBytes, 4*pixelBytes);
					}
				}
			}
		}
 		for (cellY = 0; cellY < numRows; cellY++) {
			Int rowBytes = surface_desc.Width*pixelBytes;
			Int row = cellY*tilePixelExtent;
			row += TILE_OFFSET/2;
			row += TILE_OFFSET*(cellY/2);
			if ( (cellY&1) == 0) {
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*rowBytes;
				row += 2*tilePixelExtent;
				UnsignedByte *pBase = ((UnsignedByte*)locked_rect.pBits) +
							(row)*rowBytes;
				memcpy(pBGRX-4*rowBytes, pBase-4*rowBytes, 4*rowBytes);
			} else {
				UnsignedByte *pBase = ((UnsignedByte*)locked_rect.pBits) +
							(row-tilePixelExtent)*rowBytes;
				row += tilePixelExtent;
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*rowBytes;
				memcpy(pBGRX, pBase, 4*rowBytes);
			}
		}

	}

	// GeneralsX @bugfix Qoder 21/10/2026: the atlas is mostly unwritten background. A 204-tile
	// 8192x4096 atlas is ~60% transparent black, so every mip level below the first averages that
	// black into the ground colour. Terrain cells whose UV jumps at a base-tile boundary make the
	// sampler dive to those levels (Alpine Assault changes tile every 2x2 cells), which drew a black
	// grid line over grass. Filling the unwritten texels with the mean of the written ones keeps the
	// deep mips terrain-coloured instead of black. Only fully transparent black is replaced, so the
	// actual tile content is untouched.
	if (surface_desc.Format == D3DFMT_A8R8G8B8 || surface_desc.Format == D3DFMT_X8R8G8B8) {
		UnsignedByte *bits = (UnsignedByte*)locked_rect.pBits;
		unsigned long long sr = 0, sg = 0, sb = 0, n = 0;
		for (UnsignedInt fy = 0; fy < surface_desc.Height; ++fy) {
			const UnsignedByte *p = bits + (size_t)fy * locked_rect.Pitch;
			for (UnsignedInt fx = 0; fx < surface_desc.Width; ++fx, p += 4) {
				if ((p[0] | p[1] | p[2] | p[3]) == 0) continue;
				sb += p[0]; sg += p[1]; sr += p[2]; ++n;
			}
		}
		if (n > 0) {
			const UnsignedByte fr = (UnsignedByte)(sr / n);
			const UnsignedByte fg = (UnsignedByte)(sg / n);
			const UnsignedByte fb = (UnsignedByte)(sb / n);
			unsigned long long filled = 0;
			for (UnsignedInt fy = 0; fy < surface_desc.Height; ++fy) {
				UnsignedByte *p = bits + (size_t)fy * locked_rect.Pitch;
				for (UnsignedInt fx = 0; fx < surface_desc.Width; ++fx, p += 4) {
					if ((p[0] | p[1] | p[2] | p[3]) != 0) continue;
					p[0] = fb; p[1] = fg; p[2] = fr; p[3] = 255; ++filled;
				}
			}
			fprintf(stderr, "[TERRAIN] atlas background fill: rgb=(%u,%u,%u) texels=%llu of %ux%u\n",
				(unsigned)fr, (unsigned)fg, (unsigned)fb, filled,
				(unsigned)surface_desc.Width, (unsigned)surface_desc.Height);
			fflush(stderr);
		}
	}

	surface_level->UnlockRect();
	surface_level->Release();
	DX8_ErrorCode(D3DXFilterTexture(D3DTexture, nullptr, 0, D3DX_FILTER_BOX));
	VKTex_Mirror_D3D_Texture(Peek_D3D_Texture());
	return(surface_desc.Height);
}
#endif

//=============================================================================
// TerrainTextureClass::setLOD
//=============================================================================
/** Sets the lod of the texture to be loaded into the video card.  */
//=============================================================================
void TerrainTextureClass::setLOD(Int LOD)
{
	if (Peek_D3D_Texture()) Peek_D3D_Texture()->SetLOD(LOD);
}
//=============================================================================
// TerrainTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
Bool TerrainTextureClass::updateFlat(WorldHeightMap *htMap, Int xCell, Int yCell, Int cellWidth, Int pixelsPerCell)
{
	// D3DTexture is our texture;

	IDirect3DSurface9 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));
	DEBUG_ASSERTCRASH((Int)surface_desc.Width == cellWidth*pixelsPerCell, ("Bitmap too small."));
	DEBUG_ASSERTCRASH((Int)surface_desc.Height == cellWidth*pixelsPerCell, ("Bitmap too small."));
	if (surface_desc.Width != cellWidth*pixelsPerCell) {
		return false;
	}

	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));


	Int pixelBytes = 0;
	if (surface_desc.Format == D3DFMT_A1R5G5B5) {
		pixelBytes = 2;
		Int cellX, cellY;
#if 0
		UnsignedInt X, Y;
		for (X = 0; X < surface_desc.Width; X++) {
			for (Y = 0; Y < surface_desc.Height; Y++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(Y*surface_desc.Width+X)*pixelBytes;
				*((Short*)pBGR) = (((255-2*Y)>>3)<<10) + ((2*X)>>4);
			}
		}
#endif
		for (cellX = 0; cellX < cellWidth; cellX++) {
			for (cellY = 0; cellY < cellWidth; cellY++) {
				UnsignedByte *pBGRX_data = ((UnsignedByte*)locked_rect.pBits);
				UnsignedByte *pBGR = htMap->getPointerToTileData(xCell+cellX, yCell+cellY, pixelsPerCell);
				if (pBGR == nullptr) continue; // past end of defined terrain. [3/24/2003]
				Int k, l;
				for (k=pixelsPerCell-1; k>=0; k--) {
					UnsignedByte *pBGRX = pBGRX_data + (pixelsPerCell*(cellWidth-cellY-1)+k)*surface_desc.Width*pixelBytes +
						cellX*pixelsPerCell*pixelBytes;
					for (l=0; l<pixelsPerCell; l++) {
						*((Short*)pBGRX) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
						pBGRX +=pixelBytes;
						pBGR +=TILE_BYTES_PER_PIXEL;
					}
				}
			}
		}
	} else if (surface_desc.Format == D3DFMT_A8R8G8B8) {
		// GeneralsX @performance Qoder 10/07/2026 32-bit upload path for the
		// new atlas format: BGRA tile data is packed verbatim.
		pixelBytes = 4;
		Int cellX, cellY;
		for (cellX = 0; cellX < cellWidth; cellX++) {
			for (cellY = 0; cellY < cellWidth; cellY++) {
				UnsignedByte *pBGRX_data = ((UnsignedByte*)locked_rect.pBits);
				UnsignedByte *pBGR = htMap->getPointerToTileData(xCell+cellX, yCell+cellY, pixelsPerCell);
				if (pBGR == nullptr) continue; // past end of defined terrain. [3/24/2003]
				Int k, l;
				for (k=pixelsPerCell-1; k>=0; k--) {
					UnsignedByte *pBGRX = pBGRX_data + (pixelsPerCell*(cellWidth-cellY-1)+k)*surface_desc.Width*pixelBytes +
						cellX*pixelsPerCell*pixelBytes;
					for (l=0; l<pixelsPerCell; l++) {
						*((Int*)pBGRX) = (Int)(0xFF000000u + ((Int)pBGR[2]<<16) + ((Int)pBGR[1]<<8) + pBGR[0]);
						pBGRX +=pixelBytes;
						pBGR +=TILE_BYTES_PER_PIXEL;
					}
				}
			}
		}
	}

	// GeneralsX @bugfix Qoder 10/07/2026 Runtime diagnostic for the flat
	// texture upload path.
	fprintf(stderr, "[TERRAIN] updateFlat: fmt=0x%08X w=%d h=%d pixelBytes=%d cellWidth=%d reduction=%d\n",
		(unsigned)surface_desc.Format, surface_desc.Width, surface_desc.Height,
		pixelBytes, cellWidth, WW3D::Get_Texture_Reduction());
	fflush(stderr);

	surface_level->UnlockRect();
	surface_level->Release();
	// GeneralsX @performance Qoder 10/07/2026 Triangle instead of box filtering:
	// box-averaging the mips destroys high-frequency tile detail; triangle
	// weights keep more contrast in the lower mip levels.
	HRESULT filter_hr = D3DXFilterTexture(Peek_D3D_Texture(), nullptr, 0, D3DX_FILTER_TRIANGLE);
	DX8_ErrorCode(filter_hr);
	fprintf(stderr, "[TERRAIN] D3DXFilterTexture(TRIANGLE) flat hr=0x%08X\n", (unsigned)filter_hr);
	fflush(stderr);
	VKTex_Mirror_D3D_Texture(Peek_D3D_Texture());
	return(surface_desc.Height);
}

//=============================================================================
// TerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup
(standard D3D setup, but beyond the scope of W3D).  */
//=============================================================================
void TerrainTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]
	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
	}
	// Now setup the texture pipeline.
	if (stage==0) {
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);

	}
#endif
}

/******************************************************************************
						AlphaTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// AlphaTerrainTextureClass::AlphaTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to creat a throw away 8x8 texture,
then uses the base texture's D3D texture. This way the base tiles pass, drawn
using TerrainTextureClass shares the same texture with the blended edges pass,
saving lots of texture memory, and preventing seams between blended tiles. */
//=============================================================================
AlphaTerrainTextureClass::AlphaTerrainTextureClass( TextureClass *pBaseTex ):
	TextureClass(8, 8,
		WW3D_FORMAT_A1R5G5B5, MIP_LEVELS_1 )
{
	// Attach the base texture's d3d texture.
	IDirect3DTexture9 * d3d_tex = pBaseTex->Peek_D3D_Texture();
	Set_D3D_Base_Texture(d3d_tex);
}


//=============================================================================
// AlphaTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
This may be applied in either single pass, as the second texture in the pipe,
or multipass.  If stage==0, we are doing multipass and we set up the pipe
for a single texture.  If stage==1, then we are doing a single pass, and we
set up the pipe so that we blend onto the base texture in stage 0.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void AlphaTerrainTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);

	// GeneralsX @feature PS3.0-P3-5 14/10/2026 Alpha-overlay tiles (water edges, transitions) used to
	// take their sampler filters straight from the retail Bilinear/TrilinearTerrainTex INI bools, both
	// of which ship FALSE - so these tiles sampled with POINT min/mag/mip whatever the player had
	// picked in the options menu. The tier tables now drive the stage; the bools remain a floor.
	// GENERALSX_TERRAIN_FILTER=legacy reproduces the retail rule for A/B evidence legs.
	{
		const bool bilinearHint=TheGlobalData ? TheGlobalData->m_bilinearTerrainTex!=FALSE : false;
		const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
		TextureFilterClass::Apply_Player_Filter(stage,bilinearHint,trilinearHint);
	}
	// Since we are using multiple distinct tiles, the textures doesn't wrap, so clamp it.
	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.
	if (stage==0) {
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
	}	else if (stage==1) {

		if (TheGlobalData && !TheGlobalData->m_multiPassTerrain)
		{
			///@todo: Remove 8-Stage Nvidia hack after drivers are fixed.
			//This method is a backdoor specific to Nvidia based cards.  It will fail on
			//other hardware.  Allows single pass blend of 2 textures and post modulate diffuse.
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
  			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
			DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
		}
	}
}


/******************************************************************************
						LightMapTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// LightMapTerrainTextureClass::LightMapTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture. */
//=============================================================================
LightMapTerrainTextureClass::LightMapTerrainTextureClass(AsciiString name, MipCountType mipLevelCount) :
TextureClass(name.isEmpty()?"TSNoiseUrb.tga":name.str(),name.isEmpty()?"TSNoiseUrb.tga":name.str(), mipLevelCount )
{
	// GeneralsX @bugfix Qoder 19/10/2026 BUG-03 identity probe: pair this ctor with the mirror log to
	// tell which stub texture key the battle lightmap lives under.
	fprintf(stderr, "[TERRAIN] lightmap ctor: '%s' d3d=%p\n", Get_Texture_Name().str(), (void*)Peek_D3D_Texture());
	fflush(stderr);
	Get_Filter().Set_Min_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	Get_Filter().Set_Mag_Filter(TextureFilterClass::FILTER_TYPE_BEST);
	Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
	Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_REPEAT);
}

#define STRETCH_FACTOR ((float)(1/(63.0*MAP_XY_FACTOR/2))) /* covers 63/2 tiles */

//=============================================================================
// LightMapTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The LightMapTerrainTextureClass may be applied by itself, or with the
CloudMapTerrainTextureClass.  This may be applied in either single pass,
as the second texture in the pipe,
or multipass.  If stage==0, we are doing multipass and we set up the pipe
for a single texture.  If stage==1, then we are doing a single pass, and we
set up the pipe so that we blend onto the cloud map texture in stage 0.
Also, texture is mapped using the x/y coordinates of the map, saving us
yet another set of uv coordinates.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void LightMapTerrainTextureClass::Apply(unsigned int stage)
{
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]
	// Do the base apply.
	/* previous setup */
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
	DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

	// Disable 3rd stage just in case.
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	// Now setup the texture pipeline.
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLORARG2, D3DTA_CURRENT );
	if (stage == 0) {
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
		//Disable second stage
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	} else {
		DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	}
	DX8Wrapper::Set_DX8_Texture_Stage_State( stage, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	// Two output coordinates are used.
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);


	DX8Wrapper::Set_DX8_Sampler_State( stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Sampler_State( stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

	D3DXMATRIX curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

	D3DXMATRIX inv;
	float det;
	D3DXMatrixInverse(&inv, &det, &curView);

	D3DXMATRIX scale;
	D3DXMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	inv *=scale;
	if (stage==0) {
		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, inv);
	}	if (stage==1) {
		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, inv);
	}


	if (stage==0) {
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);
	}
#endif
}









/******************************************************************************
						AlphaEdgeTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

/**
* AlphaEdgeTextureClass - Generates the alpha edge blending for terrain.
*
*/
AlphaEdgeTextureClass::AlphaEdgeTextureClass( int height, MipCountType mipLevelCount) :
//	TextureClass("EdgingTemplate.tga","EdgingTemplate.tga", mipLevelCount )
	TextureClass(TEXTURE_WIDTH, height, WW3D_FORMAT_A8R8G8B8, mipLevelCount )
{

}

int AlphaEdgeTextureClass::update256(WorldHeightMap *htMap)
{
	return 1;
}

int AlphaEdgeTextureClass::update(WorldHeightMap *htMap)
{
	// D3DTexture is our texture;

	IDirect3DSurface9 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));

	Int tilePixelExtent = TILE_PIXEL_EXTENT; // blend tiles are 1/4 tiles.
//	Int tilesPerRow = surface_desc.Width / (tilePixelExtent+8);

//	Int numRows = surface_desc.Height/(tilePixelExtent+8);

	if (surface_desc.Format == D3DFMT_A8R8G8B8) {
#if 1
#if 1
		Int cellX, cellY;
		for (cellX = 0; (UnsignedInt)cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(cellY*surface_desc.Width+cellX)*4;
				pBGR[2] = 255-cellY/2;
				pBGR[0] = cellX/2;
				pBGR[3] = cellX/2;  // alpha.
				pBGR[3] = 128;  // alpha.
			}
		}
#endif
#if 1
		Int tileNdx;
		Int pixelBytes = 4;
		for (tileNdx=0; tileNdx < htMap->m_numEdgeTiles; tileNdx++) {
			TileData *pTile = htMap->getEdgeTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<=0) continue; // all real edge offsets start at 4.  jba.
			Int i,j;
			Int column = position.x;
			for (j=0; j<tilePixelExtent; j++) {
				Int row = position.y+j;
				UnsignedByte *pBGR = htMap->getEdgeTile(tileNdx)->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-1-j)*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				UnsignedByte *pBGRX = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;
				pBGRX += column*pixelBytes;

				for (i=0; i<tilePixelExtent; i++) {
					pBGRX[0] = pBGR[0];  //r
					pBGRX[1] = pBGR[1];	//g
					pBGRX[2] = pBGR[2];	//b
					if (pBGR[0]==0 && pBGR[1]==0 && pBGR[2]==0) {
						pBGRX[3] = 0x80;
					} else if (pBGR[0]==0xff && pBGR[1]==0xff && pBGR[2]==0xff) {
						pBGRX[3] = 0x00;
					}	else {
						pBGRX[3] = 0xff;
					}

					pBGRX += pixelBytes;
					pBGR += TILE_BYTES_PER_PIXEL;
				}
			}
		}
#endif
#endif
	}
	surface_level->UnlockRect();
	surface_level->Release();
	// GeneralsX @performance Qoder 10/07/2026 Triangle instead of box filtering:
	// box-averaging the mips destroys high-frequency tile detail; triangle
	// weights keep more contrast in the lower mip levels.
	DX8_ErrorCode(D3DXFilterTexture(Peek_D3D_Texture(), nullptr, 0, D3DX_FILTER_TRIANGLE));
	VKTex_Mirror_D3D_Texture(Peek_D3D_Texture());
	return(surface_desc.Height);
}

void AlphaEdgeTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
#if 0 // obsolete [4/1/2003]

	if (TheGlobalData && (TheGlobalData->m_bilinearTerrainTex || TheGlobalData->m_trilinearTerrainTex)) {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
	}
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.
	if (stage==0) {

		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1,   D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 1 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	} else if (stage==1) {
		// Drawing texture through the mask.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1,   D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1,   D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG2,   D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG2 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 1 );
		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_ONE);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);

	}
#endif
}


/******************************************************************************
						CloudMapTerrainTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// CloudMapTerrainTextureClass::CloudMapTerrainTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture, and sets
up the "sliding" parameters for the clouds to slide over the terrain. */
//=============================================================================
//@todo - Allow adjustment of the cloud slide rate, and lose the hard coded "cloudmap.tga"
CloudMapTerrainTextureClass::CloudMapTerrainTextureClass(MipCountType mipLevelCount) :
	TextureClass("TSCloudMed.tga","TSCloudMed.tga", mipLevelCount )
{
	Get_Filter().Set_Mip_Mapping( TextureFilterClass::FILTER_TYPE_FAST );
	m_xSlidePerSecond = -0.02f;
	m_ySlidePerSecond =  1.50f * m_xSlidePerSecond;
	m_curTick = 0;
	m_xOffset = 0;
	m_yOffset = 0;

}

//=============================================================================
// CloudMapTerrainTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The CloudMapTerrainTextureClass may be applied by itself, or with the
LightMapTerrainTexture.  This may be applied in either single pass,
as the first texture in the pipe with LightMapTerrainTextureClass as the
second stage of the pape, or multipass.  We setup for stage 0, assuming that
we are the only texture, as LightMapTerrainTexture will adjust for multitexture
if it is applied to stage 1.
Also, texture is mapped using the x/y coordinates of the map, saving us
yet another set of uv coordinates.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void CloudMapTerrainTextureClass::Apply(unsigned int stage)
{


	// Do the base apply.
	TextureClass::Apply(stage);
#if 0   // obsolete
	/* previous setup */
	if (TheGlobalData && TheGlobalData->m_trilinearTerrainTex) {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
	} else {
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
	}

	DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

	// Now setup the texture pipeline.
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
	// Two output coordinates are used.
	DX8Wrapper::Set_DX8_Texture_Stage_State(stage,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);


	DX8Wrapper::Set_DX8_Sampler_State( stage,  D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Sampler_State( stage,  D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

	D3DXMATRIX curView;
	DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, curView);

	D3DXMATRIX inv;
	float det;
	D3DXMatrixInverse(&inv, &det, &curView);

	D3DXMATRIX scale;
	D3DXMatrixScaling(&scale, STRETCH_FACTOR, STRETCH_FACTOR,1);
	inv *=scale;
	D3DXMATRIX offset;

	Int delta = m_curTick;
	m_curTick = ::GetTickCount();
	delta = m_curTick-delta;
	m_xOffset += m_xSlidePerSecond*delta/1000;
	m_yOffset += m_ySlidePerSecond*delta/1000;

	if (m_xOffset > 1) m_xOffset -= 1;
	if (m_yOffset > 1) m_yOffset -= 1;
	if (m_xOffset < -1) m_xOffset += 1;
	if (m_yOffset < -1) m_yOffset += 1;


	D3DXMatrixTranslation(&offset, m_xOffset, m_yOffset,0);

	inv *= offset;

	if (stage==0) {
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1 );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE0, inv);

		// Disable 3rd stage just in case.
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 2, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

		DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR);
		DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_ZERO);
	}	else if (stage==1) {
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAARG1, D3DTA_CURRENT );
		DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );

		DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE1, inv);
	}
#endif
}

//=============================================================================
// CloudMapTerrainTextureClass::restore
//=============================================================================
/** Cleans up any custom settings to the texturing pipeline that may not be
understood by w3d. */
//=============================================================================
void CloudMapTerrainTextureClass::restore()
{
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );

	DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Sampler_State( 1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,false);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);


	if (TheGlobalData && !TheGlobalData->m_multiPassTerrain)
	{
		///@todo: Remove 8-Stage Nvidia hack after drivers are fixed.
		//This method is a backdoor specific to Nvidia based cards.  It will fail on
		//other hardware.  Allows single pass blend of 2 textures and post modulate diffuse.
		Int i;
		for (i=0; i<8; i++) {
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLOROP, D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_TEXCOORDINDEX, i);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLORARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAOP,   D3DTOP_DISABLE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
			DX8Wrapper::Set_DX8_Texture_Stage_State( i, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

			DX8Wrapper::Set_DX8_Texture(i, nullptr);
		}
	}
}

/******************************************************************************
						ScorchTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// ScorchTextureClass::ScorchTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to load the .tga texture. */
//=============================================================================
// GeneralsX @feature PS3.0-P3-31 14/10/2026: env-overridable scorch atlas name (see ctor note).
static const char* Scorch_Texture_Name()
{
	static const char* cached = nullptr;
	if (cached == nullptr) {
		cached = "EXScorch01.tga";
		const char* ev = getenv("GENERALSX_SCORCH_TEX");
		if (ev != nullptr && *ev != 0) cached = ev;
	}
	return cached;
}
// GeneralsX @feature PS3.0-P3-31 14/10/2026 G6: the retail @todo asked for a non-hardcoded texture
// path. GENERALSX_SCORCH_TEX supplies an alternate atlas texture name for QA/themed mods; the
// default stays EXScorch01.tga so the shipped look is bit-for-bit.
ScorchTextureClass::ScorchTextureClass(MipCountType mipLevelCount) :
	TextureClass(Scorch_Texture_Name(),Scorch_Texture_Name(), mipLevelCount )
{
}

//=============================================================================
// ScorchTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup.
The ScorchTextureClass is applied by iteself, as it's mesh is a subset of the
terrain mesh.
(standard D3D setup, but beyond the scope of W3D). */
//=============================================================================
void ScorchTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
	// GeneralsX @feature PS3.0-P3-5 14/10/2026 Scorch decals took their filters from the retail
	// Bilinear/TrilinearTerrainTex INI bools (both ship FALSE), so craters were nearest mip-pinned
	// and point-sampled regardless of the player's texture-filtering option. Same tier as everything
	// else now; the bools act as a floor and GENERALSX_TERRAIN_FILTER=legacy restores retail.
	{
		const bool bilinearHint=TheGlobalData ? TheGlobalData->m_bilinearTerrainTex!=FALSE : false;
		const bool trilinearHint=TheGlobalData ? TheGlobalData->m_trilinearTerrainTex!=FALSE : false;
		TextureFilterClass::Apply_Player_Filter(stage,bilinearHint,trilinearHint);
	}

	DX8Wrapper::Set_DX8_Texture_Stage_State(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	DX8Wrapper::Set_DX8_Sampler_State( 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	// Now setup the texture pipeline.

	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_COLOROP,   D3DTOP_MODULATE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1 );
	// GeneralsX @bugfix Qoder 23/10/2026: SELECTARG1 reads ALPHAARG1, but this override never wrote it.
	// The terrain pass that runs immediately before the scorch batch leaves stage 0 at D3DTA_CURRENT
	// (alpha 1.0), so every crater sampled its own mask yet blended with a constant opaque alpha and
	// stamped a hard dark square on the terrain. Bind the alpha source to the atlas, the same contract
	// flushDecals uses for its decals.
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 0, D3DTSS_TEXCOORDINDEX, 0 );
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE,true);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);

	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_COLOROP,   D3DTOP_DISABLE );
	DX8Wrapper::Set_DX8_Texture_Stage_State( 1, D3DTSS_ALPHAOP,   D3DTOP_DISABLE );
}


