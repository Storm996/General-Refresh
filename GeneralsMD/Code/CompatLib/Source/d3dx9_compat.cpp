// GLM_ENABLE_EXPERIMENTAL is passed via target_compile_definitions in CMakeLists.txt.
// Do NOT redefine it here -- that causes -Wmacro-redefined with Clang.

#include "d3dx9core.h"

// GeneralsX @build win64 11/10/2026 DXTn mip downsample below uses std::vector scratch buffers and
// memcpy/uint8_t fixed-width types.
#include <vector>
#include <cstring>
#include <cstdint>
#include <cctype>

// GeneralsX @build felipebraz 20/06/2025 GLI causes make_vec4 ambiguity with Apple Clang (GLM version mismatch).
// On macOS, exclude GLI and use stub implementations for the surface scaling path.
#ifndef __APPLE__
#include <gli/gli.hpp>
#include <gli/generate_mipmaps.hpp>
#endif

HRESULT WINAPID3DXGetErrorStringA(HRESULT hr, LPSTR pBuffer, UINT BufferLen)
{
	return D3DERR_INVALIDCALL;
}

HRESULT WINAPI
D3DXCreateTexture(LPDIRECT3DDEVICE9 pDevice,
	UINT Width,
	UINT Height,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DTEXTURE9 *ppTexture)
{
	// GeneralsX @bugfix fbraz 04/05/2026 Headless replay can request texture creation before DX8 device initialization.
	if (ppTexture == nullptr || pDevice == nullptr)
	{
		if (ppTexture != nullptr)
		{
			*ppTexture = nullptr;
		}

		return D3DERR_INVALIDCALL;
	}

	// GeneralsX @build d3d9 port 11/10/2026 - D3D9 CreateTexture takes an extra pSharedHandle param
	return pDevice->CreateTexture(Width, Height, MipLevels, Usage, Format, Pool, ppTexture, nullptr);
}

#include <iostream>

HRESULT WINAPI
D3DXCreateTextureFromFileExA(
	LPDIRECT3DDEVICE9 pDevice,
	LPCSTR pSrcFile,
	UINT Width,
	UINT Height,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,

	DWORD Filter,
	DWORD MipFilter,
	D3DCOLOR ColorKey,
	D3DXIMAGE_INFO *pSrcInfo,
	PALETTEENTRY *pPalette,

	LPDIRECT3DTEXTURE9 *ppTexture)
{
	HRESULT hr;

	return D3DERR_INVALIDCALL;
}

// GeneralsX @bugfix Antigravity 11/10/2026 win64: DXTn mip downsample support.
// Infantry (and other) diffuse maps are frequently stored as DXT2/3/5 compressed textures.
// D3DXFilterTexture generates each mip level by calling D3DXLoadSurfaceFromSurface(mip, ..., parent, ...)
// with dest = src/2. That downsample path only handled A4R4G4B4/R5G6B5 (manual box) and
// A8R8G8B8/X8R8G8B8/A1R5G5B5 (GLI). This bundled GLI has no BC/DXT decompress routines, so every
// DXT mip fell through to D3DERR_INVALIDCALL and was left uninitialized -> the whole texture rendered
// solid black once minified. Below we implement a full DXT1/2/3/4/5 -> RGBA decompress, a 2x2 box
// filter to half resolution, and a per-4x4-block average re-compress back into the source DXT format.
// Block-granular averaging is a valid (if soft) box-filter mip and, importantly, never black.
static inline bool IsDXTFormat(D3DFORMAT f)
{
	return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 ||
	       f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}
static inline bool DXTHasAlpha(D3DFORMAT f)    { return f != D3DFMT_DXT1; }          // 16-byte blocks
static inline bool DXTInterpAlpha(D3DFORMAT f) { return f == D3DFMT_DXT4 || f == D3DFMT_DXT5; }

static inline void Unpack565(unsigned short c, unsigned char &r, unsigned char &g, unsigned char &b)
{
	r = (unsigned char)((((c >> 11) & 0x1F) * 255 + 15) / 31);
	g = (unsigned char)((((c >> 5)  & 0x3F) * 255 + 31) / 63);
	b = (unsigned char)(((c & 0x1F) * 255 + 15) / 31);
}
static inline unsigned short Pack565(unsigned char r, unsigned char g, unsigned char b)
{
	return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// Decode an 8-byte DXT color sub-block (DXT1/3/5 color) to out[16] RGBA. When isDXT1 and c0<=c1 the
// 3-color + transparent-index-3 mode applies (index 3 -> fully transparent black).
static void DecompressColorSubblock(const unsigned char *block, unsigned char out[16][4], bool isDXT1)
{
	unsigned short c0 = (unsigned short)(block[0] | (block[1] << 8));
	unsigned short c1 = (unsigned short)(block[2] | (block[3] << 8));
	unsigned char col[4][3];
	Unpack565(c0, col[0][0], col[0][1], col[0][2]);
	Unpack565(c1, col[1][0], col[1][1], col[1][2]);
	bool threeColor = isDXT1 && (c0 <= c1);
	for (int k = 0; k < 3; ++k)
	{
		if (threeColor) { col[2][k] = (unsigned char)((col[0][k] + col[1][k]) / 2); col[3][k] = 0; }
		else            { col[2][k] = (unsigned char)((2 * col[0][k] + col[1][k]) / 3); col[3][k] = (unsigned char)((col[0][k] + 2 * col[1][k]) / 3); }
	}
	const unsigned char *idx = block + 4;
	for (int j = 0; j < 4; ++j)
	{
		unsigned int row = idx[j];
		for (int i = 0; i < 4; ++i)
		{
			unsigned int sel = (row >> (i * 2)) & 0x3;
			unsigned char *px = out[j * 4 + i];
			px[0] = col[sel][0];
			px[1] = col[sel][1];
			px[2] = col[sel][2];
			px[3] = 255;
			if (threeColor && sel == 3) { px[0] = px[1] = px[2] = 0; px[3] = 0; }
		}
	}
}

// DXT5/DXT4 8-alpha endpoint table (ab = first 8 bytes of the alpha sub-block).
static void DecompressAlpha58(const unsigned char *ab, unsigned char a[8])
{
	a[0] = ab[0];
	a[1] = ab[1];
	if (a[0] > a[1])
	{
		a[2] = (unsigned char)((6 * a[0] + 1 * a[1] + 3) / 7);
		a[3] = (unsigned char)((5 * a[0] + 2 * a[1] + 3) / 7);
		a[4] = (unsigned char)((4 * a[0] + 3 * a[1] + 3) / 7);
		a[5] = (unsigned char)((3 * a[0] + 4 * a[1] + 3) / 7);
		a[6] = (unsigned char)((2 * a[0] + 5 * a[1] + 3) / 7);
		a[7] = (unsigned char)((1 * a[0] + 6 * a[1] + 3) / 7);
	}
	else
	{
		a[2] = (unsigned char)((4 * a[0] + 1 * a[1] + 2) / 5);
		a[3] = (unsigned char)((3 * a[0] + 2 * a[1] + 2) / 5);
		a[4] = (unsigned char)((2 * a[0] + 3 * a[1] + 2) / 5);
		a[5] = (unsigned char)((1 * a[0] + 4 * a[1] + 2) / 5);
		a[6] = 0;
		a[7] = 255;
	}
}

// Decompress one full DXT block (fmt decides layout) to out[16] RGBA.
static void DecompressDXTBlock(const unsigned char *block, D3DFORMAT fmt, unsigned char out[16][4])
{
	if (fmt == D3DFMT_DXT1)
	{
		DecompressColorSubblock(block, out, true);
		return;
	}
	unsigned char alpha[16];
	const unsigned char *colorPtr = block + 8;
	if (!DXTInterpAlpha(fmt)) // DXT2/DXT3: 16 explicit 4-bit alpha nibbles
	{
		for (int t = 0; t < 16; ++t)
		{
			unsigned int byte = block[t / 2];
			unsigned int nib = (t & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
			alpha[t] = (unsigned char)(nib * 17);
		}
	}
	else // DXT4/DXT5: interpolated alpha (2 endpoints + 48-bit 3-bit selectors)
	{
		unsigned char a[8];
		DecompressAlpha58(block, a);
		unsigned long long bits = 0;
		for (int k = 0; k < 6; ++k) bits |= ((unsigned long long)block[2 + k]) << (8 * k);
		for (int t = 0; t < 16; ++t) alpha[t] = a[(bits >> (3 * t)) & 0x7];
	}
	DecompressColorSubblock(colorPtr, out, false);
	for (int t = 0; t < 16; ++t) out[t][3] = alpha[t];
}

// GeneralsX @bugfix d3d9 port 12/10/2026 d3dx8 decompressed a block-compressed surface on the fly
// whenever the destination format differed; the house-colour remapper depends on that, because
// recoloring is per-pixel CPU work and a DXT source has no pixels to touch. D3D9 has no such
// conversion, so decode the covered blocks here. Only 32-bit destinations are supported, which is
// what every caller in this tree asks for (Get_Valid_Texture_Format never returns 16-bit formats
// when the device depth is 32).
static HRESULT UnpackDXTTo32BitSurface(
	LPDIRECT3DSURFACE9 pDestSurface,
	CONST RECT *pDestRect,
	LPDIRECT3DSURFACE9 pSrcSurface,
	CONST RECT *pSrcRect,
	const D3DSURFACE_DESC &descSrc,
	const D3DSURFACE_DESC &descDest)
{
	if (descDest.Format != D3DFMT_A8R8G8B8 && descDest.Format != D3DFMT_X8R8G8B8) {
		return D3DERR_INVALIDCALL;
	}

	const LONG srcX = pSrcRect ? pSrcRect->left : 0;
	const LONG srcY = pSrcRect ? pSrcRect->top : 0;
	const LONG srcWidth = pSrcRect ? pSrcRect->right - pSrcRect->left : (LONG)descSrc.Width;
	const LONG srcHeight = pSrcRect ? pSrcRect->bottom - pSrcRect->top : (LONG)descSrc.Height;
	const LONG dstX = pDestRect ? pDestRect->left : 0;
	const LONG dstY = pDestRect ? pDestRect->top : 0;
	const LONG dstWidth = pDestRect ? pDestRect->right - pDestRect->left : (LONG)descDest.Width;
	const LONG dstHeight = pDestRect ? pDestRect->bottom - pDestRect->top : (LONG)descDest.Height;

	const LONG width = (srcWidth < dstWidth ? srcWidth : dstWidth);
	const LONG height = (srcHeight < dstHeight ? srcHeight : dstHeight);
	if (width <= 0 || height <= 0 || srcX < 0 || srcY < 0
		|| srcX + width > (LONG)descSrc.Width || srcY + height > (LONG)descSrc.Height
		|| dstX < 0 || dstY < 0
		|| dstX + width > (LONG)descDest.Width || dstY + height > (LONG)descDest.Height) {
		return D3DERR_INVALIDCALL;
	}

	D3DLOCKED_RECT sourceLock;
	if (FAILED(pSrcSurface->LockRect(&sourceLock, NULL, D3DLOCK_READONLY))) {
		return D3DERR_INVALIDCALL;
	}
	D3DLOCKED_RECT destLock;
	if (FAILED(pDestSurface->LockRect(&destLock, NULL, 0))) {
		pSrcSurface->UnlockRect();
		return D3DERR_INVALIDCALL;
	}

	const int bytesPerBlock = DXTHasAlpha(descSrc.Format) ? 16 : 8;
	const unsigned char *sourceBase = (const unsigned char *)sourceLock.pBits;
	unsigned char *destBase = (unsigned char *)destLock.pBits;
	const LONG firstBlockX = srcX / 4;
	const LONG lastBlockX = (srcX + width - 1) / 4;
	const LONG firstBlockY = srcY / 4;
	const LONG lastBlockY = (srcY + height - 1) / 4;

	for (LONG blockY = firstBlockY; blockY <= lastBlockY; ++blockY) {
		for (LONG blockX = firstBlockX; blockX <= lastBlockX; ++blockX) {
			unsigned char block[16][4];
			DecompressDXTBlock(
				sourceBase + (size_t)blockY * sourceLock.Pitch + (size_t)blockX * bytesPerBlock,
				descSrc.Format, block);

			for (int subY = 0; subY < 4; ++subY) {
				const LONG sy = blockY * 4 + subY;
				if (sy < srcY || sy >= srcY + height) continue;
				for (int subX = 0; subX < 4; ++subX) {
					const LONG sx = blockX * 4 + subX;
					if (sx < srcX || sx >= srcX + width) continue;
					const unsigned char *pixel = block[subY * 4 + subX];
					unsigned char *target = destBase
						+ (size_t)(dstY + sy - srcY) * destLock.Pitch
						+ (size_t)(dstX + sx - srcX) * 4;
					// A8R8G8B8/X8R8G8B8 store bytes in B,G,R,A order; block[][] is R,G,B,A.
					target[0] = pixel[2];
					target[1] = pixel[1];
					target[2] = pixel[0];
					target[3] = pixel[3];
				}
			}
		}
	}

	pDestSurface->UnlockRect();
	pSrcSurface->UnlockRect();
	return D3D_OK;
}

HRESULT WINAPI
D3DXLoadSurfaceFromSurface(
	LPDIRECT3DSURFACE9 pDestSurface,
	CONST PALETTEENTRY *pDestPalette,
	CONST RECT *pDestRect,
	LPDIRECT3DSURFACE9 pSrcSurface,
	CONST PALETTEENTRY *pSrcPalette,
	CONST RECT *pSrcRect,
	DWORD Filter,
	D3DCOLOR ColorKey)
{
	D3DSURFACE_DESC descSrc;
	D3DSURFACE_DESC descDest;

	//DEBUG_ASSERTCRASH(pDestPalette == NULL);

	pSrcSurface->GetDesc(&descSrc);
	pDestSurface->GetDesc(&descDest);

	// GeneralsX @feature Qoder 18/10/2026 wedge 9.J5: the surviving /GS fault on the no-d3d9 replay has no
	// symbolizable frame, so the last [VKD3DX] line before the dump names the call that was in flight.
	{
		static unsigned s_lsfCalls = 0;
		if (++s_lsfCalls <= 256u) {
			fprintf(stderr, "[VKD3DX] LSF #%u dst=%p %u x %u fmt=0x%X src=%p %u x %u fmt=0x%X f=0x%X\n",
				s_lsfCalls, (void *) pDestSurface, (unsigned) descDest.Width, (unsigned) descDest.Height,
				(unsigned) descDest.Format, (void *) pSrcSurface, (unsigned) descSrc.Width,
				(unsigned) descSrc.Height, (unsigned) descSrc.Format, (unsigned) Filter);
		}
	}

	if (descSrc.Format != descDest.Format)
	{
		if (IsDXTFormat(descSrc.Format) && !pSrcPalette && !pDestPalette) {
			return UnpackDXTTo32BitSurface(
				pDestSurface, pDestRect, pSrcSurface, pSrcRect, descSrc, descDest);
		}
		// Anything else still needs matching formats.
		return D3DERR_INVALIDCALL;
	}

	D3DLOCKED_RECT srcRect;
	pSrcSurface->LockRect(&srcRect, NULL, 0);

	D3DLOCKED_RECT destRect;
	pDestSurface->LockRect(&destRect, NULL, 0);

	// Fast path: No scaling needs to be done if the dimensions are the same
	if (descDest.Width == descSrc.Width && descDest.Height == descSrc.Height)
	{
		// GeneralsX @bugfix Antigravity 11/10/2026 The old fast path memcpy'd
		// srcRect.Pitch * descSrc.Height. For DXTn compressed surfaces the pitch is the
		// COMPRESSED block-row stride (e.g. 1024-wide DXT2 -> pitch 4096) but the row count
		// must be height/4 (block rows), so the old code copied 4x the real data and
		// overran the destination staging buffer by several MB on win64 (heap corruption,
		// AV in memcpy). Copy row by row over block rows instead.
		unsigned block_height = 1;
		switch (descSrc.Format)
		{
		case D3DFMT_DXT1:
		case D3DFMT_DXT2:
		case D3DFMT_DXT3:
		case D3DFMT_DXT4:
		case D3DFMT_DXT5:
			block_height = 4;
			break;
		default:
			break;
		}
		unsigned rows = (descSrc.Height + block_height - 1) / block_height;
		unsigned row_bytes = (unsigned)srcRect.Pitch < (unsigned)destRect.Pitch ? (unsigned)srcRect.Pitch : (unsigned)destRect.Pitch;
		for (unsigned y = 0; y < rows; ++y)
		{
			memcpy((char*)destRect.pBits + (size_t)y * destRect.Pitch,
			       (char*)srcRect.pBits + (size_t)y * srcRect.Pitch,
			       row_bytes);
		}
		pDestSurface->UnlockRect();
		pSrcSurface->UnlockRect();
		return D3D_OK;
	}

#ifndef __APPLE__
	// GeneralsX @bugfix Antigravity 11/10/2026 win64: DXTn mip downsample. See the helper comment
	// above the function. dest == src/2 (one mip step) for compressed sources: decompress -> 2x2 box
	// filter -> per-block average re-compress back to the same DXT format.
	if (IsDXTFormat(descSrc.Format) &&
		descDest.Width == descSrc.Width / 2 && descDest.Height == descSrc.Height / 2)
	{
		const D3DFORMAT fmt = descSrc.Format;
		const uint32_t sw = descSrc.Width, sh = descSrc.Height;
		const uint32_t dw = descDest.Width, dh = descDest.Height;
		const int bytesPerBlock = DXTHasAlpha(fmt) ? 16 : 8;

		// 1) Decompress the source mip into a full RGBA8888 scratch buffer.
		std::vector<unsigned char> srcRGBA((size_t)sw * sh * 4, 0);
		const unsigned char *sbase = (const unsigned char *)srcRect.pBits;
		const uint32_t sbc = (sw + 3) / 4, sbr = (sh + 3) / 4;
		for (uint32_t by = 0; by < sbr; ++by)
		{
			for (uint32_t bx = 0; bx < sbc; ++bx)
			{
				unsigned char blk[16][4];
				DecompressDXTBlock(sbase + (size_t)by * srcRect.Pitch + (size_t)bx * bytesPerBlock, fmt, blk);
				for (int py = 0; py < 4; ++py)
				{
					const uint32_t gy = by * 4 + py;
					if (gy >= sh) continue;
					for (int px = 0; px < 4; ++px)
					{
						const uint32_t gx = bx * 4 + px;
						if (gx >= sw) continue;
						unsigned char *d = &srcRGBA[((size_t)gy * sw + gx) * 4];
						const unsigned char *s = blk[py * 4 + px];
						d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = s[3];
					}
				}
			}
		}

		// 2) 2x2 box filter downsample to half-resolution RGBA.
		std::vector<unsigned char> dstRGBA((size_t)dw * dh * 4, 0);
		for (uint32_t y = 0; y < dh; ++y)
		{
			for (uint32_t x = 0; x < dw; ++x)
			{
				unsigned int acc[4] = { 0, 0, 0, 0 };
				unsigned int cnt = 0;
				for (int dy = 0; dy < 2; ++dy)
				{
					const uint32_t syp = y * 2 + dy;
					if (syp >= sh) continue;
					for (int dx = 0; dx < 2; ++dx)
					{
						const uint32_t sxp = x * 2 + dx;
						if (sxp >= sw) continue;
						const unsigned char *s = &srcRGBA[((size_t)syp * sw + sxp) * 4];
						for (int k = 0; k < 4; ++k) acc[k] += s[k];
						++cnt;
					}
				}
				unsigned char *d = &dstRGBA[((size_t)y * dw + x) * 4];
				for (int k = 0; k < 4; ++k) d[k] = cnt ? (unsigned char)(acc[k] / cnt) : 0;
			}
		}

		// 3) Re-compress the half-resolution RGBA into the destination DXT format, one averaged
		//    4x4 block at a time (flat endpoints, zero selectors => uniform block of the avg color).
		unsigned char *dbase = (unsigned char *)destRect.pBits;
		const uint32_t dbc = (dw + 3) / 4, dbr = (dh + 3) / 4;
		for (uint32_t by = 0; by < dbr; ++by)
		{
			for (uint32_t bx = 0; bx < dbc; ++bx)
			{
				unsigned long long sr = 0, sg = 0, sb = 0, sa = 0;
				unsigned int cnt = 0;
				for (int py = 0; py < 4; ++py)
				{
					const uint32_t gy = by * 4 + py;
					if (gy >= dh) continue;
					for (int px = 0; px < 4; ++px)
					{
						const uint32_t gx = bx * 4 + px;
						if (gx >= dw) continue;
						const unsigned char *s = &dstRGBA[((size_t)gy * dw + gx) * 4];
						sr += s[0]; sg += s[1]; sb += s[2]; sa += s[3];
						++cnt;
					}
				}
				if (cnt == 0) cnt = 1;
				const unsigned char ar = (unsigned char)(sr / cnt);
				const unsigned char ag = (unsigned char)(sg / cnt);
				const unsigned char abv = (unsigned char)(sb / cnt);
				const unsigned char aa = (unsigned char)(sa / cnt);

				unsigned char blk[16];
				memset(blk, 0, sizeof(blk));
				const unsigned short c = Pack565(ar, ag, abv);
				if (fmt == D3DFMT_DXT1)
				{
					blk[0] = (unsigned char)(c & 0xFF); blk[1] = (unsigned char)(c >> 8);
					blk[2] = (unsigned char)(c & 0xFF); blk[3] = (unsigned char)(c >> 8);
					// blk[4..7] selectors = 0 -> color0 (avg), opaque.
				}
				else if (!DXTInterpAlpha(fmt)) // DXT2/DXT3 explicit alpha
				{
					const unsigned char nib = (unsigned char)((aa * 15 + 128) / 255);
					for (int i = 0; i < 8; ++i) blk[i] = (unsigned char)(nib | (nib << 4));
					blk[8]  = (unsigned char)(c & 0xFF); blk[9]  = (unsigned char)(c >> 8);
					blk[10] = (unsigned char)(c & 0xFF); blk[11] = (unsigned char)(c >> 8);
					// blk[12..15] selectors = 0 -> color0 (avg).
				}
				else // DXT4/DXT5 interpolated alpha: alpha0 = alpha1 = avg -> selectors pick a0
				{
					blk[0] = aa; blk[1] = aa; // blk[2..7] selectors = 0
					blk[8]  = (unsigned char)(c & 0xFF); blk[9]  = (unsigned char)(c >> 8);
					blk[10] = (unsigned char)(c & 0xFF); blk[11] = (unsigned char)(c >> 8);
				}
				memcpy(dbase + (size_t)by * destRect.Pitch + (size_t)bx * bytesPerBlock, blk, bytesPerBlock);
			}
		}

		pDestSurface->UnlockRect();
		pSrcSurface->UnlockRect();
		return D3D_OK;
	}
	// GeneralsX @bugfix Antigravity 26/06/2026 Linux: GLI lacks support for A4R4G4B4/R5G6B5 formats.
	// We use manual box filter downsampling for these formats to fix black infantry rendering.
	if (descDest.Width == descSrc.Width / 2 && descDest.Height == descSrc.Height / 2)
	{
		if (descSrc.Format == D3DFMT_A4R4G4B4)
		{
			// Box filter for A4R4G4B4: average 2x2 blocks
			const uint16_t *src = (const uint16_t *)srcRect.pBits;
			uint16_t *dst = (uint16_t *)destRect.pBits;
			uint32_t srcPitch16 = srcRect.Pitch / 2;
			uint32_t dstPitch16 = destRect.Pitch / 2;

			for (uint32_t y = 0; y < descDest.Height; y++)
			{
				for (uint32_t x = 0; x < descDest.Width; x++)
				{
					uint32_t sx = x * 2;
					uint32_t sy = y * 2;
					uint16_t p00 = src[sy * srcPitch16 + sx];
					uint16_t p10 = src[sy * srcPitch16 + sx + 1];
					uint16_t p01 = src[(sy + 1) * srcPitch16 + sx];
					uint16_t p11 = src[(sy + 1) * srcPitch16 + sx + 1];

					// Extract and average each channel (A4 R4 G4 B4)
					uint32_t a = (((p00 >> 12) & 0x0F) + ((p10 >> 12) & 0x0F) +
					              ((p01 >> 12) & 0x0F) + ((p11 >> 12) & 0x0F) + 2) >> 2;
					uint32_t r = (((p00 >> 8) & 0x0F) + ((p10 >> 8) & 0x0F) +
					              ((p01 >> 8) & 0x0F) + ((p11 >> 8) & 0x0F) + 2) >> 2;
					uint32_t g = (((p00 >> 4) & 0x0F) + ((p10 >> 4) & 0x0F) +
					              ((p01 >> 4) & 0x0F) + ((p11 >> 4) & 0x0F) + 2) >> 2;
					uint32_t b = ((p00 & 0x0F) + (p10 & 0x0F) +
					              (p01 & 0x0F) + (p11 & 0x0F) + 2) >> 2;

					dst[y * dstPitch16 + x] = (uint16_t)((a << 12) | (r << 8) | (g << 4) | b);
				}
			}
			pDestSurface->UnlockRect();
			pSrcSurface->UnlockRect();
			return D3D_OK;
		}
		else if (descSrc.Format == D3DFMT_R5G6B5)
		{
			// Box filter for R5G6B5: average 2x2 blocks
			const uint16_t *src = (const uint16_t *)srcRect.pBits;
			uint16_t *dst = (uint16_t *)destRect.pBits;
			uint32_t srcPitch16 = srcRect.Pitch / 2;
			uint32_t dstPitch16 = destRect.Pitch / 2;

			for (uint32_t y = 0; y < descDest.Height; y++)
			{
				for (uint32_t x = 0; x < descDest.Width; x++)
				{
					uint32_t sx = x * 2;
					uint32_t sy = y * 2;
					uint16_t p00 = src[sy * srcPitch16 + sx];
					uint16_t p10 = src[sy * srcPitch16 + sx + 1];
					uint16_t p01 = src[(sy + 1) * srcPitch16 + sx];
					uint16_t p11 = src[(sy + 1) * srcPitch16 + sx + 1];

					// Extract and average each channel (R5 G6 B5)
					uint32_t r = (((p00 >> 11) & 0x1F) + ((p11 >> 11) & 0x1F) +
					              ((p01 >> 11) & 0x1F) + ((p10 >> 11) & 0x1F) + 2) >> 2;
					uint32_t g = (((p00 >> 5) & 0x3F) + ((p11 >> 5) & 0x3F) +
					              ((p01 >> 5) & 0x3F) + ((p10 >> 5) & 0x3F) + 2) >> 2;
					uint32_t b = ((p00 & 0x1F) + (p11 & 0x1F) +
					              (p01 & 0x1F) + (p10 & 0x1F) + 2) >> 2;

					dst[y * dstPitch16 + x] = (uint16_t)((r << 11) | (g << 5) | b);
				}
			}
			pDestSurface->UnlockRect();
			pSrcSurface->UnlockRect();
			return D3D_OK;
		}
	}
	// Pick a compatible format
	gli::format imageFormat = gli::format::FORMAT_RGBA8_UNORM_PACK8;

	assert(descSrc.Format == D3DFMT_A8R8G8B8 || descSrc.Format == D3DFMT_A1R5G5B5 || descSrc.Format == D3DFMT_X8R8G8B8);
	if (descSrc.Format == D3DFMT_A8R8G8B8 || descSrc.Format == D3DFMT_X8R8G8B8)
	{
		imageFormat = gli::format::FORMAT_RGBA8_UNORM_PACK8;	
	}
	else if (descSrc.Format == D3DFMT_A1R5G5B5)
	{
		imageFormat = gli::format::FORMAT_A1RGB5_UNORM_PACK16;
	}
	else
	{
		unsigned f = (unsigned)descSrc.Format;
		char cc[5] = { (char)(f & 0xFF), (char)((f >> 8) & 0xFF), (char)((f >> 16) & 0xFF), (char)((f >> 24) & 0xFF), 0 };
		fprintf(stderr, "[MIPTX] UNSUPPORTED mip fmt=0x%08X '%s' -> INVALIDCALL (renders black)\n", f, cc);
		fflush(stderr);
		return D3DERR_INVALIDCALL;
	}

	// Create two levels of mips, 0 and 1
	gli::texture2d texSrc(imageFormat, gli::extent2d(descSrc.Width, descSrc.Height), 2);

	// Copy the data to level 0
	// GeneralsX @bugfix d3d9 port 18/10/2026 win64: texSrc holds TWO levels, so size() returned
	// level0+level1 (5/4 of the source) and the memcpy read past the locked srcRect mirror into the
	// heap guard page - the 0xc0000005 "read of <heap base+size>" faults in D3DXFilterTexture.
	memcpy(texSrc.data(), srcRect.pBits, texSrc.size(0));
	// Generate mip 1 from level 0
	gli::texture2d mipMap = gli::generate_mipmaps(texSrc, gli::filter::FILTER_LINEAR);

	if (mipMap.size(1) != destRect.Pitch * descDest.Height)
	{
		// The generated dimension would not be the same as the destination
		// This does not happen in the game, yet let's not allow it
		pDestSurface->UnlockRect();
		pSrcSurface->UnlockRect();
		return D3DERR_INVALIDCALL;
	}

	// Copy mip level 1 to the destination
	memcpy(destRect.pBits, mipMap.data(0,0,1), destRect.Pitch * descDest.Height);

	pDestSurface->UnlockRect();
	pSrcSurface->UnlockRect();

	return D3D_OK;
#else
	// GeneralsX @bugfix BenderAI 07/03/2026 macOS: GLI not available due to Apple Clang ambiguity.
	// Implement manual box filter downsampling for mipmap generation.
	// This is critical for terrain textures - without mipmaps, terrain renders black.
	if (descDest.Width == descSrc.Width / 2 && descDest.Height == descSrc.Height / 2)
	{
		if (descSrc.Format == D3DFMT_A1R5G5B5)
		{
			// Box filter for A1R5G5B5: average 2x2 blocks
			const uint16_t *src = (const uint16_t *)srcRect.pBits;
			uint16_t *dst = (uint16_t *)destRect.pBits;
			uint32_t srcPitch16 = srcRect.Pitch / 2;
			uint32_t dstPitch16 = destRect.Pitch / 2;

			for (uint32_t y = 0; y < descDest.Height; y++)
			{
				for (uint32_t x = 0; x < descDest.Width; x++)
				{
					uint32_t sx = x * 2;
					uint32_t sy = y * 2;
					uint16_t p00 = src[sy * srcPitch16 + sx];
					uint16_t p10 = src[sy * srcPitch16 + sx + 1];
					uint16_t p01 = src[(sy + 1) * srcPitch16 + sx];
					uint16_t p11 = src[(sy + 1) * srcPitch16 + sx + 1];

					// Extract and average each channel (A1 R5 G5 B5)
					uint32_t r = (((p00 >> 10) & 0x1F) + ((p10 >> 10) & 0x1F) +
					              ((p01 >> 10) & 0x1F) + ((p11 >> 10) & 0x1F) + 2) >> 2;
					uint32_t g = (((p00 >> 5) & 0x1F) + ((p10 >> 5) & 0x1F) +
					              ((p01 >> 5) & 0x1F) + ((p11 >> 5) & 0x1F) + 2) >> 2;
					uint32_t b = ((p00 & 0x1F) + (p10 & 0x1F) +
					              (p01 & 0x1F) + (p11 & 0x1F) + 2) >> 2;
					// Alpha: majority vote (set if 2+ pixels have alpha set)
					uint32_t a = ((p00 >> 15) + (p10 >> 15) + (p01 >> 15) + (p11 >> 15)) >= 2 ? 1 : 0;

					dst[y * dstPitch16 + x] = (uint16_t)((a << 15) | (r << 10) | (g << 5) | b);
				}
			}
		}
		else if (descSrc.Format == D3DFMT_A8R8G8B8 || descSrc.Format == D3DFMT_X8R8G8B8)
		{
			// Box filter for A8R8G8B8/X8R8G8B8: average 2x2 blocks
			const uint32_t *src = (const uint32_t *)srcRect.pBits;
			uint32_t *dst = (uint32_t *)destRect.pBits;
			uint32_t srcPitch32 = srcRect.Pitch / 4;
			uint32_t dstPitch32 = destRect.Pitch / 4;

			for (uint32_t y = 0; y < descDest.Height; y++)
			{
				for (uint32_t x = 0; x < descDest.Width; x++)
				{
					uint32_t sx = x * 2;
					uint32_t sy = y * 2;
					uint32_t p00 = src[sy * srcPitch32 + sx];
					uint32_t p10 = src[sy * srcPitch32 + sx + 1];
					uint32_t p01 = src[(sy + 1) * srcPitch32 + sx];
					uint32_t p11 = src[(sy + 1) * srcPitch32 + sx + 1];

					uint32_t a = (((p00 >> 24) & 0xFF) + ((p10 >> 24) & 0xFF) +
					              ((p01 >> 24) & 0xFF) + ((p11 >> 24) & 0xFF) + 2) >> 2;
					uint32_t r = (((p00 >> 16) & 0xFF) + ((p10 >> 16) & 0xFF) +
					              ((p01 >> 16) & 0xFF) + ((p11 >> 16) & 0xFF) + 2) >> 2;
					uint32_t g = (((p00 >> 8) & 0xFF) + ((p10 >> 8) & 0xFF) +
					              ((p01 >> 8) & 0xFF) + ((p11 >> 8) & 0xFF) + 2) >> 2;
					uint32_t b = ((p00 & 0xFF) + (p10 & 0xFF) +
					              (p01 & 0xFF) + (p11 & 0xFF) + 2) >> 2;

					dst[y * dstPitch32 + x] = (a << 24) | (r << 16) | (g << 8) | b;
				}
			}
		}
		else if (descSrc.Format == D3DFMT_A4R4G4B4)
		{
			// Box filter for A4R4G4B4: average 2x2 blocks
			const uint16_t *src = (const uint16_t *)srcRect.pBits;
			uint16_t *dst = (uint16_t *)destRect.pBits;
			uint32_t srcPitch16 = srcRect.Pitch / 2;
			uint32_t dstPitch16 = destRect.Pitch / 2;

			for (uint32_t y = 0; y < descDest.Height; y++)
			{
				for (uint32_t x = 0; x < descDest.Width; x++)
				{
					uint32_t sx = x * 2;
					uint32_t sy = y * 2;
					uint16_t p00 = src[sy * srcPitch16 + sx];
					uint16_t p10 = src[sy * srcPitch16 + sx + 1];
					uint16_t p01 = src[(sy + 1) * srcPitch16 + sx];
					uint16_t p11 = src[(sy + 1) * srcPitch16 + sx + 1];

					// Extract and average each channel (A4 R4 G4 B4)
					uint32_t a = (((p00 >> 12) & 0x0F) + ((p10 >> 12) & 0x0F) +
					              ((p01 >> 12) & 0x0F) + ((p11 >> 12) & 0x0F) + 2) >> 2;
					uint32_t r = (((p00 >> 8) & 0x0F) + ((p10 >> 8) & 0x0F) +
					              ((p01 >> 8) & 0x0F) + ((p11 >> 8) & 0x0F) + 2) >> 2;
					uint32_t g = (((p00 >> 4) & 0x0F) + ((p10 >> 4) & 0x0F) +
					              ((p01 >> 4) & 0x0F) + ((p11 >> 4) & 0x0F) + 2) >> 2;
					uint32_t b = ((p00 & 0x0F) + (p10 & 0x0F) +
					              (p01 & 0x0F) + (p11 & 0x0F) + 2) >> 2;

					dst[y * dstPitch16 + x] = (uint16_t)((a << 12) | (r << 8) | (g << 4) | b);
				}
			}
		}
		else if (descSrc.Format == D3DFMT_R5G6B5)
		{
			// Box filter for R5G6B5: average 2x2 blocks
			const uint16_t *src = (const uint16_t *)srcRect.pBits;
			uint16_t *dst = (uint16_t *)destRect.pBits;
			uint32_t srcPitch16 = srcRect.Pitch / 2;
			uint32_t dstPitch16 = destRect.Pitch / 2;

			for (uint32_t y = 0; y < descDest.Height; y++)
			{
				for (uint32_t x = 0; x < descDest.Width; x++)
				{
					uint32_t sx = x * 2;
					uint32_t sy = y * 2;
					uint16_t p00 = src[sy * srcPitch16 + sx];
					uint16_t p10 = src[sy * srcPitch16 + sx + 1];
					uint16_t p01 = src[(sy + 1) * srcPitch16 + sx];
					uint16_t p11 = src[(sy + 1) * srcPitch16 + sx + 1];

					// Extract and average each channel (R5 G6 B5)
					uint32_t r = (((p00 >> 11) & 0x1F) + ((p11 >> 11) & 0x1F) +
					              ((p01 >> 11) & 0x1F) + ((p10 >> 11) & 0x1F) + 2) >> 2;
					uint32_t g = (((p00 >> 5) & 0x3F) + ((p11 >> 5) & 0x3F) +
					              ((p01 >> 5) & 0x3F) + ((p10 >> 5) & 0x3F) + 2) >> 2;
					uint32_t b = ((p00 & 0x1F) + (p11 & 0x1F) +
					              (p01 & 0x1F) + (p10 & 0x1F) + 2) >> 2;

					dst[y * dstPitch16 + x] = (uint16_t)((r << 11) | (g << 5) | b);
				}
			}
		}
		else
		{
			pDestSurface->UnlockRect();
			pSrcSurface->UnlockRect();
			return D3DERR_INVALIDCALL;
		}

		pDestSurface->UnlockRect();
		pSrcSurface->UnlockRect();
		return D3D_OK;
	}

	// Non-power-of-two scaling not supported
	pDestSurface->UnlockRect();
	pSrcSurface->UnlockRect();
	return D3DERR_INVALIDCALL;
#endif
}

HRESULT WINAPI
D3DXGetErrorStringA(
	HRESULT hr,
	LPSTR pBuffer,
	UINT BufferLen)
{
	return D3DERR_INVALIDCALL;
}

// Taken and adopted from Wine 3.21
HRESULT WINAPI
D3DXFilterTexture(
	LPDIRECT3DBASETEXTURE9 pBaseTexture,
	CONST PALETTEENTRY *pPalette,
	UINT SrcLevel,
	DWORD Filter)
{
	HRESULT hr = D3DERR_INVALIDCALL;
	if (SrcLevel == D3DX_DEFAULT)
	{
		SrcLevel = 0;
	}
	else if (SrcLevel >= pBaseTexture->GetLevelCount())
	{
		return D3DERR_INVALIDCALL;
	}

	D3DRESOURCETYPE type;
	switch (type = pBaseTexture->GetType())
	{
		case D3DRTYPE_TEXTURE:
		{
			IDirect3DTexture9 *tex = (IDirect3DTexture9 *)pBaseTexture;
			IDirect3DSurface9 *topsurf, *mipsurf;
			D3DSURFACE_DESC desc;
			int i;

			tex->GetLevelDesc(SrcLevel, &desc);
			{
				// Wedge 9.J5 pair to the LSF trace: names the mip-churn loop this call is entering.
				static unsigned s_filtCalls = 0;
				if (++s_filtCalls <= 256u) {
					fprintf(stderr, "[VKD3DX] FILT #%u tex=%p lvl=%u src=%u x %u nlevels=%u f=0x%X\n",
						s_filtCalls, (void *) pBaseTexture, (unsigned) SrcLevel, (unsigned) desc.Width,
						(unsigned) desc.Height, (unsigned) tex->GetLevelCount(), (unsigned) Filter);
				}
			}
			if (Filter == D3DX_DEFAULT)
			{
				Filter = D3DX_FILTER_BOX;
			}

			int Level = SrcLevel + 1;
			hr = tex->GetSurfaceLevel(SrcLevel, &topsurf);
			if (FAILED(hr))
			{
				return hr;
			}

			while (tex->GetSurfaceLevel(Level, &mipsurf) == D3D_OK)
			{
				// Copy the data
				D3DXLoadSurfaceFromSurface(mipsurf, NULL, NULL, topsurf, NULL, NULL, Filter, 0);

				// Release the surface
				mipsurf->Release();
				topsurf = mipsurf;

				Level++;
			}

			topsurf->Release();
			if (FAILED(hr))
			{
				return hr;
			}
		}
		return D3D_OK;
	}

	return D3D_OK;
}

HRESULT WINAPI
D3DXCreateCubeTexture(
	LPDIRECT3DDEVICE9 pDevice,
	UINT Size,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DCUBETEXTURE9 *ppCubeTexture)
{
	// Also unused by the game
	return D3DERR_INVALIDCALL;
}

HRESULT WINAPI
D3DXCreateVolumeTexture(
	LPDIRECT3DDEVICE9 pDevice,
	UINT Width,
	UINT Height,
	UINT Depth,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DVOLUMETEXTURE9 *ppVolumeTexture)
{
	// Used to create volume textures by the texture loader
	// Actually not used in the game
	return D3DERR_INVALIDCALL;
}

// GeneralsX @build d3d9 port 12/10/2026 ---------------------------------------------------------
// D3DXAssembleShader - D3D9 replacement for the DX8 d3dx8 assembler.
//
// D3D9 has no d3dx8-style assembler: D3DCompile only accepts HLSL and outright rejects the DX8
// assembly dialect ("vs.1.1"/"ps.1.4" headers -> error X3539). Every runtime-compiled shader in this
// engine therefore carries HLSL source, and this shim compiles it with the operating system's
// d3dcompiler (d3dcompiler_47.dll ships in System32 on every supported Windows) and returns the DXBC
// blob IDirect3DDevice9::CreateVertexShader/CreatePixelShader expects.
//
// Source contract (all callers pass in-repo strings):
//   // d3d-target: vs_2_0     required; D3D9 profile token, scanned in the first 16 lines
//   // d3d-entry:  main       optional; defaults to "main"
// Both are plain HLSL comments, so the same text also compiles offline with fxc.
//
// The DX8 "Flags" (assembler flags) and "ppConstants" (DX8 constant table) arguments have no D3D9
// counterpart and stay unused: DXBC carries its own reflection data, and the engine binds uniforms
// through SetVertexShaderConstantF/SetPixelShaderConstantF onto explicit register() slots.
//=================================================================================
#ifdef _WIN32
#include <d3dcompiler.h>
#endif
#include <new>

namespace
{
	// Refcounted ID3DXBuffer holding the compiled blob (or the error text). Created and released on
	// the render thread by the shader setup paths, so a plain refcount is sufficient.
	class CompatShaderBuffer : public ID3DXBuffer
	{
	public:
		CompatShaderBuffer(const void* src, SIZE_T size) : m_refCount(1)
		{
			if (src != nullptr && size > 0)
			{
				m_bytes.assign((const unsigned char*)src, (const unsigned char*)src + size);
			}
		}

		void* STDMETHODCALLTYPE GetBufferPointer() override
		{
			return m_bytes.empty() ? nullptr : m_bytes.data();
		}

		SIZE_T STDMETHODCALLTYPE GetBufferSize() override
		{
			return m_bytes.size();
		}

		ULONG STDMETHODCALLTYPE AddRef() override
		{
			return (ULONG)++m_refCount;
		}

		ULONG STDMETHODCALLTYPE Release() override
		{
			const LONG remaining = --m_refCount;
			if (remaining == 0)
			{
				delete this;
				return 0;
			}
			return (ULONG)remaining;
		}

	private:
		~CompatShaderBuffer() {}

		LONG m_refCount;
		std::vector<unsigned char> m_bytes;
	};

	static LPD3DXBUFFER Make_Shader_Buffer(const void* src, SIZE_T size)
	{
		return new (std::nothrow) CompatShaderBuffer(src, size);
	}

	static bool Is_Same_Prefix(const char* text, const char* key)
	{
		while (*key != 0)
		{
			if (toupper((unsigned char)*text) != toupper((unsigned char)*key)) return false;
			++text;
			++key;
		}
		return true;
	}

	// Read a "// <key>: <value>" directive from the first lines of the shader source.
	static bool Read_Shader_Directive(const char* src, SIZE_T len, const char* key, char* out, SIZE_T outLen)
	{
		SIZE_T pos = 0;
		for (unsigned line = 0; line < 16 && pos < len; ++line)
		{
			SIZE_T end = pos;
			while (end < len && src[end] != '\n') ++end;

			SIZE_T s = pos;
			while (s < end && (src[s] == ' ' || src[s] == '\t' || src[s] == '\r')) ++s;

			if (end - s >= 2 && src[s] == '/' && src[s + 1] == '/')
			{
				SIZE_T c = s + 2;
				while (c < end && (src[c] == ' ' || src[c] == '\t')) ++c;
				if (Is_Same_Prefix(src + c, key) && c + strlen(key) < end && src[c + strlen(key)] == ':')
				{
					SIZE_T v = c + strlen(key) + 1;
					while (v < end && (src[v] == ' ' || src[v] == '\t')) ++v;
					SIZE_T vEnd = end;
					while (vEnd > v && (src[vEnd - 1] == ' ' || src[vEnd - 1] == '\t' || src[vEnd - 1] == '\r')) --vEnd;
					if (vEnd - v > 0 && (SIZE_T)(vEnd - v) < outLen)
					{
						memcpy(out, src + v, vEnd - v);
						out[vEnd - v] = 0;
						return true;
					}
				}
			}
			pos = end + 1;
		}
		return false;
	}

#ifdef _WIN32
	typedef HRESULT(WINAPI* D3DCompileProc)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*,
		ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

	static D3DCompileProc Get_D3DCompile()
	{
		static D3DCompileProc proc = nullptr;
		static bool attempted = false;
		if (!attempted)
		{
			attempted = true;
			HMODULE module = LoadLibraryA("d3dcompiler_47.dll");
			if (module == nullptr) module = LoadLibraryA("d3dcompiler.dll");
			if (module != nullptr)
			{
				proc = (D3DCompileProc)GetProcAddress(module, "D3DCompile");
			}
		}
		return proc;
	}
#endif
}

HRESULT WINAPI
D3DXAssembleShader(
	LPCVOID pSrcData,
	UINT SrcDataLen,
	DWORD Flags,
	LPD3DXBUFFER *ppConstants,
	LPD3DXBUFFER *ppCompiledShader,
	LPD3DXBUFFER *ppCompilationErrors)
{
	// Callers: dot3 normal mapping, the inline water pixel shaders, profiler frame capture. Each one
	// guards on SUCCEEDED() and keeps running without the shader, so failing here degrades gracefully
	// - but the reason must still be reachable, hence the error buffer the callers print.
	(void)Flags;
	(void)ppConstants;

	if (ppCompiledShader != nullptr) *ppCompiledShader = nullptr;
	if (ppCompilationErrors != nullptr) *ppCompilationErrors = nullptr;

	const char* reason = nullptr;
#ifdef _WIN32
	D3DCompileProc compile = Get_D3DCompile();
	if (compile == nullptr)
	{
		reason = "d3dcompiler_47.dll could not be loaded";
	}
	else if (pSrcData == nullptr || SrcDataLen == 0)
	{
		reason = "empty shader source";
	}
	else
	{
		char target[32];
		char entry[32];
		if (!Read_Shader_Directive((const char*)pSrcData, SrcDataLen, "d3d-target", target, sizeof(target)))
		{
			reason = "missing \"// d3d-target: <profile>\" directive (D3D9 does not accept DX8 assembly)";
		}
		else if (!Read_Shader_Directive((const char*)pSrcData, SrcDataLen, "d3d-entry", entry, sizeof(entry)))
		{
			snprintf(entry, sizeof(entry), "main");
		}

		if (reason == nullptr)
		{
			ID3DBlob* code = nullptr;
			ID3DBlob* errors = nullptr;
			const HRESULT hr = compile(pSrcData, SrcDataLen, "<runtime>", nullptr, nullptr, entry, target,
				D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR, 0, &code, &errors);
			if (SUCCEEDED(hr) && code != nullptr)
			{
				LPD3DXBUFFER wrapped = Make_Shader_Buffer(code->GetBufferPointer(), code->GetBufferSize());
				code->Release();
				if (wrapped != nullptr && ppCompiledShader != nullptr)
				{
					*ppCompiledShader = wrapped;
					if (errors != nullptr) errors->Release();
					return S_OK;
				}
				if (wrapped != nullptr) wrapped->Release();
				reason = "out of memory wrapping the compiled shader";
			}
			else
			{
				if (errors != nullptr && ppCompilationErrors != nullptr && errors->GetBufferSize() > 1)
				{
					LPD3DXBUFFER text = Make_Shader_Buffer(errors->GetBufferPointer(), errors->GetBufferSize());
					if (text != nullptr) *ppCompilationErrors = text;
				}
				if (errors != nullptr) errors->Release();
				if (code != nullptr) code->Release();
				if (reason == nullptr) reason = "D3DCompile rejected the source";
			}
		}
	}
#else
	reason = "runtime shader compilation is Windows-only (no D3DCompile on this platform)";
#endif

	if (ppCompilationErrors != nullptr && *ppCompilationErrors == nullptr)
	{
		*ppCompilationErrors = Make_Shader_Buffer(reason, strlen(reason) + 1);
	}
	return D3DERR_INVALIDCALL;
}

// Taken from Wine at ca46949
static UINT Get_TexCoord_Size_From_FVF(DWORD FVF, int tex_num)
{
    return (((((FVF) >> (16 + (2 * (tex_num)))) + 1) & 0x03) + 1);
}

UINT WINAPI D3DXGetFVFVertexSize(DWORD FVF)
{
    DWORD size = 0;
    UINT i;
    UINT numTextures = (FVF & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;

    if (FVF & D3DFVF_NORMAL) size += sizeof(D3DVECTOR);
    if (FVF & D3DFVF_DIFFUSE) size += sizeof(DWORD);
    if (FVF & D3DFVF_SPECULAR) size += sizeof(DWORD);
    if (FVF & D3DFVF_PSIZE) size += sizeof(DWORD);

    switch (FVF & D3DFVF_POSITION_MASK)
    {
        case D3DFVF_XYZ:    size += sizeof(D3DVECTOR); break;
        case D3DFVF_XYZRHW: size += 4 * sizeof(FLOAT); break;
        case D3DFVF_XYZB1:  size += 4 * sizeof(FLOAT); break;
        case D3DFVF_XYZB2:  size += 5 * sizeof(FLOAT); break;
        case D3DFVF_XYZB3:  size += 6 * sizeof(FLOAT); break;
        case D3DFVF_XYZB4:  size += 7 * sizeof(FLOAT); break;
        case D3DFVF_XYZB5:  size += 8 * sizeof(FLOAT); break;
		// Not present in D3D8, thus uncommented
        //case D3DFVF_XYZW:   size += 4 * sizeof(FLOAT); break;
    }

    for (i = 0; i < numTextures; i++)
    {
        size += Get_TexCoord_Size_From_FVF(FVF, i) * sizeof(FLOAT);
    }

    return size;
}
