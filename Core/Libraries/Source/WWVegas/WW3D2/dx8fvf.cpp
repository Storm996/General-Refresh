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
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8fvf.h                               $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/26/02 5:06p                                             $*
 *                                                                                             *
 *                    $Revision:: 7                                                          $*
 *                                                                                             *
 * 06/26/02 KM VB Vertex format update for shaders                                       *
 * 07/17/02 KM VB Vertex format update for displacement mapping                               *
 * 08/01/02 KM VB Vertex format update for cube mapping                               *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "dx8fvf.h"
#include <cstdio>
#include <cstdlib>
#include "WWLib/wwstring.h"
#include <d3dx9core.h>

static unsigned Get_FVF_Vertex_Size(unsigned FVF)
{
	return D3DXGetFVFVertexSize(FVF);
}

FVFInfoClass::FVFInfoClass(unsigned FVF_)
	:
	FVF(FVF_),
	fvf_size(Get_FVF_Vertex_Size(FVF))
{
	location_offset=0;
	blend_offset=location_offset;

	if ((FVF&D3DFVF_XYZ)==D3DFVF_XYZ) blend_offset+=3*sizeof(float);
	// GeneralsX @bugfix Qoder 19/10/2026 wedge 11: D3DFVF_XYZRHW is not part of the D3DFVF_XYZ family -
	// it names the same position slot with FOUR floats (x, y, z, 1/w). Without this step every RHW batch
	// got texcoord_offset[0]==0, and the Vulkan facade bound TEXCOORD0 on top of the position: the sun
	// shadow casters wrote their pixel x (hundreds) into the map where the [0,1] depth belongs.
	else if ((FVF&D3DFVF_XYZRHW)==D3DFVF_XYZRHW) blend_offset+=4*sizeof(float);
	normal_offset=blend_offset;

	if ( ((FVF&D3DFVF_XYZB4)==D3DFVF_XYZB4) &&
		  ((FVF&D3DFVF_LASTBETA_UBYTE4)==D3DFVF_LASTBETA_UBYTE4) ) normal_offset+=3*sizeof(float)+sizeof(DWORD);
	diffuse_offset=normal_offset;

	if ((FVF&D3DFVF_NORMAL)==D3DFVF_NORMAL) diffuse_offset+=3*sizeof(float);
	specular_offset=diffuse_offset;

	if ((FVF&D3DFVF_DIFFUSE)==D3DFVF_DIFFUSE) specular_offset+=sizeof(DWORD);
	texcoord_offset[0]=specular_offset;

	if ((FVF&D3DFVF_SPECULAR)==D3DFVF_SPECULAR) texcoord_offset[0]+=sizeof(DWORD);

	for (unsigned int i=1; i<D3DDP_MAXTEXCOORD; i++)
	{
		texcoord_offset[i]=texcoord_offset[i-1];

		// GeneralsX @bugfix Qoder 22/10/2026: D3DFVF_TEXCOORDSIZE2(index) expands to
		// D3DFVF_TEXTUREFORMAT2 == 0, so the old chain's "(FVF & 0) == 0" test was always true and every
		// texcoord advanced by two floats. An XYZNDUV1TG3 vertex therefore reported tex2/tex3 at 44/52
		// instead of 48/60: the tangent/binormal/pad writes overlapped, the shader's T lost its z, B lost
		// its z, and normal-mapped barrels shaded by the vertex frame instead of by the texture. Read the
		// real two-bit format from bit 16 + 2*index (0 = 2 floats, 1 = 3, 2 = 4, 3 = 1), the same field
		// the Vulkan attribute table already decodes in dx8wrapper.cpp.
		const unsigned size_bits = (FVF >> (16u + 2u * (i - 1u))) & 0x3u;
		const unsigned comps = (size_bits == D3DFVF_TEXTUREFORMAT1) ? 1u
			: (size_bits == D3DFVF_TEXTUREFORMAT2) ? 2u
			: (size_bits == D3DFVF_TEXTUREFORMAT3) ? 3u : 4u;
		texcoord_offset[i] += comps * sizeof(float);
	}

	// GeneralsX @debug Qoder 22/10/2026: proof line for the tangent layout. Enable with
	// GENERALSX_FVF_LOG=1; TG3 must report size=72 and tex=(28,36,48,60).
	if (FVF == DX8_FVF_XYZNDUV1TG3)
	{
		static int s_log = -1;
		if (s_log < 0)
		{
			const char* e = getenv("GENERALSX_FVF_LOG");
			s_log = (e != nullptr && *e != 0 && *e != '0') ? 1 : 0;
		}
		if (s_log == 1)
		{
			fprintf(stderr, "[FVF] TG3 size=%u tex=(%u,%u,%u,%u)\n",
				fvf_size, texcoord_offset[0], texcoord_offset[1], texcoord_offset[2], texcoord_offset[3]);
			fflush(stderr);
		}
	}
}

void FVFInfoClass::Get_FVF_Name(StringClass& fvfname) const
{
	switch (Get_FVF()) {
	case DX8_FVF_XYZ: fvfname="D3DFVF_XYZ"; break;
	case DX8_FVF_XYZN: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL"; break;
	case DX8_FVF_XYZNUV1: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1"; break;
	case DX8_FVF_XYZNUV2: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX2"; break;
	case DX8_FVF_XYZNDUV1: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX1|D3DFVF_DIFFUSE"; break;
	case DX8_FVF_XYZNDUV2: fvfname="D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX2|D3DFVF_DIFFUSE"; break;
	case DX8_FVF_XYZDUV1: fvfname="D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_DIFFUSE"; break;
	case DX8_FVF_XYZDUV2: fvfname="D3DFVF_XYZ|D3DFVF_TEX2|D3DFVF_DIFFUSE"; break;
	case DX8_FVF_XYZUV1: fvfname="D3DFVF_XYZ|D3DFVF_TEX1"; break;
	case DX8_FVF_XYZUV2: fvfname="D3DFVF_XYZ|D3DFVF_TEX2"; break;
	case DX8_FVF_XYZNDUV1TG3 : fvfname="(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX4|D3DFVF_TEXCOORDSIZE2(0)|D3DFVF_TEXCOORDSIZE3(1)|D3DFVF_TEXCOORDSIZE3(2)|D3DFVF_TEXCOORDSIZE3(3))"; break;
	case DX8_FVF_XYZNUV2DMAP :	fvfname="(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_TEX3|D3DFVF_TEXCOORDSIZE1(0)|D3DFVF_TEXCOORDSIZE4(1)|D3DFVF_TEXCOORDSIZE2(2))"; break;
	case DX8_FVF_XYZNDCUBEMAP : fvfname="(D3DFVF_XYZ|D3DFVF_NORMAL|D3DFVF_DIFFUSE|D3DFVF_TEX1|D3DFVFTEXCOORDSIZE3(0)"; break;
	default: fvfname="Unknown!";
	}
}
