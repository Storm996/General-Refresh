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
 *                     $Archive:: ww3d2/texturefilter.cpp												$*
 *                                                                                             *
 *                  $Org Author:: Kenny Mitchell                                              $*
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 1                                                          $*
 *                                                                                             *
 * 08/05/02 KM Texture filter class abstraction																			*
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "texturefilter.h"
#include "dx8wrapper.h"
// GeneralsX @build PS3.0-P3-5 14/10/2026 getenv/fprintf for the terrain filter env override + marker
#include <stdlib.h>
#include <stdio.h>

const char* const TextureFilterClass::TextureFilterModeString[TEXTURE_FILTER_COUNT] = {
	"None",
	"Point",
	"Bilinear",
	"Trilinear",
	"Anisotropic"
};

TextureFilterClass::TextureFilterMode TextureFilterClass::getTextureFilterMode(const char* str) {
	for (int i = 0; i < TextureFilterClass::TEXTURE_FILTER_COUNT; ++i) {
		if (stricmp(str, TextureFilterClass::TextureFilterModeString[i]) == 0) {
			return (TextureFilterClass::TextureFilterMode)i;
		}
	}

	return TextureFilterClass::TEXTURE_FILTER_NONE;
}

unsigned _MinTextureFilters[MAX_TEXTURE_STAGES][TextureFilterClass::FILTER_TYPE_COUNT];
unsigned _MagTextureFilters[MAX_TEXTURE_STAGES][TextureFilterClass::FILTER_TYPE_COUNT];
unsigned _MipMapFilters[MAX_TEXTURE_STAGES][TextureFilterClass::FILTER_TYPE_COUNT];

/*************************************************************************
**                             TextureFilterClass
*************************************************************************/
TextureFilterClass::TextureFilterClass(MipCountType mip_level_count)
:	TextureMinFilter(FILTER_TYPE_DEFAULT),
	TextureMagFilter(FILTER_TYPE_DEFAULT),
	UAddressMode(TEXTURE_ADDRESS_REPEAT),
	VAddressMode(TEXTURE_ADDRESS_REPEAT)
{
	if (mip_level_count!=MIP_LEVELS_1)
	{
		MipMapFilter=FILTER_TYPE_DEFAULT;
	}
	else
	{
		MipMapFilter=FILTER_TYPE_NONE;
	}
}

//**********************************************************************************************
//! Apply filters (legacy)
/*!
*/
void TextureFilterClass::Apply(unsigned int stage)
{
	DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MINFILTER,_MinTextureFilters[stage][TextureMinFilter]);
	DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MAGFILTER,_MagTextureFilters[stage][TextureMagFilter]);
	DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MIPFILTER,_MipMapFilters[stage][MipMapFilter]);

	switch (Get_U_Addr_Mode())
	{
	case TEXTURE_ADDRESS_REPEAT:
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
		break;

	case TEXTURE_ADDRESS_CLAMP:
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
		break;
	}

	switch (Get_V_Addr_Mode())
	{
	case TEXTURE_ADDRESS_REPEAT:
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);
		break;

	case TEXTURE_ADDRESS_CLAMP:
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
		break;
	}
}

//**********************************************************************************************
//! Init filters (legacy)
/*!
*/
void TextureFilterClass::_Init_Filters(TextureFilterMode texture_filter, AnisotropicFilterMode anisotropy_level)
{
	const D3DCAPS9& dx8caps=DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps();

	// TheSuperHackers @info Init zero stage filter defaults, point filtering is the lowest type for non mip filtering
	_MinTextureFilters[0][FILTER_TYPE_NONE]=D3DTEXF_POINT;
	_MagTextureFilters[0][FILTER_TYPE_NONE]=D3DTEXF_POINT;
	_MipMapFilters[0][FILTER_TYPE_NONE]=D3DTEXF_NONE;

	// Bilinear
	_MinTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_LINEAR;
	_MagTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_LINEAR;
	_MipMapFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;

	// Anisotropic - MipMap interlayer filtering only goes up to linear
	_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_ANISOTROPIC;
	_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_ANISOTROPIC;
	_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;

	// TheSuperHackers @feature Mauller 08/03/2026 Add full support for all texture filtering modes;
	// None, Point, Bilinear, Trilinear, Anisotropic.
	BOOL FilterSupported = false;
	switch (texture_filter) {

	default:
		// TheSuperHackers @info if we have an invalid filter_type, set the filtering to none
		DEBUG_CRASH(("Invalid filter type passed into TextureFilterClass::_Init_Filters()"));
		FALLTHROUGH;

	case TEXTURE_FILTER_NONE:

		_MinTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;
		_MagTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;
		_MipMapFilters[0][FILTER_TYPE_FAST]=D3DTEXF_NONE;

		_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_NONE;
		break;

	case TEXTURE_FILTER_POINT:

		_MinTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;
		_MagTextureFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;
		_MipMapFilters[0][FILTER_TYPE_FAST]=D3DTEXF_POINT;

		_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		break;

	case TEXTURE_FILTER_BILINEAR:

		FilterSupported = (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) &&
			(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR);

		if (FilterSupported) {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}

		_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		break;

	case TEXTURE_FILTER_TRILINEAR:

		FilterSupported = (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) &&
			(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR);

		if (FilterSupported) {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}

		if (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MIPFLINEAR) {
			_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			// TheSuperHackers @info if only linear mipmap filtering is unsupported,
			// Trilinear filtering becomes Bilinear filtering by default
			_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}
		break;

	case TEXTURE_FILTER_ANISOTROPIC:

		FilterSupported = (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC) &&
			(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) &&
			dx8caps.MaxAnisotropy > 1;

		if (FilterSupported) {
			_MinTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_ANISOTROPIC;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=D3DTEXF_ANISOTROPIC;

			// Set the Anisotropic filtering level for all stages
			_Set_Max_Anisotropy(anisotropy_level);
		}
		else {
			// GeneralsX @bugfix Copilot 24/08/2026 Fall back to linear filtering when anisotropy is unavailable.
			const BOOL linearFilterSupported =
				(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFLINEAR) &&
				(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFLINEAR);
			_MinTextureFilters[0][FILTER_TYPE_BEST]=linearFilterSupported ? D3DTEXF_LINEAR : D3DTEXF_POINT;
			_MagTextureFilters[0][FILTER_TYPE_BEST]=linearFilterSupported ? D3DTEXF_LINEAR : D3DTEXF_POINT;
		}

		if (dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MIPFLINEAR) {
			_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MipMapFilters[0][FILTER_TYPE_BEST]=D3DTEXF_POINT;
		}
		break;

	}


	// For stages above zero, set best filter to the same as the stage zero
	int i=1;
	for (;i<MAX_TEXTURE_STAGES;++i) {
		_MinTextureFilters[i][FILTER_TYPE_NONE]=_MinTextureFilters[0][FILTER_TYPE_NONE];
		_MagTextureFilters[i][FILTER_TYPE_NONE]=_MagTextureFilters[0][FILTER_TYPE_NONE];
		_MipMapFilters[i][FILTER_TYPE_NONE]=_MipMapFilters[0][FILTER_TYPE_NONE];

		_MinTextureFilters[i][FILTER_TYPE_FAST]=_MinTextureFilters[0][FILTER_TYPE_FAST];
		_MagTextureFilters[i][FILTER_TYPE_FAST]=_MagTextureFilters[0][FILTER_TYPE_FAST];
		_MipMapFilters[i][FILTER_TYPE_FAST]=_MipMapFilters[0][FILTER_TYPE_FAST];

		// When Anisotropic filtering is used, all stages above zero use trilinear filtering
		if (_MagTextureFilters[0][FILTER_TYPE_BEST]==D3DTEXF_ANISOTROPIC) {
			_MagTextureFilters[i][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MagTextureFilters[i][FILTER_TYPE_BEST]=_MagTextureFilters[0][FILTER_TYPE_BEST];
		}

		if (_MinTextureFilters[0][FILTER_TYPE_BEST]==D3DTEXF_ANISOTROPIC) {
			_MinTextureFilters[i][FILTER_TYPE_BEST]=D3DTEXF_LINEAR;
		}
		else {
			_MinTextureFilters[i][FILTER_TYPE_BEST]=_MinTextureFilters[0][FILTER_TYPE_BEST];
		}
		_MipMapFilters[i][FILTER_TYPE_BEST]=_MipMapFilters[0][FILTER_TYPE_BEST];

	}

	// Set default to best. The level of best filter mode is controlled by the input parameter.
	for (i=0;i<MAX_TEXTURE_STAGES;++i) {
		_MinTextureFilters[i][FILTER_TYPE_DEFAULT]=_MinTextureFilters[i][FILTER_TYPE_BEST];
		_MagTextureFilters[i][FILTER_TYPE_DEFAULT]=_MagTextureFilters[i][FILTER_TYPE_BEST];
		_MipMapFilters[i][FILTER_TYPE_DEFAULT]=_MipMapFilters[i][FILTER_TYPE_BEST];
	}

}


//**********************************************************************************************
//! Set mip mapping filter (legacy)
/*!
*/
void TextureFilterClass::Set_Mip_Mapping(FilterType mipmap)
{
//	if (mipmap != FILTER_TYPE_NONE && Get_Mip_Level_Count() <= 1 && Is_Initialized())
//	{
//		WWASSERT_PRINT(0, "Trying to enable MipMapping on texture w/o Mip levels!");
//		return;
//	}
	MipMapFilter=mipmap;
}

//**********************************************************************************************
//! Set anisotropic filter level
/*!
*/
// GeneralsX @bugfix Copilot 24/08/2026 Clamp anisotropy to the active renderer capability.
int TextureFilterClass::_Set_Max_Anisotropy(int level)
{
	const D3DCAPS9& dx8caps=DX8Wrapper::Get_Current_Caps()->Get_DX8_Caps();
	const bool anisotropySupported =
		(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC) &&
		(dx8caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) &&
		dx8caps.MaxAnisotropy > 1;
	unsigned anisotropy=anisotropySupported ? (unsigned)level : 1;

	if (anisotropy < 1) anisotropy=1;
	if (anisotropySupported && anisotropy > dx8caps.MaxAnisotropy) anisotropy=dx8caps.MaxAnisotropy;

	for (int stage = 0; stage < MAX_TEXTURE_STAGES; ++stage)
		DX8Wrapper::Set_DX8_Sampler_State(stage, D3DSAMP_MAXANISOTROPY, anisotropy);

	return (int)anisotropy;
}

//**********************************************************************************************
//! Set default min filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Min_Filter(FilterType filter)
{
	for (int i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		_MinTextureFilters[i][FILTER_TYPE_DEFAULT]=_MinTextureFilters[i][filter];
	}
}


//**********************************************************************************************
//! Set default mag filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Mag_Filter(FilterType filter)
{
	for (int i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		_MagTextureFilters[i][FILTER_TYPE_DEFAULT]=_MagTextureFilters[i][filter];
	}
}

//**********************************************************************************************
//! Set default mip filter (legacy)
/*!
*/
void TextureFilterClass::_Set_Default_Mip_Filter(FilterType filter)
{
	for (int i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		_MipMapFilters[i][FILTER_TYPE_DEFAULT]=_MipMapFilters[i][filter];
	}
}

// ----------------------------------------------------------------------------
//
// GeneralsX @feature PS3.0-P3-5 14/10/2026 (report 画质提升总报告 §2 T3/T4)
// The five terrain shader set() paths in W3DShaderManager and the two live Apply() paths in
// TerrainTex used to write sampler states straight from the retail Bilinear/TrilinearTerrainTex
// INI bools. Both bools default to FALSE in the shipped GameData, so those stages were pinned to
// D3DTEXF_POINT min/mag/mip no matter what the player picked in the options menu - the biggest
// screen-area surfaces (the terrain atlas and its noise layer, the alpha-edge and scorch decals)
// lost bilinear, trilinear and anisotropic filtering alike. Three of the sites also differed from
// each other: the multi-stage variants left stage 1 on LINEAR while forcing stage 0 to POINT, an
// inconsistency with no visible purpose.
//
// Apply_Player_Filter replaces all of them by reading the same tier tables every other texture in
// the engine uses, so the player's mode, the capability vetting in _Init_Filters and the
// per-stage anisotropy downgrade (stage >0 samples a tiling helper layer, not a stretched surface)
// all apply automatically. The INI bools survive as a floor: TrilinearTerrainTex may still upgrade a
// POINT mip step. GENERALSX_TERRAIN_FILTER=legacy reproduces the retail override so the change can
// be A/B'd on the same binary.
//
// ----------------------------------------------------------------------------
unsigned TextureFilterClass::Get_Tier_Min_Filter(FilterType tier, unsigned stage)
{
	if (stage>=MAX_TEXTURE_STAGES) stage=MAX_TEXTURE_STAGES-1;
	if ((unsigned)tier>=FILTER_TYPE_COUNT) return D3DTEXF_POINT;
	return _MinTextureFilters[stage][tier];
}

unsigned TextureFilterClass::Get_Tier_Mag_Filter(FilterType tier, unsigned stage)
{
	if (stage>=MAX_TEXTURE_STAGES) stage=MAX_TEXTURE_STAGES-1;
	if ((unsigned)tier>=FILTER_TYPE_COUNT) return D3DTEXF_POINT;
	return _MagTextureFilters[stage][tier];
}

unsigned TextureFilterClass::Get_Tier_Mip_Filter(FilterType tier, unsigned stage)
{
	if (stage>=MAX_TEXTURE_STAGES) stage=MAX_TEXTURE_STAGES-1;
	if ((unsigned)tier>=FILTER_TYPE_COUNT) return D3DTEXF_NONE;
	return _MipMapFilters[stage][tier];
}

// One-time console marker so a screenshot leg can prove which path ran.
static bool s_terrainFilterLegacyMode=false;
static bool s_terrainFilterModeQueried=false;

static bool Terrain_Filter_Legacy_Mode()
{
	if (!s_terrainFilterModeQueried) {
		s_terrainFilterModeQueried=true;
		const char* env=getenv("GENERALSX_TERRAIN_FILTER");
		s_terrainFilterLegacyMode=(env!=nullptr && stricmp(env,"legacy")==0);
		fprintf(stderr,"[TEXCONV] terrain stage filtering: %s\n",
			s_terrainFilterLegacyMode?"retail override (legacy)":"player tier (converged)");
		fflush(stderr);
	}
	return s_terrainFilterLegacyMode;
}

// GeneralsX @feature PS3.0-P3-3r 14/10/2026 I4 consumer: the device-level mip capability that
// Compute_Caps probes into Support_MipMap_On_Particles() (D3DPTEXTURECAPS_MIPMAP, which under D3D9 is
// also the gate for mip-mapped point sprites) now decides whether a mip step may be requested at all.
// Previously the probe answer was logged and dropped, so the player's tier or an INI hint could raise
// MIPFILTER on hardware that reports no mipmapping support.
static bool Device_Supports_Mip_Filtering()
{
	const DX8Caps* caps=DX8Wrapper::Get_Current_Caps();
	if (caps==nullptr) return true;
	return caps->Support_MipMap_On_Particles();
}

void TextureFilterClass::Apply_Player_Filter(unsigned stage, bool bilinearHint, bool trilinearHint)
{
	if (stage>=MAX_TEXTURE_STAGES) stage=MAX_TEXTURE_STAGES-1;

	if (Terrain_Filter_Legacy_Mode()) {
		// Pre-P3-5 behaviour, kept verbatim for A/B legs: the bools are a ceiling.
		const bool linear=bilinearHint||trilinearHint;
		DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MINFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MAGFILTER,linear?D3DTEXF_LINEAR:D3DTEXF_POINT);
		DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MIPFILTER,trilinearHint?D3DTEXF_LINEAR:D3DTEXF_POINT);
		return;
	}

	unsigned minFilter=Get_Tier_Min_Filter(FILTER_TYPE_DEFAULT,stage);
	unsigned magFilter=Get_Tier_Mag_Filter(FILTER_TYPE_DEFAULT,stage);
	unsigned mipFilter=Get_Tier_Mip_Filter(FILTER_TYPE_DEFAULT,stage);

	// The retail TrilinearTerrainTex bool can only raise the mip step, never lower it.
	if (trilinearHint && mipFilter==D3DTEXF_POINT) mipFilter=D3DTEXF_LINEAR;
	if (!Device_Supports_Mip_Filtering()) mipFilter=D3DTEXF_NONE;

	DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MINFILTER,minFilter);
	DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MAGFILTER,magFilter);
	DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MIPFILTER,mipFilter);
}

void TextureFilterClass::Apply_Player_Mip_Filter(unsigned stage, bool trilinearHint)
{
	if (stage>=MAX_TEXTURE_STAGES) stage=MAX_TEXTURE_STAGES-1;

	// Retail rule for the mip-only override paths (the road shader): TrilinearTerrainTex decided
	// between LINEAR and POINT, and nothing else did.
	if (Terrain_Filter_Legacy_Mode()) {
		DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MIPFILTER,trilinearHint?D3DTEXF_LINEAR:D3DTEXF_POINT);
		return;
	}

	unsigned mipFilter=Get_Tier_Mip_Filter(FILTER_TYPE_DEFAULT,stage);
	if (trilinearHint && mipFilter==D3DTEXF_POINT) mipFilter=D3DTEXF_LINEAR;
	if (!Device_Supports_Mip_Filtering()) mipFilter=D3DTEXF_NONE;

	DX8Wrapper::Set_DX8_Sampler_State(stage,D3DSAMP_MIPFILTER,mipFilter);
}

