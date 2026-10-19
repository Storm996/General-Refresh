// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J: the answers the boot path cannot do without.
//
// See Backend/VKD3DStubImpl.h for the shape of this wedge. Everything here is either a Win32 GDI query or a
// Vulkan physical-device query - the two things that stay true once d3d9.dll is gone.
#ifdef _WIN32
#if defined(SAGE_USE_VULKAN_CORE)

#include "VKD3DStubImpl.h"

#include "VulkanCore.h"
#include "VulkanFns.h"    // G: the dynamically imported entry points; vulkan-1.lib is never link-time input
#include "VulkanGeo.h"    // wedge 9.K: VKTex_Ready/VKTex_Upload_2D - the mirror the copied bytes must reach
#include "../formconv.h"  // wedge 9.K: D3DFormat_To_WW3DFormat for the mirror feed

#include <stdio.h>
#include <string.h>
#include <new>
#include <excpt.h>   // AddVectoredExceptionHandler, LPEXCEPTION_POINTERS
#include <intrin.h>  // __debugbreak, RtlCaptureStackBackTrace

// GeneralsX @feature Qoder 18/10/2026 wedge 9.J3: the reference ring lives in Backend/VKD3DStubNotes.cpp; the
// vectored handler is the only caller, so it is declared here instead of in the generated stub header.
void VK_Stub_Ref_Dump(const void *const *regs, unsigned nregs, const char *why);

// GeneralsX @feature Qoder 18/10/2026 wedge 9.J3: guard-page-backed CPU stores for the no-d3d9 runs.
//
// Two faults in a row (the dynamic vertex pool, then a texture in DX8Wrapper::Set_DX8_Texture) showed a stub
// COM object that our own reference ring says was never deleted, but whose first qword - the vtable pointer -
// read back as zero. Something outside the object wrote over it. The stores a Lock hands out are the only
// memory this layer gives the engine to write into, and they are allocated exactly pitch*height, while d3d8
// and d3d9 hand out a row-aligned pitch inside a driver-granular allocation: a caller that writes past its
// rectangle stays harmless there and runs straight into the next heap block here, and the next heap block is
// very often another stub object.
//
// So in the diagnostic mode every store gets its own page-aligned region with a PAGE_NOACCESS page directly
// after it, and the payload is placed so its last byte touches that page. An underrun stays invisible (the
// waste page in front is readable), an overrun faults immediately with the writer's own address, stack and
// length in the crash log instead of corrupting a neighbour. Without GENERALSX_VK_NOD3D the plain new[] path
// is used, so the shipping driver is untouched.
void vk_stub_store_alloc(unsigned char *&bits, void *&region, size_t bytes)
{
    bits = nullptr;
    region = nullptr;
    if (!bytes) return;

    if (!VK_NOD3D_Enabled()) {
        bits = new (std::nothrow) unsigned char[bytes];
        if (bits) ::memset(bits, 0, bytes);
        return;
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const size_t page = (size_t) si.dwAllocationGranularity;
    const size_t payload = (bytes + page - 1) & ~(page - 1);
    unsigned char *base = (unsigned char *) VirtualAlloc(nullptr, payload + page, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) {
        bits = new (std::nothrow) unsigned char[bytes];
        if (bits) ::memset(bits, 0, bytes);
        return;
    }
    DWORD old = 0;
    VirtualProtect(base + payload, page, PAGE_NOACCESS, &old);
    // The payload ends at the byte before the guard page, so the first byte written past the store faults.
    region = base;
    bits = base + payload - bytes;
    ::memset(bits, 0, bytes);
}

void vk_stub_store_free(void *region, unsigned char *bits)
{
    if (region) VirtualFree(region, 0, MEM_RELEASE);
    else delete[] bits;
}

namespace {

struct StubMode
{
    unsigned long width;
    unsigned long height;
    unsigned long refresh;
    unsigned short bpp;
};

StubMode g_stubModes[256];
unsigned g_stubModeCount = 0;
wchar_t g_stubDisplayName[64] = L"\\\\.\\DISPLAY1";
bool g_stubModesBuilt = false;

// D3D9 enumerates display modes per back-buffer format; the mode list itself only carries bits-per-pixel,
// so the format-to-bpp mapping is what decides which modes a format sees.
unsigned short stub_format_bpp(D3DFORMAT fmt)
{
    switch (fmt) {
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
        return 16;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_R8G8B8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A16B16G16R16:
        return 32;
    default:
        return 0;
    }
}

void stub_build_modes(HWND hwnd)
{
    if (g_stubModesBuilt) return;
    g_stubModesBuilt = true;
    g_stubModeCount = 0;

    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW mi;
    ::ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO*>(&mi)) && mi.szDevice[0]) {
        wcsncpy(g_stubDisplayName, mi.szDevice, 62);
        g_stubDisplayName[63] = 0;
    }

    DEVMODEW dm;
    for (DWORD i = 0; i < 256; ++i) {
        ::ZeroMemory(&dm, sizeof(dm));
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsExW(g_stubDisplayName, i, &dm, 0)) break;
        if ((dm.dmFields & (DM_PELSWIDTH | DM_PELSHEIGHT)) != (DM_PELSWIDTH | DM_PELSHEIGHT)) continue;

        const unsigned short bpp = (unsigned short)dm.dmBitsPerPel;
        const unsigned long refresh = dm.dmDisplayFrequency ? dm.dmDisplayFrequency : 60;
        bool duplicate = false;
        for (unsigned k = 0; k < g_stubModeCount; ++k) {
            if (g_stubModes[k].width == dm.dmPelsWidth && g_stubModes[k].height == dm.dmPelsHeight &&
                g_stubModes[k].refresh == refresh && g_stubModes[k].bpp == bpp) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        g_stubModes[g_stubModeCount].width = dm.dmPelsWidth;
        g_stubModes[g_stubModeCount].height = dm.dmPelsHeight;
        g_stubModes[g_stubModeCount].refresh = refresh;
        g_stubModes[g_stubModeCount].bpp = bpp;
        ++g_stubModeCount;
    }

    fprintf(stderr, "[VKNOD3D] mode list built display=%ls modes=%u\n", g_stubDisplayName, g_stubModeCount);
    fflush(stderr);
}

// The current display mode, which is what GetAdapterDisplayMode and a windowed back buffer report.
void stub_current_mode(D3DDISPLAYMODE* pMode, D3DFORMAT fmt)
{
    DEVMODEW dm;
    ::ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsExW(g_stubDisplayName, ENUM_CURRENT_SETTINGS, &dm, 0)) {
        pMode->Width = dm.dmPelsWidth;
        pMode->Height = dm.dmPelsHeight;
        pMode->RefreshRate = dm.dmDisplayFrequency ? dm.dmDisplayFrequency : 60;
        if (pMode->Format == D3DFMT_UNKNOWN) {
            pMode->Format = (dm.dmBitsPerPel == 16) ? D3DFMT_R5G6B5 : D3DFMT_X8R8G8B8;
        }
    } else {
        pMode->Width = 1024;
        pMode->Height = 768;
        pMode->RefreshRate = 60;
        if (pMode->Format == D3DFMT_UNKNOWN) pMode->Format = D3DFMT_X8R8G8B8;
    }
    (void)fmt;
}

void stub_fill_identifier(D3DADAPTER_IDENTIFIER9* pIdent)
{
    ::ZeroMemory(pIdent, sizeof(*pIdent));

    // The PCI ids decide which vendor quirk table DX8Caps picks, so they have to be the real adapter's, and
    // the Vulkan physical device reports exactly those on Windows.
    VulkanCore* core = VulkanCore_Get();
    if (core && core->Phys()) {
        VkPhysicalDeviceProperties props;
        G.vkGetPhysicalDeviceProperties(core->Phys(), &props);
        pIdent->VendorId = props.vendorID;
        pIdent->DeviceId = props.deviceID;
        snprintf(pIdent->Description, sizeof(pIdent->Description) - 1, "%s (Vulkan, no d3d9)", props.deviceName);
    } else {
        // Enumeration ran before the Vulkan core came up; DX8Caps then takes the VENDOR_UNKNOWN branch.
        snprintf(pIdent->Description, sizeof(pIdent->Description) - 1, "GeneralsX Vulkan Backend");
    }
    snprintf(pIdent->Driver, sizeof(pIdent->Driver) - 1, "generalsxvk.dll");
    WideCharToMultiByte(CP_ACP, 0, g_stubDisplayName, -1, pIdent->DeviceName, sizeof(pIdent->DeviceName) - 1, nullptr, nullptr);
}

// The IDirect3D9 replacement. Adapter enumeration, format support and the capability block are answered
// here; every other method falls through to the inert stub base and shows up in the census.
class VKD3D9 : public VKStubD3D9
{
public:
    explicit VKD3D9(HWND hwnd) { m_hwnd = hwnd; }

    STDMETHOD_(UINT, GetAdapterCount)(THIS) override { return 1; }

    STDMETHOD(GetAdapterIdentifier)(THIS_ UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier) override
    {
        (void)Adapter; (void)Flags;
        stub_build_modes(m_hwnd);
        if (!pIdentifier) return D3DERR_INVALIDCALL;
        stub_fill_identifier(pIdentifier);
        return D3D_OK;
    }

    STDMETHOD_(UINT, GetAdapterModeCount)(THIS_ UINT Adapter, D3DFORMAT Format) override
    {
        (void)Adapter;
        stub_build_modes(m_hwnd);
        const unsigned short bpp = stub_format_bpp(Format);
        if (!bpp) return 0;
        UINT count = 0;
        for (unsigned i = 0; i < g_stubModeCount; ++i) if (g_stubModes[i].bpp == bpp) ++count;
        return count;
    }

    STDMETHOD(EnumAdapterModes)(THIS_ UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode) override
    {
        (void)Adapter;
        stub_build_modes(m_hwnd);
        const unsigned short bpp = stub_format_bpp(Format);
        if (!bpp || !pMode) return D3DERR_INVALIDCALL;
        for (unsigned i = 0; i < g_stubModeCount; ++i) {
            if (g_stubModes[i].bpp != bpp) continue;
            if (Mode == 0) {
                pMode->Width = g_stubModes[i].width;
                pMode->Height = g_stubModes[i].height;
                pMode->RefreshRate = g_stubModes[i].refresh;
                pMode->Format = Format;
                return D3D_OK;
            }
            --Mode;
        }
        return D3DERR_NOTAVAILABLE;
    }

    STDMETHOD(GetAdapterDisplayMode)(THIS_ UINT Adapter, D3DDISPLAYMODE* pMode) override
    {
        (void)Adapter;
        stub_build_modes(m_hwnd);
        if (!pMode) return D3DERR_INVALIDCALL;
        pMode->Format = D3DFMT_UNKNOWN;
        stub_current_mode(pMode, D3DFMT_UNKNOWN);
        return D3D_OK;
    }

    // Format support: the Vulkan backend is the thing that has to honour these claims, and every format the
    // engine probes here is one it already renders through VKTex/VKRT. The census records the probes so a
    // later audit can check each answer against VkFormatFeatureFlags instead of trusting this line.
    STDMETHOD(CheckDeviceType)(THIS_ UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, BOOL) override { return D3D_OK; }
    STDMETHOD(CheckDeviceFormat)(THIS_ UINT, D3DDEVTYPE, D3DFORMAT, DWORD, D3DRESOURCETYPE, D3DFORMAT) override { return D3D_OK; }
    STDMETHOD(CheckDepthStencilMatch)(THIS_ UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
    STDMETHOD(CheckDeviceFormatConversion)(THIS_ UINT, D3DDEVTYPE, D3DFORMAT, D3DFORMAT) override { return D3D_OK; }
    STDMETHOD(CheckDeviceMultiSampleType)(THIS_ UINT, D3DDEVTYPE, D3DFORMAT, BOOL, D3DMULTISAMPLE_TYPE, DWORD* pQualityLevels) override
    {
        if (pQualityLevels) *pQualityLevels = 0;
        return D3D_OK;
    }

    STDMETHOD(GetDeviceCaps)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps) override
    {
        if (!pCaps) return D3DERR_INVALIDCALL;
        VK_Stub_Fill_Caps(pCaps, Adapter, DeviceType);
        return D3D_OK;
    }

    STDMETHOD_(HMONITOR, GetAdapterMonitor)(THIS_ UINT Adapter) override
    {
        (void)Adapter;
        return MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTOPRIMARY);
    }

    STDMETHOD(CreateDevice)(THIS_ UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
        D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DDevice9** ppReturnedDeviceInterface) override
    {
        (void)Adapter; (void)DeviceType; (void)BehaviorFlags;
        if (!pPresentationParameters || !ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;
        VK_Stub_Store_Note(ppReturnedDeviceInterface, "VKStubD3D9::CreateDevice");
        *ppReturnedDeviceInterface = new VKD3DDevice(hFocusWindow ? hFocusWindow : m_hwnd, *pPresentationParameters, BehaviorFlags);
        fprintf(stderr, "[VKNOD3D] device created windowed=%d backbuffer=%ux%u fmt=%u depth=%u\n",
            (int)pPresentationParameters->Windowed, (unsigned)pPresentationParameters->BackBufferWidth,
            (unsigned)pPresentationParameters->BackBufferHeight, (unsigned)pPresentationParameters->BackBufferFormat,
            (unsigned)pPresentationParameters->AutoDepthStencilFormat);
        fflush(stderr);
        return D3D_OK;
    }

private:
    HWND m_hwnd;
};

} // namespace

bool VK_NOD3D_Enabled()
{
    static int cached = -1;
    if (cached < 0) {
        const char* value = getenv("GENERALSX_VK_NOD3D");
        cached = (value && atoi(value) != 0) ? 1 : 0;
    }
    return cached == 1;
}

namespace {
struct VkWatch { void *obj; uintptr_t live; unsigned long checks; } g_vk_watch = { nullptr, 0, 0 };
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: the cheapest thing a no-d3d9 run can watch is
// the vptr, because every virtual call reads it and a stub object with a live body but a cleared vptr is
// exactly what the last run showed. One slot, armed once, checked from the engine's VB lock/unlock sites and
// from the stub's own methods: the first line that says DEAD names the touch point the write landed behind.
void VK_NOD3D_Watch_Arm(void *obj)
{
    if (!VK_NOD3D_Enabled() || g_vk_watch.obj) return;
    g_vk_watch.obj = obj;
    g_vk_watch.live = *reinterpret_cast<uintptr_t *>(obj);
    g_vk_watch.checks = 0;
    fprintf(stderr, "[VKNOD3D] watch armed: obj=%p vptr=%p\n", obj, (void *) g_vk_watch.live);
    fflush(stderr);
}

void VK_NOD3D_Watch_Check(const char *where)
{
    if (!VK_NOD3D_Enabled() || !g_vk_watch.obj) return;
    if (++g_vk_watch.checks > 400000) { g_vk_watch.obj = nullptr; return; }
    const uintptr_t now = *reinterpret_cast<uintptr_t *>(g_vk_watch.obj);
    if (now == g_vk_watch.live) return;
    fprintf(stderr, "[VKNOD3D] watch DEAD at %s (checks=%lu): obj=%p was=%p now=%p\n",
        where, (unsigned long) g_vk_watch.checks, g_vk_watch.obj, (void *) g_vk_watch.live, (void *) now);
    fflush(stderr);
    g_vk_watch.obj = nullptr;
}

// GeneralsX @feature Qoder 18/10/2026 wedge 9.J4: hardware write watchpoints for the no-d3d9 runs.
//
// The reference ring and the live sweep settled what happened: a live stub object - refcount intact, fields
// intact - has exactly its first qword written with zero. That is a store through a pointer that names the
// object, not a recycle and not an overrun, and the only way to name the storing instruction without a
// debugger is a CPU watchpoint. Debug registers are set from inside a vectored handler: a deliberate int3
// hands us a CONTEXT we are allowed to edit, so Dr0-Dr3 get the object heads and Dr7 enables write traps.
// The window tracks the last few births, deliberately excluding the object being constructed, because its
// own constructor is the next writer of that qword and would fire before the real one.
// GENERALSX_VK_WP=1 arms it; it stays off otherwise because a trap per birth is not free.
namespace {

const unsigned VK_WP_SLOTS = 4;
const unsigned VK_WP_RECENT = 8;
const unsigned long long VK_WP_MAX_REPORTS = 40ull;

void *g_wpAddr[VK_WP_SLOTS];
void *g_wpRecent[VK_WP_RECENT];
unsigned g_wpRecentCount = 0;
unsigned long long g_wpHits = 0;
volatile LONG g_wpWantArm = 0;
volatile LONG g_wpArmRequests = 0;
volatile LONG g_wpArmDone = 0;
unsigned long long g_wpStrayBreakpoints = 0;
bool g_wpEnabled = false;
bool g_wpLive = false;

void vk_wp_program(CONTEXT *ctx)
{
    ctx->Dr0 = (DWORD_PTR) g_wpAddr[0];
    ctx->Dr1 = (DWORD_PTR) g_wpAddr[1];
    ctx->Dr2 = (DWORD_PTR) g_wpAddr[2];
    ctx->Dr3 = (DWORD_PTR) g_wpAddr[3];
    DWORD_PTR dr7 = 0;
    for (unsigned n = 0; n < VK_WP_SLOTS; ++n) {
        if (!g_wpAddr[n]) continue;
        dr7 |= (1ull << (4u * n))              // local enable
             | (1ull << (16u + 4u * n))         // condition = write
             | (2ull << (14u + 4u * n));        // length  = 8 bytes (64-bit mode)
    }
    ctx->Dr7 = dr7;
    g_wpLive = dr7 != 0;
}

// The trap is the arm request: a handler is the only place a running thread may set its own debug registers.
// Nothing may follow the instruction in the same basic block - MSVC pads the tail of a function with int3,
// and a second breakpoint with no arm pending left the first build dying on its own probe - so the counter
// update is deliberately on this side of the trap.
void vk_wp_trap_and_arm()
{
    InterlockedIncrement(&g_wpArmRequests);
    InterlockedExchange(&g_wpWantArm, 1);
    __debugbreak();
    InterlockedIncrement(&g_wpArmDone);
}

void vk_wp_disarm(CONTEXT *ctx)
{
    ctx->Dr7 = 0;
    g_wpLive = false;
}

// The trap fires before the storing instruction runs, so this names the writer exactly once per store.
void vk_wp_report(LPEXCEPTION_POINTERS info)
{
    HMODULE base = GetModuleHandleA(nullptr);
    CONTEXT *ctx = info->ContextRecord;
    ++g_wpHits;
    const DWORD_PTR dr6 = ctx->Dr6;
    unsigned slot = 0;
    for (; slot < VK_WP_SLOTS; ++slot) if (dr6 & (1ull << slot)) break;
    if (slot >= VK_WP_SLOTS) slot = 0;

    static const char *const names[14] =
        {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
    void *regs[14] = {(void *) (uintptr_t) ctx->Rax, (void *) (uintptr_t) ctx->Rbx,
        (void *) (uintptr_t) ctx->Rcx, (void *) (uintptr_t) ctx->Rdx,
        (void *) (uintptr_t) ctx->Rsi, (void *) (uintptr_t) ctx->Rdi,
        (void *) (uintptr_t) ctx->R8,  (void *) (uintptr_t) ctx->R9,
        (void *) (uintptr_t) ctx->R10, (void *) (uintptr_t) ctx->R11,
        (void *) (uintptr_t) ctx->R12, (void *) (uintptr_t) ctx->R13,
        (void *) (uintptr_t) ctx->R14, (void *) (uintptr_t) ctx->R15};

    fprintf(stderr, "[VKWP] WRITE hit #%llu slot %u watched=%p dr6=0x%llx rip=%p",
        g_wpHits, slot, (slot < VK_WP_SLOTS ? g_wpAddr[slot] : nullptr), (unsigned long long) dr6,
        info->ExceptionRecord->ExceptionAddress);
    if (base) {
        fprintf(stderr, " rva=0x%llx",
            (unsigned long long) ((const char *) info->ExceptionRecord->ExceptionAddress - (const char *) base));
    }

    // The object's own head says whether this is a store the object asked for: a destructor runs after the
    // refcount has already reached zero, a rogue store does not.
    if (slot < VK_WP_SLOTS && g_wpAddr[slot]) {
        uintptr_t head[2] = {0, 0};
        __try {
            head[0] = reinterpret_cast<const uintptr_t *>(g_wpAddr[slot])[1];
            head[1] = reinterpret_cast<const uintptr_t *>(g_wpAddr[slot])[2];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        fprintf(stderr, " obj+0x08=%p obj+0x10=%p",
            (void *) head[0], (void *) head[1]);
    }

    // The bytes at the faulting address identify the store without a disassembly pass.
    unsigned char op[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    __try {
        memcpy(op, info->ExceptionRecord->ExceptionAddress, sizeof(op));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    fprintf(stderr, "\n[VKWP] store bytes:");
    for (unsigned i = 0; i < 8; ++i) fprintf(stderr, " %02x", op[i]);
    fprintf(stderr, "\n[VKWP] regs");
    for (unsigned i = 0; i < 14; ++i) fprintf(stderr, " %s=%p", names[i], regs[i]);
    fprintf(stderr, "\n[VKWP] rsp=%p rbp=%p\n", (void *) (uintptr_t) ctx->Rsp, (void *) (uintptr_t) ctx->Rbp);

    if (g_wpHits >= VK_WP_MAX_REPORTS) {
        fprintf(stderr, "[VKWP] disarming after %llu hits\n", g_wpHits);
        vk_wp_disarm(ctx);
    }
    if (base) {
        void *frames[16];
        const USHORT depth = RtlCaptureStackBackTrace(0, 16, frames, nullptr);
        for (USHORT i = 0; i < depth; ++i) {
            fprintf(stderr, "[VKWP] stack %2u rva=0x%llx\n", (unsigned) i,
                (unsigned long long) ((const char *) frames[i] - (const char *) base));
        }
    }
    fflush(stderr);
}

void vk_wp_init()
{
    const char *e = getenv("GENERALSX_VK_WP");
    g_wpEnabled = (e && atoi(e) != 0);
}

} // namespace

// Called from the reference ring for every stub object birth (Backend/VKD3DStubNotes.cpp).
void VK_WP_Watch_Birth(void *obj)
{
    if (!g_wpEnabled || !obj) return;

    // Arm on the objects that were born before this one; this one is still inside its constructor.
    const unsigned start = g_wpRecentCount > VK_WP_SLOTS ? g_wpRecentCount - VK_WP_SLOTS : 0;
    unsigned n = 0;
    for (unsigned i = start; i < g_wpRecentCount; ++i) g_wpAddr[n++] = g_wpRecent[i];
    for (; n < VK_WP_SLOTS; ++n) g_wpAddr[n] = nullptr;

    if (g_wpRecentCount < VK_WP_RECENT) g_wpRecent[g_wpRecentCount++] = obj;
    else {
        for (unsigned i = 1; i < VK_WP_RECENT; ++i) g_wpRecent[i - 1] = g_wpRecent[i];
        g_wpRecent[VK_WP_RECENT - 1] = obj;
    }

    // The registers can only be written through an exception context, so take a breakpoint of our own.
    if (g_wpAddr[0] || g_wpAddr[1] || g_wpAddr[2] || g_wpAddr[3]) vk_wp_trap_and_arm();
}

// True byte span of the main image from its NT headers. Run6 mislabeled a KERNELBASE rip as an exe RVA
// because the old fixed 4GB bound let neighboring DLLs look like "the module".
static uintptr_t vk_fault_image_size(HMODULE base)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *) base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0x00FFFFFFull;
    const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *) ((const char *) base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0x00FFFFFFull;
    return nt->OptionalHeader.SizeOfImage;
}

// Name the module owning a foreign rip. Two of the pure-Vulkan faults land in vcruntime/ucrtbase (the GS
// reporter and the memcpy that read past a locked-rect mirror), and "other-module" alone makes the reader
// guess which runtime is involved. A mapped DLL's AllocationBase is its image base, so one VirtualQuery
// plus GetModuleFileNameA names it - no toolhelp snapshot, which WIN32_LEAN_AND_MEAN keeps unincluded.
static void vk_fault_module_name(const char *addr)
{
    MEMORY_BASIC_INFORMATION mbi;
    char name[MAX_PATH];
    if (VirtualQuery(addr, &mbi, sizeof(mbi)) != 0 && mbi.AllocationBase &&
        GetModuleFileNameA((HMODULE) mbi.AllocationBase, name, sizeof(name)) > 0) {
        const char *lo = (const char *) mbi.AllocationBase;
        fprintf(stderr, " in %s+0x%llx", name, (unsigned long long) (addr - lo));
        return;
    }
    fprintf(stderr, " other-module");
}

// The first no-d3d9 runs are expected to die on a null resource: an inert stub hands back a zeroed
// IDirect3DTexture9**, and the caller that trusts it faults. The scoreboard is the whole point of the run,
// so it is written before whatever handler the game installs gets the exception. CONTINUE_SEARCH keeps the
// game's own crash reporting intact - this handler only prints.
static LONG CALLBACK vk_stub_crash_dump(LPEXCEPTION_POINTERS info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_BREAKPOINT && info->ContextRecord && InterlockedExchange(&g_wpWantArm, 0) != 0) {
        vk_wp_program(info->ContextRecord);
        // An int3 is one byte and CONTINUE_EXECUTION restarts it where it faulted. Without stepping over
        // it the thread re-traps on the same byte forever - which is exactly how the first two WP runs hung.
        info->ContextRecord->Rip += 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP && info->ContextRecord && g_wpLive) {
        vk_wp_report(info);
        // A data breakpoint is a fault raised before the store runs, so resuming re-executes the same
        // store. The Intel resume flag lets that one instruction through without re-testing the
        // breakpoint, which is what makes every later write to the address fire exactly once.
        info->ContextRecord->Dr6 = 0;
        info->ContextRecord->EFlags |= 0x00010000u;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // A breakpoint with no arm pending is compiler padding next to the trap we take for ourselves. The game
    // has no use for it and a diagnostic run that dies on it learns nothing, so it is spent here.
    if (code == EXCEPTION_BREAKPOINT && g_wpEnabled) {
        if (++g_wpStrayBreakpoints <= 8ull) {
            HMODULE b = GetModuleHandleA(nullptr);
            fprintf(stderr, "[VKWP] stray breakpoint consumed at %p", info->ExceptionRecord->ExceptionAddress);
            if (b) {
                fprintf(stderr, " rva=0x%llx",
                    (unsigned long long) ((const char *) info->ExceptionRecord->ExceptionAddress - (const char *) b));
            }
            fprintf(stderr, " (arms requested=%ld done=%ld)\n", (long) g_wpArmRequests, (long) g_wpArmDone);
            fflush(stderr);
        }
        if (info->ContextRecord) info->ContextRecord->Rip += 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (g_wpEnabled) {
        fprintf(stderr, "[VKNOD3D] watchpoint state: arms=%ld done=%ld hits=%llu stray=%llu live=%d\n",
            (long) g_wpArmRequests, (long) g_wpArmDone, g_wpHits, g_wpStrayBreakpoints, g_wpLive ? 1 : 0);
    }

    // One dump at a time per process. The ring dump probes candidate pointers and a probe always faults
    // somewhere; that fault re-enters this vectored handler, and before the guard the nested dump probed
    // again - the run that walked the stack guard page down 0x10000 bytes at a time and died of 0xc00000fd
    // was our own handler recursing, not the game. CONTINUE_SEARCH from the nested entry lets the SEH in
    // vk_ref_read_block take it, which is what the probe was written for.
    static volatile LONG s_dumpBusy = 0;
    if (InterlockedCompareExchange(&s_dumpBusy, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;

    HMODULE base = GetModuleHandleA(nullptr);
    const char *where = (const char *) info->ExceptionRecord->ExceptionAddress;
    fprintf(stderr, "[VKNOD3D] fault 0x%08lx at %p",
        (unsigned long) info->ExceptionRecord->ExceptionCode, (void *) where);
    if (base) {
        // The RVA is what dumpbin /DISASM labels, so the crash line names the function without a debugger.
        // Outside the main module the number is meaningless but the raw address still names the DLL.
        if (where >= (const char *) base && (uintptr_t) (where - (const char *) base) < vk_fault_image_size(base)) {
            fprintf(stderr, " module=%p rva=0x%llx", (void *) base,
                (unsigned long long) (where - (const char *) base));
        } else {
            vk_fault_module_name(where);
        }
    }
    // 0x406D1388 is vcruntime's GS_FAIL_CODE: a /GS security cookie check, i.e. somebody's stack buffer
    // was written past its end. It carries no AV operands, and calling it an access violation sends the
    // reader hunting for a bad heap pointer instead of the smashed frame that raised it.
    if (info->ExceptionRecord->ExceptionCode == 0x406D1388u) fprintf(stderr, " STACK_BUFFER_OVERRUN(/GS)");
    // vcruntime hands the GS exception two operands - the smashed cookie and the victim frame's stack
    // pointer. The victim rsp is what RaiseException overwrote in ContextRecord, so it is the only start
    // address that still leads to the function whose buffer was written past its end.
    if (info->ExceptionRecord->ExceptionCode == 0x406D1388u) {
        for (unsigned p = 0; p < info->ExceptionRecord->NumberParameters && p < 4; ++p) {
            fprintf(stderr, "[VKNOD3D] gs-param[%u]=%p\n", p,
                (void *) (uintptr_t) info->ExceptionRecord->ExceptionInformation[p]);
        }
    }
    // For an access violation the record also says which pointer was touched and how, and that is what
    // separates "a shell handed back null" from "a shell handed back a zero-length store".
    if (info->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
        info->ExceptionRecord->NumberParameters >= 2) {
        static const char *const ops[3] = {"read", "write", "execute"};
        const unsigned long long op = info->ExceptionRecord->ExceptionInformation[0];
        fprintf(stderr, " %s of %p", op < 3 ? ops[op] : "unknown-op",
            (void *) (uintptr_t) info->ExceptionRecord->ExceptionInformation[1]);
    }
    fprintf(stderr, " - census follows\n");

    // GeneralsX @feature Qoder 18/10/2026 wedge 9.J3: the operand address alone says "a null vtable was
    // followed", it does not say which object. The registers do, so they are printed and handed to the
    // reference ring, which names the class and the stack that dropped the last reference.
    const CONTEXT *ctx = info->ContextRecord;
    void *regs[14];
    if (ctx) {
        regs[0] = (void *) (uintptr_t) ctx->Rax;  regs[1] = (void *) (uintptr_t) ctx->Rbx;
        regs[2] = (void *) (uintptr_t) ctx->Rcx;  regs[3] = (void *) (uintptr_t) ctx->Rdx;
        regs[4] = (void *) (uintptr_t) ctx->Rsi;  regs[5] = (void *) (uintptr_t) ctx->Rdi;
        regs[6] = (void *) (uintptr_t) ctx->R8;   regs[7] = (void *) (uintptr_t) ctx->R9;
        regs[8] = (void *) (uintptr_t) ctx->R10;  regs[9] = (void *) (uintptr_t) ctx->R11;
        regs[10] = (void *) (uintptr_t) ctx->R12; regs[11] = (void *) (uintptr_t) ctx->R13;
        regs[12] = (void *) (uintptr_t) ctx->R14; regs[13] = (void *) (uintptr_t) ctx->R15;
        static const char *const names[14] =
            {"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"};
        fprintf(stderr, "[VKNOD3D] regs");
        for (unsigned i = 0; i < 14; ++i) fprintf(stderr, " %s=%p", names[i], regs[i]);
        fprintf(stderr, "\n rip=%p rsp=%p rbp=%p\n",
            (void *) (uintptr_t) ctx->Rip, (void *) (uintptr_t) ctx->Rsp, (void *) (uintptr_t) ctx->Rbp);
        fflush(stderr);
    } else {
        memset(regs, 0, sizeof(regs));
    }
    VK_Stub_Ref_Dump(regs, 14, "fault");

    if (base) {
        void *frames[24];
        const USHORT depth = RtlCaptureStackBackTrace(0, 24, frames, nullptr);
        for (USHORT i = 0; i < depth; ++i) {
            fprintf(stderr, "[VKNOD3D] stack %2u rva=0x%llx\n", (unsigned) i,
                (unsigned long long) ((const char *) frames[i] - (const char *) base));
        }
    }

    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J5: RtlCaptureStackBackTrace walks THIS
    // handler's chain, so it never shows the frames of the code that faulted - the /GS culprit hides below
    // the vcruntime RaiseException frame. Scan the faulting thread's raw stack for qwords that are return
    // addresses into the main module; resolving those names the smashed frame without a debugger.
    // The first scan tried one straight 8KB read from ctx->Rsp and died "unreadable" on the GS fault, so
    // the walk is now: try every start address we know (rsp, rbx, the GS operand), gate each on
    // VirtualQuery, and read qword-by-qword inside its own __try so one bad page ends the walk with its
    // offset printed instead of losing the whole scan.
    if (base) {
        uintptr_t starts[4];
        unsigned nstarts = 0;
        if (ctx) {
            starts[nstarts++] = (uintptr_t) ctx->Rsp;
            starts[nstarts++] = (uintptr_t) ctx->Rbx;
        }
        if (info->ExceptionRecord->ExceptionCode == 0x406D1388u &&
            info->ExceptionRecord->NumberParameters >= 2) {
            starts[nstarts++] = (uintptr_t) info->ExceptionRecord->ExceptionInformation[1];
        }
        const uintptr_t lo = (uintptr_t) base;
        // Run6 showed the single-region walk dying at one page: committed stack pages grow page by page, so
        // VirtualQuery reports each as its own region. Walk region-by-region instead, and filter candidates
        // by the REAL image size (Run6 mislabeled a KERNELBASE rip as "module rva=0x740f408a" because the
        // old 4GB bound let the whole address space look like the exe).
        const uintptr_t imageSize = vk_fault_image_size(base);
        for (unsigned s = 0; s < nstarts; ++s) {
            const uintptr_t start = starts[s];
            if (!start) continue;
            fprintf(stderr, "[VKNOD3D] fault-stack return candidates from #%u %p:\n", s, (void *) start);
            fflush(stderr);
            uintptr_t sp = start;
            unsigned shown = 0;
            for (unsigned long long walked = 0; walked < 65536 && shown < 40; ) {
                MEMORY_BASIC_INFORMATION mbi;
                if (VirtualQuery((const void *) sp, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
                    !(mbi.Protect & (PAGE_EXECUTE_READWRITE | PAGE_READONLY | PAGE_READWRITE |
                                     PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                    fprintf(stderr, "[VKNOD3D]   stopped at rsp+0x%04llx (region unavailable/not readable)\n",
                        (unsigned long long) walked);
                    break;
                }
                const uintptr_t regionEnd = (uintptr_t) mbi.BaseAddress + mbi.RegionSize;
                for (; sp + 8 <= regionEnd && walked < 65536 && shown < 40; sp += 8, walked += 8) {
                    uintptr_t v = 0;
                    bool bad = false;
                    __try { v = *(const uintptr_t *) sp; }
                    __except (EXCEPTION_EXECUTE_HANDLER) { bad = true; }
                    if (bad) {
                        fprintf(stderr, "[VKNOD3D]   unreadable at rsp+0x%04llx\n",
                            (unsigned long long) (sp - start));
                        sp = regionEnd;  // leave the inner loop; the region walk below stops on requery
                        break;
                    }
                    if (v <= lo) continue;
                    const uintptr_t d = v - lo;
                    if (d < imageSize) {
                        fprintf(stderr, "[VKNOD3D]   rsp+0x%04llx rva=0x%llx\n",
                            (unsigned long long) (sp - start), (unsigned long long) d);
                        ++shown;
                        continue;
                    }
                    // The GS raiser turned out to live entirely in other images, so exe-only filtering hid the
                    // whole chain. Anything that lands inside a committed EXECUTE page of a mapped image is a
                    // plausible return address too and gets named by its DLL.
                    MEMORY_BASIC_INFORMATION q;
                    if (VirtualQuery((const void *) v, &q, sizeof(q)) != 0 && q.State == MEM_COMMIT &&
                        (q.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                      PAGE_EXECUTE_WRITECOPY)) && q.AllocationBase) {
                        char nm[MAX_PATH];
                        if (GetModuleFileNameA((HMODULE) q.AllocationBase, nm, sizeof(nm)) > 0) {
                            fprintf(stderr, "[VKNOD3D]   rsp+0x%04llx %s+0x%llx\n",
                                (unsigned long long) (sp - start), nm,
                                (unsigned long long) (v - (uintptr_t) q.AllocationBase));
                            ++shown;
                        }
                    }
                }
                if (sp >= regionEnd && walked < 65536 && shown < 40) {
                    // Page-grown stacks commit each page as its own region; cross the boundary and keep going.
                    continue;
                }
            }
            fflush(stderr);
        }
    }
    VK_Stub_Census_Dump("fault");
    InterlockedExchange(&s_dumpBusy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

IDirect3D9 *VK_Stub_Create_D3D9(HWND hwnd)
{
    stub_build_modes(hwnd);
    vk_wp_init();
    AddVectoredExceptionHandler(1, vk_stub_crash_dump);
    return new VKD3D9(hwnd);
}

// ------------------------------------------------------------------ device shell

namespace {

// Measured, not assumed: taken from the d3d9 path with GENERALSX_VK_CAPDUMP=1 ([VKRS] lines, DXVK on the
// NVIDIA driver this tree is validated against), read off a device before Do_Onetime_Device_Dependent_Inits
// and before any engine code has written a state. A state that is absent here reads back as zero, which is
// what the same device reports for it. Once d3d9 is gone this table *is* the machine's initial state, so a
// wrong entry is a behaviour change - which is why it is a transcription of a log line and not a list of
// documented defaults.
struct VKStubStateDefault { unsigned short index; unsigned long value; };

const VKStubStateDefault VK_RS_DEFAULTS[] = {
    {0x007, 0x00000001UL}, {0x008, 0x00000003UL}, {0x009, 0x00000002UL}, {0x00e, 0x00000001UL},
    {0x010, 0x00000001UL}, {0x013, 0x00000002UL}, {0x014, 0x00000001UL}, {0x016, 0x00000003UL},
    {0x017, 0x00000004UL}, {0x019, 0x00000008UL}, {0x025, 0x3f800000UL}, {0x026, 0x3f800000UL},
    {0x035, 0x00000001UL}, {0x036, 0x00000001UL}, {0x037, 0x00000001UL}, {0x038, 0x00000008UL},
    {0x03a, 0xffffffffUL}, {0x03b, 0xffffffffUL}, {0x03c, 0xffffffffUL}, {0x088, 0x00000001UL},
    {0x089, 0x00000001UL}, {0x08d, 0x00000001UL}, {0x08e, 0x00000001UL}, {0x091, 0x00000001UL},
    {0x092, 0x00000002UL}, {0x09a, 0x3f800000UL}, {0x09b, 0x3f800000UL}, {0x09e, 0x3f800000UL},
    {0x0a1, 0x00000001UL}, {0x0a2, 0xffffffffUL}, {0x0a6, 0x44fffe00UL}, {0x0a8, 0x0000000fUL},
    {0x0ab, 0x00000001UL}, {0x0ac, 0x00000003UL}, {0x0ad, 0x00000001UL}, {0x0b2, 0x3f800000UL},
    {0x0b3, 0x3f800000UL}, {0x0b6, 0x3f800000UL}, {0x0ba, 0x00000001UL}, {0x0bb, 0x00000001UL},
    {0x0bc, 0x00000001UL}, {0x0bd, 0x00000008UL}, {0x0be, 0x0000000fUL}, {0x0bf, 0x0000000fUL},
    {0x0c0, 0x0000000fUL}, {0x0c1, 0xffffffffUL}, {0x0cf, 0x00000002UL}, {0x0d0, 0x00000001UL},
    {0x0d1, 0x00000001UL},
};

// Stage 0 differs from the rest, which is the D3D9 convention (MODULATE on the first stage, DISABLE after)
// and the reason a single loop would be wrong.
const VKStubStateDefault VK_TSS_DEFAULTS_FIRST[] = {
    {0x01, 0x00000004UL}, {0x02, 0x00000002UL}, {0x03, 0x00000001UL}, {0x04, 0x00000002UL},
    {0x05, 0x00000002UL}, {0x06, 0x00000001UL}, {0x1a, 0x00000001UL}, {0x1b, 0x00000001UL},
    {0x1c, 0x00000001UL},
};
const VKStubStateDefault VK_TSS_DEFAULTS_LATER[] = {
    {0x01, 0x00000001UL}, {0x02, 0x00000001UL}, {0x03, 0x00000001UL}, {0x04, 0x00000001UL},
    {0x05, 0x00000002UL}, {0x06, 0x00000001UL}, {0x1a, 0x00000001UL}, {0x1b, 0x00000001UL},
    {0x1c, 0x00000001UL},
};
const VKStubStateDefault VK_SAMP_DEFAULTS[] = {
    {0x01, 0x00000001UL}, {0x02, 0x00000001UL}, {0x03, 0x00000001UL}, {0x05, 0x00000001UL},
    {0x06, 0x00000001UL}, {0x0a, 0x00000001UL}, {0x0f, 0x00000001UL},
};

} // namespace

VKD3DDevice::VKD3DDevice(HWND hwnd, const D3DPRESENT_PARAMETERS &pp, DWORD behaviorFlags)
{
    m_hwnd = hwnd;
    m_pp = pp;
    m_behaviorFlags = behaviorFlags;
    ::ZeroMemory(m_rs, sizeof(m_rs));
    ::ZeroMemory(m_tss, sizeof(m_tss));
    ::ZeroMemory(m_samp, sizeof(m_samp));
    // GeneralsX @bugfix Qoder 19/10/2026 BUG-03: the d3d9 device opens with an identity transform stack,
    // so a never-set slot must echo identity, not the zero the other state tables start from (the terrain
    // noise passes invert what they read back, and inverting a zero matrix is how the ground went NaN).
    for (unsigned i = 0; i < VK_TS_SLOTS; ++i) {
        ::ZeroMemory(&m_ts[i], sizeof(D3DMATRIX));
        m_ts[i]._11 = m_ts[i]._22 = m_ts[i]._33 = m_ts[i]._44 = 1.0f;
    }

    for (unsigned i = 0; i < sizeof(VK_RS_DEFAULTS) / sizeof(VK_RS_DEFAULTS[0]); ++i) {
        if (VK_RS_DEFAULTS[i].index < VK_RS_SLOTS) m_rs[VK_RS_DEFAULTS[i].index] = VK_RS_DEFAULTS[i].value;
    }
    for (unsigned stage = 0; stage < VK_STAGE_COUNT; ++stage) {
        const VKStubStateDefault *tss = stage == 0 ? VK_TSS_DEFAULTS_FIRST : VK_TSS_DEFAULTS_LATER;
        const unsigned count = stage == 0 ? sizeof(VK_TSS_DEFAULTS_FIRST) / sizeof(VK_TSS_DEFAULTS_FIRST[0])
                                         : sizeof(VK_TSS_DEFAULTS_LATER) / sizeof(VK_TSS_DEFAULTS_LATER[0]);
        for (unsigned i = 0; i < count; ++i) {
            if (tss[i].index < VK_TSS_SLOTS) m_tss[stage][tss[i].index] = tss[i].value;
        }
        // Measured on every stage but the first: the transform flag reports the stage ordinal itself.
        if (stage != 0 && stage < VK_TSS_SLOTS) m_tss[stage][0x0b] = stage;

        for (unsigned i = 0; i < sizeof(VK_SAMP_DEFAULTS) / sizeof(VK_SAMP_DEFAULTS[0]); ++i) {
            if (VK_SAMP_DEFAULTS[i].index < VK_SAMP_SLOTS) m_samp[stage][VK_SAMP_DEFAULTS[i].index] = VK_SAMP_DEFAULTS[i].value;
        }
    }
}

HRESULT VKD3DDevice::Reset(D3DPRESENT_PARAMETERS *pPresentationParameters)
{
    if (!pPresentationParameters) return D3DERR_INVALIDCALL;
    m_pp = *pPresentationParameters;
    return D3D_OK;
}

HRESULT VKD3DDevice::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParameters)
{
    if (!pParameters) return D3DERR_INVALIDCALL;
    ::ZeroMemory(pParameters, sizeof(*pParameters));
    pParameters->AdapterOrdinal = 0;
    pParameters->DeviceType = D3DDEVTYPE_HAL;
    pParameters->hFocusWindow = m_hwnd;
    // Vertex_Processing_Behavior is what the facade computed at creation; handing it back is what lets a
    // later wedge read the processing mode off the device instead of a shadow it no longer has.
    pParameters->BehaviorFlags = m_behaviorFlags;
    return D3D_OK;
}

HRESULT VKD3DDevice::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode)
{
    (void)iSwapChain;
    if (!pMode) return D3DERR_INVALIDCALL;
    pMode->Width = m_pp.BackBufferWidth;
    pMode->Height = m_pp.BackBufferHeight;
    pMode->RefreshRate = m_pp.FullScreen_RefreshRateInHz ? m_pp.FullScreen_RefreshRateInHz : 60;
    pMode->Format = m_pp.BackBufferFormat;
    return D3D_OK;
}

// ------------------------------------------------------------------ resource shells

unsigned VK_Stub_Mip_Dim(unsigned baseDim, unsigned level)
{
    unsigned d = baseDim ? baseDim : 1;
    for (unsigned i = 0; i < level && d > 1; ++i) d >>= 1;
    return d ? d : 1;
}

// Bytes in one row of a level of the given width, for every format the engine asks for. Block-compressed
// formats count 4x4 blocks, which is also why a compressed level may only be locked whole. An unlisted
// format returns false and the caller refuses the lock instead of guessing a stride - a wrong stride
// corrupts pixels silently, a refused lock shows up as a census line.
static bool stub_stride(D3DFORMAT fmt, unsigned width, unsigned* bytesPerRow)
{
    switch (fmt) {
    case D3DFMT_R3G3B2:
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
        *bytesPerRow = width; return true;
    case D3DFMT_R5G6B5:
    case D3DFMT_X1R5G5B5:
    case D3DFMT_A1R5G5B5:
    case D3DFMT_A4R4G4B4:
    case D3DFMT_A8P8:
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
    case D3DFMT_L6V5U5:
    case D3DFMT_D16:
    case D3DFMT_D16_LOCKABLE:
        *bytesPerRow = width * 2u; return true;
    case D3DFMT_R8G8B8:
        *bytesPerRow = width * 3u; return true;
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10:
    case D3DFMT_A2B10G10R10:
    case D3DFMT_G16R16:
    case D3DFMT_G16R16F:
    case D3DFMT_R16F:
    case D3DFMT_R32F:
    case D3DFMT_D24X8:
    case D3DFMT_D24S8:
    case D3DFMT_D24X4S4:
    case D3DFMT_D32:
        *bytesPerRow = width * 4u; return true;
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F:
        *bytesPerRow = width * 8u; return true;
    case D3DFMT_A32B32G32R32F:
        *bytesPerRow = width * 16u; return true;
    case D3DFMT_DXT1:
        *bytesPerRow = ((width + 3u) / 4u) * 8u; return true;
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        *bytesPerRow = ((width + 3u) / 4u) * 16u; return true;
    default:
        return false;
    }
}

// A partial rect lock is only meaningful for a per-row format; D3D9 rejects it for block-compressed
// levels because a 4x4 block cannot be split across a sub-rect.
static bool stub_stride_is_uncompressed(D3DFORMAT fmt)
{
    return fmt != D3DFMT_DXT1 && fmt != D3DFMT_DXT2 && fmt != D3DFMT_DXT3 &&
           fmt != D3DFMT_DXT4 && fmt != D3DFMT_DXT5;
}

static void stub_log_unknown_format(D3DFORMAT fmt)
{
    static unsigned seen = 0;
    if (seen < 16) {
        ++seen;
        fprintf(stderr, "[VKNOD3D] no stride known for format 0x%08x - lock refused\n", (unsigned) fmt);
        fflush(stderr);
    }
}

VKD3DSurface::VKD3DSurface(VKD3DDevice *dev, const D3DSURFACE_DESC &desc, IUnknown *container)
{
    m_dev = dev;
    m_desc = desc;
    m_container = container;
    m_bits = nullptr;
    m_region = nullptr;
    m_pitch = 0;
    m_tex = nullptr;
    m_texLevel = 0;
    if (m_container) m_container->AddRef();
}

VKD3DSurface::~VKD3DSurface()
{
    // A level view owns no bytes; the texture's store outlives every view of it.
    if (!m_tex) vk_stub_store_free(m_region, m_bits);
    if (m_container) m_container->Release();
}

void VKD3DSurface::set_texture_level_view(VKD3DTexture *tex, unsigned level)
{
    m_tex = tex;
    m_texLevel = level;
}

bool VKD3DSurface::cpu_store(unsigned char **bits, unsigned *pitch)
{
    if (!bits || !pitch) return false;
    if (m_tex) return m_tex->cpu_store_level(m_texLevel, bits, pitch);
    if (!m_bits) {
        unsigned perRow = 0;
        if (!stub_stride(m_desc.Format, m_desc.Width, &perRow)) { stub_log_unknown_format(m_desc.Format); return false; }
        vk_stub_store_alloc(m_bits, m_region, (size_t) perRow * m_desc.Height);
        if (!m_bits) return false;
        m_pitch = perRow;
    }
    *bits = m_bits;
    *pitch = m_pitch;
    return true;
}

VKD3DTexture::VKD3DTexture(VKD3DDevice *dev, UINT width, UINT height, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool)
{
    m_dev = dev;
    m_width = width;
    m_height = height;
    m_usage = usage;
    m_format = format;
    m_pool = pool;
    m_lod = 0;
    // Wedge 9.K: Levels=0 is d3d9 for "the whole mip chain", and DXVK used to generate that chain on the
    // device upload for MANAGED textures. The stub has no device upload, so it expands the level count
    // here and box-filters the chain when the writer unlocks level 0 (generate_mips_from_level0).
    m_auto_mip = (levels == 0);
    if (!m_auto_mip) {
        m_levels = levels;
    } else {
        m_levels = 1;
        unsigned w = width ? width : 1;
        unsigned h = height ? height : 1;
        while (w > 1 || h > 1) {
            w = VK_Stub_Mip_Dim(w, 1);
            h = VK_Stub_Mip_Dim(h, 1);
            ++m_levels;
            if (m_levels >= 16) break;
        }
    }
    for (unsigned i = 0; i < 16; ++i) { m_store[i].bits = nullptr; m_store[i].pitch = 0; m_store[i].region = nullptr; m_store[i].written = 0; }
}

VKD3DTexture::~VKD3DTexture()
{
    // GeneralsX @bugfix Qoder 19/10/2026 wedge 11: the Vulkan upload mirror is keyed by this pointer, and
    // a texture created later can land on this same address. If the mirror outlives its owner, the new
    // owner is sampled through the dead texture's image - which is how the sun shadow map inherited
    // tsmorningw_nm.tga's mirror and the terrain receiver went black. Drop it here, at the real death.
    VKTex_Drop(this);
    for (unsigned i = 0; i < 16; ++i) vk_stub_store_free(m_store[i].region, m_store[i].bits);
}

VKD3DVertexBuffer::VKD3DVertexBuffer(VKD3DDevice *dev, UINT length, DWORD usage, DWORD fvf, D3DPOOL pool)
{
    m_desc.Size = length;
    m_desc.Usage = usage;
    m_desc.FVF = fvf;
    m_desc.Pool = pool;
    m_dev = dev;
    m_bits = nullptr;
    m_region = nullptr;
    // wedge 9.J2d: the dynamic pool's DX9 object went out from under a second WriteLock in the same frame
    // (vptr reads 0 at the later lock while it was a live vtable at the earlier one). These two lines name
    // the death and any rebirth, which is the only way a no-d3d9 run can show who released the pool.
    if (VK_NOD3D_Enabled()) {
        fprintf(stderr, "[VKNOD3D] VB born this=%p len=%u usage=%08x pool=%u\n",
            (void *) this, length, (unsigned) usage, (unsigned) pool);
        fflush(stderr);
    }
}

VKD3DVertexBuffer::~VKD3DVertexBuffer() {
    if (VK_NOD3D_Enabled()) {
        fprintf(stderr, "[VKNOD3D] VB died this=%p len=%u refs=%ld\n",
            (void *) this, (unsigned) m_desc.Size, (long) m_vkStubRefs);
        fflush(stderr);
    }
    vk_stub_store_free(m_region, m_bits);
}

VKD3DIndexBuffer::VKD3DIndexBuffer(VKD3DDevice *dev, UINT length, DWORD usage, D3DFORMAT format, D3DPOOL pool)
{
    m_desc.Size = length;
    m_desc.Usage = usage;
    m_desc.Format = format;
    m_desc.Pool = pool;
    m_dev = dev;
    m_bits = nullptr;
    m_region = nullptr;
}

VKD3DIndexBuffer::~VKD3DIndexBuffer() { vk_stub_store_free(m_region, m_bits); }

// wedge 9.K: the store a lock hands out. Zeroed, so "black" means "nobody wrote this texture" instead of
// "the allocator happened to hand back garbage".
bool VKD3DTexture::ensure_level(unsigned level)
{
    if (level >= m_levels) return false;
    Level_Store &s = m_store[level];
    if (s.bits) return true;

    unsigned perRow = 0;
    const unsigned w = VK_Stub_Mip_Dim(m_width, (unsigned) level);
    const unsigned h = VK_Stub_Mip_Dim(m_height, (unsigned) level);
    if (!stub_stride(m_format, w, &perRow)) { stub_log_unknown_format(m_format); return false; }

    vk_stub_store_alloc(s.bits, s.region, (size_t) perRow * h);
    if (!s.bits) return false;
    s.pitch = perRow;
    return true;
}

bool VKD3DTexture::cpu_store_level(unsigned level, unsigned char **bits, unsigned *pitch)
{
    if (!bits || !pitch) return false;
    if (!ensure_level(level)) return false;
    *bits = m_store[level].bits;
    *pitch = m_store[level].pitch;
    return true;
}

// Wedge 9.K: hand the whole CPU store to the keyed Vulkan upload without going through LockRect. The
// device-side copy movers land bytes in DEFAULT-pool levels the mirror entry could never re-lock, and
// the key here is the same IDirect3DTexture9* address every Set_DX8_Texture/VKTex caller already uses,
// so the upload converges into the image the draw path samples.
void VKD3DTexture::mirror_to_vulkan()
{
    if (!VKTex_Ready()) return;
    if (!m_levels || m_levels > 16) return;

    VKTex_Mip_Source src[16];
    for (unsigned i = 0; i < m_levels; ++i) {
        if (!ensure_level(i)) return;
        src[i].bits = m_store[i].bits;
        src[i].pitch = m_store[i].pitch;
    }
    VKTex_Upload_2D(static_cast<IDirect3DTexture9 *>(this), m_width, m_height, m_levels,
        (int)D3DFormat_To_WW3DFormat(m_format), src);
}

// Wedge 9.K: the mip chain DXVK used to build at MANAGED upload, box-filtered here when the level-0
// writer unlocks. Only formats whose channels average cleanly are touched; anything else logs once and
// keeps zeroed upper levels, which is louder in the forensic log than a wrong-colored blur would be.
void VKD3DTexture::generate_mips_from_level0()
{
    if (!m_store[0].bits) return;

    // The loader fills every level itself (it writable-locks the whole chain), and DXVK only autogen'd
    // when the caller left the chain alone. Ownership is decided the same way here.
    for (unsigned l = 1; l < m_levels; ++l) {
        if (m_store[l].written) return;
    }

    unsigned channels = 0;
    switch (m_format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        channels = 4; break;
    case D3DFMT_R8G8B8:
        channels = 3; break;
    case D3DFMT_A8L8:
    case D3DFMT_V8U8:
        channels = 2; break;
    case D3DFMT_A8:
    case D3DFMT_L8:
    case D3DFMT_P8:
    case D3DFMT_R3G3B2:
        channels = 1; break;
    case D3DFMT_A4R4G4B4:
        channels = 16; break;
    default:
        break;
    }
    if (!channels) {
        static unsigned seen = 0;
        if (seen < 8) {
            ++seen;
            // Fourcc letters when the low bytes are letters, plus shape/usage, so the next sighting
            // names the texture family instead of a bare number (0x57 is not in the d3d9 enum).
            const unsigned f = (unsigned) m_format;
            const char c0 = (char)(f & 0xFF), c1 = (char)((f >> 8) & 0xFF);
            const char c2 = (char)((f >> 16) & 0xFF), c3 = (char)((f >> 24) & 0xFF);
            const bool fourccish =
                (c0 >= 0x40 && c0 < 0x7F) && (c1 >= 0x40 && c1 < 0x7F) &&
                (c2 >= 0x40 && c2 < 0x7F) && (c3 >= 0x40 && c3 < 0x7F);
            fprintf(stderr, "[VKNOD3D] auto-mip unsupported fmt=0x%08x%s%c%c%c%c%s usage=0x%x %ux%u"
                " - upper levels stay zero\n",
                f, fourccish ? " ('" : " {", c0, c1, c2, c3, fourccish ? "')" : "}",
                m_usage, m_width, m_height);
            fflush(stderr);
        }
        return;
    }

    for (unsigned l = 0; l + 1 < m_levels; ++l) {
        const unsigned sw = VK_Stub_Mip_Dim(m_width, l);
        const unsigned sh = VK_Stub_Mip_Dim(m_height, l);
        const unsigned dw = VK_Stub_Mip_Dim(m_width, l + 1);
        const unsigned dh = VK_Stub_Mip_Dim(m_height, l + 1);
        if (!ensure_level(l) || !ensure_level(l + 1)) return;
        const unsigned char *sb = m_store[l].bits;
        const unsigned sp = m_store[l].pitch;
        unsigned char *db = m_store[l + 1].bits;
        const unsigned dp = m_store[l + 1].pitch;

        for (unsigned y = 0; y < dh; ++y) {
            const unsigned y0 = 2 * y < sh ? 2 * y : sh - 1;
            const unsigned y1 = 2 * y + 1 < sh ? 2 * y + 1 : sh - 1;
            for (unsigned x = 0; x < dw; ++x) {
                const unsigned x0 = 2 * x < sw ? 2 * x : sw - 1;
                const unsigned x1 = 2 * x + 1 < sw ? 2 * x + 1 : sw - 1;
                if (channels == 16) {
                    const unsigned short *s00 = (const unsigned short *)(sb + (size_t) y0 * sp + (size_t) x0 * 2);
                    const unsigned short *s10 = (const unsigned short *)(sb + (size_t) y0 * sp + (size_t) x1 * 2);
                    const unsigned short *s01 = (const unsigned short *)(sb + (size_t) y1 * sp + (size_t) x0 * 2);
                    const unsigned short *s11 = (const unsigned short *)(sb + (size_t) y1 * sp + (size_t) x1 * 2);
                    unsigned short out = 0;
                    for (unsigned shift = 0; shift < 16; shift += 4) {
                        const unsigned a = (*s00 >> shift) & 0xFu;
                        const unsigned b = (*s10 >> shift) & 0xFu;
                        const unsigned c = (*s01 >> shift) & 0xFu;
                        const unsigned d = (*s11 >> shift) & 0xFu;
                        out = (unsigned short) (out | (((a + b + c + d + 2u) >> 2) << shift));
                    }
                    *(unsigned short *) (db + (size_t) y * dp + (size_t) x * 2) = out;
                } else {
                    for (unsigned c = 0; c < channels; ++c) {
                        const unsigned v00 = sb[(size_t) y0 * sp + (size_t) x0 * channels + c];
                        const unsigned v10 = sb[(size_t) y0 * sp + (size_t) x1 * channels + c];
                        const unsigned v01 = sb[(size_t) y1 * sp + (size_t) x0 * channels + c];
                        const unsigned v11 = sb[(size_t) y1 * sp + (size_t) x1 * channels + c];
                        db[(size_t) y * dp + (size_t) x * channels + c] =
                            (unsigned char) ((v00 + v10 + v01 + v11 + 2u) >> 2);
                    }
                }
            }
        }
    }
}

void VKD3DDevice::back_buffer_desc(D3DSURFACE_DESC *pDesc) const
{
    ::ZeroMemory(pDesc, sizeof(*pDesc));
    unsigned w = (unsigned) m_pp.BackBufferWidth;
    unsigned h = (unsigned) m_pp.BackBufferHeight;
    if (!w || !h) {
        RECT rc;
        if (m_hwnd && GetClientRect(m_hwnd, &rc)) {
            w = (unsigned) (rc.right - rc.left);
            h = (unsigned) (rc.bottom - rc.top);
        }
        if (!w) w = 1024;
        if (!h) h = 768;
    }
    D3DFORMAT fmt = m_pp.BackBufferFormat;
    if (fmt == D3DFMT_UNKNOWN) {
        D3DDISPLAYMODE mode;
        ::ZeroMemory(&mode, sizeof(mode));
        mode.Format = D3DFMT_UNKNOWN;
        stub_current_mode(&mode, D3DFMT_UNKNOWN);
        fmt = mode.Format;
    }
    pDesc->Format = fmt;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = D3DUSAGE_RENDERTARGET;
    pDesc->Pool = D3DPOOL_DEFAULT;
    pDesc->MultiSampleType = m_pp.MultiSampleType;
    pDesc->MultiSampleQuality = m_pp.MultiSampleQuality;
    pDesc->Width = w;
    pDesc->Height = h;
}

HRESULT VKD3DDevice::GetDeviceCaps(D3DCAPS9 *pCaps)
{
    if (!pCaps) return D3DERR_INVALIDCALL;
    VK_Stub_Fill_Caps(pCaps, 0, D3DDEVTYPE_HAL);
    return D3D_OK;
}

HRESULT VKD3DDevice::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    if ((DWORD) State < VK_RS_SLOTS) m_rs[State] = Value;
    return D3D_OK;
}

HRESULT VKD3DDevice::GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(pValue, "VKD3DDevice::GetRenderState");
    *pValue = ((DWORD) State < VK_RS_SLOTS) ? m_rs[State] : 0;
    return D3D_OK;
}

// GeneralsX @bugfix Qoder 19/10/2026 BUG-03: transform stack storage, see the header note. The bytes are
// the ones DX8Wrapper::_Set_DX8_Transform would have handed the d3d9 device, verbatim and un-transposed,
// so an engine read-back (the terrain noise inverse, the water relays, the PPL matrix pair) gets exactly
// what the d3d9 path got.
HRESULT VKD3DDevice::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix)
{
    if (!pMatrix) return D3DERR_INVALIDCALL;
    if ((DWORD) State < VK_TS_SLOTS) m_ts[State] = *pMatrix;
    return D3D_OK;
}

HRESULT VKD3DDevice::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix)
{
    if (!pMatrix) return D3DERR_INVALIDCALL;
    if ((DWORD) State < VK_TS_SLOTS) *pMatrix = m_ts[State];
    else ::ZeroMemory(pMatrix, sizeof(D3DMATRIX));
    return D3D_OK;
}

HRESULT VKD3DDevice::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{
    if (Stage < VK_STAGE_COUNT && (DWORD) Type < VK_TSS_SLOTS) m_tss[Stage][Type] = Value;
    return D3D_OK;
}

HRESULT VKD3DDevice::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(pValue, "VKD3DDevice::GetTextureStageState");
    *pValue = (Stage < VK_STAGE_COUNT && (DWORD) Type < VK_TSS_SLOTS) ? m_tss[Stage][Type] : 0;
    return D3D_OK;
}

HRESULT VKD3DDevice::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    if (Sampler < VK_STAGE_COUNT && (DWORD) Type < VK_SAMP_SLOTS) m_samp[Sampler][Type] = Value;
    return D3D_OK;
}

HRESULT VKD3DDevice::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue)
{
    if (!pValue) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(pValue, "VKD3DDevice::GetSamplerState");
    *pValue = (Sampler < VK_STAGE_COUNT && (DWORD) Type < VK_SAMP_SLOTS) ? m_samp[Sampler][Type] : 0;
    return D3D_OK;
}

// The census would otherwise blame DX8Caps for a zeroed caps3, SHADER_BUDGET for a 0.0 shader version and
// DEPTH_BIAS for a device that "does not echo" - all three of which were really just this one call falling
// through to the inert body. Answering it is what makes the next fault worth reading.
HRESULT VKD3DDevice::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format,
    D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle)
{
    (void)pSharedHandle;
    if (!ppTexture) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateTexture");
    VK_Stub_Store_Note(ppTexture, "VKD3DDevice::CreateTexture");
    *ppTexture = new VKD3DTexture(this, Width, Height, Levels, Usage, Format, Pool);
    return D3D_OK;
}

HRESULT VKD3DDevice::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage,
    D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle)
{
    (void)Width; (void)Height; (void)Depth; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)pSharedHandle;
    if (!ppVolumeTexture) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateVolumeTexture");
    VK_Stub_Store_Note(ppVolumeTexture, "VKD3DDevice::CreateVolumeTexture");
    *ppVolumeTexture = new VKStubVolumeTexture();
    return D3D_OK;
}

HRESULT VKD3DDevice::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle)
{
    (void)EdgeLength; (void)Levels; (void)Usage; (void)Format; (void)Pool; (void)pSharedHandle;
    if (!ppCubeTexture) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateCubeTexture");
    VK_Stub_Store_Note(ppCubeTexture, "VKD3DDevice::CreateCubeTexture");
    *ppCubeTexture = new VKStubCubeTexture();
    return D3D_OK;
}

HRESULT VKD3DDevice::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool,
    IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle)
{
    (void)pSharedHandle;
    if (!ppVertexBuffer) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateVertexBuffer");
    VK_Stub_Store_Note(ppVertexBuffer, "VKD3DDevice::CreateVertexBuffer");
    *ppVertexBuffer = new VKD3DVertexBuffer(this, Length, Usage, FVF, Pool);
    return D3D_OK;
}

HRESULT VKD3DDevice::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle)
{
    (void)pSharedHandle;
    if (!ppIndexBuffer) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateIndexBuffer");
    VK_Stub_Store_Note(ppIndexBuffer, "VKD3DDevice::CreateIndexBuffer");
    *ppIndexBuffer = new VKD3DIndexBuffer(this, Length, Usage, Format, Pool);
    return D3D_OK;
}

// GeneralsX @feature Qoder 19/10/2026 wedge 9.P shader sovereignty: a created shader is only ever a
// key for the program registry (handle -> translated twin) and a token for the DX8Wrapper mirrors, so
// the base VKStub shells carry it completely. The census keeps counting the creation: the SPIR-V side
// still has to grow a pipeline variant per tag before these handles do GPU work.
HRESULT VKD3DDevice::CreateVertexShader(CONST DWORD *pFunction, IDirect3DVertexShader9 **ppShader)
{
    (void)pFunction;
    if (!ppShader) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateVertexShader");
    VK_Stub_Store_Note(ppShader, "VKD3DDevice::CreateVertexShader");
    *ppShader = new VKStubVertexShader();
    return D3D_OK;
}

HRESULT VKD3DDevice::CreatePixelShader(CONST DWORD *pFunction, IDirect3DPixelShader9 **ppShader)
{
    (void)pFunction;
    if (!ppShader) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreatePixelShader");
    VK_Stub_Store_Note(ppShader, "VKD3DDevice::CreatePixelShader");
    *ppShader = new VKStubPixelShader();
    return D3D_OK;
}

HRESULT VKD3DDevice::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample,
    DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
    (void)Lockable; (void)pSharedHandle;
    if (!ppSurface) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateRenderTarget");
    VK_Stub_Store_Note(ppSurface, "VKD3DDevice::CreateRenderTarget");
    D3DSURFACE_DESC desc;
    ::ZeroMemory(&desc, sizeof(desc));
    desc.Format = Format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = D3DUSAGE_RENDERTARGET;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.MultiSampleType = MultiSample;
    desc.MultiSampleQuality = MultisampleQuality;
    desc.Width = Width;
    desc.Height = Height;
    *ppSurface = new VKD3DSurface(this, desc, nullptr);
    return D3D_OK;
}

HRESULT VKD3DDevice::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format,
    D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
    (void)Discard; (void)pSharedHandle;
    if (!ppSurface) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateDepthStencilSurface");
    VK_Stub_Store_Note(ppSurface, "VKD3DDevice::CreateDepthStencilSurface");
    D3DSURFACE_DESC desc;
    ::ZeroMemory(&desc, sizeof(desc));
    desc.Format = Format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = D3DUSAGE_DEPTHSTENCIL;
    desc.Pool = D3DPOOL_DEFAULT;
    desc.MultiSampleType = MultiSample;
    desc.MultiSampleQuality = MultisampleQuality;
    desc.Width = Width;
    desc.Height = Height;
    *ppSurface = new VKD3DSurface(this, desc, nullptr);
    return D3D_OK;
}

HRESULT VKD3DDevice::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool,
    IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle)
{
    (void)pSharedHandle;
    if (!ppSurface) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "CreateOffscreenPlainSurface");
    VK_Stub_Store_Note(ppSurface, "VKD3DDevice::CreateOffscreenPlainSurface");
    D3DSURFACE_DESC desc;
    ::ZeroMemory(&desc, sizeof(desc));
    desc.Format = Format;
    desc.Type = D3DRTYPE_SURFACE;
    desc.Usage = 0;
    desc.Pool = Pool;
    desc.MultiSampleType = D3DMULTISAMPLE_NONE;
    desc.Width = Width;
    desc.Height = Height;
    *ppSurface = new VKD3DSurface(this, desc, nullptr);
    return D3D_OK;
}

HRESULT VKD3DDevice::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type,
    IDirect3DSurface9 **ppBackBuffer)
{
    (void)iSwapChain; (void)Type;
    if (!ppBackBuffer) return D3DERR_INVALIDCALL;
    VK_Stub_Note("IDirect3DDevice9", "GetBackBuffer");
    VK_Stub_Store_Note(ppBackBuffer, "VKD3DDevice::GetBackBuffer");
    if (iBackBuffer != 0) { *ppBackBuffer = nullptr; return D3DERR_NOTAVAILABLE; }
    D3DSURFACE_DESC desc;
    back_buffer_desc(&desc);
    *ppBackBuffer = new VKD3DSurface(this, desc, nullptr);
    return D3D_OK;
}

// ---- wedge 9.K: the device-side pixel movers -----------------------------------
//
// d3d9 routed DEFAULT-pool CPU traffic through these copies, not through LockRect, and the facade's
// upload paths (DX8Wrapper::_Copy_DX8_Rects above all) branch on their success. Under the stub the
// bytes live in each resource's store, so a mover is a row copy between stores - plus the mirror
// feed, because a DEFAULT destination level can never be re-locked by VKTex_Mirror_D3D_Texture and
// would otherwise diverge from the Vulkan image the draw path samples.

namespace {

HRESULT stub_surface_copy(VKD3DSurface *src, CONST RECT *pSrcRect, VKD3DSurface *dst, CONST POINT *pDstPoint)
{
    const D3DSURFACE_DESC &sd = src->Desc();
    const D3DSURFACE_DESC &dd = dst->Desc();
    // Both movers demand identical formats (measured on d3d9/DXVK: a mismatch is INVALIDCALL, and
    // SurfaceClass::Copy routes conversions to D3DXLoadSurfaceFromSurface instead).
    if (sd.Format != dd.Format) return D3DERR_INVALIDCALL;

    unsigned char *sb = nullptr;
    unsigned sp = 0;
    unsigned char *db = nullptr;
    unsigned dp = 0;
    if (!src->cpu_store(&sb, &sp) || !dst->cpu_store(&db, &dp)) return D3DERR_INVALIDCALL;

    // A compressed level copies whole block rows: the 4x4 grid makes sub-rect byte math wrong, and every
    // caller in this tree with compressed traffic passes the full level anyway.
    if (!stub_stride_is_uncompressed(sd.Format)) {
        const size_t span = sp < dp ? sp : dp;
        const unsigned rows = sd.Height < dd.Height ? sd.Height : dd.Height;
        ::memcpy(db, sb, span * rows);
        return D3D_OK;
    }

    RECT r = { 0, 0, (LONG) sd.Width, (LONG) sd.Height };
    if (pSrcRect) r = *pSrcRect;
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > (LONG) sd.Width) r.right = (LONG) sd.Width;
    if (r.bottom > (LONG) sd.Height) r.bottom = (LONG) sd.Height;

    POINT p = { 0, 0 };
    if (pDstPoint) p = *pDstPoint;
    if (p.x < 0 || p.y < 0) return D3DERR_INVALIDCALL;

    long w = r.right - r.left;
    long h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return D3D_OK;
    // The facade trims its own rect lists; the stub trims too, so an overrunning caller corrupts nothing.
    if (p.x + w > (LONG) dd.Width) w = (LONG) dd.Width - p.x;
    if (p.y + h > (LONG) dd.Height) h = (LONG) dd.Height - p.y;
    if (w <= 0 || h <= 0) return D3D_OK;

    const unsigned bytesPerPixel = sp / (sd.Width ? sd.Width : 1);
    for (long y = 0; y < h; ++y) {
        ::memcpy(db + (size_t) (p.y + y) * dp + (size_t) p.x * bytesPerPixel,
            sb + (size_t) (r.top + y) * sp + (size_t) r.left * bytesPerPixel,
            (size_t) w * bytesPerPixel);
    }
    return D3D_OK;
}

} // namespace

HRESULT VKD3DDevice::UpdateSurface(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect,
    IDirect3DSurface9 *pDestinationSurface, CONST POINT *pDestPoint)
{
    VK_Stub_Note("IDirect3DDevice9", "UpdateSurface");
    VKD3DSurface *src = dynamic_cast<VKD3DSurface *>(pSourceSurface);
    VKD3DSurface *dst = dynamic_cast<VKD3DSurface *>(pDestinationSurface);
    if (!src || !dst) return D3DERR_INVALIDCALL;
    const HRESULT hr = stub_surface_copy(src, pSourceRect, dst, pDestPoint);
    // A destination that views a texture level has no other road to the mirror: the engine will never
    // lock the DEFAULT-pool face it, and the rect-list callers do not feed VKTex themselves.
    if (SUCCEEDED(hr) && dst->texture_view()) dst->texture_view()->mirror_to_vulkan();
    return hr;
}

HRESULT VKD3DDevice::UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture)
{
    VK_Stub_Note("IDirect3DDevice9", "UpdateTexture");
    VKD3DTexture *src = dynamic_cast<VKD3DTexture *>(pSourceTexture);
    VKD3DTexture *dst = dynamic_cast<VKD3DTexture *>(pDestinationTexture);
    if (!src || !dst) {
        // Volume/cube stubs are not on the sovereignty side yet; keep the inert answer and the census line.
        return D3D_OK;
    }
    if (src->level_format() != dst->level_format()) return D3DERR_INVALIDCALL;
    const unsigned levels = src->level_count() < dst->level_count() ? src->level_count() : dst->level_count();
    for (unsigned l = 0; l < levels; ++l) {
        unsigned char *sb = nullptr;
        unsigned sp = 0;
        unsigned char *db = nullptr;
        unsigned dp = 0;
        if (!src->cpu_store_level(l, &sb, &sp) || !dst->cpu_store_level(l, &db, &dp)) return D3DERR_INVALIDCALL;
        const unsigned sh = VK_Stub_Mip_Dim(src->height(), l);
        const unsigned dh = VK_Stub_Mip_Dim(dst->height(), l);
        const unsigned rows = sh < dh ? sh : dh;
        const size_t span = sp < dp ? sp : dp;
        ::memcpy(db, sb, span * rows);
    }
    // The live callers here are the SYSTEMMEM staging uploads, and a DEFAULT destination cannot be
    // re-locked; feed the mirror from the copy's own bytes under the destination key.
    dst->mirror_to_vulkan();
    return D3D_OK;
}

// The readback's GPU half is not sovereign yet: what a render target holds lives in the Vulkan mirror
// (VKDraw writes it), not in the surface store, so this copy hands back whatever the CPU side wrote
// (usually nothing). Smudge and screenshot converge when the RTT readback wedge routes the VkImage back;
// until then the census counts these calls and the bytes are honest zeros.
HRESULT VKD3DDevice::GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface)
{
    VK_Stub_Note("IDirect3DDevice9", "GetRenderTargetData");
    VKD3DSurface *src = dynamic_cast<VKD3DSurface *>(pRenderTarget);
    VKD3DSurface *dst = dynamic_cast<VKD3DSurface *>(pDestSurface);
    if (!src || !dst) return D3D_OK;
    return stub_surface_copy(src, nullptr, dst, nullptr);
}

HRESULT VKD3DSurface::GetDesc(D3DSURFACE_DESC *pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(pDesc, "stub GetDesc");
    *pDesc = m_desc;
    return D3D_OK;
}

HRESULT VKD3DSurface::GetContainer(REFIID riid, void **ppContainer)
{
    if (!ppContainer) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(ppContainer, "VKD3DSurface::GetContainer");
    if (!m_container) { *ppContainer = nullptr; return D3DERR_NOTAVAILABLE; }
    return m_container->QueryInterface(riid, ppContainer);
}

HRESULT VKD3DSurface::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(ppDevice, "stub resource::GetDevice");
    *ppDevice = static_cast<IDirect3DDevice9 *>(m_dev);
    m_dev->AddRef();
    return D3D_OK;
}

void VKD3DTexture::fill_desc(D3DSURFACE_DESC *pDesc, UINT level) const
{
    ::ZeroMemory(pDesc, sizeof(*pDesc));
    pDesc->Format = m_format;
    pDesc->Type = D3DRTYPE_SURFACE;
    pDesc->Usage = m_usage;
    pDesc->Pool = m_pool;
    pDesc->MultiSampleType = D3DMULTISAMPLE_NONE;
    pDesc->Width = VK_Stub_Mip_Dim(m_width, level);
    pDesc->Height = VK_Stub_Mip_Dim(m_height, level);
}

HRESULT VKD3DTexture::GetLevelDesc(UINT level, D3DSURFACE_DESC *pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    if (level >= m_levels) return D3DERR_NOTAVAILABLE;
    fill_desc(pDesc, level);
    return D3D_OK;
}

HRESULT VKD3DTexture::GetSurfaceLevel(UINT level, IDirect3DSurface9 **ppSurfaceLevel)
{
    if (!ppSurfaceLevel) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(ppSurfaceLevel, "VKD3DTexture::GetSurfaceLevel");
    if (level >= m_levels) { *ppSurfaceLevel = nullptr; return D3DERR_NOTAVAILABLE; }
    D3DSURFACE_DESC desc;
    fill_desc(&desc, level);
    // The surface carries the texture as its container, which is what GetContainer has to answer with and
    // how the engine's own surface->texture identity checks keep working.
    VKD3DSurface *surface = new VKD3DSurface(m_dev, desc, static_cast<IUnknown *>(this));
    // Wedge 9.K: and it is a *view* - its locks and its copy-path store resolve into the texture's level
    // bytes, so an engine write through the view and a mirror re-lock of the texture see the same texels.
    surface->set_texture_level_view(this, level);
    *ppSurfaceLevel = surface;
    return D3D_OK;
}

HRESULT VKD3DTexture::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(ppDevice, "stub resource::GetDevice");
    *ppDevice = static_cast<IDirect3DDevice9 *>(m_dev);
    m_dev->AddRef();
    return D3D_OK;
}

HRESULT VKD3DVertexBuffer::GetDesc(D3DVERTEXBUFFER_DESC *pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(pDesc, "stub GetDesc");
    *pDesc = m_desc;
    return D3D_OK;
}

HRESULT VKD3DVertexBuffer::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(ppDevice, "stub resource::GetDevice");
    *ppDevice = static_cast<IDirect3DDevice9 *>(m_dev);
    m_dev->AddRef();
    return D3D_OK;
}

HRESULT VKD3DIndexBuffer::GetDesc(D3DINDEXBUFFER_DESC *pDesc)
{
    if (!pDesc) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(pDesc, "stub GetDesc");
    *pDesc = m_desc;
    return D3D_OK;
}

HRESULT VKD3DIndexBuffer::GetDevice(IDirect3DDevice9 **ppDevice)
{
    if (!ppDevice) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(ppDevice, "stub resource::GetDevice");
    *ppDevice = static_cast<IDirect3DDevice9 *>(m_dev);
    m_dev->AddRef();
    return D3D_OK;
}

// ---- wedge 9.K: the CPU data plane ------------------------------------------
// These locks are the seam the whole wedge turns on. VKTex_Mirror_D3D_Texture re-locks every mip READONLY
// to feed the Vulkan upload, and the engine's own writers (textureloader, surfaceclass, missingtexture,
// the UpdateTexture staging paths in dx8wrapper.h) lock the same levels. Handing both the same bytes is
// what lets a texture exist with no d3d9 behind it; no engine call site has to change.
//
// D3D9's own lock rule is kept rather than relaxed: a D3DPOOL_DEFAULT level is only lockable when it was
// created DYNAMIC. Callers branch on that failure to take their staging path, and if the lock "succeeded"
// everywhere the staging paths would never run and their mirrors would stop being fed.

HRESULT VKD3DTexture::LockRect(UINT level, D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD flags)
{
    VK_NOD3D_Watch_Check("stub Texture::LockRect");
    if (!pLockedRect) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(pLockedRect, "stub LockRect");
    pLockedRect->pBits = nullptr;
    pLockedRect->Pitch = 0;
    if (level >= m_levels) return D3DERR_INVALIDCALL;
    if (m_pool == D3DPOOL_DEFAULT && !(m_usage & D3DUSAGE_DYNAMIC)) return D3DERR_INVALIDCALL;
    if (pRect && !stub_stride_is_uncompressed(m_format)) return D3DERR_INVALIDCALL;
    if (!ensure_level(level)) return D3DERR_INVALIDCALL;

    unsigned perRow = 0;
    stub_stride(m_format, VK_Stub_Mip_Dim(m_width, level), &perRow);
    unsigned bytesPerPixel = perRow / VK_Stub_Mip_Dim(m_width, level);
    if (!bytesPerPixel) bytesPerPixel = 1;

    unsigned char *base = m_store[level].bits;
    if (pRect) {
        const long left = pRect->left < 0 ? 0 : pRect->left;
        const long top = pRect->top < 0 ? 0 : pRect->top;
        base += (size_t) top * m_store[level].pitch + (size_t) left * bytesPerPixel;
    }
    // Wedge 9.K: a writable lock marks the level as engine-owned, which is how the auto-mip pass knows
    // the caller maintains its own chain (the loader locks and fills every level) and must not overwrite it.
    if (!(flags & D3DLOCK_READONLY)) m_store[level].written = 1;
    pLockedRect->pBits = base;
    pLockedRect->Pitch = (int) m_store[level].pitch;
    return D3D_OK;
}

HRESULT VKD3DTexture::UnlockRect(UINT level)
{
    VK_NOD3D_Watch_Check("stub Texture::UnlockRect");
    // The CPU store is the content: there is no device memory to copy back to, so unlocking is complete
    // work, not a placeholder. Writers that expect D3DLOCK_DONOTWAIT semantics get the same answer.
    if (level >= m_levels) return D3DERR_INVALIDCALL;
    // Wedge 9.K: the writer has finished the face side of level 0; this is the same point at which the
    // d3d9/DXVK upload used to sample the mip chain, so the chain is generated here.
    if (m_auto_mip && level == 0) generate_mips_from_level0();
    return D3D_OK;
}

// Wedge 9.K: d3d9 stores the LOD and reports the new value back; the x2 in the 9.J census is the
// engine's own TextureClass LOD plumbing running through a device that now lives here.
DWORD VKD3DTexture::SetLOD(DWORD LODNew)
{
    m_lod = (LODNew < m_levels) ? LODNew : m_levels - 1;
    return m_lod;
}

DWORD VKD3DTexture::GetLOD(THIS)
{
    return m_lod;
}

HRESULT VKD3DSurface::LockRect(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD flags)
{
    VK_NOD3D_Watch_Check("stub Surface::LockRect");
    // Wedge 9.K: a texture-level view locks the texture's store, not its own. A view that kept private
    // bytes would take the engine's writes into a buffer the Vulkan mirror never reads.
    if (m_tex) return m_tex->LockRect(m_texLevel, pLockedRect, pRect, flags);
    (void)flags;
    if (!pLockedRect) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(pLockedRect, "stub LockRect");
    pLockedRect->pBits = nullptr;
    pLockedRect->Pitch = 0;
    if (m_desc.Pool == D3DPOOL_DEFAULT && !(m_desc.Usage & D3DUSAGE_DYNAMIC)) return D3DERR_INVALIDCALL;
    if (pRect && !stub_stride_is_uncompressed(m_desc.Format)) return D3DERR_INVALIDCALL;

    if (!m_bits) {
        unsigned perRow = 0;
        if (!stub_stride(m_desc.Format, m_desc.Width, &perRow)) { stub_log_unknown_format(m_desc.Format); return D3DERR_INVALIDCALL; }
        vk_stub_store_alloc(m_bits, m_region, (size_t) perRow * m_desc.Height);
        if (!m_bits) return D3DERR_OUTOFVIDEOMEMORY;
        m_pitch = perRow;
    }

    unsigned char *base = m_bits;
    if (pRect) {
        const long left = pRect->left < 0 ? 0 : pRect->left;
        const long top = pRect->top < 0 ? 0 : pRect->top;
        const unsigned bytesPerPixel = m_pitch / (m_desc.Width ? m_desc.Width : 1);
        base += (size_t) top * m_pitch + (size_t) left * (bytesPerPixel ? bytesPerPixel : 1);
    }
    pLockedRect->pBits = base;
    pLockedRect->Pitch = (int) m_pitch;
    return D3D_OK;
}

HRESULT VKD3DSurface::UnlockRect(THIS)
{
    VK_NOD3D_Watch_Check("stub Surface::UnlockRect");
    // Wedge 9.K: forward so an auto-mip level-0 view unlock still lands in the texture's generate pass.
    if (m_tex) return m_tex->UnlockRect(m_texLevel);
    return D3D_OK;
}

HRESULT VKD3DVertexBuffer::Lock(UINT offsetToLock, UINT sizeToLock, void **ppbData, DWORD flags)
{
    VK_NOD3D_Watch_Check("stub VB::Lock");
    (void)flags;
    if (!ppbData) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(ppbData, "stub buffer::Lock");
    *ppbData = nullptr;
    if (m_desc.Pool == D3DPOOL_DEFAULT && !(m_desc.Usage & D3DUSAGE_DYNAMIC)) return D3DERR_INVALIDCALL;
    if (!m_bits) {
        const size_t size = m_desc.Size ? m_desc.Size : 16;
        vk_stub_store_alloc(m_bits, m_region, size);
        if (!m_bits) return D3DERR_OUTOFVIDEOMEMORY;
    }
    const UINT whole = sizeToLock == 0 ? m_desc.Size : sizeToLock;
    if (offsetToLock > m_desc.Size || offsetToLock + whole > m_desc.Size) return D3DERR_INVALIDCALL;
    *ppbData = m_bits + offsetToLock;
    return D3D_OK;
}

HRESULT VKD3DVertexBuffer::Unlock(THIS)
{
    VK_NOD3D_Watch_Check("stub VB::Unlock");
    return D3D_OK;
}

HRESULT VKD3DIndexBuffer::Lock(UINT offsetToLock, UINT sizeToLock, void **ppbData, DWORD flags)
{
    VK_NOD3D_Watch_Check("stub IB::Lock");
    (void)flags;
    if (!ppbData) return D3DERR_INVALIDCALL;
    VK_Stub_Store_Note(ppbData, "stub buffer::Lock");
    *ppbData = nullptr;
    if (m_desc.Pool == D3DPOOL_DEFAULT && !(m_desc.Usage & D3DUSAGE_DYNAMIC)) return D3DERR_INVALIDCALL;
    if (!m_bits) {
        const size_t size = m_desc.Size ? m_desc.Size : 16;
        vk_stub_store_alloc(m_bits, m_region, size);
        if (!m_bits) return D3DERR_OUTOFVIDEOMEMORY;
    }
    const UINT whole = sizeToLock == 0 ? m_desc.Size : sizeToLock;
    if (offsetToLock > m_desc.Size || offsetToLock + whole > m_desc.Size) return D3DERR_INVALIDCALL;
    *ppbData = m_bits + offsetToLock;
    return D3D_OK;
}

HRESULT VKD3DIndexBuffer::Unlock(THIS)
{
    VK_NOD3D_Watch_Check("stub IB::Unlock");
    return D3D_OK;
}

// ------------------------------------------------------------------ capability block

// GeneralsX @feature Qoder 18/10/2026 wedge 9.J2c: the capability block, captured verbatim.
//
// The first revision of this function assigned about forty fields by hand from the GENERALSX_VK_CAPDUMP=1
// print, and a hand list is only as good as the guess about which fields the engine reads. MaxTextureHeight
// stayed zero, TextureLoader::Validate_Texture_Size (textureloader.cpp:574) divides the power-of-two height
// by it, and the no-d3d9 boot died with 0xC0000094 inside Apply_Dim_Reduction during terrain init - a fault
// that has nothing to do with the resource sovereignty the mode is meant to measure. So the block is now the
// measured bytes themselves, and nothing is left to forget.
//
// Captured 18/10/2026 with GENERALSX_VK_CAPDUMP=1 on the d3d9/DXVK path (NVIDIA GeForce RTX 4070 Ti SUPER);
// the source line is the [VKCAPSBIN] entry in the run log. Regenerate with logs/_run_vk_caps_j2.bat.
// DeviceType and AdapterOrdinal are patched per call because they echo the request, not the adapter.
static const unsigned char g_vkCapsBlob[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x02, 0x60, 0xa0, 0x03, 0x00, 0x00, 0x0f, 0x00, 0x00, 0x80,
    0x01, 0x00, 0x00, 0x00, 0xf0, 0xbe, 0x1b, 0x00, 0xf2, 0xcf, 0x3f, 0x00,
    0x91, 0x21, 0x73, 0x0f, 0xff, 0x00, 0x00, 0x00, 0xff, 0x3f, 0x00, 0x00,
    0xff, 0x3f, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0x08, 0x42, 0x08, 0x00,
    0xc5, 0xec, 0x01, 0x00, 0x00, 0x07, 0x03, 0x07, 0x00, 0x07, 0x03, 0x07,
    0x00, 0x07, 0x03, 0x07, 0x3f, 0x00, 0x00, 0x00, 0x3f, 0x00, 0x00, 0x00,
    0x3f, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
    0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00,
    0x10, 0x00, 0x00, 0x00, 0xf9, 0x02, 0x15, 0x50, 0x00, 0x00, 0x00, 0xc7,
    0x00, 0x00, 0x00, 0xc7, 0x00, 0x00, 0x00, 0x47, 0x00, 0x00, 0x00, 0x47,
    0x00, 0x00, 0x00, 0x00, 0xff, 0x01, 0x00, 0x00, 0x08, 0x00, 0x10, 0x00,
    0xff, 0xff, 0xff, 0x03, 0x08, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x7b, 0x01, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xfe, 0xff, 0x44,
    0x55, 0x55, 0x55, 0x00, 0xff, 0xff, 0xff, 0x00, 0x10, 0x00, 0x00, 0x00,
    0xfc, 0x01, 0x00, 0x00, 0x00, 0x03, 0xfe, 0xff, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x03, 0xff, 0xff, 0xff, 0xff, 0x7f, 0x7f, 0x51, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00,
    0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00,
    0x1f, 0x00, 0x00, 0x00, 0x18, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x03, 0x00, 0x03,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00, 0x80, 0x00, 0x00,
    0x00, 0x80, 0x00, 0x00,
};

void VK_Stub_Fill_Caps(D3DCAPS9 *caps, UINT adapter, D3DDEVTYPE devtype)
{
    static_assert(sizeof(D3DCAPS9) == sizeof(g_vkCapsBlob), "caps blob captured against a different D3DCAPS9 layout");

    ::memcpy(caps, g_vkCapsBlob, sizeof(*caps));
    caps->AdapterOrdinal = adapter;
    caps->DeviceType = devtype;

    // GeneralsX @bugfix Qoder 19/10/2026 BUG-03 red shadows: the blob was captured from a device that
    // did not advertise D3DPMISCCAPS_COLORWRITEENABLE, so W3DVolumetricShadow::renderShadows and
    // W3DScene took the blend-fake branch (SRCBLEND ZERO / DESTBLEND ONE) to mask color writes during
    // the stencil-fill pass. That fake does not survive the facade's shader-state folding - the batches
    // submitted with blend disabled and the shadow volumes painted their red test material on screen.
    // Vulkan supports color write masks unconditionally (VulkanDraw.cpp maps D3DRS_COLORWRITEENABLE
    // straight onto VkPipelineColorBlendAttachmentState), so report the bit truthfully and the engine
    // takes the real COLORWRITEENABLE=0 path.
    caps->PrimitiveMiscCaps |= D3DPMISCCAPS_COLORWRITEENABLE;
}


#endif // SAGE_USE_VULKAN_CORE
#endif // _WIN32
