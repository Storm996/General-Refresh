#pragma once

#ifdef _WIN32
#include <windows.h>
#else
// CRITICAL: windows_compat must be included BEFORE d3d8.h so Win32 types (DWORD, FLOAT, etc.) are defined
#include "windows_compat.h"
#include "com_compat.h"  // COM/DirectX macros (DECLARE_INTERFACE_, STDMETHOD, etc.) - needed for d3d8.h
#endif

// Now d3d8.h can find DWORD, FLOAT, UINT, etc.
#include <d3d9.h>

#define D3DX_DEFAULT                     UINT_MAX

// GeneralsX @build d3d9 port 12/10/2026 - minimal buffer object returned by the D3DXAssembleShader
// shim: it now wraps D3DCompile's DXBC output (or the compiler's error text) so the DX8-shaped
// callers keep working. Defined before the extern "C" block so the shader-assembly prototype below
// can name LPD3DXBUFFER.
struct ID3DXBuffer
{
	virtual void* STDMETHODCALLTYPE GetBufferPointer() = 0;
	virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(void* /*REFIID*/, void** ppvObject) { *ppvObject = nullptr; return E_NOINTERFACE; }
	virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
	virtual ULONG STDMETHODCALLTYPE Release() = 0;
};
typedef ID3DXBuffer *LPD3DXBUFFER;

#ifdef __cplusplus
extern "C"
{
#endif
typedef enum eD3DXFilters
{
    D3DX_FILTER_NONE,
    D3DX_FILTER_TRIANGLE,
    D3DX_FILTER_BOX,
} eD3DXFilters;

// These defines belong into d3d8 of DXVK-Native
#define D3DPRESENT_RATE_DEFAULT          0x00000000
#define D3DSGR_CALIBRATE                 0x00000001
#define D3DSGR_NO_CALIBRATION            0x00000002

// Blatantly ripped from Wine:
HRESULT WINAPID3DXGetErrorStringA(HRESULT hr, LPSTR pBuffer, UINT BufferLen);

HRESULT WINAPI
D3DXCreateTexture(LPDIRECT3DDEVICE9 pDevice,
	UINT Width,
	UINT Height,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DTEXTURE9 *ppTexture);

typedef enum _D3DXIMAGE_FILEFORMAT
{
    D3DXIFF_BMP = 0,
    D3DXIFF_JPG = 1,
    D3DXIFF_TGA = 2,
    D3DXIFF_PNG = 3,
    D3DXIFF_DDS = 4,
    D3DXIFF_PPM = 5,
    D3DXIFF_DIB = 6,
    D3DXIFF_HDR = 7,
    D3DXIFF_PFM = 8,
    D3DXIFF_FORCE_DWORD = 0x7fffffff
} D3DXIMAGE_FILEFORMAT;

typedef struct D3DXIMAGE_INFO
{
    UINT Width;
    UINT Height;
    UINT Depth;
    UINT MipLevels;
    D3DFORMAT Format;
    D3DRESOURCETYPE ResourceType;
    D3DXIMAGE_FILEFORMAT ImageFileFormat;
} D3DXIMAGE_INFO;

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

	LPDIRECT3DTEXTURE9 *ppTexture);

HRESULT WINAPI
D3DXLoadSurfaceFromSurface(
	LPDIRECT3DSURFACE9 pDestSurface,
	CONST PALETTEENTRY *pDestPalette,
	CONST RECT *pDestRect,
	LPDIRECT3DSURFACE9 pSrcSurface,
	CONST PALETTEENTRY *pSrcPalette,
	CONST RECT *pSrcRect,
	DWORD Filter,
	D3DCOLOR ColorKey);

HRESULT WINAPI
D3DXGetErrorStringA(
	HRESULT hr,
	LPSTR pBuffer,
	UINT BufferLen);

// Taken and adopted from Wine 3.21
HRESULT WINAPI
D3DXFilterTexture(
	LPDIRECT3DBASETEXTURE9 pBaseTexture,
	CONST PALETTEENTRY *pPalette,
	UINT SrcLevel,
	DWORD Filter);

HRESULT WINAPI
D3DXCreateCubeTexture(
	LPDIRECT3DDEVICE9 pDevice,
	UINT Size,
	UINT MipLevels,
	DWORD Usage,
	D3DFORMAT Format,
	D3DPOOL Pool,
	LPDIRECT3DCUBETEXTURE9 *ppCubeTexture);

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
	LPDIRECT3DVOLUMETEXTURE9 *ppVolumeTexture);

// ID3DXBuffer/LPD3DXBUFFER are defined above (before the extern "C" block).

// Compiles HLSL through the system D3DCompile. The source must carry a
// "// d3d-target: vs_2_0|ps_2_0|..." directive and may override the default "main" entry point
// with "// d3d-entry: <name>". Returns the DXBC blob for CreateVertexShader/CreatePixelShader.
HRESULT WINAPI
D3DXAssembleShader(
	LPCVOID pSrcData,
	UINT SrcDataLen,
	DWORD Flags,
	LPD3DXBUFFER *ppConstants,
	LPD3DXBUFFER *ppCompiledShader,
	LPD3DXBUFFER *ppCompilationErrors);


UINT WINAPI D3DXGetFVFVertexSize(DWORD FVF);

#ifdef __cplusplus
}
#endif
