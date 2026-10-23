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

// FILE: W3DTreeBuffer.cpp ////////////////////////////////////////////////
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
// File name: W3DTreeBuffer.cpp
//
// Created:   John Ahlquist, May 2001
//
// Desc:      Draw buffer to handle all the trees in a scene.
//
//-----------------------------------------------------------------------------

// ------------------------------------------------------------------------------------------------
/** Topple options */
// ------------------------------------------------------------------------------------------------
enum
{
	W3D_TOPPLE_OPTIONS_NONE			 = 0x00000000,
	W3D_TOPPLE_OPTIONS_NO_BOUNCE = 0x00000001,  ///< do not bounce when hit the ground
	W3D_TOPPLE_OPTIONS_NO_FX		 = 0x00000002	///< do not play any FX when hit the ground
};
//-----------------------------------------------------------------------------
//         Includes
//-----------------------------------------------------------------------------

#include "W3DDevice/GameClient/W3DTreeBuffer.h"
#include "W3DDevice/GameClient/W3DDisplay.h"	// GeneralsX @feature Qoder 22/10/2026 FPNIGHT: render-side TOD
#include "W3DDevice/GameClient/W3DTreeAlias.h"

#include <cstdlib>   // GeneralsX @feature PS3.0 batch8 G1 14/10/2026: getenv for GENERALSX_TREE30 opt-out
#include <cstring>   // GeneralsX @feature PS3.0 batch8 G1 14/10/2026: strcmp for the opt-out compare
#include <WW3D2/assetmgr.h>
#include <WW3D2/texture.h>
#include "WW3D2/Backend/VulkanPgm.h"	// GeneralsX @feature Qoder 22/10/2026: enable the translated tree PS on pure Vulkan
#include "Common/FramePacer.h"
#include "Common/GameUtility.h"
#include "Common/MapReaderWriterInfo.h"
#include "Common/FileSystem.h"
#include "Common/file.h"
#include "Common/PerfTimer.h"
#include "Common/Player.h"
#include "Common/PlayerList.h"
#include "GameLogic/ScriptEngine.h"
#include "GameLogic/GameLogic.h"
#include "GameLogic/Object.h"
#include "GameLogic/PartitionManager.h"
#include "GameClient/ClientRandomValue.h"
#include "GameClient/FXList.h"
#include "W3DDevice/GameClient/TerrainTex.h"
#include "W3DDevice/GameClient/HeightMap.h"
#include "W3DDevice/GameClient/W3DDynamicLight.h"
#include "W3DDevice/GameClient/Module/W3DTreeDraw.h"
#include "W3DDevice/GameClient/W3DShaderManager.h"
#include "W3DDevice/GameClient/W3DShadow.h"
#include "W3DDevice/GameClient/W3DShroud.h"
#include "W3DDevice/GameClient/W3DProjectedShadow.h"
#include "WW3D2/camera.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/dx8renderer.h"
#include "WW3D2/matinfo.h"
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.3: converge the procedural tree atlas into the
// Vulkan texture mirror (unconditional: the symbol is a no-op unless the win64vk Vulkan core is up).
#include "WW3D2/Backend/VulkanTexMirror.h"
#include "WW3D2/Backend/VulkanGeo.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "d3dx9tex.h"


// If TEST_AND_BLEND is defined, it will do an alpha test and blend.  Otherwise just alpha test. jba. [5/30/2003]
#define dontTEST_AND_BLEND 1

// GeneralsX @bugfix Qoder 21/10/2026: T10 raised MAX_BUFFERS to 8. The DEFAULT-pool
// static buffers go through the wrapper's shadow-lock path, and under the pure-Vulkan
// backend the lock for the later buffers returned a non-writable mapping; the index fill
// then wrote through a bad pointer (AV at *curIb++ = ... in loadTreesInVertexAndIndexBuffers).
// Use the dynamic path the rest of the geometry already uses.
//#define USE_STATIC 1

// GeneralsX @bugfix Qoder 22/10/2026 TREEATLAS: every 256px tile gets a replicated gutter of 16
// texels. Tree textures are cutout masks packed edge to edge, so any bilinear or mip tap outside a
// tile used to read the neighbouring tree: at runtime that shows up as small torn/stretched leaf
// fragments that flicker as the sampled mip changes with the camera. Replicating the tile's own edge
// pixels into the gutter keeps those taps inside the same tree.
#define TREE_ATLAS_GUTTER 16
#define TREE_ATLAS_SLOT (TILE_PIXEL_EXTENT + 2 * TREE_ATLAS_GUTTER)

// GeneralsX @bugfix Qoder 22/10/2026 TREEATLAS: the atlas keeps only 5 mip levels (down to 16px per
// tile). Beyond that a mip texel covers a whole tile, its box-averaged coverage (a cutout tree is
// 38-50% alpha) crosses the 0x60 alpha-test reference for every texel, and each distant card becomes
// an opaque square. The chain is generated on the CPU (D3DX filtering is unreliable under the Vulkan
// D3D9 stub) with box-averaged, alpha-weighted colour and alpha.
#define TREE_ATLAS_MIP_LEVELS MIP_LEVELS_5

#define END_OF_PARTITION (-1)

#define DELETED_TREE_TYPE (-2)



/******************************************************************************
						W3DTreeTextureClass
******************************************************************************/
//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// W3DTreeBuffer::W3DTreeTextureClass::W3DTreeTextureClass
//=============================================================================
/** Constructor. Calls parent constructor to create a 16 bit per pixel D3D
texture of the desired height and mip level. */
//=============================================================================
W3DTreeBuffer::W3DTreeTextureClass::W3DTreeTextureClass(unsigned width, unsigned height) :
	TextureClass(width, height,
		WW3D_FORMAT_A8R8G8B8, TREE_ATLAS_MIP_LEVELS )
{
}

// Copy the tile's edge pixels out into its gutter (corners clamp to the corner texel).
static void treeAtlasReplicateGutter(unsigned* base, int pitchPixels, int x0, int y0, int inner, int gutter)
{
	for (int dy = -gutter; dy < inner + gutter; ++dy) {
		int sy = dy;
		if (sy < 0) sy = 0;
		else if (sy >= inner) sy = inner - 1;
		unsigned* dstRow = base + (size_t)(y0 + dy) * pitchPixels + x0;
		const unsigned* srcRow = base + (size_t)(y0 + sy) * pitchPixels + x0;
		for (int dx = -gutter; dx < inner + gutter; ++dx) {
			if (dx >= 0 && dx < inner) continue;	// interior already holds the tile
			int sx = dx;
			if (sx < 0) sx = 0;
			else if (sx >= inner) sx = inner - 1;
			dstRow[dx] = srcRow[sx];
		}
	}
}

//=============================================================================
// W3DTreeBuffer::W3DTreeTextureClass::update
//=============================================================================
/** Sets the tile bitmap data into the texture.  The tiles are placed with 4
	pixel borders around them, so that when the tiles are scaled and bilinearly
	interpolated, you don't get seams between the tiles.  */
//=============================================================================
int W3DTreeBuffer::W3DTreeTextureClass::update(W3DTreeBuffer *buffer)
{

	//Set to clamp.
	Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
	Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);

	IDirect3DSurface9 *surface_level;
	D3DSURFACE_DESC surface_desc;
	D3DLOCKED_RECT locked_rect;
	DX8_ErrorCode(Peek_D3D_Texture()->GetSurfaceLevel(0, &surface_level));
	DX8_ErrorCode(surface_level->GetDesc(&surface_desc));

	DX8_ErrorCode(surface_level->LockRect(&locked_rect, nullptr, 0));

	Int tilePixelExtent = TILE_PIXEL_EXTENT;
//	Int numRows = surface_desc.Height/(tilePixelExtent+TILE_OFFSET);
#ifdef RTS_DEBUG
	//DASSERT_MSG(tilesPerRow*numRows >= htMap->m_numBitmapTiles,Debug::Format ("Too many tiles."));
	//DEBUG_ASSERTCRASH((Int)surface_desc.Width >= tilePixelExtent*tilesPerRow, ("Bitmap too small."));
#endif
	if (surface_desc.Format == D3DFMT_A8R8G8B8) {
		Int tileNdx;
		Int pixelBytes = 4;
#if 0 // Fill unused texture for debug display.
		UnsignedInt cellX, cellY;
		for (cellX = 0; cellX < surface_desc.Width; cellX++) {
			for (cellY = 0; cellY < surface_desc.Height; cellY++) {
				UnsignedByte *pBGR = ((UnsignedByte *)locked_rect.pBits)+(cellY*surface_desc.Width+cellX)*pixelBytes;
				//*((Short*)pBGR) =  0x8000 + (((255-2*cellY)>>3)<<10) + ((4*cellX)>>4);
				*((Int*)pBGR) =  0xFF000000 | ( (((255-cellY))<<16) + ((cellX)) );

			}
		}
#endif
		for (tileNdx=0; tileNdx < buffer->getNumTiles(); tileNdx++) {
			TileData *pTile = buffer->getSourceTile(tileNdx);
			if (!pTile) continue;
			ICoord2D position = pTile->m_tileLocationInTexture;
			if (position.x<0) {
				continue;
			}
			Int i,j;
			for (j=0; j<tilePixelExtent; j++) {
				UnsignedByte *pBGR = pTile->getRGBDataForWidth(tilePixelExtent);
				pBGR += (tilePixelExtent-(1+j))*TILE_BYTES_PER_PIXEL*tilePixelExtent; // invert to match.
				Int row = position.y+j;
				UnsignedByte *pBGRA = ((UnsignedByte*)locked_rect.pBits) +
							(row)*surface_desc.Width*pixelBytes;

				Int column = position.x;
				pBGRA += column*pixelBytes;
				for (i=0; i<tilePixelExtent; i++) {
					// 15 bit color *((Short*)pBGRA) = 0x8000 + ((pBGR[2]>>3)<<10) + ((pBGR[1]>>3)<<5) + (pBGR[0]>>3);
					*((Int *)pBGRA) = (pBGR[3]<<24) + (pBGR[2]<<16) + (pBGR[1]<<8) + (pBGR[0]);
					pBGRA +=pixelBytes;
					pBGR +=TILE_BYTES_PER_PIXEL;
				}
			}
			// GeneralsX @bugfix Qoder 22/10/2026 TREEATLAS: keep tiles from bleeding into each
			// other when sampled with bilinear/mip filtering (see TREE_ATLAS_GUTTER).
			treeAtlasReplicateGutter((unsigned*)locked_rect.pBits, (int)(locked_rect.Pitch / sizeof(unsigned)),
				position.x, position.y, tilePixelExtent, TREE_ATLAS_GUTTER);
		}

	}
	// GeneralsX @debug Qoder 20/10/2026 TREEATLAS: one bounded census of the atlas alpha, taken while
	// the level-0 lock still holds the bytes the game just filled. A tree card needs 0/255 alpha; an
	// all-127 atlas (or a 24-bit source promoted to 255) cannot be cut out by the FF alpha test.
	{
		static int s_atlasCensus = 0;
		if (s_atlasCensus < 2 && surface_desc.Format == D3DFMT_A8R8G8B8) {
			unsigned a0 = 0, a127 = 0, a255 = 0, amid = 0;
			unsigned amin = 255, amax = 0;
			unsigned rgbNonZero = 0;
			for (unsigned yy = 0; yy < surface_desc.Height; ++yy) {
				const unsigned* row = (const unsigned*)((const unsigned char*)locked_rect.pBits + (size_t)yy * locked_rect.Pitch);
				for (unsigned xx = 0; xx < surface_desc.Width; ++xx) {
					const unsigned v = row[xx];
					if (v & 0x00FFFFFFu) ++rgbNonZero;
					const unsigned a = (v >> 24) & 0xFFu;
					if (a == 0) ++a0; else if (a == 127) ++a127; else if (a == 255) ++a255; else ++amid;
					if (a < amin) amin = a;
					if (a > amax) amax = a;
				}
			}
			++s_atlasCensus;
			fprintf(stderr, "[TREEATLAS] atlas %ux%u rgb=%u a0=%u a127=%u amid=%u a255=%u amin=%u amax=%u\n",
				(unsigned)surface_desc.Width, (unsigned)surface_desc.Height, rgbNonZero, a0, a127, amid, a255, amin, amax);
			fflush(stderr);
		}
	}
	DX8_ErrorCode(surface_level->UnlockRect());
	surface_level->Release();
	// CPU mip chain. D3DXFilterTexture is not a reliable path under the pure-Vulkan D3D9 stub: the
	// generated mips could stay uninitialised, and the far tree then sampled garbage alpha (opaque
	// squares / flicker). Build the chain here: box-average RGB weighted by alpha, then binarise the
	// averaged coverage at 0x50. The 22/10 version kept the average alpha, which made the fixed 0x60
	// alpha test cross its threshold whenever the sampled mip changed - the visible leaf-edge shimmer.
	// Binarising every mip keeps all levels on the same side of the test. 0x50 (not 0x60) compensates
	// for box coverage shrinking slightly at coarse levels; the old "all four sub-texels >= 0x60" rule
	// was far more aggressive and shrank a 64px bush tile to zero by mip 2. The chain still stops at
	// TREE_ATLAS_MIP_LEVELS: a coarser mip averages a whole tile, and a cutout tree's tile is more than
	// 37% opaque, so every texel would pass and each distant card would become an opaque square.
	{
		IDirect3DTexture9* tex = Peek_D3D_Texture();
		const UINT levels = tex ? tex->GetLevelCount() : 0;
		if (levels > 1 && surface_desc.Format == D3DFMT_A8R8G8B8) {
			for (UINT level = 1; level < levels; ++level) {
				IDirect3DSurface9* src = nullptr; IDirect3DSurface9* dst = nullptr;
				D3DLOCKED_RECT slr, dlr; D3DSURFACE_DESC sd, dd;
				if (FAILED(tex->GetSurfaceLevel(level - 1, &src)) || !src) break;
				if (FAILED(tex->GetSurfaceLevel(level, &dst)) || !dst) { src->Release(); break; }
				bool ok = SUCCEEDED(src->GetDesc(&sd)) && SUCCEEDED(dst->GetDesc(&dd))
					&& SUCCEEDED(src->LockRect(&slr, nullptr, D3DLOCK_READONLY))
					&& SUCCEEDED(dst->LockRect(&dlr, nullptr, 0));
				if (ok) {
					for (UINT y = 0; y < dd.Height; ++y) {
						unsigned* drow = (unsigned*)((unsigned char*)dlr.pBits + (size_t)y * dlr.Pitch);
						const UINT sy0 = y * 2u, sy1 = (sd.Height > sy0 + 1u) ? sy0 + 1u : sy0;
						const unsigned* s0 = (const unsigned*)((const unsigned char*)slr.pBits + (size_t)sy0 * slr.Pitch);
						const unsigned* s1 = (const unsigned*)((const unsigned char*)slr.pBits + (size_t)sy1 * slr.Pitch);
						for (UINT x = 0; x < dd.Width; ++x) {
							const UINT sx0 = x * 2u, sx1 = (sd.Width > sx0 + 1u) ? sx0 + 1u : sx0;
							const unsigned p00 = s0[sx0], p01 = s0[sx1], p10 = s1[sx0], p11 = s1[sx1];
							const unsigned a00 = (p00 >> 24) & 0xFFu, a01 = (p01 >> 24) & 0xFFu;
							const unsigned a10 = (p10 >> 24) & 0xFFu, a11 = (p11 >> 24) & 0xFFu;
							const unsigned asum = a00 + a01 + a10 + a11;
							unsigned r = 0, g = 0, b = 0;
							if (asum)
							{
								// Alpha-weighted colour: transparent texels must not darken leaf edges.
								r = (((p00 >> 16) & 0xFFu) * a00 + ((p01 >> 16) & 0xFFu) * a01 +
									((p10 >> 16) & 0xFFu) * a10 + ((p11 >> 16) & 0xFFu) * a11 + asum / 2u) / asum;
								g = (((p00 >> 8) & 0xFFu) * a00 + ((p01 >> 8) & 0xFFu) * a01 +
									((p10 >> 8) & 0xFFu) * a10 + ((p11 >> 8) & 0xFFu) * a11 + asum / 2u) / asum;
								b = ((p00 & 0xFFu) * a00 + (p01 & 0xFFu) * a01 +
									(p10 & 0xFFu) * a10 + (p11 & 0xFFu) * a11 + asum / 2u) / asum;
							}
							// GeneralsX @bugfix 23/10/2026: keep the generated mips BINARY. The 22/10 chain
							// averaged alpha, so a leaf edge texel's coverage could fall above 0x60 on one
							// mip and below it on the next; every LOD switch then toggled the same silhouette
							// texel (the small-shrub shimmer). Box-average the coverage, then binarise it at
							// 0x50 - slightly under the 0x60 alpha test so the coarse-level coverage shrink
							// does not eat the leaf, while every mip stays on the same side of the test.
							unsigned a = (asum + 2u) >> 2;
							a = (a >= 0x50u) ? 255u : 0u;
							drow[x] = (a << 24) | (r << 16) | (g << 8) | b;
						}
					}
					dst->UnlockRect();
					src->UnlockRect();
				}
				dst->Release();
				src->Release();
				if (!ok) break;
			}
			static int s_mipLog = 0;
			if (s_mipLog < 2) {
				++s_mipLog;
				fprintf(stderr, "[TREEMIP] CPU-generated %u atlas mip levels (coverage-binarised, +%dpx gutters)\n",
					levels, TREE_ATLAS_GUTTER);
				fflush(stderr);
			}
		} else {
			DX8_ErrorCode(D3DXFilterTexture(Peek_D3D_Texture(), nullptr, (UINT)0, D3DX_FILTER_BOX));
		}
	}	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.3: mirror the procedurally-built tree atlas.
	// The mips were filled after the level-0 hash source was filled; drop any earlier mirror entry so
	// the upload cannot take the unchanged-content early-out and keep stale mips.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	VKTex_Drop((void*)Peek_D3D_Texture());
#endif
	VKTex_Mirror_D3D_Texture(Peek_D3D_Texture());
	if (WW3D::Get_Texture_Reduction()) {
		DX8_ErrorCode(Peek_D3D_Texture()->SetLOD((DWORD)WW3D::Get_Texture_Reduction()));
	}
	return(surface_desc.Height);
}


//=============================================================================
// W3DTreeBuffer::W3DTreeTextureClass::setLOD
//=============================================================================
/** Sets the lod of the texture to be loaded into the video card.  */
//=============================================================================
void W3DTreeBuffer::W3DTreeTextureClass::setLOD(Int LOD) const
{
	if (Peek_D3D_Texture()) {
		DX8_ErrorCode(Peek_D3D_Texture()->SetLOD((DWORD)LOD));
	}
}
//=============================================================================
// W3DTreeBuffer::W3DTreeTextureClass::Apply
//=============================================================================
/** Sets the texture as the current D3D texture, and does some custom setup
(standard D3D setup, but beyond the scope of W3D).  */
//=============================================================================
void W3DTreeBuffer::W3DTreeTextureClass::Apply(unsigned int stage)
{
	// Do the base apply.
	TextureClass::Apply(stage);
}
//-----------------------------------------------------------------------------
//         Private Data
//-----------------------------------------------------------------------------

#ifdef TEST_AND_BLEND
// A W3D shader that does alpha, texturing, tests zbuffer, doesn't update zbuffer.
#define SC_ALPHA_DETAIL ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_ENABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_SRC_ALPHA, \
	ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_ENABLE, ShaderClass::CULL_MODE_ENABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

#define SC_ALPHA_DETAIL_2X ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_ENABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_SRC_ALPHA, \
	ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE2X, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_ENABLE, ShaderClass::CULL_MODE_ENABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

#else
#define SC_ALPHA_DETAIL ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_ENABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_ONE, \
	ShaderClass::DSTBLEND_ZERO, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_ENABLE, ShaderClass::CULL_MODE_DISABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

#define SC_ALPHA_DETAIL_2X ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_ENABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_ONE, \
	ShaderClass::DSTBLEND_ZERO, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE2X, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_ENABLE, ShaderClass::CULL_MODE_ENABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )
#endif
static ShaderClass detailAlphaShader(SC_ALPHA_DETAIL);
static ShaderClass detailAlphaShader2X(SC_ALPHA_DETAIL_2X);

// GeneralsX @feature PS3.0 batch8 G1 14/10/2026 -----------------------------------------------------------------------
// TREE30: per-pixel haze for the fixed-function tree path (the one taken when shaders/Trees.vso is
// not installed; the .vso sway path keeps retail behaviour because a binary vertex shader cannot
// relay eye depth and POSITION-as-PS-input is rejected by fxc (X4502, batch8 probe forensics -
// logs/posw_probe_run5.txt). The shader mirrors this path's exact FF combiner state: stage0 is the
// detailAlphaShader GRADIENT_MODULATE (diffuse * atlas), stage1 is the shroud projection set by
// setShroudTex() (MODULATE rgb / SELECTARG2 keeps current alpha), and eye depth arrives through the
// stage-2 CAMERASPACEPOSITION COUNT3 identity relay - the only per-pixel depth channel that works
// on both native d3d9 and DXVK. c0/c1 are the shared [TERRAIN30] haze ramp, so the path is gated on
// W3DShaderManager::arePerPixelFogShadersActive(); GENERALSX_TREE30=0 opts out explicitly.
static const char* kTreePS30 =
	"sampler s0 : register(s0);\n"                 // atlas (stage0, uv0)
	"sampler s1 : register(s1);\n"                 // shroud projection (stage1, CAMERASPACEPOSITION COUNT2)
	"float4 c0 : register(c0);\n"                  // haze rgb, w = fog strength
	"float4 c1 : register(c1);\n"                  // x = eye-depth start, y = 1/ramp length
	"struct PS_IN\n"
	"{\n"
	"    float2 uv  : TEXCOORD0;\n"
	"    float2 shr : TEXCOORD1;\n"                 // projected shroud uv
	"    float3 dep : TEXCOORD2;\n"                 // .z = camera depth (stage-2 COUNT3 relay)
	"    float4 vc  : COLOR0;\n"                    // vertex diffuse
	"};\n"
	"float4 main(PS_IN i) : COLOR\n"
	"{\n"
	"    float4 tex = tex2D(s0, i.uv);\n"
	"    float4 c   = i.vc * tex;\n"                          // stage0: GRADIENT_MODULATE
	"    c.rgb      = c.rgb * tex2D(s1, i.shr).rgb;\n"        // stage1: shroud MODULATE
	"    float a    = tex.a * i.vc.a;\n"                      // stage1 ALPHAOP SELECTARG2 = current
	"    float fogf = saturate((-i.dep.z - c1.x) * c1.y) * c0.w;\n"   // batch23: camera z is negative ahead (D3D -Z); batch8 form folded to zero
	"    return float4(lerp(c.rgb, c0.rgb, fogf), a);\n"
	"}\n";
static IDirect3DPixelShader9* s_treeHlslPS = nullptr;
static Bool s_treeHlslTried = FALSE;


/*
#define SC_ALPHA_DETAIL ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_ENABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_ONE, \
	ShaderClass::DSTBLEND_ZERO, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_ENABLE, ShaderClass::CULL_MODE_DISABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

static ShaderClass detailAlphaShader(SC_ALPHA_DETAIL);
*/

/*
#define SC_ALPHA_DETAIL ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_DISABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_SRC_ALPHA, \
	ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::ALPHATEST_ENABLE, ShaderClass::CULL_MODE_ENABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

static ShaderClass detailAlphaShader(SC_ALPHA_DETAIL);
*/

/*
#define SC_ALPHA_MIRROR ( SHADE_CNST(ShaderClass::PASS_LEQUAL, ShaderClass::DEPTH_WRITE_DISABLE, ShaderClass::COLOR_WRITE_ENABLE, ShaderClass::SRCBLEND_SRC_ALPHA, \
	ShaderClass::DSTBLEND_ONE_MINUS_SRC_ALPHA, ShaderClass::FOG_DISABLE, ShaderClass::GRADIENT_MODULATE, ShaderClass::SECONDARY_GRADIENT_DISABLE, ShaderClass::TEXTURING_ENABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE, ShaderClass::ALPHATEST_DISABLE, ShaderClass::CULL_MODE_DISABLE, \
	ShaderClass::DETAILCOLOR_DISABLE, ShaderClass::DETAILALPHA_DISABLE) )

static ShaderClass mirrorAlphaShader(SC_ALPHA_DETAIL);

// ShaderClass::PASS_ALWAYS,

#define SC_ALPHA_2D ( SHADE_CNST(PASS_ALWAYS, DEPTH_WRITE_DISABLE, COLOR_WRITE_ENABLE, \
	SRCBLEND_SRC_ALPHA, DSTBLEND_ONE_MINUS_SRC_ALPHA, FOG_DISABLE, GRADIENT_DISABLE, \
	SECONDARY_GRADIENT_DISABLE, TEXTURING_ENABLE, DETAILCOLOR_DISABLE, DETAILALPHA_DISABLE, \
	ALPHATEST_DISABLE, CULL_MODE_ENABLE, DETAILCOLOR_DISABLE, DETAILALPHA_DISABLE) )
ShaderClass ShaderClass::_PresetAlpha2DShader(SC_ALPHA_2D);
*/
//-----------------------------------------------------------------------------
//         Private Functions
//-----------------------------------------------------------------------------

//=============================================================================
// W3DTreeBuffer::cull
//=============================================================================
/** Culls the trees, marking the visible flag.  If a tree becomes visible, it sets
it's sortKey */
//=============================================================================
void W3DTreeBuffer::cull(const CameraClass * camera)
{
	Int curTree;

	// Calculate the vector direction that the camera is looking at.
	const Matrix3D &camera_matrix = camera->Get_Transform();
	float zmod = -1;
	float x = zmod * camera_matrix[0][2] ;
	float y = zmod * camera_matrix[1][2] ;
	float z = zmod * camera_matrix[2][2] ;
	m_cameraLookAtVector.Set(x,y,z);

	for (curTree=0; curTree<m_numTrees; curTree++) {
		Bool doKey = false;	// We calculate the key when a tree becomes visible.
		Bool visible = !camera->Cull_Sphere(m_trees[curTree].bounds);
		if (visible != m_trees[curTree].visible) {
			m_trees[curTree].visible=visible;
			m_anythingChanged = true;
			if (visible) {
				doKey = true;
			}
		}
		// Also calculate sort key if a tree is visible, and the view changed setting m_updateAllKeys to true.
		if (doKey || (visible&&m_updateAllKeys)) {
			// The sort key is essentially the distance of location in the direction of the
			// camera look at.
			m_trees[curTree].sortKey = Vector3::Dot_Product(m_trees[curTree].location, m_cameraLookAtVector);
		}
	}
	m_updateAllKeys = false;
}
//=============================================================================
// W3DTreeBuffer::getPartitionBucket
//=============================================================================
/** Returns the bucket index into m_areaPartition for a given location. */
//=============================================================================
Int W3DTreeBuffer::getPartitionBucket(const Coord3D &pos) const
{
	Real x = pos.x;
	Real y = pos.y;
	if (x<m_bounds.lo.x) x = m_bounds.lo.x;
	if (y<m_bounds.lo.y) y = m_bounds.lo.y;
	if (x>m_bounds.hi.x) x = m_bounds.hi.x;
	if (y>m_bounds.hi.y) y = m_bounds.hi.y;
	Int xIndex = REAL_TO_INT_FLOOR ( (x/(m_bounds.hi.x-m_bounds.lo.x)) * (static_cast<float>(PARTITION_WIDTH_HEIGHT)-0.1f) );
	Int yIndex = REAL_TO_INT_FLOOR ( (y/(m_bounds.hi.y-m_bounds.lo.y)) * (static_cast<float>(PARTITION_WIDTH_HEIGHT)-0.1f) );
	DEBUG_ASSERTCRASH(xIndex>=0 && yIndex>=0 && xIndex<PARTITION_WIDTH_HEIGHT && yIndex<PARTITION_WIDTH_HEIGHT, ("Invalid range."));
	return yIndex*PARTITION_WIDTH_HEIGHT + xIndex;
}

//=============================================================================
// W3DTreeBuffer::updateSway
//=============================================================================
void W3DTreeBuffer::updateSway(const BreezeInfo& info)
{
	Int i;
	for	(i=0; i<NUM_SWAY_ENTRIES; i++) {
		Real factor = Cos(i*2.0f*PI/(static_cast<float>(NUM_SWAY_ENTRIES)+1.0f));
		Real angle = info.m_lean + (info.m_intensity  * factor);
		Real S = Sin(angle);
		Real C = Cos(angle);
		m_swayOffsets[i].X = info.m_directionVec.x * S;
		m_swayOffsets[i].Y = info.m_directionVec.y * S;
		m_swayOffsets[i].Z = C - 1.0f;
	}

	Real delta = info.m_randomness * 0.5f;

	for (i=0; i<MAX_SWAY_TYPES; i++) {
		m_curSwayStep[i] = static_cast<float>(NUM_SWAY_ENTRIES) / (Real)info.m_breezePeriod;
		m_curSwayStep[i]	*= GameClientRandomValueReal(1.0f-delta, 1.0f+delta);
		if (m_curSwayStep[i]<0.0f) {
			m_curSwayStep[i] = 0.0f;
		}
		m_curSwayOffset[i] = 0;
		m_curSwayFactor[i] = GameClientRandomValueReal(1.0f-delta, 1.0f+delta);
	}
	m_curSwayVersion = info.m_breezeVersion;
}

#if 0 // sort is not used, and messes up the order jba. [6/6/2003]
//=============================================================================
// W3DTreeBuffer::sort
//=============================================================================
/** Sorts the trees.  Does num_iterations of a bubble sort.  This is good because
it ends immediately if the trees are already sorted (which is most of the time)
and will perform a fixed amount of work each frame until it becomes sorted. */
//=============================================================================
void W3DTreeBuffer::sort(Int numIterations)
{
	// sort in descending order.
	Int iter;
	Bool swap = false;
	for (iter = 0; iter<numIterations; iter++) {
		Int cur = 0;
		// Note - only sorts the visible trees.
		while (cur<m_numTrees-iter && !m_trees[cur].visible) {
			cur++;
		}
		Int i;
		for (i=cur+1; i<m_numTrees-iter; i++) {
			if (m_trees[i].visible) {
				if (m_trees[cur].sortKey > m_trees[i].sortKey) {
					TTree tmp = m_trees[cur];
					m_trees[cur] = m_trees[i];
					m_trees[i] = tmp;
					swap = true;
				}
				cur = i;
			}
		}
		if (!swap) {
			return;
		}
		m_anythingChanged = true;
	}
}
#endif

/********** GDIFileStream2 class ****************************/
class GDIFileStream2 : public InputStream
{
protected:
	File* m_file;
public:
	GDIFileStream2():m_file(nullptr) {};
	GDIFileStream2(File* pFile):m_file(pFile) {};
	virtual Int read(void *pData, Int numBytes) override {
		return(m_file?m_file->read(pData, numBytes):0);
	};
};

//=============================================================================
// GeneralsX @feature T10 21/10/2026: original tree type -> GX RTS asset
//=============================================================================
static Bool treeFileExists(const char *path)
{
	if (!TheFileSystem || !path) {
		return TRUE;
	}
	File *f = TheFileSystem->openFile(path, File::READ | File::BINARY);
	if (f) {
		f->close();
		return TRUE;
	}
	return FALSE;
}

static Bool treeTextureExists(const char *name)
{
	if (!name) return FALSE;
	char path[_MAX_PATH];
	snprintf(path, ARRAY_SIZE(path), "%s%s", TERRAIN_TGA_DIR_PATH, name);
	if (treeFileExists(path)) return TRUE;
	snprintf(path, ARRAY_SIZE(path), "%s%s", TGA_DIR_PATH, name);
	return treeFileExists(path);
}

static Bool treeModelExists(const char *name)
{
	if (!name) return FALSE;
	char path[_MAX_PATH];
	snprintf(path, ARRAY_SIZE(path), "Art\W3D\%s.w3d", name);
	return treeFileExists(path);
}

struct TreeResolvedNameCache
{
	char from[64];
	char model[64];
	char texture[64];
};
static TreeResolvedNameCache s_resolvedNames[256];
static Int s_resolvedNameCount = 0;

static void resolveTreeNames(const W3DTreeDrawModuleData *data, AsciiString &model, AsciiString &texture)
{
	const char *origModel = data->m_modelName.str();
	const char *origTexture = data->m_textureName.str();
	for (Int i = 0; i < s_resolvedNameCount; ++i) {
		if (W3DTreeAliasCaseCmp(origModel, s_resolvedNames[i].from) == 0) {
			model = s_resolvedNames[i].model;
			texture = s_resolvedNames[i].texture;
			return;
		}
	}

	const char *resolvedModel = W3DTreeAliasResolve(origModel);
	const char *resolvedTexture = W3DTreeAliasResolveModelTexture(origModel);
	if (resolvedTexture == nullptr) {
		resolvedTexture = origTexture;
	}
	Bool aliased = (resolvedModel != origModel);
	if (aliased && (!treeModelExists(resolvedModel) || !treeTextureExists(resolvedTexture))) {
		static Int s_missingLog = 0;
		if (s_missingLog++ < 24) {
			fprintf(stderr, "[TREEALIAS] MISSING %s / %s, fallback %s / %s\n",
				resolvedModel, resolvedTexture, origModel, origTexture);
			fflush(stderr);
		}
		resolvedModel = origModel;
		resolvedTexture = origTexture;
		aliased = FALSE;
	}
	if (aliased) {
		static Int s_aliasLog = 0;
		if (s_aliasLog++ < 48) {
			fprintf(stderr, "[TREEALIAS] %s -> %s / %s\n", origModel, resolvedModel, resolvedTexture);
			fflush(stderr);
		}
	}
	if (s_resolvedNameCount < (Int)ARRAY_SIZE(s_resolvedNames)) {
		TreeResolvedNameCache &entry = s_resolvedNames[s_resolvedNameCount++];
		strlcpy(entry.from, origModel, sizeof(entry.from));
		strlcpy(entry.model, resolvedModel, sizeof(entry.model));
		strlcpy(entry.texture, resolvedTexture, sizeof(entry.texture));
	}
	model = resolvedModel;
	texture = resolvedTexture;
}

//=============================================================================
// W3DTreeBuffer::updateTexture
//=============================================================================
/** Creates a new texture. */
//=============================================================================
void W3DTreeBuffer::updateTexture()
{

	// GeneralsX @bugfix Qoder 22/10/2026 TREEATLAS: 4096 wide so the 16px-per-tile gutters do not
	// reduce how many tree sheets a map can keep in one atlas (2048 / 288px slots = 7x7 tiles).
	const Int MAX_TEX_WIDTH = 4096;

	Int i, j;
	Int maxHeight = 0;
	const Int maxTilesPerRow = MAX_TEX_WIDTH/TREE_ATLAS_SLOT;

	REF_PTR_RELEASE(m_treeTexture);

	Bool availableGrid[maxTilesPerRow][maxTilesPerRow];
	Int row, column;
	for (row=0; row<maxTilesPerRow; row++) {
		for (column=0; column<maxTilesPerRow; column++) {
			availableGrid[row][column] = true;
		}
	}

	for (i=0; i<m_numTiles; i++) {
		REF_PTR_RELEASE (m_sourceTiles[i]);
	}
	m_numTiles = 0;
	File *theFile = nullptr;
	for (i=0; i<m_numTreeTypes; i++) {
		char texturePath[ _MAX_PATH ];
		m_treeTypes[i].m_numTiles = 0;
		snprintf( texturePath, ARRAY_SIZE(texturePath), "%s%s", TERRAIN_TGA_DIR_PATH, m_treeTypes[i].m_textureName.str() );
		theFile = TheFileSystem->openFile( texturePath, File::READ|File::BINARY);
		if (theFile==nullptr) {
			snprintf( texturePath, ARRAY_SIZE(texturePath), "%s%s", TGA_DIR_PATH, m_treeTypes[i].m_textureName.str() );
			theFile = TheFileSystem->openFile( texturePath, File::READ|File::BINARY);
		}
		if (theFile != nullptr) {
			GDIFileStream2 theStream(theFile);
			InputStream *pStr = &theStream;
			Bool halfTile;
			Bool isLegacyGrid = false;
			Int numTiles = WorldHeightMap::countTiles(pStr, &halfTile, 0, &isLegacyGrid);
			Int width;
			for (width = 10; width >= 1; width--) {
				if (numTiles >= width*width) {
					numTiles = width*width;
					break;
				}
			}
			Bool texFound = false;
			for	(j=0; j<i; j++) {
				if (m_treeTypes[j].m_textureName.compareNoCase(m_treeTypes[i].m_textureName)==0) {
					m_treeTypes[i].m_firstTile = 0;
					m_treeTypes[i].m_tileWidth = width;
					m_treeTypes[i].m_numTiles = 0;
					texFound = true;
					break;
				}
			}
			if (texFound) {
				theFile->close();
				continue;
			}
			if (m_numTiles+numTiles<=MAX_TILES) {
				theFile->seek(0, File::START);
				m_treeTypes[i].m_firstTile = m_numTiles;
				m_treeTypes[i].m_tileWidth = width;
				m_treeTypes[i].m_numTiles = numTiles;
				m_treeTypes[i].m_halfTile = halfTile;
				WorldHeightMap::readTiles(pStr, m_sourceTiles+m_treeTypes[i].m_firstTile, width, isLegacyGrid);
				m_numTiles += numTiles;
			} else {
				m_treeTypes[i].m_firstTile = 0;
				m_treeTypes[i].m_tileWidth = 0;
				m_treeTypes[i].m_numTiles = 0;
			}
			theFile->close();
		} else {
			DEBUG_CRASH(("Could not find texture %s", m_treeTypes[i].m_textureName.str()));
			m_treeTypes[i].m_firstTile = 0;
			m_treeTypes[i].m_tileWidth = 0;
			m_treeTypes[i].m_numTiles = 0;
		}
	}

	Int tmpWidth = 8;
	while (tmpWidth*tmpWidth<m_numTiles) {
		tmpWidth*=2;
	}
	Int tilesPerRow = tmpWidth;
	m_textureWidth = tmpWidth*TREE_ATLAS_SLOT;

	fprintf(stderr, "[TREEATLAS] build tiles=%d tmpWidth=%d width=%d TILE=%d MAX=%d\n",
		m_numTiles, tmpWidth, m_textureWidth, (int)TILE_PIXEL_EXTENT, (int)MAX_TEX_WIDTH);
	fflush(stderr);

	if (m_textureWidth>MAX_TEX_WIDTH) {
		m_textureWidth = 64;
		m_textureHeight = 64;
		if (m_treeTexture==nullptr) {
			m_treeTexture = new TextureClass("missing.tga");
		}
		fprintf(stderr, "[TREEATLAS] FALLBACK to missing.tga: tile grid exceeds MAX_TEX_WIDTH\n");
		fflush(stderr);
		DEBUG_CRASH(("Too many trees in a scene."));
		return;
	}

	for (i=0; i<m_numTiles; i++) {
		if (m_sourceTiles[i]) {
			m_sourceTiles[i]->m_tileLocationInTexture.x = -1;
			m_sourceTiles[i]->m_tileLocationInTexture.y = -1;
		}
	}

	/* put the tree tiles into the texture */
	Int texClass;
	Int tileWidth;
	for (tileWidth = tilesPerRow; tileWidth>0; tileWidth--) {
		for (texClass=0; texClass<m_numTreeTypes; texClass++) {
			Int width = m_treeTypes[texClass].m_tileWidth;
			if (width != tileWidth) continue;
			Bool texFound = false;
			for	(i=0; i<texClass; i++) {
				if (m_treeTypes[i].m_textureName.compareNoCase(m_treeTypes[texClass].m_textureName)==0) {
					m_treeTypes[texClass].m_textureOrigin.x = m_treeTypes[i].m_textureOrigin.x;
					m_treeTypes[texClass].m_textureOrigin.y = m_treeTypes[i].m_textureOrigin.y;
					texFound = true;
					break;
				}
			}
			if (texFound) {
				continue;
			}

			// Find an available block of space.
			Bool found = false;
			for (row=0; row<(tilesPerRow-width)+1 && !found; row++) {
				for (column=0; column<(tilesPerRow-width)+1 && !found; column++) {
					if (availableGrid[row][column]) {
						Bool open = true;
						for (i=0; i<width && open; i++) {
							for (j=0; j<width&&open; j++) {
								if (!availableGrid[row+j][column+i]) {
									open = false;
								}
							}
						}
						if (open) found = true;
						break;
					}
				}
				if (found) break;
			}
			if (!found) {
				m_treeTypes[texClass].m_textureOrigin.x = 0;
				m_treeTypes[texClass].m_textureOrigin.y = 0;
				continue;
			}

			Int xOrigin = column*(TREE_ATLAS_SLOT);
			Int yOrigin = row*(TREE_ATLAS_SLOT);
			m_treeTypes[texClass].m_textureOrigin.x = xOrigin;
			m_treeTypes[texClass].m_textureOrigin.y = yOrigin;
			Int classHeight = yOrigin + width*TREE_ATLAS_SLOT;
			if (maxHeight < classHeight) maxHeight = classHeight;

			for (i=0; i<width; i++) {
				for (j=0; j<width; j++) {
					availableGrid[row+j][column+i] = false;
					Int baseNdx = m_treeTypes[texClass].m_firstTile + i + j*width;
					Int x = xOrigin + i*TREE_ATLAS_SLOT + TREE_ATLAS_GUTTER;
					Int y = yOrigin + ((width-j)-1)*TREE_ATLAS_SLOT + TREE_ATLAS_GUTTER;
					m_sourceTiles[baseNdx]->m_tileLocationInTexture.x = x;
					m_sourceTiles[baseNdx]->m_tileLocationInTexture.y = y;
				}
			}
		}
	}
	DEBUG_ASSERTCRASH(maxHeight<=m_textureWidth, ("Bad max height."));
	W3DTreeTextureClass *tex = new W3DTreeTextureClass((DWORD)m_textureWidth, (DWORD)m_textureWidth);
	m_textureHeight = tex->update(this);

	m_treeTexture = tex;
	fprintf(stderr, "[TREEATLAS] built %ux%u from %d tiles (texture=%s)\n",
		(unsigned)m_textureWidth, (unsigned)m_textureHeight, m_numTiles,
		(const char*)m_treeTexture->Get_Texture_Name());
	fflush(stderr);

	for (i=0; i<m_numTiles; i++) {
		REF_PTR_RELEASE (m_sourceTiles[i]);
	}
	//	m_treeTexture = NEW_REF (TextureClass, (m_treeTypes[0].m_textureName.str()));

}

/**Adjust the resolution of tree texture uploaded to the video card.  The system memory
version always remains at full resolution.  Someone should probably optimize this at
some point since it wastes a lot of system memory on low-end systems. -MW
*/
void W3DTreeBuffer::setTextureLOD(Int lod)
{
	if (m_treeTexture)
		((W3DTreeTextureClass*)m_treeTexture)->setLOD(lod);
}

//=============================================================================
// W3DTreeBuffer::doLighting
//=============================================================================
/** Calculates the diffuse lighting as affected by dynamic lighting. */
//=============================================================================
UnsignedInt W3DTreeBuffer::doLighting(const Vector3 *normal,
															const GlobalData::TerrainLighting	*objectLighting,
															const Vector3 *emissive, UnsignedInt vertDiffuse, Real scale) const
{

	Real shadeR, shadeG, shadeB;
	Real shade;
	shadeR = objectLighting[0].ambient.red+emissive->X;	//only the first light contributes to ambient
	shadeG = objectLighting[0].ambient.green+emissive->Y;
	shadeB = objectLighting[0].ambient.blue+emissive->Z;

	Int i;
	for	(i=0; i<MAX_GLOBAL_LIGHTS; i++) {
		Vector3 lightDirection(objectLighting[i].lightPos.x, objectLighting[i].lightPos.y, objectLighting[i].lightPos.z);
		lightDirection.Normalize();
		Vector3 lightRay(-lightDirection.X, -lightDirection.Y, -lightDirection.Z);
		shade = Vector3::Dot_Product(lightRay, *normal);

		if (shade > 1.0) shade = 1.0;
		if(shade < 0.0f) shade = 0.0f;
		shadeR += shade*objectLighting[i].diffuse.red;
		shadeG += shade*objectLighting[i].diffuse.green;
		shadeB += shade*objectLighting[i].diffuse.blue;
	}

	shadeR *= scale;
	shadeG *= scale;
	shadeB *= scale;

	if (shadeR > 1.0) shadeR = 1.0;
	if(shadeR < 0.0f) shadeR = 0.0f;
	if (shadeG > 1.0) shadeG = 1.0;
	if(shadeG < 0.0f) shadeG = 0.0f;
	if (shadeB > 1.0) shadeB = 1.0;
	if(shadeB < 0.0f) shadeB = 0.0f;

	if (vertDiffuse!=0xFFFFFFFF) {
		shade = vertDiffuse&0xff; //blue;
		shadeB *= shade/255.0f;
		shade = (vertDiffuse>>8)&0xFF; // green;
		shadeG *= shade/255.0f;
		shade = (vertDiffuse>>16)&0xFF; // red;
		shadeR *= shade/255.0f;
	}

	shadeR*=255.0f;
	shadeG*=255.0f;
	shadeB*=255.0f;
	const Real alpha = 255.0;
	return REAL_TO_UNSIGNEDINT(shadeB) | (REAL_TO_INT(shadeG) << 8) | (REAL_TO_INT(shadeR) << 16) | ((Int)alpha << 24);

}

//=============================================================================
// W3DTreeBuffer::loadTreesInVertexAndIndexBuffers
//=============================================================================
/** Loads the trees into the vertex buffer for drawing. */
//=============================================================================
void W3DTreeBuffer::loadTreesInVertexAndIndexBuffers(RefRenderObjListIterator *pDynamicLightsIterator)
{
	if (!m_indexTree[0] || !m_vertexTree[0] || !m_initialized) {
		return;
	}
	if (!m_anythingChanged) {
		return;
	}

	if (m_shadow == nullptr && TheW3DProjectedShadowManager) {
		Shadow::ShadowTypeInfo shadowInfo;
		shadowInfo.allowUpdates=FALSE;	//shadow image will never update
		shadowInfo.allowWorldAlign=TRUE;	//shadow image will wrap around world objects
		shadowInfo.m_type = (ShadowType)SHADOW_DECAL;
		shadowInfo.m_sizeX=20;
		shadowInfo.m_sizeY=20;
		m_shadow = TheW3DProjectedShadowManager->createDecalShadow(&shadowInfo);
	}

	// TheSuperHackers @bugfix Reset bufferNdx so updateVertexBuffer skips trees absent from this rebuild.
	for (Int t = 0; t < m_numTrees; t++) {
		m_trees[t].bufferNdx = -1;
	}
	m_anythingChanged = false;
	Int curTree=0;
	Int bNdx;
	const GlobalData::TerrainLighting *objectLighting = TheGlobalData->m_terrainObjectsLighting[W3DDisplay::getRenderTimeOfDay()];
	for (bNdx=0; bNdx<MAX_BUFFERS; bNdx++) {
		m_curNumTreeVertices[bNdx] = 0;
		m_curNumTreeIndices[bNdx] = 0;
		if (curTree >= m_numTrees) {
			break;
		}
		VertexFormatXYZNDUV1 *vb;
		UnsignedShort *ib;
		// Lock the buffers.
	#ifdef USE_STATIC
		DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexTree[bNdx], 0);
		DX8VertexBufferClass::WriteLockClass lockVtxBuffer(m_vertexTree[bNdx], 0);
	#else
		DX8IndexBufferClass::WriteLockClass lockIdxBuffer(m_indexTree[bNdx], D3DLOCK_DISCARD);
		DX8VertexBufferClass::WriteLockClass lockVtxBuffer(m_vertexTree[bNdx], D3DLOCK_DISCARD);
	#endif
		vb=(VertexFormatXYZNDUV1*)lockVtxBuffer.Get_Vertex_Array();
		ib = lockIdxBuffer.Get_Index_Array();
		if (vb == nullptr || ib == nullptr) {
			// A failed/degenerate lock must not turn into a write through a bad pointer.
			fprintf(stderr, "[TREEBUF] lock failed for buffer %d (vb=%p ib=%p)\n", bNdx, (void*)vb, (void*)ib);
			fflush(stderr);
			break;
		}
		// Add to the index buffer & vertex buffer.
		Vector2 lookAtVector(m_cameraLookAtVector.X, m_cameraLookAtVector.Y);
		lookAtVector.Normalize();
		// We draw from back to front, so we put the indexes in the buffer
		// from back to front.
		UnsignedShort *curIb = ib;

		VertexFormatXYZNDUV1 *curVb = vb;




		for ( ;curTree<m_numTrees;curTree++) {
			Int type = m_trees[curTree].treeType;
			if (type<0 || type>=MAX_TYPES || m_treeTypes[type].m_mesh == nullptr) {
				continue; // Deleted tree, out-of-range type, or missing mesh.
			}
			if (!m_trees[curTree].visible) continue;
			Real scale = m_trees[curTree].scale;
			Vector3 loc = m_trees[curTree].location;
			Real theSin = m_trees[curTree].sin;
			Real theCos = m_trees[curTree].cos;

			Bool doVertexLighting = true;

	#if 0 // no dynamic lighting.
			for (pDynamicLightsIterator->First(); !pDynamicLightsIterator->Is_Done(); pDynamicLightsIterator->Next())
			{
				W3DDynamicLight *pLight = (W3DDynamicLight*)pDynamicLightsIterator->Peek_Obj();
				if (!pLight->isEnabled()) {
					continue; // he is turned off.
				}
				if (CollisionMath::Overlap_Test(m_trees[curTree].bounds, pLight->Get_Bounding_Sphere()) == CollisionMath::OUTSIDE) {
					continue; // this tree is outside of the light's influence.
				}
				doVertexLighting = true;
			}
	#endif
			Vector3 emissive(0.0f,0.0f,0.0f);
			MaterialInfoClass * matInfo = m_treeTypes[type].m_mesh->Get_Material_Info();
			if (matInfo) {
				VertexMaterialClass *vertMat = matInfo->Peek_Vertex_Material(0);
				if (vertMat) {
					vertMat->Get_Emissive(&emissive);
				}
			}
			REF_PTR_RELEASE(matInfo);


			Int startVertex = m_curNumTreeVertices[bNdx];
			m_trees[curTree].firstIndex = startVertex;
			m_trees[curTree].bufferNdx = bNdx;
			Int i;
			Int numVertex = m_treeTypes[type].m_mesh->Peek_Model()->Get_Vertex_Count();
			Vector3 *pVert = m_treeTypes[type].m_mesh->Peek_Model()->Get_Vertex_Array();



			// If we happen to have too many trees, stop.
			if (m_curNumTreeVertices[bNdx]+numVertex+2>= MAX_TREE_VERTEX) {
				break;
			}
			Int numIndex = m_treeTypes[type].m_mesh->Peek_Model()->Get_Polygon_Count();
			const TriIndex *pPoly = m_treeTypes[type].m_mesh->Peek_Model()->Get_Polygon_Array();
			if (m_curNumTreeIndices[bNdx]+3*numIndex+6 >= MAX_TREE_INDEX) {
				break;
			}

			const Vector2*uvs=m_treeTypes[type].m_mesh->Peek_Model()->Get_UV_Array_By_Index(0);

			const Vector3*normals = m_treeTypes[type].m_mesh->Peek_Model()->Get_Vertex_Normal_Array();
			const unsigned *vecDiffuse = m_treeTypes[type].m_mesh->Peek_Model()->Get_Color_Array(0, false);

			Int diffuse = 0;
			if (normals == nullptr) {
				doVertexLighting = false;
				Vector3 normal(0.0f,0.0f,1.0f);
				diffuse = doLighting(&normal, objectLighting, &emissive, 0xFFFFFFFF, 1.0f);
			}

			const Int blockTiles = m_treeTypes[type].m_tileWidth;
			const Int slotExtent = TREE_ATLAS_SLOT;
			for (i=0; i<numVertex; i++) {
				if (m_curNumTreeVertices[bNdx] >= MAX_TREE_VERTEX)
					break;
				if (curVb >= vb + MAX_TREE_VERTEX)
					break;

				// Update the uv values.  The W3D models each have their own texture, and
				// we use one texture with all images in one, so we have to change the uvs to
				// match.
				Real U, V;
				U = uvs[i].U;
				V = uvs[i].V;

				if (U>1.0f) U=1.0f;
				if (U<0.0f) U=0.0f;
				if (V>1.0f) V=1.0f;
				if (V<0.0f) V=0.0f;

				// GeneralsX @bugfix Qoder 22/10/2026 TREEATLAS: resolve the authored UV to its tile
				// first, then re-map inside that tile's gutter slot. A plain linear scale across the
				// block (the retail formula) cannot respect the gutters inserted between tiles.
				// Block-local pixel coordinates (origin-free), preserving the retail span: a half
				// tile row only uses the upper half of the sheet and starts half a tile down.
				const Real halfScale = m_treeTypes[type].m_halfTile ? 0.5f : 1.0f;
				const Real vBias = m_treeTypes[type].m_halfTile ? (Real)TILE_PIXEL_EXTENT * 0.5f : 0.0f;
				Real uPix = U * (Real)blockTiles * (Real)TILE_PIXEL_EXTENT * halfScale;
				Real vPix = V * (Real)blockTiles * (Real)TILE_PIXEL_EXTENT * halfScale + vBias;
				Int uTile = REAL_TO_INT_FLOOR(uPix / (Real)TILE_PIXEL_EXTENT);
				Int vTile = REAL_TO_INT_FLOOR(vPix / (Real)TILE_PIXEL_EXTENT);
				if (uTile < 0) uTile = 0; else if (uTile >= blockTiles) uTile = blockTiles - 1;
				if (vTile < 0) vTile = 0; else if (vTile >= blockTiles) vTile = blockTiles - 1;
				Real uFrac = uPix / (Real)TILE_PIXEL_EXTENT - (Real)uTile;
				Real vFrac = vPix / (Real)TILE_PIXEL_EXTENT - (Real)vTile;
				if (uFrac < 0.0f) uFrac = 0.0f; else if (uFrac > 1.0f) uFrac = 1.0f;
				if (vFrac < 0.0f) vFrac = 0.0f; else if (vFrac > 1.0f) vFrac = 1.0f;

				curVb->u1 = ((Real)(m_treeTypes[type].m_textureOrigin.x + uTile*slotExtent + TREE_ATLAS_GUTTER)
					+ 0.5f + uFrac * (Real)(TILE_PIXEL_EXTENT - 1)) / (Real)m_textureWidth;
				curVb->v1 = ((Real)(m_treeTypes[type].m_textureOrigin.y + vTile*slotExtent + TREE_ATLAS_GUTTER)
					+ 0.5f + vFrac * (Real)(TILE_PIXEL_EXTENT - 1)) / (Real)m_textureHeight;
				Real x = pVert[i].X;
				Real y = pVert[i].Y;


				Vector3 vLoc;
				x += m_treeTypes[type].m_offset.X;
				y += m_treeTypes[type].m_offset.Y;
				vLoc.X = x*scale*theCos - y*scale*theSin;
				vLoc.Y = y*scale*theCos + x*scale*theSin;
				vLoc.Z = pVert[i].Z*scale;
				vLoc.Z += m_treeTypes[type].m_offset.Z;

				if (m_trees[curTree].m_toppleState != TOPPLE_UPRIGHT) {
					Matrix3D::Transform_Vector(m_trees[curTree].m_mtx, vLoc, &vLoc);
				} else {
					if (m_trees[curTree].pushAside>0.0f) {
						vLoc.X += pVert[i].Z * m_trees[curTree].pushAside * m_trees[curTree].pushAsideCos * m_treeTypes[type].m_data->m_maxOutwardMovement;
						vLoc.Y += pVert[i].Z * m_trees[curTree].pushAside * m_trees[curTree].pushAsideSin* m_treeTypes[type].m_data->m_maxOutwardMovement;
					}
					vLoc.X += loc.X;
					vLoc.Y += loc.Y;
					vLoc.Z += loc.Z;
				}


				curVb->x = vLoc.X;
				curVb->y = vLoc.Y;
				curVb->z = vLoc.Z;
				curVb->nx = m_trees[curTree].swayType;
				curVb->ny = 1.0f - m_treeTypes[type].m_data->m_darkening*m_trees[curTree].pushAside;
				curVb->nz = loc.Z;
				if (doVertexLighting) {
					Vector3 normal(0.0f, 0.0f, 1.0f);
					if (normals) {
						normal.X = normals[i].X*theCos - normals[i].Y*theSin;
						normal.Y = normals[i].Y*theCos + normals[i].X*theSin;
						normal.Z = normals[i].Z;
					}
					UnsignedInt vertexDiffuse;
					if (vecDiffuse) {
						vertexDiffuse = vecDiffuse[i];
					} else {
						vertexDiffuse = 0xffffffff;
					}
					curVb->diffuse = doLighting(&normal, objectLighting, &emissive,
														vertexDiffuse, 1.0f);
					{
						static int s_vtxLog = 0;
						if (s_vtxLog < 3) {
							++s_vtxLog;
							fprintf(stderr, "[TREEVTX] diffuse=%08X tod=%d objLight=%p amb=(%d,%d,%d) dif=(%d,%d,%d)\n",
								curVb->diffuse, (int)TheGlobalData->m_timeOfDay, (const void*)objectLighting,
								(int)objectLighting[0].ambient.red, (int)objectLighting[0].ambient.green, (int)objectLighting[0].ambient.blue,
								(int)objectLighting[0].diffuse.red, (int)objectLighting[0].diffuse.green, (int)objectLighting[0].diffuse.blue);
							fflush(stderr);
						}
					}
				} else {
					curVb->diffuse = diffuse;
				}
				curVb++;
				m_curNumTreeVertices[bNdx]++;
			}

			for (i=0; i<numIndex; i++) {
				if (m_curNumTreeIndices[bNdx]+4 > MAX_TREE_INDEX)
					break;
				if (curIb + 3 > ib + MAX_TREE_INDEX + 4)
					break;
				*curIb++ = startVertex + pPoly[i].I;
				*curIb++ = startVertex + pPoly[i].J;
				*curIb++ = startVertex + pPoly[i].K;
				m_curNumTreeIndices[bNdx]+=3;
			}
		}
	}

	// GeneralsX @debug T10 21/10/2026: report capacity, so a missing tree is not
	// mistaken for an alias/asset failure.
	{
		static Int s_layoutLog = 0;
		if (s_layoutLog++ < 12) {
			Int usedVerts = 0, usedIndices = 0, usedBuffers = 0;
			for (Int b = 0; b < MAX_BUFFERS; ++b) {
				usedVerts += m_curNumTreeVertices[b];
				usedIndices += m_curNumTreeIndices[b];
				if (m_curNumTreeIndices[b] > 0) ++usedBuffers;
			}
			fprintf(stderr, "[TREEBUF] placed=%d/%d types=%d buffers=%d/%d verts=%d/%d indices=%d/%d\n",
				curTree, m_numTrees, m_numTreeTypes, usedBuffers, (int)MAX_BUFFERS,
				usedVerts, (int)(MAX_BUFFERS * MAX_TREE_VERTEX),
				usedIndices, (int)(MAX_BUFFERS * MAX_TREE_INDEX));
			fflush(stderr);
			if (curTree < m_numTrees) {
				fprintf(stderr, "[TREEBUF] OVERFLOW remaining=%d trees (raise MAX_BUFFERS or lower RTS LOD)\n",
					m_numTrees - curTree);
				fflush(stderr);
			}
		}
	}

}
//=============================================================================
// W3DTreeBuffer::updateVertexBuffer
//=============================================================================
/** Updates the push aside offset in vertex buffer. */
//=============================================================================
void W3DTreeBuffer::updateVertexBuffer()
{
	if (!m_indexTree[0] || !m_vertexTree[0] || !m_initialized) {
		return;
	}
	Int bNdx;
	for	(bNdx = 0; bNdx<MAX_BUFFERS; bNdx++) {
		if (m_curNumTreeIndices[bNdx]==0) {
			break;
		}
		VertexFormatXYZNDUV1 *vb;
		// Lock the buffers.
	#ifdef USE_STATIC
		DX8VertexBufferClass::WriteLockClass lockVtxBuffer(m_vertexTree[bNdx], 0);
	#else
		DX8VertexBufferClass::WriteLockClass lockVtxBuffer(m_vertexTree[bNdx], D3DLOCK_DISCARD);
	#endif
		vb=(VertexFormatXYZNDUV1*)lockVtxBuffer.Get_Vertex_Array();
		if (!vb) {
			continue;
		}

		VertexFormatXYZNDUV1 *curVb;

		Int curTree;
		for (curTree=0; curTree<m_numTrees; curTree++) {
			if (m_trees[curTree].bufferNdx!=bNdx) {
				continue;
			}
			Int type = m_trees[curTree].treeType;
			if (m_trees[curTree].pushAsideDelta==0.0f && m_trees[curTree].m_toppleState == TOPPLE_UPRIGHT) {
				continue; // not toppling or pushed, no need to update. jba [7/11/2003]
			}
			m_anyPushChanged = true;
			if (!m_trees[curTree].visible) continue;
			Real scale = m_trees[curTree].scale;
			Vector3 loc = m_trees[curTree].location;
			Real theSin = m_trees[curTree].sin;
			Real theCos = m_trees[curTree].cos;
			DEBUG_ASSERTCRASH(type>=0 && m_treeTypes[type].m_mesh!=nullptr, ("Invalid tree type or mesh."));

			Int startVertex = m_trees[curTree].firstIndex;
			curVb = vb+startVertex;
			Int i;
			Int numVertex = m_treeTypes[type].m_mesh->Peek_Model()->Get_Vertex_Count();
			Vector3 *pVert = m_treeTypes[type].m_mesh->Peek_Model()->Get_Vertex_Array();

			for (i=0; i<numVertex; i++) {
				Real x = pVert[i].X;
				Real y = pVert[i].Y;


				Vector3 vLoc;
				x += m_treeTypes[type].m_offset.X;
				y += m_treeTypes[type].m_offset.Y;
				vLoc.X = x*scale*theCos - y*scale*theSin;
				vLoc.Y = y*scale*theCos + x*scale*theSin;
				vLoc.Z = pVert[i].Z*scale;
				vLoc.Z += m_treeTypes[type].m_offset.Z;

				if (m_trees[curTree].m_toppleState != TOPPLE_UPRIGHT) {
					m_trees[curTree].m_mtx.Transform_Vector(m_trees[curTree].m_mtx, vLoc, &vLoc);
				} else {
					if (m_trees[curTree].pushAside>0.0f) {
						vLoc.X += pVert[i].Z * m_trees[curTree].pushAside * m_trees[curTree].pushAsideCos * m_treeTypes[type].m_data->m_maxOutwardMovement;
						vLoc.Y += pVert[i].Z * m_trees[curTree].pushAside * m_trees[curTree].pushAsideSin* m_treeTypes[type].m_data->m_maxOutwardMovement;
					}
					vLoc.X += loc.X;
					vLoc.Y += loc.Y;
					vLoc.Z += loc.Z;
				}

				curVb->x = vLoc.X;
				curVb->y = vLoc.Y;
				curVb->z = vLoc.Z;
				curVb->ny = 1.0f - m_treeTypes[type].m_data->m_darkening*m_trees[curTree].pushAside;
				curVb++;
			}
		}
	}
}

//-----------------------------------------------------------------------------
//         Public Functions
//-----------------------------------------------------------------------------

//=============================================================================
// W3DTreeBuffer::~W3DTreeBuffer
//=============================================================================
/** Destructor. Releases w3d assets. */
//=============================================================================
W3DTreeBuffer::~W3DTreeBuffer()
{
	freeTreeBuffers();
	REF_PTR_RELEASE(m_treeTexture);
	Int i;
	for (i=0; i<MAX_TYPES; i++) {
		REF_PTR_RELEASE(m_treeTypes[i].m_mesh);
	}

	delete m_shadow;
	m_shadow = nullptr;
}

//=============================================================================
// W3DTreeBuffer::W3DTreeBuffer
//=============================================================================
/** Constructor. Sets m_initialized to true if it finds the w3d models it needs
for the trees. */
//=============================================================================
W3DTreeBuffer::W3DTreeBuffer()
{
	m_initialized = false;
	Int i;
	for	(i=0; i<MAX_BUFFERS; i++) {
		m_vertexTree[i] = nullptr;
		m_indexTree[i] = nullptr;
		m_curNumTreeVertices[i]=0;
		m_curNumTreeIndices[i]=0;
	}
	m_treeTexture = nullptr;
	m_dwTreeVertexShader = nullptr;
	m_dwTreePixelShader = nullptr;
	clearAllTrees();
	allocateTreeBuffers();
	m_initialized = true;
	m_curSwayVersion = -1;

	m_shadow = nullptr;

}


//=============================================================================
// W3DTreeBuffer::freeTreeBuffers
//=============================================================================
/** Frees the index and vertex buffers. */
//=============================================================================
void W3DTreeBuffer::freeTreeBuffers()
{
	Int i;
	for	(i=0; i<MAX_BUFFERS; i++) {
		REF_PTR_RELEASE(m_vertexTree[i]);
		REF_PTR_RELEASE(m_indexTree[i]);
	}

	if (m_dwTreePixelShader)
		m_dwTreePixelShader->Release();
	m_dwTreePixelShader = nullptr;

	if (m_dwTreeVertexShader)
		m_dwTreeVertexShader->Release();
	m_dwTreeVertexShader = nullptr;
}

//=============================================================================
// W3DTreeBuffer::unitMoved
//=============================================================================
/** Check to see if a unit collided with a tree/grass/bush. */
//=============================================================================
void W3DTreeBuffer::unitMoved(Object *unit)
{
	if (unit->isKindOf(KINDOF_IMMOBILE)) {
		// This is the initial positioning of the object, and we don't care. jba. [6/5/2003]
		return;
	}
	Real radius = unit->getGeometryInfo().getMajorRadius();
	if (unit->getGeometryInfo().getGeomType()==GEOMETRY_BOX) {
		if (radius>unit->getGeometryInfo().getMinorRadius()) {
			radius = unit->getGeometryInfo().getMinorRadius();
		}
	}
	// Value to assume for the tree radius.
#define TREE_RADIUS_APPROX 7.0f
	radius += TREE_RADIUS_APPROX;

	Coord3D pos = *unit->getPosition();
	Real x = pos.x-radius;
	Real y = pos.y-radius;
	if (x<m_bounds.lo.x) x = m_bounds.lo.x;
	if (y<m_bounds.lo.y) y = m_bounds.lo.y;
	if (x>m_bounds.hi.x) x = m_bounds.hi.x;
	if (y>m_bounds.hi.y) y = m_bounds.hi.y;
	Int xIndex = REAL_TO_INT_FLOOR ( (x/(m_bounds.hi.x-m_bounds.lo.x)) * (static_cast<float>(PARTITION_WIDTH_HEIGHT)-0.1f) );
	Int yIndex = REAL_TO_INT_FLOOR ( (y/(m_bounds.hi.y-m_bounds.lo.y)) * (static_cast<float>(PARTITION_WIDTH_HEIGHT)-0.1f) );
	DEBUG_ASSERTCRASH(xIndex>=0 && yIndex>=0 && xIndex<PARTITION_WIDTH_HEIGHT && yIndex<PARTITION_WIDTH_HEIGHT, ("Invalid range."));

	x = pos.x+radius;
	y = pos.y+radius;
	if (x<m_bounds.lo.x) x = m_bounds.lo.x;
	if (y<m_bounds.lo.y) y = m_bounds.lo.y;
	if (x>m_bounds.hi.x) x = m_bounds.hi.x;
	if (y>m_bounds.hi.y) y = m_bounds.hi.y;
	Int xMax = REAL_TO_INT_CEIL ( (x/(m_bounds.hi.x-m_bounds.lo.x)) * (static_cast<float>(PARTITION_WIDTH_HEIGHT)-0.1f) );
	Int yMax = REAL_TO_INT_CEIL ( (y/(m_bounds.hi.y-m_bounds.lo.y)) * (static_cast<float>(PARTITION_WIDTH_HEIGHT)-0.1f) );
	DEBUG_ASSERTCRASH(xMax>=0 && yMax>=0 && xMax<=PARTITION_WIDTH_HEIGHT && yMax<=PARTITION_WIDTH_HEIGHT, ("Invalid range."));
	Int i, j;
	for (i=xIndex; i<xMax; i++) {
		for (j=yIndex; j<yMax; j++) {
			Int treeNdx = m_areaPartition[i + PARTITION_WIDTH_HEIGHT*j];
			while (treeNdx != END_OF_PARTITION) {
				// paranoia [7/7/2003]
				if (treeNdx<0 || treeNdx>=m_numTrees) {
					DEBUG_CRASH(("Invalid index."));
					break;
				}
				if (m_trees[treeNdx].treeType<0) {
					treeNdx = m_trees[treeNdx].nextInPartition;
					continue;	//  Tree is deleted. [7/11/2003]
				}
				Coord3D delta;
				delta.set(m_trees[treeNdx].location.X, m_trees[treeNdx].location.Y, m_trees[treeNdx].location.Z );
				delta.sub(pos);
				if (radius*radius>delta.lengthSqr()) {
					bool canTopple = unit->getCrusherLevel() > 1;
					if (canTopple && m_treeTypes[m_trees[treeNdx].treeType].m_data->m_doTopple) {
						// Give a vector with direction to thing.
						Coord3D toppleVector;
						toppleVector.set(m_trees[treeNdx].location.X, m_trees[treeNdx].location.Y, 0);
						toppleVector.x -= unit->getPosition()->x;
						toppleVector.y -= unit->getPosition()->y;
						applyTopplingForce(m_trees+treeNdx, &toppleVector, 0, W3D_TOPPLE_OPTIONS_NONE);
					} else if (m_treeTypes[m_trees[treeNdx].treeType].m_data->m_framesToMoveOutward>1) {
						pushAsideTree(m_trees[treeNdx].drawableID, &pos, unit->getUnitDirectionVector2D(), unit->getID());
					}
				}
				treeNdx = m_trees[treeNdx].nextInPartition;
			}
		}
	}


}

//=============================================================================
// W3DTreeBuffer::allocateTreeBuffers
//=============================================================================
/** Allocates the index and vertex buffers. */
//=============================================================================
void W3DTreeBuffer::allocateTreeBuffers()
{
	Int i;
	for	(i=0; i<MAX_BUFFERS; i++) {
	#ifdef USE_STATIC
		m_vertexTree[i]=NEW_REF(DX8VertexBufferClass,(DX8_FVF_XYZNDUV1,MAX_TREE_VERTEX+4,DX8VertexBufferClass::USAGE_DEFAULT));
		m_indexTree[i]=NEW_REF(DX8IndexBufferClass,(MAX_TREE_INDEX+4, DX8IndexBufferClass::USAGE_DEFAULT));
	#else
		m_vertexTree[i]=NEW_REF(DX8VertexBufferClass,(DX8_FVF_XYZNDUV1,MAX_TREE_VERTEX+4,DX8VertexBufferClass::USAGE_DYNAMIC));
		m_indexTree[i]=NEW_REF(DX8IndexBufferClass,(MAX_TREE_INDEX+4, DX8IndexBufferClass::USAGE_DYNAMIC));
	#endif
		m_curNumTreeVertices[i]=0;
		m_curNumTreeIndices[i]=0;
	}

		// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: compile the TREE30 fixed-function haze pixel
		// shader once. This runs before the Trees.vso load below because the tree geometry only reaches
		// the FF path (Set_Vertex_FVF) when the .vso is absent - the exact case this HLSL PS serves. It is
		// gated on the ps_3_0 terrain trio being active so the shared c0/c1 haze ramp is valid.
	if (!s_treeHlslTried)
	{
		s_treeHlslTried = TRUE;
		// GeneralsX @bugfix PS3.0 batch22 14/10/2026: default off with the rest of the batch-8
		// surface HLSL family (user blocky-ground verdict); GENERALSX_TREE30=1 opts back in and
		// still requires the TERRAIN30 fog shaders to be active.
		// GeneralsX @bugfix 23/10/2026: TREE30 is opt-in again. Its translated twin (tag 121,
		// pgm_b1_tree.frag) introduced two independent differences from the fixed-function tree path -
		// a round-to-nearest alpha test where the FF shader truncates, and a Forward+ point-light term
		// whose tile Y was mirrored - and a map with `shroudBound` toggling could switch between the two
		// alpha rules. The 21/10 FF path is the stable reference. GENERALSX_TREE30=1 still opts in for
		// A/B runs; the env wins over this default.
		bool tree30On = false;
		const char* tenv = getenv("GENERALSX_TREE30");
		if (tenv != nullptr && *tenv != 0) tree30On = (strcmp(tenv, "0") != 0);
		if (tree30On && W3DShaderManager::arePerPixelFogShadersActive())
		{
			const char* prof = nullptr;
			const HRESULT hr = W3DShaderManager::compileShaderAtBestProfile(kTreePS30, false, "[TREE30]", &prof, (void**)&s_treeHlslPS);
			// Only keep the handle when the draw facade can translate it; an untranslated custom PS
			// would take over the batch and leave the trees undrawn under VKPRESENT.
			const bool translated = s_treeHlslPS != nullptr &&
				VKPgm_Tag_For_Handle((void*)s_treeHlslPS, 1) != 0;
			if (SUCCEEDED(hr) && translated)
			{
				fprintf(stderr, "[TREE30] HLSL tree haze pixel shader installed profile=%s\n", prof ? prof : "?");
				fflush(stderr);
			}
			else
			{
				if (s_treeHlslPS != nullptr) { s_treeHlslPS->Release(); s_treeHlslPS = nullptr; }
				fprintf(stderr, "[TREE30] HLSL compile failed, fixed-function trees kept\n");
				fflush(stderr);
			}
		}
	}

		//shader decleration
	// DX8_FVF_XYZNDUV1
	D3DVERTEXELEMENT9 Declaration[] =
	{
		
		D3DDECL_USAGE(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0, 0),  // Position
		D3DDECL_USAGE(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0, 12),  // Normal
		D3DDECL_USAGE(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0, 24), // Diffuse color
		D3DDECL_USAGE(D3DDECLTYPE_FLOAT2, D3DDECLUSAGE_TEXCOORD, 0, 28),  // Tex coord
		D3DDECL_END()
	};

	HRESULT hr;
	hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\Trees.vso", &Declaration[0], 0, true, (void**)&m_dwTreeVertexShader);
	if (FAILED(hr))
		return;

	hr = W3DShaderManager::LoadAndCreateD3DShader("shaders\\Trees.pso", &Declaration[0], 0, false, (void**)&m_dwTreePixelShader);
	if (FAILED(hr))
		return;
}

//=============================================================================
// W3DTreeBuffer::clearAllTrees
//=============================================================================
/** Removes all trees. */
//=============================================================================
void W3DTreeBuffer::clearAllTrees()
{
	m_numTrees=0;
	m_bounds.lo.x = m_bounds.lo.y = 0;
	m_bounds.hi.x = m_bounds.hi.y = 1;
	REF_PTR_RELEASE(m_treeTexture);
	m_curNumTreeIndices[0]=0;
	m_anythingChanged = true;
	Int i;
	for (i=0; i<MAX_TYPES; i++) {
		REF_PTR_RELEASE(m_treeTypes[i].m_mesh);
	}
	for (i=0; i<PARTITION_WIDTH_HEIGHT*PARTITION_WIDTH_HEIGHT; i++) {
		m_areaPartition[i] = END_OF_PARTITION;
	}
	m_numTreeTypes = 0;
}

//=============================================================================
// W3DTreeBuffer::removeTree
//=============================================================================
/** Removes a tree.  */
//=============================================================================
void W3DTreeBuffer::removeTree(DrawableID id)
{
	Int i;
	for (i=0; i<m_numTrees; i++) {
		if (m_trees[i].drawableID == id) {
			m_trees[i].location = Vector3(0,0,0);
			m_trees[i].treeType = DELETED_TREE_TYPE;
			// Translate the bounding sphere of the model.
			m_trees[i].bounds.Center = Vector3(0,0,0);
			m_trees[i].bounds.Radius = 1;
			m_anythingChanged = true;
		}
	}
}

//=============================================================================
// W3DTreeBuffer::removeTree
//=============================================================================
/** Removes any trees that would be under a building.  */
//=============================================================================
void W3DTreeBuffer::removeTreesForConstruction(const Coord3D* pos, const GeometryInfo& geom, Real angle )
{
	// Just iterate all trees, as even non-collidable ones get removed. jba. [7/11/2003]
	Int i;
	for (i=0; i<m_numTrees; i++) {
		// small, height,							radius,									minor radius
		if (m_trees[i].treeType < 0) {
			continue; // already deleted. jba [7/11/2003]
		}
		GeometryInfo info(GEOMETRY_CYLINDER, false, 5*TREE_RADIUS_APPROX, 2*TREE_RADIUS_APPROX, 2*TREE_RADIUS_APPROX);
		Coord3D treePos;
		treePos.set(m_trees[i].location.X, m_trees[i].location.Y, m_trees[i].location.Z);
		if (ThePartitionManager->geomCollidesWithGeom( pos, geom, angle, &treePos, info, 0.0f)) {
			// remove it [7/11/2003]
			m_trees[i].treeType = DELETED_TREE_TYPE;
			m_anythingChanged = true;
		}
	}
}


//=============================================================================
// W3DTreeBuffer::addTreeTypes
//=============================================================================
/** Adds a type of tree (model & texture). */
//=============================================================================
Int W3DTreeBuffer::addTreeType(const W3DTreeDrawModuleData *data)
{
	if (m_numTreeTypes>=MAX_TYPES) {
		DEBUG_CRASH(("Too many kinds of trees in map.  Reduce kinds of trees, or raise tree limit. jba."));
		return 0;
	}
	m_needToUpdateTexture = true;

	m_treeTypes[m_numTreeTypes].m_mesh = nullptr;
	AsciiString resolvedModel;
	AsciiString resolvedTexture;
	resolveTreeNames(data, resolvedModel, resolvedTexture);
	m_treeTypes[m_numTreeTypes].m_modelName = resolvedModel;
	m_treeTypes[m_numTreeTypes].m_textureName = resolvedTexture;
	static Int s_typeLog = 0;
	if (s_typeLog++ < 48) {
		fprintf(stderr, "[TREETYPE] model=%s texture=%s original=%s%s\n",
			resolvedModel.str(), resolvedTexture.str(), data->m_modelName.str());
		fflush(stderr);
	}

	RenderObjClass *robj=WW3DAssetManager::Get_Instance()->Create_Render_Obj(resolvedModel.str());

	if (robj==nullptr) {
		DEBUG_CRASH(("Unable to find model for tree %s", resolvedModel.str()));
		return 0;
	}
	AABoxClass box;

	robj->Get_Obj_Space_Bounding_Box(box);
	Vector3 offset(0,0,0);
	if (robj->Class_ID() == RenderObjClass::CLASSID_HLOD) {
		RenderObjClass *hlod = robj;
		robj = hlod->Get_Sub_Object(0);
		const Matrix3D& xfm = robj->Get_Bone_Transform(0);
		xfm.Get_Translation(&offset);
		REF_PTR_RELEASE(hlod);
	}

	if (robj->Class_ID() == RenderObjClass::CLASSID_MESH)
		m_treeTypes[m_numTreeTypes].m_mesh = (MeshClass*)robj;

	if (m_treeTypes[m_numTreeTypes].m_mesh==nullptr) {
		DEBUG_CRASH(("Tree %s is not simple mesh. Tell artist to re-export. Don't Ignore!!!", resolvedModel.str()));
		return 0;
	}

	Int numVertex = m_treeTypes[m_numTreeTypes].m_mesh->Peek_Model()->Get_Vertex_Count();
	Vector3 *pVert = m_treeTypes[m_numTreeTypes].m_mesh->Peek_Model()->Get_Vertex_Array();

	SphereClass bounds(pVert, numVertex);
	bounds.Center += offset;
	m_treeTypes[m_numTreeTypes].m_bounds = bounds;
	m_treeTypes[m_numTreeTypes].m_textureOrigin.x = 0;
	m_treeTypes[m_numTreeTypes].m_textureOrigin.y = 0;
	m_treeTypes[m_numTreeTypes].m_data = data;
	m_treeTypes[m_numTreeTypes].m_offset = offset;
	m_treeTypes[m_numTreeTypes].m_shadowSize = (box.Extent.X + box.Extent.Y); // Average extent * 2. jba.
	m_treeTypes[m_numTreeTypes].m_doShadow = data->m_doShadow;
	m_numTreeTypes++;
	return m_numTreeTypes-1;
}

//=============================================================================
// W3DTreeBuffer::addTree
//=============================================================================
/** Adds a tree.  Name is the W3D model name, supported models are
ALPINE, DECIDUOUS and SHRUB. */
//=============================================================================
void W3DTreeBuffer::addTree(DrawableID id, Coord3D location, Real scale, Real angle,
								Real randomScaleAmount, const W3DTreeDrawModuleData *data)
{
	if (m_numTrees >= MAX_TREES) {
		return;
	}
	if (!m_initialized) {
		return;
	}
	Int treeType = DELETED_TREE_TYPE;
	Int i;
	AsciiString resolvedModel;
	AsciiString resolvedTexture;
	resolveTreeNames(data, resolvedModel, resolvedTexture);
	for (i=0; i<m_numTreeTypes; i++) {
		if (m_treeTypes[i].m_modelName.compareNoCase(resolvedModel)==0 &&
				m_treeTypes[i].m_textureName.compareNoCase(resolvedTexture)==0) {
			treeType = i;
			break;
		}
	}
	if (treeType<0) {
		treeType = addTreeType(data);
		if (treeType<0) {
			return;
		}
		m_needToUpdateTexture = true;
	}
	if (data->m_framesToMoveOutward > 2 || data->m_doTopple) {
		// Trees/grass that topples or gets pushed aside (outward) gets put in the area partition. jba [7/7/2003]
		Short bucket = getPartitionBucket(location);
		m_trees[m_numTrees].nextInPartition = m_areaPartition[bucket];
		m_areaPartition[bucket] = m_numTrees;
	} else {
		m_trees[m_numTrees].nextInPartition = END_OF_PARTITION;
	}

	Real randomScale = GameClientRandomValueReal( 1.0f - randomScaleAmount, 1.0f+ randomScaleAmount );
	m_trees[m_numTrees].sin = WWMath::Sin(angle);
	m_trees[m_numTrees].cos = WWMath::Cos(angle);
	if (randomScaleAmount>0.0f) {
		// Randomizes the scale and orientation of trees.
		m_trees[m_numTrees].scale = scale*randomScale;
	} else {
		// Don't randomly scale & orient
		m_trees[m_numTrees].scale = scale;
	}
	m_trees[m_numTrees].location = Vector3(location.x, location.y, location.z);
	m_trees[m_numTrees].treeType = treeType;
	// Translate the bounding sphere of the model.
	m_trees[m_numTrees].bounds = m_treeTypes[treeType].m_bounds;
	m_trees[m_numTrees].bounds.Center *= m_trees[m_numTrees].scale;
	m_trees[m_numTrees].bounds.Radius *= m_trees[m_numTrees].scale;
	m_trees[m_numTrees].bounds.Center += m_trees[m_numTrees].location;
	// Initially set it invisible.  cull will update it's visibility flag.
	m_trees[m_numTrees].visible = false;
	m_trees[m_numTrees].drawableID = id;

	m_trees[m_numTrees].firstIndex = 0;
	m_trees[m_numTrees].bufferNdx = -1;

	m_trees[m_numTrees].swayType = GameClientRandomValue(1, MAX_SWAY_TYPES);
	m_trees[m_numTrees].pushAside = 0;
	m_trees[m_numTrees].lastFrameUpdated = 0;
	m_trees[m_numTrees].pushAsideSource = INVALID_ID;
	m_trees[m_numTrees].pushAsideDelta = 0;
	m_trees[m_numTrees].pushAsideCos = 1;
	m_trees[m_numTrees].pushAsideSin = 1;
	m_trees[m_numTrees].m_toppleState = TOPPLE_UPRIGHT;
	m_numTrees++;
}

//=============================================================================
// W3DTreeBuffer::updateTreePosition
//=============================================================================
/** Updates a tree's position */
//=============================================================================
Bool W3DTreeBuffer::updateTreePosition(DrawableID id, Coord3D location, Real angle)
{
	Int i;
	for (i=0; i<m_numTrees; i++) {
		if (m_trees[i].drawableID == id) {
			m_trees[i].location = Vector3(location.x, location.y, location.z);
			m_trees[i].sin = WWMath::Sin(angle);
			m_trees[i].cos = WWMath::Cos(angle);
			// Translate the bounding sphere of the model.
			m_trees[i].bounds = m_treeTypes[m_trees[i].treeType].m_bounds;
			m_trees[i].bounds.Center *= m_trees[i].scale;
			m_trees[i].bounds.Radius *= m_trees[i].scale;
			m_trees[i].bounds.Center += m_trees[i].location;
			m_anythingChanged = true;
			return true;
		}
	}
	return false;
}

//=============================================================================
// W3DTreeBuffer::pushAsideTree
//=============================================================================
/** Push sideways tree or grass. */
//=============================================================================
void W3DTreeBuffer::pushAsideTree(DrawableID id, const Coord3D *pusherPos,
																	const Coord3D *pusherDirection, ObjectID pusherID )
{
	Int i;
	for (i=0; i<m_numTrees; i++) {
		if (m_trees[i].drawableID == id) {
			UnsignedInt lastFrame = m_trees[i].lastFrameUpdated;
			m_trees[i].lastFrameUpdated = TheGameLogic->getFrame();
			if(m_trees[i].pushAsideSource == pusherID) {
				if (m_trees[i].lastFrameUpdated - lastFrame < 3)
					return; // already pushing. [5/28/2003]
			}

			if(m_trees[i].pushAside != 0.0f) {
				return; // already pushing. [5/28/2003]
			}
			m_trees[i].pushAsideSource = pusherID;
			Coord3D delta;
			delta.set(m_trees[i].location.X, m_trees[i].location.Y, m_trees[i].location.Z);
			delta.sub(*pusherPos);

			if (pusherDirection->x*delta.y - pusherDirection->y*delta.x > 0.0f) {
				m_trees[i].pushAsideCos = -pusherDirection->y;
				m_trees[i].pushAsideSin = pusherDirection->x;
			} else {
				m_trees[i].pushAsideCos = pusherDirection->y;
				m_trees[i].pushAsideSin = -pusherDirection->x;
			}
			m_anyPushChanged = true;
			m_trees[i].pushAsideDelta = 1.0f/(Real)m_treeTypes[m_trees[i].treeType].m_data->m_framesToMoveOutward;
		}
	}
}

DECLARE_PERF_TIMER(Tree_Render)

//=============================================================================
// W3DTreeBuffer::drawTrees
//=============================================================================
/** Draws the trees.  Uses camera to cull. */
//=============================================================================
void W3DTreeBuffer::drawTrees(CameraClass * camera, RefRenderObjListIterator *pDynamicLightsIterator)
{
	USE_PERF_TIMER(Tree_Render)
	if (!m_isTerrainPass) {
		return;
	}

	// if breeze changes, always process the full update, even if not visible,
	// so that things offscreen won't 'pop' when first viewed
	const BreezeInfo& info = TheScriptEngine->getBreezeInfo();
	if (info.m_breezeVersion != m_curSwayVersion)
	{
		updateSway(info);
	}

	// TheSuperHackers @tweak The tree sway, topple and sink time steps are now decoupled from the render update.
	const Real timeScale = TheFramePacer->getActualLogicTimeScaleOverFpsRatio();
	Vector3 swayFactor[MAX_SWAY_TYPES];
	Int i;
	for (i=0; i<MAX_SWAY_TYPES; i++)
	{
		m_curSwayOffset[i] += m_curSwayStep[i] * timeScale;
		if (m_curSwayOffset[i] > NUM_SWAY_ENTRIES-1) {
			m_curSwayOffset[i] -= NUM_SWAY_ENTRIES-1;
		}
		Int minOffset = REAL_TO_INT_FLOOR(m_curSwayOffset[i]);
		if (minOffset>=0 && minOffset+1<NUM_SWAY_ENTRIES) {
			Real f2 = m_curSwayOffset[i] - minOffset;
			Real f1 = 1.0f - f2;
			swayFactor[i] = f1*m_swayOffsets[minOffset] + f2*m_swayOffsets[minOffset+1];
			swayFactor[i] *= m_curSwayFactor[i];
		}
	}

	m_isTerrainPass = false;

	if (m_needToUpdateTexture) {
		m_needToUpdateTexture = false;
		updateTexture();
	}
	if (m_treeTexture==nullptr) {
		return;
	}
	if (m_updateAllKeys) {
		cull(camera);
	}

	Int curTree;
	// Draw tree shadows. GeneralsX @bugfix 23/10/2026: this path queues and flushes decals directly, so
	// DoShadows()' retail mute never saw it; with the native sun shadow on, the tree blobs were still
	// multiplied over the terrain (and a stale/black decal texture read as a dark patch under the tree).
	if (m_shadow && TheW3DProjectedShadowManager && TheGlobalData->m_useShadowDecals
		&& !DX8Wrapper::VK_Retail_Shadow_Off()) {
		for (curTree=0; curTree<m_numTrees; curTree++) {
			Int type = m_trees[curTree].treeType;
			if (type<0) {
				// deleted.
				continue;
			}
			if (!m_trees[curTree].visible || !m_treeTypes[type].m_doShadow) {
				continue;
			}

			if (m_trees[curTree].m_toppleState == TOPPLE_FALLING ||
					m_trees[curTree].m_toppleState == TOPPLE_DOWN) {
				continue;
			}
			m_shadow->setSize(m_treeTypes[type].m_shadowSize, m_treeTypes[type].m_shadowSize);
			m_shadow->setPosition(m_trees[curTree].location.X, m_trees[curTree].location.Y, m_trees[curTree].location.Z);
			TheW3DProjectedShadowManager->queueDecal(m_shadow);
		}
		TheW3DProjectedShadowManager->flushDecals(m_shadow->getTexture(0), SHADOW_DECAL);
	}

	// Update pushed aside and toppling trees.
	for (curTree=0; curTree<m_numTrees; curTree++) {
		Int type = m_trees[curTree].treeType;
		if (type<0) {
			// deleted.
			continue;
		}
		const W3DTreeDrawModuleData *moduleData = m_treeTypes[type].m_data;
		if(m_trees[curTree].m_toppleState == TOPPLE_FALLING ||
			 m_trees[curTree].m_toppleState == TOPPLE_FOGGED) {
			updateTopplingTree(m_trees+curTree, timeScale);
		} else if(m_trees[curTree].m_toppleState == TOPPLE_DOWN) {
			if (moduleData->m_killWhenToppled) {
				if (m_trees[curTree].m_sinkFramesLeft <= 0.0f) {
					m_trees[curTree].treeType = DELETED_TREE_TYPE; // delete it. [7/11/2003]
					m_anythingChanged = true; // need to regenerate trees. [7/11/2003]
				}
				const Real sinkDistancePerFrame = moduleData->m_sinkDistance / moduleData->m_sinkFrames;
				m_trees[curTree].m_sinkFramesLeft -= timeScale;
				m_trees[curTree].location.Z -= sinkDistancePerFrame * timeScale;
				m_trees[curTree].m_mtx.Set_Translation(m_trees[curTree].location);
			}
		} else if (m_trees[curTree].pushAsideDelta!=0.0f) {
			m_trees[curTree].pushAside += m_trees[curTree].pushAsideDelta;
			if (m_trees[curTree].pushAside>=1.0f) {
				m_trees[curTree].pushAsideDelta = -1.0f/(Real)moduleData->m_framesToMoveInward;
			} else if (m_trees[curTree].pushAside<=0.0f) {
				m_trees[curTree].pushAsideDelta = 0.0f;
				m_trees[curTree].pushAside = 0.0f;
			}
		}
	}

	if (m_anythingChanged) {
		loadTreesInVertexAndIndexBuffers(pDynamicLightsIterator);
		m_anythingChanged = false;
	} else if (m_anyPushChanged) {
		m_anyPushChanged = false;
		updateVertexBuffer();
	}

//#define DEBUG_TEXTURE 1
#ifdef DEBUG_TEXTURE // Draw the combined texture for debugging. jba. [4/21/2003]
	// Setup the vertex buffer, shader & texture.
	DX8Wrapper::Set_Shader(detailAlphaShader);
	DX8Wrapper::Set_Texture(0,m_treeTexture);
	DynamicIBAccessClass ib_access(BUFFER_TYPE_DYNAMIC_DX8, 6);
	//draw an infinite sky plane
	DynamicVBAccessClass vb_access(BUFFER_TYPE_DYNAMIC_DX8, DX8_FVF_XYZNDUV2, 4);
	{
		DynamicIBAccessClass::WriteLockClass ibLock(&ib_access);
		UnsignedShort *ndx = ibLock.Get_Index_Array();

		if (ndx) {
			ndx[0] = 0;
			ndx[1] = 1;
			ndx[2] = 2;
			ndx[3] = 1;
			ndx[4] = 3;
			ndx[5] = 2;
		}
		DynamicVBAccessClass::WriteLockClass lock(&vb_access);
		VertexFormatXYZNDUV2* verts=lock.Get_Formatted_Vertex_Array();
		if(verts)
		{
			Real width = 300;
			Real origin = 40;
			verts[0].x=origin;
			verts[0].y=origin;
			verts[0].z=15;
			verts[0].u1=0;
			verts[0].v1=0;
			verts[0].diffuse=0xffffffff;

			verts[1].x=origin+width;
			verts[1].y=origin;
			verts[1].z=15;
			verts[1].u1=1;
			verts[1].v1=0;
			verts[1].diffuse=0xffffffff;

			verts[2].x=origin;
			verts[2].y=origin+width;
			verts[2].z=15;
			verts[2].u1=0;
			verts[2].v1=1;
			verts[2].diffuse=0xffffffff;

			verts[3].x=origin+width;
			verts[3].y=origin+width;
			verts[3].z=15;
			verts[3].u1=1;
			verts[3].v1=1;
			verts[3].diffuse=0xffffffff;
		}
	}

	DX8Wrapper::Set_Index_Buffer(ib_access,0);
	DX8Wrapper::Set_Vertex_Buffer(vb_access);

	Matrix3D tm(1);
	DX8Wrapper::Set_Transform(D3DTS_WORLD,tm);

	DX8Wrapper::Draw_Triangles(	0,2, 0,	4);	//draw a quad, 2 triangles, 4 verts
#endif


	if (m_curNumTreeIndices[0] == 0) {
		return;
	}
	DX8Wrapper::Set_Shader(detailAlphaShader);

	DX8Wrapper::Set_Texture(0,m_treeTexture);
	DX8Wrapper::Set_Texture(1,nullptr);
	DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXCOORDINDEX, 0);
	DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXCOORDINDEX, 1);
	// GeneralsX @bugfix Qoder 21/10/2026 TREEALPHA: force the detailAlphaShader's exact raster state.
	// The projected-shadow decal drawn immediately before this pass leaves D3DRS_SRCBLEND=ZERO and
	// DSTBLEND=SRCCOLOR. ShaderClass::Apply only rewrites the blend/alpha-test bits when its cached
	// shader bits changed, so a tree batch that follows another tree batch can keep that decal blend:
	// leaves then multiply the terrain/destination (black silhouettes after a cutscene) and the
	// transparent atlas background shows as whole cards. The appended tree state also matches the
	// shaders' CULL_MODE_DISABLE, which the shared state shadow otherwise leaves culled.
	DX8Wrapper::Set_DX8_Render_State(D3DRS_SRCBLEND, D3DBLEND_ONE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_DESTBLEND, D3DBLEND_ZERO);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHABLENDENABLE, FALSE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHATESTENABLE, TRUE);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_ALPHAREF, 0x60);
	DX8Wrapper::Set_DX8_Render_State(D3DRS_CULLMODE, D3DCULL_NONE);
	// Draw all the trees.
	DX8Wrapper::Apply_Render_State_Changes();
	// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: capture the shroud bind result - the TREE30 HLSL
	// PS mirrors the stage-1 shroud modulation, so without a live shroud texture it would multiply the
	// trees to black; the path falls back to plain fixed-function rendering in that case.
	Int shroudBound = W3DShaderManager::setShroudTex(1);
	DX8Wrapper::Apply_Render_State_Changes();
	{
		// GeneralsX @debug Qoder 21/10/2026 TREEALPHA: isolate the stage-1 shroud modulation. If the
		// trees turn green with the shroud colour op bypassed, the black is the shroud sample/mask,
		// not the atlas or the vertex tint. Also used as the automatic fallback when the shroud has no
		// live source (an uninitialised destination is black, which blackened every tree).
		static const bool s_noShroud = (getenv("GENERALSX_TREE_NO_SHROUD") != nullptr)
			&& (atoi(getenv("GENERALSX_TREE_NO_SHROUD")) != 0);
		if (s_noShroud || !shroudBound) {
			DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
			DX8Wrapper::Set_DX8_Texture_Stage_State(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
			DX8Wrapper::Apply_Render_State_Changes();
		}
	}
	{
		static int s_drawProbe = 0;
		if (s_drawProbe < 4 && m_treeTexture != nullptr) {
			++s_drawProbe;
			fprintf(stderr, "[TREEDRAW] tex=%s at=%u func=%u ref=%u sb=%u db=%u shroud=%d "
				"aop0=%u a10=%u a20=%u aop1=%u a11=%u a21=%u cop0=%u tci1=%u ttff1=%u\n",
				(const char*)m_treeTexture->Get_Texture_Name(),
				DX8Wrapper::VK_Read_RS(D3DRS_ALPHATESTENABLE),
				DX8Wrapper::VK_Read_RS(D3DRS_ALPHAFUNC),
				DX8Wrapper::VK_Read_RS(D3DRS_ALPHAREF),
				DX8Wrapper::VK_Read_RS(D3DRS_SRCBLEND),
				DX8Wrapper::VK_Read_RS(D3DRS_DESTBLEND), shroudBound,
				DX8Wrapper::VK_Read_TSS(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE),
				DX8Wrapper::VK_Read_TSS(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
				DX8Wrapper::VK_Read_TSS(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE),
				DX8Wrapper::VK_Read_TSS(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE),
				DX8Wrapper::VK_Read_TSS(1, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
				DX8Wrapper::VK_Read_TSS(1, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE),
				DX8Wrapper::VK_Read_TSS(0, D3DTSS_COLOROP, D3DTOP_DISABLE),
				DX8Wrapper::VK_Read_TSS(1, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU),
				DX8Wrapper::VK_Read_TSS(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
			fflush(stderr);
		}
	}

	// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: TRUE when this call bound the TREE30 HLSL pixel
	// shader; the cleanup below then also drops the stage-2 depth relay so it cannot leak.
	Bool tree30BoundNow = FALSE;

	if (m_dwTreeVertexShader != nullptr) {
		// GeneralsX @bugfix BenderAI 13/02/2026 Matrix4x4 for GLM compatibility (fighter19 pattern)
		Matrix4x4 matProj, matView, matWorld;
		DX8Wrapper::_Get_DX8_Transform(D3DTS_WORLD, matWorld);
		DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, matView);
		DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, matProj);
		Matrix4x4 mat;
		D3DXMatrixMultiply( (D3DXMATRIX*)&mat, (D3DXMATRIX*)&matView, (D3DXMATRIX*)&matProj );
		D3DXMatrixMultiply( (D3DXMATRIX*)&mat, (D3DXMATRIX*)&matWorld, (D3DXMATRIX*)&mat );
		D3DXMatrixTranspose( (D3DXMATRIX*)&mat, (D3DXMATRIX*)&mat );

		// c4  - Composite World-View-Projection Matrix
		DX8Wrapper::Set_Vertex_Shader_Constant(  4, (const float*)&mat,  4 );
		Vector4 noSway(0,0,0,0);
		DX8Wrapper::Set_Vertex_Shader_Constant(  8, (const float*)&noSway,  1 );

		// c8 - c8+MAX_SWAY_TYPES - the sway amount.
		for	(i=0; i<MAX_SWAY_TYPES; i++) {
			Vector4 sway4(swayFactor[i].X, swayFactor[i].Y, swayFactor[i].Z, 0);
			DX8Wrapper::Set_Vertex_Shader_Constant(  9+i, (const float*)&sway4,  1 );
		}

		W3DShroud *shroud;
		if ((shroud=TheTerrainRenderObject->getShroud()) != nullptr) {
			// Setup shroud texture info [6/6/2003]
			float xoffset = 0;
			float yoffset = 0;
			Real width=shroud->getCellWidth();
			Real height=shroud->getCellHeight();

			xoffset = -(float)shroud->getDrawOriginX() + width;
			yoffset = -(float)shroud->getDrawOriginY() + height;
			Vector4 offset(xoffset, yoffset, 0, 0);
			DX8Wrapper::Set_Vertex_Shader_Constant(  32, (const float*)&offset,  1 );
			width = 1.0f/(width*shroud->getTextureWidth());
			height = 1.0f/(height*shroud->getTextureHeight());
			offset.Set(width, height, 1, 1);
			DX8Wrapper::Set_Vertex_Shader_Constant(  33, (const float*)&offset,  1 );

		} else {
			Vector4 offset(0,0,0,0);
			DX8Wrapper::Set_Vertex_Shader_Constant(  32, (const float*)&offset,  1 );
			DX8Wrapper::Set_Vertex_Shader_Constant(  33, (const float*)&offset,  1 );
		}

		DX8Wrapper::Set_Vertex_Shader((UINT_PTR)m_dwTreeVertexShader);
#if 0
		DX8Wrapper::Set_Pixel_Shader((UINT_PTR)m_dwTreePixelShader);
		// a.c. 6/16 - allow switching between normal and 2X mode for terrain
		Real mulTwoX = 0.5f;
		if(TheGlobalData && TheGlobalData->m_useOverbright)
			mulTwoX = 1.0f;
		DX8Wrapper::Set_Pixel_Shader_Constant(1, (const float*)D3DXVECTOR4(mulTwoX, mulTwoX, mulTwoX, mulTwoX), 1);
#endif

	} else {
		DX8Wrapper::Set_Vertex_FVF(DX8_FVF_XYZNDUV1); // GeneralsX @bugfix d3d9 port 12/10/2026 FVF constants must go through SetFVF

		// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: TREE30 per-pixel haze for the fixed-function
		// tree path. Needs the compiled HLSL PS and a live stage-1 shroud (the PS mirrors the shroud
		// modulation, so without it the trees would multiply to black). Eye depth arrives through the
		// stage-2 CAMERASPACEPOSITION COUNT3 identity relay - the channel the batch8 [FFDEPTH] probe
		// confirmed on both native d3d9 and DXVK; nothing samples stage 2, the generator output is
		// pure relay. The sway .vso path is untouched: a binary vertex shader cannot add the channel
		// (POSITION-as-PS-input is fxc-rejected X4502), so it keeps the retail fixed-function pixel
		// pipeline.
		if (s_treeHlslPS != nullptr && shroudBound)
		{
			W3DShaderManager::uploadPerPixelFogConstants();
			DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
			DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
			Matrix4x4 identM(true);
			DX8Wrapper::_Set_DX8_Transform(D3DTS_TEXTURE2, identM);
			tree30BoundNow = TRUE;
			DX8Wrapper::Set_Pixel_Shader((UINT_PTR)s_treeHlslPS);
		}
	}


	Int bNdx;
	for (bNdx=0;bNdx<MAX_BUFFERS; bNdx++) {
		if (m_curNumTreeIndices[bNdx]==0) {
			break;
		}
		DX8Wrapper::Set_Index_Buffer(m_indexTree[bNdx],0);
		DX8Wrapper::Set_Vertex_Buffer(m_vertexTree[bNdx]);
		// Render the waving grass
		DX8Wrapper::Apply_Render_State_Changes();
		if (m_dwTreeVertexShader != nullptr) {
			DX8Wrapper::Set_Vertex_Shader(reinterpret_cast<UINT_PTR>(m_dwTreeVertexShader));
			DX8Wrapper::Set_DX8_Texture_Stage_State(0,  D3DTSS_TEXCOORDINDEX, 0);
			DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXCOORDINDEX, 1);
			DX8Wrapper::Set_DX8_Texture_Stage_State(1,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		}
		DX8Wrapper::Draw_Triangles(	0, m_curNumTreeIndices[bNdx]/3, 0,	m_curNumTreeVertices[bNdx]);
	}

	// GeneralsX @bugfix Qoder 12/10/2026 - SetFVF does not clear the sway vertex shader on D3D9
	// (verified on native d3d9 and DXVK 2.6); the tree path binds it above, so it has to drop it
	// here or the next batch is transformed by the sway shader's constants.
	DX8Wrapper::Set_Vertex_Shader(0);
	DX8Wrapper::Set_Vertex_FVF(DX8_FVF_XYZNDUV1); // GeneralsX @bugfix d3d9 port 12/10/2026 FVF through SetFVF
	DX8Wrapper::Set_Pixel_Shader(0);
	// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: drop the TREE30 stage-2 depth relay so its
	// COUNT3 CAMERASPACEPOSITION generator cannot leak into the next fixed-function batch.
	if (tree30BoundNow)
	{
		DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
		DX8Wrapper::Set_DX8_Texture_Stage_State(2,  D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_PASSTHRU|2);
	}
	DX8Wrapper::Invalidate_Cached_Render_States();	//code above mucks around with W3D states so make sure we reset

}

//-------------------------------------------------------------------------------------------------
///< Start the toppling process by giving a force vector
//-------------------------------------------------------------------------------------------------
void W3DTreeBuffer::applyTopplingForce( TTree *tree, const Coord3D* toppleDirection, Real toppleSpeed,
																			 UnsignedInt options )
{
	if (tree->m_toppleState != TOPPLE_UPRIGHT) {
		return;
	}
	const W3DTreeDrawModuleData* d = m_treeTypes[tree->treeType].m_data;
  // Having a low toppleSpeed is BAD. In particular, if the toppleSpeed is exactly 0, the
  // tree will stay upright forever, frozen in place (because the sway update is dead)
  // but never dying
  if ( toppleSpeed < d->m_minimumToppleSpeed )
  {
    toppleSpeed = d->m_minimumToppleSpeed;
  }

	tree->m_toppleDirection = *toppleDirection;
	tree->m_toppleDirection.normalize();
	tree->m_angularAccumulation = 0;

	tree->m_angularVelocity = toppleSpeed * d->m_initialVelocityPercent;
	tree->m_angularAcceleration = toppleSpeed * d->m_initialAccelPercent;
	tree->m_toppleState = TOPPLE_FALLING;
	tree->m_options = options;
	Coord3D pos;
	pos.set(tree->location.X, tree->location.Y, tree->location.Z);
	FXList::doFXPos(d->m_toppleFX, &pos);
	m_anyPushChanged = true;
	tree->m_mtx.Make_Identity();
	tree->m_mtx.Set_Translation(tree->location);

}

// this is our "bounce" limit -- slightly less that 90 degrees, to account for slop.
static const Real ANGULAR_LIMIT = PI/2 - PI/64;

//-------------------------------------------------------------------------------------------------
///< Keep track of rotational fall distance, bounce and/or stop when needed.
//-------------------------------------------------------------------------------------------------
void W3DTreeBuffer::updateTopplingTree(TTree *tree, Real timeScale)
{
	//DLOG(Debug::Format("updating W3DTreeBuffer %08lx\n",this));
	DEBUG_ASSERTCRASH(tree->m_toppleState != TOPPLE_UPRIGHT, ("hmm, we should be sleeping here"));
	if ( (tree->m_toppleState == TOPPLE_UPRIGHT)  ||  (tree->m_toppleState == TOPPLE_DOWN) )
		return;

	const W3DTreeDrawModuleData* d = m_treeTypes[tree->treeType].m_data;
	const Int localPlayerIndex = rts::getObservedOrLocalPlayerIndex_Safe();
	Coord3D pos;
	pos.set(tree->location.X, tree->location.Y, tree->location.Z);
	ObjectShroudStatus ss = ThePartitionManager->getPropShroudStatusForPlayer(localPlayerIndex, &pos);
	if (ss==OBJECTSHROUD_FOGGED) {
		// Don't update fogged trees. [8/11/2003]
		tree->m_toppleState = TOPPLE_FOGGED;
		return;
	} else if (tree->m_toppleState == TOPPLE_FOGGED) {
		// was fogged, now isn't.
		tree->m_angularVelocity = 0;
		tree->m_toppleState = TOPPLE_DOWN;
		tree->m_mtx.In_Place_Pre_Rotate_X(-ANGULAR_LIMIT * tree->m_toppleDirection.y);
		tree->m_mtx.In_Place_Pre_Rotate_Y(ANGULAR_LIMIT * tree->m_toppleDirection.x);
		if (d->m_killWhenToppled) {
			// If got killed in the fog, just remove. jba [8/11/2003]
			tree->m_sinkFramesLeft = 0.0f;
		}
		return;
	}
	const Real VELOCITY_BOUNCE_LIMIT = 0.01f;				// if the velocity after a bounce will be this or lower, just stop at zero
	const Real VELOCITY_BOUNCE_SOUND_LIMIT = 0.03f;	// and if this low, then skip the bounce sound

	Real curVelToUse = tree->m_angularVelocity * timeScale;
	if (tree->m_angularAccumulation + curVelToUse > ANGULAR_LIMIT)
		curVelToUse = ANGULAR_LIMIT - tree->m_angularAccumulation;

	tree->m_mtx.In_Place_Pre_Rotate_X(-curVelToUse * tree->m_toppleDirection.y);
	tree->m_mtx.In_Place_Pre_Rotate_Y(curVelToUse * tree->m_toppleDirection.x);

	tree->m_angularAccumulation += curVelToUse;
	if ((tree->m_angularAccumulation >= ANGULAR_LIMIT) && (tree->m_angularVelocity > 0))
	{
		// Hit so either bounce or stop if too little remaining velocity.
		tree->m_angularVelocity *= -d->m_bounceVelocityPercent;

		if( BitIsSet( tree->m_options, W3D_TOPPLE_OPTIONS_NO_BOUNCE ) == TRUE ||
				fabs(tree->m_angularVelocity) < VELOCITY_BOUNCE_LIMIT )
		{
			// too slow, just stop
			tree->m_angularVelocity = 0;
			tree->m_toppleState = TOPPLE_DOWN;
			if (d->m_killWhenToppled) {
				tree->m_sinkFramesLeft = d->m_sinkFrames;
			}
		}
		else if( fabs(tree->m_angularVelocity) >= VELOCITY_BOUNCE_SOUND_LIMIT )
		{
			// fast enough bounce to warrant the bounce fx
			if( BitIsSet( tree->m_options, W3D_TOPPLE_OPTIONS_NO_FX ) == FALSE ) {
				Vector3 loc(0, 0, 3*TREE_RADIUS_APPROX); // Kinda towards the top of the tree. jba. [7/11/2003]
				Vector3 xloc;
				tree->m_mtx.Transform_Vector(tree->m_mtx, loc, &xloc);
				Coord3D pos;
				pos.set(xloc.X, xloc.Y, xloc.Z);
				FXList::doFXPos(d->m_bounceFX, &pos);
			}
		}
	}
	else
	{
		tree->m_angularVelocity += tree->m_angularAcceleration * timeScale;
	}

}

// ------------------------------------------------------------------------------------------------
/** CRC */
// ------------------------------------------------------------------------------------------------
void W3DTreeBuffer::crc( Xfer *xfer )
{
	// empty. jba [8/11/2003]
}

// ------------------------------------------------------------------------------------------------
/** Xfer
	* Version Info:
	* 1: Initial version
	* 2: TheSuperHackers @tweak Serialize sink frames as float instead of integer
	*/
// ------------------------------------------------------------------------------------------------
void W3DTreeBuffer::xfer( Xfer *xfer )
{

	// version
#if RETAIL_COMPATIBLE_XFER_SAVE
	XferVersion currentVersion = 1;
#else
	XferVersion currentVersion = 2;
#endif
	XferVersion version = currentVersion;
	xfer->xferVersion( &version, currentVersion );

	Int i;
	Int numTrees = m_numTrees;
	xfer->xferInt(&numTrees);
	if (xfer->getXferMode() == XFER_LOAD)	{
		m_numTrees = 0;
		for (i=0; i<PARTITION_WIDTH_HEIGHT*PARTITION_WIDTH_HEIGHT; i++) {
			m_areaPartition[i] = END_OF_PARTITION;
		}
	}

	// Save trees. [8/11/2003]
	for	(i=0; i<numTrees; i++) {
		TTree tree;
		memset(&tree, 0, sizeof(tree));
		AsciiString modelName;
		AsciiString modelTexture;
		Int treeType = DELETED_TREE_TYPE;
		if (xfer->getXferMode() != XFER_LOAD) {
			tree = m_trees[i];
			treeType = m_trees[i].treeType;
			if (treeType != DELETED_TREE_TYPE) {
				modelName = m_treeTypes[treeType].m_data->m_modelName;
				modelTexture = m_treeTypes[treeType].m_data->m_textureName;
			}
		}
		xfer->xferAsciiString(&modelName);
		xfer->xferAsciiString(&modelTexture);
		if (xfer->getXferMode() == XFER_LOAD) {
			Int j;
			for (j=0; j<m_numTreeTypes; j++) {
				if (m_treeTypes[j].m_data->m_modelName.compareNoCase(modelName)==0 &&
						m_treeTypes[j].m_data->m_textureName.compareNoCase(modelTexture)==0) {
					treeType = j;
					break;
				}
			}
		}

		xfer->xferReal(&tree.location.X);
		xfer->xferReal(&tree.location.Y);
		xfer->xferReal(&tree.location.Z);

		xfer->xferReal(&tree.scale);	///< Scale at location.
		xfer->xferReal(&tree.sin);	///< Sine of the rotation angle at location.
		xfer->xferReal(&tree.cos);	///< Cosine of the rotation angle at location.

		xfer->xferDrawableID(&tree.drawableID);	///< Drawable this tree corresponds to.

		// Topple parameters. [7/7/2003]
		xfer->xferReal(&tree.m_angularVelocity);	///< Velocity in degrees per frame (or is it radians per frame?)
		xfer->xferReal(&tree.m_angularAcceleration);	///< Acceleration angularVelocity is increasing
		xfer->xferCoord3D(&tree.m_toppleDirection);	///< Z-less direction we are toppling
		xfer->xferUser(&tree.m_toppleState, sizeof(tree.m_toppleState));	///< Stage this module is in.
		xfer->xferReal(&tree.m_angularAccumulation);	///< How much have I rotated so I know when to bounce.
		xfer->xferUnsignedInt(&tree.m_options);	///< topple options
		xfer->xferMatrix3D(&tree.m_mtx);

		if (version <= 1)
		{
			UnsignedInt sinkFramesLeft = (UnsignedInt)tree.m_sinkFramesLeft;
			xfer->xferUnsignedInt(&sinkFramesLeft);	///< Toppled trees sink into the terrain & disappear, how many frames left.
			tree.m_sinkFramesLeft = (Real)sinkFramesLeft;
		}
		else
		{
			xfer->xferReal(&tree.m_sinkFramesLeft);	///< Toppled trees sink into the terrain & disappear, how many frames left.
		}

		if (xfer->getXferMode() == XFER_LOAD && treeType != DELETED_TREE_TYPE && treeType < m_numTreeTypes) {
			Coord3D pos;
			pos.set(tree.location.X, tree.location.Y, tree.location.Z);
			Real angle = 0;
			addTree(tree.drawableID, pos, tree.scale, angle, 0, m_treeTypes[treeType].m_data);
			if (m_numTrees) {
				TTree *curTree = &m_trees[m_numTrees-1];
				curTree->m_angularAcceleration = tree.m_angularAcceleration;
				curTree->m_angularVelocity = tree.m_angularVelocity;
				curTree->m_toppleDirection = tree.m_toppleDirection;
				curTree->m_toppleState = tree.m_toppleState;
				curTree->m_options = tree.m_options;
				curTree->m_mtx = tree.m_mtx;
				curTree->m_sinkFramesLeft = tree.m_sinkFramesLeft;
			}
		}
	}

}

// ------------------------------------------------------------------------------------------------
/** Load post process */
// ------------------------------------------------------------------------------------------------
void W3DTreeBuffer::loadPostProcess()
{
	// empty. jba [8/11/2003]
}




