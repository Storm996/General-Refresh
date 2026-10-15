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

#include "bitmaphandler.h"
#include "WWDebug/wwdebug.h"
#include "colorspace.h"
// GeneralsX @feature PS3.0-P3-7 14/10/2026 getenv for the mip-kernel A/B switch, fprintf for the
// one-time console marker that lets a verification leg prove which kernel ran.
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void Bitmap_Assert(bool condition)
{
	WWASSERT(condition);
}

// ----------------------------------------------------------------------------
//
// GeneralsX @feature PS3.0-P3-7 14/10/2026 Load-time mip-chain kernel selection.
//
// The retail generator built every mip level from a 2x2 box average (Combine_A8R8G8B8), which
// is the low-pass that makes every non-DDS texture in the game look mushy from mip 1 upwards:
// a 50% duty box keeps only the DC term of each 2x2 block, so high-frequency detail is folded
// into the level below rather than attenuated, and the chain loses contrast far faster than a
// hardware-generated chain would.  The replacement is the Catmull-Rom form of the Lanczos-2
// windowed sinc, sampled at the fixed offsets of a 2:1 minification:
//
//   w(0.5) = 0.5625,  w(1.5) = -0.0625   ->  taps [-1/16, 9/16, 9/16, -1/16], sum = 1
//
// which keeps the passband the box filter throws away while its small negative lobes cancel
// most of the box's blur.  Negative lobes can overshoot, so every channel is clamped back into
// [0,255] and edge taps (where the window hangs off the surface) are re-normalised instead of
// silently darkening the border.
//
// The kernel replaces only the *halving* side effect of Copy_Image; the destination copy that
// happens in the same call still goes through the untouched retail code path, so formats that
// need conversion (DXT targets, 16-bit sources, bump-map Sobel passes, 1-pixel mip tails) keep
// exactly the behaviour they had.
//
// ----------------------------------------------------------------------------

static bool s_mipgenBoxMode=false;
static bool s_mipgenModeQueried=false;

static bool Mipgen_High_Quality_Mode()
{
	if (!s_mipgenModeQueried) {
		s_mipgenModeQueried=true;
		const char* env=getenv("GENERALSX_MIPGEN");
		s_mipgenBoxMode=(env!=nullptr && stricmp(env,"box")==0);
		fprintf(stderr,"[MIPGEN] load-time mip kernel: %s\n",
			s_mipgenBoxMode?"retail 2x2 box (legacy)":"catmull-rom / lanczos2");
		fflush(stderr);
	}
	return !s_mipgenBoxMode;
}

// Catmull-Rom (Lanczos-2) weight at distance t, in destination-filter units.
static float Lanczos2_Weight(float t)
{
	const float a=(t<0.0f)?-t:t;
	if (a<=1.0f) return ((1.5f*a-2.5f)*a*a)+1.0f;
	if (a<2.0f) return ((-0.5f*a+2.5f)*a-4.0f)*a+2.0f;
	return 0.0f;
}

// Halve a 32-bit (A8R8G8B8 / X8R8G8B8) surface in place, exactly the footprint the retail box
// path leaves behind: a (width/2 x height/2) image stored at the top-left of the same buffer,
// row pitch unchanged.  All reads happen before any write-back, so the wider 4-tap window can
// look ahead into rows the in-place 2-tap scheme would have had to consume first.
static void Downscale_Level_High_Quality(unsigned char* surface,unsigned width,unsigned height,
	unsigned pitch_bytes,const Vector3& hsv_shift)
{
	const unsigned hw=width/2;
	const unsigned hh=height/2;
	if (hw==0||hh==0) return;

	const bool has_hsv_shift=hsv_shift[0]!=0.0f||hsv_shift[1]!=0.0f||hsv_shift[2]!=0.0f;
	const unsigned pitch=pitch_bytes/4;
	unsigned* src=(unsigned*)surface;

	// The four taps of a 2:1 downscale sit at a constant fractional offset (centre = 2*d+0.5),
	// so the weights are loop invariants: -1/16, 9/16, 9/16, -1/16.
	float weight[4];
	for (int t=0;t<4;++t) weight[t]=Lanczos2_Weight((float)(t-2)+0.5f);

	unsigned* mip=new unsigned[hw*hh];
	for (unsigned y=0;y<hh;++y) {
		unsigned* mip_row=mip+y*hw;
		for (unsigned x=0;x<hw;++x) {
			float acc[4]={0.0f,0.0f,0.0f,0.0f};
			float wsum=0.0f;
			for (int ty=0;ty<4;++ty) {
				int sy=(int)(2*y+ty-1);
				if (sy<0) sy=0;
				if (sy>=(int)height) sy=height-1;
				const unsigned* src_row=src+(unsigned)sy*pitch;
				const float wy=weight[ty];
				for (int tx=0;tx<4;++tx) {
					int sx=(int)(2*x+tx-1);
					if (sx<0) sx=0;
					if (sx>=(int)width) sx=width-1;
					unsigned pixel=src_row[sx];
					if (has_hsv_shift) Recolor(pixel,hsv_shift);
					const float w=wy*weight[tx];
					acc[0]+=w*(float)(pixel&0xffu);
					acc[1]+=w*(float)((pixel>>8)&0xffu);
					acc[2]+=w*(float)((pixel>>16)&0xffu);
					acc[3]+=w*(float)((pixel>>24)&0xffu);
					wsum+=w;
				}
			}
			// Re-normalise so clamped edge windows keep the surface's brightness, then clamp the
			// negative-lobe overshoot back into the 8-bit range.
			const float inv=1.0f/wsum;
			unsigned out=0;
			for (int c=0;c<4;++c) {
				float v=acc[c]*inv;
				if (v<0.0f) v=0.0f;
				if (v>255.0f) v=255.0f;
				out|=((unsigned)(v+0.5f))<<(8*c);
			}
			mip_row[x]=out;
		}
	}

	for (unsigned y=0;y<hh;++y) {
		memcpy(src+y*pitch,mip+y*hw,hw*sizeof(unsigned));
	}
	delete[] mip;
}

void BitmapHandlerClass::Create_Mipmap_B8G8R8A8(
	unsigned char* dest_surface,
	unsigned dest_surface_pitch,
	unsigned char* src_surface,
	unsigned src_surface_pitch,
	unsigned width,
	unsigned height)
{
	unsigned src_pitch=src_surface_pitch/4;
	for (unsigned y=0;y<height;y+=2) {
		unsigned* dest=(unsigned*)dest_surface;
		dest_surface+=dest_surface_pitch;
		unsigned* src=(unsigned*)src_surface;
		src_surface+=src_surface_pitch;
		for (unsigned x=0;x<width;x+=2) {
			unsigned bgra3=src[src_pitch];
			unsigned bgra1=*src++;
			unsigned bgra4=src[src_pitch];
			unsigned bgra2=*src++;
			*dest++=Combine_A8R8G8B8(bgra1,bgra2,bgra3,bgra4);
		}
	}
}

void BitmapHandlerClass::Copy_Image_Generate_Mipmap(
	unsigned width,
	unsigned height,
	unsigned char* dest_surface,
	unsigned dest_pitch,
	WW3DFormat dest_format,
	unsigned char* src_surface,
	unsigned src_pitch,
	WW3DFormat src_format,
	unsigned char* mip_surface,
	unsigned mip_pitch,
	const Vector3& hsv_shift)
{
	// Optimized loop if source and destination are 32 bit
	bool has_hsv_shift = hsv_shift[0]!=0.0f || hsv_shift[1]!=0.0f || hsv_shift[2]!=0.0f;
	if (src_format==dest_format && src_format==WW3D_FORMAT_A8R8G8B8) {
		dest_pitch/=4;
		src_pitch/=4;
		mip_pitch/=4;
		// GeneralsX @bugfix Copilot 24/08/2026 Box-filter one-dimensional mip tails instead of leaving rectangular texture levels uninitialized.
		if (width==1) {
			for (unsigned y=0;y<height/2;++y) {
				unsigned* dest_ptr=(unsigned*)dest_surface+y*2*dest_pitch;
				unsigned* src_ptr=(unsigned*)src_surface+y*2*src_pitch;
				unsigned* mip_ptr=(unsigned*)mip_surface+y*mip_pitch;
				unsigned col0=src_ptr[0];
				unsigned col1=src_ptr[src_pitch];
				dest_ptr[0]=col0;
				dest_ptr[dest_pitch]=col1;
				unsigned combined=Combine_A8R8G8B8(col0,col0,col1,col1);
				if (has_hsv_shift) Recolor(combined,hsv_shift);
				mip_ptr[0]=combined;
			}
			return;
		}
		if (height==1) {
			unsigned* dest_ptr=(unsigned*)dest_surface;
			unsigned* src_ptr=(unsigned*)src_surface;
			unsigned* mip_ptr=(unsigned*)mip_surface;
			for (unsigned x=0;x<width/2;++x) {
				unsigned col0=*src_ptr++;
				unsigned col1=*src_ptr++;
				*dest_ptr++=col0;
				*dest_ptr++=col1;
				unsigned combined=Combine_A8R8G8B8(col0,col1,col0,col1);
				if (has_hsv_shift) Recolor(combined,hsv_shift);
				*mip_ptr++=combined;
			}
			return;
		}
		for (unsigned y=0;y<height/2;++y) {
			unsigned* dest_ptr=(unsigned*)dest_surface;
			dest_ptr+=2*y*dest_pitch;
			unsigned* src_ptr=(unsigned*)src_surface;
			src_ptr+=y*2*src_pitch;
			unsigned* mip_ptr=(unsigned*)mip_surface;
			mip_ptr+=y*mip_pitch;
			unsigned b8g8r8a8_00;
			unsigned b8g8r8a8_01;
			unsigned b8g8r8a8_10;
			unsigned b8g8r8a8_11;
			for (unsigned x=0;x<width/2;x++) {
				b8g8r8a8_10=src_ptr[src_pitch];
				dest_ptr[dest_pitch]=b8g8r8a8_10;
				b8g8r8a8_00=*src_ptr++;
				*dest_ptr++=b8g8r8a8_00;

				b8g8r8a8_11=src_ptr[src_pitch];
				dest_ptr[dest_pitch]=b8g8r8a8_11;
				b8g8r8a8_01=*src_ptr++;
				*dest_ptr++=b8g8r8a8_01;

				unsigned b8g8r8a8=Combine_A8R8G8B8(b8g8r8a8_00,b8g8r8a8_01,b8g8r8a8_10,b8g8r8a8_11);
				if (has_hsv_shift) {
					Recolor(b8g8r8a8,hsv_shift);
				}
				*mip_ptr++=b8g8r8a8;
			}
		}
		return;
	}

	WWASSERT(src_format!=WW3D_FORMAT_P8);		// This function doesn't support paletted formats
	unsigned src_bpp=Get_Bytes_Per_Pixel(src_format);
	unsigned dest_bpp=Get_Bytes_Per_Pixel(dest_format);

	// GeneralsX @bugfix Copilot 24/08/2026 Preserve format conversion and box filtering for one-dimensional mip tails.
	if (width==1) {
		for (unsigned y=0;y<height/2;++y) {
			unsigned char* dest_ptr=dest_surface+y*2*dest_pitch;
			unsigned char* src_ptr=src_surface+y*2*src_pitch;
			unsigned char* mip_ptr=mip_surface+y*mip_pitch;
			unsigned col0;
			unsigned col1;
			Read_B8G8R8A8(col0,src_ptr,src_format,nullptr,0);
			Read_B8G8R8A8(col1,src_ptr+src_pitch,src_format,nullptr,0);
			Write_B8G8R8A8(dest_ptr,dest_format,col0);
			Write_B8G8R8A8(dest_ptr+dest_pitch,dest_format,col1);
			unsigned combined=Combine_A8R8G8B8(col0,col0,col1,col1);
			if (has_hsv_shift) Recolor(combined,hsv_shift);
			Write_B8G8R8A8(mip_ptr,dest_format,combined);
		}
		return;
	}
	if (height==1) {
		unsigned char* dest_ptr=dest_surface;
		unsigned char* src_ptr=src_surface;
		unsigned char* mip_ptr=mip_surface;
		for (unsigned x=0;x<width/2;++x,dest_ptr+=dest_bpp*2,src_ptr+=src_bpp*2,mip_ptr+=dest_bpp) {
			unsigned col0;
			unsigned col1;
			Read_B8G8R8A8(col0,src_ptr,src_format,nullptr,0);
			Read_B8G8R8A8(col1,src_ptr+src_bpp,src_format,nullptr,0);
			Write_B8G8R8A8(dest_ptr,dest_format,col0);
			Write_B8G8R8A8(dest_ptr+dest_bpp,dest_format,col1);
			unsigned combined=Combine_A8R8G8B8(col0,col1,col0,col1);
			if (has_hsv_shift) Recolor(combined,hsv_shift);
			Write_B8G8R8A8(mip_ptr,dest_format,combined);
		}
		return;
	}

	for (unsigned y=0;y<height/2;++y) {
		unsigned char* dest_ptr=dest_surface+2*y*dest_pitch;
		unsigned char* src_ptr=src_surface+y*2*src_pitch;
		unsigned char* mip_ptr=mip_surface+y*mip_pitch;
		unsigned b8g8r8a8_00;
		unsigned b8g8r8a8_01;
		unsigned b8g8r8a8_10;
		unsigned b8g8r8a8_11;
		for (unsigned x=0;x<width/2;x++,dest_ptr+=dest_bpp*2,src_ptr+=src_bpp*2,mip_ptr+=dest_bpp) {
			Read_B8G8R8A8(b8g8r8a8_00,src_ptr,src_format,nullptr,0);
			Write_B8G8R8A8(dest_ptr,dest_format,b8g8r8a8_00);

			Read_B8G8R8A8(b8g8r8a8_01,src_ptr+src_bpp,src_format,nullptr,0);
			Write_B8G8R8A8(dest_ptr+dest_bpp,dest_format,b8g8r8a8_01);

			Read_B8G8R8A8(b8g8r8a8_10,src_ptr+src_pitch,src_format,nullptr,0);
			Write_B8G8R8A8(dest_ptr+dest_pitch,dest_format,b8g8r8a8_10);

			Read_B8G8R8A8(b8g8r8a8_11,src_ptr+src_bpp+src_pitch,src_format,nullptr,0);
			Write_B8G8R8A8(dest_ptr+dest_bpp+dest_pitch,dest_format,b8g8r8a8_11);

			unsigned b8g8r8a8=Combine_A8R8G8B8(b8g8r8a8_00,b8g8r8a8_01,b8g8r8a8_10,b8g8r8a8_11);
			if (has_hsv_shift) {
				Recolor(b8g8r8a8,hsv_shift);
			}

			Write_B8G8R8A8(mip_ptr,dest_format,b8g8r8a8);
		}
	}
}

// ----------------------------------------------------------------------------
//
// Copy image from source surface to destination surface with stretch and color
// space conversion if needed. If 'generate_mip_level' is set, process image
// in 2x2 blocks and generate mipmap on top of the original source image while
// copying.
//
// ----------------------------------------------------------------------------

void BitmapHandlerClass::Copy_Image(
	unsigned char* dest_surface,
	unsigned dest_surface_width,
	unsigned dest_surface_height,
	unsigned dest_surface_pitch,
	WW3DFormat dest_surface_format,
	unsigned char* src_surface,
	unsigned src_surface_width,
	unsigned src_surface_height,
	unsigned src_surface_pitch,
	WW3DFormat src_surface_format,
	const unsigned char* src_palette,
	unsigned src_palette_bpp,
	bool generate_mip_level,
	const Vector3& hsv_shift)
{
	WWASSERT(dest_surface_width);
	WWASSERT(dest_surface_height);

	// Bumpmap?
	if (dest_surface_format==WW3D_FORMAT_U8V8 ||
		dest_surface_format==WW3D_FORMAT_L6V5U5 ||
		dest_surface_format==WW3D_FORMAT_X8L8V8U8) {

		unsigned src_bpp=Get_Bytes_Per_Pixel(src_surface_format);

		for( unsigned y=0; y<dest_surface_height; y++ ) {
			unsigned char* dest_ptr=dest_surface;
			dest_ptr+=y*dest_surface_pitch;
			unsigned char* src_ptr_mid=src_surface;
			src_ptr_mid+=y*src_surface_pitch;
			unsigned char* src_ptr_next_line = ( src_ptr_mid + src_surface_pitch );
			unsigned char* src_ptr_prev_line = ( src_ptr_mid - src_surface_pitch );

			if( y == src_surface_height-1 )  // Don't go past the last line
				src_ptr_next_line = src_ptr_mid;
			if( y == 0 )               // Don't go before first line
				src_ptr_prev_line = src_ptr_mid;

			for( unsigned x=0; x<dest_surface_width; x++ ) {
				unsigned pixel00;
				unsigned pixel01;
				unsigned pixelM1;
				unsigned pixel10;
				unsigned pixel1M;

				Read_B8G8R8A8(pixel00,src_ptr_mid,src_surface_format,nullptr,0);
				Read_B8G8R8A8(pixel01,src_ptr_mid+src_bpp,src_surface_format,nullptr,0);
				Read_B8G8R8A8(pixelM1,src_ptr_mid-src_bpp,src_surface_format,nullptr,0);
				Read_B8G8R8A8(pixel10,src_ptr_prev_line,src_surface_format,nullptr,0);
				Read_B8G8R8A8(pixel1M,src_ptr_next_line,src_surface_format,nullptr,0);

				// Convert to luminance
				unsigned char bv00;
				unsigned char bv01;
				unsigned char bvM1;
				unsigned char bv10;
				unsigned char bv1M;
				Write_B8G8R8A8(&bv00,WW3D_FORMAT_L8,pixel00);
				Write_B8G8R8A8(&bv01,WW3D_FORMAT_L8,pixel01);
				Write_B8G8R8A8(&bvM1,WW3D_FORMAT_L8,pixelM1);
				Write_B8G8R8A8(&bv10,WW3D_FORMAT_L8,pixel10);
				Write_B8G8R8A8(&bv1M,WW3D_FORMAT_L8,pixel1M);
				int v00=bv00,v01=bv01,vM1=bvM1,v10=bv10,v1M=bv1M;

				int iDu = (vM1-v01); // The delta-u bump value
				int iDv = (v1M-v10); // The delta-v bump value

				if( (v00 < vM1) && (v00 < v01) ) {
					 // If we are at valley
					 iDu = vM1-v00;                 // Choose greater of 1st order diffs
					 if( iDu < v00-v01 )
						  iDu = v00-v01;
				}

				// The luminance bump value (land masses are less shiny)
				unsigned short uL = ( v00>1 ) ? 63 : 127;

				switch(dest_surface_format) {
				case WW3D_FORMAT_U8V8:
					*dest_ptr++ = (unsigned char)iDu;
					*dest_ptr++ = (unsigned char)iDv;
					break;

				case WW3D_FORMAT_L6V5U5:
					*(unsigned short*)dest_ptr  = (unsigned short)( ( (iDu>>3) & 0x1f ) <<  0 );
					*(unsigned short*)dest_ptr |= (unsigned short)( ( (iDv>>3) & 0x1f ) <<  5 );
					*(unsigned short*)dest_ptr |= (unsigned short)( ( ( uL>>2) & 0x3f ) << 10 );
					dest_ptr += 2;
					break;

				case WW3D_FORMAT_X8L8V8U8:
					*dest_ptr++ = (unsigned char)iDu;
					*dest_ptr++ = (unsigned char)iDv;
					*dest_ptr++ = (unsigned char)uL;
					*dest_ptr++ = (unsigned char)0L;
					break;

				default:
					WWASSERT(0);	// Unknown bumpmap format
					break;
				}

				// Move one pixel to the left (src is 32-bpp)
				src_ptr_mid+=src_bpp;
				src_ptr_prev_line+=src_bpp;
				src_ptr_next_line+=src_bpp;
			}
		}
		return;
	}

	// GeneralsX @feature PS3.0-P3-7 14/10/2026 High-quality mip-chain hook.
	//
	// Only the halving side effect of the mip-generation call is replaced: the destination copy
	// is delegated back through the untouched retail code (same formats, generate_mip_level now
	// false), and the next level is then built from the 32-bit source with the windowed-sinc
	// kernel. Bump-map (Sobel) passes returned above, 16-bit/paletted sources and non-32-bit
	// *sources* keep the retail path, as do the 1-pixel mip tails where a 4-tap window has
	// nothing left to reach into. Set GENERALSX_MIPGEN=box to get the shipped box filter back
	// for an A/B leg.
	if (generate_mip_level && Mipgen_High_Quality_Mode() &&
		(src_surface_format==WW3D_FORMAT_A8R8G8B8 || src_surface_format==WW3D_FORMAT_X8R8G8B8) &&
		dest_surface_width==src_surface_width && dest_surface_height==src_surface_height &&
		dest_surface_width>1 && dest_surface_height>1)
	{
		Copy_Image(
			dest_surface,
			dest_surface_width,
			dest_surface_height,
			dest_surface_pitch,
			dest_surface_format,
			src_surface,
			src_surface_width,
			src_surface_height,
			src_surface_pitch,
			src_surface_format,
			src_palette,
			src_palette_bpp,
			false,
			hsv_shift);
		Downscale_Level_High_Quality(src_surface,src_surface_width,src_surface_height,
			src_surface_pitch,hsv_shift);
		return;
	}

	bool has_hsv_shift = hsv_shift[0]!=0.0f || hsv_shift[1]!=0.0f || hsv_shift[2]!=0.0f;
	if (src_surface_format==dest_surface_format && (src_surface_format==WW3D_FORMAT_A8R8G8B8 || src_surface_format==WW3D_FORMAT_X8R8G8B8)) {
		// One-to-one copy or scaling?
		dest_surface_pitch/=4;
		src_surface_pitch/=4;
		if (dest_surface_width==src_surface_width && dest_surface_height==src_surface_height) {
			// Generate the next mip level while copying the current surface?
			if (generate_mip_level) {
				if (dest_surface_width==1) {
					unsigned* dest_ptr=(unsigned*)dest_surface;
					unsigned* src_ptr=(unsigned*)src_surface;
					if (dest_surface_height==1) {
						unsigned b8g8r8a8=*src_ptr;
						if (has_hsv_shift) Recolor(b8g8r8a8,hsv_shift);
						*dest_ptr=b8g8r8a8;
					}
					else {
						// GeneralsX @bugfix Copilot 24/08/2026 Generate vertical one-dimensional mip tails with the same box filter as two-dimensional levels.
						for (unsigned y=0;y<dest_surface_height/2;++y) {
							unsigned col0=src_ptr[y*2*src_surface_pitch];
							unsigned col1=src_ptr[(y*2+1)*src_surface_pitch];
							if (has_hsv_shift) {
								Recolor(col0,hsv_shift);
								Recolor(col1,hsv_shift);
							}
							dest_ptr[y*2*dest_surface_pitch]=col0;
							dest_ptr[(y*2+1)*dest_surface_pitch]=col1;
							src_ptr[y*src_surface_pitch]=Combine_A8R8G8B8(col0,col0,col1,col1);
						}
					}
				}
				else if (dest_surface_height==1) {
					// GeneralsX @bugfix Copilot 24/08/2026 Generate horizontal one-dimensional mip tails with the same box filter as two-dimensional levels.
					unsigned* dest_ptr=(unsigned*)dest_surface;
					unsigned* src_ptr=(unsigned*)src_surface;
					unsigned* mip_ptr=(unsigned*)src_surface;
					for (unsigned x=0;x<dest_surface_width/2;++x) {
						unsigned col0=*src_ptr++;
						unsigned col1=*src_ptr++;
						if (has_hsv_shift) {
							Recolor(col0,hsv_shift);
							Recolor(col1,hsv_shift);
						}
						*dest_ptr++=col0;
						*dest_ptr++=col1;
						*mip_ptr++=Combine_A8R8G8B8(col0,col1,col0,col1);
					}
				}
				else {
					for (unsigned y=0;y<dest_surface_height/2;++y) {
						unsigned* dest_ptr=(unsigned*)dest_surface;
						dest_ptr+=2*y*dest_surface_pitch;
						unsigned* src_ptr=(unsigned*)src_surface;
						unsigned* mip_ptr=src_ptr;
						src_ptr+=y*2*src_surface_pitch;
						mip_ptr+=y*src_surface_pitch;
						unsigned b8g8r8a8_00;
						unsigned b8g8r8a8_01;
						unsigned b8g8r8a8_10;
						unsigned b8g8r8a8_11;
						for (unsigned x=0;x<dest_surface_width/2;x++) {
							// Read four pixels from the source
							b8g8r8a8_10=src_ptr[src_surface_pitch];
							b8g8r8a8_00=*src_ptr++;
							b8g8r8a8_11=src_ptr[src_surface_pitch];
							b8g8r8a8_01=*src_ptr++;
							// Recolor if necessary
							if (has_hsv_shift) {
								Recolor(b8g8r8a8_00,hsv_shift);
								Recolor(b8g8r8a8_01,hsv_shift);
								Recolor(b8g8r8a8_10,hsv_shift);
								Recolor(b8g8r8a8_11,hsv_shift);
							}

							// Write the four pixels to the destination
							dest_ptr[dest_surface_pitch]=b8g8r8a8_10;
							*dest_ptr++=b8g8r8a8_00;
							dest_ptr[dest_surface_pitch]=b8g8r8a8_11;
							*dest_ptr++=b8g8r8a8_01;

							// Write combined four pixels to the destination mip map level
							*mip_ptr++=Combine_A8R8G8B8(b8g8r8a8_00,b8g8r8a8_01,b8g8r8a8_10,b8g8r8a8_11);
						}
					}
				}
			}
			else {
				for (unsigned y=0;y<dest_surface_height;++y) {
					unsigned* dest_ptr=(unsigned*)dest_surface;
					dest_ptr+=y*dest_surface_pitch;
					const unsigned* src_ptr=(unsigned*)src_surface;
					src_ptr+=y*src_surface_pitch;

					if (has_hsv_shift) {
						for (unsigned x=0;x<dest_surface_width;++x) {
							unsigned b8g8r8a8=*src_ptr++;
							Recolor(b8g8r8a8,hsv_shift);
							*dest_ptr++=b8g8r8a8;
						}
					}
					else {
						for (unsigned x=0;x<dest_surface_width;++x) {
							*dest_ptr++=*src_ptr++;
						}
					}
				}
			}
		}
		else {

			// For now do only point-sampling
			for (unsigned y=0;y<dest_surface_height;++y) {
				unsigned* dest_ptr=(unsigned*)dest_surface;
				dest_ptr+=y*dest_surface_pitch;
				unsigned src_y=y*src_surface_height/dest_surface_height;
				const unsigned* src_ptr=(unsigned*)src_surface;
				src_ptr+=src_y*src_surface_pitch;
				for (unsigned x=0;x<dest_surface_width;++x) {
					unsigned src_x=x*src_surface_width/dest_surface_width;
					unsigned b8g8r8a8=src_ptr[src_x];
					if (has_hsv_shift) {
						Recolor(b8g8r8a8,hsv_shift);
					}
					*dest_ptr++=b8g8r8a8;
				}
			}
		}
		return;
	}

	unsigned dest_bpp=Get_Bytes_Per_Pixel(dest_surface_format);
	unsigned src_bpp=Get_Bytes_Per_Pixel(src_surface_format);

	// One-to-one copy or scaling?
	if (dest_surface_width==src_surface_width && dest_surface_height==src_surface_height) {
		// Generate the next mip level while copying the current surface?
		if (generate_mip_level) {
			WWASSERT(src_surface_format!=WW3D_FORMAT_P8);	// Paletted textures can't be mipmapped
			if (dest_surface_width==1) {
				if (dest_surface_height==1) {
					unsigned b8g8r8a8;
					Read_B8G8R8A8(b8g8r8a8,src_surface,src_surface_format,src_palette,src_palette_bpp);
					if (has_hsv_shift) Recolor(b8g8r8a8,hsv_shift);
					Write_B8G8R8A8(dest_surface,dest_surface_format,b8g8r8a8);
				}
				else {
					// GeneralsX @bugfix Copilot 24/08/2026 Generate converted vertical one-dimensional mip tails without skipping their pixels.
					for (unsigned y=0;y<dest_surface_height/2;++y) {
						unsigned char* dest_ptr=dest_surface+y*2*dest_surface_pitch;
						unsigned char* src_ptr=src_surface+y*2*src_surface_pitch;
						unsigned char* mip_ptr=src_surface+y*src_surface_pitch;
						unsigned col0;
						unsigned col1;
						Read_B8G8R8A8(col0,src_ptr,src_surface_format,src_palette,src_palette_bpp);
						Read_B8G8R8A8(col1,src_ptr+src_surface_pitch,src_surface_format,src_palette,src_palette_bpp);
						if (has_hsv_shift) {
							Recolor(col0,hsv_shift);
							Recolor(col1,hsv_shift);
						}
						Write_B8G8R8A8(dest_ptr,dest_surface_format,col0);
						Write_B8G8R8A8(dest_ptr+dest_surface_pitch,dest_surface_format,col1);
						Write_B8G8R8A8(mip_ptr,src_surface_format,Combine_A8R8G8B8(col0,col0,col1,col1));
					}
				}
			}
			else if (dest_surface_height==1) {
				// GeneralsX @bugfix Copilot 24/08/2026 Generate converted horizontal one-dimensional mip tails without skipping their pixels.
				unsigned char* dest_ptr=dest_surface;
				unsigned char* src_ptr=src_surface;
				unsigned char* mip_ptr=src_surface;
				for (unsigned x=0;x<dest_surface_width/2;x++,dest_ptr+=dest_bpp*2,src_ptr+=src_bpp*2,mip_ptr+=src_bpp) {
					unsigned col0;
					unsigned col1;
					Read_B8G8R8A8(col0,src_ptr,src_surface_format,src_palette,src_palette_bpp);
					Read_B8G8R8A8(col1,src_ptr+src_bpp,src_surface_format,src_palette,src_palette_bpp);
					if (has_hsv_shift) {
						Recolor(col0,hsv_shift);
						Recolor(col1,hsv_shift);
					}
					Write_B8G8R8A8(dest_ptr,dest_surface_format,col0);
					Write_B8G8R8A8(dest_ptr+dest_bpp,dest_surface_format,col1);
					Write_B8G8R8A8(mip_ptr,src_surface_format,Combine_A8R8G8B8(col0,col1,col0,col1));
				}
			}
			else {
				for (unsigned y=0;y<dest_surface_height/2;++y) {
					unsigned char* dest_ptr=dest_surface+2*y*dest_surface_pitch;
					unsigned char* src_ptr=src_surface+y*2*src_surface_pitch;
					unsigned char* mip_ptr=src_surface+y*src_surface_pitch;
					unsigned b8g8r8a8_00;
					unsigned b8g8r8a8_01;
					unsigned b8g8r8a8_10;
					unsigned b8g8r8a8_11;
					for (unsigned x=0;x<dest_surface_width/2;x++,dest_ptr+=dest_bpp*2,src_ptr+=src_bpp*2,mip_ptr+=src_bpp) {
						// Read four pixels from the source
						Read_B8G8R8A8(b8g8r8a8_00,src_ptr,src_surface_format,src_palette,src_palette_bpp);
						Read_B8G8R8A8(b8g8r8a8_01,src_ptr+src_bpp,src_surface_format,src_palette,src_palette_bpp);
						Read_B8G8R8A8(b8g8r8a8_10,src_ptr+src_surface_pitch,src_surface_format,src_palette,src_palette_bpp);
						Read_B8G8R8A8(b8g8r8a8_11,src_ptr+src_bpp+src_surface_pitch,src_surface_format,src_palette,src_palette_bpp);

						// Recolor if necessary
						if (has_hsv_shift) {
							Recolor(b8g8r8a8_00,hsv_shift);
							Recolor(b8g8r8a8_01,hsv_shift);
							Recolor(b8g8r8a8_10,hsv_shift);
							Recolor(b8g8r8a8_11,hsv_shift);
						}

						// Write the four pixels to the destination
						Write_B8G8R8A8(dest_ptr,dest_surface_format,b8g8r8a8_00);
						Write_B8G8R8A8(dest_ptr+dest_bpp,dest_surface_format,b8g8r8a8_01);
						Write_B8G8R8A8(dest_ptr+dest_surface_pitch,dest_surface_format,b8g8r8a8_10);
						Write_B8G8R8A8(dest_ptr+dest_bpp+dest_surface_pitch,dest_surface_format,b8g8r8a8_11);

						// Write combined four pixels to the destination mip map level
						unsigned b8g8r8a8=Combine_A8R8G8B8(b8g8r8a8_00,b8g8r8a8_01,b8g8r8a8_10,b8g8r8a8_11);
						Write_B8G8R8A8(mip_ptr,src_surface_format,b8g8r8a8);
					}
				}
			}
		}
		else {
			for (unsigned y=0;y<dest_surface_height;++y) {
				unsigned char* dest_ptr=dest_surface+y*dest_surface_pitch;
				const unsigned char* src_ptr=src_surface+y*src_surface_pitch;
				if (has_hsv_shift) {
					for (unsigned x=0;x<dest_surface_width;++x,dest_ptr+=dest_bpp,src_ptr+=src_bpp) {
						Copy_Pixel(dest_ptr,dest_surface_format,src_ptr,src_surface_format,src_palette,src_palette_bpp,hsv_shift);
					}
				}
				else {
					for (unsigned x=0;x<dest_surface_width;++x,dest_ptr+=dest_bpp,src_ptr+=src_bpp) {
						Copy_Pixel(dest_ptr,dest_surface_format,src_ptr,src_surface_format,src_palette,src_palette_bpp);
					}
				}
			}
		}
	}
	else {

		// For now do only point-sampling
		for (unsigned y=0;y<dest_surface_height;++y) {
			unsigned char* dest_ptr=dest_surface+y*dest_surface_pitch;
			unsigned src_y=y*src_surface_height/dest_surface_height;
			const unsigned char* src_ptr=src_surface+src_y*src_surface_pitch;
			if (has_hsv_shift) {
				for (unsigned x=0;x<dest_surface_width;++x,dest_ptr+=dest_bpp) {
					unsigned src_x=x*src_surface_width/dest_surface_width;
					src_x*=src_bpp;
					Copy_Pixel(dest_ptr,dest_surface_format,src_ptr+src_x,src_surface_format,src_palette,src_palette_bpp,hsv_shift);
				}
			}
			else {
				for (unsigned x=0;x<dest_surface_width;++x,dest_ptr+=dest_bpp) {
					unsigned src_x=x*src_surface_width/dest_surface_width;
					src_x*=src_bpp;
					Copy_Pixel(dest_ptr,dest_surface_format,src_ptr+src_x,src_surface_format,src_palette,src_palette_bpp);
				}
			}
		}
	}
}
