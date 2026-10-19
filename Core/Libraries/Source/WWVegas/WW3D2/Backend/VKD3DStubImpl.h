// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J: the no-d3d9 driver front.
//
// VKD3DStub.h answers every D3D9 method inertly. This file is the part that has to answer *correctly* for
// the engine to reach a frame at all: adapter enumeration, the capability block, display modes, and device
// creation. Everything else stays inert and is counted, which is how wedge 9 knows what to port next.
//
// Why a D3D9-shaped object at all: the engine holds IDirect3D* pointers in roughly 1600 places outside the
// facade. Replacing the library means supplying objects of those shapes, backed by Vulkan, not deleting the
// types. The shells here are that seam.
//
// What the census means once these shells exist: a method that still reports a hit is one the Vulkan
// backend must own. Metadata answers (sizes, formats, level counts, the caps block) are backed here and so
// stop appearing, because storing the creation parameters is not GPU work. Creation calls keep appearing
// even though they hand back a live object: an object without storage is still unbacked, and LockRect is
// where that becomes visible.
#pragma once

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

#include "VKD3DStub.h"

// Reads GENERALSX_VK_NOD3D once. Off by default: d3d9 stays the driver and this file is never entered.
bool VK_NOD3D_Enabled();

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: one-qword write watch for the no-d3d9 runs.
// A stub COM object in this build was found alive at one engine call and with only its vptr qword cleared at
// the next, with no destructor of ours ever running - so somebody wrote a null over it. Arm names the object
// to hold still, check is called from every engine and stub touch point and reports the first one that finds
// the vptr changed, which is the write that killed it. No-ops unless GENERALSX_VK_NOD3D is on.
void VK_NOD3D_Watch_Arm(void *obj);
void VK_NOD3D_Watch_Check(const char *where);

// Creates the IDirect3D9 replacement for DX8Wrapper::Init. The window handle is kept for display-mode
// lookup (the monitor the engine presents on) and for the device.
IDirect3D9 *VK_Stub_Create_D3D9(HWND hwnd);

// Fills the capability block the engine's DX8Caps layer and the facade read side consume. The values are
// measured, not invented: GENERALSX_VK_CAPDUMP=1 on the d3d9 path prints the reference block.
void VK_Stub_Fill_Caps(D3DCAPS9 *caps, UINT adapter, D3DDEVTYPE devtype);

class VKD3DDevice;
class VKD3DTexture;

// D3D floors a mip dimension at 1 rather than 0, and the engine walks level counts on the assumption that
// GetLevelDesc never returns a zero extent.
unsigned VK_Stub_Mip_Dim(unsigned baseDim, unsigned level);

// ---- descriptor-carrying resource shells ---------------------------------------

class VKD3DSurface : public VKStubSurface
{
public:
    VKD3DSurface(VKD3DDevice *dev, const D3DSURFACE_DESC &desc, IUnknown *container);
    virtual ~VKD3DSurface();

    STDMETHOD_(D3DRESOURCETYPE, GetType)(THIS) override { return D3DRTYPE_SURFACE; }
    STDMETHOD(GetDesc)(THIS_ D3DSURFACE_DESC *pDesc) override;
    STDMETHOD(GetContainer)(THIS_ REFIID riid, void **ppContainer) override;
    STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9 **ppDevice) override;
    STDMETHOD(LockRect)(THIS_ D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override;
    STDMETHOD(UnlockRect)(THIS) override;

    // Wedge 9.K: a GetSurfaceLevel handle is a *view* of a texture level, not a second copy of it. The
    // engine locks the view (UpdateSurface destinations, D3DX paths) while the Vulkan mirror re-locks the
    // texture, so both faces have to hand out the same bytes or the upload would serve stale texels.
    void set_texture_level_view(VKD3DTexture *tex, unsigned level);
    VKD3DTexture *texture_view() const { return m_tex; }
    unsigned texture_view_level() const { return m_texLevel; }

    // Wedge 9.K: the copy paths inside the stub device need the creation descriptor, which is what the
    // surface already hands out through GetDesc.
    const D3DSURFACE_DESC &Desc() const { return m_desc; }

    // wedge 9.K: the CPU store behind this surface, allocated on demand and *without* the lock-legality
    // gate. D3D9 routes DEFAULT-pool writes through device copies (UpdateSurface), not locks, and a copy
    // destination must still land in the mirror even though callers can never lock it.
    bool cpu_store(unsigned char **bits, unsigned *pitch);

private:
    VKD3DDevice *m_dev;
    D3DSURFACE_DESC m_desc;
    IUnknown *m_container;
    unsigned char *m_bits;    // wedge 9.K: the CPU store a locked surface hands out
    void *m_region;           // wedge 9.J3: guard-page region behind m_bits in the no-d3d9 runs, else null
    unsigned m_pitch;
    VKD3DTexture *m_tex;      // wedge 9.K: non-null on a GetSurfaceLevel view; bytes live in the texture
    unsigned m_texLevel;
};

class VKD3DTexture : public VKStubTexture
{
public:
    VKD3DTexture(VKD3DDevice *dev, UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    virtual ~VKD3DTexture();

    STDMETHOD_(D3DRESOURCETYPE, GetType)(THIS) override { return D3DRTYPE_TEXTURE; }
    STDMETHOD_(DWORD, GetLevelCount)(THIS) override { return m_levels; }
    STDMETHOD_(DWORD, SetLOD)(THIS_ DWORD LODNew) override;
    STDMETHOD_(DWORD, GetLOD)(THIS) override;
    STDMETHOD(GetLevelDesc)(THIS_ UINT level, D3DSURFACE_DESC *pDesc) override;
    STDMETHOD(GetSurfaceLevel)(THIS_ UINT level, IDirect3DSurface9 **ppSurfaceLevel) override;
    STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9 **ppDevice) override;
    STDMETHOD(LockRect)(THIS_ UINT level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD flags) override;
    STDMETHOD(UnlockRect)(THIS_ UINT level) override;

    // wedge 9.K: the CPU store a lock hands out. Allocated on the first lock rather than at creation,
    // because most textures the engine makes are never locked and the game holds thousands of them.
    bool ensure_level(unsigned level);
    unsigned char *level_bits(unsigned level) const { return m_store[level].bits; }
    unsigned level_pitch(unsigned level) const { return m_store[level].pitch; }
    unsigned level_count() const { return m_levels; }
    D3DFORMAT level_format() const { return m_format; }
    unsigned width() const { return m_width; }
    unsigned height() const { return m_height; }

    // wedge 9.K: byte store for a device copy destination. Unlike LockRect this ignores the
    // DEFAULT-pool/lockable gate, because D3D9 only lets DEFAULT levels be written through UpdateSurface -
    // refusing the store there would silently drop exactly the copies the Vulkan mirror needs to see.
    bool cpu_store_level(unsigned level, unsigned char **bits, unsigned *pitch);

    // wedge 9.K: feed the finished CPU store straight into the keyed Vulkan mirror, bypassing the
    // lock-legality gate a mirror re-lock would hit on a DEFAULT-pool texture. The device-side copy
    // movers call this after they land bytes in a texture level the caller can never lock.
    void mirror_to_vulkan();

private:
    void fill_desc(D3DSURFACE_DESC *pDesc, UINT level) const;
    // wedge 9.K: d3d9/DXVK generated the mip chain of a Levels=0 MANAGED texture on the device upload;
    // with no device behind the stub, the chain has to be box-filtered here when the writer unlocks
    // level 0. Formats without a safe CPU filter log once and keep zeroed upper levels.
    void generate_mips_from_level0();

    struct Level_Store { unsigned char *bits; unsigned pitch; void *region; unsigned char written; };

    VKD3DDevice *m_dev;
    unsigned m_width;
    unsigned m_height;
    unsigned m_levels;
    DWORD m_usage;
    D3DFORMAT m_format;
    D3DPOOL m_pool;
    bool m_auto_mip;      // wedge 9.K: created with Levels=0, so the chain is generated on unlock
    DWORD m_lod;          // wedge 9.K: SetLOD/GetLOD echo the stored value like the real device
    Level_Store m_store[16];
};

class VKD3DVertexBuffer : public VKStubVertexBuffer
{
public:
    VKD3DVertexBuffer(VKD3DDevice *dev, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool);
    virtual ~VKD3DVertexBuffer();

    STDMETHOD_(D3DRESOURCETYPE, GetType)(THIS) override { return D3DRTYPE_VERTEXBUFFER; }
    STDMETHOD(GetDesc)(THIS_ D3DVERTEXBUFFER_DESC *pDesc) override;
    STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9 **ppDevice) override;
    STDMETHOD(Lock)(THIS_ UINT offsetToLock, UINT sizeToLock, void **ppbData, DWORD flags) override;
    STDMETHOD(Unlock)(THIS) override;

private:
    VKD3DDevice *m_dev;
    D3DVERTEXBUFFER_DESC m_desc;
    unsigned char *m_bits;
    void *m_region;   // wedge 9.J3: guard-page region in the no-d3d9 runs, else null
};

class VKD3DIndexBuffer : public VKStubIndexBuffer
{
public:
    VKD3DIndexBuffer(VKD3DDevice *dev, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool);
    virtual ~VKD3DIndexBuffer();

    STDMETHOD_(D3DRESOURCETYPE, GetType)(THIS) override { return D3DRTYPE_INDEXBUFFER; }
    STDMETHOD(GetDesc)(THIS_ D3DINDEXBUFFER_DESC *pDesc) override;
    STDMETHOD(GetDevice)(THIS_ IDirect3DDevice9 **ppDevice) override;
    STDMETHOD(Lock)(THIS_ UINT offsetToLock, UINT sizeToLock, void **ppbData, DWORD flags) override;
    STDMETHOD(Unlock)(THIS) override;

private:
    VKD3DDevice *m_dev;
    D3DINDEXBUFFER_DESC m_desc;
    unsigned char *m_bits;
    void *m_region;   // wedge 9.J3: guard-page region in the no-d3d9 runs, else null
};

// The inert stub device, plus the few answers the boot path cannot do without: the caps block, the swap
// chain's buffers, and live (if storage-less) objects for every Create* the engine reaches.
class VKD3DDevice : public VKStubDevice
{
public:
    VKD3DDevice(HWND hwnd, const D3DPRESENT_PARAMETERS &pp, DWORD behaviorFlags);

    STDMETHOD(Reset)(THIS_ D3DPRESENT_PARAMETERS *pPresentationParameters) override;
    STDMETHOD(TestCooperativeLevel)(THIS) override { return D3D_OK; }
    STDMETHOD(GetCreationParameters)(THIS_ D3DDEVICE_CREATION_PARAMETERS *pParameters) override;
    STDMETHOD(GetDisplayMode)(THIS_ UINT iSwapChain, D3DDISPLAYMODE *pMode) override;
    STDMETHOD(GetDeviceCaps)(THIS_ D3DCAPS9 *pCaps) override;

    // The device is the state machine. Raw DX8CALL sites (the DEPTH_BIAS probe is one) ask the device to
    // echo back what they set, and an inert answer of 0 makes them take a "device does not support this"
    // branch that the d3d9 path never takes. Storage is all this needs; the initial values are the
    // measured d3d9 defaults, printed by GENERALSX_VK_CAPDUMP.
    STDMETHOD(SetRenderState)(THIS_ D3DRENDERSTATETYPE State, DWORD Value) override;
    STDMETHOD(GetRenderState)(THIS_ D3DRENDERSTATETYPE State, DWORD *pValue) override;
    STDMETHOD(SetTextureStageState)(THIS_ DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override;
    STDMETHOD(GetTextureStageState)(THIS_ DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override;
    STDMETHOD(SetSamplerState)(THIS_ DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override;
    STDMETHOD(GetSamplerState)(THIS_ DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override;
    // GeneralsX @bugfix Qoder 19/10/2026 BUG-03: the transform stack, same argument as the block above.
    // DX8Wrapper::_Get_DX8_Transform asks the DEVICE (not the wrapper shadow) for VIEW/PROJECTION/TEXTUREn,
    // and the terrain noise passes invert the answer: the inherited inert stub zeroed it, the zero inverse
    // came back NaN, and NaN texture matrices blacked out the ground. Identity defaults are the d3d9
    // device's own initial state; SetTransform stores the bytes verbatim, GetTransform echoes them back.
    STDMETHOD(SetTransform)(THIS_ D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override;
    STDMETHOD(GetTransform)(THIS_ D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override;

    // Creation calls stay counted in the census: handing back an object is not handing back storage, and
    // the count of each is the work list for the wedges that follow (9.K textures, 9.L buffers).
    STDMETHOD(CreateTexture)(THIS_ UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
        D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) override;
    STDMETHOD(CreateVolumeTexture)(THIS_ UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
        D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle) override;
    STDMETHOD(CreateCubeTexture)(THIS_ UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
        IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) override;
    STDMETHOD(CreateVertexBuffer)(THIS_ UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
        IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) override;
    STDMETHOD(CreateIndexBuffer)(THIS_ UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
        IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) override;
    // Wedge 9.P shader sovereignty: W3DShaderManager and the program registry key the translated SPIR-V
    // twins by the handle the device hands back, so an inert null here makes every runtime HLSL compile
    // "succeed" into nothing and the manager's fail-closed guard has no object left to bind. A live
    // identity shell is all the seam needs: the bytecode is not stored, because the registry matches on
    // the HLSL source and nothing in the engine calls GetFunction on a created shader.
    STDMETHOD(CreateVertexShader)(THIS_ CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader) override;
    STDMETHOD(CreatePixelShader)(THIS_ CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader) override;
    STDMETHOD(CreateRenderTarget)(THIS_ UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
        DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override;
    STDMETHOD(CreateDepthStencilSurface)(THIS_ UINT Width, UINT Height, D3DFORMAT Format,
        D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
        IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override;
    STDMETHOD(CreateOffscreenPlainSurface)(THIS_ UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
        IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override;
    STDMETHOD(GetBackBuffer)(THIS_ UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
        IDirect3DSurface9 **ppBackBuffer) override;

    // Wedge 9.K: device-level pixel movers. D3D9 funnels DEFAULT-pool CPU writes through these instead of
    // LockRect, so the engine's copy paths (DX8Wrapper::_Copy_DX8_Rects above all) only land in the Vulkan
    // mirror once the stub performs the row copy itself. Census proved UpdateSurface is the only mover with
    // real traffic in the replay (459 calls); the rest stay inert until the scoreboard names them.
    STDMETHOD(UpdateSurface)(THIS_ IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect,
        IDirect3DSurface9 *pDestinationSurface, CONST POINT *pDestPoint) override;
    STDMETHOD(UpdateTexture)(THIS_ IDirect3DBaseTexture9 *pSourceTexture,
        IDirect3DBaseTexture9 *pDestinationTexture) override;
    STDMETHOD(GetRenderTargetData)(THIS_ IDirect3DSurface9 *pRenderTarget,
        IDirect3DSurface9 *pDestSurface) override;

    // Metadata floors the engine reads as "device has no memory left" if the inert 0 keeps answering.
    STDMETHOD_(UINT, GetAvailableTextureMem)(THIS) override { return 64u * 1024u * 1024u; }

    HWND Window() const { return m_hwnd; }
    const D3DPRESENT_PARAMETERS &PresentParameters() const { return m_pp; }

    // Back-buffer extent, which is the description GetBackBuffer hands out in windowed mode.
    void back_buffer_desc(D3DSURFACE_DESC *pDesc) const;

private:
    enum { VK_RS_SLOTS = 512, VK_STAGE_COUNT = 8, VK_TSS_SLOTS = 64, VK_SAMP_SLOTS = 16, VK_TS_SLOTS = 260 };

    HWND m_hwnd;
    D3DPRESENT_PARAMETERS m_pp;
    DWORD m_behaviorFlags;
    DWORD m_rs[VK_RS_SLOTS];
    DWORD m_tss[VK_STAGE_COUNT][VK_TSS_SLOTS];
    DWORD m_samp[VK_STAGE_COUNT][VK_SAMP_SLOTS];
    D3DMATRIX m_ts[VK_TS_SLOTS];   // BUG-03: VIEW=2, PROJECTION=3, TEXTURE0..7=16..23, WORLD..WORLD3=256..259
};

#endif // _WIN32 && SAGE_USE_VULKAN_CORE
