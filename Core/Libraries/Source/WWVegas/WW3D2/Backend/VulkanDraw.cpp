// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5: pipeline state objects and the draw path.
// See Backend/VulkanDraw.h for the scope and the recorded design decisions.
//
// Frame model. DX8Wrapper::Begin_Scene() opens a command buffer for the frame slot, every batch in the
// frame records into it through VK_Draw_Submit (which lazily opens the render pass for whichever target
// is current), and the present call closes the pass, blits the scene target onto a swapchain image and
// submits. The scene target is ours, not the backbuffer, so a render-target switch inside a frame is a
// pass boundary rather than a swapchain operation.
#ifdef _WIN32
#if defined(SAGE_USE_VULKAN_CORE)

#include "VulkanCore.h"
#include "VulkanFns.h"
#include "VulkanDraw.h"
#include "VulkanGeo.h"
#include "VulkanDesc.h"
#include "VulkanPgm.h"
#include "VulkanForward.h"
#include "VkLightingLayout.h"
#include "VkShadersDraw.h"
#include "VkShadersFF.h"
#include "VkShadersShadow.h"
#include "VkShadersPost.h"
#include "VkShadersSky.h"
// GeneralsX @feature Qoder 21/10/2026 in-game GPU vegetation instance pass.
#include "VkShadersVeg.h"
#include "VkShadersFx.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/texture.h"
#include "WW3D2/surfaceclass.h"
#include "WW3D2/ww3dformat.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <deque>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <thread>

// Shadow-receive stencil bit. The engine owns the MSB (0x80: volumetric PotentialOccluderBit) and the
// player-colour field in bits 3..6, so the old 0x80 mask read an engine stamp as "this material receives
// shadows". Bit 2 was unused; keeping the receive mask there makes the two owners independent.
#define VK_SHADOW_RECEIVE_BIT 0x04u

// GeneralsX @feature 26/10/2026 volumetric fog: the post pass reads one small UBO per in-flight slot.
// 240 B of payload, padded to 512 so every slot clears max(minUniformBufferOffsetAlignment) even on
// devices that report 256.
#define VKPOST_FOG_SLOT_BYTES 512u
struct VKPostFogUbo
{
    float invView[16];
    float lightVP[16];
    float color[4];       // rgb ambient fog, w = mode (0..3)
    float params[4];      // density, 1/heightScale, baseHeight, startDistance
    float params2[4];     // fadeLength, maxOpacity, noiseScale, time
    float params3[4];     // sunDir.xyz, sunScatter
    float params4[4];     // sunColor.rgb, skyAffect
    float params5[4];     // windX, windY, quality, contact-shadow strength (0..1)
    float viewport[4];    // xywh in scene-target pixels
    float proj[16];       // scene projection rows (view -> clip), for screen-space contact shadows
    float fx0[4];         // x = contact shadows, y = god rays, zw reserved
    float heatPos[16];    // A7: 4 x (xyz world, w radius) heat sources, brightest additive spawns
    float heatInfo[4];    // x = count, y = strength, z = reserved, w = time (s)
};

// GeneralsX @feature Qoder 18/10/2026 wedge 7 diagnosis: frame breadcrumbs. A stall inside a Vulkan
// entry point of the driver writes nothing of its own, so with GENERALSX_VKTRACE=1 every boundary is
// named before the call that can block. The last line of a hung run then says which call it is in.
static bool vktrace_on()
{
    static int on = -1;
    if (on < 0) {
        char v[8] = {};
        on = GetEnvironmentVariableA("GENERALSX_VKTRACE", v, (DWORD)sizeof(v)) ? (atoi(v) != 0) : 0;
    }
    return on != 0;
}
#define VKTRACE(...) do { if (vktrace_on()) { VKWP1_LOG(__VA_ARGS__); } } while (0)

// GeneralsX @feature Qoder 18/10/2026 wedge 7 diagnosis: in-process stall watchdog. The render thread
// stores the step it is about to execute; a watchdog samples it every two seconds and, when the step and
// frame have not moved, suspends the render thread to capture its instruction pointer. That separates a
// spin inside a driver call from a spin in engine code after Begin_Frame returned, with no external
// debugger (and no symbols) needed.
static std::atomic<long> g_vkStep{ 0 };
static std::atomic<unsigned long long> g_vkStepFrame{ 0ull };
static unsigned long g_vkRenderThread = 0u;
#define VKSTEP(n) do { g_vkStep.store(n); } while (0)

static void vktrace_watchdog_start()
{
    if (!vktrace_on()) return;
    static bool started = false;
    if (started) return;
    started = true;
    g_vkRenderThread = (unsigned long)GetCurrentThreadId();
    std::thread([]{
        long lastStep = -999;
        unsigned long long lastFrame = ~0ull;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            const long step = g_vkStep.load();
            const unsigned long long frame = g_vkStepFrame.load();
            if (step != lastStep || frame != lastFrame) { lastStep = step; lastFrame = frame; continue; }
            unsigned long long rip = 0ull;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, (DWORD)g_vkRenderThread);
            if (h) {
                SuspendThread(h);
                CONTEXT c;
                memset(&c, 0, sizeof(c));
                c.ContextFlags = CONTEXT_CONTROL;
                if (GetThreadContext(h, &c)) rip = (unsigned long long)c.Rip;
                ResumeThread(h);
                CloseHandle(h);
            }
            const unsigned long long base = (unsigned long long)(uintptr_t)GetModuleHandleW(nullptr);
            VKWP1_LOG("VKTRACE watchdog: frame=%llu step=%ld stall>2s renderRip=%llx exeRva=%llx",
                frame, step, rip, rip - base);
        }
    }).detach();
    VKWP1_LOG("VKTRACE watchdog started on render thread %lu", g_vkRenderThread);
}

// ---------------------------------------------------------------------------
// D3D ordinal translations
// The facade hands over D3D9 ordinals because those are the values it natively stores. Every mapping is
// an explicit switch: compare ops and blend factors are not monotonic between the two APIs, so nothing
// here is arithmetic.
// ---------------------------------------------------------------------------
namespace {

// The real D3DPRIMITIVETYPE ordinals (d3d9types.h): point=1, line list/strip=2/3, triangle
// list/strip/fan=4/5/6. There is no LINELOOP in D3D8/D3D9, so the enum stops at the fan.
enum { D3D_PT_POINTLIST = 1, D3D_PT_LINELIST = 2, D3D_PT_LINESTRIP = 3,
       D3D_PT_TRIANGLELIST = 4, D3D_PT_TRIANGLESTRIP = 5, D3D_PT_TRIANGLEFAN = 6 };

VkPrimitiveTopology vk_topology(unsigned d3d_pt)
{
    switch (d3d_pt) {
    case D3D_PT_POINTLIST:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3D_PT_LINELIST:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3D_PT_LINESTRIP:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case D3D_PT_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case D3D_PT_TRIANGLEFAN:   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;  // rewritten indices, see below
    default:                   return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;   // D3D_PT_TRIANGLELIST
    }
}

// How many indices one polygon_count consumes per primitive type, exactly as D3D9 counts them.
unsigned vk_index_count_for(unsigned d3d_pt, unsigned polygon_count)
{
    switch (d3d_pt) {
    case D3D_PT_TRIANGLELIST:  return polygon_count * 3u;
    case D3D_PT_TRIANGLESTRIP: return polygon_count + 2u;
    case D3D_PT_TRIANGLEFAN:   return polygon_count + 2u;
    case D3D_PT_LINELIST:      return polygon_count * 2u;
    case D3D_PT_LINESTRIP:     return polygon_count + 1u;
    case D3D_PT_POINTLIST:     return polygon_count;
    default:                   return polygon_count * 3u;
    }
}

// D3DCMPFUNC ordinals (d3d8types.h): never=1, less=2, equal=3, lessequal=4, greater=5, notequal=6,
// greaterequal=7, always=8. The Vulkan enum is a different order, so the table has to be spelled out -
// D3DCMP_LESSEQUAL is the engine's default ZFUNC and D3DRS_ALPHAFUNC uses the same enum, so a slip here
// silently inverts every depth and alpha test instead of failing loudly.
VkCompareOp vk_compare(unsigned d3d_cmp)
{
    switch (d3d_cmp) {
    case 1:  return VK_COMPARE_OP_NEVER;
    case 2:  return VK_COMPARE_OP_LESS;
    case 3:  return VK_COMPARE_OP_EQUAL;
    case 4:  return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5:  return VK_COMPARE_OP_GREATER;
    case 6:  return VK_COMPARE_OP_NOT_EQUAL;
    case 7:  return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case 8:  return VK_COMPARE_OP_ALWAYS;
    default: return VK_COMPARE_OP_LESS_OR_EQUAL;    // unset state falls back to the D3D default
    }
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: D3DSTENCILOP ordinals are already in the
// VkStencilOp order - KEEP=1..DECR=8 against VK_STENCIL_OP_KEEP=0..DECREMENT_AND_WRAP=7 - so the map is
// the value minus one, with an unset state falling back to KEEP exactly as D3D does.
VkStencilOp vk_stencil_op(unsigned d3d_op)
{
    if (d3d_op < 1u || d3d_op > 8u) return VK_STENCIL_OP_KEEP;   // D3DSTENCILOP_KEEP .. D3DSTENCILOP_DECR
    return (VkStencilOp)(d3d_op - 1u);
}

// D3DBLEND ordinals as DX8 defines them. DX8 has no separate-alpha blending (D3DRS_SRCBLENDALPHA is a
// D3D9 state that does not exist in d3d8types.h), so one factor table serves both channels.
VkBlendFactor vk_blend(unsigned d3d_blend)
{
    switch (d3d_blend) {
    case 1:  return VK_BLEND_FACTOR_ZERO;
    case 2:  return VK_BLEND_FACTOR_ONE;
    case 3:  return VK_BLEND_FACTOR_SRC_COLOR;
    case 4:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5:  return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6:  return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7:  return VK_BLEND_FACTOR_DST_ALPHA;
    case 8:  return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9:  return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    // D3DBLEND_BOTHSRCALPHA / BOTHINVSRCALPHA are the legacy DX5 modes: they ignore the channel and
    // blend with the source alpha in both factors, so they must not map to the constant colour/alpha.
    case 12: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkCullModeFlags vk_cull(unsigned d3d_cull, unsigned front_ccw)
{
    if (d3d_cull <= 1) return VK_CULL_MODE_NONE;
    // D3D's cull mode names the side it REMOVES: D3DCULL_CW (2) "cull back faces with clockwise
    // vertices", D3DCULL_CCW (3) the counter-clockwise back faces. Vulkan instead culls front or back
    // relative to frontFace, so the translation is which winding the removed side has under that
    // declaration - not a fixed swap. This engine sets D3DCULL_CW in Apply_Default_State, i.e. its
    // visible faces are counter-clockwise in screen space, and the facade pins front_ccw = 1 to match.
    // The earlier form returned the opposite bit in both branches, which culled every front face and
    // left buildings showing their interior walls (found 18/10/2026 from a replay screenshot).
    const bool cwIsFront = (front_ccw == 0);
    if (d3d_cull == 2) return cwIsFront ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
    return cwIsFront ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
}

VkFormat vk_attr_format(unsigned type, unsigned components)
{
    if (type == 1) return VK_FORMAT_B8G8R8A8_UNORM;   // D3DCOLOR: BGRA in memory, ARGB as a dword
    if (type == 2) return VK_FORMAT_R16G16B16A16_SNORM;
    if (type == 3) return VK_FORMAT_R32_UINT;
    if (type == 4) return VK_FORMAT_R16G16B16A16_UNORM;
    switch (components) {
    case 1:  return VK_FORMAT_R32_SFLOAT;
    case 2:  return VK_FORMAT_R32G32_SFLOAT;
    case 4:  return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: return VK_FORMAT_R32G32B32_SFLOAT;
    }
}

// Bytes one declared vertex attribute reads. Mirrors vk_attr_format() above, and is what lets the submit
// range check notice an attribute that reaches past the end of its own vertex.
unsigned vk_attr_bytes(unsigned type, unsigned components)
{
    if (type == 1) return 4u;                    // D3DCOLOR: one packed BGRA byte
    if (type == 2) return 8u;                    // four normalized shorts
    if (type == 3) return 4u;                    // one uint
    if (type == 4) return 8u;                    // four unnormalized shorts
    return components * 4u;                      // one to four 32-bit floats
}

// ---------------------------------------------------------------------------
// wedge 6: the fixed-function vertex stage's vertex input contract
// ---------------------------------------------------------------------------
// These are the eight input locations shaders/vkff.vert reads, in declaration order. A D3D9 vertex
// processor filled a field the FVF did not declare with a documented default and carried on; Vulkan has no
// such rule - an input with no buffer behind it is undefined - so this layer supplies the same defaults by
// binding a second, stride-zero stream whose 16-byte groups are exactly those values. The stream is one
// instance wide, so every vertex reads the same group, which is what a per-primitive constant is.
enum { VKFF_LOCATIONS = 8 };
enum { VKFF_DUMMY_BINDING = 1, VKFF_DUMMY_SLOT_BYTES = 16 };

// Component counts have to match what vkff.vert declares each location as, because the format decides how
// many floats the shader sees: vec3 at 1, vec4 everywhere else.
const unsigned VKFF_LOC_COMPONENTS[VKFF_LOCATIONS] = { 4, 3, 4, 4, 4, 4, 4, 4 };
const float VKFF_LOC_DEFAULT[VKFF_LOCATIONS][4] = {
    { 0.0f, 0.0f, 0.0f, 1.0f },   // 0 position: the origin, and w = 1 so it survives the divide
    { 0.0f, 0.0f, -1.0f, 0.0f },  // 1 normal: D3D's unlit face points down -Z, the same fallback the
                                  //   facade uses for a mesh that carries no normals at all
    { 1.0f, 1.0f, 1.0f, 1.0f },   // 2 vertex diffuse: white, so a missing colour cannot darken a face
    { 0.0f, 0.0f, 0.0f, 1.0f },   // 3 vertex specular: no highlight
    { 0.0f, 0.0f, 0.0f, 1.0f },   // 4..7 texture coordinates: the (0,0,0,1) the FVF format bits imply
    { 0.0f, 0.0f, 0.0f, 1.0f },
    { 0.0f, 0.0f, 0.0f, 1.0f },
    { 0.0f, 0.0f, 0.0f, 1.0f },
};

// ---------------------------------------------------------------------------
// state
// ---------------------------------------------------------------------------
// Taken straight from the present chain's slot count so the geometry rotation, the per-slot descriptor
// pools and the fence-per-slot rule can never disagree with the frame loop.
enum { VKGEO_SLOT_COUNT = (int)VulkanCore::VK_IN_FLIGHT };

// One stable Vulkan buffer per WW3D vertex / index buffer object. Wedge 3.1's shared bump ring handed
// every lock a new offset, which is fine while nothing reads it back and fatal the moment a draw binds
// it. Dynamic buffers rotate over VKGEO_SLOT_COUNT regions keyed by the frame index, so a region is only
// overwritten once the fence of the frame that last wrote it has passed.
struct VKGeoBack {
    void*              key;
    VkBuffer           buf;
    VkDeviceMemory     mem;
    void*              map;
    unsigned long long cap;        // total bytes
    unsigned long long slotBytes;  // bytes per rotating slot (== cap when static)
    unsigned           stride;
    unsigned           slot;       // slot the current region lives in
    unsigned long long curOffset;
    unsigned long long curBytes;
    int                dynamic;
    // GeneralsX @performance Qoder 19/10/2026 R-05: write generation. Every lock of this backing bumps it
    // to a globally fresh value, so a cached per-range fact (the index scan's maxIndex) can verify that no
    // byte of this backing has been rewritten since it was read - even if the VkBuffer handle itself was
    // recycled into an entirely different backing in between.
    unsigned long long gen = 0;
    // GeneralsX @bugfix Qoder 19/10/2026 R-05: tombstone flag. See VKDrawState::geo - the deque never
    // erases from the middle, so a removed entry sits freed until the free list recycles its slot.
    int freed = 0;
    // GeneralsX @bugfix Qoder 20/10/2026 R-08: DISCARD generations. The engine restarts its pool offset at
    // zero whenever the next batch would not fit, which in D3D9 is a buffer rename: the new writes go to
    // fresh storage and every draw already recorded keeps reading the old bytes. One region per frame slot
    // cannot express that, and the second batch of a frame then overwrote vertices a skinned mesh (or the
    // UI) had already recorded - the atlas-coloured triangles stretching out of infantry. A generation is
    // one more pool-sized region inside this frame's slot, so an in-frame discard moves forward instead of
    // over the bytes still in flight. It grows only when a frame actually discards twice.
    unsigned           gens = 1;
    unsigned           curGen = 0;
    unsigned long long lockFrame = ~0ull;
    int                lockedInFrame = 0;
    // GeneralsX @debug Qoder 20/10/2026 R-08: the event this fix is about, one bounded line per discard.
    unsigned           traceDiscards = 0;
    // GeneralsX @debug 28/10/2026 in-place forensics: how many times a STATIC backing has been locked.
    // A static backing never rotates, so its second lock rewrites the exact bytes every frame still in
    // flight is reading - that is the one-frame "geometry stretched across the screen" tear.
    unsigned           locks = 0;
};

// A backing that has been dropped by its WW3D object, or rebuilt because the object grew, is not ours to
// destroy yet: command buffers of the frames still in flight hold the VkBuffer handle and the driver reads
// it when the GPU runs them. Freeing it on the spot is exactly the "worked in wedge 3, lost the device the
// moment real draws arrived" failure, so the handle goes on a retirement list and is only destroyed after
// enough frame boundaries have passed that every slot which could still reference it has retired.
struct VKGeoZombie {
    VkBuffer           buf;
    VkDeviceMemory     mem;
    void*              map;
    unsigned           slotsLeft;   // frame boundaries to wait before the destroy is safe
};

struct VKDrawPass {
    unsigned long long sig;
    unsigned       samples;
    VkRenderPass   rp;
};
struct VKDrawFB {
    void*    colorView;
    void*    depthView;
    unsigned w, h;
    unsigned pass;
    VkFramebuffer fb;
};
struct VKDrawPSO {
    VKDraw_PSO_Key key;
    unsigned       attr_count;
    VKDraw_Attr    attrs[VKDRAW_MAX_ATTRS];
    // Wedge 6: which vertex stage this pipeline was built with. The key cannot say it (a fixed-function
    // batch and a mechanism probe share the same FVF, shader bits and light mask), and the FF path adds a
    // second vertex binding, so the two must never alias onto one pipeline object.
    unsigned       ff;
    // Phase 5B.3: the material pipeline key that selected this pipeline. Zero means the family still
    // uses the legacy state-derived identity, so its cache behaviour is unchanged.
    unsigned long long mat_key;
    VkPipeline     pso;
};
struct VKDrawSetEntry {
    void*           views[VKDRAW_MAX_TEX_SLOTS];
    void*           samplers[VKDRAW_MAX_TEX_SLOTS];
    // A set also records WHICH constant buffer each of the three dynamic bindings points at and with
    // what range. The offset itself is deliberately not part of the key - it is supplied at bind time,
    // which is what lets one set cover every draw that reuses the same constant payload.
    void*           cbuf[3];
    unsigned long long cbytes[3];
    // bindings 5..8: one light-shadow map per slot (E3), 9: cookie atlas, 10: sky cube. A rebuilt mirror
    // must never be sampled by a stale set, so every view that can change per frame is part of the key.
    void*           lightMaps[VkLighting::kShadowMaps];
    void*           cookie;        // static, but cheap to compare and it keeps the key complete
    void*           skyCube;
    VkDescriptorSet set;
};

// Per-in-flight-slot constant rings (wedge 5.1). The engine's shadow constant arrays are large - the
// register budgets are 256 vertex / 224 pixel vec4 plus a 1 KiB light block - so a fresh reservation per
// draw would be a 9 KiB memcpy on every batch and a ring that fits a frame would be hundreds of MB.
// Instead: one reservation per DISTINCT payload, detected by comparing against the region this slot
// last wrote, and a ring partitioned by frame slot so a cursor reset can only ever reuse a region whose
// frame has already retired (Begin_Frame resets the cursor after the slot's fence passed).
struct VKDrawConstRing {
    VkBuffer           buf = nullptr;
    VkDeviceMemory     mem = nullptr;
    void*              map = nullptr;
    unsigned long long cap = 0;        // whole ring
    unsigned long long slotCap = 0;    // one in-flight slot's region: window + spill
    // GeneralsX @bugfix Qoder 19/10/2026 R-02 ring spill: the frame window is the first part of the
    // slot region and the spill the rest (the FF ring gets 4 window-sizes of spill after the CH02 burst
    // frames overran a 1x spill). A window that fills mid-frame appends into the spill
    // instead of restarting at the base, so the payloads the frame's EARLIEST batches still point at
    // are never overwritten while that frame is in flight - the old wrap was exactly that overwrite,
    // and the R-02 white silhouettes are what replaying later batches' blocks looks like.
    unsigned long long winCap = 0;     // per-slot frame window
    unsigned long long cur[VKGEO_SLOT_COUNT] = {};
    unsigned long long lastOff[VKGEO_SLOT_COUNT] = {};
    unsigned           lastBytes[VKGEO_SLOT_COUNT] = {};
    unsigned long long uploads = 0;    // distinct payloads written
    unsigned long long reuses = 0;     // draws that matched the previous payload
    unsigned long long spills = 0;     // window filled: appended into the spill (correct, just hot)
    unsigned long long wraps = 0;      // window AND spill exhausted: in-frame overwrite, must be ~0
    // GeneralsX @bugfix Qoder 19/10/2026 R-02 forensics: the deepest cursor this ring's current frame
    // window has reached, in bytes. A ring that fills without wrapping is the near miss that says the
    // wrap counter alone is one frame away from lying, so both numbers travel with every shot line.
    unsigned long long peak = 0;
};

// GeneralsX @feature Qoder 20/10/2026 native sun shadow: one opaque batch remembered while the scene is
// drawn, replayed from the sun before the receiver runs. The handles stay valid for the rest of the frame
// because the geometry backings are only retired at a frame boundary.
struct VKShadowCaster {
    VkBuffer     vb;
    VkDeviceSize voff;
    VkBuffer     ib;
    VkDeviceSize ioff;
    unsigned     index_count;
    int          base_vertex;
    unsigned     stride;
    unsigned     pos_offset;
    unsigned     topology;
    float        world[16];
    // Alpha-tested casters (W3DTreeBuffer foliage and other leaf cards): skipping them was why a forest
    // produced no visible shadow at all. The cutout runs in the cast fragment stage with the same texture
    // and compare the scene used, so the card quad occludes nothing and the leaves do.
    void*        tex;
    void*        samp;
    unsigned     uv_offset;
    unsigned     alpha_enable, alpha_ref, alpha_cmp;
    // GeneralsX @debug Qoder 20/10/2026: CPU-skinned batches are the only casters whose world is the
    // identity; the diagnostic in mode 13 replays just those, so their silhouettes can be read without
    // the terrain and building clutter.
    unsigned     skin;
};
struct VKShadowCastPSO {
    unsigned   stride, pos_offset, topology, uv_offset, alpha_enable;
    VkPipeline pso;
};
struct VKShadowCastSet {
    void*           tex;
    void*           samp;
    VkDescriptorSet set;
};

struct VKDrawState {
    bool  envOn = false, envChecked = false, built = false, broken = false;
    // P6 per-material shadow receive: 1 stamps VK_SHADOW_RECEIVE_BIT, 0 clears it. Set around a draw
    // by DX8Renderer from W3DMATF_RECEIVE_SHADOW; the native receiver tests the bit.
    int   shadowReceive = 1;
    unsigned msaaSamples = 1u;   // B5: 1 = off, 4 = 4x MSAA (colour + depth resolve)
    // Phase 5A current material key, published by W3DMaterialBinder.
    unsigned matFamily = 0, matFeatures = 0, matVariant = 0, matPass = 0;
    // Phase 5B.3: explicit material pipeline key hash for families migrated off the state-derived
    // identity (water). Zero for every other family and for draws that never go through the binder.
    unsigned long long matPipeline = 0;

    // GeneralsX @performance Qoder 19/10/2026 R-05: deque, not vector - the draw path looked a backing up
    // by walking X.geo linearly (247+ entries, twice per batch, 1.27k batches per frame). The two maps make
    // those lookups O(1).
    // GeneralsX @bugfix Qoder 19/10/2026 R-05: the stability the maps need is stricter than "push_back is
    // enough": erasing from the MIDDLE of a deque relocates the surviving entries, and a map that stored a
    // raw pointer then names the wrong object (the round-3 forensics caught a Bind returning another
    // buffer's VkBuffer, two keys naming one entry, and a memcpy into the wrong host mapping - the memory
    // corruption that fast-failed the process). So entries are never erased: a removed backing is tombstoned
    // (freed=1, buffers retired) and its slot is recycled from geoFree, and the deque only ever grows at the
    // back, which is the one operation that keeps every existing element address valid.
    std::deque<VKGeoBack> geo;
    std::vector<VKGeoBack*> geoFree;                  // tombstoned slots, reused instead of push_back
    std::unordered_map<void*, VKGeoBack*> geoByKey;   // WW3D buffer object -> its backing
    std::unordered_map<VkBuffer, VKGeoBack*> geoByBuf; // the bound VkBuffer handle -> its backing
    unsigned long long geoGenSeq = 0;                 // monotonic; every lock of every backing takes a new one

    // Cached answer of the scan stage: for a static index range, the highest index in it. Valid only while
    // the backing's generation still matches, which is the whole point for the WW3D mesh index buffers -
    // they are written at load and never rewritten, so after the first frame the scan is free.
    struct IdxMaxEnt {
        VkBuffer           buf;
        unsigned long long ioff;
        unsigned long long words;
        unsigned long long gen;
        unsigned           maxIndex;
    };
    std::unordered_map<unsigned long long, IdxMaxEnt> idxMax;

    std::vector<VKGeoZombie> geoZombies;

    VkShaderModule   vs = nullptr, ps = nullptr;
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the fixed-function vertex stage and the
    // default-vertex stream that backs whichever of its inputs the batch's FVF does not provide. Both are
    // nullptr when GENERALSX_VKFFVERT is off, which keeps the wedge-5 mechanism path intact: a submit that
    // carries no FF state block (the probes) still gets the passthrough vertex stage.
    VkShaderModule   ffVs = nullptr;
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7: the interpreted fixed-function pixel stage,
    // the other half of the same reversible gate pair. It is built only when GENERALSX_VKFFPIX is on AND
    // the FF block it reads was made, because vkff.frag consumes the FF state block and the vertex gate is
    // what puts that block in every submit's descriptor.
    VkShaderModule   ffPs = nullptr;
    VkShaderModule   ffPsHdr = nullptr;   // P6: FF variant without the final 1.0 clamp
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 8: shader modules stood up from the translated
    // program registry, one per (tag, stage), created on the first pipeline that names it. The registry is
    // keyed by the exact HLSL source the facade compiled; this layer only turns its rows into modules.
    struct VKPgmModule { unsigned tag; unsigned pixel; VkShaderModule mod; };
    std::vector<VKPgmModule> pgmModules;
    VkBuffer         dummyVb = nullptr;
    VkDeviceMemory   dummyVbMem = nullptr;
    VkPipelineLayout layout = nullptr;

    // All-zero constant bindings. A draw whose caller has no constants at all (the mechanism probes, a
    // batch recorded before the facade started shadowing) still needs a complete descriptor set, because
    // Vulkan refuses an incomplete one at draw time where D3D9 simply read nothing. The third of these is
    // the fixed-function block from wedge 6 (matrices, material, 8 lights, fog, stage selectors); it is
    // still named "light" because that was its only content until the vertex stage arrived.
    VkBuffer       zeroVtx = nullptr, zeroPix = nullptr, zeroLight = nullptr;
    VkDeviceMemory zeroVtxMem = nullptr, zeroPixMem = nullptr, zeroLightMem = nullptr;
    unsigned long long zeroVtxBytes = 256ull * 16ull, zeroPixBytes = 224ull * 16ull, zeroLightBytes = VKFF_STATE_BYTES;

    // The three dynamic uniform rings the real payloads go through: vertex, pixel, lights.
    VKDrawConstRing cr[3];
    unsigned long long constAlign = 256;   // minUniformBufferOffsetAlignment, asked of the device

    std::vector<VKDrawPass> passes;
    std::vector<VKDrawFB>   fbs;
    std::vector<VKDrawPSO>  psos;
    // GeneralsX @performance Qoder 19/10/2026 R-05: hash buckets over the pipeline cache, same contract as
    // setIdx - the key hash only picks candidates and pso_key_equal plus the attrs compare still decides.
    std::unordered_multimap<unsigned long long, unsigned> psoIdx;

    // Per-in-flight-slot descriptor storage: reset with the slot, so a set is never recycled while the
    // frame that bound it is still in flight.
    VkDescriptorPool pool[VKGEO_SLOT_COUNT] = {};
    std::vector<VKDrawSetEntry> sets[VKGEO_SLOT_COUNT];
    // GeneralsX @performance Qoder 19/10/2026 R-05: hash buckets over the per-slot set cache. The lookup
    // compared every field of every earlier entry (four memcmp per entry, ~81 entries per frame, ~1.2k
    // batches per frame); the bucket narrows the candidates to the entries that share the key's hash and
    // the full comparison still runs inside the bucket, so a hash collision can never bind a wrong set.
    // Cleared whenever a slot's cache is cleared.
    std::unordered_multimap<unsigned long long, unsigned> setIdx[VKGEO_SLOT_COUNT];

    // Scratch rings for the two paths that have no WW3D buffer object behind them: triangle-fan index
    // rewriting (Vulkan has no fan topology) and DrawPrimitiveUP vertex data.
    VkBuffer       fanBuf = nullptr, upBuf = nullptr;
    VkDeviceMemory fanMem = nullptr, upMem = nullptr;
    void*          fanMap = nullptr, *upMap = nullptr;
    unsigned long long fanCur = 0, upCur = 0;
    static const unsigned long long SCRATCH_CAP = 8ull << 20;

    // Recording state for the frame in progress.
    VkCommandBuffer cmd = nullptr;
    int       frameOpen = 0;
    // D3D9 lets a Clear arrive after the target is set but before BeginScene, and WW3D::Begin_Render does
    // exactly that (Set_Viewport, Clear, then Begin_Scene - ww3d.cpp:926). The Vulkan frame only exists from
    // Begin_Scene on, so such a request is parked here and applied by the Begin that opens this frame. The
    // scene pass loads rather than clears, so a parked clear that is never applied leaves last frame's
    // depth in the attachment and every 3D batch of the frame fails the depth test.
    int     pendingClear = 0;
    int     pendingClearColor = 0, pendingClearZStencil = 0;
    float   pendingColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    float   pendingZ = 0.0f;
    unsigned pendingStencil = 0;
    uint32_t  slot = 0;
    uint32_t  acquired = 0;
    int       haveImage = 0;
    int       inPass = 0;
    unsigned  curPass = 0;
    void*     colorKey = nullptr;
    void*     depthKey = nullptr;
    unsigned  w = 0, h = 0;
    // Target-shape and unsupported-shape problems repeat every frame; the facade bugs they expose are
    // worth four log lines each and not four thousand.
    unsigned  openPassWarns = 0, topologyWarns = 0, streamWarns = 0, indexWarns = 0, swapWarns = 0;
    // Set when an acquire or present reported a swapchain that no longer matches the surface. The present
    // dispatch reads it through VKDraw_Resize_Needed() and recreates the swapchain; Begin_Frame clears it.
    unsigned  resizePending = 0;

    // Scene target keys: addresses of statics, so they cannot alias a recycled D3D pointer.
    char sceneColorKey[8] = {};
    char sceneDepthKey[8] = {};
    // GeneralsX @feature 23/10/2026 P6 "底片/镜头": when any of HDR / SSAA / 3D-only AA is on, the 3D
    // scene is resolved into this LDR overlay before the UI is drawn, so tonemap/bloom/AA cannot touch
    // the HUD. The overlay is always display-sized; the scene may be larger (RenderScale).
    char overlayColorKey[8] = {};
    int  overlayActive = 0;
    int  overlayFailed = 0;
    // 6B: water reflection image adopted from the engine's render-target wrapper. The graph owns its
    // barrier/lifetime policy; this key is what the graph target layer resolves the name to.
    void* waterReflectionKey = nullptr;
    unsigned waterReflectionW = 0, waterReflectionH = 0, waterReflectionFormat = 0;
    int  hdrOn = 0;
    float hdrExposure = 0.0f;
    int  hdrTonemap = 1;
    int  aaMode = 0;
    int  renderScale = 100;
    unsigned displayW = 0, displayH = 0;
    // Phase 6B: sceneColor/sceneDepth/overlay lifetime is owned by W3DRenderGraph and reaches this layer
    // only through VKDraw_Graph_Alloc_Target()/VKDraw_Graph_Free_Target() and the scene descriptor.

    // Render resolution of the scene mirror (0 = follow the swapchain) and the destination rect of the
    // end-of-frame blit (0 width = full extent). Together these are what keeps a game rendered at 1024x768
    // inside a 2560x1440 window letterboxed exactly like the D3D9 path does.
    unsigned sceneW = 0, sceneH = 0;
    // GeneralsX @bugfix 23/10/2026: a render-resolution/device change rebuilds the scene colour/depth
    // pair. The engine's own clear is normally parked and applied below, but a frame that loses that
    // race starts with an undefined depth image and every 3D fragment fails DEPTH_LEQUAL. Force one
    // depth clear on the first frame after the extent changes; colour is only forced when the engine
    // did not already park a colour clear, so an intentional fog/clear colour is preserved.
    unsigned forceClearOnBegin = 0;
    unsigned rectX = 0, rectY = 0, rectW = 0, rectH = 0;

    // ---- GeneralsX @feature Qoder 21/10/2026 in-game vegetation -------------------------------------
    // A self-contained instanced pass recorded inside the scene's open pass from W3DGrassBuffer. The
    // instance stream carries (base,width), (height,lean,phase) and tint; both shaders synthesise their
    // geometry from gl_VertexIndex, so there is no per-vertex buffer and no descriptor set - the camera
    // and fog travel in a 128-byte push constant block, the maximum the spec guarantees.
    struct VKVegState {
        bool failed = false, ready = false;
        VkShaderModule gvs = nullptr, gfs = nullptr, rvs = nullptr, rfs = nullptr;
        VkPipelineLayout layout = nullptr;
        VkPipeline gpipe = nullptr, rpipe = nullptr;
        VkRenderPass pass = nullptr;
        unsigned samples = 1;
        VkBuffer     buf[VKGEO_SLOT_COUNT] = {};
        VkDeviceMemory mem[VKGEO_SLOT_COUNT] = {};
        void*        map[VKGEO_SLOT_COUNT] = {};
        unsigned long long cap[VKGEO_SLOT_COUNT] = {};
        unsigned long long draws = 0, instances = 0;
    };
    VKVegState veg;

    // 光影升级2.0 B1: GPU particle scaffolding (own pipeline, recorded inside the open 3D pass).
    // Step 2 adds a compute pass that simulates the particles into a per-slot SSBO; the graphics
    // pipeline only reads it, so the retail spawn path can be attached later without touching this.
    struct VKFXState {
        bool failed = false, ready = false;
        VkShaderModule vs = nullptr, fs = nullptr, cs = nullptr;
        VkSampler sampler = nullptr;
        VkDescriptorSetLayout dsl = nullptr;
        VkDescriptorPool pool = nullptr;
        VkPipelineLayout layout = nullptr;
        VkPipeline pipe = nullptr;      // graphics
        VkPipeline cpipe = nullptr;     // compute
        VkRenderPass pass = nullptr;
        VkBuffer     pbuf[VKGEO_SLOT_COUNT] = {};
        VkDeviceMemory pmem[VKGEO_SLOT_COUNT] = {};
        void*        pmap[VKGEO_SLOT_COUNT] = {};
        VkBuffer     sbuf[VKGEO_SLOT_COUNT] = {};   // this frame's retail spawn records (host-written)
        VkDeviceMemory smem[VKGEO_SLOT_COUNT] = {};
        void*        smap[VKGEO_SLOT_COUNT] = {};
        VkImageView  tex[VKGEO_SLOT_COUNT] = {};     // batch texture currently written into each set
        void*        softKey = nullptr;              // depth-copy mirror key (this layer owns it)
        VkImageView  softView = nullptr;
        unsigned     softW = 0, softH = 0, softFmt = 0;
        VkImageView  softBound[VKGEO_SLOT_COUNT] = {};
        VkImageView  texCur = nullptr;               // this frame's batch texture
        int          multiTex = 0;                   // distinct textures seen in one frame (logged only)
        unsigned spawnCursor = 0;
        unsigned long long spawnFrame = ~0ull;
        unsigned long long spawned = 0;
        VkDescriptorSet dset[VKGEO_SLOT_COUNT] = {};
        VkBuffer     ibuf[VKGEO_SLOT_COUNT] = {};
        VkDeviceMemory imem[VKGEO_SLOT_COUNT] = {};
        void*        imap[VKGEO_SLOT_COUNT] = {};
        unsigned count = 0;
        unsigned long long dispFrame = ~0ull;
        unsigned draws = 0;

        // B2 weather: same descriptor layout and SSBO shape, own buffers/pipelines/push block.
        VkShaderModule wvs = nullptr, wfs = nullptr, wcs = nullptr;
        VkPipelineLayout wlayout = nullptr;
        VkPipeline wpipe = nullptr, wcpipe = nullptr;
        VkRenderPass wpass = nullptr;
        VkBuffer     wbuf[VKGEO_SLOT_COUNT] = {};
        VkDeviceMemory wmem[VKGEO_SLOT_COUNT] = {};
        void*        wmap[VKGEO_SLOT_COUNT] = {};
        VkDescriptorSet wset[VKGEO_SLOT_COUNT] = {};
        bool         wready = false, wfailed = false;
        unsigned     wcount = 0;
        unsigned long long wdispFrame = ~0ull;
        unsigned long long wdraws = 0;
        // Shipped switches (Options.ini), pushed by VKDraw_GPU_FX_Configure.
        int          cfgParticles = 0;
        int          cfgWeather = 0;
        int          cfgSoft = 0;
        // A7 heat sources: up to 4 warm additive spawns of this frame (world pos + radius).
        float        heat[16] = {};
        unsigned     heatCount = 0;
        int          heatOn = -1;
    };
    VKFXState fx;

    VKDraw_Stats st = {};
    unsigned long long submitTicks = 0;

    // ---- GeneralsX @feature Qoder 20/10/2026 native sun shadow -------------------------------------
    // Casters for the frame being recorded; filled by VK_Draw_Submit, consumed and cleared by
    // VKDraw_Shadow_Apply between the 3D views and the UI.
    std::vector<VKShadowCaster> casters;
    // The last scene batch's matrices: the camera the receiver reconstructs world positions with.
    float shView[16] = {}, shProj[16] = {};
    // Last frame's sun matrix, copied out of shadow_build_light for the post pass's fog shafts. Zero
    // means the shadow pass did not run, and the shader then leaves the sun term at visibility 1.
    float shLightVP[16] = {};
    // GeneralsX @bugfix Qoder 21/10/2026: the viewport that produced the scene depth, in target pixels.
    // The tactical view is 0,0,W,0.8H (control bar), not the whole target, so the screen-space shadow
    // receiver must unproject through this rect instead of assuming full-screen NDC.
    float shViewport[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    // Hysteresis state for the sun box side: a camera idling on a 256-unit quantization boundary used
    // to flip the size every frame (shell-map flicker). 0 = no previous fit (map change / first frame).
    float shFitSide = 0.0f;
    int   shHaveVP = 0;
    bool  shEnvChecked = false, shOn = true, shFailed = false, shReady = false;
    // GeneralsX @debug Qoder 20/10/2026 A/B capture: flip the pass every N presented frames so one run
    // yields interleaved on/off pairs at the same camera path. Temporary forensic switch.
    unsigned shAlt = 0u;
    // GeneralsX @debug Qoder 20/10/2026 three-way A/B: 0 = retail decal/stencil only, 1 = native only,
    // 2 = both. The retail mute is read by DoShadows() later in this same frame.
    bool     abOn = false;
    unsigned abMode = 0u;
    // GeneralsX @tweak Qoder 21/10/2026 far-view salt-and-pepper: the fit is capped at 4096 world
    // units, so a 2048 map gives 2 units per texel and a detailed caster aliases into holes that the
    // PCSS taps turn into stipple. Match the reference's top tier: 4096 keeps 1 unit/texel at the
    // cap. A QA run can still force 1024/2048/4096 through GENERALSX_VKSHADOW_SIZE.
    unsigned shMapSize = 4096u;
    VkShaderModule shCastVs = nullptr, shCastPs = nullptr, shCastAlphaPs = nullptr;
    VkShaderModule shRecvVs = nullptr, shRecvPs = nullptr;
    VkPipelineLayout shCastLayout = nullptr, shCastAlphaLayout = nullptr, shRecvLayout = nullptr;
    VkDescriptorSetLayout shSetLayout = nullptr, shCastSetLayout = nullptr;
    VkDescriptorPool shPool = nullptr;
    // Alpha-tested casters each need the texture and sampler the scene used, so the cutout matches the
    // visible leaves. One descriptor set per distinct pair, recycled with the slot that recorded them.
    VkDescriptorPool shCastPool[VKGEO_SLOT_COUNT] = {};
    std::vector<VKShadowCastSet> shCastSets[VKGEO_SLOT_COUNT];
    VkDescriptorSet shSets[VKGEO_SLOT_COUNT] = {};
    void* shSetViews[VKGEO_SLOT_COUNT][3] = {};
    VkSampler shSampler = nullptr;
    // GeneralsX @feature 23/10/2026 cloud shadows on every opaque surface: the receiver samples the
    // game's cloud map in world XZ. Params: xy scroll, z world->uv scale, w strength (0 = inert).
    VkImageView shCloudView = nullptr;
    VkSampler shCloudSampler = nullptr;
    float shCloudParams[4] = { 0.0f, 0.0f, 1.0f / 315.0f, 0.0f };
    // GeneralsX @refactor 23/10/2026: material cast flag, toggled by the sky/grass draws so the
    // receiver does not replay a skybox or grass blades as occluders.
    bool shCasterAllowed = true;
    std::vector<VKShadowCastPSO> shCastPSOs;
    VkPipeline shRecvPSO = nullptr;
    unsigned shRecvPass = 0xFFFFFFFFu;
    char shColorKey[8] = {}, shDepthKey[8] = {};
    float shGroundZ = 0.0f;
    int   shGroundInit = 0;
    unsigned long long shFrames = 0ull, shCasterTotal = 0ull, shReplayDraws = 0ull, shNoCasterFrames = 0ull;
    unsigned long long shFramesLogged = 0ull;
    // GeneralsX @debug Qoder 20/10/2026 periodic-vanish forensics: the fit inputs that can flip a frame
    // from "shadowed" to "fully lit" without changing any of the counters the trace already prints.
    unsigned shLastHits = 0u;
    float shLastCam[3] = { 0.0f, 0.0f, 0.0f };
    float shLastProj[3] = { 0.0f, 0.0f, 0.0f };

    // ---- GeneralsX @feature Qoder 20/10/2026 Forward+ -----------------------------------------------
    // One host-visible dynamic UBO, one 64 KiB slice per in-flight frame slot. The slice carries the
    // frame's point lights and a coarse screen-tile index list; GN pixel shaders read it through the
    // already-existing binding 3 (the FF light block) whenever c31.x says the tile path is live.
    VkBuffer fpBuf = nullptr;
    VkDeviceMemory fpMem = nullptr;
    void* fpMap = nullptr;
    // GeneralsX @feature 26/10/2026 volumetric fog UBO, one 256 B slice per in-flight slot.
    VkBuffer fogBuf = nullptr;
    VkDeviceMemory fogMem = nullptr;
    void* fogMap = nullptr;
    unsigned long long fpBuiltFrame = ~0ull;
    // GeneralsX @bugfix Qoder 24/10/2026 P4 D2: the block used to be built by whichever draw into the
    // scene target came first - in the shell menu that is a 2D UI batch with an orthographic projection,
    // so every light landed outside the tile grid and no surface was lit for the whole frame. Remember
    // the exact camera the block was clustered for and rebuild when the draw's camera differs.
    float fpBuiltView[16] = { 0 };
    float fpBuiltProj[16] = { 0 };
    int fpBuiltCam = 0;               // fpBuiltView/fpBuiltProj hold a camera
    int fpActive = 0;                 // this frame's block has at least one light in some tile
    int fpEnvChecked = 0, fpEnvOn = 1;
    unsigned fpTilesX = 0, fpTilesY = 0, fpTileSize = 0;
    unsigned long long fpBuilds = 0ull, fpLightTotal = 0ull, fpTileMaxLights = 0ull;
};
VKDrawState X;

void vksky_destroy();   // defined with the sky cubemap path below
bool sky_ready_internal();   // ditto: whether the retail sky cubemap is uploaded and bindable
void* vksky_view_internal();      // the cube view, or null while no sky is uploaded
void* vksky_sampler_internal();   // linear clamp sampler owned by the sky path

bool vkdraw_env_on()
{
    if (!X.envChecked) {
        X.envChecked = true;
        X.envOn = vk_env_gate("GENERALSX_VKDRAW");
    }
    return X.envOn;
}

// The core singleton is file-local inside VulkanCore.cpp (it lives in an anonymous namespace there), so
// this translation unit reaches the device through the accessor declared in VulkanCore.h.
VulkanCore* core_() { return VulkanCore_Get(); }

VkDevice dev_() { return core_() ? core_()->Device() : VK_NULL_HANDLE; }

uint32_t memtype_(VkMemoryRequirements req, VkMemoryPropertyFlags need)
{
    VkPhysicalDeviceMemoryProperties mp;
    G.vkGetPhysicalDeviceMemoryProperties(core_()->Phys(), &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) return i;
    return 0xFFFFFFFFu;
}

// Host-visible, coherent, persistently mapped buffer. Wedge 5 deliberately keeps geometry host-visible:
// the CPU writes these batches every frame anyway, and moving static buffers to device-local plus a
// staging copy is a throughput change rather than a correctness one - it belongs to the performance pass.
// 'outMap' may be null: buffers that only ever hold pre-zeroed constants (the wedge-5 placeholder UBOs)
// are filled through a temporary mapping and left unmapped, because nothing writes them again.
bool make_host_buffer(unsigned long long bytes, VkBufferUsageFlags usage, VkBuffer* outBuf,
                      VkDeviceMemory* outMem, void** outMap)
{
    if (!bytes || !outBuf || !outMem) return false;
    VkDevice dev = dev_();
    if (!dev) return false;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = bytes; bci.usage = usage; bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!chk("vkCreateBuffer(wedge5)", G.vkCreateBuffer(dev, &bci, nullptr, outBuf))) return false;
    VkMemoryRequirements req; G.vkGetBufferMemoryRequirements(dev, *outBuf, &req);
    // GeneralsX @performance Qoder 19/10/2026 R-05 second wave: prefer the CACHEABLE host type. Without it
    // NV hands back write-combined pages, and every READ this layer does - the ring's reuse memcmp over
    // the last payload, the scan stage's index window, the fan's index rewrite - runs at WC uncached speed
    // (the measured miss cost of the scan cache, ~21us per re-read, is exactly that). Writes are equally
    // fast either way, so this only speeds up the read side. Coherent stays required: no flush path exists.
    const uint32_t cachedMt = memtype_(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                             | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    const uint32_t mt = cachedMt != 0xFFFFFFFFu
        ? cachedMt
        : memtype_(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == 0xFFFFFFFFu) {
        VKWP1_LOG("FAIL no host-visible memory type for %llu bytes", bytes);
        G.vkDestroyBuffer(dev, *outBuf, nullptr); *outBuf = nullptr; return false;
    }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    if (!chk("vkAllocateMemory(wedge5)", G.vkAllocateMemory(dev, &mai, nullptr, outMem))) {
        G.vkDestroyBuffer(dev, *outBuf, nullptr); *outBuf = nullptr; *outMem = nullptr; return false;
    }
    void* tmp = nullptr;
    if (!chk("vkBindBufferMemory(wedge5)", G.vkBindBufferMemory(dev, *outBuf, *outMem, 0)) ||
        !chk("vkMapMemory(wedge5)", G.vkMapMemory(dev, *outMem, 0, req.size, 0, &tmp))) {
        if (*outMem) G.vkFreeMemory(dev, *outMem, nullptr);
        *outMem = nullptr;
        G.vkDestroyBuffer(dev, *outBuf, nullptr); *outBuf = nullptr;
        if (outMap) *outMap = nullptr;
        return false;
    }
    memset(tmp, 0, (size_t)req.size);
    if (outMap) *outMap = tmp;
    else G.vkUnmapMemory(dev, *outMem);
    ++X.st.buffersCreated;
    return true;
}

void free_host_buffer(VkBuffer buf, VkDeviceMemory mem, void** map)
{
    VkDevice dev = dev_();
    if (!dev) return;
    if (mem && map && *map) G.vkUnmapMemory(dev, mem);
    if (mem) G.vkFreeMemory(dev, mem, nullptr);
    if (buf) G.vkDestroyBuffer(dev, buf, nullptr);
    if (map) *map = nullptr;
}

// ---------------------------------------------------------------------------
// Forward+ point-light feed (see Backend/VulkanForward.h)
// ---------------------------------------------------------------------------
// The block layout below is the std140 image of the GLSL `GNForwardPlus` block declared by the GN
// fragment shaders. It travels through binding 4 as a read-only SSBO: the block is 90,272 bytes,
// which is above NVIDIA's 65,536-byte maxUniformBufferRange, so a UBO is not a legal home for it.
// Each in-flight slot gets one 128 KiB slice of a host-visible storage buffer, written once per
// frame at the first scene draw.
enum {
    // 3D clustered feed: 1024 clusters = screen tiles x 8 eye-depth slices, 16 lights per cluster,
    // selected per cluster by proximity (see fp_build_block) rather than by global strength. Layout
    // v2 is 160 (header) + 8192 (512 vec4 lights) + 1024*20*4 (clusters) + 8192 (512 vec4 spot/cookie)
    // = 98,464 bytes; v2 appended the aux array so no v1 offset moved.
    VKFP_MAX_LIGHTS    = VkLighting::kMaxLights,
    VKFP_MAX_CLUSTERS  = VkLighting::kMaxClusters,
    VKFP_Z_SLICES      = 8,
    VKFP_MAX_PER_CLUSTER = VkLighting::kMaxPerCluster,
    VKFP_CLUSTER_UINTS = VkLighting::kClusterUints,   // count + 16 one-uint indices + 3 pad
    VKFP_MAX_TILES     = VKFP_MAX_CLUSTERS,          // legacy name used by the stats/probes
    VKFP_MAX_PER_TILE  = VKFP_MAX_PER_CLUSTER,
    VKFP_TILE_UINTS    = VKFP_CLUSTER_UINTS,
    VKFP_SLOT_BYTES    = VkLighting::kSlotBytes
};

static float s_fpLights[VKFP_MAX_LIGHTS][8];
// GeneralsX @feature Qoder 24/10/2026 P4 D2: per-light spot/cookie record, indexed exactly like
// s_fpLights and sorted with it, so the cluster index a shader reads still names the right cone.
// (dir.xyz, cosOuter) / (cosInner, cookieId, kind, exponent); a zeroed row is a point light.
static float s_fpLightAux[VKFP_MAX_LIGHTS][8];
// GeneralsX @feature 24/10/2026 P13 A6: admission key per staged light, parallel to s_fpLights and
// descending, so the staging list is always strongest-first. See fp_admit_light().
static float s_fpLightScore[VKFP_MAX_LIGHTS];
// Every Add_* call this frame, before the layout cap / budget are applied. Diagnostics only.
static int   s_fpPublished = 0;
// GeneralsX @feature Qoder 22/10/2026 light shadows: the strongest published light (index 0 after the
// score sort in fp_build_block) plus the state of its top-down shadow map. Default OFF: the feature is
// opt-in until it has been evaluated on hardware, so the shipping look cannot regress.
static float s_fpTopLight[8];
static int   s_fpTopValid = 0;
// GeneralsX @feature 24/10/2026 P34 E3: up to four independent light-shadow maps, one per light-list
// slot. Slot s in fpLights is shadowed by map s; the CPU swaps the chosen lights into slots 0..N-1 so
// the receiver needs no lookup table (see fp_build_block and the pass below).
static int   s_lsKeyObj[VkLighting::kShadowMaps] = { 0, 0, 0, 0 };  // stable dummy keys for VKRT
static void* const s_lsKey[VkLighting::kShadowMaps] = {
    (void*)&s_lsKeyObj[0], (void*)&s_lsKeyObj[1], (void*)&s_lsKeyObj[2], (void*)&s_lsKeyObj[3]
};
// GeneralsX @bugfix 24/10/2026 P17 D1: each light map needs its own depth target. It used to borrow the
// sun map's depth (4096x4096) while its colour target is 1024x1024, and open_pass() refuses a framebuffer
// whose depth and colour sizes disagree - so the strongest-light shadow map was never rendered once.
static int   s_lsDepthObj[VkLighting::kShadowMaps] = { 0, 0, 0, 0 };
static void* const s_lsDepthKey[VkLighting::kShadowMaps] = {
    (void*)&s_lsDepthObj[0], (void*)&s_lsDepthObj[1], (void*)&s_lsDepthObj[2], (void*)&s_lsDepthObj[3]
};
static const unsigned s_lsSize = 1024u;
static int   s_lsValid[VkLighting::kShadowMaps] = { 0, 0, 0, 0 };   // map s holds this frame's depth
static float s_lsVP[VkLighting::kShadowMaps][16];                   // row-vector world -> light clip
static float s_lsPosRange[VkLighting::kShadowMaps][4];              // light xyz + reach
// Selection the shadow pass made for the *next* frame's light block, and the bytes it wants there.
static float s_lsSelectPos[VkLighting::kShadowMaps][3];
static int   s_lsSelectCount = 0;
static float s_lsBlock[VkLighting::kShadowVec4 * 4];                // fpLightShadow[20] image
static int   s_lsEnv = -1;
static int   s_lsMaps = -1;                                          // 1..4, GENERALSX_VK_LIGHTSHADOW_MAPS
// GeneralsX @feature 24/10/2026 P34 C4: shared environment for the shaders' fpEnv[] block. Filled by
// VKDraw_Set_Environment() from W3D_RefreshRenderLighting() (the same RenderLightingState every other
// surface reads), copied into each frame slot's block. s_envStrength 0 disables the sky reflection.
static float s_envVec[8][4];
static float s_envStrength = 0.0f;
static int   s_fpLightCount = 0;
// GeneralsX @feature 24/10/2026 P34 E3: last non-empty published list, for the light-shadow pass. The
// world scene publishes its lamps, then the 3D interface scene calls VK_Forward_Begin again and leaves
// the live list empty; the shadow pass (which runs after both) reads this snapshot instead.
static float s_fpSnapLights[VKFP_MAX_LIGHTS][8];
static float s_fpSnapAux[VKFP_MAX_LIGHTS][8];
static int   s_fpSnapCount = 0;
// GeneralsX @bugfix 25/10/2026 P35: the snapshot must belong to THIS frame. Without the age check a
// frame whose world scene published no lights (menu, interface-only frame) reused an old list, and a
// frame whose interface scene published a handful of QA lights preferred those over the world scene's
// 166 (see the light-shadow selection below).
static int   s_fpSnapFrame = -1;
static int   s_fpFresh = 0;      // the game layer began a scene light list this frame
static int   s_fpDirty = 0;      // rebuild the block even if a pre-render pass built an empty one
static int   s_fpMode = 1;       // 0 off, 1 3D clusters, 2 direct 256 (Graphics Options)
static int   s_fpBudget = 128;   // global light cap, 32..256 (T4.4 default)
static int   s_fpUseForTests = -1;
static int   s_fpLog = -1;       // GENERALSX_VK_VBTRACE gates the [VKFP] diagnostic lines
static int   s_fpUnsupported = 0; // device limit check failed: fall back to the per-object list

} // namespace (the forward+ entry points below need external linkage)

// GeneralsX @feature Qoder 20/10/2026 Forward+: public entry points. The state above stays file-local;
// these four functions are the only symbols the game layer and DX8GeomNormal resolve across TUs.
int VK_Forward_Enabled()
{
    if (s_fpMode == 0 || s_fpUnsupported) return 0;
    if (s_fpUseForTests < 0) {
        const char* e = getenv("GENERALSX_VK_FORWARDPLUS");
        // Default ON: the per-object list is the fallback the shaders keep when the tile block is empty.
        s_fpUseForTests = (e && *e) ? ((atoi(e) != 0) ? 1 : 0) : 1;
    }
    return s_fpUseForTests;
}

void VK_Forward_SetMode(int mode)
{
    s_fpMode = (mode >= 0 && mode <= 2) ? mode : 2;
    s_fpDirty = 1;
}

void VK_Forward_SetBudget(int budget)
{
    if (budget < 32) budget = 32;
    if (budget > VKFP_MAX_LIGHTS) budget = VKFP_MAX_LIGHTS;
    s_fpBudget = budget;
    s_fpDirty = 1;
}

void VK_Forward_Begin()
{
    // GeneralsX @feature Qoder 24/10/2026 P4 T4.4: a real per-frame budget. 256 was the hard array
    // cap, not a budget; wire the setter (previously never called) and expose it for QA. 128 keeps
    // the Forward+ list bounded without touching the 256-light layout.
    // GeneralsX @note 24/10/2026 P13: OptionPreferences::getForwardLightBudget() already resolves the
    // QA override (GENERALSX_VK_FP_BUDGET) before the game layer hands the value to SetBudget(), so this
    // first-Begin read is only the fallback for a caller that never sets a budget. Do not add a second
    // override path here: two sources for the same cap is how the earlier budget bugs happened.
    static bool s_budgetFromEnv = false;
    if (!s_budgetFromEnv) {
        s_budgetFromEnv = true;
        const char* ev = getenv("GENERALSX_VK_FP_BUDGET");
        int b = (ev != nullptr && *ev != 0) ? atoi(ev) : 128;
        VK_Forward_SetBudget(b);
    }
    s_fpLightCount = 0;
    s_fpPublished = 0;
    s_fpFresh = 1;
    s_fpDirty = 1;
}

void VK_Forward_Snapshot()
{
    // GeneralsX @feature 24/10/2026 P34 E3: copy the list only while it has content. The world scene
    // publishes its lamps before DRAW(); the 3D interface/overlay scene that follows calls Begin() again,
    // so an unconditional snapshot at the end of the frame would erase exactly the list the shadow pass
    // needs. Nothing else reads this copy.
    if (s_fpLightCount <= 0) return;
    s_fpSnapCount = s_fpLightCount;
    s_fpSnapFrame = (int)core_()->FrameIndex();
    memcpy(s_fpSnapLights, s_fpLights, (size_t)s_fpLightCount * 8u * sizeof(float));
    memcpy(s_fpSnapAux, s_fpLightAux, (size_t)s_fpLightCount * 8u * sizeof(float));
}

// GeneralsX @feature 24/10/2026 P13 A6: strength-aware admission into the Forward+ staging list.
//
// The feed used to drop every light that arrived after the budget-th Add_* call, so *which* lights
// survived was decided by the scene's iteration order. On a night map with ~300 published lamps that is
// arbitrary: a light cluster that iterates first can push out a brighter lamp elsewhere in the frame.
// Two separate caps are involved and must not be confused:
//   - the layout cap (VKFP_MAX_LIGHTS = 256 records, the array the shaders index);
//   - the per-frame budget (Options "ForwardLightBudget", 32..256 after clamping).
// The staging list is now kept sorted by (energy * reach) and capped at the layout cap, so the 256
// strongest of everything published survive; fp_build_block then takes the first `budget` of that list.
// Ties keep the light that was published first, which keeps the frame deterministic.
// GeneralsX @bugfix 26/10/2026 HDR: the Forward+ feed used to stage the published light colours
// verbatim. Those came from LightClass/W3DScene in display space, so every program shader's point/spot
// term (buildings, terrain, trees, water) added a display value where the directional base was already
// linear - the term was 2-3x too strong and its relative weight differed per family. Convert here, at
// the single staging boundary, with the same per-channel EOTF the rig/object publication uses. There is
// deliberately no hue-preserving relight or per-family saturation: the unified contract decodes once.
static inline float vkfp_srgb_ext(float v)
{
    if (v <= 0.0f) return 0.0f;
    if (v <= 1.0f) return (v <= 0.04045f) ? (v / 12.92f) : powf((v + 0.055f) / 1.055f, 2.4f);
    return powf(v, 2.2f);
}

static inline void vkfp_light_color(float& r, float& g, float& b)
{
    if (!DX8Wrapper::VK_HDR_Enabled()) return;
    // GeneralsX @refactor 27/10/2026 unified space: per-channel EOTF, exactly what the directional rig
    // (W3DDisplay's snapshot), the object light colours (dx8normalmap linearLights) and the FF pack use.
    // A warm lamp now has the same chromaticity whether it reaches a surface as a directional light or
    // as a point/spot light; the old hue-preserving re-light and its saturation knob are retired.
    r = vkfp_srgb_ext(r);
    g = vkfp_srgb_ext(g);
    b = vkfp_srgb_ext(b);
}

static void fp_admit_light(const float L[8], const float A[8])
{
    ++s_fpPublished;
    const float reach = (L[7] > 0.0f) ? L[7] : 1.0f;
    const float key = (L[4] + L[5] + L[6]) * reach;
    int n = s_fpLightCount;
    if (n >= VKFP_MAX_LIGHTS) {
        // Full: only a strictly stronger light may evict the weakest kept record. Shifting below
        // overwrites index n, which is exactly the evicted tail.
        if (!(key > s_fpLightScore[VKFP_MAX_LIGHTS - 1])) return;
        n = VKFP_MAX_LIGHTS - 1;
    }
    int pos = n;
    while (pos > 0 && s_fpLightScore[pos - 1] < key) {
        memcpy(s_fpLights[pos], s_fpLights[pos - 1], sizeof(s_fpLights[0]));
        memcpy(s_fpLightAux[pos], s_fpLightAux[pos - 1], sizeof(s_fpLightAux[0]));
        s_fpLightScore[pos] = s_fpLightScore[pos - 1];
        --pos;
    }
    memcpy(s_fpLights[pos], L, sizeof(s_fpLights[0]));
    memcpy(s_fpLightAux[pos], A, sizeof(s_fpLightAux[0]));
    s_fpLightScore[pos] = key;
    s_fpLightCount = n + 1;
}

void VK_Forward_Add_Light(float x, float y, float z, float inner, float outer,
                          float r, float g, float b)
{
    vkfp_light_color(r, g, b);
    const float L[8] = { x, y, z, inner, r, g, b, outer };
    // A point light has no cone. The record must be all-zero: a stale kind=spot would clip it, and the
    // staging rows are reused across frames (only the count resets).
    const float A[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    fp_admit_light(L, A);
}

// GeneralsX @feature Qoder 24/10/2026 P4 D2: spot light with cone + optional procedural cookie. It
// shares the light array (position/colour/range) and only adds the auxiliary record, so clustering,
// budget and the shadow slot apply to spots exactly as they do to points.
void VK_Forward_Add_Spot(float x, float y, float z, float inner, float outer,
                         float r, float g, float b,
                         float dirX, float dirY, float dirZ,
                         float cosInner, float cosOuter, float exponent, int cookieId)
{
    vkfp_light_color(r, g, b);
    const float L[8] = { x, y, z, inner, r, g, b, outer };
    float A[8] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    const float len = sqrtf(dirX * dirX + dirY * dirY + dirZ * dirZ);
    if (len > 1e-6f) {
        A[0] = dirX / len; A[1] = dirY / len; A[2] = dirZ / len; A[3] = cosOuter;
        A[4] = cosInner; A[5] = (float)cookieId; A[6] = (float)VkLighting::kKindSpot;
        A[7] = (exponent > 0.0f) ? exponent : 1.0f;
    }
    // No usable axis: the all-zero aux row degrades this spot to a point rather than lighting in an
    // arbitrary direction.
    fp_admit_light(L, A);
}

int VK_Forward_Active()
{
    // 'fresh' is set by the game layer before the scene draw and cleared at End_Frame; the built-frame
    // check keeps c31 and the descriptor binding in lockstep: the PS only takes the tile path once this
    // frame's block exists, and upload_constants binds exactly that block under the same test.
    return (VK_Forward_Enabled() && s_fpFresh && s_fpLightCount > 0
            && X.fpBuiltFrame == core_()->FrameIndex()) ? 1 : 0;
}

namespace {

// Called at End_Frame: the feed is consumed once, so a frame whose scene does not publish lights (the
// shell screens, shadow-only passes) falls back to the per-object list instead of reusing stale data.
static void VK_Forward_End_Frame()
{
    s_fpFresh = 0;
    s_fpDirty = 0;
}

static void fp_build_block(unsigned slot, const unsigned viewport[4], unsigned w, unsigned h)
{
    if (!X.fpMap || !X.fpBuf || !w || !h) return;
    // The caller (vkdraw_submit) decides when to rebuild: a new frame, a new camera, or a new light list
    // (s_fpDirty, set by VK_Forward_Begin). There is no early-out here any more - a per-frame "build
    // once" was what let a 2D UI batch become the frame's light camera.
    // mode 1 = 3D clusters, mode 2 = direct 256-light loop (visual quality/debug), mode 0 = off.
    const int slices = (s_fpMode == 1) ? VKFP_Z_SLICES : 0;
    unsigned ts = 128u, tx = 0, ty = 0;
    for (;;) {
        tx = (w + ts - 1u) / ts;
        ty = (h + ts - 1u) / ts;
        if ((unsigned long long)tx * ty * (unsigned)slices <= VKFP_MAX_CLUSTERS) break;
        ts <<= 1;
        if (ts > 8192u) { X.fpActive = 0; return; }
    }
    X.fpTilesX = tx; X.fpTilesY = ty; X.fpTileSize = ts;

    VKLightingBuffer* blk = (VKLightingBuffer*)((char*)X.fpMap + (unsigned long long)slot * VKFP_SLOT_BYTES);
    memset(blk, 0, sizeof(VKLightingBuffer));
    // GeneralsX @feature 24/10/2026 P13 A6: apply the per-frame budget to the admission-sorted staging
    // list instead of dropping lights at publish time. s_fpPublished is what the scene asked for; the
    // staging list already holds the strongest VKFP_MAX_LIGHTS of those, strongest first.
    const int staged = s_fpLightCount;
    const int selected = (staged > s_fpBudget) ? s_fpBudget : staged;
    // Mode 0 is the "off" leg. GN already gates on c31, but the terrain PS reads this block directly
    // from binding 4, so switch the count off here as well instead of leaving lights on the ground.
    blk->header.fpHeader[0] = (float)tx; blk->header.fpHeader[1] = (float)ty;
    blk->header.fpHeader[2] = (float)ts; blk->header.fpHeader[3] = (s_fpMode != 0 && !s_fpUnsupported) ? (float)selected : 0.0f;
    const float depthNear = 1.0f, depthFar = 4000.0f;
    blk->header.fpHeader2[0] = (float)slices;
    blk->header.fpHeader2[1] = depthNear;
    blk->header.fpHeader2[2] = depthFar;
    blk->header.fpHeader2[3] = 0.0f;   // filled with the viewport height below (shader flips gl_FragCoord.y)
    // GeneralsX @feature 24/10/2026 P34 C4: the eight header vec4s the shaders declare as fpEnv[] are the
    // shared lighting snapshot: sun, sky/ground ambient, fill, and the environment-reflection strength.
    // They were reserved by P0 and never written, so every shader that wanted sky colour had to invent it.
    // fpEnv[1].w gates the reflection (0 = off) and fpEnv[2].w says whether the real sky cubemap is bound.
    for (int e = 0; e < 8; ++e) {
        memcpy(&blk->header.sunDirIntensity[e * 4], s_envVec[e], sizeof(float) * 4);
    }
    // GeneralsX @debug 27/10/2026 live sun sweep (QA, default 1.0): scale the direction light here, at
    // the one per-frame copy, so terrain/road/trees/water/FF all animate without touching the state.
    if (const float sunSweep = DX8Wrapper::VK_Sun_Sweep(); sunSweep != 1.0f) {
        for (int c = 0; c < 3; ++c) blk->header.sunDirIntensity[1 * 4 + c] *= sunSweep;
    }
    blk->header.sunDirIntensity[3] = 1.0f;
    // GeneralsX @feature 27/10/2026 unified lighting: fpEnv[6].w tells every family whether the scene
    // target holds linear radiance (texture/vertex-colour decode decisions read this instead of a
    // per-family flag).
    blk->header.sunDirIntensity[6 * 4 + 3] = X.hdrOn ? 1.0f : 0.0f;
    blk->header.sunColor[3] = s_envStrength;
    blk->header.skyColor[3] = sky_ready_internal() ? 1.0f : 0.0f;
    // GeneralsX @bugfix Qoder 24/10/2026 P0: direct mode returns before the cluster path's light copy,
    // so every direct-mode draw read an all-zero light array (fpHeader.w said N, fpLights held zeros).
    // Copy the selected list here, before the direct branch, so both modes see the same rows.
    if (s_fpMode != 0 && selected > 0) {
        memcpy(blk->lights, s_fpLights, (size_t)selected * 8u * sizeof(float));
        memcpy(blk->aux, s_fpLightAux, (size_t)selected * 8u * sizeof(float));
    }
    if (slices == 0) {
        // Direct mode: the PS iterates fpHeader.w lights. No cluster pass, so the whole screen sees
        // every light in range. This is the quality reference while the clustered path is validated.
        X.fpActive = (s_fpMode != 0) && selected > 0;
        X.fpBuiltFrame = core_()->FrameIndex();
        s_fpDirty = 0;
        ++X.fpBuilds;
        X.fpLightTotal += (unsigned long long)((s_fpMode != 0) ? selected : 0);
        static int s_directLog = 0;
        if (s_directLog < 3) {
            ++s_directLog;
            fprintf(stderr, "[VKFP] DIRECT frame=%llu count=%d published=%d staged=%d budget=%d\n",
                X.fpBuiltFrame, selected, s_fpPublished, staged, s_fpBudget);
            fflush(stderr);
        }
        return;
    }

    // The staging list is admission-sorted (fp_admit_light), so the first `selected` rows are already
    // the strongest ones; no per-build sort is needed. GeneralsX @refactor 24/10/2026 P13 A6.
    const int count = (s_fpMode == 0) ? 0 : selected;
    // GeneralsX @feature 24/10/2026 P34 E3: up to four independent light-shadow maps. The shadow pass
    // runs after these draws (W3DDisplay calls VKDraw_Shadow_Apply once the scene is recorded), so it
    // cannot touch this frame's block any more. It stores the lights it chose plus their world
    // positions; here those lights are matched back by position and swapped into slots 0..N-1, so slot
    // s of fpLights is exactly the light whose depth map s holds. The receiver then only tests `li == s`
    // and needs no lookup table. A light that vanished between the two steps clears that slot's gate
    // instead of sampling a map that belongs to a different fixture.
    memset(blk->shadow, 0, sizeof(blk->shadow));
    int lsMapped = 0;
    if (count > 0 && s_lsSelectCount > 0) {
        memcpy(blk->shadow, s_lsBlock, sizeof(blk->shadow));
        const int want = (s_lsSelectCount < (int)VkLighting::kShadowMaps)
                       ? s_lsSelectCount : (int)VkLighting::kShadowMaps;
        for (int s = 0; s < want; ++s) {
            float bestD = 1e30f;
            int best = -1;
            for (int i = s; i < count; ++i) {
                const float dx = s_fpLights[i][0] - s_lsSelectPos[s][0];
                const float dy = s_fpLights[i][1] - s_lsSelectPos[s][1];
                const float dz = s_fpLights[i][2] - s_lsSelectPos[s][2];
                const float d = dx * dx + dy * dy + dz * dz;
                if (d < bestD) { bestD = d; best = i; }
            }
            if (best < 0 || bestD > 64.0f) {   // 8 units: a lamp that moved or was culled
                blk->shadow[s * 20 + 19] = 0.0f;
                continue;
            }
            if (best != s) {
                for (int k = 0; k < 8; ++k) {
                    const float t = s_fpLights[s][k];
                    s_fpLights[s][k] = s_fpLights[best][k];
                    s_fpLights[best][k] = t;
                    const float a = s_fpLightAux[s][k];
                    s_fpLightAux[s][k] = s_fpLightAux[best][k];
                    s_fpLightAux[best][k] = a;
                }
            }
            ++lsMapped;
        }
    }
    if (count > 0) {
        memcpy(blk->lights, s_fpLights, (size_t)count * 8u * sizeof(float));
        memcpy(blk->aux, s_fpLightAux, (size_t)count * 8u * sizeof(float));
    }
    // The list is strongest-first; the light maps are built for slots 0..lsMapped-1.
    s_fpTopValid = count > 0 ? 1 : 0;
    if (count > 0) memcpy(s_fpTopLight, s_fpLights[0], sizeof(s_fpTopLight));

    // Camera matrices in the exact row-vector convention the vertex twins use: a world point reaches
    // clip space through object->world (identity for the point) -> view -> projection.
    Matrix4x4 view, proj;
    DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, view);
    DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, proj);
    Matrix4x4 vp = view * proj;
    Matrix4x4 vpT = vp.Transpose();

    const float vx = (float)viewport[0], vy = (float)viewport[1];
    const float vw = viewport[2] ? (float)viewport[2] : (float)w;
    const float vh = viewport[3] ? (float)viewport[3] : (float)h;
    // GeneralsX @bugfix 23/10/2026: gl_FragCoord has a TOP-LEFT origin in Vulkan (the old comment
    // claimed bottom-left, which is OpenGL). The PS converts it to this grid's bottom-up row with
    // (vh - gl_FragCoord.y); the light centres below are built in that same bottom-up space so the
    // two meet. Mixing a top-down grid with the bottom-up lookup mirrored every light.
    blk->header.fpHeader2[3] = vh;
    const float yscale = fabsf(proj[1][1]);

    static unsigned short counts[VKFP_MAX_CLUSTERS];
    static unsigned char  idx[VKFP_MAX_CLUSTERS][VKFP_MAX_PER_CLUSTER];
    // Per-cluster relevance of every candidate, kept sorted descending alongside idx. GeneralsX
    // @bugfix Qoder 22/10/2026: the old fill took lights in global strength order, so eight strong
    // lights from one building (4 lamps + 4 ground washes) could occupy a cluster and push out a
    // nearer, dimmer light that would actually have reached the surface there - a parked tank next to
    // a lit building stayed dark. Selection is now per cluster: score = energy / (1 + (d/reach)^2),
    // with d the world-space distance from the light to that cluster's centre at its slice depth.
    static float scoreCl[VKFP_MAX_CLUSTERS][VKFP_MAX_PER_CLUSTER];
    memset(counts, 0, sizeof(counts));
    int any = 0;
    static int s_fpProbeLog = 0;
    const float depthRange = depthFar - depthNear;
    for (int li = 0; li < count; ++li) {
        const float* L = s_fpLights[li];
        Vector4 clip;
        Matrix4x4::Transform_Vector(vpT, Vector4(L[0], L[1], L[2], 1.0f), &clip);
        if (clip.W <= 0.05f) continue;
        const float inv = 1.0f / clip.W;
        const float sx = vx + (clip.X * inv * 0.5f + 0.5f) * vw;
        // GeneralsX @bugfix 23/10/2026: the point-light shaders reconstruct their row with
        // (vh - gl_FragCoord.y)/ts (see fpHeader2[3]), i.e. they index the tile grid bottom-up. The
        // previous top-down sy mirrored every light onto the opposite half of the screen. Build the
        // grid in the same bottom-up space the shaders sample, not in D3D's top-down screen space.
        const float sy = vy + (0.5f + clip.Y * inv * 0.5f) * vh;
        float rpx = yscale * L[7] * inv * (vh * 0.5f);
        if (!(rpx >= 0.0f) || L[7] <= 0.0f) rpx = (float)ts * 2.0f;   // no far range: conservative
        if (rpx > 4096.0f) rpx = 4096.0f;
        int x0 = (int)floorf((sx - rpx) / (float)ts);
        int x1 = (int)floorf((sx + rpx) / (float)ts);
        int y0 = (int)floorf((sy - rpx) / (float)ts);
        int y1 = (int)floorf((sy + rpx) / (float)ts);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= (int)tx) x1 = (int)tx - 1;
        if (y1 >= (int)ty) y1 = (int)ty - 1;
        if (x0 > x1 || y0 > y1) continue;
        // The cluster's depth axis is the light's own view-space W (eye depth). Expand by the far
        // radius so a light whose centre is outside a slice still lights geometry inside it.
        const float depth = clip.W;
        int z0 = (int)floorf((depth - L[7] - depthNear) / depthRange * (float)slices);
        int z1 = (int)floorf((depth + L[7] - depthNear) / depthRange * (float)slices);
        if (z0 < 0) z0 = 0;
        if (z1 >= slices) z1 = slices - 1;
        if (z0 > z1) continue;
        if (li == 0 && count > 0 && s_fpProbeLog < 3 && (s_fpLog != 0)) {
            ++s_fpProbeLog;
            fprintf(stderr, "[VKFP] L0 world=(%.1f,%.1f,%.1f) r=%.1f depth=%.1f screen=(%.1f,%.1f) rpx=%.1f tile=(%d..%d,%d..%d) z=(%d..%d) vp=(%.0f,%.0f %.0fx%.0f)\n",
                L[0], L[1], L[2], L[7], depth, sx, sy, rpx, x0, x1, y0, y1, z0, z1, vx, vy, vw, vh);
            fflush(stderr);
        }
        const float energy = L[4] + L[5] + L[6];
        const float reach = (L[7] > 1.0f) ? L[7] : 1.0f;
        for (int zz = z0; zz <= z1; ++zz) {
            const float sliceDepth = depthNear + ((float)zz + 0.5f) * (depthRange / (float)slices);
            const float worldPerPix = (yscale > 1e-6f) ? (2.0f * sliceDepth / (yscale * vh)) : 1.0f;
            const float tileWorld = (float)ts * worldPerPix;
            for (int yy = y0; yy <= y1; ++yy) {
                const float ccy = ((float)yy + 0.5f) * (float)ts;
                for (int xx = x0; xx <= x1; ++xx) {
                    const unsigned ci = (unsigned)((zz * (int)ty + yy) * (int)tx + xx);
                    if (ci >= (unsigned)VKFP_MAX_CLUSTERS) continue;
                    const float ccx = ((float)xx + 0.5f) * (float)ts;
                    const float dx = (sx - ccx) * worldPerPix;
                    const float dy = (sy - ccy) * worldPerPix;
                    const float dz = depth - sliceDepth;
                    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
                    // Hard cull: beyond its own reach plus a tile of slack the light cannot contribute.
                    if (dist > reach + tileWorld * 0.75f) continue;
                    const float t = dist / reach;
                    const float sc = energy / (1.0f + t * t);
                    unsigned short& n = counts[ci];
                    if (n < (unsigned short)VKFP_MAX_PER_CLUSTER) {
                        int pos = (int)n;
                        while (pos > 0 && scoreCl[ci][pos - 1] < sc) {
                            scoreCl[ci][pos] = scoreCl[ci][pos - 1];
                            idx[ci][pos] = idx[ci][pos - 1];
                            --pos;
                        }
                        scoreCl[ci][pos] = sc;
                        idx[ci][pos] = (unsigned char)li;
                        ++n;
                        any = 1;
                    } else if (sc > scoreCl[ci][VKFP_MAX_PER_CLUSTER - 1]) {
                        int pos = VKFP_MAX_PER_CLUSTER - 1;
                        while (pos > 0 && scoreCl[ci][pos - 1] < sc) {
                            scoreCl[ci][pos] = scoreCl[ci][pos - 1];
                            idx[ci][pos] = idx[ci][pos - 1];
                            --pos;
                        }
                        scoreCl[ci][pos] = sc;
                        idx[ci][pos] = (unsigned char)li;
                    }
                }
            }
        }
    }

    X.fpTileMaxLights = 0;
    const unsigned clusters = tx * ty * (unsigned)slices;
    for (unsigned ci = 0; ci < clusters; ++ci) {
        const unsigned base = ci * VKFP_CLUSTER_UINTS;
        const unsigned c = counts[ci];
        blk->tiles[base] = c;
        if (c > X.fpTileMaxLights) X.fpTileMaxLights = c;
        for (unsigned j = 0; j < c; ++j)
            blk->tiles[base + 1u + j] = (unsigned)idx[ci][j];
    }
    X.fpActive = any;
    X.fpBuiltFrame = core_()->FrameIndex();
    s_fpDirty = 0;
    ++X.fpBuilds;
    X.fpLightTotal += (unsigned long long)count;
    // GeneralsX @feature 24/10/2026 P16 A5: per-frame selection probe. "Light popping" is a *frame to
    // frame* property of the cluster assignment, so a still screenshot cannot measure it. With
    // GENERALSX_VK_FP_SETLOG=1 every build prints the number of assigned (light, cluster) pairs and the
    // sum of their relevance scores; a smooth camera pan must produce a smooth energy curve, and a
    // discontinuity in that curve is exactly a light being swapped out of a cluster. Rate: one line per
    // build, so a QA run writes a few thousand lines - meant for a log, not for shipping.
    {
        static Int s_setLog = -1;
        if (s_setLog < 0) {
            const char* e = getenv("GENERALSX_VK_FP_SETLOG");
            s_setLog = (e != nullptr && atoi(e) != 0) ? 1 : 0;
        }
        if (s_setLog) {
            double assigned = 0.0;
            unsigned long long pairs = 0ull;
            for (unsigned ci = 0; ci < clusters; ++ci) {
                const unsigned c = counts[ci];
                for (unsigned j = 0; j < c; ++j) { assigned += (double)scoreCl[ci][j]; ++pairs; }
            }
            fprintf(stderr, "[VKFP] setlog frame=%llu vp=%ux%u published=%d staged=%d kept=%d clusters=%u assignedPairs=%llu energy=%.6f maxPerCluster=%llu\n",
                X.fpBuiltFrame, w, h, s_fpPublished, staged, count, clusters, pairs, assigned, X.fpTileMaxLights);
            fflush(stderr);
        }
    }
    // Bounded proof line: how many lights the scene published, how the screen/depth was clustered,
    // and the densest cluster this frame. A run where this never prints is not exercising the feed.
    if (s_fpLog < 0) {
        const char* e = getenv("GENERALSX_VK_VBTRACE");
        s_fpLog = (e && atoi(e)) ? 1 : 0;
    }
    {
        static int s_fpNonEmptyLogged = 0;
        if (s_fpLog && count > 0 && s_fpNonEmptyLogged < 4) {
            ++s_fpNonEmptyLogged;
            Vector4 dbg;
            Matrix4x4::Transform_Vector(vpT, Vector4(s_fpLights[0][0], s_fpLights[0][1], s_fpLights[0][2], 1.0f), &dbg);
            fprintf(stderr, "[VKFP] LIGHTS frame=%llu count=%d tiles=%ux%u z=%u clusters=%u maxPerCluster=%llu active=%d L0clip=(%.1f,%.1f,%.1f,%.1f) L0r=%.1f\n",
                X.fpBuiltFrame, count, tx, ty, (unsigned)slices, clusters, X.fpTileMaxLights, X.fpActive,
                (double)dbg.X, (double)dbg.Y, (double)dbg.Z, (double)dbg.W, s_fpLights[0][7]);
            fflush(stderr);
        }
    }
    if (s_fpLog && (X.fpBuilds <= 3ull || (X.fpBuilds % 600ull) == 0ull)) {
        const unsigned mid = ((slices / 2) * ty + ty / 2) * tx + tx / 2;
        fprintf(stderr, "[VKFP] build #%llu frame=%llu lights=%d tiles=%ux%u z=%u clusters=%u size=%u maxPerCluster=%llu midCluster=%u active=%d\n",
            X.fpBuilds, X.fpBuiltFrame, count, tx, ty, (unsigned)slices, clusters, ts,
            X.fpTileMaxLights, (unsigned)counts[mid], X.fpActive);
        fflush(stderr);
    }
    // GeneralsX @feature 24/10/2026 P13 A6: proof line for the admission/budget split. 'dropped' is how
    // many published lights did not fit the 256-record layout cap; kept is what the shaders iterate.
    if (s_fpLog && s_fpPublished > selected &&
        (X.fpBuilds <= 3ull || (X.fpBuilds % 600ull) == 0ull)) {
        fprintf(stderr, "[VKFP] select frame=%llu published=%d staged=%d kept=%d layoutCap=%d budget=%d\n",
            X.fpBuiltFrame, s_fpPublished, staged, selected, (int)VKFP_MAX_LIGHTS, s_fpBudget);
        fflush(stderr);
    }
}

// ---------------------------------------------------------------------------
// constant rings (wedge 5.1)
// ---------------------------------------------------------------------------
// Why not one reservation per draw: the register budgets are 4 KiB vertex + 3.5 KiB pixel + 1 KiB of
// lights, so a per-draw copy is a 9 KiB memcpy on every batch and a ring large enough for one frame
// would be hundreds of megabytes. Why not one region rewritten in place: the frame that bound it may
// still be in flight. The middle ground is a slot-partitioned ring with a payload-equality check -
// consecutive batches share constants far more often than they share textures, so the copy rate is the
// number of distinct payloads, not the number of draws.
bool const_ring_create(VKDrawConstRing& r, unsigned long long cap, unsigned long long spill_factor)
{
    // 'cap' is the total frame-window budget. Each slot gets that window plus a 'spill_factor'-sized
    // spill, so a frame whose distinct payloads overflow the window appends into the spill instead of
    // restarting at the base over the payloads its earliest recorded batches still bind (the R-02
    // in-frame overwrite). The FF ring takes a 4x spill: the CH02 A/B/C runs measured the burst frames
    // (a city in view, ~28 frames of the save) needing just over 2x the 546-payload window, while the
    // vertex/pixel rings never exceeded their window in any run and stay at the 1x baseline.
    unsigned long long win = cap / (unsigned long long)VKGEO_SLOT_COUNT;
    win = (win / X.constAlign) * X.constAlign;      // a window must start on a legal dynamic offset
    if (!win) return false;
    r.winCap = win;
    r.slotCap = win * (1ull + spill_factor);
    r.cap = r.slotCap * (unsigned long long)VKGEO_SLOT_COUNT;
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) {
        r.cur[i] = (unsigned long long)i * r.slotCap;
        r.lastOff[i] = 0; r.lastBytes[i] = 0;
    }
    return make_host_buffer(r.cap, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &r.buf, &r.mem, &r.map);
}

void const_ring_destroy(VKDrawConstRing& r)
{
    free_host_buffer(r.buf, r.mem, &r.map);
    r.buf = nullptr; r.mem = nullptr; r.map = nullptr; r.cap = 0; r.slotCap = 0; r.winCap = 0;
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) { r.cur[i] = 0; r.lastOff[i] = 0; r.lastBytes[i] = 0; }
}

// Start this slot's window over. Called from Begin_Frame, which has already waited the slot's fence, so
// nothing still in flight can be reading the bytes being handed out again.
void const_ring_reset(VKDrawConstRing& r, unsigned slot)
{
    r.cur[slot] = (unsigned long long)slot * r.slotCap;
    r.lastBytes[slot] = 0;
    r.peak = 0;   // the frame starting on this slot measures its own window high-water
}

// Copy one payload into the slot's window and report the device offset the bind should use. 'budget' is
// the size the shader's block declares, and it is what gets reserved and bound: a dynamic offset only
// moves the window the descriptor may read, so a reservation smaller than the block would let a later
// batch's data show through the registers this one never wrote. Whatever the caller did not fill is
// zeroed, which matches D3D9's "uninitialised constant register reads zero" and keeps the comparison
// below honest about the whole block.
bool const_ring_write(VKDrawConstRing& r, int slot, const void* src, unsigned used,
                      unsigned budget, unsigned long long* offset)
{
    if (!r.map || !src || !offset) return false;
    if (used > budget) used = budget;
    const unsigned long long align = X.constAlign;
    const unsigned long long base = (unsigned long long)slot * r.slotCap;
    const unsigned long long winEnd = base + r.winCap;   // [base,winEnd) window, [winEnd,end) spill
    const unsigned long long end  = base + r.slotCap;
    const unsigned long long need = ((unsigned long long)budget + align - 1) & ~(align - 1);
    if (need > r.winCap) return false;

    // Reuse test: the same bytes already sit at the offset this slot last wrote. That is the whole point
    // of the ring - consecutive batches overwhelmingly repeat their constants, so the copy rate tracks
    // the number of distinct payloads rather than the number of draws.
    if (r.lastBytes[slot] == used &&
        memcmp((const char*)r.map + r.lastOff[slot], src, (size_t)used) == 0) {
        *offset = r.lastOff[slot]; ++r.reuses; return true;
    }
    unsigned long long off = (r.cur[slot] + align - 1) & ~(align - 1);
    if (off + need > winEnd) {
        // GeneralsX @bugfix Qoder 19/10/2026 R-02: the window is full. Append into the spill - the
        // frame's earliest payloads, still bound by recorded commands, keep their bytes. Only when the
        // spill is also exhausted does the cursor restart at the base, and that (a wrap) is now a
        // should-never-happen corruption counter rather than the ordinary end-of-window event.
        if (off < winEnd) { off = winEnd; ++r.spills; }
        else if (off + need > end) { off = base; ++r.wraps; }
    }
    char* dst = (char*)r.map + off;
    memcpy(dst, src, (size_t)used);
    if (used < budget) memset(dst + used, 0, (size_t)(budget - used));
    r.cur[slot] = off + need;
    if (r.cur[slot] - base > r.peak) r.peak = r.cur[slot] - base;
    r.lastOff[slot] = off; r.lastBytes[slot] = used;
    ++r.uploads;
    *offset = off;
    return true;
}

// The constant side of one draw: which buffer each dynamic binding points at, the range written into the
// descriptor, and the offset handed to vkCmdBindDescriptorSets. A caller that supplied no payload binds
// the standing all-zero buffer instead, so the set stays complete either way.
struct VKDraw_Const_Bind {
    void*                buf[3];
    unsigned long long   bytes[3];
    unsigned long long   off[3];
};

void upload_constants(const VKDraw_Submit* s, VKDraw_Const_Bind* out)
{
    const void*    src[3]    = { s->vertex_constants, s->pixel_constants, s->ff };
    const unsigned budget[3] = { (unsigned)X.zeroVtxBytes, (unsigned)X.zeroPixBytes, (unsigned)X.zeroLightBytes };
    // The vec4 counts say how much of each shadow array the facade considers written; the rest of the
    // block is zeroed by the ring. Absent counts mean "the whole budget", which is the safe reading.
    unsigned used[3] = { budget[0], budget[1], budget[2] };
    if (s->vertex_constant_vec4s && s->vertex_constant_vec4s * 16u < budget[0]) used[0] = s->vertex_constant_vec4s * 16u;
    if (s->pixel_constant_vec4s  && s->pixel_constant_vec4s  * 16u < budget[1]) used[1] = s->pixel_constant_vec4s * 16u;
    if (!s->ff) used[2] = 0;
    const VkBuffer zero[3] = { X.zeroVtx, X.zeroPix, X.zeroLight };
    for (int i = 0; i < 3; ++i) {
        out->off[i] = 0; out->bytes[i] = budget[i];
        unsigned long long off = 0;
        if (src[i] && used[i] && const_ring_write(X.cr[i], (int)X.slot, src[i], used[i], budget[i], &off)) {
            out->buf[i] = (void*)X.cr[i].buf; out->off[i] = off;
        } else {
            out->buf[i] = (void*)zero[i];
        }
    }
}

// GeneralsX @performance Qoder 19/10/2026 R-05: one 64-bit avalanche for the three hash indexes below.
// SplitMix64's finalizer: cheap, well-distributed, and it only ever picks candidates - every hit re-checks
// the full identity, so the hash is never trusted alone.
static unsigned long long vkdraw_hash_bytes(const void* p, size_t n)
{
    unsigned long long h = 0x9E3779B97F4A7C15ull + (unsigned long long)n * 0xFF51AFD7ED558CCDull;
    const unsigned char* b = (const unsigned char*)p;
    for (size_t i = 0; i < n; ++i) {
        h = (h ^ b[i]) * 0x100000001B3ull;
    }
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    return h ^ (h >> 31);
}

VKGeoBack* geo_find(void* key)
{
    std::unordered_map<void*, VKGeoBack*>::iterator it = X.geoByKey.find(key);
    return it == X.geoByKey.end() ? nullptr : it->second;
}

// Bucket for the scan cache: (buffer, first byte, word count) -> one key. The entry re-checks all three
// plus the generation, so this only has to spread, not be exact.
static unsigned long long idxmax_key(VkBuffer buf, unsigned long long ioff, unsigned long long words)
{
    unsigned long long h = (unsigned long long)(uintptr_t)buf;
    h = (h ^ (ioff * 0x9E3779B97F4A7C15ull)) * 0xC2B2AE3D27D4EB4Full;
    h ^= words * 0xBF58476D1CE4E5B9ull;
    h ^= h >> 30; h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27; h *= 0x94D049BB133111EBull;
    return h ^ (h >> 31);
}

void geo_destroy(VKGeoBack& b)
{
    free_host_buffer(b.buf, b.mem, &b.map);
    b.buf = nullptr; b.mem = nullptr; b.cap = 0; b.slotBytes = 0;
}

// Hand a backing over to the reaper instead of destroying it. The object it mirrors is gone (or is being
// rewritten at a new size), so nothing will bind it again from now on - but the frames already submitted
// may still be running on the GPU.
void geo_retire(VKGeoBack& b)
{
    if (b.buf) X.geoByBuf.erase(b.buf);
    if (!b.buf && !b.mem) { b.map = nullptr; b.cap = 0; b.slotBytes = 0; return; }
    VKGeoZombie z;
    z.buf = b.buf; z.mem = b.mem; z.map = b.map;
    z.slotsLeft = VKGEO_SLOT_COUNT + 1u;
    X.geoZombies.push_back(z);
    b.buf = nullptr; b.mem = nullptr; b.map = nullptr; b.cap = 0; b.slotBytes = 0;
    ++X.st.backingsRetired;
}

// GeneralsX @bugfix Qoder 19/10/2026 R-05: the one legal way to remove a backing from X.geo. Retire the
// GPU-side buffer, drop both lookup maps, and tombstone the slot for the free list - never erase the
// deque in the middle, because geoByKey/geoByBuf hold raw element pointers and a mid-deque erase
// relocates the survivors (round-3 forensics: two keys naming one entry, a Bind answering with another
// object's VkBuffer, a memcpy into the wrong host mapping, then a stack-cookie fastfail).
void geo_tombstone(VKGeoBack* b, void* key)
{
    geo_retire(*b);
    X.geoByKey.erase(key);
    b->stride = 0; b->slot = 0; b->curOffset = 0; b->curBytes = 0; b->dynamic = 0; b->gen = 0;
    b->freed = 1;
    X.geoFree.push_back(b);
}

// Called once per frame from Begin_Frame, after that slot's fence has been waited, which is the point
// where one more in-flight frame has definitively finished.
void geo_reap()
{
    if (X.geoZombies.empty()) return;
    for (size_t i = 0; i < X.geoZombies.size(); ) {
        if (X.geoZombies[i].slotsLeft > 1u) { --X.geoZombies[i].slotsLeft; ++i; continue; }
        free_host_buffer(X.geoZombies[i].buf, X.geoZombies[i].mem, &X.geoZombies[i].map);
        X.geoZombies.erase(X.geoZombies.begin() + (long)i);
        ++X.st.backingsReaped;
    }
}

// Drain every retired backing now. Only legal behind a device idle (shutdown) or a full teardown.
void geo_reap_all()
{
    for (size_t i = 0; i < X.geoZombies.size(); ++i)
        free_host_buffer(X.geoZombies[i].buf, X.geoZombies[i].mem, &X.geoZombies[i].map);
    X.geoZombies.clear();
}

// Does a VkFormat carry a stencil aspect? Only such an attachment may be cleared with
// VK_IMAGE_ASPECT_STENCIL_BIT, and the scene depth this wedge picks is D32_SFLOAT, which does not - yet
// the engine clears D3DCLEAR_STENCIL on it every frame.
bool vk_format_has_stencil(unsigned f)
{
    return f == (unsigned)VK_FORMAT_D24_UNORM_S8_UINT ||
           f == (unsigned)VK_FORMAT_D32_SFLOAT_S8_UINT ||
           f == (unsigned)VK_FORMAT_D16_UNORM_S8_UINT ||
           f == (unsigned)VK_FORMAT_S8_UINT;
}

// How many bytes a bound geometry buffer can actually be read from. The two scratch rings are not
// per-object backings, so they answer from their own capacity; anything else has to be a backing this
// layer created, or the range check has nothing to check against.
unsigned long long geo_cap_of(VkBuffer buf)
{
    if (!buf) return 0;
    if (buf == X.upBuf || buf == X.fanBuf) return VKDrawState::SCRATCH_CAP;
    std::unordered_map<VkBuffer, VKGeoBack*>::iterator it = X.geoByBuf.find(buf);
    return it == X.geoByBuf.end() ? 0ull : it->second->cap;
}

// The host mapping behind a bound geometry buffer, so a submit can look at the bytes it is about to hand
// the GPU. nullptr when the buffer is not one this layer mapped.
const void* geo_map_of(VkBuffer buf)
{
    if (!buf) return nullptr;
    if (buf == X.upBuf) return X.upMap;
    if (buf == X.fanBuf) return X.fanMap;
    std::unordered_map<VkBuffer, VKGeoBack*>::iterator it = X.geoByBuf.find(buf);
    return it == X.geoByBuf.end() ? nullptr : it->second->map;
}


// ---------------------------------------------------------------------------
// render passes and framebuffers
// ---------------------------------------------------------------------------
// B5: framebuffer for the MSAA pass (4x colour, 4x depth, and the two 1x resolve targets).
VkFramebuffer fb_for_msaa(unsigned passIdx, void* colorMSAA, void* depthMSAA, void* resolveColor,
                          void* resolveDepth, unsigned w, unsigned h)
{
    for (size_t i = 0; i < X.fbs.size(); ++i) {
        const VKDrawFB& e = X.fbs[i];
        if (e.colorView == colorMSAA && e.depthView == depthMSAA && e.w == w && e.h == h && e.pass == passIdx)
            return e.fb;
    }
    VkImageView atv[4] = { (VkImageView)colorMSAA, (VkImageView)depthMSAA,
                           (VkImageView)resolveColor, (VkImageView)resolveDepth };
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = X.passes[passIdx].rp;
    fbi.attachmentCount = 4; fbi.pAttachments = atv;
    fbi.width = w; fbi.height = h; fbi.layers = 1;
    VkFramebuffer fb = nullptr;
    if (!chk("vkCreateFramebuffer(msaa)", G.vkCreateFramebuffer(dev_(), &fbi, nullptr, &fb))) return nullptr;
    VKDrawFB e; e.colorView = colorMSAA; e.depthView = depthMSAA; e.w = w; e.h = h; e.pass = passIdx; e.fb = fb;
    X.fbs.push_back(e);
    ++X.st.framebuffers;
    return fb;
}

unsigned pass_for(unsigned colorFmt, unsigned depthFmt, unsigned samples = 1u,
                  unsigned resolveColorFmt = 0u, unsigned resolveDepthFmt = 0u)
{
    unsigned long long sig = (unsigned long long)colorFmt | ((unsigned long long)depthFmt << 16)
                           | ((unsigned long long)samples << 32) | (1ull << 48)
                           | ((unsigned long long)resolveColorFmt << 49);
    for (size_t i = 0; i < X.passes.size(); ++i)
        if (X.passes[i].sig == sig) return (unsigned)i;

    VkAttachmentDescription att[4] = {};
    unsigned n = 1;
    // LOAD/STORE rather than CLEAR: DX8Wrapper::Clear is the engine's own clear and it arrives as a
    // vkCmdClearAttachment inside the pass, which keeps the D3D ordering (target set, then clear, then
    // draws, and a clear that happens before any draw still wins).
    att[0].format = (VkFormat)colorFmt; att[0].samples = (VkSampleCountFlagBits)samples;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
    // Both colour transitions are the pass's, and the recorded layout in the render-target table is
    // refreshed from finalLayout when the pass ends. That pairing is what stops wedge 3.4's explicit
    // barriers and wedge 5's implicit ones from fighting over the same image.
    att[0].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att[0].finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference dref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    unsigned deps = 2;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL; dep[0].dstSubpass = 0;
    dep[0].srcStageMask = (VkPipelineStageFlags)(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dep[0].dstStageMask = (VkPipelineStageFlags)(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT);
    dep[0].dstAccessMask = (VkAccessFlags)(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    dep[1].srcSubpass = 0; dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = (VkPipelineStageFlags)(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
    dep[1].srcAccessMask = (VkAccessFlags)(VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    if (depthFmt) {
        att[1].format = (VkFormat)depthFmt; att[1].samples = (VkSampleCountFlagBits)samples;
        att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD; att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        // A depth format with no stencil aspect must not carry a stencil load or store - that pair has its
        // own validity rules, and the scene depth this wedge prefers (D32_SFLOAT) is exactly that case.
        if (!vk_format_has_stencil(depthFmt)) {
            att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[1].finalLayout   = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        n = 2;
    } else {
        deps = 1;
    }

    // B5 MSAA: 4x colour + depth, both resolved into the 1-sample mirrors the post pass already reads.
    // Built with the RenderPass2 structs because this header copy has no pNext on VkSubpassDescription.
    if (samples > 1u && depthFmt && resolveColorFmt) {
        VkAttachmentDescription2 ma[4] = {};
        for (int i = 0; i < 4; ++i) ma[i].sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
        ma[0].format = (VkFormat)colorFmt; ma[0].samples = (VkSampleCountFlagBits)samples;
        ma[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ma[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ma[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ma[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ma[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ma[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ma[1].format = (VkFormat)depthFmt; ma[1].samples = (VkSampleCountFlagBits)samples;
        ma[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ma[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ma[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ma[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ma[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ma[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        ma[2].format = (VkFormat)resolveColorFmt; ma[2].samples = VK_SAMPLE_COUNT_1_BIT;
        ma[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ma[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ma[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ma[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ma[2].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ma[2].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ma[3].format = (VkFormat)resolveDepthFmt; ma[3].samples = VK_SAMPLE_COUNT_1_BIT;
        ma[3].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; ma[3].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ma[3].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ma[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ma[3].initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ma[3].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference2 c2 = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
        c2.attachment = 0; c2.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 d2 = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
        d2.attachment = 1; d2.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 r2 = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
        r2.attachment = 2; r2.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        VkAttachmentReference2 rd2 = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2 };
        rd2.attachment = 3; rd2.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescriptionDepthStencilResolve dsr = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE };
        dsr.depthResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        dsr.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        dsr.pDepthStencilResolveAttachment = &rd2;

        VkSubpassDescription2 msub = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2 };
        msub.pNext = &dsr;
        msub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        msub.colorAttachmentCount = 1; msub.pColorAttachments = &c2;
        msub.pResolveAttachments = &r2;
        msub.pDepthStencilAttachment = &d2;

        VkRenderPassCreateInfo2 mrpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2 };
        mrpi.attachmentCount = 4; mrpi.pAttachments = ma;
        mrpi.subpassCount = 1; mrpi.pSubpasses = &msub;
        VkRenderPass mrp = nullptr;
        if (!chk("vkCreateRenderPass2(msaa)", G.vkCreateRenderPass2(dev_(), &mrpi, nullptr, &mrp))) return 0xFFFFFFFFu;
        VKDrawPass mp; mp.sig = sig; mp.samples = samples; mp.rp = mrp;
        X.passes.push_back(mp);
        ++X.st.passes;
        VKWP1_LOG("[VKMSAA] pass %u: color=%u depth=%u samples=%u (resolve)",
                  (unsigned)X.passes.size() - 1u, colorFmt, depthFmt, samples);
        return (unsigned)X.passes.size() - 1u;
    }

    VkSubpassDescription sub = {};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = depthFmt ? &dref : nullptr;

    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpi.attachmentCount = n; rpi.pAttachments = att;
    rpi.subpassCount = 1; rpi.pSubpasses = &sub;
    rpi.dependencyCount = deps; rpi.pDependencies = dep;

    VkRenderPass rp = nullptr;
    if (!chk("vkCreateRenderPass(wedge5)", G.vkCreateRenderPass(dev_(), &rpi, nullptr, &rp))) return 0xFFFFFFFFu;
    VKDrawPass p; p.sig = sig; p.samples = samples; p.rp = rp;
    X.passes.push_back(p);
    ++X.st.passes;
    VKWP1_LOG("wedge5 pass %u: color=%u depth=%u samples=%u",
        (unsigned)X.passes.size() - 1u, colorFmt, depthFmt, samples);
    return (unsigned)X.passes.size() - 1u;
}

VkFramebuffer fb_for(unsigned passIdx, void* colorView, void* depthView, unsigned w, unsigned h)
{
    for (size_t i = 0; i < X.fbs.size(); ++i) {
        const VKDrawFB& e = X.fbs[i];
        if (e.colorView == colorView && e.depthView == depthView && e.w == w && e.h == h && e.pass == passIdx)
            return e.fb;
    }
    VkImageView att[2];
    att[0] = (VkImageView)colorView;
    unsigned n = 1;
    if (depthView) { att[1] = (VkImageView)depthView; n = 2; }
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = X.passes[passIdx].rp;
    fbi.attachmentCount = n; fbi.pAttachments = att;
    fbi.width = w; fbi.height = h; fbi.layers = 1;
    VkFramebuffer fb = nullptr;
    if (!chk("vkCreateFramebuffer(wedge5)", G.vkCreateFramebuffer(dev_(), &fbi, nullptr, &fb))) return nullptr;
    VKDrawFB e; e.colorView = colorView; e.depthView = depthView; e.w = w; e.h = h; e.pass = passIdx; e.fb = fb;
    X.fbs.push_back(e);
    ++X.st.framebuffers;
    return fb;
}

// ---------------------------------------------------------------------------
// pipelines
// ---------------------------------------------------------------------------
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 8: the one place a registry row becomes a
// VkShaderModule. Created lazily, once per (tag, stage); the registry itself never sees a Vulkan handle.
static VkShaderModule pgm_module(unsigned tag, unsigned isPixel)
{
    for (size_t i = 0; i < X.pgmModules.size(); ++i)
        if (X.pgmModules[i].tag == tag && X.pgmModules[i].pixel == isPixel)
            return X.pgmModules[i].mod;

    const VKPgm_Program* p = nullptr;
    const unsigned n = VKPgm_Program_Count();
    for (unsigned i = 0; i < n; ++i) {
        const VKPgm_Program* q = VKPgm_Program_At(i);
        if (q && q->tag == tag && q->is_pixel == isPixel) { p = q; break; }
    }
    if (!p) {
        VKWP1_LOG("wedge5 pipeline refused: no registry row for tag=%u stage=%s", tag, isPixel ? "PS" : "VS");
        return nullptr;
    }
    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smvi.codeSize = (size_t)p->words * 4u;
    smvi.pCode = p->spv;
    VkShaderModule mod = nullptr;
    if (!chk("vkCreateShaderModule(wedge8 pgm)", G.vkCreateShaderModule(dev_(), &smvi, nullptr, &mod)))
        return nullptr;
    X.pgmModules.push_back({ tag, isPixel, mod });
    VKWP1_LOG("wedge8 module created: %s tag=%u (%u SPIR-V words)", p->name, tag, p->words);
    return mod;
}

bool pso_key_equal(const VKDraw_PSO_Key& a, const VKDraw_PSO_Key& b)
{
    return a.shader_bits == b.shader_bits && a.fvf == b.fvf && a.light_mask == b.light_mask &&
           a.vs_tag == b.vs_tag && a.ps_tag == b.ps_tag && a.topology == b.topology &&
           a.rt_format == b.rt_format && a.depth_format == b.depth_format &&
           a.sample_count == b.sample_count && a.cull_mode == b.cull_mode &&
           a.front_ccw == b.front_ccw && a.fill_mode == b.fill_mode &&
           a.depth_test == b.depth_test && a.depth_write == b.depth_write && a.depth_cmp == b.depth_cmp &&
           a.stencil_test == b.stencil_test && a.stencil_func == b.stencil_func &&
           a.stencil_ref == b.stencil_ref && a.stencil_mask == b.stencil_mask &&
           a.stencil_write_mask == b.stencil_write_mask && a.stencil_fail == b.stencil_fail &&
           a.stencil_zfail == b.stencil_zfail && a.stencil_pass == b.stencil_pass &&
           a.blend_enable == b.blend_enable &&
           a.src_blend == b.src_blend && a.dst_blend == b.dst_blend &&
           a.src_blend_alpha == b.src_blend_alpha && a.dst_blend_alpha == b.dst_blend_alpha &&
           a.write_mask == b.write_mask && a.alpha_test_enable == b.alpha_test_enable &&
           a.alpha_ref == b.alpha_ref && a.alpha_cmp == b.alpha_cmp && a.scissor_test == b.scissor_test;
}

VkPipeline pso_for(const VKDraw_Submit* s)
{
    // Wedge 6: a submit that carries the fixed-function block is drawn by vkff.vert; one that does not
    // (the wedge-4/5 probes) keeps the passthrough stage. The flag is part of the cache identity because
    // the two stages share every other element of the key.
    // Wedge 7 adds the second bit: the pixel stage is chosen the same way, out of the same block, and the
    // two bits are kept apart rather than collapsed into a bool, so a pipeline that runs the interpreted
    // combiners over a passthrough vertex stage can never alias onto the one that runs both.
    // Wedge 8: a stage the registry matched replaces the mechanism stage for that half of the pipeline.
    // The tags are in the key, so two programs can never alias onto one pipeline object; the FF bit is
    // cleared only for the half a custom program owns, which is what lets a PS-only family (bloom) keep
    // the fixed-function vertex stage it reads its varyings from.
    const unsigned wantFF = ((X.ffVs && s->ff && !s->key.vs_tag) ? VKDRAW_FF_VERT_BIT : 0u)
                          | ((X.ffPs && s->ff && !s->key.ps_tag) ? VKDRAW_FF_PIX_BIT  : 0u);
    // GeneralsX @performance Qoder 19/10/2026 R-05: bucket the ~30-field key comparison. The hash covers
    // the whole key struct (plain unsigned fields, no padding), the FF stage bits, the attribute count and
    // the attribute bytes; pso_key_equal and the attrs memcmp still decide every hit inside the bucket.
    unsigned long long ph = vkdraw_hash_bytes(&s->key, sizeof(s->key));
    ph ^= vkdraw_hash_bytes(&wantFF, sizeof(wantFF));
    ph ^= vkdraw_hash_bytes(&s->attr_count, sizeof(s->attr_count));
    if (s->attr_count) ph ^= vkdraw_hash_bytes(s->attrs, sizeof(VKDraw_Attr) * s->attr_count);
    // Phase 5B.3: a migrated family names its pipeline by material, so the key is part of identity.
    // Keeping it out of VKDraw_PSO_Key leaves the shared struct and its census layout untouched; the
    // stored entry carries the same number and the linear probe compares it field-for-field.
    const unsigned long long matKey = X.matPipeline;
    ph ^= vkdraw_hash_bytes(&matKey, sizeof(matKey));
    const std::pair<std::unordered_multimap<unsigned long long, unsigned>::iterator,
                     std::unordered_multimap<unsigned long long, unsigned>::iterator> prange =
        X.psoIdx.equal_range(ph);
    for (std::unordered_multimap<unsigned long long, unsigned>::iterator pi = prange.first;
         pi != prange.second; ++pi) {
        const size_t ei = pi->second;
        if (ei >= X.psos.size()) continue;
        const VKDrawPSO& e = X.psos[ei];
        if (e.ff == wantFF && e.mat_key == matKey &&
            e.attr_count == s->attr_count && pso_key_equal(e.key, s->key) &&
            (e.attr_count == 0 ||
             memcmp(e.attrs, s->attrs, sizeof(VKDraw_Attr) * e.attr_count) == 0)) {
            ++X.st.psoReused;
            return e.pso;
        }
    }
    if (s->attr_count > VKDRAW_MAX_ATTRS) {
        VKWP1_LOG("wedge5 pipeline refused: %u attributes over the %u limit", s->attr_count, VKDRAW_MAX_ATTRS);
        ++X.st.psoFailed;
        return nullptr;
    }
    if (X.psos.size() >= 4096) {
        VKWP1_LOG("wedge5 pipeline cache full (%zu) - draw refused", X.psos.size());
        ++X.st.psoFailed;
        return nullptr;
    }

    const VKDraw_PSO_Key& k = s->key;
    // Wedge 8 stage choice: the mechanism/FF default, overridden per half by a registry tag.
    VkShaderModule vsMod = (wantFF & VKDRAW_FF_VERT_BIT) ? X.ffVs : X.vs;
    // GeneralsX @feature 26/10/2026 HDR: one FF pixel stage now serves both modes. vkff.frag reads
    // VKFF_FLAG_LINEAR to skip its legacy clamps; the old vkff_hdr variant had drifted behind the
    // light-shadow work and is no longer selected.
    VkShaderModule psMod = (wantFF & VKDRAW_FF_PIX_BIT) ? X.ffPs : X.ps;
    if (k.vs_tag) {
        vsMod = pgm_module(k.vs_tag, 0u);
        if (!vsMod) { ++X.st.psoFailed; return nullptr; }
    }
    if (k.ps_tag) {
        psMod = pgm_module(k.ps_tag, 1u);
        if (!psMod) { ++X.st.psoFailed; return nullptr; }
    }
    // A pipeline is only valid inside the render pass it was created for, and that pass is the one the
    // recorder has open. Nothing reaches here without open_pass() having set curPass, but the index is
    // what would corrupt memory if that ever stopped being true, so it is checked.
    if (X.curPass >= X.passes.size()) {
        VKWP1_LOG("wedge5 pipeline refused: no render pass open (curPass=%u)", X.curPass);
        ++X.st.psoFailed;
        return nullptr;
    }
    // The stride is the vertex layout's size and only the facade knows it (FVFInfoClass owns that table),
    // so a missing stride is an incomplete submit rather than something to guess at here.
    const unsigned stride = s->streams[0].stride;
    if (!stride || stride > 512) {
        VKWP1_LOG("wedge5 pipeline refused: bad vertex stride %u (fvf=%08X)", stride, k.fvf);
        ++X.st.psoFailed;
        return nullptr;
    }
    if (s->attr_count == 0) {
        VKWP1_LOG("wedge5 pipeline refused: no vertex attributes (fvf=%08X)", k.fvf);
        ++X.st.psoFailed;
        return nullptr;
    }

    // Vertex input. On the fixed-function path the pipeline has to describe every input vkff.vert reads,
    // not only the ones this batch's FVF happens to carry, because a shader input with no buffer behind it
    // is undefined on Vulkan where the D3D9 vertex processor had a documented default for the field. The
    // missing ones therefore point into the stride-zero default stream, one 16-byte group per location.
    VkVertexInputBindingDescription vbd[2] = {
        { 0, stride, VK_VERTEX_INPUT_RATE_VERTEX },
        { VKFF_DUMMY_BINDING, 0u, VK_VERTEX_INPUT_RATE_INSTANCE },
    };
    VkVertexInputAttributeDescription vad[VKDRAW_MAX_ATTRS + VKFF_LOCATIONS];
    unsigned attrN = 0;
    unsigned bindN = 1u;
    if (wantFF & VKDRAW_FF_VERT_BIT) {
        bindN = 2u;
        for (unsigned loc = 0; loc < (unsigned)VKFF_LOCATIONS; ++loc) {
            const VKDraw_Attr* src = nullptr;
            for (unsigned i = 0; i < s->attr_count && i < VKDRAW_MAX_ATTRS; ++i) {
                if (s->attrs[i].location == loc) { src = &s->attrs[i]; break; }
            }
            vad[attrN].location = loc;
            if (src) {
                vad[attrN].binding  = 0;
                vad[attrN].format   = vk_attr_format(src->type, src->components);
                vad[attrN].offset   = src->offset;
            } else {
                vad[attrN].binding  = (uint32_t)VKFF_DUMMY_BINDING;
                vad[attrN].format   = vk_attr_format(0u, VKFF_LOC_COMPONENTS[loc]);
                vad[attrN].offset   = (uint32_t)(VKFF_DUMMY_SLOT_BYTES * loc);
            }
            ++attrN;
        }
        // Anything the facade described outside the fixed-function range (a wedge-8 attribute, say) still
        // gets bound - the stage simply does not read it.
        for (unsigned i = 0; i < s->attr_count && i < VKDRAW_MAX_ATTRS; ++i) {
            if (s->attrs[i].location < (unsigned)VKFF_LOCATIONS) continue;
            vad[attrN].location = s->attrs[i].location;
            vad[attrN].binding  = 0;
            vad[attrN].format   = vk_attr_format(s->attrs[i].type, s->attrs[i].components);
            vad[attrN].offset   = s->attrs[i].offset;
            ++attrN;
        }
    } else {
        for (unsigned i = 0; i < s->attr_count && i < VKDRAW_MAX_ATTRS; ++i) {
            vad[attrN].location = s->attrs[i].location;
            vad[attrN].binding  = 0;
            vad[attrN].format   = vk_attr_format(s->attrs[i].type, s->attrs[i].components);
            vad[attrN].offset   = s->attrs[i].offset;
            ++attrN;
        }
    }
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = s->attr_count ? bindN : 0u;
    vin.pVertexBindingDescriptions = s->attr_count ? vbd : nullptr;
    vin.vertexAttributeDescriptionCount = attrN;
    vin.pVertexAttributeDescriptions = attrN ? vad : nullptr;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = vk_topology(k.topology);

    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = (k.fill_mode == 1) ? VK_POLYGON_MODE_POINT :
                     (k.fill_mode == 2) ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = vk_cull(k.cull_mode, k.front_ccw);
    rs.frontFace = k.front_ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    // Samples come from the render pass this pipeline will be used in, never from the submit, because a
    // mismatch between the two is an invalid-state-object error rather than a preference.
    ms.rasterizationSamples = (VkSampleCountFlagBits)X.passes[X.curPass].samples;

    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable  = k.depth_test  ? VK_TRUE : VK_FALSE;
    dss.depthWriteEnable = k.depth_write ? VK_TRUE : VK_FALSE;
    dss.depthCompareOp   = vk_compare(k.depth_cmp);
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: the full D3D9 stencil block, one face -
    // the engine never writes D3DRS_CCW_STENCIL*, so the back face carries the same state (D3D9 defaults
    // the two-sided states to the front face as well). The player-colour pass depends on the ZFAIL/MISS
    // ops as much as on the compare, which is why this is a state block rather than the old bool.
    dss.stencilTestEnable = k.stencil_test ? VK_TRUE : VK_FALSE;
    VkStencilOpState so = {};
    so.failOp      = vk_stencil_op(k.stencil_fail);
    so.passOp      = vk_stencil_op(k.stencil_pass);
    so.depthFailOp = vk_stencil_op(k.stencil_zfail);
    so.compareOp   = vk_compare(k.stencil_func);
    so.compareMask = k.stencil_mask & 0xFFu;
    so.writeMask   = k.stencil_write_mask & 0xFFu;
    so.reference   = k.stencil_ref & 0xFFu;
    dss.front = so; dss.back = so;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = k.blend_enable ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = vk_blend(k.src_blend);
    cba.dstColorBlendFactor = vk_blend(k.dst_blend);
    // DX8 has no separate-alpha blend states, so both channels read the same pair of factors.
    cba.srcAlphaBlendFactor = vk_blend(k.src_blend_alpha ? k.src_blend_alpha : k.src_blend);
    cba.dstAlphaBlendFactor = vk_blend(k.dst_blend_alpha ? k.dst_blend_alpha : k.dst_blend);
    cba.colorWriteMask = (VkColorComponentFlags)(k.write_mask & 0xFu);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    // Alpha test is not fixed Vulkan pipeline state: wedge 7 folds it into the interpreted pixel shader,
    // which is why it is in the key but unused here.
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipelineShaderStageCreateInfo stg[2] = {};
    stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    // One module per bit, not one for both: wedge 6 and wedge 7 stand up independently, and a run with only
    // the vertex gate has to keep the mechanism fragment it has always had.
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stg[0].module = vsMod; stg[0].pName = "main";
    stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stg[1].module = psMod; stg[1].pName = "main";
    gpi.stageCount = 2; gpi.pStages = stg;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
    gpi.layout = X.layout; gpi.renderPass = X.passes[X.curPass].rp; gpi.subpass = 0;

    VkPipeline pso = nullptr;
    VKTRACE("pso #%llu: create fvf=%08X bits=%llx light=%u topo=%u ff=%u blend=%u cmp=%u alphaTest=%u attrs=%u stride=%u",
        X.st.psoCreated + 1ull, k.fvf, (unsigned long long)k.shader_bits, k.light_mask, k.topology,
        wantFF, k.blend_enable, k.depth_cmp, k.alpha_test_enable, s->attr_count, stride);
    if (!chk("vkCreateGraphicsPipelines(wedge5)",
             G.vkCreateGraphicsPipelines(dev_(), nullptr, 1, &gpi, nullptr, &pso))) {
        ++X.st.psoFailed;
        if (X.st.psoFailed < 8) {
            VKWP1_LOG("pipeline failed: fvf=%08X bits=%llx light=%u topo=%u rt=%u depth=%u blend=%u cmp=%u attrs=%u stride=%u",
                k.fvf, (unsigned long long)k.shader_bits, k.light_mask, k.topology, k.rt_format,
                k.depth_format, k.blend_enable, k.depth_cmp, s->attr_count, stride);
        }
        return nullptr;
    }
    VKDrawPSO e; e.key = k; e.attr_count = s->attr_count; e.pso = pso; e.ff = wantFF; e.mat_key = matKey;
    if (s->attr_count) memcpy(e.attrs, s->attrs, sizeof(VKDraw_Attr) * s->attr_count);
    X.psoIdx.emplace(ph, (unsigned)X.psos.size());
    X.psos.push_back(e);
    ++X.st.psoCreated;
    if ((X.st.psoCreated & 63ull) == 0ull) {
        VKWP1_LOG("wedge5 pipelines: created=%llu reused=%llu failed=%llu passes=%u framebuffers=%u",
            X.st.psoCreated, X.st.psoReused, X.st.psoFailed,
            (unsigned)X.passes.size(), (unsigned)X.fbs.size());
    }
    return pso;
}

// ---------------------------------------------------------------------------
// descriptors: one set per distinct texture combination inside a frame slot
// ---------------------------------------------------------------------------
bool set_for(void* const views[VKDRAW_MAX_TEX_SLOTS], void* const samplers[VKDRAW_MAX_TEX_SLOTS], const VKDraw_Const_Bind& cb, VkDescriptorSet* out)
{
    std::vector<VKDrawSetEntry>& cache = X.sets[X.slot];
    // R-05: bucket first, verify inside. Every field that the comparison below treats as the identity is
    // hashed; the entry check remains the full memcmp, so a collision costs a compare, never a wrong set.
    unsigned long long h = vkdraw_hash_bytes(views, sizeof(void*) * VKDRAW_MAX_TEX_SLOTS);
    h ^= vkdraw_hash_bytes(samplers, sizeof(void*) * VKDRAW_MAX_TEX_SLOTS);
    h ^= vkdraw_hash_bytes(cb.buf, sizeof(void*) * 3);
    h ^= vkdraw_hash_bytes(cb.bytes, sizeof(unsigned long long) * 3);
    // Resolve every view that can change within or between frames before the cache lookup, so a rebuilt
    // map, a newly uploaded sky or the (static) cookie atlas is part of the set's identity.
    void* lsViews[VkLighting::kShadowMaps] = { nullptr, nullptr, nullptr, nullptr };
    for (int m = 0; m < (int)VkLighting::kShadowMaps; ++m) lsViews[m] = VKDraw_LightShadow_View(m);
    void* cookieView = VKDesc_Cookie_View();
    void* skyView = VKDraw_Sky_View();
    if (!skyView) skyView = VKDesc_White_Cube_View();
    // Last-resort fallbacks keep every binding a valid descriptor even if an upload failed (a null
    // descriptor would be undefined behaviour, not just a wrong colour).
    if (!cookieView) cookieView = VKDesc_White_View();
    if (!skyView) skyView = VKDesc_White_View();
    h ^= vkdraw_hash_bytes(lsViews, sizeof(lsViews));
    h ^= vkdraw_hash_bytes(&cookieView, sizeof(cookieView));
    h ^= vkdraw_hash_bytes(&skyView, sizeof(skyView));
    const std::pair<std::unordered_multimap<unsigned long long, unsigned>::iterator,
                     std::unordered_multimap<unsigned long long, unsigned>::iterator> range =
        X.setIdx[X.slot].equal_range(h);
    for (std::unordered_multimap<unsigned long long, unsigned>::iterator si = range.first;
         si != range.second; ++si) {
        const size_t ci = si->second;
        if (ci >= cache.size()) continue;    // defensive: the cache can only shrink by a slot reset,
        const VKDrawSetEntry& e = cache[ci]; // which clears this map with it
        if (memcmp(e.views, views, sizeof(void*) * VKDRAW_MAX_TEX_SLOTS) == 0 &&
            memcmp(e.samplers, samplers, sizeof(void*) * VKDRAW_MAX_TEX_SLOTS) == 0 &&
            memcmp(e.cbuf, cb.buf, sizeof(void*) * 3) == 0 &&
            memcmp(e.cbytes, cb.bytes, sizeof(unsigned long long) * 3) == 0 &&
            memcmp(e.lightMaps, lsViews, sizeof(lsViews)) == 0 &&
            e.cookie == cookieView && e.skyCube == skyView) {
            *out = e.set; ++X.st.setsReused; return true;
        }
    }
    const VkDescriptorSetLayout sl = (VkDescriptorSetLayout)VKDesc_Set_Layout();
    if (!sl || !X.pool[X.slot]) return false;

    VkDescriptorSet set = nullptr;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = X.pool[X.slot]; ai.descriptorSetCount = 1; ai.pSetLayouts = &sl;
    if (G.vkAllocateDescriptorSets(dev_(), &ai, &set) != VK_SUCCESS) { ++X.st.drawsBad; return false; }

    const VkImageView white = (VkImageView)VKDesc_White_View();
    const VkSampler def = (VkSampler)VKDesc_Default_Sampler();
    VkDescriptorImageInfo img[VKDRAW_MAX_TEX_SLOTS];
    for (int i = 0; i < VKDRAW_MAX_TEX_SLOTS; ++i) {
        img[i] = {};
        img[i].sampler = (VkSampler)(samplers[i] ? samplers[i] : (void*)def);
        // An unset slot falls to the built-in white: D3D8 sampled nothing there, and the fixed-function
        // combiners treat that as a passthrough, which is exactly what a 1x1 white texel multiplies to.
        img[i].imageView = (VkImageView)(views[i] ? views[i] : (void*)white);
        img[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorBufferInfo bufs[3] = {};
    for (int i = 0; i < 3; ++i) {
        bufs[i] = { (VkBuffer)cb.buf[i], 0, (VkDeviceSize)cb.bytes[i] };
    }

    VkWriteDescriptorSet w[11] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet = set; w[0].dstBinding = 0;
    // No stageFlags on VkWriteDescriptorSet - the binding's shader stage lives in the pipeline layout,
    // which the wedge-4 set layout already fixed.
    w[0].descriptorCount = VKDRAW_MAX_TEX_SLOTS; w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = img;
    for (int i = 0; i < 3; ++i) {
        w[1 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[1 + i].dstSet = set; w[1 + i].dstBinding = 1 + (uint32_t)i; w[1 + i].descriptorCount = 1;
        w[1 + i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        w[1 + i].pBufferInfo = &bufs[i];
    }
    // GeneralsX @feature Qoder 24/10/2026 P0: binding 4 carries this frame slot's light buffer as a
    // read-only SSBO. The 90,272-byte block is above NVIDIA's 65,536-byte maxUniformBufferRange, so
    // it must not be a UBO. The slot address is fixed for the whole slot, so no dynamic offset is
    // added; ffState keeps binding 3 untouched.
    VkDescriptorBufferInfo bfp = {};
    if (X.fpBuf) {
        bfp = { (VkBuffer)X.fpBuf, (VkDeviceSize)X.slot * VKFP_SLOT_BYTES, (VkDeviceSize)sizeof(VKLightingBuffer) };
        w[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[4].dstSet = set; w[4].dstBinding = 4; w[4].descriptorCount = 1;
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[4].pBufferInfo = &bfp;
    }
    // GeneralsX @feature Qoder 22/10/2026 light shadows, extended P34 E3 to four maps + D2 cookie atlas +
    // C4 sky cube: bindings 5..10 always have a valid descriptor. White means "no light shadow", the
    // cookie atlas falls back to white, and the sky slot falls back to the 1x1 white cube.
    VkDescriptorImageInfo extraImg[6] = {};
    const VkSampler cookieSamp = (VkSampler)(VKDesc_Cookie_Sampler() ? VKDesc_Cookie_Sampler()
                                                                     : VKDesc_Default_Sampler());
    for (int m = 0; m < 4; ++m) {
        extraImg[m].sampler = (VkSampler)VKDesc_Default_Sampler();
        extraImg[m].imageView = (VkImageView)(lsViews[m] ? lsViews[m] : VKDesc_White_View());
        extraImg[m].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    extraImg[4].sampler = cookieSamp;
    extraImg[4].imageView = (VkImageView)(cookieView ? cookieView : VKDesc_White_View());
    extraImg[4].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    extraImg[5].sampler = cookieSamp;      // clamp-to-edge linear is right for the cube as well
    extraImg[5].imageView = (VkImageView)skyView;
    extraImg[5].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (int m = 0; m < 6; ++m) {
        w[5 + m].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[5 + m].dstSet = set;
        w[5 + m].descriptorCount = 1;
        w[5 + m].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[5 + m].pImageInfo = &extraImg[m];
    }
    // Bindings 5..8 are the four light-shadow maps, 9 the cookie atlas, 10 the sky cube: six single writes.
    for (int m = 0; m < 6; ++m) w[5 + m].dstBinding = (uint32_t)(5 + m);
    G.vkUpdateDescriptorSets(dev_(), 11u, w, 0, nullptr);

    VKDrawSetEntry e;
    for (int i = 0; i < VKDRAW_MAX_TEX_SLOTS; ++i) { e.views[i] = views[i]; e.samplers[i] = samplers[i]; }
    for (int i = 0; i < 3; ++i) { e.cbuf[i] = cb.buf[i]; e.cbytes[i] = cb.bytes[i]; }
    for (int m = 0; m < (int)VkLighting::kShadowMaps; ++m) e.lightMaps[m] = lsViews[m];
    e.cookie = cookieView;
    e.skyCube = skyView;
    e.set = set;
    X.setIdx[X.slot].emplace(h, (unsigned)cache.size());
    cache.push_back(e);
    ++X.st.setsBuilt;
    *out = set;
    return true;
}

// ---------------------------------------------------------------------------
// R-05 L10: GPU-side pass breakdown (measurement only)
// ---------------------------------------------------------------------------
// GeneralsX @performance Qoder 19/10/2026 R-05 L10: V11_11 proved the begin-phase cost is one slot
// fence (7.6-7.9 ms on the 4K@240 baseline), i.e. pure GPU backpressure, but CPU probes cannot name
// which pass owns those milliseconds. A per-slot timestamp query pool writes marks at every pass
// boundary inside the same command buffer, a pipeline-statistics pool counts vertex/fragment
// invocations per pass, and the next Begin_Frame of that slot reads both after its fence passed.
// Gated by GENERALSX_VKFPS_PHASES like every other R-05 probe; dumps every 600 frames to
// vkfps_phase12.log. With the gate off no pool is created and no command is recorded.
enum { L10_MARKS = 64, L10_PAIRS = 31, L10_KEYS = 96 };

struct L10Pair { unsigned openIdx; unsigned closeIdx; unsigned pass; unsigned w, h; };

struct L10Slot {
    VkQueryPool ts;
    VkQueryPool ps;
    unsigned marks;      // timestamp marks consumed in the frame being recorded
    unsigned pairs;      // passes opened by the frame whose results are pending
    int lastOpen;        // pair index of the currently open pass, -1 when none
    unsigned endIdx;     // mark index written by End_Frame (only valid when endMarked)
    bool endMarked;      // the closing mark was written by End_Frame
    L10Pair pair[L10_PAIRS];
};

static L10Slot g_l10[(int)VulkanCore::VK_IN_FLIGHT] = {};
static bool l10On = false;
static bool l10PoolsDone = false;
static bool l10HavePs = false;
static double l10Period = 1.0;        // ns per timestamp tick
static unsigned l10Counters = 2;      // pipeline-statistics counters per query

struct L10Key {
    unsigned long long key;
    double sumMs;
    unsigned long long pairs, v, f;
};
static L10Key g_l10Keys[L10_KEYS] = {};
static unsigned g_l10KeyN = 0;
static double g_l10FrameMsSum = 0.0, g_l10GapMsSum = 0.0;
static unsigned long long g_l10FrameN = 0, g_l10Windows = 0;

static inline double l10_ns2ms(double ns) { return ns * 1e-6 * l10Period; }

static void l10_ensure_pools()
{
    if (l10PoolsDone) return;
    if (!dev_()) return;
    l10PoolsDone = true;
    const char* e = getenv("GENERALSX_VKFPS_PHASES");
    l10On = (e != nullptr && atoi(e) != 0);
    if (!l10On) return;
    VkPhysicalDeviceProperties props = {};
    G.vkGetPhysicalDeviceProperties(core_()->Phys(), &props);
    l10Period = (double)props.limits.timestampPeriod;
    const VkQueryPipelineStatisticFlags want =
        VK_QUERY_PIPELINE_STATISTIC_VERTEX_SHADER_INVOCATIONS_BIT |
        VK_QUERY_PIPELINE_STATISTIC_FRAGMENT_SHADER_INVOCATIONS_BIT;
    // This header keeps statistic support in VkPhysicalDeviceFeatures (pipelineStatisticsQuery), not
    // in the limits struct. bit 2 (vertex) and bit 7 (fragment) are both requested, so every query
    // returns two counters in ascending bit order.
    VkPhysicalDeviceFeatures feats = {};
    G.vkGetPhysicalDeviceFeatures(core_()->Phys(), &feats);
    l10HavePs = feats.pipelineStatisticsQuery != VK_FALSE;
    for (unsigned i = 0; i < (unsigned)VulkanCore::VK_IN_FLIGHT; ++i) {
        VkQueryPoolCreateInfo ti = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
        ti.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ti.queryCount = L10_MARKS;
        if (G.vkCreateQueryPool(dev_(), &ti, nullptr, &g_l10[i].ts) != VK_SUCCESS) g_l10[i].ts = VK_NULL_HANDLE;
        if (l10HavePs) {
            VkQueryPoolCreateInfo pi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            pi.queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS;
            pi.queryCount = L10_PAIRS;
            pi.pipelineStatistics = want;
            if (G.vkCreateQueryPool(dev_(), &pi, nullptr, &g_l10[i].ps) != VK_SUCCESS) g_l10[i].ps = VK_NULL_HANDLE;
        }
    }
    fprintf(stderr, "[VKFPH12] pools created ts=%d ps=%d period_ns=%.3f\n",
        g_l10[0].ts ? 1 : 0, l10HavePs ? 1 : 0, l10Period);
    fflush(stderr);
}

static L10Key* l10_key(unsigned long long k)
{
    for (unsigned i = 0; i < g_l10KeyN; ++i) if (g_l10Keys[i].key == k) return &g_l10Keys[i];
    if (g_l10KeyN >= L10_KEYS) return nullptr;
    g_l10Keys[g_l10KeyN].key = k;
    return &g_l10Keys[g_l10KeyN++];
}

// Read the results the fence we just passed guarantees are complete, fold them into the window
// accumulators, and dump every 600 frames.
static void l10_read(unsigned slot)
{
    if (!l10On || slot >= (unsigned)VulkanCore::VK_IN_FLIGHT) return;
    l10_ensure_pools();
    L10Slot& s = g_l10[slot];
    if (!s.ts || s.pairs == 0) return;
    // Only the contiguous written range is available: reset-but-unwritten queries would make
    // vkGetQueryPoolResults return VK_NOT_READY for the whole call.
    const unsigned nMarks = s.marks > L10_MARKS ? L10_MARKS : s.marks;
    unsigned long long t[L10_MARKS] = {};
    const VkResult tr = G.vkGetQueryPoolResults(dev_(), s.ts, 0, nMarks, sizeof(t), t,
        8, VK_QUERY_RESULT_64_BIT);
    unsigned long long psBuf[L10_PAIRS * 2] = {};
    // No WITH_STATUS_BIT: the slot fence just passed, so every query of the previous frame is complete.
    const bool psOk = s.ps && s.pairs > 0 &&
        G.vkGetQueryPoolResults(dev_(), s.ps, 0, s.pairs, sizeof(psBuf), psBuf,
            l10Counters * 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    if (tr == VK_SUCCESS) {
        double sumMs = 0.0;
        for (unsigned k = 0; k < s.pairs; ++k) {
            const L10Pair& p = s.pair[k];
            if (p.closeIdx <= p.openIdx || p.closeIdx >= L10_MARKS) continue;
            const double ms = l10_ns2ms((double)(t[p.closeIdx] - t[p.openIdx]));
            sumMs += ms;
            L10Key* e = l10_key(((unsigned long long)p.pass << 40) | ((unsigned long long)p.w << 20) | p.h);
            if (e) {
                e->sumMs += ms;
                ++e->pairs;
                if (psOk) {
                    e->v += psBuf[k * l10Counters];
                    e->f += psBuf[k * l10Counters + 1];
                }
            }
        }
        if (s.endMarked && s.endIdx < nMarks) {
            const double total = l10_ns2ms((double)(t[s.endIdx] - t[0]));
            g_l10FrameMsSum += total;
            g_l10GapMsSum += total - sumMs;
            ++g_l10FrameN;
        }
    }
    s.pairs = 0;
    if (++g_l10Windows % 600 == 0) {
        FILE* fp = fopen("vkfps_phase12.log", "a");
        if (fp) {
            const double inv = 1.0 / 600.0;
            fprintf(fp, "[V12_12] frames=600 gpu_frame_avg_ms=%.3f gap_avg_ms=%.3f\n",
                g_l10FrameN ? g_l10FrameMsSum * inv : -1.0,
                g_l10FrameN ? g_l10GapMsSum * inv : -1.0);
            for (unsigned i = 0; i < g_l10KeyN; ++i) {
                const L10Key& e = g_l10Keys[i];
                if (!e.pairs) continue;
                fprintf(fp, "[V12_12]   pass=%u %lux%lu pairs/f=%.1f avg_ms=%.3f frags=%.2e verts=%.2e\n",
                    (unsigned)(e.key >> 40), (unsigned long)(e.key >> 20) & 0xFFFFFu,
                    (unsigned long)(e.key & 0xFFFFFu), (double)e.pairs * inv,
                    e.sumMs / (double)e.pairs,
                    e.pairs ? (double)e.f / (double)e.pairs : 0.0,
                    e.pairs ? (double)e.v / (double)e.pairs : 0.0);
            }
            fflush(fp);
            fclose(fp);
        }
        for (unsigned i = 0; i < g_l10KeyN; ++i) {
            g_l10Keys[i].sumMs = 0; g_l10Keys[i].pairs = 0; g_l10Keys[i].v = 0; g_l10Keys[i].f = 0;
        }
        g_l10FrameMsSum = 0; g_l10GapMsSum = 0; g_l10FrameN = 0;
    }
}

// Called with the new frame's command buffer open: reset this slot's pools on the GPU and stamp
// mark 0 at the top of the frame.
static void l10_frame_begin(unsigned slot)
{
    if (!l10On) { l10_ensure_pools(); if (!l10On) return; }
    L10Slot& s = g_l10[slot];
    if (!s.ts) return;
    G.vkCmdResetQueryPool(X.cmd, s.ts, 0, L10_MARKS);
    if (s.ps) G.vkCmdResetQueryPool(X.cmd, s.ps, 0, L10_PAIRS);
    G.vkCmdWriteTimestamp(X.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.ts, 0);
    s.marks = 1;
    s.lastOpen = -1;
    s.endMarked = false;
    s.endIdx = 0xFFFFFFFFu;
}

static void l10_pass_open()
{
    if (!l10On) return;
    L10Slot& s = g_l10[X.slot];
    if (!s.ts || s.pairs >= L10_PAIRS || s.marks + 3 >= L10_MARKS) return;
    if (s.ps) {
        G.vkCmdBeginQuery(X.cmd, s.ps, s.pairs, 0);
    }
    const unsigned oi = s.marks;
    G.vkCmdWriteTimestamp(X.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, s.ts, oi);
    ++s.marks;
    L10Pair& p = s.pair[s.pairs];
    p.openIdx = oi; p.closeIdx = 0xFFFFFFFFu;
    p.pass = X.curPass; p.w = X.w; p.h = X.h;
    s.lastOpen = (int)s.pairs;
    ++s.pairs;
}

static void l10_pass_close_pre()
{
    // The statistics end must still be inside the renderpass, so it runs before vkCmdEndRenderPass.
    if (!l10On) return;
    L10Slot& s = g_l10[X.slot];
    if (s.ps && s.lastOpen >= 0) G.vkCmdEndQuery(X.cmd, s.ps, (unsigned)s.lastOpen);
}

static void l10_pass_close_post()
{
    if (!l10On) return;
    L10Slot& s = g_l10[X.slot];
    if (!s.ts || s.lastOpen < 0 || s.marks >= L10_MARKS) return;
    const unsigned ci = s.marks;
    G.vkCmdWriteTimestamp(X.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.ts, ci);
    ++s.marks;
    s.pair[s.lastOpen].closeIdx = ci;
    s.lastOpen = -1;
}

// End_Frame stamps the closing mark right after the last pass pair so the written range stays
// contiguous (a reset-but-unwritten query makes the whole-availability read fail with NOT_READY).
static void l10_frame_end()
{
    if (!l10On) return;
    L10Slot& s = g_l10[X.slot];
    if (!s.ts || s.marks >= L10_MARKS) return;
    s.endIdx = s.marks;
    G.vkCmdWriteTimestamp(X.cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, s.ts, s.endIdx);
    ++s.marks;
    s.endMarked = true;
}

// ---------------------------------------------------------------------------
// pass boundaries
// ---------------------------------------------------------------------------
void close_pass()
{
    if (!X.inPass) return;
    l10_pass_close_pre();
    G.vkCmdEndRenderPass(X.cmd);
    l10_pass_close_post();
    // The pass moved both attachments implicitly; tell the state machine where they ended up or the next
    // explicit barrier would be recorded against a stale layout.
    VKRT_Set_Layout(X.colorKey, VKRT_LAYOUT_SHADER_READ_ONLY);
    if (X.depthKey) VKRT_Set_Layout(X.depthKey, VKRT_LAYOUT_SHADER_READ_ONLY);
    X.inPass = 0;
    X.curPass = 0xFFFFFFFFu;
}

// Put the current target's images where the pass's initialLayout expects them and open the pass.
bool open_pass()
{
    if (X.inPass) return true;
    if (!X.cmd || !X.colorKey) return false;

    VKRT_Info ci, di;
    if (!VKRT_Query(X.colorKey, &ci) || !ci.view) {
        if (X.openPassWarns < 4) { ++X.openPassWarns; VKWP1_LOG("wedge5 pass refused: no colour mirror for key %p", X.colorKey); }
        return false;
    }
    unsigned depthFmt = 0;
    if (X.depthKey) {
        if (VKRT_Query(X.depthKey, &di) && di.view) {
            depthFmt = di.format;
            // A colour/depth pair of different sizes cannot form a framebuffer, and the facade is the only
            // party that knows which two RTs it paired. Refusing beats an invalid-usage validation error.
            if (di.width != ci.width || di.height != ci.height) {
                if (X.openPassWarns < 4) {
                    ++X.openPassWarns;
                    VKWP1_LOG("wedge5 pass refused: depth %ux%u does not match colour %ux%u",
                        di.width, di.height, ci.width, ci.height);
                }
                return false;
            }
        }
        X.depthKey = depthFmt ? X.depthKey : nullptr;   // no depth mirror behind that key: draw without it
    }

    // The mirrors define the render area; a caller-supplied extent that disagrees is a facade bug, and
    // following the images is what keeps vkCmdBeginRenderPass and the framebuffer consistent.
    if ((ci.width != X.w || ci.height != X.h) && X.openPassWarns < 4) {
        ++X.openPassWarns;
        VKWP1_LOG("wedge5 target size taken from the mirror: %ux%u (asked %ux%u)",
            ci.width, ci.height, X.w, X.h);
    }
    X.w = ci.width; X.h = ci.height;

    // A mirror created by wedge 3.4 starts life UNDEFINED, and a barrier from UNDEFINED discards it,
    // which is precisely right for a target the engine is about to clear.
    // B5: with MSAA on, the pass renders into 4x colour/depth and resolves into the same 1-sample
    // mirrors the post pass has always read, so nothing downstream changes.
    unsigned passIdx = 0xFFFFFFFFu;
    VkFramebuffer fb = nullptr;
    // Only the HDR scene target (R16G16B16A16) gets multisampled. The UI overlay goes through this same
    // open_pass with the display format, and AA-ing the UI is exactly what was rejected before.
    const bool msaaTarget = (ci.format == 100u);   // VK_FORMAT_R16G16B16A16_SFLOAT
    if (X.msaaSamples > 1u && msaaTarget && depthFmt && di.view) {
        void* kc = (void*)((uintptr_t)X.colorKey ^ 0x4D534141u);
        void* kd = (void*)((uintptr_t)X.depthKey ^ 0x4D534141u);
        if (!VKRT_Alloc_Color_Samples(kc, X.w, X.h, ci.format, X.msaaSamples) ||
            !VKRT_Alloc_Depth_Samples(kd, X.w, X.h, depthFmt, X.msaaSamples)) {
            X.msaaSamples = 1u;   // unsupported on this target shape: fall back for good
            VKWP1_LOG("[VKMSAA] multisample targets unavailable, falling back to 1x");
        } else {
            VKRT_Info mc, md;
            if (!VKRT_Query(kc, &mc) || !VKRT_Query(kd, &md) || !mc.view || !md.view) return false;
            VKRT_Cmd_Transition(X.cmd, X.colorKey, VKRT_LAYOUT_SHADER_READ_ONLY);
            VKRT_Cmd_Transition(X.cmd, X.depthKey, VKRT_LAYOUT_SHADER_READ_ONLY);
            passIdx = pass_for(ci.format, depthFmt, X.msaaSamples, ci.format, depthFmt);
            if (passIdx == 0xFFFFFFFFu) { X.msaaSamples = 1u; passIdx = pass_for(ci.format, depthFmt, 1u); }
            else fb = fb_for_msaa(passIdx, mc.view, md.view, ci.view, di.view, X.w, X.h);
        }
    }
    if (!fb) {
        VKRT_Cmd_Transition(X.cmd, X.colorKey, VKRT_LAYOUT_SHADER_READ_ONLY);
        if (depthFmt) VKRT_Cmd_Transition(X.cmd, X.depthKey, VKRT_LAYOUT_DEPTH_STENCIL_ATTACHMENT);
        passIdx = pass_for(ci.format, depthFmt, 1u);
        if (passIdx == 0xFFFFFFFFu) return false;
        fb = fb_for(passIdx, ci.view, depthFmt ? di.view : nullptr, X.w, X.h);
    }
    if (!fb) return false;

    X.curPass = passIdx;
    VkClearValue clr[2] = {};
    clr[0].color.float32[0] = 0.0f; clr[0].color.float32[1] = 0.0f;
    clr[0].color.float32[2] = 0.0f; clr[0].color.float32[3] = 1.0f;
    clr[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = X.passes[passIdx].rp;
    rbi.framebuffer = fb;
    rbi.renderArea = { { 0, 0 }, { X.w, X.h } };
    rbi.clearValueCount = depthFmt ? 2u : 1u;
    rbi.pClearValues = clr;      // unused for LOAD ops, present because the struct requires a pointer
    G.vkCmdBeginRenderPass(X.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    X.inPass = 1;
    l10_pass_open();
    return true;
}

bool switch_target(void* colorKey, void* depthKey, unsigned w, unsigned h)
{
    if (X.colorKey == colorKey && X.depthKey == depthKey && X.w == w && X.h == h) return open_pass();
    close_pass();
    X.colorKey = colorKey; X.depthKey = depthKey; X.w = w; X.h = h;
    return open_pass();
}

} // anon

// ===========================================================================
// public: wedge 5.1 geometry backings (declared in VulkanGeo.h)
// ===========================================================================

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A forensics: a lock that does not get a
// backing leaves the buffer writing only to D3D9, and every draw of it falls back forever - a static
// buffer is written once, so the miss never heals and never shows up near its cause. Three reasons,
// and the fix differs for each, so each gets its own bounded line.
enum VKGeoLockFail {
    GEOLF_NOT_READY = 1,   // VKDraw_Ready() false: locked before the draw layer existed
    GEOLF_EMPTY = 2,       // zero-size lock: nothing to back
    GEOLF_ALLOC = 3,       // VkBuffer/host-memory allocation failed
};

void geo_lock_fail(void* key, unsigned int bytes, int dynamic, int why)
{
    static unsigned warns = 0;
    static unsigned byReason[4] = {};
    ++byReason[why];
    if (warns < 24u) {
        ++warns;
        VKWP1_LOG("wedge5 backing refused (%d): key=%p bytes=%u dynamic=%d totals=%d/%d/%d",
            why, key, bytes, dynamic, (int)byReason[1], (int)byReason[2], (int)byReason[3]);
    }
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A forensics: a backing exists between a lock
// that creates it and a drop that removes it, so the two ends of that window are the only events that can
// explain a live buffer with no backing - either it was never created, or it was dropped while still being
// drawn. Each end gets its own bounded line, keyed and sized so a same-address sibling is still readable.
// GeneralsX @performance Qoder 19/10/2026 R-05: the born line also names the VkBuffer handle, so an
// unknown-handle refusal can be reconciled against the create/drop history byte for byte.
void geo_life(void* key, unsigned long long cap, int static_buffer, int born, void* buf)
{
    static unsigned created = 0, dropped = 0;
    const unsigned n = born ? created : dropped;
    if (n >= 1000u) return;
    if (born) ++created; else ++dropped;
    VKWP1_LOG("wedge5 backing %s: key=%p cap=%llu static=%u buf=%p counts=%u/%u",
        born ? "created" : "dropped", key, cap, (unsigned)static_buffer, buf, created, dropped);
}

void* VKGeo_Back_Lock(void* key, unsigned int bytes, unsigned int stride, int dynamic, int discard)
{
    if (!VKDraw_Ready()) { geo_lock_fail(key, bytes, dynamic, GEOLF_NOT_READY); return nullptr; }
    if (!key || !bytes) { if (key) geo_lock_fail(key, bytes, dynamic, GEOLF_EMPTY); return nullptr; }
    vk_queue_lock();

    const unsigned long long slotBytes = (bytes + 255ull) & ~255ull;
    const unsigned long long frame = core_()->FrameIndex();

    VKGeoBack* b = geo_find(key);
    if (b && b->dynamic && b->lockFrame != frame) {
        // A new frame starts a new write window in this backing; its slot region is free again because the
        // fence for the frame that last used that slot has already been waited.
        b->lockFrame = frame;
        b->curGen = 0;
        b->lockedInFrame = 0;
        b->traceDiscards = 0;
    }

    // GeneralsX @bugfix Qoder 20/10/2026 R-08: DISCARD generations. A discard inside a frame that has
    // already written this backing must land in fresh storage, exactly like the D3D9 buffer rename; the
    // draw already recorded keeps reading the bytes it captured. Each generation is one more pool-sized
    // region inside the frame slot, and the backing is widened only when a frame really discards twice.
    unsigned wantGens = (b && b->dynamic) ? b->gens : 1u;
    if (b && b->dynamic && discard && b->lockedInFrame) {
        const unsigned need = b->curGen + 2u;
        if (need > wantGens) wantGens = need;
    }
    const unsigned long long cap = dynamic
        ? slotBytes * (unsigned long long)wantGens * (unsigned long long)VKGEO_SLOT_COUNT
        : slotBytes;

    if (b && b->cap < cap) {            // the object grew: rebuild wider, the old data is being rewritten
        VKWP1_LOG("wedge5 backing rebuilt: key=%p cap=%llu->%llu gens=%u oldbuf=%p",
            key, b->cap, cap, wantGens, (void*)b->buf);
        geo_tombstone(b, key);
        b = nullptr;
    }
    if (!b) {
        // Recycle a tombstoned slot if there is one; only a brand-new slot may extend the deque (push_back
        // is the one operation that keeps every earlier element address valid for the maps).
        if (!X.geoFree.empty()) { b = X.geoFree.back(); X.geoFree.pop_back(); }
        else { X.geo.push_back(VKGeoBack()); b = &X.geo[X.geo.size() - 1]; }
        *b = VKGeoBack();
        b->key = key;
        const VkBufferUsageFlags usage = (stride ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                                 : VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
                                       | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!make_host_buffer(cap, usage, &b->buf, &b->mem, &b->map)) {
            b->freed = 1;
            X.geoFree.push_back(b);
            geo_lock_fail(key, bytes, dynamic, GEOLF_ALLOC);
            vk_queue_unlock();
            return nullptr;
        }
        b->cap = cap; b->slotBytes = slotBytes; b->dynamic = dynamic ? 1 : 0;
        b->gens = dynamic ? wantGens : 1u;
        b->lockFrame = frame;
        X.geoByKey[key] = b;
        X.geoByBuf[b->buf] = b;
        geo_life(key, cap, dynamic ? 0 : 1, 1, b->buf);
    }

    // A static backing always writes region 0; a dynamic one rotates over frame slots, and inside a slot
    // over the generations a discard has consumed so far.
    unsigned slot = 0, genIdx = 0;
    if (b->dynamic) {
        slot = core_()->FrameSlot();
        if (slot != b->slot) {
            // GeneralsX @bugfix Qoder 28/10/2026: a new frame starts at generation 0 again. Without this
            // reset curGen only ever climbed and then stayed clamped at gens-1, so every discard after the
            // first few frames rewrote the SAME region: the second lock of a frame silently overwrote the
            // bytes an already-recorded draw still pointed at. That is the one-frame "texture stretched
            // across a quad" flicker on trees/bushes, and it gets worse the more objects a frame queues.
            ++X.st.ringRotations;
            b->curGen = 0;
            b->lockedInFrame = 0;
            b->slot = slot;
        }
        if (b->lockedInFrame && discard) {
            if (b->curGen + 1u < b->gens) ++b->curGen;
            else {
                // Out of generations inside one frame: the previous draw's data is about to be reused.
                static unsigned long long s_overflow = 0;
                if (s_overflow < 8ull) {
                    ++s_overflow;
                    fprintf(stderr, "[VKGEN] in-frame generation overflow key=%p gens=%u slot=%u bytes=%llu\n",
                            key, b->gens, slot, (unsigned long long)bytes);
                    fflush(stderr);
                }
            }
        }
        b->lockedInFrame = 1;
        genIdx = b->curGen;
    }
    // GeneralsX @debug Qoder 20/10/2026 R-08: one bounded line per discard, so a captured run can be
    // matched one-to-one against a stretched mesh.
    {
        static int trace = -1;
        if (trace < 0) {
            const char* e = getenv("GENERALSX_VK_VBTRACE");
            trace = (e && atoi(e)) ? 1 : 0;
        }
        if (trace && b->dynamic && discard) {
            ++b->traceDiscards;
            static unsigned long long logged = 0;
            if (logged < 400) {
                ++logged;
                fprintf(stderr, "[VKDISCARD] frame=%llu key=%p cap=%llu slotBytes=%llu gen=%u gens=%u byteoff=%llu n=%u\n",
                    frame, key, b->cap, b->slotBytes, genIdx, b->gens,
                    (unsigned long long)bytes, b->traceDiscards);
                fflush(stderr);
            }
        }
    }
    b->stride = stride;
    b->curOffset = ((unsigned long long)slot * b->gens + genIdx) * b->slotBytes;
    b->curBytes = slotBytes;

    // GeneralsX @debug 28/10/2026 static re-lock forensics: a static backing is pinned to region 0, so
    // the second time it is filled the bytes a frame still in flight is reading change underneath it.
    // The engine side of that tear is always the same shape - a buffer created USAGE_DEFAULT that is
    // re-uploaded later (roads on every camera move, scorch decals when a crater is added) - so the fix
    // belongs at the owning buffer: USAGE_DYNAMIC plus D3DLOCK_DISCARD gives the backing its per-frame
    // regions. This line names any buffer still doing it in place; bounded to the first two re-locks of
    // each backing so a long session cannot flood the log.
    if (!b->dynamic) {
        if (b->locks > 0u && b->locks < 3u) {
            fprintf(stderr, "[VKGEOINPLACE] frame=%llu key=%p cap=%llu stride=%u lock=%u discard=%d\n",
                    frame, key, b->cap, stride, b->locks + 1u, discard);
            fflush(stderr);
        }
        ++b->locks;
    }

    // R-05: a lock IS a write window - the caller is about to fill these bytes. Bump the generation so any
    // cached per-range fact about this backing (idxMax) is refused until it is re-read.
    b->gen = ++X.geoGenSeq;
    vk_queue_unlock();
    return (char*)b->map + b->curOffset;
}

bool VKGeo_Back_Bind(void* key, void** vk_buffer, unsigned long long* offset, unsigned int* stride,
                     unsigned int* bytes)
{
    if (!key) return false;
    VKGeoBack* found = geo_find(key);
    if (found) {
        const VKGeoBack& b = *found;
        if (vk_buffer) *vk_buffer = (void*)b.buf;
        if (offset) *offset = b.curOffset;
        if (stride) *stride = b.stride;
        if (bytes) *bytes = (unsigned int)b.curBytes;
        return b.buf != nullptr;
    }
    return false;
}

void VKGeo_Back_Drop(void* key)
{
    if (!key) return;
    vk_queue_lock();
    VKGeoBack* b = geo_find(key);
    if (b) {
        geo_life(key, b->cap, b->dynamic ? 0 : 1, 0, b->buf);
        geo_tombstone(b, key);
        ++X.st.buffersDropped;
    }
    vk_queue_unlock();
}

void VKGeo_Back_Reset()
{
    // GeneralsX @note Qoder 18/10/2026 pure-Vulkan wedge 5.A: wired to nothing yet - grep says the only
    // references are this definition and its declaration. It discards every static backing, which is the
    // exact coverage hole wedge 5.A is chasing, so a future resize or device-loss path that reaches it has
    // to re-lock every buffer it wants drawn; until that wiring exists the log line makes it undeniable.
    vk_queue_lock();
    VKWP1_LOG("wedge5 backing reset: discarding %zu backings", X.geo.size());
    for (size_t i = 0; i < X.geo.size(); ++i) geo_retire(X.geo[i]);
    X.geo.clear();
    X.geoFree.clear();
    X.geoByKey.clear();
    X.geoByBuf.clear();
    // Everything the scan stage cached belonged to a backing that no longer exists.
    X.idxMax.clear();
    vk_queue_unlock();
}

// One-shot table print for the wedge-5.A coverage hunt: the draw site only knows a key is absent, this
// says what IS present (and whether the searched-for object ever got a backing at all). Bounded, because
// a menu session already holds a couple of hundred entries.
void VKGeo_Back_Dump(void* wanted)
{
    vk_queue_lock();
    VKWP1_LOG("wedge5 backing table: %zu entries, wanted=%p", X.geo.size(), wanted);
    unsigned shown = 0;
    for (size_t i = 0; i < X.geo.size() && shown < 400u; ++i, ++shown) {
        const VKGeoBack& b = X.geo[i];
        VKWP1_LOG("  [%c] key=%p cap=%llu slot=%llu stride=%u dyn=%u buf=%p",
            b.key == wanted ? '*' : ' ', b.key, b.cap, b.slotBytes, b.stride, b.dynamic, (void*)b.buf);
    }
    vk_queue_unlock();
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A forensics: birth and death of the WW3D buffer
// objects the table above is keyed by. Reading one line off the bottom: an object born here with no
// "backing created" line for the same key was never locked, while a "backing dropped" line before the end
// of the run means the draw site is still binding an address whose backing is gone.
void VKGeo_Trace_Buffer(void* key, unsigned int fvf, unsigned int count, int born)
{
    static unsigned borns = 0, deaths = 0;
    const unsigned n = born ? borns : deaths;
    if (n >= 2000u) return;
    if (born) ++borns; else ++deaths;
    VKWP1_LOG("wedge5 ww3d buffer %s: key=%p fvf=%08X count=%u counts=%u/%u",
        born ? "born" : "died", key, fvf, count, borns, deaths);
}

// ===========================================================================
// public: lifecycle
// ===========================================================================
bool VKDraw_Ready()
{
    if (!core_() || !core_()->IsReady() || !vkdraw_env_on()) return false;
    return X.built && !X.broken;
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7: is the interpreted fixed-function pixel stage in
// force? The facade asks this before it counts a combiner construct the table cannot say, so a run with the
// gate off does not report gaps in a path it never took. It follows the vertex gate for the reason the
// module below gives: vkff.frag reads the varyings vkff.vert writes.
bool VKDraw_FF_Pixel_On()
{
    static bool checked = false;
    static bool on = false;
    if (!checked) {
        checked = true;
        on = vk_env_gate("GENERALSX_VKFFPIX") && vk_env_gate("GENERALSX_VKFFVERT");
    }
    return on;
}

static bool vkdraw_build_layout_and_shaders()
{
    VkDevice dev = dev_();
    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smvi.codeSize = sizeof(VKDRAW_VERT_SPV); smvi.pCode = VKDRAW_VERT_SPV;
    if (!chk("vkCreateShaderModule(wedge5 vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.vs))) return false;
    smvi.codeSize = sizeof(VKDRAW_FRAG_SPV); smvi.pCode = VKDRAW_FRAG_SPV;
    if (!chk("vkCreateShaderModule(wedge5 fs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.ps))) return false;

    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the fixed-function vertex stage and the
    // default vertex it completes a partial FVF with. Both are optional: with the gate off every pipeline
    // keeps the wedge-5 passthrough stage, which is what makes the gate reversible at runtime.
    if (vk_env_gate("GENERALSX_VKFFVERT")) {
        smvi.codeSize = sizeof(VKFF_VERT_SPV); smvi.pCode = VKFF_VERT_SPV;
        if (!chk("vkCreateShaderModule(wedge6 ff vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.ffVs))) return false;
        void* dmap = nullptr;
        if (!make_host_buffer((unsigned long long)VKFF_DUMMY_SLOT_BYTES * VKFF_LOCATIONS,
                              VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &X.dummyVb, &X.dummyVbMem, &dmap)) return false;
        float* f = (float*)dmap;
        for (unsigned loc = 0; loc < (unsigned)VKFF_LOCATIONS; ++loc)
            for (unsigned c = 0; c < 4u; ++c) f[loc * 4u + c] = VKFF_LOC_DEFAULT[loc][c];
        // Written once for the life of the device, so the mapping does not have to stay around.
        G.vkUnmapMemory(dev, X.dummyVbMem);
        VKWP1_LOG("wedge6 fixed-function vertex stage: vs=%p default stream=%p (%u locations x %u bytes)",
            (void*)X.ffVs, (void*)X.dummyVb, (unsigned)VKFF_LOCATIONS, (unsigned)VKFF_DUMMY_SLOT_BYTES);
    }

    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7: the interpreted fixed-function pixel stage -
    // the combiner interpreter that replays the D3DTOP table, the alpha test folded in from the state
    // block's third flag word, and the fog choice. It is only made when the vertex gate is also on,
    // because the two stages are one contract: vkff.frag reads vColor/vTex0..3/vFog/vSpecular, and a
    // wedge-5 passthrough vertex stage writes none of them. Asking for VKFFPIX alone is therefore a
    // no-op rather than a half-built pipeline, which is what makes the ladder safe to walk one gate at a
    // time. The sampler it reads per texture slot is no longer baked into the stage: wedge 7's submit
    // filler hands the descriptor the D3DSAMP tuple the game set, so this module samples with the same
    // filters the D3D9 device used.
    if (VKDraw_FF_Pixel_On() && X.ffVs) {
        smvi.codeSize = sizeof(VKFF_FRAG_SPV); smvi.pCode = VKFF_FRAG_SPV;
        if (!chk("vkCreateShaderModule(wedge7 ff fs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.ffPs))) return false;
        smvi.codeSize = sizeof(VKFF__HDR_FRAG_SPV); smvi.pCode = VKFF__HDR_FRAG_SPV;
        if (!chk("vkCreateShaderModule(p6 ff hdr fs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.ffPsHdr))) return false;
        VKWP1_LOG("wedge7 fixed-function pixel stage: fs=%p (%u SPIR-V words)",
            (void*)X.ffPs, (unsigned)(sizeof(VKFF_FRAG_SPV) / 4u));
    }

    const VkDescriptorSetLayout sl = (VkDescriptorSetLayout)VKDesc_Set_Layout();
    if (!sl) { VKWP1_LOG("wedge5 needs the wedge-4 set layout (GENERALSX_VKDESC=1)"); return false; }
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &sl;
    if (!chk("vkCreatePipelineLayout(wedge5)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &X.layout))) return false;

    // One pool per in-flight slot. maxSets is sized for a frame's worth of distinct texture combinations,
    // which the census bounds far below this; running out is a logged refusal, never a corruption.
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) {
        if (X.pool[i]) continue;
        VkDescriptorPoolSize ps[2] = {
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048u * (uint32_t)VKDRAW_MAX_TEX_SLOTS },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2048u * 3u },   // vertex / pixel / lights
        };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pci.maxSets = 2048; pci.poolSizeCount = 2; pci.pPoolSizes = ps;
        if (!chk("vkCreateDescriptorPool(wedge5)", G.vkCreateDescriptorPool(dev, &pci, nullptr, &X.pool[i])))
            return false;
    }
    return true;
}

bool VKDraw_Create()
{
    if (X.built) return true;
    if (!core_() || !core_()->IsReady() || !vkdraw_env_on()) return false;
    // Wedge 5 renders into wedge 3.4 render-target mirrors using the wedge 4 descriptor set layout, so
    // both gates are prerequisites rather than options. Naming them here beats a wall of per-draw refusals.
    if (!VKRT_Ready() || !VKDesc_Ready()) {
        X.broken = true;
        VKWP1_LOG("wedge5 refused: needs GENERALSX_VKRT=1 (targets) and GENERALSX_VKDESC=1 (set layout), got rt=%d desc=%d",
            VKRT_Ready() ? 1 : 0, VKDesc_Ready() ? 1 : 0);
        return false;
    }
    vk_queue_lock();
    X.broken = false;   // an explicit create attempt overrides a previous failure
    // GeneralsX @bugfix Qoder 19/10/2026 R-02 forensics A/B: the FF ring is the 4 MiB one - about 546
    // payloads per in-flight slot - while the CH02 menu plateau churns a similar count of DISTINCT FF
    // blocks in one frame, so const_ring_write's wrap fires mid-frame and the frame's earliest batches
    // (buildings, terrain) replay the blocks written by its latest ones: white silhouette, position drift
    // and black ground together, which no combiner arithmetic on the recorded state can produce. The env
    // resizes only the FF ring, so one binary runs both arms of the A/B on the same save.
    unsigned long long ffRingMb = 4ull;
    if (const char* e = getenv("GENERALSX_VKCRINGFFMB")) {
        const unsigned long v = strtoul(e, nullptr, 10);
        if (v >= 4ull && v <= 256ull) ffRingMb = v;
    }
    // The scratch ring for DrawPrimitiveUP is bound as a vertex buffer AND as the synthesised sequential
    // index buffer for the same draw, so it needs both usage bits from creation.
    bool ok = vkdraw_build_layout_and_shaders() &&
              make_host_buffer(X.zeroVtxBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &X.zeroVtx, &X.zeroVtxMem, nullptr) &&
              make_host_buffer(X.zeroPixBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &X.zeroPix, &X.zeroPixMem, nullptr) &&
              make_host_buffer(X.zeroLightBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &X.zeroLight, &X.zeroLightMem, nullptr) &&
              (X.constAlign = core_()->UniformAlign(), X.constAlign != 0) &&
              const_ring_create(X.cr[0], 16ull << 20, 1ull) &&
              const_ring_create(X.cr[1], 16ull << 20, 1ull) &&
              const_ring_create(X.cr[2], ffRingMb << 20, 4ull) &&
              make_host_buffer((unsigned long long)VKFP_SLOT_BYTES * VKGEO_SLOT_COUNT,
                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &X.fpBuf, &X.fpMem, &X.fpMap) &&
              make_host_buffer((unsigned long long)VKPOST_FOG_SLOT_BYTES * VKGEO_SLOT_COUNT,
                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &X.fogBuf, &X.fogMem, &X.fogMap) &&
              make_host_buffer(VKDrawState::SCRATCH_CAP, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &X.fanBuf, &X.fanMem, &X.fanMap) &&
              make_host_buffer(VKDrawState::SCRATCH_CAP,
                  (VkBufferUsageFlags)(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT),
                  &X.upBuf, &X.upMem, &X.upMap);
    vk_queue_unlock();
    // GeneralsX @feature Qoder 24/10/2026 P0: the Forward+ block is a storage buffer, not a UBO. Log
    // the exact size and the two device limits the old 90 KB UBO violated, and fail closed to the
    // per-object eight-light path if a driver cannot take it.
    {
        VkPhysicalDeviceProperties props = {};
        G.vkGetPhysicalDeviceProperties(core_()->Phys(), &props);
        const bool storageOK = props.limits.maxStorageBufferRange >= (unsigned long long)sizeof(VKLightingBuffer)
                            && props.limits.maxPerStageDescriptorStorageBuffers >= 1u;
        s_fpUnsupported = storageOK ? 0 : 1;
        fprintf(stderr, "[VKLIGHT] layout ok size=%zu lights=%u clusters=%u maxStorage=%llu maxUniform=%llu maxStageStorage=%u fp=%d budget=%d\n",
            sizeof(VKLightingBuffer),
            (unsigned)VkLighting::kMaxLights,
            (unsigned)VkLighting::kMaxClusters,
            (unsigned long long)props.limits.maxStorageBufferRange,
            (unsigned long long)props.limits.maxUniformBufferRange,
            (unsigned)props.limits.maxPerStageDescriptorStorageBuffers,
            s_fpUnsupported ? 0 : 1,
            s_fpBudget);
        fflush(stderr);
    }
    if (!ok) {
        X.broken = true;
        VKWP1_LOG("wedge5 build failed - draw path stays off");
        VKDraw_Destroy();   // release whatever the partial build managed to create
        return false;
    }

    VkExtent2D e = core_()->Extent();
    X.w = e.width; X.h = e.height;
    X.colorKey = X.sceneColorKey; X.depthKey = X.sceneDepthKey;
    X.built = true;
    VKWP1_LOG("wedge5 layer ok: scene target %ux%u, slots=%d, scratch=2x%llu MiB, ffRing=%lluMB window + 4x spill x %d slots",
        X.w, X.h, VKGEO_SLOT_COUNT, VKDrawState::SCRATCH_CAP >> 20, ffRingMb, VKGEO_SLOT_COUNT);
    vktrace_watchdog_start();
    return true;
}

// Frees the wedge-11 probe staging; the probe state lives further down this file, so the shutdown hook
// reaches it through this one-line bridge.
void vkrt_probe_free_staging();

void VKDraw_Destroy()
{
    VkDevice dev = dev_();
    vk_queue_lock();
    // Drain before freeing: pipelines, framebuffers and the geometry backings may all still be referenced
    // by a frame in flight. Shutdown happens once per process, so the wait costs nothing.
    if (dev) chk("wedge5 destroyWait", G.vkDeviceWaitIdle(dev));
    vksky_destroy();
    vkrt_probe_free_staging();   // defined with the wedge-11 probe below
    X.frameOpen = 0; X.cmd = nullptr; X.inPass = 0; X.haveImage = 0;
    // 6B: the graph forgets its target records on the next frame (W3DDisplay resets when not ready); the
    // adopted water key must not survive a destroyed VKRT table either.
    X.waterReflectionKey = nullptr;
    X.waterReflectionW = X.waterReflectionH = X.waterReflectionFormat = 0;
    if (dev) {
        for (size_t i = 0; i < X.psos.size(); ++i) G.vkDestroyPipeline(dev, X.psos[i].pso, nullptr);
        for (size_t i = 0; i < X.fbs.size(); ++i) G.vkDestroyFramebuffer(dev, X.fbs[i].fb, nullptr);
        for (size_t i = 0; i < X.passes.size(); ++i) G.vkDestroyRenderPass(dev, X.passes[i].rp, nullptr);
        for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) if (X.pool[i]) G.vkDestroyDescriptorPool(dev, X.pool[i], nullptr);
        if (X.layout) G.vkDestroyPipelineLayout(dev, X.layout, nullptr);
        if (X.vs) G.vkDestroyShaderModule(dev, X.vs, nullptr);
        if (X.ps) G.vkDestroyShaderModule(dev, X.ps, nullptr);
        if (X.ffVs) G.vkDestroyShaderModule(dev, X.ffVs, nullptr);
        if (X.ffPs) G.vkDestroyShaderModule(dev, X.ffPs, nullptr);
        if (X.ffPsHdr) G.vkDestroyShaderModule(dev, X.ffPsHdr, nullptr);
        for (size_t i = 0; i < X.pgmModules.size(); ++i)
            G.vkDestroyShaderModule(dev, X.pgmModules[i].mod, nullptr);
        // Native sun shadow: the shadow map images belong to the VKRT mirror layer, everything created
        // here is owned by this layer and goes away with it.
        for (size_t i = 0; i < X.shCastPSOs.size(); ++i) G.vkDestroyPipeline(dev, X.shCastPSOs[i].pso, nullptr);
        if (X.shRecvPSO) G.vkDestroyPipeline(dev, X.shRecvPSO, nullptr);
        if (X.shCastVs) G.vkDestroyShaderModule(dev, X.shCastVs, nullptr);
        if (X.shCastPs) G.vkDestroyShaderModule(dev, X.shCastPs, nullptr);
        if (X.shCastAlphaPs) G.vkDestroyShaderModule(dev, X.shCastAlphaPs, nullptr);
        if (X.shRecvVs) G.vkDestroyShaderModule(dev, X.shRecvVs, nullptr);
        if (X.shRecvPs) G.vkDestroyShaderModule(dev, X.shRecvPs, nullptr);
        if (X.shPool) G.vkDestroyDescriptorPool(dev, X.shPool, nullptr);
        for (int i = 0; i < VKGEO_SLOT_COUNT; ++i)
            if (X.shCastPool[i]) G.vkDestroyDescriptorPool(dev, X.shCastPool[i], nullptr);
        if (X.shCastLayout) G.vkDestroyPipelineLayout(dev, X.shCastLayout, nullptr);
        if (X.shCastAlphaLayout) G.vkDestroyPipelineLayout(dev, X.shCastAlphaLayout, nullptr);
        if (X.shRecvLayout) G.vkDestroyPipelineLayout(dev, X.shRecvLayout, nullptr);
        if (X.shSetLayout) G.vkDestroyDescriptorSetLayout(dev, X.shSetLayout, nullptr);
        if (X.shCastSetLayout) G.vkDestroyDescriptorSetLayout(dev, X.shCastSetLayout, nullptr);
        if (X.shSampler) G.vkDestroySampler(dev, X.shSampler, nullptr);
        if (X.shCloudSampler) G.vkDestroySampler(dev, X.shCloudSampler, nullptr);
        // GeneralsX @feature Qoder 21/10/2026 vegetation instancing resources.
        if (X.veg.gpipe) G.vkDestroyPipeline(dev, X.veg.gpipe, nullptr);
        if (X.veg.rpipe) G.vkDestroyPipeline(dev, X.veg.rpipe, nullptr);
        if (X.veg.layout) G.vkDestroyPipelineLayout(dev, X.veg.layout, nullptr);
        if (X.veg.gvs) G.vkDestroyShaderModule(dev, X.veg.gvs, nullptr);
        if (X.veg.gfs) G.vkDestroyShaderModule(dev, X.veg.gfs, nullptr);
        if (X.veg.rvs) G.vkDestroyShaderModule(dev, X.veg.rvs, nullptr);
        if (X.veg.rfs) G.vkDestroyShaderModule(dev, X.veg.rfs, nullptr);
        for (int i = 0; i < VKGEO_SLOT_COUNT; ++i)
            if (X.veg.buf[i]) free_host_buffer(X.veg.buf[i], X.veg.mem[i], &X.veg.map[i]);
        X.veg = VKDrawState::VKVegState();
    }
    X.shCastPSOs.clear();
    X.shRecvPSO = nullptr; X.shRecvPass = 0xFFFFFFFFu;
    X.shCastVs = X.shCastPs = X.shCastAlphaPs = X.shRecvVs = X.shRecvPs = nullptr;
    X.shCastLayout = X.shCastAlphaLayout = X.shRecvLayout = nullptr;
    X.shSetLayout = nullptr; X.shCastSetLayout = nullptr;
    X.shPool = nullptr; X.shSampler = nullptr; X.shCloudSampler = nullptr;
    X.shCloudView = nullptr;
    X.shCloudParams[0] = X.shCloudParams[1] = 0.0f;
    X.shCloudParams[2] = 1.0f / 315.0f; X.shCloudParams[3] = 0.0f;
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) {
        X.shSets[i] = nullptr;
        X.shSetViews[i][0] = X.shSetViews[i][1] = X.shSetViews[i][2] = nullptr;
        X.shCastPool[i] = nullptr;
        X.shCastSets[i].clear();
    }
    X.shReady = false; X.shHaveVP = 0; X.casters.clear();
    X.pgmModules.clear();
    X.psos.clear(); X.psoIdx.clear(); X.fbs.clear(); X.passes.clear();
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) { X.pool[i] = nullptr; X.sets[i].clear(); X.setIdx[i].clear(); }
    X.layout = nullptr; X.vs = nullptr; X.ps = nullptr; X.ffVs = nullptr; X.ffPs = nullptr; X.ffPsHdr = nullptr;
    free_host_buffer(X.dummyVb, X.dummyVbMem, nullptr);
    X.dummyVb = nullptr; X.dummyVbMem = nullptr;
    for (size_t i = 0; i < X.geo.size(); ++i) geo_destroy(X.geo[i]);
    X.geo.clear();
    X.geoFree.clear();
    X.geoByKey.clear();
    X.geoByBuf.clear();
    X.idxMax.clear();
    geo_reap_all();          // the device idle above makes every retired backing safe to destroy now
    vk_queue_unlock();
    free_host_buffer(X.zeroVtx, X.zeroVtxMem, nullptr);
    free_host_buffer(X.zeroPix, X.zeroPixMem, nullptr);
    free_host_buffer(X.zeroLight, X.zeroLightMem, nullptr);
    free_host_buffer(X.fpBuf, X.fpMem, &X.fpMap);
    free_host_buffer(X.fogBuf, X.fogMem, &X.fogMap);
    for (int i = 0; i < 3; ++i) const_ring_destroy(X.cr[i]);
    free_host_buffer(X.fanBuf, X.fanMem, &X.fanMap);
    free_host_buffer(X.upBuf, X.upMem, &X.upMap);
    X.zeroVtx = nullptr; X.zeroPix = nullptr; X.zeroLight = nullptr;
    X.fpBuf = nullptr; X.fpMem = nullptr; X.fpMap = nullptr;
    X.fogBuf = nullptr; X.fogMem = nullptr; X.fogMap = nullptr;
    X.fpActive = 0; X.fpBuiltFrame = ~0ull;
    X.fanBuf = nullptr; X.upBuf = nullptr;
    X.built = false; X.broken = false; X.inPass = 0; X.frameOpen = 0; X.cmd = nullptr;
}

// ===========================================================================
// public: targets, clears
// ===========================================================================
void VKDraw_Set_Targets(void* color_key, void* depth_key, unsigned width, unsigned height)
{
    if (!VKDraw_Ready() || !color_key) return;
    // GeneralsX @debug 27/10/2026 HDR water: one line per session proves whether the water reflection
    // target is ever published to the recorder. Without it the sea cannot be reflecting the offscreen
    // render, and a "reflection too bright" hunt would be chasing the wrong image.
    if (X.waterReflectionKey && color_key == X.waterReflectionKey) {
        static int s_waterRt = 0;
        if (s_waterRt < 3) {
            ++s_waterRt;
            fprintf(stderr, "[VKRT] water reflection target published %ux%u depth=%p\n", width, height, depth_key);
            fflush(stderr);
        }
    }
    vk_queue_lock();
    if (!width) width = X.w;
    if (!height) height = X.h;
    if ((width != X.w || height != X.h) && X.inPass) close_pass();
    // A false return is not fatal and must not revert the target: the keys stay switched, the pass is
    // closed, and the first draw that arrives after the mirror exists opens it. Refusing the batch is
    // what VK_Draw_Submit already does when open_pass still fails.
    if (!switch_target(color_key, depth_key, width, height) && X.openPassWarns < 4) {
        ++X.openPassWarns;
        VKWP1_LOG("wedge5 set-target pending: key %p depth %p %ux%u (no pass open yet)",
            color_key, depth_key, width, height);
    }
    vk_queue_unlock();
}

void VKDraw_Get_Targets(void** color_key, void** depth_key, unsigned* width, unsigned* height)
{
    if (color_key) *color_key = X.colorKey;
    if (depth_key) *depth_key = X.depthKey;
    if (width) *width = X.w;
    if (height) *height = X.h;
}

void VKDraw_Scene_Targets(void** color_key, void** depth_key, unsigned* width, unsigned* height)
{
    if (color_key) *color_key = X.sceneColorKey;
    if (depth_key) *depth_key = X.sceneDepthKey;
    if (width) *width = X.w;
    if (height) *height = X.h;
}

void VKDraw_Set_Scene_Extent(unsigned width, unsigned height)
{
    // GeneralsX @bugfix 23/10/2026: a resolution change is a new projection and a rebuilt scene mirror.
    // The shadow fit's ground estimate and hysteresis side describe the old projection; keeping them
    // reads as "shadows are late" after a fullscreen switch. Reset only on an actual change so the
    // per-frame call from Begin_Scene stays free.
    if (width && height && (width != X.sceneW || height != X.sceneH)) {
        X.sceneW = width; X.sceneH = height;
        X.displayW = width; X.displayH = height;
        X.forceClearOnBegin = 1;
        VKDraw_Shadow_Reset();
    } else {
        X.sceneW = width; X.sceneH = height;
        if (width && height) { X.displayW = width; X.displayH = height; }
    }
}

// GeneralsX @feature 23/10/2026 P6: overlay key/decision helpers. The overlay is the LDR image the UI
// draws into after the 3D resolve.
//
// P0 24/10/2026: the overlay is no longer optional. Resolving 3D into a display-sized LDR image before
// the UI is what guarantees that AO/grade/bloom/FXAA/tonemap can never touch the HUD, and it removes
// the old "UI drawn into a render-scaled scene" path. GENERALSX_VK_PARITY=1 opts out for a retail-blit
// A/B leg, and the draw-layer setters force scale/AA/HDR off in that mode.
static bool vk_parity_on()
{
    static int s = -1;
    if (s < 0) {
        const char* e = getenv("GENERALSX_VK_PARITY");
        s = (e != nullptr && *e != 0 && atoi(e) != 0) ? 1 : 0;
    }
    return s != 0;
}

void* vkdraw_overlay_key()
{
    return X.overlayColorKey;
}

bool vkdraw_overlay_wanted()
{
    return !vk_parity_on();
}

void VKDraw_Set_Present_Rect(unsigned x, unsigned y, unsigned width, unsigned height)
{
    X.rectX = x; X.rectY = y; X.rectW = width; X.rectH = height;
}

// The clear body with the queue lock already held, so the frame-open path can apply a parked request
// without re-entering a lock it already owns.
static void vkdraw_clear_locked(int clear_color, int clear_z_stencil, const float color[4], float z,
                                unsigned stencil, const unsigned* rect)
{
    if (!open_pass()) return;

    VkClearAttachment att[2] = {};
    VkClearRect area = {};
    // D3D's Clear with no rect list clears the current viewport, not the surface, so a sub-rect here is
    // what keeps a shadow pass from wiping the tiles of a shared render target it does not own.
    if (rect && rect[2] && rect[3]) {
        area.rect = { { (int32_t)rect[0], (int32_t)rect[1] }, { rect[2], rect[3] } };
    } else {
        area.rect = { { 0, 0 }, { X.w, X.h } };
    }
    area.baseArrayLayer = 0; area.layerCount = 1;
    unsigned n = 0;
    if (clear_color) {
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].colorAttachment = 0;
        if (color) {
            att[n].clearValue.color.float32[0] = color[0]; att[n].clearValue.color.float32[1] = color[1];
            att[n].clearValue.color.float32[2] = color[2]; att[n].clearValue.color.float32[3] = color[3];
        }
        ++n;
    }
    if (clear_z_stencil) {
        // The engine clears D3DCLEAR_STENCIL unconditionally, but this port's depth attachment is picked
        // by vkdraw_scene_depth_format() and the first choice, D32_SFLOAT, has no stencil aspect. Naming
        // one on such an attachment is an invalid clear, so the aspect follows the format that is actually
        // attached.
        unsigned depthFmt = 0;
        if (X.depthKey) {
            VKRT_Info di;
            if (VKRT_Query(X.depthKey, &di) && di.view) depthFmt = di.format;
        }
        att[n].aspectMask = depthFmt && vk_format_has_stencil(depthFmt)
            ? (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
            : VK_IMAGE_ASPECT_DEPTH_BIT;
        att[n].clearValue.depthStencil.depth = z;
        att[n].clearValue.depthStencil.stencil = (uint32_t)stencil;
        ++n;
    }
    if (n) {
        G.vkCmdClearAttachments(X.cmd, n, att, 1, &area);
        ++X.st.clears;
    }
}

void VKDraw_Clear(int clear_color, int clear_z_stencil, const float color[4], float z, unsigned stencil,
                  const unsigned* rect)
{
    if (!VKDraw_Ready()) return;
    if (!X.frameOpen) {
        // Parked, not dropped: WW3D::Begin_Render clears before Begin_Scene opens this frame's command
        // buffer, and D3D9 says that clear belongs to the frame about to start. VKDraw_Begin_Frame applies
        // whatever is parked here once the scene target is standing.
        //
        // GeneralsX @bugfix 23/10/2026: merge instead of overwrite. The engine can issue a colour clear
        // and a depth/stencil clear as two calls; the old single-slot park kept only the last one, which
        // left the depth image undefined (the "battlefield black, GUI lit" signature). Keep whichever
        // aspects have been requested and update only those values.
        if (!X.pendingClear) {
            X.pendingClear = 1;
            X.pendingClearColor = clear_color ? 1 : 0;
            X.pendingClearZStencil = clear_z_stencil ? 1 : 0;
            X.pendingZ = z; X.pendingStencil = stencil;
            if (color) {
                for (int i = 0; i < 4; ++i) X.pendingColor[i] = color[i];
            }
        } else {
            if (clear_color) {
                X.pendingClearColor = 1;
                if (color) {
                    for (int i = 0; i < 4; ++i) X.pendingColor[i] = color[i];
                }
            }
            if (clear_z_stencil) {
                X.pendingClearZStencil = 1;
                X.pendingZ = z; X.pendingStencil = stencil;
            }
        }
        ++X.st.clearsDeferred;
        return;
    }
    vk_queue_lock();
    vkdraw_clear_locked(clear_color, clear_z_stencil, color, z, stencil, rect);
    vk_queue_unlock();
}

// ===========================================================================
// wedge 11 forensics: render-target mirror probe
// ===========================================================================
// The sun shadow map's receiver samples the map mirror and reads 0 at every terrain point, which splits
// into two different failures - the far fill never reached the mirror, or the sample path reads the wrong
// bytes - and only the mirror's own pixels can decide between them. One recorded copy per interval brings
// the whole RGBA16F map to host staging, and the scan runs at the next frame's Begin, which is the wait
// that proves the copy finished (same slot-fence contract as the wedge-7 luma sampler). No-op unless
// GENERALSX_VKRTPROBE=1; cadence GENERALSX_VKRTPROBE_EVERY frames (default 30).
namespace {

struct VKRTProbe {
    bool    envChecked;
    bool    on;
    bool    built;
    bool    pending;                      // a copy was recorded and has not been scanned yet
    unsigned long long offered;           // probe calls seen while on
    unsigned long long interval;
    unsigned long long cap;               // staging capacity in bytes
    unsigned  fmt;                        // VkFormat of the pending sample
    unsigned  w, h;                       // mirror extent of the pending sample
    unsigned  recSlot;                    // frame slot the pending copy was recorded into
    unsigned long long offerFrame;        // recorder frame label when the sample was offered (R-06 pairing)
    unsigned  stuck;                      // record attempts refused while a sample was unread
    VkBuffer  buf;
    VkDeviceMemory mem;
    unsigned char* map;
};
VKRTProbe RP = { false, false, false, false, 0ull, 30ull, 0ull, 0u, 0u, 0u, 0u, 0ull, 0u,
                 nullptr, nullptr, nullptr };

bool vkrt_probe_on()
{
    if (!RP.envChecked) {
        RP.envChecked = true;
        char env[8] = {};
        DWORD n = GetEnvironmentVariableA("GENERALSX_VKRTPROBE", env, (DWORD)sizeof(env));
        RP.on = (n != 0 && env[0] != '0');
        if (RP.on) {
            char every[16] = { 0 };
            if (GetEnvironmentVariableA("GENERALSX_VKRTPROBE_EVERY", every, (DWORD)sizeof(every)) > 0) {
                const long v = atol(every);
                if (v >= 1) RP.interval = (unsigned long long)v;
            }
            fprintf(stderr, "[VKRTPROBE] sampling on, every %llu frames\n", RP.interval);
            fflush(stderr);
        }
    }
    return RP.on;
}

bool vkrt_probe_build(unsigned long long bytes)
{
    if (RP.built && RP.cap >= bytes) return true;
    VkDevice dev = dev_();
    if (!dev) return false;
    if (RP.built) { free_host_buffer(RP.buf, RP.mem, (void**)&RP.map); RP.buf = nullptr; RP.mem = nullptr; RP.built = false; }
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!chk("VKRTPROBE CreateBuffer", G.vkCreateBuffer(dev, &bi, nullptr, &RP.buf))) return false;
    VkMemoryRequirements req = {};
    G.vkGetBufferMemoryRequirements(dev, RP.buf, &req);
    const uint32_t mt = memtype_(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
    bool ok = mt != 0xFFFFFFFFu &&
              chk("VKRTPROBE AllocMemory", G.vkAllocateMemory(dev, &mai, nullptr, &RP.mem)) &&
              chk("VKRTPROBE BindBuffer", G.vkBindBufferMemory(dev, RP.buf, RP.mem, 0)) &&
              chk("VKRTPROBE Map", G.vkMapMemory(dev, RP.mem, 0, bytes, 0, (void**)&RP.map));
    if (!ok) {
        free_host_buffer(RP.buf, RP.mem, (void**)&RP.map);
        RP.buf = nullptr; RP.mem = nullptr;
        return false;
    }
    RP.built = true; RP.cap = bytes;
    return true;
}

// IEEE half -> float. Subnormal halves land in the float normal range (their exponent field is zero and
// the leading-bit search below restores the bias), which is exact for the values this probe reads.
float half_to_f_(unsigned short h)
{
    const unsigned sign = (unsigned)(h >> 15) << 31;
    const unsigned em = h & 0x7FFFu;
    unsigned bits;
    if (em == 0u) bits = sign;
    else if (em >= 0x7C00u) bits = sign | 0x7F800000u | ((em & 0x3FFu) << 23);
    else if (em < 0x0400u) {
        unsigned v = em, e = 0u;
        while ((v & 0x0400u) == 0u) { v <<= 1; ++e; }
        bits = sign | ((112u - e) << 23) | ((v & 0x3FFu) << 13);
    } else {
        bits = sign | (((em >> 10) + 112u) << 23) | ((em & 0x3FFu) << 13);
    }
    float f; memcpy(&f, &bits, 4); return f;
}

// Runs from VKDraw_Begin_Frame right after the slot's fence wait, so the copy below is done.
void vkrt_probe_poll(unsigned slot)
{
    if (!RP.pending || RP.recSlot != slot) return;
    RP.pending = false; RP.stuck = 0u;
    const unsigned long long px = (unsigned long long)RP.w * RP.h;
    if (RP.fmt != (unsigned)VK_FORMAT_R16G16B16A16_SFLOAT || !RP.map || !px) {
        fprintf(stderr, "[VKRTPROBE] sample fmt=%u %ux%u: scan covers RGBA16F only, first shorts=%04X %04X %04X %04X\n",
            RP.fmt, RP.w, RP.h,
            RP.map ? *(const unsigned short*)RP.map : 0u, 0u, 0u, 0u);
        fflush(stderr);
        return;
    }
    const unsigned short* t = (const unsigned short*)RP.map;
    double sum = 0.0; float mn = 1e30f, mx = -1e30f;
    unsigned long long nonzero = 0ull, aboveHalf = 0ull;
    for (unsigned long long i = 0; i < px; ++i) {
        const float r = half_to_f_(t[i * 4]);
        sum += (double)r;
        if (r < mn) mn = r;
        if (r > mx) mx = r;
        if (r != 0.0f) ++nonzero;
        if (r > 0.5f) ++aboveHalf;
    }
    const unsigned long long mid = (px / 2ull) * 4ull;
    fprintf(stderr,
        "[VKRTPROBE] f=%llu map %ux%u R: min=%.4f max=%.4f mean=%.4f nonzero=%.2f%% aboveHalf=%.2f%%"
        " | t0=(%.3f,%.3f,%.3f,%.3f) tmid=(%.3f,%.3f,%.3f,%.3f)\n",
        RP.offerFrame, RP.w, RP.h, (double)mn, (double)mx, sum / (double)px,
        100.0 * (double)nonzero / (double)px, 100.0 * (double)aboveHalf / (double)px,
        (double)half_to_f_(t[0]), (double)half_to_f_(t[1]), (double)half_to_f_(t[2]), (double)half_to_f_(t[3]),
        (double)half_to_f_(t[mid]), (double)half_to_f_(t[mid + 1]), (double)half_to_f_(t[mid + 2]), (double)half_to_f_(t[mid + 3]));
    fflush(stderr);
}

} // anon

void vkrt_probe_free_staging()
{
    if (!RP.built) return;
    free_host_buffer(RP.buf, RP.mem, (void**)&RP.map);
    RP.buf = nullptr; RP.mem = nullptr; RP.built = false; RP.cap = 0ull; RP.pending = false;
}

// Sample the mirror behind key at the next frame boundary. Called by the game right after an off-scene
// pass restored the recorder to the scene target, so the map pass is closed and its image sits in
// SHADER_READ_ONLY: the copy closes the open scene pass, transitions the image to TRANSFER_SRC, copies,
// transitions it back and reopens the scene pass, all inside this frame's command buffer.
void VKDraw_RTProbe(void* key)
{
    if (!VKDraw_Ready() || !key) return;
    if (!vkrt_probe_on()) return;
    vk_queue_lock();
    if (!X.frameOpen || !X.cmd) { vk_queue_unlock(); return; }
    if (RP.pending) {
        if (++RP.stuck > 8u) { RP.pending = false; RP.stuck = 0u; }   // unread sample expired, same rule as luma
        vk_queue_unlock();
        return;
    }
    ++RP.offered;
    if ((RP.offered % RP.interval) != 0ull) { vk_queue_unlock(); return; }
    VKRT_Info ri;
    if (!VKRT_Query(key, &ri) || !ri.image || ri.is_depth) { vk_queue_unlock(); return; }
    if (!vkrt_probe_build((unsigned long long)ri.width * ri.height * 8ull)) { vk_queue_unlock(); return; }
    close_pass();
    VKRT_Cmd_Transition(X.cmd, key, VKRT_LAYOUT_TRANSFER_SRC);
    VkBufferImageCopy cc = {};
    cc.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    cc.imageOffset = { 0, 0, 0 };
    cc.imageExtent = { ri.width, ri.height, 1u };
    G.vkCmdCopyImageToBuffer(X.cmd, (VkImage)ri.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, RP.buf, 1, &cc);
    VKRT_Cmd_Transition(X.cmd, key, VKRT_LAYOUT_SHADER_READ_ONLY);
    open_pass();
    RP.pending = true; RP.recSlot = X.slot; RP.fmt = ri.format; RP.w = ri.width; RP.h = ri.height;
    RP.offerFrame = g_vkStepFrame.load();
    vk_queue_unlock();
}

// ===========================================================================
// GeneralsX @feature Qoder 20/10/2026 native sun shadow (ported from ModernGeneralsZH)
//
// Two steps, both inside the frame's command buffer, both between the 3D views and the UI:
//  1. replay every opaque batch captured during the scene into a sun-space depth map (R32F colour read
//     back by the receiver, D32 depth for the caster-to-caster occlusion);
//  2. draw one full-screen triangle over the scene colour that reconstructs each pixel's world position
//     from the scene depth and multiplies the scene by a PCSS visibility factor.
// The engine-side entry point is VKDraw_Shadow_Apply, called from W3DDisplay after drawViews.
// ===========================================================================
namespace {

bool shadow_env_on()
{
    if (!X.shEnvChecked) {
        X.shEnvChecked = true;
        // GENERALSX_VKSHADOW=0 is the QA force-off; the shipped switch is the options-page
        // "ShadowMap" row, which DX8Wrapper::VK_Set_Real_Shadow_Enabled() keeps in sync.
        const char* e = getenv("GENERALSX_VKSHADOW");
        X.shOn = (e == nullptr) || (atoi(e) != 0);
        const char* a = getenv("GENERALSX_VKSHADOW_ALT");
        X.shAlt = (a && atoi(a) > 0) ? (unsigned)atoi(a) : 0u;
    }
    if (!X.shOn) return false;
    // GeneralsX @debug Qoder 20/10/2026 three-way A/B: the mode is latched at the frame boundary.
    if (X.abOn) return X.abMode != 0u;
    // GeneralsX @debug Qoder 20/10/2026 A/B capture: alternate the pass itself. Every shadow state is
    // rebuilt from scratch each frame (casters are cleared in VKDraw_Shadow_Apply), so a skipped frame
    // leaves nothing behind.
    if (X.shAlt) {
        static unsigned long long s_seg = ~0ull;
        const unsigned long long seg = X.st.framesBegun / X.shAlt;
        const bool on = (seg & 1ull) == 0ull;
        if (seg != s_seg) {
            s_seg = seg;
            fprintf(stderr, "[VKSHADOW] alt segment=%llu frame=%llu on=%d\n",
                seg, X.st.framesBegun, on ? 1 : 0);
            fflush(stderr);
        }
        return on;
    }
    // Options-page "ShadowMap" row: on runs the native pass, off leaves the retail managers alone.
    return DX8Wrapper::VK_Real_Shadow_Enabled();
}

// GeneralsX @feature Qoder 20/10/2026: both receiver knobs are owned by the graphics page now. The QA env
// variables stay authoritative for scripted A/B runs, so they are resolved once and win over any menu push.
float shadow_strength()
{
    static bool init = false;
    static bool hasEnv = false;
    static float envValue = 0.58f;
    if (!init) {
        init = true;
        if (const char* e = getenv("GENERALSX_VKSHADOW_STRENGTH")) {
            const float v = (float)atof(e);
            if (v >= 0.0f && v <= 1.0f) { envValue = v; hasEnv = true; }
        }
    }
    // GeneralsX @tweak 27/10/2026 P1-2: 0.7 -> 0.8. With the light rig decoded once at the boundary and
    // every family reading it, the HDR day leg against the SDR baseline measured the shadowed building
    // wall at +81% and the crate at +29% when the linear scene kept 0.7; at 0.8 they land at +44% / +9.6%
    // and the wall (0.0918) sits inside the 0.09-0.12 band this calibration targets. 1.0 overshoots the
    // other way (-35% / -27%), so 0.8 is the single global value closest to the retail shadow read. The
    // player's own strength still scales it, and GENERALSX_VKSHADOW_STRENGTH still overrides for an A/B.
    if (hasEnv) return envValue;
    const float authored = DX8Wrapper::VK_Sun_Shadow_Strength();
    return DX8Wrapper::VK_HDR_Enabled() ? (authored * 0.8f) : authored;
}

float shadow_penumbra()
{
    static bool init = false;
    static bool hasEnv = false;
    static float envValue = 1.0f;
    if (!init) {
        init = true;
        if (const char* e = getenv("GENERALSX_VKSHADOW_SOFT")) {
            const float v = (float)atof(e);
            if (v >= 0.25f && v <= 4.0f) { envValue = v; hasEnv = true; }
        }
    }
    return hasEnv ? envValue : DX8Wrapper::VK_Sun_Shadow_Penumbra();
}

// Diagnostics: 1 = scene depth, 2 = pass stage classification, 3 = shadow-map sample, 4 = rejection-reason
// tint, 5 = receiver light-space depth, 6 = blocker count over a huge radius, 7 = RGB triple of lc.z,
// scene depth and blocker count, 8 = same triple with the map's depth at the receiver's own texel in R,
// 9 = the shadow map stretched over the screen, 10 = receiver light-space uv coverage,
// 11 = own texel depth vs the y-flipped row vs lc.z.
// The receiver reads a negative strength as "debug mode n" and replaces the colour instead of scaling it.
int shadow_debug_mode()
{
    static int mode = -1;
    if (mode < 0) {
        const char* e = getenv("GENERALSX_VKSHADOW_DEBUG");
        mode = e ? atoi(e) : 0;
        if (mode < 0 || mode > 13) mode = 0;
    }
    return mode;
}

// 4x4 multiply in the convention the shaders read: arrays are column-major, out = a * b.
void shadow_mat_mul(float out[16], const float a[16], const float b[16])
{
    float t[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[k * 4 + r] * b[c * 4 + k];
            t[c * 4 + r] = s;
        }
    }
    memcpy(out, t, sizeof(t));
}

bool shadow_mat_inverse(float out[16], const float m[16])
{
    const float a00 = m[0],  a01 = m[1],  a02 = m[2],  a03 = m[3];
    const float a10 = m[4],  a11 = m[5],  a12 = m[6],  a13 = m[7];
    const float a20 = m[8],  a21 = m[9],  a22 = m[10], a23 = m[11];
    const float a30 = m[12], a31 = m[13], a32 = m[14], a33 = m[15];
    const float b00 = a00 * a11 - a01 * a10;
    const float b01 = a00 * a12 - a02 * a10;
    const float b02 = a00 * a13 - a03 * a10;
    const float b03 = a01 * a12 - a02 * a11;
    const float b04 = a01 * a13 - a03 * a11;
    const float b05 = a02 * a13 - a03 * a12;
    const float b06 = a20 * a31 - a21 * a30;
    const float b07 = a20 * a32 - a22 * a30;
    const float b08 = a20 * a33 - a23 * a30;
    const float b09 = a21 * a32 - a22 * a31;
    const float b10 = a21 * a33 - a23 * a31;
    const float b11 = a22 * a33 - a23 * a32;
    const float det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
    if (fabsf(det) < 1e-20f) return false;
    const float id = 1.0f / det;
    out[0]  = ( a11 * b11 - a12 * b10 + a13 * b09) * id;
    out[1]  = (-a01 * b11 + a02 * b10 - a03 * b09) * id;
    out[2]  = ( a31 * b05 - a32 * b04 + a33 * b03) * id;
    out[3]  = (-a21 * b05 + a22 * b04 - a23 * b03) * id;
    out[4]  = (-a10 * b11 + a12 * b08 - a13 * b07) * id;
    out[5]  = ( a00 * b11 - a02 * b08 + a03 * b07) * id;
    out[6]  = (-a30 * b05 + a32 * b02 - a33 * b01) * id;
    out[7]  = ( a20 * b05 - a22 * b02 + a23 * b01) * id;
    out[8]  = ( a10 * b10 - a11 * b08 + a13 * b06) * id;
    out[9]  = (-a00 * b10 + a01 * b08 - a03 * b06) * id;
    out[10] = ( a30 * b04 - a31 * b02 + a33 * b00) * id;
    out[11] = (-a20 * b04 + a21 * b02 - a23 * b00) * id;
    out[12] = (-a10 * b09 + a11 * b07 - a12 * b06) * id;
    out[13] = ( a00 * b09 - a01 * b07 + a02 * b06) * id;
    out[14] = (-a30 * b03 + a31 * b01 - a32 * b00) * id;
    out[15] = ( a20 * b03 - a21 * b01 + a22 * b00) * id;
    return true;
}

// Clip-space corner back to world space: inverse(proj*view) applied to (nx, ny, nz, 1).
bool shadow_unproject(const float invVP[16], float nx, float ny, float nz, float out[3])
{
    const float x = invVP[0] * nx + invVP[4] * ny + invVP[8]  * nz + invVP[12];
    const float y = invVP[1] * nx + invVP[5] * ny + invVP[9]  * nz + invVP[13];
    const float z = invVP[2] * nx + invVP[6] * ny + invVP[10] * nz + invVP[14];
    const float w = invVP[3] * nx + invVP[7] * ny + invVP[11] * nz + invVP[15];
    if (fabsf(w) < 1e-8f) return false;
    out[0] = x / w; out[1] = y / w; out[2] = z / w;
    return true;
}

// Fits an orthographic sun projection around what this frame can actually shade. The receiver rebuilds
// world points with inverse(proj*view), so the fit uses that same matrix: every screen corner is
// unprojected onto two ground-parallel slabs, and the recorded casters' world origins are folded in.
// Recovering an eye position from the view matrix was the first approach, but the engine's stored bytes
// do not follow the camera convention it assumed (the reconstructed eye sat below the map and the box
// covered empty ground). Deriving the footprint from the matrix the receiver itself uses removes that
// guess. The rest is the reference's recipe: a caster margin along the light axis, a 256-unit size
// quantization and a texel-grid snap in x/y so the map does not shimmer as the camera moves.
bool shadow_build_light(const float sun[3], const float view[16], const float proj[16], float groundZ,
                        float outVP[16], float& outRange, float& outMapWorld,
                        float outLo[3], float outHi[3])
{
    float s[3] = { sun[0], sun[1], sun[2] };
    if (s[2] < 0.15f) s[2] = 0.15f;
    const float sl = sqrtf(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (sl < 1e-6f) return false;
    s[0] /= sl; s[1] /= sl; s[2] /= sl;
    const float d[3] = { -s[0], -s[1], -s[2] };   // the way the light travels

    float r[3], u[3];
    r[0] = d[1] * 1.0f - d[2] * 0.0f; r[1] = d[2] * 0.0f - d[0] * 1.0f; r[2] = d[0] * 0.0f - d[1] * 0.0f;
    const float rl = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) return false;
    r[0] /= rl; r[1] /= rl; r[2] /= rl;
    u[0] = r[1] * d[2] - r[2] * d[1];
    u[1] = r[2] * d[0] - r[0] * d[2];
    u[2] = r[0] * d[1] - r[1] * d[0];

    float pv[16], invVP[16];
    shadow_mat_mul(pv, proj, view);
    if (!shadow_mat_inverse(invVP, pv)) return false;

    float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
    auto addPoint = [&](const float p[3]) {
        const float a[3] = { p[0] * r[0] + p[1] * r[1] + p[2] * r[2],
                             p[0] * u[0] + p[1] * u[1] + p[2] * u[2],
                             p[0] * d[0] + p[1] * d[1] + p[2] * d[2] };
        for (int i = 0; i < 3; ++i) {
            if (a[i] < lo[i]) lo[i] = a[i];
            if (a[i] > hi[i]) hi[i] = a[i];
        }
    };

    // Each corner pixel maps to a world ray between its near and far clip points; wherever that ray
    // crosses a ground-parallel slab is part of the visible footprint. Terrain is not recorded as a
    // caster (identity-world batches are rejected by shadow_record_caster), so groundZ is only a
    // caster-average estimate. Two slabs plus the old 60-unit far padding therefore clipped valleys
    // and cliff bases: their receivers failed the lc.z test in vkshadow_recv.frag and came out fully
    // lit, leaving the shadow on the casters only. Sample a ladder of heights across a full terrain
    // range instead, and fall back to the whole visible frustum when a ray crosses none of them.
    static const float kHeightOffsets[6] = { -600.0f, -300.0f, -60.0f, 60.0f, 240.0f, 600.0f };
    float cornerNear[4][3] = {};
    float cornerFar[4][3] = {};
    bool  cornerValid[4] = { false, false, false, false };
    int footprintHits = 0;
    static int s_cornerProbe = 8;
    static int s_matrixProbe = 2;
    const bool realView = fabsf(view[0]) < 0.99f;   // loading frames hand us an identity view
    if (s_matrixProbe > 0 && realView) {
        --s_matrixProbe;
        VKWP1_LOG("[VKSHADOW] view: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f",
            (double)view[0], (double)view[1], (double)view[2], (double)view[3],
            (double)view[4], (double)view[5], (double)view[6], (double)view[7],
            (double)view[8], (double)view[9], (double)view[10], (double)view[11],
            (double)view[12], (double)view[13], (double)view[14], (double)view[15]);
        VKWP1_LOG("[VKSHADOW] proj: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f",
            (double)proj[0], (double)proj[1], (double)proj[2], (double)proj[3],
            (double)proj[4], (double)proj[5], (double)proj[6], (double)proj[7],
            (double)proj[8], (double)proj[9], (double)proj[10], (double)proj[11],
            (double)proj[12], (double)proj[13], (double)proj[14], (double)proj[15]);
    }
    for (int k = 0; k < 4; ++k) {
        const float nx = (k & 1) ? 1.0f : -1.0f;
        const float ny = (k & 2) ? 1.0f : -1.0f;
        float w0[3], w1[3];
        if (!shadow_unproject(invVP, nx, ny, -1.0f, w0) || !shadow_unproject(invVP, nx, ny, 1.0f, w1))
            continue;
        memcpy(cornerNear[k], w0, sizeof(cornerNear[k]));
        memcpy(cornerFar[k], w1, sizeof(cornerFar[k]));
        cornerValid[k] = true;
        if (s_cornerProbe > 0 && realView) {
            --s_cornerProbe;
            VKWP1_LOG("[VKSHADOW] corner[%d] (%+.0f,%+.0f) w0=(%.0f %.0f %.0f) w1=(%.0f %.0f %.0f) ground=%.0f",
                k, (double)nx, (double)ny,
                (double)w0[0], (double)w0[1], (double)w0[2],
                (double)w1[0], (double)w1[1], (double)w1[2], (double)groundZ);
        }
        const float dz = w1[2] - w0[2];
        if (fabsf(dz) < 1e-6f) continue;
        for (int h = 0; h < 6; ++h) {
            const float hz = groundZ + kHeightOffsets[h];
            const float t = (hz - w0[2]) / dz;
            if (t < 0.0f || t > 1.0f) continue;   // the slab is outside this corner's depth range
            const float p[3] = { w0[0] + (w1[0] - w0[0]) * t,
                                 w0[1] + (w1[1] - w0[1]) * t,
                                 hz };
            addPoint(p);
            ++footprintHits;
        }
    }
    if (footprintHits == 0) {
        // Camera along the ground or above the slab ladder: keep the whole visible frustum so the
        // receivers inside the view still fall in the map instead of being reported as lit.
        for (int k = 0; k < 4; ++k) {
            if (!cornerValid[k]) continue;
            addPoint(cornerNear[k]);
            addPoint(cornerFar[k]);
            ++footprintHits;
        }
        static int s_frustumFallback = 4;
        if (footprintHits > 0 && s_frustumFallback > 0) {
            --s_frustumFallback;
            VKWP1_LOG("[VKSHADOW] footprint: frustum fallback (ground=%.1f corners=%d)",
                (double)groundZ, footprintHits);
        }
    }
    if (footprintHits == 0 && realView) {
        static int s_noHit = 4;
        if (s_noHit > 0) {
            --s_noHit;
            VKWP1_LOG("[VKSHADOW] footprint: no corner ray reached a slab (ground=%.1f)", (double)groundZ);
        }
    }
    X.shLastHits = (unsigned)footprintHits;
    X.shLastCam[0] = view[12]; X.shLastCam[1] = view[13]; X.shLastCam[2] = view[14];
    X.shLastProj[0] = proj[0]; X.shLastProj[1] = proj[10]; X.shLastProj[2] = proj[14];
    // GeneralsX @bugfix Qoder 21/10/2026: caster origins may keep the box sane when no corner ray
    // reached a slab, but once the footprint exists they must not inflate it. The shell map's far
    // camera sees casters spread over the whole map; folding all of them in pushed the side to 12k+
    // world units (a 2048 map then has 6 units per texel and small building shadows vanish). Keep only
    // casters near the visible footprint; the reference implementation does not fold casters in at all.
    const bool haveFootprint = lo[0] <= hi[0];
    // GeneralsX @bugfix 23/10/2026: 800 was not enough for a tall structure just outside the visible
    // footprint (its shadow still reaches the screen) or for a caster whose origin is well inside the
    // building footprint. A larger margin keeps those origins in the fit; the 4096 -> 8192 side cap
    // below is what keeps the extra area from re-clipping the far edge.
    const float casterFitMargin = 1200.0f;
    for (size_t i = 0; i < X.casters.size(); ++i) {
        const float* w = X.casters[i].world;
        if (fabsf(w[12]) > 60000.0f || fabsf(w[13]) > 60000.0f || fabsf(w[14]) > 5000.0f) continue;
        const float p[3] = { w[12], w[13], w[14] };
        // GeneralsX @bugfix 28/10/2026: identity-world batches (CPU-skinned and instanced vegetation bake
        // the world position into the vertices) all report (0,0,0) here. Folding that phantom point into
        // the fit drags the box toward the map origin and away from what is actually on screen, so those
        // casters rely on the frustum footprint alone - their geometry is inside it by definition.
        if (fabsf(p[0]) < 0.001f && fabsf(p[1]) < 0.001f && fabsf(p[2]) < 0.001f) continue;
        if (haveFootprint) {
            const float a[3] = { p[0] * r[0] + p[1] * r[1] + p[2] * r[2],
                                 p[0] * u[0] + p[1] * u[1] + p[2] * u[2],
                                 p[0] * d[0] + p[1] * d[1] + p[2] * d[2] };
            if (a[0] < lo[0] - casterFitMargin || a[0] > hi[0] + casterFitMargin ||
                a[1] < lo[1] - casterFitMargin || a[1] > hi[1] + casterFitMargin) continue;
        }
        addPoint(p);
    }
    if (!(lo[0] <= hi[0])) return false;
    {
        static int s_fpProbe = 3;
        if (s_fpProbe > 0 && realView) {
            --s_fpProbe;
            VKWP1_LOG("[VKSHADOW] footprint: hits=%d lo=(%.0f %.0f %.0f) hi=(%.0f %.0f %.0f) casters=%u",
                footprintHits, (double)lo[0], (double)lo[1], (double)lo[2],
                (double)hi[0], (double)hi[1], (double)hi[2], (unsigned)X.casters.size());
        }
    }

    // Room for the casters themselves: geometry above a receiver lies toward the sun (smaller light-space
    // depth), and a receiver can sit slightly below the slab. The far side used to add only 60 units,
    // which clipped every terrain level below the caster average: those receivers answered "outside the
    // map" and lost their shadow. The range only costs R32F depth precision, not map resolution.
    lo[0] -= 500.0f; hi[0] += 500.0f;
    lo[1] -= 500.0f; hi[1] += 500.0f;
    lo[2] -= 1500.0f;
    hi[2] += 1200.0f;

    // GeneralsX @bugfix 28/10/2026 shadow flicker + vanishing shadows (user report: "moving the camera
    // flickers", "zoom in and some shadows disappear, edges lose theirs"). The side used to follow the
    // requested footprint in 256-unit steps: measured with an orbit + zoom sweep, ONE run walked through
    // 8 different sides (2816..4864), and every change re-scales the light map AND re-snaps its origin,
    // so all shadow edges shimmer while the camera moves and casters/receivers that fall outside the
    // fitted box drop their shadow (the receiver answers "outside the map" and comes out fully lit).
    // The fit is therefore a FIXED world size, centred on the footprint and texel-snapped: the map
    // never rescales, so its texels are stable for the whole session and the box always covers the
    // visible footprint plus the margins above. 8192 world with the 4096 map is 2 units/texel, which
    // is the same density the 23/10 zoom-out fix settled on; GENERALSX_VKSHADOW_FIT_FIXED=0 restores
    // the old adaptive fit for an A/B, any other value sets the side.
    static float s_fitFixed = -1.0f;
    if (s_fitFixed < 0.0f) {
        const char* e = getenv("GENERALSX_VKSHADOW_FIT_FIXED");
        s_fitFixed = (e != nullptr && *e != 0) ? (float)atof(e) : 8192.0f;
        if (s_fitFixed < 0.0f) s_fitFixed = 0.0f;
    }
    float side;
    if (s_fitFixed > 0.0f) {
        side = s_fitFixed;
    } else {
        const float ext[2] = { hi[0] - lo[0], hi[1] - lo[1] };
        const float kFitMaxSide = 8192.0f;
        const float kFitShrinkRatio = 0.80f;
        float need = ext[0] > ext[1] ? ext[0] : ext[1];
        need = ceilf(need / 256.0f) * 256.0f;
        if (need < 256.0f) need = 256.0f;
        if (need > kFitMaxSide) need = kFitMaxSide;
        side = X.shFitSide;
        if (side <= 0.0f || side > kFitMaxSide) side = need;
        else if (need > side) side = need;
        else if (need < side * kFitShrinkRatio) side = need;
    }
    X.shFitSide = side;
    // Centre the box on the requested footprint so the view centre keeps shadows.
    const float cx = (lo[0] + hi[0]) * 0.5f;
    const float cy = (lo[1] + hi[1]) * 0.5f;
    lo[0] = cx - side * 0.5f;
    lo[1] = cy - side * 0.5f;
    hi[0] = lo[0] + side;
    hi[1] = lo[1] + side;
    const float texel = side / (float)X.shMapSize;
    lo[0] = floorf(lo[0] / texel) * texel;
    lo[1] = floorf(lo[1] / texel) * texel;
    hi[0] = lo[0] + side;
    hi[1] = lo[1] + side;
    outRange = hi[2] - lo[2];
    if (outRange < 1.0f) return false;
    outMapWorld = side;
    memcpy(outLo, lo, sizeof(lo));
    memcpy(outHi, hi, sizeof(hi));

    const float sa = 2.0f / side, sb = 2.0f / side, sc = 1.0f / outRange;
    memset(outVP, 0, sizeof(float) * 16);
    // Column-major: row i of the light view-projection sits in outVP[0*4+i], outVP[1*4+i], ...
    outVP[0] = r[0] * sa; outVP[4] = r[1] * sa; outVP[8]  = r[2] * sa; outVP[12] = -lo[0] * sa - 1.0f;
    outVP[1] = u[0] * sb; outVP[5] = u[1] * sb; outVP[9]  = u[2] * sb; outVP[13] = -lo[1] * sb - 1.0f;
    outVP[2] = d[0] * sc; outVP[6] = d[1] * sc; outVP[10] = d[2] * sc; outVP[14] = -lo[2] * sc;
    outVP[3] = 0.0f;      outVP[7] = 0.0f;      outVP[11] = 0.0f;      outVP[15] = 1.0f;
    return true;
}

VkShaderModule shadow_module(const uint32_t* code, size_t bytes)
{
    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smvi.codeSize = bytes; smvi.pCode = code;
    VkShaderModule m = nullptr;
    if (!chk("vkCreateShaderModule(vkshadow)", G.vkCreateShaderModule(dev_(), &smvi, nullptr, &m))) return nullptr;
    return m;
}

bool shadow_ensure()
{
    if (X.shFailed) return false;
    // shRecvPSO is created lazily inside the first receiver pass, so it cannot be the completion signal:
    // testing it made the second frame build a whole second set of modules, layouts and pools.
    if (X.shReady) return true;
    // GeneralsX @feature Qoder 20/10/2026 the reference backend picks 4096 for its top quality tier.
    // 2048 stays the default; a test run can ask for the sharper tier without a rebuild.
    if (const char* q = getenv("GENERALSX_VKSHADOW_SIZE")) {
        const int v = atoi(q);
        if (v == 1024 || v == 2048 || v == 4096) X.shMapSize = (unsigned)v;
    }
    VkDevice dev = dev_();
    if (!dev || !core_()) return false;

    VkPhysicalDeviceProperties props = {};
    G.vkGetPhysicalDeviceProperties(core_()->Phys(), &props);
    // The receiver pushes two mat4 plus three vec4 = 176 bytes (viewport + normal-offset aux); the
    // specification only guarantees 128, so a part below that cannot run this pass and the reason is
    // named once instead of failing every frame.
    if (props.limits.maxPushConstantsSize < 176u) {
        X.shFailed = true;
        VKWP1_LOG("[VKSHADOW] off: maxPushConstantsSize=%u < 176",
            (unsigned)props.limits.maxPushConstantsSize);
        return false;
    }

    if (!X.shCastVs) X.shCastVs = shadow_module(VKSHADOW__CAST_VERT_SPV, sizeof(VKSHADOW__CAST_VERT_SPV));
    if (!X.shCastPs) X.shCastPs = shadow_module(VKSHADOW__CAST_FRAG_SPV, sizeof(VKSHADOW__CAST_FRAG_SPV));
    if (!X.shCastAlphaPs)
        X.shCastAlphaPs = shadow_module(VKSHADOW__CAST_ALPHA_FRAG_SPV, sizeof(VKSHADOW__CAST_ALPHA_FRAG_SPV));
    if (!X.shRecvVs) X.shRecvVs = shadow_module(VKSHADOW__RECV_VERT_SPV, sizeof(VKSHADOW__RECV_VERT_SPV));
    if (!X.shRecvPs) X.shRecvPs = shadow_module(VKSHADOW__RECV_FRAG_SPV, sizeof(VKSHADOW__RECV_FRAG_SPV));
    if (!X.shCastVs || !X.shCastPs || !X.shCastAlphaPs || !X.shRecvVs || !X.shRecvPs) {
        X.shFailed = true;
        VKWP1_LOG("[VKSHADOW] off: shader module creation failed");
        return false;
    }

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 128u };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (!chk("vkCreatePipelineLayout(vkshadow cast)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &X.shCastLayout))) {
        X.shFailed = true; return false;
    }

    VkDescriptorSetLayoutBinding texBinding = {};
    texBinding.binding = 0;
    texBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texBinding.descriptorCount = 1;
    texBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo clci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    clci.bindingCount = 1; clci.pBindings = &texBinding;
    if (!chk("vkCreateDescriptorSetLayout(vkshadow cast)",
             G.vkCreateDescriptorSetLayout(dev, &clci, nullptr, &X.shCastSetLayout))) {
        X.shFailed = true; return false;
    }
    // The alpha-tested variant pushes one extra vec4 (reference, compare, enable) and reads the texture,
    // so it needs both stages in the range and the texture set in the layout. Opaque casters keep the
    // smaller layout: no descriptor is bound for them, and one layout cannot describe both.
    VkPushConstantRange pcrA = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 144u };
    VkPipelineLayoutCreateInfo plciA = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plciA.setLayoutCount = 1; plciA.pSetLayouts = &X.shCastSetLayout;
    plciA.pushConstantRangeCount = 1; plciA.pPushConstantRanges = &pcrA;
    if (!chk("vkCreatePipelineLayout(vkshadow cast alpha)", G.vkCreatePipelineLayout(dev, &plciA, nullptr, &X.shCastAlphaLayout))) {
        X.shFailed = true; return false;
    }

    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) {
        VkDescriptorPoolSize cps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1024u };
        VkDescriptorPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        cpci.maxSets = 1024u;
        cpci.poolSizeCount = 1; cpci.pPoolSizes = &cps;
        if (!chk("vkCreateDescriptorPool(vkshadow cast)", G.vkCreateDescriptorPool(dev, &cpci, nullptr, &X.shCastPool[i]))) {
            X.shFailed = true; return false;
        }
    }

    VkDescriptorSetLayoutBinding binding[3] = {};
    for (int i = 0; i < 3; ++i) {
        binding[i].binding = (uint32_t)i;
        binding[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding[i].descriptorCount = 1;
        binding[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo slci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    slci.bindingCount = 3; slci.pBindings = binding;
    if (!chk("vkCreateDescriptorSetLayout(vkshadow)",
             G.vkCreateDescriptorSetLayout(dev, &slci, nullptr, &X.shSetLayout))) {
        X.shFailed = true; return false;
    }
    VkPushConstantRange pcr2 = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 192u };
    VkPipelineLayoutCreateInfo plci2 = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci2.setLayoutCount = 1; plci2.pSetLayouts = &X.shSetLayout;
    plci2.pushConstantRangeCount = 1; plci2.pPushConstantRanges = &pcr2;
    if (!chk("vkCreatePipelineLayout(vkshadow recv)", G.vkCreatePipelineLayout(dev, &plci2, nullptr, &X.shRecvLayout))) {
        X.shFailed = true; return false;
    }

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3u * (uint32_t)VKGEO_SLOT_COUNT };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    pci.maxSets = (uint32_t)VKGEO_SLOT_COUNT;
    pci.poolSizeCount = 1; pci.pPoolSizes = &ps;
    if (!chk("vkCreateDescriptorPool(vkshadow)", G.vkCreateDescriptorPool(dev, &pci, nullptr, &X.shPool))) {
        X.shFailed = true; return false;
    }

    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    si.magFilter = VK_FILTER_NEAREST; si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.compareEnable = VK_FALSE;
    si.minLod = 0.0f; si.maxLod = 0.0f;
    if (!chk("vkCreateSampler(vkshadow)", G.vkCreateSampler(dev, &si, nullptr, &X.shSampler))) {
        X.shFailed = true; return false;
    }
    VkSamplerCreateInfo csi = si;
    csi.magFilter = csi.minFilter = VK_FILTER_LINEAR;
    csi.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    csi.addressModeU = csi.addressModeV = csi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    csi.maxLod = 16.0f;
    if (!chk("vkCreateSampler(vkshadow cloud)", G.vkCreateSampler(dev, &csi, nullptr, &X.shCloudSampler))) {
        X.shFailed = true; return false;
    }

    // The map's colour is what the receiver samples: R = light-space NDC depth, far = 1. The depth
    // attachment only resolves caster-to-caster occlusion inside the replay.
    if (!VKRT_Alloc_Color_Format(X.shColorKey, X.shMapSize, X.shMapSize, (unsigned)VK_FORMAT_R32_SFLOAT) ||
        !VKRT_Alloc_Depth_Format(X.shDepthKey, X.shMapSize, X.shMapSize, (unsigned)VK_FORMAT_D32_SFLOAT)) {
        X.shFailed = true;
        VKWP1_LOG("[VKSHADOW] off: shadow map targets could not be created (%ux%u R32F/D32)",
            X.shMapSize, X.shMapSize);
        return false;
    }
    X.shReady = true;
    VKWP1_LOG("[VKSHADOW] ready: %ux%u R32F depth map, strength=%.2f penumbra=%.2f", X.shMapSize, X.shMapSize,
        (double)shadow_strength(), (double)shadow_penumbra());
    return true;
}

VkPipeline shadow_cast_pso(const VKShadowCaster& c)
{
    for (size_t i = 0; i < X.shCastPSOs.size(); ++i) {
        const VKShadowCastPSO& e = X.shCastPSOs[i];
        // uv_offset and alpha_enable are part of the identity: the same stride drawn with and without the
        // cutout is a different vertex layout (one has location 1) and a different fragment stage.
        if (e.stride == c.stride && e.pos_offset == c.pos_offset && e.topology == c.topology &&
            e.uv_offset == c.uv_offset && e.alpha_enable == c.alpha_enable) return e.pso;
    }
    if (X.curPass >= X.passes.size()) return nullptr;

    VkVertexInputBindingDescription vbd = { 0, c.stride, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription vad[2] = {};
    vad[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT, c.pos_offset };
    unsigned vadCount = 1;
    if (c.alpha_enable) {
        // The fragment stage tests tex0.a, so the cast pipeline declares the same coordinate set the
        // scene used. Location 1 in the cast shader, whatever location the FVF gave it.
        vad[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, c.uv_offset };
        vadCount = 2;
    }
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vbd;
    vin.vertexAttributeDescriptionCount = vadCount; vin.pVertexAttributeDescriptions = vad;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = vk_topology(c.topology);

    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    // Two-sided: a shadow caster's winding must not decide whether it occludes.
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    VkPipelineShaderStageCreateInfo stg[2] = {};
    stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = X.shCastVs; stg[0].pName = "main";
    stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stg[1].module = c.alpha_enable ? X.shCastAlphaPs : X.shCastPs;
    stg[1].pName = "main";

    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2; gpi.pStages = stg;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
    gpi.layout = c.alpha_enable ? X.shCastAlphaLayout : X.shCastLayout;
    gpi.renderPass = X.passes[X.curPass].rp;
    gpi.subpass = 0;

    VkPipeline pso = nullptr;
    if (!chk("vkCreateGraphicsPipelines(vkshadow cast)",
             G.vkCreateGraphicsPipelines(dev_(), nullptr, 1, &gpi, nullptr, &pso)))
        return nullptr;
    VKShadowCastPSO e; e.stride = c.stride; e.pos_offset = c.pos_offset; e.topology = c.topology;
    e.uv_offset = c.alpha_enable ? c.uv_offset : 0u; e.alpha_enable = c.alpha_enable; e.pso = pso;
    X.shCastPSOs.push_back(e);
    return pso;
}

VkPipeline shadow_recv_pso()
{
    if (X.curPass >= X.passes.size()) return nullptr;
    if (X.shRecvPSO && X.shRecvPass == X.curPass) return X.shRecvPSO;

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = VK_FALSE;
    dss.depthWriteEnable = VK_FALSE;
    // P6 per-material receive: only pixels stamped by a RECEIVE_SHADOW draw are darkened. The stencil
    // aspect is optional (D32_SFLOAT fallback has none), in which case the old full-screen behaviour stays.
    bool stencilReceivers = false;
    if (X.depthKey) {
        VKRT_Info di = {};
        if (VKRT_Query(X.depthKey, &di) && di.is_depth && vk_format_has_stencil((VkFormat)di.format))
            stencilReceivers = true;
    }
    if (stencilReceivers) {
        dss.stencilTestEnable = VK_TRUE;
        VkStencilOpState so = {};
        so.compareOp = VK_COMPARE_OP_EQUAL;
        so.compareMask = VK_SHADOW_RECEIVE_BIT;
        so.writeMask = 0u;
        so.reference = VK_SHADOW_RECEIVE_BIT;
        so.failOp = VK_STENCIL_OP_KEEP;
        so.depthFailOp = VK_STENCIL_OP_KEEP;
        so.passOp = VK_STENCIL_OP_KEEP;
        dss.front = so; dss.back = so;
    }

    // Scene colour *= visibility: zero contribution from the new fragment, destination scaled by it.
    // Diagnostic modes replace the colour instead, so 0.35/0.75/0.5..1 read as plain grey levels.
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = shadow_debug_mode() ? VK_FALSE : VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.colorWriteMask = (VkColorComponentFlags)(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                 VK_COLOR_COMPONENT_B_BIT);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    VkPipelineShaderStageCreateInfo stg[2] = {};
    stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = X.shRecvVs; stg[0].pName = "main";
    stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = X.shRecvPs; stg[1].pName = "main";

    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2; gpi.pStages = stg;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
    gpi.layout = X.shRecvLayout;
    gpi.renderPass = X.passes[X.curPass].rp;
    gpi.subpass = 0;

    VkPipeline pso = nullptr;
    if (!chk("vkCreateGraphicsPipelines(vkshadow recv)",
             G.vkCreateGraphicsPipelines(dev_(), nullptr, 1, &gpi, nullptr, &pso)))
        return nullptr;
    // A format change would invalidate the old pipeline; that does not happen at run time (the scene
    // mirror's format is fixed), but naming the pass keeps the cache honest if it ever does.
    X.shRecvPSO = pso;
    X.shRecvPass = X.curPass;
    return pso;
}

bool shadow_update_set(VkImageView shadowView, VkImageView depthView, VkImageView cloudView)
{
    const unsigned slot = X.slot;
    if (!X.shSets[slot]) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = X.shPool;
        ai.descriptorSetCount = 1; ai.pSetLayouts = &X.shSetLayout;
        if (G.vkAllocateDescriptorSets(dev_(), &ai, &X.shSets[slot]) != VK_SUCCESS) return false;
        X.shSetViews[slot][0] = nullptr;
        X.shSetViews[slot][1] = nullptr;
        X.shSetViews[slot][2] = nullptr;
    }
    if (X.shSetViews[slot][0] == shadowView && X.shSetViews[slot][1] == depthView &&
        X.shSetViews[slot][2] == cloudView) return true;

    VkDescriptorImageInfo ii[3] = {};
    for (int i = 0; i < 3; ++i) {
        ii[i].sampler = (i == 2) ? X.shCloudSampler : X.shSampler;
        ii[i].imageView = (i == 0) ? shadowView : ((i == 1) ? depthView : (cloudView ? cloudView : depthView));
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkWriteDescriptorSet w[3] = {};
    for (int i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = X.shSets[slot];
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &ii[i];
    }
    G.vkUpdateDescriptorSets(dev_(), 3, w, 0, nullptr);
    X.shSetViews[slot][0] = shadowView;
    X.shSetViews[slot][1] = depthView;
    X.shSetViews[slot][2] = cloudView;
    return true;
}

// The alpha-tested cast pass samples the scene's own tex0. One set per distinct (texture, sampler) pair:
// a forest is a few textures across hundreds of batches, so the cache turns the whole canopy into a
// handful of allocations per frame.
VkDescriptorSet shadow_cast_set(void* tex, void* samp)
{
    const unsigned slot = X.slot;
    for (size_t i = 0; i < X.shCastSets[slot].size(); ++i)
        if (X.shCastSets[slot][i].tex == tex && X.shCastSets[slot][i].samp == samp)
            return X.shCastSets[slot][i].set;

    if (!X.shCastPool[slot] || !X.shCastSetLayout) return nullptr;
    VkDescriptorSet set = nullptr;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = X.shCastPool[slot];
    ai.descriptorSetCount = 1; ai.pSetLayouts = &X.shCastSetLayout;
    if (G.vkAllocateDescriptorSets(dev_(), &ai, &set) != VK_SUCCESS) return nullptr;

    VkDescriptorImageInfo ii = {};
    ii.sampler = samp ? (VkSampler)samp : X.shSampler;
    ii.imageView = (VkImageView)tex;
    ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &ii;
    G.vkUpdateDescriptorSets(dev_(), 1, &w, 0, nullptr);

    VKShadowCastSet e; e.tex = tex; e.samp = samp; e.set = set;
    X.shCastSets[slot].push_back(e);
    return set;
}

// Called from VK_Draw_Submit for a batch that is about to be recorded into the scene. Opaque,
// depth-writing, world-space triangles are candidates; identity-world batches (terrain, sky, UI) are
// receivers. Alpha-tested batches are candidates too - the reference records what the scene recorded -
// but they carry their tex0 and the compare into the cast pass, so a leaf card occludes only where the
// texture says it is opaque instead of casting a solid quad.
// GeneralsX @bugfix Qoder 20/10/2026 the native sun pass must not borrow a camera from camera-space
// geometry. Grass, smudge and point groups bake the camera into their vertices and draw with
// D3DTS_VIEW = identity. When such a batch happened to be the last one recorded, the fit and the
// receiver reconstructed the world through that identity view, every lookup landed outside the map and
// the scene lost all shadows for as long as the effect stayed on screen. Identity means "not a camera",
// not "a camera at the origin".
bool vkdraw_no_camera(const float* view, const float* proj)
{
    static const float id[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
    bool viewIdent = true, projIdent = true;
    for (int i = 0; i < 16; ++i) {
        if (fabsf(view[i] - id[i]) > 1e-5f) viewIdent = false;
        if (fabsf(proj[i] - id[i]) > 1e-5f) projIdent = false;
    }
    return viewIdent || projIdent;
}

void shadow_record_caster(const VKDraw_Submit& s)
{
    if (!shadow_env_on() || X.shFailed) return;
    if (!X.shCasterAllowed) return;
    if (s.ff && vkdraw_no_camera(s.ff->view, s.ff->proj)) return;

    // One line per vertex program (and axis: skin or not) that reaches this recorder, with the state
    // that decides whether it can cast. A skin batch is CPU-skinned geometry: its vertices are already
    // in world space even though it is drawn by the fixed-function vertex stage, so it cannot be told
    // apart from terrain by its world or its program - only by the engine's own batch flag.
    {
        static unsigned s_tagSeen[64];
        static unsigned s_tagN = 0u;
        const unsigned key = (s.key.vs_tag << 1) | (s.skin_batch ? 1u : 0u);
        bool seen = false;
        for (unsigned i = 0; i < s_tagN; ++i) if (s_tagSeen[i] == key) { seen = true; break; }
        if (!seen && s_tagN < 64u) {
            s_tagSeen[s_tagN++] = key;
            const float* w = s.ff ? s.ff->world : nullptr;
            fprintf(stderr, "[VKSHADOW] tag census: vs=%u ps=%u skin=%u blend=%u zw=%u mask=%u topo=%u attrs=%u "
                "wpos=(%.2f,%.2f,%.2f) tex0=%p\n",
                s.key.vs_tag, s.key.ps_tag, s.skin_batch, s.key.blend_enable, s.key.depth_write,
                s.key.write_mask, s.key.topology, s.attr_count,
                w ? w[12] : -999.f, w ? w[13] : -999.f, w ? w[14] : -999.f, s.texture_views[0]);
            fflush(stderr);
        }
    }
    if (s.key.topology != D3D_PT_TRIANGLELIST && s.key.topology != D3D_PT_TRIANGLESTRIP) return;
    if (s.key.blend_enable || !s.key.depth_write) return;
    if (!(s.key.write_mask & 0x7u)) return;
    if (!s.ff || (s.ff->flags[0] & VKFF_FLAG_RHW)) return;
    if (!s.vk_index_buffer || !s.streams[0].vk_buffer) return;
    if (X.casters.size() >= 8192) return;
    // Identity world normally means "already in world space and not an object": terrain, sky, water, UI.
    // CPU-skinned meshes are the exception - DX8SkinFVFCategoryContainer writes world-space vertices and
    // declares the identity - so the engine's batch flag, not the vertex program, is what identifies them.
    // GeneralsX @feature Qoder 24/10/2026 T3.5/E1: the baked B1 terrain/road pixel tags are the second
    // exception. Those batches are heightmap/road chunks whose vertices are already in world space and
    // whose world matrix is the identity, which is exactly why hills, cliffs and road embankments never
    // cast a shadow before (the receiver fit had to compensate by sampling a ladder of ground heights).
    // Recognise them by ps tag (110..118 terrain variants, 120 road) and let them cast. The env switch
    // keeps the old behaviour for A/B: GENERALSX_VK_TERRAINSHADOW=0.
    static const int s_terrainCaster = []() {
        const char* e = getenv("GENERALSX_VK_TERRAINSHADOW");
        return (e != nullptr && *e != 0) ? (atoi(e) != 0 ? 1 : 0) : 1;
    }();
    static const float s_ident[16] = { 1,0,0,0,  0,1,0,0,  0,0,1,0,  0,0,0,1 };
    const bool skin_verts = (s.skin_batch != 0u);
    const unsigned psTagId = s.key.ps_tag;
    const bool terrain_like = s_terrainCaster != 0
        && ((psTagId >= 110u && psTagId <= 118u) || psTagId == 120u);
    if (!skin_verts && !terrain_like
        && s.ff->world[12] == 0.0f && s.ff->world[13] == 0.0f && s.ff->world[14] == 0.0f) {
        const unsigned vs = s.key.vs_tag;
        if (vs != 7u && vs != 9u) {
            // GeneralsX @debug 28/10/2026 tree/bush caster audit: which identity-world batches are being
            // refused, so the foliage tags can be identified instead of guessed. Bounded.
            static int s_rejProbe = 40;
            if (s_rejProbe > 0) {
                --s_rejProbe;
                fprintf(stderr, "[SHCASTER] identity refused vs=%u ps=%u alpha=%u ref=%u stride=%u idx=%u tex=%p\n",
                    vs, s.key.ps_tag, s.key.alpha_test_enable ? 1u : 0u, s.key.alpha_ref & 0xFFu,
                    s.streams[0].stride, (unsigned)s.polygon_count, s.texture_views[0]);
                fflush(stderr);
            }
            return;
        }
    }

    const VKDraw_Attr* pos = nullptr;
    for (unsigned i = 0; i < s.attr_count && i < VKDRAW_MAX_ATTRS; ++i)
        if (s.attrs[i].location == VKDRAW_LOC_POSITION) { pos = &s.attrs[i]; break; }
    if (!pos || pos->type != 0u || pos->components != 3u) return;

    unsigned alpha_enable = s.key.alpha_test_enable ? 1u : 0u;
    unsigned uv_offset = 0u, alpha_ref = 0u, alpha_cmp = 7u;
    if (alpha_enable) {
        // Without the texture the cutout cannot be reproduced, and a solid card is worse than no shadow.
        if (!s.texture_views[0]) return;
        const VKDraw_Attr* uv = nullptr;
        for (unsigned i = 0; i < s.attr_count && i < VKDRAW_MAX_ATTRS; ++i)
            if (s.attrs[i].location == VKDRAW_LOC_TEX0) { uv = &s.attrs[i]; break; }
        if (!uv || uv->type != 0u || uv->components < 2u) return;
        uv_offset = uv->offset;
        alpha_ref = s.key.alpha_ref & 0xFFu;
        const unsigned f = s.key.alpha_cmp;
        alpha_cmp = (f >= 1u && f <= 8u) ? (f - 1u) : 7u;   // D3DCMPFUNC -> the shader's 0..7
    }

    VKShadowCaster c = {};
    c.vb = (VkBuffer)s.streams[0].vk_buffer;
    c.voff = (VkDeviceSize)s.streams[0].offset;
    c.stride = s.streams[0].stride;
    c.ib = (VkBuffer)s.vk_index_buffer;
    c.ioff = (VkDeviceSize)s.index_offset + (VkDeviceSize)(s.start_index * 2u);
    c.index_count = vk_index_count_for(s.key.topology, s.polygon_count);
    c.base_vertex = (int)s.base_vertex;
    c.pos_offset = pos->offset;
    c.topology = s.key.topology;
    // A skin batch's vertices are already world-space; applying the (possibly stale) world transform
    // twice is what put the shadow somewhere other than under the infantry.
    memcpy(c.world, skin_verts ? s_ident : s.ff->world, sizeof(c.world));
    c.tex = s.texture_views[0];
    c.samp = s.texture_samplers[0];
    c.uv_offset = uv_offset;
    c.alpha_enable = alpha_enable;
    c.alpha_ref = alpha_ref;
    c.alpha_cmp = alpha_cmp;
    c.skin = skin_verts ? 1u : 0u;
    X.casters.push_back(c);

    // Camera-independent ground estimate for the light frustum slab: the average height of the casters
    // (terrain itself is identity-world and therefore not recorded). A caster far away from the running
    // estimate is a new world (map change before the explicit reset lands) or an outlier such as a
    // cliff-top base; re-seed in that case. Blending at 0.5% from the previous map's height made the
    // sun fit take seconds to follow a load, which read as "no ground shadow after loading".
    const float z = s.ff->world[14];
    if (z > -500.0f && z < 2000.0f && z != 0.0f) {
        if (!X.shGroundInit || fabsf(z - X.shGroundZ) > 700.0f) { X.shGroundZ = z; X.shGroundInit = 1; }
        else X.shGroundZ = X.shGroundZ * 0.995f + z * 0.005f;
    }
}

// GeneralsX @debug Qoder 20/10/2026 periodic-disappearance forensics: the native pass has a dozen exits
// that all look the same from outside (the scene simply loses its shadows for that frame), and the
// 300-frame summary cannot name which one fired. This prints one line per frame with the branch that
// decided the outcome, so a vanish can be matched against the frame it happened on. Off by default.
// GeneralsX @feature Qoder 22/10/2026: light shadows are opt-in (default OFF) until they have been
// evaluated; GENERALSX_VK_LIGHTSHADOW=1 turns the strongest light's own map on.
bool light_shadow_on()
{
    if (s_lsEnv < 0) {
        const char* e = getenv("GENERALSX_VK_LIGHTSHADOW");
        s_lsEnv = (e && atoi(e) != 0) ? 1 : 0;
    }
    return s_lsEnv != 0;
}

bool shadow_trace_on()
{
    static const bool on = [] {
        const char* e = getenv("GENERALSX_VKSHADOW_TRACE");
        return e != nullptr && atoi(e) != 0;
    }();
    return on;
}

void shadow_trace(const char* why, const char* detail, unsigned extra, int applied, unsigned replayed,
                  float side, float range)
{
    if (!shadow_trace_on()) return;
    static unsigned n = 0;
    if (n >= 24000u) return;
    ++n;
    fprintf(stderr, "[VKSTRACE] f=%llu slot=%u why=%s casters=%u haveVP=%u extra=%u applied=%d "
        "replayed=%u side=%.0f range=%.0f ground=%.1f hits=%u cam=(%.1f %.1f %.1f) proj=(%.4f %.4f %.4f) detail=%s\n",
        X.st.framesBegun, X.slot, why, (unsigned)X.casters.size(), X.shHaveVP, extra, applied,
        replayed, (double)side, (double)range, (double)X.shGroundZ, X.shLastHits,
        (double)X.shLastCam[0], (double)X.shLastCam[1], (double)X.shLastCam[2],
        (double)X.shLastProj[0], (double)X.shLastProj[1], (double)X.shLastProj[2],
        detail ? detail : "-");
    fflush(stderr);
}

} // anon

// ===========================================================================
// GeneralsX @feature Qoder 21/10/2026 in-game GPU vegetation
// ===========================================================================
namespace {

struct VKVegPush {
    float vp[16];      // column-major world -> clip
    float cam[4];      // xyz eye, w time
    float sun[4];      // xyz direction toward the sun
    float fog[4];      // rgb fog colour, w fog start
    float params[4];   // x fog end, y cull distance, z/w reserved
    // GeneralsX @feature 27/10/2026 unified lighting: the shared rig, so foliage is lit per pixel from
    // the same sun/sky/ground every other family reads (no per-family light constants).
    float sunCol[4];   // rgb rig sun colour (live sweep applied)
    float skyCol[4];   // rgb rig sky ambient
    float grdCol[4];   // rgb rig ground ambient
};

static const unsigned VKVEG_INSTANCE_BYTES = 48u;
static const unsigned VKVEG_MAX_INSTANCES = 60000u;

bool vkveg_ensure()
{
    if (X.veg.failed) return false;
    if (X.veg.ready) return true;
    VkDevice dev = dev_();
    if (!dev) return false;

    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smvi.codeSize = sizeof(VKVEG_GRA_VERT_SPV); smvi.pCode = VKVEG_GRA_VERT_SPV;
    if (!chk("vkCreateShaderModule(vkveg grass vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.veg.gvs))) { X.veg.failed = true; return false; }
    smvi.codeSize = sizeof(VKVEG_GRA_FRAG_SPV); smvi.pCode = VKVEG_GRA_FRAG_SPV;
    if (!chk("vkCreateShaderModule(vkveg grass fs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.veg.gfs))) { X.veg.failed = true; return false; }
    smvi.codeSize = sizeof(VKVEG_ROCK_VERT_SPV); smvi.pCode = VKVEG_ROCK_VERT_SPV;
    if (!chk("vkCreateShaderModule(vkveg rock vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.veg.rvs))) { X.veg.failed = true; return false; }
    smvi.codeSize = sizeof(VKVEG_ROCK_FRAG_SPV); smvi.pCode = VKVEG_ROCK_FRAG_SPV;
    if (!chk("vkCreateShaderModule(vkveg rock fs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.veg.rfs))) { X.veg.failed = true; return false; }

    // 128 bytes is the guaranteed push-constant budget and exactly what the block above needs.
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(VKVegPush) };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (!chk("vkCreatePipelineLayout(vkveg)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &X.veg.layout))) {
        X.veg.failed = true; return false;
    }

    X.veg.ready = true;
    VKWP1_LOG("[VKVEG] ready: instanced blades + stones, %u bytes push constants, cap %u instances",
        (unsigned)sizeof(VKVegPush), VKVEG_MAX_INSTANCES);
    return true;
}

// Both pipelines target the pass that is open for the frame; a change of pass (render resolution,
// MSAA tier) invalidates them the same way the shadow receiver is invalidated.
bool vkveg_pipelines()
{
    if (!X.veg.ready) return false;
    VkRenderPass rp = X.passes[X.curPass].rp;
    if (X.veg.gpipe && X.veg.rpipe && X.veg.pass == rp) return true;

    if (X.veg.gpipe) { G.vkDestroyPipeline(dev_(), X.veg.gpipe, nullptr); X.veg.gpipe = nullptr; }
    if (X.veg.rpipe) { G.vkDestroyPipeline(dev_(), X.veg.rpipe, nullptr); X.veg.rpipe = nullptr; }
    X.veg.pass = rp;

    VkVertexInputBindingDescription vib = {};
    vib.binding = 0; vib.stride = VKVEG_INSTANCE_BYTES; vib.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
    VkVertexInputAttributeDescription via[3] = {};
    via[0] = { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
    via[1] = { 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16 };
    via[2] = { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32 };
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vin.vertexBindingDescriptionCount = 1; vin.pVertexBindingDescriptions = &vib;
    vin.vertexAttributeDescriptionCount = 3; vin.pVertexAttributeDescriptions = via;

    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = (VkSampleCountFlagBits)X.passes[X.curPass].samples;

    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = (VkColorComponentFlags)(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    for (int pass = 0; pass < 2; ++pass) {
        const bool rocks = pass == 1;
        VkPipelineShaderStageCreateInfo stg[2] = {};
        stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stg[0].module = rocks ? X.veg.rvs : X.veg.gvs;
        stg[0].pName = "main";
        stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stg[1].module = rocks ? X.veg.rfs : X.veg.gfs;
        stg[1].pName = "main";

        VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
        ia.topology = rocks ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;

        VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
        gpi.stageCount = 2; gpi.pStages = stg;
        gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
        gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
        gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
        gpi.layout = X.veg.layout; gpi.renderPass = rp; gpi.subpass = 0;

        VkPipeline pso = nullptr;
        if (!chk(rocks ? "vkCreateGraphicsPipelines(vkveg rock)" : "vkCreateGraphicsPipelines(vkveg grass)",
                 G.vkCreateGraphicsPipelines(dev_(), nullptr, 1, &gpi, nullptr, &pso))) {
            X.veg.failed = true;
            return false;
        }
        if (rocks) X.veg.rpipe = pso; else X.veg.gpipe = pso;
    }
    return true;
}

bool vkveg_buffer(unsigned slot, unsigned long long bytes)
{
    if (slot >= VKGEO_SLOT_COUNT) return false;
    if (X.veg.buf[slot] && X.veg.cap[slot] >= bytes) return true;
    if (X.veg.buf[slot]) {
        free_host_buffer(X.veg.buf[slot], X.veg.mem[slot], &X.veg.map[slot]);
        X.veg.buf[slot] = nullptr; X.veg.mem[slot] = nullptr; X.veg.cap[slot] = 0;
    }
    unsigned long long cap = (bytes + (1ull << 20) - 1ull) & ~((1ull << 20) - 1ull);
    if (cap < (2ull << 20)) cap = 2ull << 20;
    if (!make_host_buffer(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                          &X.veg.buf[slot], &X.veg.mem[slot], &X.veg.map[slot])) return false;
    X.veg.cap[slot] = cap;
    return true;
}

} // anon

bool VKDraw_LightShadow_Info(int slot, float vp[16], float pos_range[4])
{
    if (slot < 0 || slot >= (int)VkLighting::kShadowMaps) return false;
    if (!s_lsValid[slot] || !vp || !pos_range) return false;
    memcpy(vp, s_lsVP[slot], sizeof(s_lsVP[slot]));
    memcpy(pos_range, s_lsPosRange[slot], sizeof(s_lsPosRange[slot]));
    return true;
}

int VKDraw_LightShadow_Count()
{
    return s_lsSelectCount;
}

void* VKDraw_LightShadow_View(int slot)
{
    if (slot < 0 || slot >= (int)VkLighting::kShadowMaps || !s_lsValid[slot]) return nullptr;
    VKRT_Info mi = {};
    if (VKRT_Query(s_lsKey[slot], &mi) && mi.view) return mi.view;
    return nullptr;
}

void VKDraw_Set_Environment(const float sun_dir[3], const float sun_color[3], const float sky_color[3],
                            const float ground_color[3], const float fill_dir[3], const float fill_color[3],
                            float reflect_strength)
{
    // GeneralsX @feature 24/10/2026 P34 C4: the eight reserved header vec4s, filled from the same
    // W3DRenderLightingState terrain/road/GN read. Shaders declare them as fpEnv[8] and build the sky
    // reflection (and a colour fallback when the cubemap is not uploaded) from them.
    auto put = [](float* dst, const float* src3, float w) {
        for (int i = 0; i < 3; ++i) dst[i] = src3 ? src3[i] : 0.0f;
        dst[3] = w;
    };
    put(s_envVec[0], sun_dir, 1.0f);
    put(s_envVec[1], sun_color, reflect_strength);
    put(s_envVec[2], sky_color, 0.0f);      // .w is overwritten with the sky-cube-ready flag per frame
    put(s_envVec[3], ground_color, 0.0f);
    put(s_envVec[4], fill_dir, 0.0f);
    put(s_envVec[5], fill_color, 0.0f);
    for (int i = 6; i < 8; ++i) for (int c = 0; c < 4; ++c) s_envVec[i][c] = 0.0f;
    s_envStrength = (reflect_strength > 0.0f) ? reflect_strength : 0.0f;
}

void* VKDraw_Sky_View()
{
    return vksky_view_internal();
}

// GeneralsX @feature 26/10/2026 HDR: the scene is linear exactly when the HDR target is active and the
// recorder is not drawing the UI overlay. Texture binding and the FF packer both key their colour-space
// conversion off this one predicate, so the two can never disagree about a batch.
bool VKDraw_Linear_Scene_Active()
{
    return X.hdrOn != 0 && X.overlayActive == 0;
}

void* VKDraw_Sky_Sampler()
{
    return vksky_sampler_internal();
}

bool VKDraw_Vegetation_Ready()
{
    return VKDraw_Ready() && !X.veg.failed;
}

bool VKDraw_Vegetation_Bind()
{
    return vkveg_ensure() && vkveg_pipelines();
}

// ===========================================================================
// 光影升级2.0 B1 step 1: GPU particle scaffold.
// N camera-facing, procedurally shaded points recorded into the scene pass that is already open. No
// vertex buffer and no descriptor set: the instance index and a 128-byte push block carry everything,
// which is the cheapest proof that a self-made pipeline can draw inside the engine's 3D pass. The
// prototype's compute simulation lands on top of this in step 2.
// ===========================================================================
struct VKFXPush {
    float vp[16];
    float right[4];      // xyz = world right, w = time (seconds)
    float up[4];         // xyz = world up,    w = particle size
    float fwd[4];        // xyz = world forward (away from the eye), w = intensity
    float camPos[4];     // xyz = camera world position, w = dt (seconds)
    float soft[4];       // x = enabled, y = fade band (NDC depth), z/w reserved
};

static bool vkfx_buffers(unsigned count)
{
    if (X.fx.count == count && X.fx.pbuf[0]) return true;
    const unsigned long long bytes = (unsigned long long)count * 48ull;   // Particle = 3 x vec4
    const unsigned long long sbytes = 16ull + (unsigned long long)count * 32ull;
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) {
        if (X.fx.pbuf[i]) free_host_buffer(X.fx.pbuf[i], X.fx.pmem[i], &X.fx.pmap[i]);
        if (X.fx.ibuf[i]) free_host_buffer(X.fx.ibuf[i], X.fx.imem[i], &X.fx.imap[i]);
        if (X.fx.sbuf[i]) free_host_buffer(X.fx.sbuf[i], X.fx.smem[i], &X.fx.smap[i]);
        X.fx.pbuf[i] = nullptr; X.fx.ibuf[i] = nullptr; X.fx.sbuf[i] = nullptr;
        if (!make_host_buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             &X.fx.pbuf[i], &X.fx.pmem[i], &X.fx.pmap[i])) return false;
        // age = 1 means "finished", so nothing renders until a real spawn arrives or the compute
        // admits one; the buffer's zeroed state would otherwise show up at the world origin.
        float* p = (float*)X.fx.pmap[i];
        for (unsigned k = 0; k < count; ++k) p[k * 12 + 3] = 1.0f;

        if (!make_host_buffer(sbytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              &X.fx.sbuf[i], &X.fx.smem[i], &X.fx.smap[i])) return false;

        if (!make_host_buffer(16ull, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                              &X.fx.ibuf[i], &X.fx.imem[i], &X.fx.imap[i])) return false;
        uint32_t* d = (uint32_t*)X.fx.imap[i];
        d[0] = 6u; d[1] = count; d[2] = 0u; d[3] = 0u;
    }
    X.fx.count = count;
    X.fx.spawnCursor = 0;
    return true;
}

// Set layout, sampler and descriptor pool are shared by the particle and the weather pipelines; both
// can be armed independently, so whoever runs first creates them.
static bool vkfx_shared()
{
    if (X.fx.dsl && X.fx.sampler && X.fx.pool) return true;
    if (X.fx.failed) return false;
    VkDevice dev = dev_();
    if (!dev) return false;

    // Two storage buffers: the persistent particle state, and this frame's retail spawn records.
    // Their own layout/pool on purpose: the engine's global set0 has exactly one storage descriptor and
    // binding the particle buffers there would evict the cached sets.
    // 0/1 = particle state + spawns, 2 = batch texture, 3 = scene-depth copy (soft particles).
    VkDescriptorSetLayoutBinding dsb[4] = {};
    for (int i = 0; i < 4; ++i) {
        dsb[i].binding = (uint32_t)i;
        dsb[i].descriptorType = (i < 2) ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dsb[i].descriptorCount = 1;
        dsb[i].stageFlags = (i < 2) ? (VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT)
                                    : VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dslci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dslci.bindingCount = 4; dslci.pBindings = dsb;
    if (!chk("vkCreateDescriptorSetLayout(vkfx)", G.vkCreateDescriptorSetLayout(dev, &dslci, nullptr, &X.fx.dsl))) {
        X.fx.failed = true; return false;
    }
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 16.0f;
    if (!chk("vkCreateSampler(vkfx)", G.vkCreateSampler(dev, &sci, nullptr, &X.fx.sampler))) {
        X.fx.failed = true; return false;
    }
    VkDescriptorPoolSize dps[2] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (uint32_t)(4 * VKGEO_SLOT_COUNT) },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(2 * VKGEO_SLOT_COUNT) },
    };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = (uint32_t)(2 * VKGEO_SLOT_COUNT); dpci.poolSizeCount = 2; dpci.pPoolSizes = dps;
    if (!chk("vkCreateDescriptorPool(vkfx)", G.vkCreateDescriptorPool(dev, &dpci, nullptr, &X.fx.pool))) {
        X.fx.failed = true; return false;
    }
    return true;
}

static bool vkfx_ensure(unsigned count)
{
    if (X.fx.failed) return false;
    if (X.fx.ready) return vkfx_buffers(count);
    VkDevice dev = dev_();
    if (!dev || !vkfx_shared()) return false;

    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smvi.codeSize = sizeof(VKFX_VERT_SPV); smvi.pCode = VKFX_VERT_SPV;
    if (!chk("vkCreateShaderModule(vkfx vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.fx.vs))) { X.fx.failed = true; return false; }
    smvi.codeSize = sizeof(VKFX_FRAG_SPV); smvi.pCode = VKFX_FRAG_SPV;
    if (!chk("vkCreateShaderModule(vkfx fs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.fx.fs))) { X.fx.failed = true; return false; }
    smvi.codeSize = sizeof(VKFX_COMP_SPV); smvi.pCode = VKFX_COMP_SPV;
    if (!chk("vkCreateShaderModule(vkfx cs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.fx.cs))) { X.fx.failed = true; return false; }

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                0, (uint32_t)sizeof(VKFXPush) };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &X.fx.dsl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (!chk("vkCreatePipelineLayout(vkfx)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &X.fx.layout))) {
        X.fx.failed = true; return false;
    }
    if (!vkfx_buffers(count)) { X.fx.failed = true; return false; }

    // The descriptor sets never change after this: one buffer per in-flight slot.
    VkDescriptorSetLayout layouts[VKGEO_SLOT_COUNT] = {};
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) layouts[i] = X.fx.dsl;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = X.fx.pool; ai.descriptorSetCount = (uint32_t)VKGEO_SLOT_COUNT;
    ai.pSetLayouts = layouts;
    if (!chk("vkAllocateDescriptorSets(vkfx)", G.vkAllocateDescriptorSets(dev, &ai, X.fx.dset))) {
        X.fx.failed = true; return false;
    }
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) {
        VkDescriptorBufferInfo bi[2] = { { X.fx.pbuf[i], 0, VK_WHOLE_SIZE },
                                         { X.fx.sbuf[i], 0, VK_WHOLE_SIZE } };
        VkWriteDescriptorSet w[2] = {};
        for (int k = 0; k < 2; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = X.fx.dset[i]; w[k].dstBinding = (uint32_t)k; w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[k].pBufferInfo = &bi[k];
        }
        G.vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }

    X.fx.ready = true;
    VKWP1_LOG("[VKFX] ready: compute + indirect particle pipeline, %u bytes push constants, %u particles",
              (unsigned)sizeof(VKFXPush), count);
    return true;
}

// Pipeline is per render pass (resolution / sample tier), exactly like the vegetation pair.
static bool vkfx_pipeline()
{
    if (!X.fx.ready) return false;
    VkRenderPass rp = X.passes[X.curPass].rp;
    if (X.fx.pipe && X.fx.pass == rp) return true;
    if (X.fx.pipe) { G.vkDestroyPipeline(dev_(), X.fx.pipe, nullptr); X.fx.pipe = nullptr; }
    X.fx.pass = rp;

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = (VkSampleCountFlagBits)X.passes[X.curPass].samples;

    // Translucent fire: depth-tested (geometry occludes it) but never depth-writing, additive blend.
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState ba = {};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;

    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = X.fx.vs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = X.fx.fs; st[1].pName = "main";

    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vpi; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dy;
    gp.layout = X.fx.layout; gp.renderPass = rp; gp.subpass = 0;
    if (!chk("vkCreateGraphicsPipelines(vkfx)",
            G.vkCreateGraphicsPipelines(dev_(), nullptr, 1, &gp, nullptr, &X.fx.pipe))) {
        X.fx.pipe = nullptr; return false;
    }
    return true;
}

static bool vkfx_compute_pipeline()
{
    if (!X.fx.ready) return false;
    if (X.fx.cpipe) return true;
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = X.fx.cs;
    ci.stage.pName = "main";
    ci.layout = X.fx.layout;
    if (!chk("vkCreateComputePipelines(vkfx)", G.vkCreateComputePipelines(dev_(), VK_NULL_HANDLE, 1, &ci, nullptr, &X.fx.cpipe))) {
        X.fx.cpipe = nullptr; return false;
    }
    return true;
}

// One dispatch per frame, recorded OUTSIDE the pass: the compute writes the particle SSBO that the
// vertex stage reads a few draws later, and the memory barrier plus the pass close/reopen is what
// makes that ordering legal. The scene pass is LOAD/STORE, so reopening it loses nothing.
static bool vkfx_simulate(const VKFXPush& pc)
{
    if (X.fx.dispFrame == X.st.framesBegun) return true;
    if (!vkfx_compute_pipeline()) return false;
    X.fx.dispFrame = X.st.framesBegun;
    close_pass();
    G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.fx.cpipe);
    G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.fx.layout, 0, 1,
                              &X.fx.dset[X.slot], 0, nullptr);
    G.vkCmdPushConstants(X.cmd, X.fx.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(VKFXPush), &pc);
    G.vkCmdDispatch(X.cmd, (X.fx.count + 63u) / 64u, 1u, 1u);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    G.vkCmdPipelineBarrier(X.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                           0, 1, &mb, 0, nullptr, 0, nullptr);
    return open_pass();
}

static int vkfx_env_capacity()
{
    static int s_env = -2;      // -2 = not parsed, -1 = no env override
    if (s_env == -2) {
        const char* e = getenv("GENERALSX_VK_GPU_FX");
        s_env = (e != nullptr && *e != 0) ? atoi(e) : -1;
        if (s_env == 0) s_env = -1;
        if (s_env > 20000) s_env = 20000;
    }
    return s_env;
}

static unsigned vkfx_capacity()
{
    const int env = vkfx_env_capacity();
    if (env > 0) return (unsigned)env;
    return X.fx.cfgParticles ? 4096u : 0u;
}

static bool vkfx_armed()
{
    static bool s_logged = false;
    const unsigned cap = vkfx_capacity();
    if (cap > 0 && !s_logged) {
        s_logged = true;
        fprintf(stderr, "[VKFX] GPU particles ON (capacity %u)\n", cap);
        fflush(stderr);
    }
    return cap > 0;
}

// Shipped switch, pushed from the game layer every frame (Options.ini row / launcher). The env vars
// still win so scripted A/B legs keep working unchanged.
// B5: the only switch the MSAA path needs. Called from the device layer with the Options.ini value;
// GENERALSX_VK_MSAA still wins for QA legs.
void VKDraw_Set_MSAA(int samples)
{
    static int s_env = -2;
    if (s_env == -2) {
        const char* e = getenv("GENERALSX_VK_MSAA");
        s_env = (e != nullptr && *e != 0) ? atoi(e) : -1;
    }
    const int want = (s_env >= 0) ? s_env : samples;
    const unsigned v = (want >= 4) ? 4u : 1u;
    if (v != X.msaaSamples) VKWP1_LOG("[VKMSAA] samples %u -> %u", X.msaaSamples, v);
    X.msaaSamples = v;
}

void VKDraw_GPU_FX_Configure(int particles, int weatherType, int softParticles, int heatHaze)
{
    X.fx.cfgParticles = particles ? 1 : 0;
    X.fx.cfgWeather = weatherType;
    X.fx.cfgSoft = softParticles ? 1 : 0;
    if (heatHaze) X.fx.heatOn = 1;      // sticky: the env probe below still wins when it is set
}

// Retail spawn feed: the batch arrays are already in world space (position, size, RGBA), so this is a
// straight pack into the per-slot spawn SSBO. The batch texture rides along because the retail sprite's
// shape lives in its alpha, not in the quad. Returns true when the GPU path is armed, which is the
// caller's signal to skip the retail point-group draw for that batch (otherwise both would render).
bool VKDraw_GPU_FX_Spawn(const void* pos, const void* size, const void* rgba, unsigned count, void* d3dTex)
{
    if (!vkfx_armed() || !count) return vkfx_armed();
    if (!pos || !size || !rgba) return true;
    if (!vkfx_buffers(vkfx_capacity())) return true;
    float* base = (float*)X.fx.smap[X.slot];
    // A new frame starts a fresh spawn list. It must be cleared HERE, not at the end of the draw:
    // every host write lands before the submission, so an end-of-frame clear is what the GPU would
    // actually read when it runs this frame's dispatch.
    if (X.fx.spawnFrame != X.st.framesBegun) {
        X.fx.spawnFrame = X.st.framesBegun;
        X.fx.spawnCursor = 0;
        X.fx.texCur = nullptr;
        *(unsigned*)base = 0u;
    }
    if (d3dTex) {
        VKTex_Info info;
        if (VKTex_Query(d3dTex, &info) && info.view) {
            VkImageView v = info.view_srgb ? (VkImageView)info.view_srgb : (VkImageView)info.view;
            // One texture per frame: every additive batch in a frame shares the drawn set, so a second
            // distinct texture is a known gap (per-batch draw ranges are the fix).
            if (!X.fx.texCur) X.fx.texCur = v;
            else if (X.fx.texCur != v && X.fx.multiTex < 4) {
                ++X.fx.multiTex;
                fprintf(stderr, "[VKFX] second additive texture in one frame; using the first\n");
                fflush(stderr);
            }
        }
    }
    static float s_size = -1.0f;
    if (s_size < 0.0f) {
        const char* e = getenv("GENERALSX_VK_GPU_FX_SIZE");
        s_size = (e && *e) ? (float)atof(e) : 1.0f;
        if (s_size <= 0.0f) s_size = 1.0f;
    }
    const float* p = (const float*)pos;      // Vector3[count]
    const float* s = (const float*)size;     // float[count]
    const float* c = (const float*)rgba;     // Vector4[count] (rgb + alpha, 0..1)
    unsigned written = 0;
    for (unsigned i = 0; i < count && X.fx.spawnCursor < X.fx.count; ++i, ++written) {
        float* rec = base + 4 + (unsigned long long)X.fx.spawnCursor * 8ull;
        rec[0] = p[i * 3 + 0]; rec[1] = p[i * 3 + 1]; rec[2] = p[i * 3 + 2];
        rec[3] = (s[i] > 0.05f ? s[i] : 0.05f) * s_size;
        rec[4] = c[i * 4 + 0]; rec[5] = c[i * 4 + 1]; rec[6] = c[i * 4 + 2];
        rec[7] = c[i * 4 + 3];
        ++X.fx.spawnCursor;
    }
    *(unsigned*)base = X.fx.spawnCursor;      // header count; the dispatch consumes exactly this many
    X.fx.spawned += written;

    // A7: remember a few warm, opaque spawns as heat sources for the post-pass distortion.
    if (X.fx.heatOn < 0) { const char* e = getenv("GENERALSX_VK_HEAT"); X.fx.heatOn = (e && *e) ? atoi(e) : 0; }
    if (X.fx.heatOn && X.fx.spawnCursor == written) {   // first batch of this frame
        X.fx.heatCount = 0;
        for (unsigned i = 0; i < count && X.fx.heatCount < 4u; ++i) {
            const float r = c[i * 4 + 0], g = c[i * 4 + 1], b = c[i * 4 + 2], a = c[i * 4 + 3];
            if (a < 0.45f || r < 0.5f || r < g * 0.95f || r < b * 0.95f) continue;   // warm and visible
            bool tooClose = false;
            for (unsigned k = 0; k < X.fx.heatCount; ++k) {
                const float dx = X.fx.heat[k * 4 + 0] - p[i * 3 + 0];
                const float dy = X.fx.heat[k * 4 + 1] - p[i * 3 + 1];
                if (dx * dx + dy * dy < 400.0f) { tooClose = true; break; }
            }
            if (tooClose) continue;
            float* h = &X.fx.heat[X.fx.heatCount * 4];
            h[0] = p[i * 3 + 0]; h[1] = p[i * 3 + 1]; h[2] = p[i * 3 + 2];
            h[3] = (s[i] > 2.0f ? s[i] : 2.0f) * 1.6f;
            ++X.fx.heatCount;
        }
    }
    return true;
}


// A2 soft particles: the depth written so far has to become a *texture* before the particle pass may
// look at it (sampling the live attachment is a feedback loop). Every VKRT depth mirror already carries
// TRANSFER_SRC | SAMPLED, so this is a copy, not a new pipeline.
static bool vkfx_soft_enabled()
{
    static int s_soft = -1;
    if (s_soft < 0) {
        const char* e = getenv("GENERALSX_VK_SOFT");
        s_soft = (e != nullptr && *e != 0) ? atoi(e) : X.fx.cfgSoft;
        s_soft = s_soft ? 1 : 0;
    }
    return s_soft != 0;
}

static bool vkfx_soft_prepare()
{
    if (!vkfx_soft_enabled() || !X.depthKey) return false;
    VKRT_Info di;
    if (!VKRT_Query(X.depthKey, &di) || !di.image) return false;
    static int s_softKeyTag = 0;
    if (!X.fx.softKey) X.fx.softKey = (void*)&s_softKeyTag;   // identity key, never dereferenced
    if (X.fx.softW != di.width || X.fx.softH != di.height || X.fx.softFmt != di.format) {
        if (X.fx.softW) VKRT_Free(X.fx.softKey);
        X.fx.softView = nullptr;
        if (!VKRT_Alloc_Depth_Format(X.fx.softKey, di.width, di.height, di.format)) return false;
        VKRT_Info ci;
        if (!VKRT_Query(X.fx.softKey, &ci) || !ci.image) return false;
        X.fx.softView = (VkImageView)(ci.sample_view ? ci.sample_view : ci.view);
        X.fx.softW = di.width; X.fx.softH = di.height; X.fx.softFmt = di.format;
        for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) X.fx.softBound[i] = nullptr;
        VKWP1_LOG("[VKSOFT] depth copy %ux%u fmt=%u", di.width, di.height, di.format);
    }
    VKRT_Info ci;
    if (!VKRT_Query(X.fx.softKey, &ci) || !ci.image) return false;

    close_pass();
    VKRT_Cmd_Transition(X.cmd, X.depthKey, VKRT_LAYOUT_TRANSFER_SRC);
    VKRT_Cmd_Transition(X.cmd, X.fx.softKey, VKRT_LAYOUT_TRANSFER_DST);
    VkImageCopy rc = {};
    rc.srcSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    rc.srcSubresource.layerCount = 1;
    rc.dstSubresource = rc.srcSubresource;
    rc.extent = { di.width, di.height, 1 };
    G.vkCmdCopyImage(X.cmd, (VkImage)di.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     (VkImage)ci.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rc);
    VKRT_Cmd_Transition(X.cmd, X.depthKey, VKRT_LAYOUT_DEPTH_STENCIL_ATTACHMENT);
    VKRT_Cmd_Transition(X.cmd, X.fx.softKey, VKRT_LAYOUT_SHADER_READ_ONLY);
    return open_pass();
}

static bool vkfx_draw(unsigned instance_count, float time_seconds, float size_scale,
                      const float cam[3], const float right[3], const float up[3], const float fwd[3],
                      const float view_proj[16])
{
    if (!X.frameOpen || !X.cmd || !X.inPass || !X.depthKey) return false;
    if (!X.fx.texCur) return false;      // nothing to sample: the sprite shape lives in the batch texture
    if (!vkfx_ensure(instance_count) || !vkfx_pipeline()) return false;

    // Same source and layout as the vegetation pass (W3DGrassBuffer builds P*V from the camera and
    // emits it row-major). The DX8 transform state is NOT the matrix the scene actually draws with:
    // reading it here put every particle at negative clip w, i.e. behind the eye.
    VKFXPush pc = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc.vp[c * 4 + r] = view_proj[r * 4 + c];
    for (int c = 0; c < 3; ++c) { pc.right[c] = right[c]; pc.up[c] = up[c]; pc.fwd[c] = fwd[c]; pc.camPos[c] = cam[c]; }
    pc.right[3] = time_seconds;
    pc.up[3] = size_scale;
    pc.fwd[3] = 0.30f;
    // Wall-clock step, not the 30 Hz logic tick: the sim advances once per rendered frame.
    {
        const unsigned long long now = X.st.framesBegun;
        const long long d = (X.fx.dispFrame == ~0ull) ? 1 : (long long)(now - X.fx.dispFrame);
        float dt = (d > 0) ? (float)d * (1.0f / 60.0f) : (1.0f / 60.0f);
        if (dt > 0.10f) dt = 0.10f;
        pc.camPos[3] = dt;
    }
    const bool softOn = vkfx_soft_prepare();
    if (softOn) {
        static float s_band = -1.0f;
        if (s_band < 0.0f) {
            const char* e = getenv("GENERALSX_VK_SOFT_BAND");
            s_band = (e && *e) ? (float)atof(e) : 0.010f;
            if (s_band <= 0.0f) s_band = 0.010f;
        }
        pc.soft[0] = 1.0f; pc.soft[1] = s_band;
    }

    if (!vkfx_simulate(pc)) return false;
    if (softOn && X.fx.softView && X.fx.softBound[X.slot] != X.fx.softView) {
        VkDescriptorImageInfo ii = {};
        ii.sampler = X.fx.sampler;
        ii.imageView = X.fx.softView;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = X.fx.dset[X.slot]; w.dstBinding = 3; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        G.vkUpdateDescriptorSets(dev_(), 1, &w, 0, nullptr);
        X.fx.softBound[X.slot] = X.fx.softView;
    }

    VkViewport vpw = { 0.0f, 0.0f, (float)X.w, (float)X.h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { X.w, X.h } };
    G.vkCmdSetViewport(X.cmd, 0, 1, &vpw);
    G.vkCmdSetScissor(X.cmd, 0, 1, &sc);
    G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.fx.pipe);
    G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.fx.layout, 0, 1,
                              &X.fx.dset[X.slot], 0, nullptr);
    if (X.fx.tex[X.slot] != X.fx.texCur) {
        VkDescriptorImageInfo ii = {};
        ii.sampler = X.fx.sampler;
        ii.imageView = X.fx.texCur;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = X.fx.dset[X.slot]; w.dstBinding = 2; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; w.pImageInfo = &ii;
        G.vkUpdateDescriptorSets(dev_(), 1, &w, 0, nullptr);
        X.fx.tex[X.slot] = X.fx.texCur;
    }
    G.vkCmdPushConstants(X.cmd, X.fx.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, (uint32_t)sizeof(VKFXPush), &pc);
    G.vkCmdDrawIndirect(X.cmd, X.fx.ibuf[X.slot], 0, 1, 0);
    ++X.fx.draws;

    // A frame with no particle batches at all still has to hand the compute an empty list; the ring
    // head lives on the GPU and is never reset.
    if (X.fx.spawnFrame != X.st.framesBegun) {
        X.fx.spawnFrame = X.st.framesBegun;
        X.fx.spawnCursor = 0;
        *(unsigned*)X.fx.smap[X.slot] = 0u;
    }
    if ((X.fx.draws % 120u) == 1u)
        VKWP1_LOG("[VKFX] draws=%u spawned=%llu", X.fx.draws, X.fx.spawned);
    return true;
}

// Public entry (光影升级2.0 B1), called after the retail particle batches are flushed so the spawns
// recorded this frame are the ones simulated and drawn this frame.
void VKDraw_GPU_FX(const float cam[3], const float right[3], const float up[3], const float fwd[3],
                   const float view_proj[16])
{
    if (!vkfx_armed()) return;
    vkfx_draw(vkfx_capacity(), (float)(X.st.framesBegun % 200000ull) * 0.033f, 1.0f,
              cam, right, up, fwd, view_proj);
}

// ---------------------------------------------------------------------------
// B2 weather (rain / snow / dust). Same SSBO particle shape and descriptor layout as B1, but the
// positions live in a camera-relative box and the look is procedural, so it needs no textures.
// ---------------------------------------------------------------------------
struct VKWeatherPush {
    float vp[16];
    float right[4];
    float up[4];
    float fwd[4];
    float camPos[4];
    float box[4];
};

static int vkweather_type()
{
    static int s_env = -1;      // -1 = not parsed yet, 0 = no override
    static int s_last = -1;
    if (s_env < 0) {
        const char* e = getenv("GENERALSX_VK_WEATHER");
        s_env = 0;
        if (e && *e) {
            if (!strcmp(e, "rain") || !strcmp(e, "1")) s_env = 1;
            else if (!strcmp(e, "snow") || !strcmp(e, "2")) s_env = 2;
            else if (!strcmp(e, "dust") || !strcmp(e, "3")) s_env = 3;
        }
    }
    const int type = s_env ? s_env : X.fx.cfgWeather;   // env wins, Options.ini row otherwise
    if (type != s_last) {
        s_last = type;
        if (type > 0) {
            fprintf(stderr, "[VKWX] GPU weather type=%d\n", type);
            fflush(stderr);
        }
    }
    return type;
}

static unsigned vkweather_count()
{
    static int s_n = -1;
    if (s_n < 0) {
        const char* e = getenv("GENERALSX_VK_WEATHER_COUNT");
        s_n = (e && *e) ? atoi(e) : 1400;
        if (s_n < 64) s_n = 64;
        if (s_n > 20000) s_n = 20000;
    }
    return (unsigned)s_n;
}

static bool vkweather_ensure(unsigned count)
{
    if (X.fx.wfailed) return false;
    if (X.fx.wready) return true;   // capacity is fixed for the run; it comes from the env
    VkDevice dev = dev_();
    if (!dev || !vkfx_shared()) { X.fx.wfailed = true; return false; }

    VkShaderModuleCreateInfo smvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smvi.codeSize = sizeof(VKWX_VERT_SPV); smvi.pCode = VKWX_VERT_SPV;
    if (!chk("vkCreateShaderModule(vkwx vs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.fx.wvs))) { X.fx.wfailed = true; return false; }
    smvi.codeSize = sizeof(VKWX_FRAG_SPV); smvi.pCode = VKWX_FRAG_SPV;
    if (!chk("vkCreateShaderModule(vkwx fs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.fx.wfs))) { X.fx.wfailed = true; return false; }
    smvi.codeSize = sizeof(VKWX_COMP_SPV); smvi.pCode = VKWX_COMP_SPV;
    if (!chk("vkCreateShaderModule(vkwx cs)", G.vkCreateShaderModule(dev, &smvi, nullptr, &X.fx.wcs))) { X.fx.wfailed = true; return false; }

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                                0, (uint32_t)sizeof(VKWeatherPush) };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &X.fx.dsl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (!chk("vkCreatePipelineLayout(vkwx)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &X.fx.wlayout))) {
        X.fx.wfailed = true; return false;
    }

    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) {
        if (!make_host_buffer((unsigned long long)count * 48ull, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              &X.fx.wbuf[i], &X.fx.wmem[i], &X.fx.wmap[i])) { X.fx.wfailed = true; return false; }
        // age = -1 marks "not initialised yet"; the first dispatch scatters the field.
        float* p = (float*)X.fx.wmap[i];
        for (unsigned k = 0; k < count; ++k) { p[k * 12 + 3] = -1.0f; p[k * 12 + 11] = 0.35f; }

        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        ai.descriptorPool = X.fx.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &X.fx.dsl;
        if (!chk("vkAllocateDescriptorSets(vkwx)", G.vkAllocateDescriptorSets(dev, &ai, &X.fx.wset[i]))) {
            X.fx.wfailed = true; return false;
        }
        VkDescriptorBufferInfo bi = { X.fx.wbuf[i], 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w[2] = {};
        for (int k = 0; k < 2; ++k) {
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = X.fx.wset[i]; w[k].dstBinding = (uint32_t)k; w[k].descriptorCount = 1;
            w[k].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[k].pBufferInfo = &bi;
        }
        G.vkUpdateDescriptorSets(dev, 2, w, 0, nullptr);
    }
    X.fx.wcount = count;
    X.fx.wready = true;
    VKWP1_LOG("[VKWX] ready: %u weather particles, %u bytes push", count, (unsigned)sizeof(VKWeatherPush));
    return true;
}

static bool vkweather_pipelines()
{
    if (!X.fx.wready) return false;
    VkRenderPass rp = X.passes[X.curPass].rp;
    if (X.fx.wpipe && X.fx.wpass == rp && X.fx.wcpipe) return true;
    X.fx.wpass = rp;

    if (!X.fx.wcpipe) {
        VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = X.fx.wcs; ci.stage.pName = "main";
        ci.layout = X.fx.wlayout;
        if (!chk("vkCreateComputePipelines(vkwx)", G.vkCreateComputePipelines(dev_(), VK_NULL_HANDLE, 1, &ci, nullptr, &X.fx.wcpipe))) {
            X.fx.wcpipe = nullptr; return false;
        }
    }
    if (X.fx.wpipe) { G.vkDestroyPipeline(dev_(), X.fx.wpipe, nullptr); X.fx.wpipe = nullptr; }

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = (VkSampleCountFlagBits)X.passes[X.curPass].samples;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState ba = {};
    ba.blendEnable = VK_TRUE;
    ba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.colorBlendOp = VK_BLEND_OP_ADD;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    const VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = X.fx.wvs; st[0].pName = "main";
    st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = X.fx.wfs; st[1].pName = "main";
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.stageCount = 2; gp.pStages = st;
    gp.pVertexInputState = &vin; gp.pInputAssemblyState = &ia; gp.pViewportState = &vpi;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dy;
    gp.layout = X.fx.wlayout; gp.renderPass = rp; gp.subpass = 0;
    if (!chk("vkCreateGraphicsPipelines(vkwx)", G.vkCreateGraphicsPipelines(dev_(), nullptr, 1, &gp, nullptr, &X.fx.wpipe))) {
        X.fx.wpipe = nullptr; return false;
    }
    return true;
}

void VKDraw_GPU_Weather(const float cam[3], const float right[3], const float up[3], const float fwd[3],
                        const float view_proj[16])
{
    const int type = vkweather_type();
    if (type <= 0) return;
    if (!X.frameOpen || !X.cmd || !X.inPass || !X.depthKey) return;
    const unsigned count = vkweather_count();
    if (!vkweather_ensure(count) || !vkweather_pipelines()) return;
    const unsigned live = X.fx.wcount;

    VKWeatherPush pc = {};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc.vp[c * 4 + r] = view_proj[r * 4 + c];
    for (int c = 0; c < 3; ++c) { pc.right[c] = right[c]; pc.up[c] = up[c]; pc.fwd[c] = fwd[c]; pc.camPos[c] = cam[c]; }
    pc.right[3] = (float)(X.st.framesBegun % 200000ull) * 0.033f;
    pc.up[3] = (type == 1) ? 4.5f : ((type == 2) ? 0.9f : 18.0f);
    pc.fwd[3] = (float)(type - 1);          // shader speaks 0 rain / 1 snow / 2 dust
    {
        const unsigned long long now = X.st.framesBegun;
        const long long d = (X.fx.wdispFrame == ~0ull) ? 1 : (long long)(now - X.fx.wdispFrame);
        float dt = (d > 0) ? (float)d * (1.0f / 60.0f) : (1.0f / 60.0f);
        if (dt > 0.10f) dt = 0.10f;
        pc.camPos[3] = dt;
    }
    pc.box[0] = 320.0f; pc.box[1] = 260.0f; pc.box[2] = 320.0f;
    pc.box[3] = (type == 1) ? 1500.0f : ((type == 2) ? 70.0f : 25.0f);

    if (X.fx.wdispFrame != X.st.framesBegun) {
        X.fx.wdispFrame = X.st.framesBegun;
        close_pass();
        G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.fx.wcpipe);
        G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, X.fx.wlayout, 0, 1,
                                  &X.fx.wset[X.slot], 0, nullptr);
        G.vkCmdPushConstants(X.cmd, X.fx.wlayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(VKWeatherPush), &pc);
        G.vkCmdDispatch(X.cmd, (live + 63u) / 64u, 1u, 1u);
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        G.vkCmdPipelineBarrier(X.cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                               0, 1, &mb, 0, nullptr, 0, nullptr);
        if (!open_pass()) return;
    }

    VkViewport vpw = { 0.0f, 0.0f, (float)X.w, (float)X.h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { X.w, X.h } };
    G.vkCmdSetViewport(X.cmd, 0, 1, &vpw);
    G.vkCmdSetScissor(X.cmd, 0, 1, &sc);
    G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.fx.wpipe);
    G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.fx.wlayout, 0, 1,
                              &X.fx.wset[X.slot], 0, nullptr);
    G.vkCmdPushConstants(X.cmd, X.fx.wlayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, (uint32_t)sizeof(VKWeatherPush), &pc);
    G.vkCmdDraw(X.cmd, 6u, live, 0u, 0u);
    ++X.fx.wdraws;
}


bool VKDraw_Vegetation_Draw(const void* grass, unsigned grass_count,
                            const void* rocks, unsigned rock_count,
                            const float view_proj[16], const float camera[3], float time_seconds,
                            const float sun_dir[3], const float fog_color[3],
                            float fog_start, float fog_end, float cull_distance)
{
    if (!VKDraw_Ready() || !X.frameOpen || !X.cmd || !X.inPass) return false;
    if (!X.depthKey) return false;   // the vegetation pass needs the scene depth attachment
    if (!grass_count && !rock_count) return false;
    if ((grass_count && !grass) || (rock_count && !rocks)) return false;
    if (!vkveg_ensure() || !vkveg_pipelines()) return false;

    if (grass_count >= VKVEG_MAX_INSTANCES) { grass_count = VKVEG_MAX_INSTANCES; rock_count = 0; }
    if (grass_count + rock_count > VKVEG_MAX_INSTANCES)
        rock_count = VKVEG_MAX_INSTANCES - grass_count;

    const unsigned long long bytes = (unsigned long long)(grass_count + rock_count) * VKVEG_INSTANCE_BYTES;
    if (!vkveg_buffer(X.slot, bytes)) return false;

    unsigned char* dst = (unsigned char*)X.veg.map[X.slot];
    if (grass_count) memcpy(dst, grass, (size_t)grass_count * VKVEG_INSTANCE_BYTES);
    if (rock_count) memcpy(dst + (size_t)grass_count * VKVEG_INSTANCE_BYTES, rocks,
                           (size_t)rock_count * VKVEG_INSTANCE_BYTES);

    VKVegPush pc = {};
    // D3D row-major (v * view * proj) -> GLSL column-major (vp * v).
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            pc.vp[c * 4 + r] = view_proj[r * 4 + c];
    pc.cam[0] = camera[0]; pc.cam[1] = camera[1]; pc.cam[2] = camera[2]; pc.cam[3] = time_seconds;
    pc.sun[0] = sun_dir[0]; pc.sun[1] = sun_dir[1]; pc.sun[2] = sun_dir[2]; pc.sun[3] = 0.0f;
    pc.fog[0] = fog_color[0]; pc.fog[1] = fog_color[1]; pc.fog[2] = fog_color[2]; pc.fog[3] = fog_start;
    pc.params[0] = fog_end;
    pc.params[1] = cull_distance > 1.0f ? cull_distance : 1.0f;
    pc.params[2] = 0.0f; pc.params[3] = 0.0f;
    {
        const float sweep = DX8Wrapper::VK_Sun_Sweep();
        for (int c = 0; c < 3; ++c) {
            pc.sunCol[c] = s_envVec[1][c] * sweep;
            pc.skyCol[c] = s_envVec[2][c];
            pc.grdCol[c] = s_envVec[3][c];
        }
    }

    VkViewport vp = { 0.0f, 0.0f, (float)X.w, (float)X.h, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { X.w, X.h } };
    G.vkCmdSetViewport(X.cmd, 0, 1, &vp);
    G.vkCmdSetScissor(X.cmd, 0, 1, &sc);
    G.vkCmdPushConstants(X.cmd, X.veg.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                         0, (uint32_t)sizeof(VKVegPush), &pc);

    VkDeviceSize offset = 0;
    if (grass_count) {
        G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.veg.gpipe);
        G.vkCmdBindVertexBuffers(X.cmd, 0, 1, &X.veg.buf[X.slot], &offset);
        G.vkCmdDraw(X.cmd, 12u, grass_count, 0, 0);
    }
    if (rock_count) {
        offset = (VkDeviceSize)grass_count * VKVEG_INSTANCE_BYTES;
        G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.veg.rpipe);
        G.vkCmdBindVertexBuffers(X.cmd, 0, 1, &X.veg.buf[X.slot], &offset);
        G.vkCmdDraw(X.cmd, 24u, rock_count, 0, 0);
    }

    X.veg.draws++;
    X.veg.instances += grass_count + rock_count;
    return true;
}

// GeneralsX @bugfix Qoder 21/10/2026: a new map means a new world. The sun-fit ground estimate and
// the caster list describe the previous world; keeping the estimate made the fit start at the old
// terrain height and converge at 0.5% per recorded caster, so the new map rendered without ground
// shadows for a few seconds. Resources stay; only the per-world frame-window state is cleared.
void VKDraw_Shadow_Reset()
{
    X.shGroundInit = 0;
    X.shGroundZ = 0.0f;
    X.shHaveVP = 0;
    X.shViewport[0] = X.shViewport[1] = X.shViewport[2] = X.shViewport[3] = 0.0f;
    X.shFitSide = 0.0f;
    X.shLastHits = 0;
    X.casters.clear();
}

void VKDraw_Set_Cloud_Texture(void* d3dTex, float ox, float oy, float scale, float strength)
{
    VkImageView oldView = X.shCloudView;
    X.shCloudParams[0] = ox;
    X.shCloudParams[1] = oy;
    X.shCloudParams[2] = scale > 1e-9f ? scale : (1.0f / 315.0f);
    X.shCloudParams[3] = strength > 0.0f ? strength : 0.0f;
    if (!d3dTex || strength <= 0.0f) {
        X.shCloudView = nullptr;
    } else {
        VKTex_Info info;
        X.shCloudView = (VKTex_Query(d3dTex, &info) && info.view) ? (VkImageView)info.view : nullptr;
    }
    if (oldView != X.shCloudView) {
        // Once per transition: proves the receiver actually has a cloud map, not just a strength.
        fprintf(stderr, "[VKCLOUD] view=%p strength=%.2f scale=%.5f tex=%p\n",
            (void*)X.shCloudView, (double)X.shCloudParams[3], (double)X.shCloudParams[2], d3dTex);
        fflush(stderr);
    }
}

bool VKDraw_Cloud_Shadow_Active()
{
    return X.shCloudView != nullptr && X.shCloudParams[3] > 0.0f;
}

void VKDraw_Set_ShadowCaster(bool allowed)
{
    X.shCasterAllowed = allowed;
}

void VKDraw_Set_Shadow_Receiver(bool receive)
{
    X.shadowReceive = receive ? 1 : 0;
}

void VKDraw_Material_Bind(unsigned family, unsigned features, unsigned variant, unsigned pass,
                          unsigned long long pipelineKey)
{
    X.matFamily = family;
    X.matFeatures = features;
    X.matVariant = variant;
    X.matPass = pass;
    X.matPipeline = pipelineKey;
    // Phase 5B.3: prove the material-keyed path reached the cache. One line per distinct key, so the
    // log says which water kinds actually selected a pipeline by material instead of by leftover state.
    if (pipelineKey) {
        static unsigned long long seen[8] = {};
        static unsigned seenCount = 0;
        bool known = false;
        for (unsigned i = 0; i < seenCount; ++i)
            if (seen[i] == pipelineKey) { known = true; break; }
        if (!known && seenCount < 8u) {
            seen[seenCount++] = pipelineKey;
            fprintf(stderr, "[VKMAT] material pipeline key=%016llx family=%u variant=%u pass=%u\n",
                pipelineKey, family, variant, pass);
            fflush(stderr);
        }
    }
}

// 6B: graph-owned P6 bloom chain keys (declared here because both the barrier bridge and the post pass
// resolve them by name).
static char s_bloomKeys[5][8] = {};
static const char* const s_bloomNames[5] = { "bloom0", "bloom1", "bloom2", "bloom3", "bloom4" };

bool VKDraw_Graph_Barrier(const char* resource, int layout)
{
    if (!resource) return false;
    void* key = nullptr;
    // Named graph resources resolve to the frame targets, not to whatever target the recorder happens
    // to be on (during UI the recorder is on the overlay, and a sceneColor barrier must still name the
    // scene image).
    if (strcmp(resource, "sceneColor") == 0) key = X.sceneColorKey;
    else if (strcmp(resource, "sceneDepth") == 0) key = X.sceneDepthKey;
    else if (strcmp(resource, "shadowMap") == 0) key = X.shColorKey;
    else if (strcmp(resource, "shadowDepth") == 0) key = X.shDepthKey;
    else if (strcmp(resource, "overlay") == 0) key = X.overlayColorKey;
    else if (strcmp(resource, "waterReflection") == 0) key = X.waterReflectionKey;
    else {
        for (int i = 0; i < 5; ++i)
            if (strcmp(resource, s_bloomNames[i]) == 0) { key = s_bloomKeys[i]; break; }
    }
    if (!key) return false;
    if (layout < 0 || layout >= (int)VKRT_LAYOUT_COUNT) return false;

    // Phase 6B: if the frame is being recorded and no render pass is open, this is a real
    // vkCmdPipelineBarrier recorded by the graph (VKRT_Cmd_Transition derives the stage/access masks
    // from the tracked old layout and updates it). Attachments inside an open pass are transitioned by
    // the pass's initial/finalLayout, so those cases stay bookkeeping-only.
    if (X.frameOpen && X.cmd && !X.inPass) {
        return VKRT_Cmd_Transition(X.cmd, key, layout);
    }
    VKRT_Set_Layout(key, layout);
    return true;
}

bool VKDraw_Shadow_Apply(const float sun_dir[3])
{
    // The post fog's sun term reuses this frame's light matrix. Zero it on every exit path; the shader
    // treats an all-zero matrix as "no shadow map" and keeps visibility at 1.
    memset(X.shLightVP, 0, sizeof(X.shLightVP));
    if (!VKDraw_Ready() || !X.frameOpen || !X.cmd) {
        shadow_trace("not-ready", nullptr, (unsigned)(X.shEnvChecked + X.frameOpen), 0, 0u, 0.0f, 0.0f);
        return false;
    }
    if (!shadow_env_on() || !sun_dir) {
        shadow_trace("env-off", nullptr, (unsigned)(X.shOn + (sun_dir != nullptr)), 0, 0u, 0.0f, 0.0f);
        return false;
    }
    if (X.shFailed) {
        shadow_trace("sh-failed", nullptr, 0u, 0, 0u, 0.0f, 0.0f);
        return false;
    }

    float sun[3] = { sun_dir[0], sun_dir[1], sun_dir[2] };
    const float sl = sqrtf(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    if (sl < 1e-3f) {
        shadow_trace("sun-zero", nullptr, (unsigned)(sl * 1000.0f), 0, 0u, 0.0f, 0.0f);
        return false;
    }
    sun[0] /= sl; sun[1] /= sl; sun[2] /= sl;

    if (!X.shHaveVP || X.casters.empty()) {
        ++X.shFrames;
        if (X.casters.empty()) ++X.shNoCasterFrames;
        shadow_trace(X.casters.empty() ? "no-casters" : "no-vp", nullptr, 0u, 0, 0u, 0.0f, 0.0f);
        X.casters.clear();
        X.shHaveVP = 0;
        return false;
    }

    vk_queue_lock();
    if (!shadow_ensure()) {
        shadow_trace("ensure", nullptr, 0u, 0, 0u, 0.0f, 0.0f);
        vk_queue_unlock();
        return false;
    }

    void* const sceneColor = X.colorKey;
    void* const sceneDepthKey = X.depthKey;
    const unsigned sceneW = X.w, sceneH = X.h;
    VKRT_Info depthInfo;
    if (!sceneColor || !sceneDepthKey || !VKRT_Query(sceneDepthKey, &depthInfo) || !depthInfo.view) {
        shadow_trace("no-depth", nullptr, (unsigned)(sceneColor != nullptr) + (unsigned)(sceneDepthKey != nullptr),
                     0, 0u, 0.0f, 0.0f);
        X.casters.clear();
        X.shHaveVP = 0;
        vk_queue_unlock();
        return false;
    }

    float lightVP[16];
    float range = 1.0f, mapWorld = 1.0f;
    float fitLo[3], fitHi[3];
    if (!shadow_build_light(sun, X.shView, X.shProj, X.shGroundZ, lightVP, range, mapWorld, fitLo, fitHi)) {
        shadow_trace("fit", nullptr, 0u, 0, 0u, 0.0f, 0.0f);
        X.casters.clear();
        X.shHaveVP = 0;
        vk_queue_unlock();
        return false;
    }
    memcpy(X.shLightVP, lightVP, sizeof(lightVP));

    // Close the scene pass so the map can be rendered; every later pass reopens what it needs.
    close_pass();

    // Fit report: every recorded caster origin has to fall inside the box (lc in [-1,1] x [-1,1] x
    // [0,1]); an OUT here means the map misses geometry, while casters all inside but no on-screen
    // shadow points at the receiver instead.
    // GeneralsX @debug 28/10/2026 flicker forensic (GENERALSX_VKSHADOW_TRACE=1): one line per frame so a
    // camera move can be correlated with the fit's side/centre flipping. Bounded by run length.
    static int s_shTrace = -1;
    if (s_shTrace < 0) { const char* e = getenv("GENERALSX_VKSHADOW_TRACE"); s_shTrace = (e && *e && atoi(e)) ? 1 : 0; }
    if (s_shTrace) {
        fprintf(stderr, "[SHFIT] f=%llu side=%.0f cx=%.1f cy=%.1f casters=%u range=%.0f\n",
            X.shFrames, (double)mapWorld, (double)((fitLo[0] + fitHi[0]) * 0.5),
            (double)((fitLo[1] + fitHi[1]) * 0.5), (unsigned)X.casters.size(),
            (double)range);
        fflush(stderr);
    }
    if (X.shFrames < 2ull || (X.shFrames % 600ull) == 0ull) {
        auto lcOf = [&](const float p[3], float out[3]) {
            out[0] = lightVP[0] * p[0] + lightVP[4] * p[1] + lightVP[8] * p[2] + lightVP[12];
            out[1] = lightVP[1] * p[0] + lightVP[5] * p[1] + lightVP[9] * p[2] + lightVP[13];
            out[2] = lightVP[2] * p[0] + lightVP[6] * p[1] + lightVP[10] * p[2] + lightVP[14];
        };
        VKWP1_LOG("[VKSHADOW] fit: side=%.0f range=%.0f ground=%.1f casters=%u "
                  "lo=(%.0f %.0f %.0f) hi=(%.0f %.0f %.0f) sun=(%.3f %.3f %.3f)",
            (double)mapWorld, (double)range, (double)X.shGroundZ, (unsigned)X.casters.size(),
            (double)fitLo[0], (double)fitLo[1], (double)fitLo[2],
            (double)fitHi[0], (double)fitHi[1], (double)fitHi[2],
            (double)sun[0], (double)sun[1], (double)sun[2]);
        const size_t shown = X.casters.size() < 6 ? X.casters.size() : 6;
        for (size_t i = 0; i < shown; ++i) {
            const float* w = X.casters[i].world;
            const float p[3] = { w[12], w[13], w[14] };
            float lc[3];
            lcOf(p, lc);
            const bool out = lc[0] < -1.0f || lc[0] > 1.0f || lc[1] < -1.0f || lc[1] > 1.0f ||
                             lc[2] < 0.0f || lc[2] > 1.0f;
            VKWP1_LOG("[VKSHADOW] caster[%u] p=(%.0f %.0f %.0f) lc=(%.3f %.3f %.3f)%s",
                (unsigned)i, (double)p[0], (double)p[1], (double)p[2],
                (double)lc[0], (double)lc[1], (double)lc[2], out ? " OUT" : "");
        }
    }

    // ---- 1. sun-space depth map -------------------------------------------------
    X.colorKey = X.shColorKey;
    X.depthKey = X.shDepthKey;
    X.w = X.shMapSize; X.h = X.shMapSize;
    bool ok = open_pass();
    unsigned replayed = 0, replaySkipped = 0;
    if (ok) {
        const float clear[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        vkdraw_clear_locked(1, 1, clear, 1.0f, 0u, nullptr);
        VkViewport vp = { 0.0f, 0.0f, (float)X.shMapSize, (float)X.shMapSize, 0.0f, 1.0f };
        VkRect2D sc = { { 0, 0 }, { X.shMapSize, X.shMapSize } };
        G.vkCmdSetViewport(X.cmd, 0, 1, &vp);
        G.vkCmdSetScissor(X.cmd, 0, 1, &sc);

        struct ShadowPush {
            float lightVP[16];
            float world[16];
            float params[4];   // x = alpha reference (0..255), y = compare 0..7, z = cutout enable
        } push;
        memcpy(push.lightVP, lightVP, sizeof(push.lightVP));
        const VkShaderStageFlags castStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        // GeneralsX @debug Qoder 20/10/2026 mode 13: replay only the CPU-skinned casters, so the map shows
        // their silhouettes on their own.
        const bool skinOnly = shadow_debug_mode() == 13;
        for (size_t i = 0; i < X.casters.size(); ++i) {
            const VKShadowCaster& c = X.casters[i];
            if (skinOnly && !c.skin) continue;
            VkPipeline pso = shadow_cast_pso(c);
            if (!pso) { ++replaySkipped; continue; }
            memcpy(push.world, c.world, sizeof(push.world));
            push.params[0] = (float)c.alpha_ref;
            push.params[1] = (float)c.alpha_cmp;
            push.params[2] = c.alpha_enable ? 1.0f : 0.0f;
            push.params[3] = 0.0f;
            G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
            if (c.alpha_enable) {
                VkDescriptorSet texSet = shadow_cast_set(c.tex, c.samp);
                if (!texSet) { ++replaySkipped; continue; }
                G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.shCastAlphaLayout,
                                          0, 1, &texSet, 0, nullptr);
                G.vkCmdPushConstants(X.cmd, X.shCastAlphaLayout, castStages, 0,
                                     (uint32_t)sizeof(push), &push);
            } else {
                G.vkCmdPushConstants(X.cmd, X.shCastLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 128u, &push);
            }
            G.vkCmdBindVertexBuffers(X.cmd, 0, 1, &c.vb, &c.voff);
            G.vkCmdBindIndexBuffer(X.cmd, c.ib, c.ioff, VK_INDEX_TYPE_UINT16);
            G.vkCmdDrawIndexed(X.cmd, c.index_count, 1, 0, c.base_vertex, 0);
            ++replayed;
        }
        close_pass();
    }

    // ---- 1b. dynamic lights' own shadow maps (E3: up to four, one per light-list slot) -----------
    // This pass runs after the scene was recorded (W3DDisplay calls VKDraw_Shadow_Apply once the scene
    // draws are in), so the staged light list is complete. Each chosen light gets its own 1024^2 R32F map
    // rendered from the light's own position along its cone axis (straight down for a point light), with
    // light-space depth in [0,1]. The matrices go into the Forward+ block's fpLightShadow[] section, so a
    // receiver applies map s to light slot s with no lookup table. Opt-in:
    //   GENERALSX_VK_LIGHTSHADOW=1                  enable
    //   GENERALSX_VK_LIGHTSHADOW_MAPS=1..4          how many maps (default 4)
    for (int s = 0; s < (int)VkLighting::kShadowMaps; ++s) s_lsValid[s] = 0;
    memset(s_lsBlock, 0, sizeof(s_lsBlock));
    s_lsSelectCount = 0;
    {
        // GeneralsX @debug 24/10/2026 P17 D1: "no visible difference" and "never rendered" look identical
        // from a screenshot, so one line names every branch the light maps can take.
        static Int s_lsLog = -1;
        if (s_lsLog < 0) {
            const char* e = getenv("GENERALSX_VK_LIGHTSHADOW_LOG");
            s_lsLog = (e != nullptr && atoi(e) != 0) ? 1 : 0;
        }
        // GeneralsX @feature 24/10/2026 P34 E3 / @bugfix P35: the shadow pass runs after the 3D
        // interface scene called VK_Forward_Begin again, so the live list is that scene's (often empty,
        // but with the QA rig it can hold a handful of test cones). The world scene's list was snapshotted
        // during its own doRender; prefer it when it is from THIS frame and at least as large as the live
        // list. A frame with no world scene falls back to the live list.
        const bool lsUseSnap = (s_fpSnapCount > 0)
                            && (s_fpSnapFrame == (int)core_()->FrameIndex())
                            && (s_fpSnapCount >= s_fpLightCount);
        const int lsLightCount = lsUseSnap ? s_fpSnapCount : s_fpLightCount;
        const float (*lsLights)[8] = lsUseSnap ? s_fpSnapLights : s_fpLights;
        const float (*lsAux)[8] = lsUseSnap ? s_fpSnapAux : s_fpLightAux;
        int lsAlloc = 0, lsOpen = 0, lsBuilt = 0;
        unsigned lsCasters = 0;
        float lsAxis[3] = { 0.0f, 0.0f, -1.0f };
        float lsR = 0.0f;
        int lsPicked = 0;
        // GeneralsX @bugfix 24/10/2026 P34 E3: this used to require the *sun* replay count (`replayed`).
        // At night the sun is below the horizon, the sun pass replays nothing, and every dynamic light
        // lost its shadow exactly when lamps are the only light source. The caster list is collected by
        // the scene pass regardless of the sun, so that is the right precondition.
        if (light_shadow_on() && !X.casters.empty() && lsLightCount > 0) {
            if (s_lsMaps < 0) {
                const char* e = getenv("GENERALSX_VK_LIGHTSHADOW_MAPS");
                int v = (e != nullptr && *e != 0) ? atoi(e) : (int)VkLighting::kShadowMaps;
                if (v < 1) v = 1;
                if (v > (int)VkLighting::kShadowMaps) v = (int)VkLighting::kShadowMaps;
                s_lsMaps = v;
            }
            // Only the lights the receiver can actually see (the block's first `budget` slots) qualify.
            const int staged = (lsLightCount < s_fpBudget) ? lsLightCount : s_fpBudget;
            int maxMaps = (s_lsMaps < staged) ? s_lsMaps : staged;
            if (maxMaps > (int)VkLighting::kShadowMaps) maxMaps = (int)VkLighting::kShadowMaps;

            // ---- pick the lights: on screen, closest to the screen centre, one per fixture ----------
            // A fixture usually publishes several lights a few units apart (four lamps + four washes on
            // one building); a map per sibling would render the same silhouette twice, so anything within
            // 60 units of an already-picked light is skipped.
            int pick[VkLighting::kShadowMaps] = { -1, -1, -1, -1 };
            static float lsScore[VKFP_MAX_LIGHTS];
            {
                Matrix4x4 camView, camProj;
                DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, camView);
                DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, camProj);
                const Matrix4x4 camInv = camView.Inverse();
                const float cx = camInv[3].X, cy = camInv[3].Y, cz = camInv[3].Z;
                const Matrix4x4 vp = camView * camProj;
                for (int i = 0; i < staged; ++i) {
                    const float* p = lsLights[i];
                    const float reach = (p[7] > 1.0f) ? p[7] : 40.0f;
                    const Vector3 wp(p[0], p[1], p[2]);
                    const float wclip = wp.X * vp[0].W + wp.Y * vp[1].W + wp.Z * vp[2].W + vp[3].W;
                    float score;
                    int onScreen = 0;
                    if (wclip > 1e-4f) {
                        const float sx = (wp.X * vp[0].X + wp.Y * vp[1].X + wp.Z * vp[2].X + vp[3].X) / wclip;
                        const float sy = (wp.X * vp[0].Y + wp.Y * vp[1].Y + wp.Z * vp[2].Y + vp[3].Y) / wclip;
                        if (sx >= -1.0f && sx <= 1.0f && sy >= -1.0f && sy <= 1.0f) {
                            onScreen = 1;
                            score = sx * sx + sy * sy;      // 0 = dead centre
                        } else {
                            score = 2.0f + fabsf(sx) + fabsf(sy);
                        }
                    } else {
                        score = 1e6f;
                    }
                    if (!onScreen) {
                        const float dx = p[0] - cx, dy = p[1] - cy, dz = p[2] - cz;
                        float d = sqrtf(dx * dx + dy * dy + dz * dz) - reach;
                        if (d < 0.0f) d = 0.0f;
                        score += d * 1e-4f;
                    }
                    lsScore[i] = score;
                }
                for (int s = 0; s < maxMaps; ++s) {
                    int best = -1;
                    float bestS = 1e30f;
                    for (int i = 0; i < staged; ++i) {
                        if (!(lsScore[i] < 1e29f)) continue;         // already picked
                        bool sameFixture = false;
                        for (int k = 0; k < s; ++k) {
                            if (pick[k] < 0) continue;
                            const float dx = lsLights[i][0] - lsLights[pick[k]][0];
                            const float dy = lsLights[i][1] - lsLights[pick[k]][1];
                            const float dz = lsLights[i][2] - lsLights[pick[k]][2];
                            if (dx * dx + dy * dy + dz * dz < 3600.0f) { sameFixture = true; break; }
                        }
                        if (sameFixture) continue;
                        if (lsScore[i] < bestS) { bestS = lsScore[i]; best = i; }
                    }
                    if (best < 0) break;
                    pick[s] = best;
                    lsScore[best] = 1e30f;
                    ++lsPicked;
                }
            }
            // ---- one light-space map per picked slot ------------------------------------------------
            for (int s = 0; s < (int)VkLighting::kShadowMaps && pick[s] >= 0; ++s) {
                const int li = pick[s];
                const float lx = lsLights[li][0], ly = lsLights[li][1], lz = lsLights[li][2];
                const float lr = (lsLights[li][7] > 1.0f) ? lsLights[li][7] : 40.0f;
                // GeneralsX @bugfix 24/10/2026 P18/P19 D1: the map used to be rendered from a virtual lamp
                // ABOVE the light looking down (only overhangs ever shadowed); it now sits at the light and
                // looks along the light's own axis, with depth = distance in front of the lamp in [0,1]
                // (the receivers compare lc.z against the map in that range, and D = 2R put half the
                // caster volume at negative z, which Vulkan clips).
                float ax = 0.0f, ay = 0.0f, az = -1.0f;
                if (lsAux[li][6] > 0.5f) {   // VkLighting::kKindSpot
                    ax = lsAux[li][0]; ay = lsAux[li][1]; az = lsAux[li][2];
                }
                float alen = sqrtf(ax * ax + ay * ay + az * az);
                if (!(alen > 1e-4f)) { ax = 0.0f; ay = 0.0f; az = -1.0f; alen = 1.0f; }
                ax /= alen; ay /= alen; az /= alen;
                // Stable orthonormal basis around the axis; a near-vertical axis picks world X as the hint.
                float hx = 0.0f, hy = 1.0f, hz = 0.0f;
                if (fabsf(az) > 0.9f) { hx = 1.0f; hy = 0.0f; hz = 0.0f; }
                float rx = hy * az - hz * ay, ry = hz * ax - hx * az, rz = hx * ay - hy * ax;   // right = up x axis
                const float rlen = sqrtf(rx * rx + ry * ry + rz * rz);
                if (rlen > 1e-5f) { rx /= rlen; ry /= rlen; rz /= rlen; }
                else { rx = 1.0f; ry = 0.0f; rz = 0.0f; }
                const float bxv = ay * rz - az * ry, byv = az * rx - ax * rz, bzv = ax * ry - ay * rx;
                const float R = (lr > 40.0f) ? lr : 40.0f;   // ortho half-extent: the light's reach
                const float D = R;                           // depth range along the axis: 0..reach
                if (s == 0) { lsAxis[0] = ax; lsAxis[1] = ay; lsAxis[2] = az; lsR = R; }
                const bool alloc = VKRT_Alloc_Color_Format(s_lsKey[s], s_lsSize, s_lsSize,
                                                           (unsigned)VK_FORMAT_R32_SFLOAT) != 0
                                && VKRT_Alloc_Depth_Format(s_lsDepthKey[s], s_lsSize, s_lsSize,
                                                           (unsigned)VK_FORMAT_D32_SFLOAT);
                if (!alloc) break;
                ++lsAlloc;
                // Row-vector view: x' = (p - L).right, y' = (p - L).up, z' = (p - L).axis (0 at the light).
                const float view[16] = {
                    rx, ry, rz, 0.0f,
                    bxv, byv, bzv, 0.0f,
                    ax, ay, az, 0.0f,
                    -(lx * rx + ly * ry + lz * rz),
                    -(lx * bxv + ly * byv + lz * bzv),
                    -(lx * ax + ly * ay + lz * az), 1.0f
                };
                const float proj[16] = { 1.0f/R,0,0,0,  0,1.0f/R,0,0,  0,0,1.0f/D,0,  0,0,0,1 };
                shadow_mat_mul(s_lsVP[s], proj, view);
                s_lsPosRange[s][0] = lx; s_lsPosRange[s][1] = ly;
                s_lsPosRange[s][2] = lz; s_lsPosRange[s][3] = lr;
                s_lsSelectPos[s][0] = lx; s_lsSelectPos[s][1] = ly; s_lsSelectPos[s][2] = lz;

                X.colorKey = s_lsKey[s]; X.depthKey = s_lsDepthKey[s];
                X.w = s_lsSize; X.h = s_lsSize;
                const bool open = open_pass() != false;
                if (open) {
                    ++lsOpen;
                    // GeneralsX @debug 24/10/2026 P19 D1: the map itself is invisible from a screenshot,
                    // so the clear value doubles as a probe. 1.0 is the "nothing occludes here" value the
                    // receiver expects; forcing 0.0 makes every texel look like an occluder at the light.
                    float lsClear = 1.0f;
                    {
                        static float s_clearOverride = -1.0f;
                        if (s_clearOverride < 0.0f) {
                            const char* e = getenv("GENERALSX_VK_LIGHTSHADOW_CLEAR");
                            s_clearOverride = (e != nullptr && *e != 0) ? (float)atof(e) : 1.0f;
                            if (s_clearOverride < 0.0f) s_clearOverride = 0.0f;
                            if (s_clearOverride > 1.0f) s_clearOverride = 1.0f;
                        }
                        lsClear = s_clearOverride;
                    }
                    const float clear[4] = { lsClear, lsClear, lsClear, lsClear };
                    vkdraw_clear_locked(1, 1, clear, 1.0f, 0u, nullptr);
                    VkViewport vp2 = { 0.0f, 0.0f, (float)s_lsSize, (float)s_lsSize, 0.0f, 1.0f };
                    VkRect2D sc2 = { { 0, 0 }, { s_lsSize, s_lsSize } };
                    G.vkCmdSetViewport(X.cmd, 0, 1, &vp2);
                    G.vkCmdSetScissor(X.cmd, 0, 1, &sc2);
                    struct LightShadowPush {
                        float lightVP[16];
                        float world[16];
                        float params[4];
                    } lpush;
                    memcpy(lpush.lightVP, s_lsVP[s], sizeof(lpush.lightVP));
                    const VkShaderStageFlags castStages2 = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                    unsigned lReplayed = 0;
                    for (size_t i = 0; i < X.casters.size(); ++i) {
                        const VKShadowCaster& c = X.casters[i];
                        VkPipeline pso = shadow_cast_pso(c);
                        if (!pso) continue;
                        memcpy(lpush.world, c.world, sizeof(lpush.world));
                        lpush.params[0] = (float)c.alpha_ref;
                        lpush.params[1] = (float)c.alpha_cmp;
                        lpush.params[2] = c.alpha_enable ? 1.0f : 0.0f;
                        lpush.params[3] = 0.0f;
                        G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
                        if (c.alpha_enable) {
                            VkDescriptorSet texSet = shadow_cast_set(c.tex, c.samp);
                            if (!texSet) continue;
                            G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.shCastAlphaLayout,
                                                      0, 1, &texSet, 0, nullptr);
                            G.vkCmdPushConstants(X.cmd, X.shCastAlphaLayout, castStages2, 0,
                                                 (uint32_t)sizeof(lpush), &lpush);
                        } else {
                            G.vkCmdPushConstants(X.cmd, X.shCastLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, 128u, &lpush);
                        }
                        G.vkCmdBindVertexBuffers(X.cmd, 0, 1, &c.vb, &c.voff);
                        G.vkCmdBindIndexBuffer(X.cmd, c.ib, c.ioff, VK_INDEX_TYPE_UINT16);
                        G.vkCmdDrawIndexed(X.cmd, c.index_count, 1, 0, c.base_vertex, 0);
                        ++lReplayed;
                    }
                    close_pass();
                    s_lsValid[s] = lReplayed ? 1 : 0;
                    lsCasters += lReplayed;
                    if (s_lsValid[s]) ++lsBuilt;
                }
                // Publish the slot: four VP rows + (light xyz, gate). gate = reach when the map is live.
                for (int r = 0; r < 4; ++r) {
                    for (int c = 0; c < 4; ++c) s_lsBlock[s * 20 + r * 4 + c] = s_lsVP[s][r * 4 + c];
                }
                s_lsBlock[s * 20 + 16] = lx;
                s_lsBlock[s * 20 + 17] = ly;
                s_lsBlock[s * 20 + 18] = lz;
                s_lsBlock[s * 20 + 19] = s_lsValid[s] ? lr : 0.0f;
            }
            s_lsSelectCount = lsPicked;
        }
        // GeneralsX @debug 24/10/2026 P18 D1: one line per attempt names the branch, so "no visible change"
        // can be told apart from "never allocated / never opened / no casters".
        if (s_lsLog) {
            fprintf(stderr, "[VKLS] on=%d maps=%d picked=%d built=%d alloc=%d open=%d casters=%u/%u "
                            "lights=%d axis0=(%.2f,%.2f,%.2f) R=%.0f\n",
                (int)light_shadow_on(), s_lsMaps, lsPicked, lsBuilt, lsAlloc, lsOpen, lsCasters,
                (unsigned)X.casters.size(), lsLightCount,
                (double)lsAxis[0], (double)lsAxis[1], (double)lsAxis[2], (double)lsR);
            fflush(stderr);
        }
    }

    // ---- 2. screen-space receiver ------------------------------------------------
    X.colorKey = sceneColor;
    X.depthKey = nullptr;    // the receiver samples the depth, it must not be an attachment of this pass
    X.w = sceneW; X.h = sceneH;
    bool applied = false;
    const char* recvWhy = ok ? "recv-open" : "cast-open";
    if (ok && open_pass()) {
        recvWhy = "recv-pso";
        VKRT_Info mapInfo;
        VkPipeline pso = shadow_recv_pso();
        if (pso) {
            recvWhy = "recv-map";
            if (VKRT_Query(X.shColorKey, &mapInfo) && mapInfo.view) {
                recvWhy = "recv-inverse";
                float pv[16], invVP[16];
                shadow_mat_mul(pv, X.shProj, X.shView);
                if (shadow_mat_inverse(invVP, pv)) {
                    recvWhy = "recv-set";
                    if (shadow_update_set((VkImageView)mapInfo.view,
                                          (VkImageView)(depthInfo.sample_view ? depthInfo.sample_view : depthInfo.view),
                                          (VkImageView)X.shCloudView)) {
                        const float mapSize = (float)X.shMapSize;
                        const float texelWorld = mapWorld / mapSize;
                        const float safeRange = range > 1e-3f ? range : 1.0f;
                        const float lightSize = 0.05f;   // the reference's sun angular size
                        float searchUV = lightSize * safeRange * 0.35f / mapWorld;
                        if (searchUV < 2.0f / mapSize) searchUV = 2.0f / mapSize;
                        if (searchUV > 0.03f) searchUV = 0.03f;

                        struct RecvPush {
                            float invVP[16];
                            float lightVP[16];
                            float params[4];
                            float viewport[4];
                            float aux[4];
                            float cloud[4];
                        } push;
                        memcpy(push.invVP, invVP, sizeof(push.invVP));
                        memcpy(push.lightVP, lightVP, sizeof(push.lightVP));
                        memcpy(push.viewport, X.shViewport, sizeof(push.viewport));
                        push.aux[0] = texelWorld;   // normal-offset step (world units per map texel)
                        push.aux[1] = safeRange;
                        push.aux[2] = mapSize;
                        push.aux[3] = mapWorld;     // box world size, used for the PCSS radius ceilings
                        push.cloud[0] = X.shCloudParams[0];
                        push.cloud[1] = X.shCloudParams[1];
                        push.cloud[2] = X.shCloudParams[2];
                        push.cloud[3] = X.shCloudView ? X.shCloudParams[3] : 0.0f;
                        push.params[0] = (texelWorld * 2.2f + 0.12f) / safeRange;  // depth bias
                        push.params[1] = shadow_debug_mode() ? -(float)shadow_debug_mode() : shadow_strength();
                        push.params[2] = searchUV;
                        push.params[3] = safeRange * lightSize / mapWorld * shadow_penumbra();  // penumbra scale
                        VkViewport vp = { 0.0f, 0.0f, (float)sceneW, (float)sceneH, 0.0f, 1.0f };
                        VkRect2D sc = { { 0, 0 }, { sceneW, sceneH } };
                        G.vkCmdSetViewport(X.cmd, 0, 1, &vp);
                        G.vkCmdSetScissor(X.cmd, 0, 1, &sc);
                        G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);
                        G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.shRecvLayout,
                                                  0, 1, &X.shSets[X.slot], 0, nullptr);
                        G.vkCmdPushConstants(X.cmd, X.shRecvLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                             (uint32_t)sizeof(push), &push);
                        G.vkCmdDraw(X.cmd, 3, 1, 0, 0);
                        applied = true;
                        recvWhy = "applied";
                    }
                }
            }
        }
        close_pass();
    }

    // Put the recorder back on the full scene target: the UI and any later batch expect the depth
    // attachment the frame's earlier draws used.
    // A caster origin outside the light box is geometry the map cannot see even though the replay
    // submitted it, so the trace carries in/total next to applied.
    unsigned inBox = 0u;
    for (size_t i = 0; i < X.casters.size(); ++i) {
        const float* w = X.casters[i].world;
        const float p[3] = { w[12], w[13], w[14] };
        const float lx = lightVP[0] * p[0] + lightVP[4] * p[1] + lightVP[8] * p[2] + lightVP[12];
        const float ly = lightVP[1] * p[0] + lightVP[5] * p[1] + lightVP[9] * p[2] + lightVP[13];
        const float lz = lightVP[2] * p[0] + lightVP[6] * p[1] + lightVP[10] * p[2] + lightVP[14];
        if (lx >= -1.0f && lx <= 1.0f && ly >= -1.0f && ly <= 1.0f && lz >= 0.0f && lz <= 1.0f) ++inBox;
    }
    char traceDetail[64];
    snprintf(traceDetail, sizeof(traceDetail), "in=%u skipped=%u", inBox, replaySkipped);
    shadow_trace(recvWhy, traceDetail, 0u, applied ? 1 : 0, replayed, mapWorld, range);
    X.colorKey = sceneColor;
    X.depthKey = sceneDepthKey;
    X.w = sceneW; X.h = sceneH;

    ++X.shFrames;
    X.shCasterTotal += (unsigned long long)X.casters.size();
    X.shReplayDraws += replayed;
    if (X.shFrames - X.shFramesLogged >= 300ull) {
        X.shFramesLogged = X.shFrames;
        // Composition of the current frame's list: identity-world entries are the CPU-skinned meshes
        // (their vertices are already world space), alpha entries are the cutout foliage/fences.
        unsigned ident = 0u, alpha = 0u;
        for (size_t i = 0; i < X.casters.size(); ++i) {
            if (X.casters[i].alpha_enable) ++alpha;
            const float* w = X.casters[i].world;
            if (w[12] == 0.0f && w[13] == 0.0f && w[14] == 0.0f) ++ident;
        }
        VKWP1_LOG("[VKSHADOW] frames=%llu casters=%llu avg=%.1f replayed=%llu none=%llu range=%.1f side=%.1f "
                  "ground=%.1f strength=%.2f applied=%d ident=%u alpha=%u",
            X.shFrames, X.shCasterTotal,
            (double)X.shCasterTotal / (double)(X.shFrames ? X.shFrames : 1ull),
            X.shReplayDraws, X.shNoCasterFrames, (double)range, (double)mapWorld,
            (double)X.shGroundZ, (double)shadow_strength(), applied ? 1 : 0, ident, alpha);
    }
    X.casters.clear();
    X.shHaveVP = 0;
    vk_queue_unlock();
    return applied;
}

// ===========================================================================
// public: frame boundaries and present
// ===========================================================================

// The scene depth attachment format, picked once by asking the physical device what it will actually
// accept as an attachment.
//
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: a stencil-capable format comes first now.
// The engine's player-colour and shadow passes write the stencil buffer every frame, and the retired
// D3D9 device was created with D24S8, so D24_UNORM_S8_UINT is both the faithful choice and the one that
// makes those passes describable. D32_SFLOAT_S8_UINT is the wider fallback; a part that offers neither
// keeps D32_SFLOAT and the stencil passes stay on the refused path rather than drawing wrong.
static unsigned vkdraw_scene_depth_format()
{
    static unsigned cached = 0;
    if (cached) return cached;
    const unsigned candidates[4] = { (unsigned)VK_FORMAT_D24_UNORM_S8_UINT,
                                     (unsigned)VK_FORMAT_D32_SFLOAT_S8_UINT,
                                     (unsigned)VK_FORMAT_D32_SFLOAT,
                                     (unsigned)VK_FORMAT_D16_UNORM };
    const char* names[4] = { "D24_UNORM_S8_UINT", "D32_SFLOAT_S8_UINT", "D32_SFLOAT", "D16_UNORM" };
    VkFormatProperties fp = {};
    for (int i = 0; i < 4; ++i) {
        G.vkGetPhysicalDeviceFormatProperties(core_()->Phys(), (VkFormat)candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            cached = candidates[i];
            VKWP1_LOG("wedge5 scene depth format = %u (%s)", cached, names[i]);
            return cached;
        }
    }
    VKWP1_LOG("FAIL wedge5: no depth format usable as an attachment");
    return 0;
}

// Phase 6B: the graph asks for the exact targets the frame needs. The rules that decide them stay in
// the backend (HDR format, render scale, depth format, overlay policy), but the lifetime is graph-owned:
// it calls this once in its frame prologue, then allocates through VKDraw_Graph_Alloc_Target().
static void vkdraw_pull_p6_state()
{
    X.hdrOn = DX8Wrapper::VK_HDR_Enabled() ? 1 : 0;
    // GeneralsX @bugfix 23/10/2026 P6: exposure and the filmic curve belong to the HDR scene target.
    // The overlay can still exist with HDR off (3D-only AA, SSAA), and running ACES over an already
    // clamped LDR image was the "fake" look; without HDR headroom the parity path is the correct one.
    // GeneralsX @feature 27/10/2026: the game layer adds a per-time-of-day offset, so the volumetric
    // fog's night lift can be compensated without touching the lighting or the fog data.
    X.hdrExposure = X.hdrOn
        ? (DX8Wrapper::VK_HDR_Exposure() + DX8Wrapper::VK_TOD_Exposure())
        : 0.0f;
    X.hdrTonemap = X.hdrOn ? DX8Wrapper::VK_HDR_Tonemap() : 0;
    X.aaMode = DX8Wrapper::VK_Post_AA_Mode();
    X.renderScale = DX8Wrapper::VK_Render_Scale();
    X.overlayActive = 0;
    X.overlayFailed = 0;
}

static void vkdraw_resolve_scene_shape(unsigned& w, unsigned& h)
{
    VkExtent2D e = core_()->Extent();
    const unsigned baseW = X.displayW ? X.displayW : (X.sceneW ? X.sceneW : e.width);
    const unsigned baseH = X.displayH ? X.displayH : (X.sceneH ? X.sceneH : e.height);
    const unsigned scale = (X.renderScale > 100) ? (unsigned)X.renderScale : 100u;
    const unsigned rw = baseW ? (baseW * scale + 99u) / 100u : 0u;
    const unsigned rh = baseH ? (baseH * scale + 99u) / 100u : 0u;
    if (rw && rh) { w = rw; h = rh; }
    else if (e.width) { w = e.width; h = e.height; }
}

bool VKDraw_Graph_Target_Desc(unsigned* sceneW, unsigned* sceneH, unsigned* colorFormat,
                              unsigned* depthFormat, unsigned* overlayW, unsigned* overlayH,
                              unsigned* overlayFormat, int* overlayWanted)
{
    if (!VKDraw_Ready() || !core_() || !sceneW || !sceneH || !colorFormat || !depthFormat ||
        !overlayW || !overlayH || !overlayFormat || !overlayWanted) {
        return false;
    }
    vkdraw_pull_p6_state();
    unsigned w = 0, h = 0;
    vkdraw_resolve_scene_shape(w, h);
    const unsigned dFmt = vkdraw_scene_depth_format();
    if (!w || !h || !dFmt) return false;
    X.w = w; X.h = h;   // the graph and Begin_Frame must agree on the extent
    *sceneW = w; *sceneH = h;
    *colorFormat = X.hdrOn ? (unsigned)VK_FORMAT_R16G16B16A16_SFLOAT : (unsigned)VK_FORMAT_B8G8R8A8_UNORM;
    *depthFormat = dFmt;
    *overlayWanted = vkdraw_overlay_wanted() ? 1 : 0;
    *overlayW = X.displayW ? X.displayW : w;
    *overlayH = X.displayH ? X.displayH : h;
    *overlayFormat = (unsigned)VK_FORMAT_B8G8R8A8_UNORM;
    return true;
}

// 6B: P6 intermediates the graph must own. Bloom is the only real GPU resource in that chain; the
// film LUT is generated in vkpost.frag and auto exposure is computed from bloom4, so no separate image
// is created for either. count=0 lets the graph release the chain when bloom/auto-exposure is off.
bool VKDraw_Graph_Post_Target_Desc(unsigned* count, unsigned* width, unsigned* height, unsigned* format)
{
    if (!count || !width || !height || !format) return false;
    *count = 0;
    const bool want = !vk_parity_on() && X.w && X.h &&
        (DX8Wrapper::VK_Post_Bloom() > 0.0001f || (X.hdrOn && DX8Wrapper::VK_Auto_Exposure()));
    if (!want) return true;
    *format = X.hdrOn ? (unsigned)VK_FORMAT_R16G16B16A16_SFLOAT
                      : (unsigned)VK_FORMAT_B8G8R8A8_UNORM;
    unsigned sw = X.w, sh = X.h;
    for (int i = 0; i < 5; ++i) {
        sw = sw > 2u ? sw / 2u : 1u;
        sh = sh > 2u ? sh / 2u : 1u;
        width[i] = sw;
        height[i] = sh;
    }
    *count = 5u;
    return true;
}

// 6B: the water reflection image is created by WaterRenderObjClass through Create_Render_Target (the
// engine handle), then registered here. The descriptor is what turns it into a graph resource.
bool VKDraw_Graph_Water_Target_Desc(void** key, unsigned* width, unsigned* height, unsigned* format)
{
    if (!key || !width || !height || !format || !X.waterReflectionKey) return false;
    VKRT_Info info = {};
    if (!VKRT_Query(X.waterReflectionKey, &info) || !info.view || !info.width || !info.height)
        return false;
    X.waterReflectionW = info.width;
    X.waterReflectionH = info.height;
    X.waterReflectionFormat = info.format;
    *key = X.waterReflectionKey;
    *width = info.width;
    *height = info.height;
    *format = info.format;
    return true;
}

void VKDraw_Graph_Adopt_Target(const char* name, void* key)
{
    if (!name || !key || strcmp(name, "waterReflection") != 0) return;
    VKRT_Info info = {};
    if (!VKRT_Query(key, &info) || !info.view || !info.width || !info.height) {
        static int s_adoptFail = 0;
        if (s_adoptFail < 4) {
            ++s_adoptFail;
            VKWP1_LOG("[RG] water reflection adoption failed: key=%p query=%d", key, info.view ? 1 : 0);
        }
        return;
    }
    const bool changed = X.waterReflectionKey != key || X.waterReflectionW != info.width ||
        X.waterReflectionH != info.height || X.waterReflectionFormat != info.format;
    X.waterReflectionKey = key;
    X.waterReflectionW = info.width;
    X.waterReflectionH = info.height;
    X.waterReflectionFormat = info.format;
    if (changed) {
        VKWP1_LOG("[RG] adopted target waterReflection key=%p %ux%u fmt=%u",
            key, info.width, info.height, info.format);
    }
}

void VKDraw_Graph_Free_Target_Key(void* key)
{
    if (!key) return;
    if (key == X.waterReflectionKey) {
        X.waterReflectionKey = nullptr;
        X.waterReflectionW = X.waterReflectionH = X.waterReflectionFormat = 0;
    }
    VKRT_Free(key);
}

static void* vkdraw_target_key(const char* name)
{
    if (!name) return nullptr;
    if (strcmp(name, "sceneColor") == 0) return X.sceneColorKey;
    if (strcmp(name, "sceneDepth") == 0) return X.sceneDepthKey;
    if (strcmp(name, "overlay") == 0) return X.overlayColorKey;
    if (strcmp(name, "waterReflection") == 0) return X.waterReflectionKey;
    for (int i = 0; i < 5; ++i)
        if (strcmp(name, s_bloomNames[i]) == 0) return s_bloomKeys[i];
    return nullptr;
}

bool VKDraw_Graph_Alloc_Target(const char* name, unsigned width, unsigned height,
                               unsigned format, bool depth)
{
    if (!width || !height || !format) return false;
    void* key = vkdraw_target_key(name);
    if (!key) return false;
    return depth ? VKRT_Alloc_Depth_Format(key, width, height, format)
                 : VKRT_Alloc_Color_Format(key, width, height, format);
}

void VKDraw_Graph_Free_Target(const char* name)
{
    if (!name) return;
    void* key = vkdraw_target_key(name);
    if (!key) return;
    if (strcmp(name, "waterReflection") == 0) {
        X.waterReflectionKey = nullptr;
        X.waterReflectionW = X.waterReflectionH = X.waterReflectionFormat = 0;
    }
    VKRT_Free(key);
}

static bool vkdraw_validate_scene();

// The graph prologue runs before WW3D::Begin_Render publishes the final render resolution through
// VKDraw_Set_Scene_Extent(), so the very first frame after a resolution change can arrive with targets
// allocated for the previous shape. Repair that one transition here; steady-state lifetime stays
// graph-owned (the next PrepareTargets sees the published shape and reuses it).
static bool vkdraw_repair_scene_targets()
{
    const unsigned sceneFmt = X.hdrOn
        ? (unsigned)VK_FORMAT_R16G16B16A16_SFLOAT
        : (unsigned)VK_FORMAT_B8G8R8A8_UNORM;
    const unsigned dFmt = vkdraw_scene_depth_format();
    if (!dFmt) return false;
    if (!VKRT_Alloc_Color_Format(X.sceneColorKey, X.w, X.h, sceneFmt)) return false;
    if (!VKRT_Alloc_Depth_Format(X.sceneDepthKey, X.w, X.h, dFmt)) return false;
    if (vkdraw_overlay_wanted()) {
        const unsigned dw = X.displayW ? X.displayW : X.w;
        const unsigned dh = X.displayH ? X.displayH : X.h;
        if (!VKRT_Alloc_Color_Format(X.overlayColorKey, dw, dh, (unsigned)VK_FORMAT_B8G8R8A8_UNORM))
            return false;
    } else {
        VKRT_Free(X.overlayColorKey);
    }
    static int s_repairLog = 0;
    if (s_repairLog < 4) {
        ++s_repairLog;
        fprintf(stderr, "[RG] repaired scene targets after resolution publish: %ux%u\n", X.w, X.h);
        fflush(stderr);
    }
    return vkdraw_validate_scene();
}

// The graph allocated these before the frame opened; this only proves they are present at the shape the
// recorder is about to use. A missing/mismatched pair fails the frame instead of rendering undefined.
static bool vkdraw_validate_scene()
{
    VKRT_Info ci = {}, di = {};
    const unsigned sceneFmt = X.hdrOn
        ? (unsigned)VK_FORMAT_R16G16B16A16_SFLOAT
        : (unsigned)VK_FORMAT_B8G8R8A8_UNORM;
    const unsigned dFmt = vkdraw_scene_depth_format();
    if (!dFmt) return false;
    if (!VKRT_Query(X.sceneColorKey, &ci) || !ci.view || ci.width != X.w || ci.height != X.h ||
        ci.format != sceneFmt) {
        return false;
    }
    if (!VKRT_Query(X.sceneDepthKey, &di) || !di.view || di.width != X.w || di.height != X.h ||
        di.format != dFmt) {
        return false;
    }
    if (vkdraw_overlay_wanted()) {
        VKRT_Info oi = {};
        const unsigned dw = X.displayW ? X.displayW : X.w;
        const unsigned dh = X.displayH ? X.displayH : X.h;
        if (!VKRT_Query(X.overlayColorKey, &oi) || !oi.view || oi.width != dw || oi.height != dh)
            return false;
    }
    return true;
}

// GeneralsX @feature Qoder 19/10/2026 R-04: cumulative microseconds spent in the per-frame slot
// fence wait, reported on the VKDRAW stats line.
static long long s_vkdrawWaitUs = 0;

// GeneralsX @feature Qoder 19/10/2026 R-04 frame-cost decomposition: after the mirror dedup cut
// syncUs from 9.3 s to 3.1 s, ~60 ms/frame is still unattributed. These scopes measure the wall
// time of the three remaining queue-lock holders - per-draw submit, end-of-frame record/close and
// present - so the next session log names which one owns the frame budget. Accumulated totals only;
// no behaviour change.
static long long s_vkdrawSubmitUs = 0, s_vkdrawSubmitMaxUs = 0;
static unsigned long long s_vkdrawSubmitN = 0;
static long long s_vkdrawEndUs = 0, s_vkdrawPresentUs = 0;
// GeneralsX @feature Qoder 19/10/2026 R-04 second layer: submitUs=52 s of a 60 s session (68.6 us per
// draw) with endUs/presentUs in the tens of ms - the frame budget is inside VK_Draw_Submit. These four
// segment totals split it: note+lock, pass-open/PSO/vertex-bind, the range-guard scans, and the tail
// (fan expansion, constants, descriptor lookup, vkCmdDrawIndexed). Refused paths stop early and are not
// in the tail total; this session had skipped=0/bad=0 so nearly every draw runs to completion.
static long long s_subPreUs = 0, s_subPassUs = 0, s_subScanUs = 0, s_subTailUs = 0;
static long long s_frameDraws = 0, s_blankFrames = 0;
static unsigned long long s_scanSkips = 0ull;
// GeneralsX @performance Qoder 19/10/2026 R-05: the scan cache split of the non-skipped draws - entries
// answered from idxMax versus walks that had to (re)read the index bytes.
static unsigned long long s_idxCacheHits = 0ull, s_idxCacheScans = 0ull;
static unsigned s_unknownHandleWarns = 0u;
static inline long long vkdraw_now_us(std::chrono::steady_clock::time_point& prev)
{
    const auto n = std::chrono::steady_clock::now();
    const long long us = (long long)std::chrono::duration_cast<std::chrono::microseconds>(n - prev).count();
    prev = n;
    return us;
}
struct VKDraw_UsScope {
    const std::chrono::steady_clock::time_point t0;
    long long& tot; long long* mx;
    VKDraw_UsScope(long long& t, long long* m = nullptr) : t0(std::chrono::steady_clock::now()), tot(t), mx(m) {}
    ~VKDraw_UsScope() {
        const long long us = (long long)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count();
        tot += us; if (mx && us > *mx) *mx = us;
    }
};

bool VKDraw_Begin_Frame()
{
    if (!VKDraw_Ready()) return false;
    X.shCasterAllowed = true;
    // GeneralsX @performance Qoder 19/10/2026 R-05 L9: phase11 inner probe. B3_10 measured begin_scene at
    // 9-10 ms/frame while the slot fence it waits averages single-digit us in steady state, so the time
    // hides in one of the steps below. Each step is accumulated and dumped every 600 frames to
    // vkfps_phase11.log (same GENERALSX_VKFPS_PHASES gate as every other R-05 probe).
    static const bool v11On = [] {
        const char* e = getenv("GENERALSX_VKFPS_PHASES");
        return e != nullptr && atoi(e) != 0;
    }();
    static long long v11Calls = 0;
    static long long v11Head = 0, v11Fence = 0, v11Poll = 0, v11Pool = 0, v11Reap = 0, v11Rings = 0,
                     v11Cmd = 0, v11Close = 0, v11Scene = 0, v11Clear = 0, v11Tail = 0;
    auto v11Now = [] {
        return (long long)std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    const long long v11B0 = v11On ? v11Now() : 0;
    long long v11Bx = v11B0;
#define V11_MARK(acc) do { if (v11On) { const long long n_ = v11Now(); acc += n_ - v11Bx; v11Bx = n_; } } while (0)
    vk_queue_lock();
    const uint32_t slot = core_()->FrameSlot();
    X.slot = slot;
    const unsigned long long flabel = X.st.framesBegun + 1ull;
    s_frameDraws = 0;
    // A frame that never reaches VKDraw_Shadow_Apply (menu, loading screen) must not carry the previous
    // frame's casters into the next one.
    X.casters.clear();
    X.shHaveVP = 0;
    g_vkStepFrame.store(flabel);
    VkFence f = core_()->SlotFence(slot);
    VKSTEP(1);
    VKTRACE("frame %llu: wait fence slot %u", flabel, (unsigned)slot);
    // GeneralsX @feature Qoder 19/10/2026 R-04 timing: the slot fence is where the render thread
    // blocks on the GPU; if this dominates the frame, the pipeline is GPU-bound (or dragged into
    // it by the fence-synchronous texture mirror sharing the queue).
    V11_MARK(v11Head);
    const auto waitT0 = std::chrono::steady_clock::now();
    bool ok = chk("wedge5 waitFence", G.vkWaitForFences(dev_(), 1, &f, VK_TRUE, ~0ull)) &&
              chk("wedge5 resetFence", G.vkResetFences(dev_(), 1, &f));
    s_vkdrawWaitUs += (long long)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - waitT0).count();
    V11_MARK(v11Fence);
    VKTRACE("frame %llu: fence done ok=%d", flabel, ok ? 1 : 0);
    if (ok) {
        // GeneralsX @performance Qoder 19/10/2026 R-05 L10: the fence above is also the point where
        // this slot's PREVIOUS frame GPU timestamps become readable; fold them in before recording.
        l10_read(slot);
        // The fence above is what makes the wedge-7 scene sampler readable: a sample may only be consumed
        // by the Begin that reuses the slot it was recorded into, which is the wait that proves its copy
        // finished.
        VKSTEP(2);
        VKCore_Luma_Poll(slot);
        VKCore_Shot_Poll(slot);
        vkrt_probe_poll(slot);
        VKTRACE("frame %llu: luma polled", flabel);
        V11_MARK(v11Poll);
        // The slot's sets may only be recycled once its fence has passed - that is the whole reason the
        // pools are per-slot.
        VKSTEP(3);
        if (X.pool[slot]) G.vkResetDescriptorPool(dev_(), X.pool[slot], 0);
        if (X.shCastPool[slot]) G.vkResetDescriptorPool(dev_(), X.shCastPool[slot], 0);
        VKTRACE("frame %llu: pool reset", flabel);
        VKSTEP(4);
        X.sets[slot].clear();
        X.setIdx[slot].clear();   // R-05: the bucket indexes name the entries that just went away
        X.shCastSets[slot].clear();
        V11_MARK(v11Pool);
        // One more frame boundary retired, so the backings that were dropped while it was in flight are
        // now old enough that no submitted command buffer can still name them.
        VKSTEP(5);
        geo_reap();
        VKTRACE("frame %llu: geo reaped", flabel);
        V11_MARK(v11Reap);
        // Same rule for the constant rings: this slot's window is dead space now that its frame retired,
        // so start writing at the beginning of it again.
        VKSTEP(6);
        for (int i = 0; i < 3; ++i) const_ring_reset(X.cr[i], (int)slot);
        VKTRACE("frame %llu: rings reset", flabel);
        V11_MARK(v11Rings);
        VKSTEP(7);
        VkCommandBuffer cmd = core_()->SlotCmd(slot);
        ok = cmd != nullptr && chk("wedge5 resetCmd", G.vkResetCommandBuffer(cmd, 0));
        VKTRACE("frame %llu: cmd reset ok=%d", flabel, ok ? 1 : 0);
        if (ok) {
            VKSTEP(8);
            VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ok = chk("wedge5 beginCmd", G.vkBeginCommandBuffer(cmd, &bb));
            VKTRACE("frame %llu: cmd begin ok=%d", flabel, ok ? 1 : 0);
        }
        V11_MARK(v11Cmd);
        if (ok) {
            X.cmd = cmd;
            X.frameOpen = 1;
            X.inPass = 0;
            X.haveImage = 0;
            X.resizePending = 0;   // re-evaluated by this frame's acquire and present
            ++X.st.framesBegun;
            l10_frame_begin(slot);
            // GeneralsX @debug Qoder 20/10/2026 three-way A/B: 0 = retail decal/stencil only, 1 = native
            // only, 2 = both. Decided here so DoShadows() and the native receiver see the same value.
            static const bool abOn = [] {
                const char* e = getenv("GENERALSX_VKSHADOW_AB");
                return e != nullptr && atoi(e) != 0;
            }();
            static const unsigned long long abPeriod = [] {
                const char* a = getenv("GENERALSX_VKSHADOW_ALT");
                const int v = a ? atoi(a) : 0;
                return (unsigned long long)(v > 0 ? v : 10);
            }();
            X.abOn = abOn;
            if (abOn) {
                static const char* const abNames[3] = { "retail-only", "native-only", "both" };
                static unsigned long long s_abSeg = ~0ull;
                const unsigned long long abSeg = X.st.framesBegun / abPeriod;
                const unsigned mode = (unsigned)(abSeg % 3ull);
                X.abMode = mode;
                if (abSeg != s_abSeg) {
                    s_abSeg = abSeg;
                    fprintf(stderr, "[VKSHADOW] ab segment=%llu frame=%llu mode=%u (%s)\n",
                        abSeg, X.st.framesBegun, mode, abNames[mode]);
                    fflush(stderr);
                }
                DX8Wrapper::VK_Set_Retail_Shadow_Off(mode == 1u);
            } else {
                // Non-A/B runs follow the options row: native on mutes the retail sun shadows, off restores
                // them. GENERALSX_VKSHADOW=0 also keeps retail alive when QA force-disables the native pass.
                static const bool qaOff = [] {
                    const char* e = getenv("GENERALSX_VKSHADOW");
                    return e != nullptr && atoi(e) == 0;
                }();
                const bool nativeOn = DX8Wrapper::VK_Real_Shadow_Enabled() && !qaOff;
                DX8Wrapper::VK_Set_Retail_Shadow_Off(nativeOn);
            }

            // Every frame starts on the scene target, whatever render target the last one ended on.
            // The targets themselves were acquired by the FrameGraph prologue (see
            // VKDraw_Graph_Target_Desc); this only re-derives the same shape for the recorder.
            vkdraw_pull_p6_state();
            unsigned shapeW = 0, shapeH = 0;
            vkdraw_resolve_scene_shape(shapeW, shapeH);
            if (shapeW && shapeH) {
                X.w = shapeW; X.h = shapeH;
            } else {
                VkExtent2D e = core_()->Extent();
                if (e.width) { X.w = e.width; X.h = e.height; }
            }
            VKSTEP(9);
            close_pass();
            VKTRACE("frame %llu: pass closed", flabel);
            V11_MARK(v11Close);
            X.colorKey = X.sceneColorKey; X.depthKey = X.sceneDepthKey;
            VKSTEP(10);
            const bool sceneOk = vkdraw_validate_scene() || vkdraw_repair_scene_targets();
            V11_MARK(v11Scene);
            if (!sceneOk) {
                ok = false;
                if (X.openPassWarns < 4) {
                    ++X.openPassWarns;
                    VKWP1_LOG("wedge5 scene target failed at %ux%u - frame not recorded", X.w, X.h);
                }
            }
            // The clear the engine asked for before this frame existed belongs to this frame: apply it now,
            // while the scene target is standing and nothing has been drawn into it yet.
            else {
                static const bool dbgClear = getenv("GENERALSX_VK_FORCECLEAR") != nullptr;
                const bool hadPending = X.pendingClear != 0;
                if (X.pendingClear) {
                    X.pendingClear = 0;
                    vkdraw_clear_locked(X.pendingClearColor, X.pendingClearZStencil, X.pendingColor,
                                        X.pendingZ, X.pendingStencil, nullptr);
                    if (dbgClear && flabel < 400ull) {
                        fprintf(stderr, "[VKCLEAR] f=%llu pending c=%d zs=%d z=%.2f target=%ux%u\n",
                            flabel, X.pendingClearColor, X.pendingClearZStencil, (double)X.pendingZ, X.w, X.h);
                        fflush(stderr);
                    }
                }
                // GeneralsX @bugfix 23/10/2026: the resize path rebuilds the scene depth image; the
                // engine's parked clear normally covers it, but a race can leave the depth undefined and
                // every 3D fragment then fails DEPTH_LEQUAL while the UI still draws. Clear depth on the
                // first frame after an extent change, and clear colour only if the engine did not already
                // park one (a fog/clear colour must survive).
                if (X.forceClearOnBegin) {
                    X.forceClearOnBegin = 0;
                    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    vkdraw_clear_locked(hadPending ? 0 : 1, 1, black, 1.0f, 0, nullptr);
                    if (flabel < 400ull) {
                        fprintf(stderr, "[VKCLEAR] f=%llu resize clear c=%d target=%ux%u\n",
                            flabel, hadPending ? 0 : 1, X.w, X.h);
                        fflush(stderr);
                    }
                }
                // GeneralsX @debug Qoder 23/10/2026 fullscreen forensics: forced re-clear under the QA
                // switch. Kept separate from the production resize clear above.
                if (dbgClear) {
                    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
                    vkdraw_clear_locked(1, 1, black, 1.0f, 0, nullptr);
                    if (flabel < 400ull) {
                        fprintf(stderr, "[VKCLEAR] f=%llu forced c+z target=%ux%u\n", flabel, X.w, X.h);
                        fflush(stderr);
                    }
                }
            }
            V11_MARK(v11Clear);
            VKTRACE("frame %llu: scene ok", flabel);
        }
    }
    vk_queue_unlock();
    VKSTEP(0);
    VKTRACE("frame %llu: begin done ok=%d", flabel, ok ? 1 : 0);
    if (!ok) { ++X.st.framesFailed; X.frameOpen = 0; X.cmd = nullptr; }
    if (v11On) {
        V11_MARK(v11Tail);
        if (++v11Calls % 600 == 0) {
            FILE* fp11 = fopen("vkfps_phase11.log", "a");
            if (fp11) {
                const double inv = 1.0 / 600.0;
                const double sum = (double)(v11Head + v11Fence + v11Poll + v11Pool + v11Reap +
                                             v11Rings + v11Cmd + v11Close + v11Scene + v11Clear + v11Tail) * inv;
                fprintf(fp11, "[V11_11] frames=600 avg_us head=%.1f fence=%.1f polls=%.1f poolreset=%.1f georeap=%.1f"
                    " rings=%.1f cmd=%.1f closepass=%.1f ensure_scene=%.1f parkedclear=%.1f tail=%.1f total=%.1f\n",
                    (double)v11Head * inv, (double)v11Fence * inv, (double)v11Poll * inv,
                    (double)v11Pool * inv, (double)v11Reap * inv, (double)v11Rings * inv,
                    (double)v11Cmd * inv, (double)v11Close * inv, (double)v11Scene * inv,
                    (double)v11Clear * inv, (double)v11Tail * inv, sum);
                fflush(fp11);
                fclose(fp11);
            }
            v11Head = v11Fence = v11Poll = v11Pool = v11Reap = v11Rings = 0;
            v11Cmd = v11Close = v11Scene = v11Clear = v11Tail = 0;
        }
    }
#undef V11_MARK
    return ok;
}

bool VK_Draw_Submit(const VKDraw_Submit* s)
{
    if (!VKDraw_Ready() || !s || !X.frameOpen) return false;
    VKDraw_UsScope scUs(s_vkdrawSubmitUs, &s_vkdrawSubmitMaxUs);
    ++s_vkdrawSubmitN;
    ++X.submitTicks;
    auto tSeg = std::chrono::steady_clock::now();
    // GeneralsX @feature Qoder 18/10/2026 wedge 8 path census: count the attempt before any refusal, so
    // the per-tag enter/ok gap names the family that is not reaching the screen under VKPRESENT.
    VKPgm_Note_Draw(s->key.vs_tag, s->key.ps_tag, 0, s->texture_views[0] != nullptr);
    VKTRACE("draw %llu: enter fvf=%08X topo=%u polys=%u idxcap=%u",
        X.submitTicks, s->key.fvf, s->key.topology, s->polygon_count, s->index_count);
    vk_queue_lock();
    s_subPreUs += vkdraw_now_us(tSeg);

    if (!X.cmd) { vk_queue_unlock(); ++X.st.drawsSkipped; return false; }

    if (!open_pass()) { vk_queue_unlock(); ++X.st.drawsSkipped; return false; }

    VKDraw_Submit mut = *s;
    // P6 receive-shadow stencil mask. Every depth-tested scene draw stamps bit 2 (1 = receive,
    // 0 = explicitly excluded); the native shadow receiver tests that bit. The engine keeps 0x80 and
    // bits 3..6 for volumetric occluders/player colour, so the two owners no longer alias. Only draws
    // that do not already use stencil are touched, so selection/player-colour state stays untouched.
    if (mut.key.depth_test && !mut.key.stencil_test) {
        mut.key.stencil_test = 1;
        mut.key.stencil_func = 8;          // D3DCMP_ALWAYS
        mut.key.stencil_ref = X.shadowReceive ? VK_SHADOW_RECEIVE_BIT : 0u;
        mut.key.stencil_mask = VK_SHADOW_RECEIVE_BIT;
        mut.key.stencil_write_mask = VK_SHADOW_RECEIVE_BIT;
        mut.key.stencil_fail = 1;          // D3DSTENCILOP_KEEP
        mut.key.stencil_zfail = 1;         // KEEP
        mut.key.stencil_pass = X.shadowReceive ? 3 : 2;   // REPLACE 1 / ZERO
    }
    mut.key.rt_format = 0; mut.key.depth_format = 0;    // Terrain attribution: tag 118 is emitted by the heightmap pass, but the visible ground may be a
    // P6 SSAA: the engine still publishes viewports in display pixels, while the scene mirror is
    // render-scaled. Only the 3D scene is scaled; the UI runs after the resolve into the display-sized
    // overlay and must keep native coordinates.
    if (!X.overlayActive && X.renderScale > 100) {
        const unsigned sc = (unsigned)X.renderScale;
        mut.viewport[0] = (mut.viewport[0] * (float)sc) / 100.0f;
        mut.viewport[1] = (mut.viewport[1] * (float)sc) / 100.0f;
        mut.viewport[2] = (mut.viewport[2] * (float)sc) / 100.0f;
        mut.viewport[3] = (mut.viewport[3] * (float)sc) / 100.0f;
        mut.scissor[0] = (mut.scissor[0] * (float)sc) / 100.0f;
        mut.scissor[1] = (mut.scissor[1] * (float)sc) / 100.0f;
        mut.scissor[2] = (mut.scissor[2] * (float)sc) / 100.0f;
        mut.scissor[3] = (mut.scissor[3] * (float)sc) / 100.0f;
    }
    // different target. Name the target/viewport for the first few terrain submits so a color test that
    // never appears can be attributed to "drawn offscreen" instead of "shader ignored".
    if (mut.key.ps_tag >= 110u && mut.key.ps_tag <= 118u) {
        static int s_terrainTargetLog = 0;
        if (s_terrainTargetLog < 10) {
            ++s_terrainTargetLog;
            fprintf(stderr, "[TERRTGT] tag=%u color=%p scene=%p depth=%p vp=%d,%d %dx%d target=%ux%u\n",
                mut.key.ps_tag, X.colorKey, X.sceneColorKey, X.depthKey,
                (int)mut.viewport[0], (int)mut.viewport[1], (int)mut.viewport[2], (int)mut.viewport[3], X.w, X.h);
            fflush(stderr);
        }
    }

    VKRT_Info ci;
    if (!VKRT_Query(X.colorKey, &ci)) { vk_queue_unlock(); ++X.st.drawsSkipped; return false; }
    mut.key.rt_format = ci.format;
    if (X.depthKey) { VKRT_Info di; if (VKRT_Query(X.depthKey, &di)) mut.key.depth_format = di.format; }

    // GeneralsX @bugfix Qoder 20/10/2026: a viewport smaller than the target is legal in D3D and in
    // Vulkan, and the in-game tactical view relies on it - the camera viewport is the display minus
    // the control bar (0,0 2560x1152 inside a 2560x1440 target). Forcing such a viewport to the full
    // target stretched the 3D world vertically while the CPU-side pick ray and health bars still
    // mapped pixels over the 1152-high view, so clicks had to land above the rendered unit and bars
    // floated above it. Only trim what overflows the target, which is the case a stale full-screen
    // viewport left behind by a shadow or offscreen pass actually needs.
    if (mut.viewport[0] >= X.w) mut.viewport[0] = 0;
    if (mut.viewport[1] >= X.h) mut.viewport[1] = 0;
    if (mut.viewport[2] > X.w - mut.viewport[0]) mut.viewport[2] = X.w - mut.viewport[0];
    if (mut.viewport[3] > X.h - mut.viewport[1]) mut.viewport[3] = X.h - mut.viewport[1];

    // GeneralsX @feature Qoder 20/10/2026 Forward+: build the tile block for the camera that is about to
    // draw. GeneralsX @bugfix Qoder 24/10/2026 P4 D2: this used to run only for the first draw into the
    // scene target each frame, whatever camera that draw used. In the shell menu the first such draw is a
    // 2D UI batch (orthographic projection, identity view): every light projected outside the tile grid,
    // the block was cached with zero clusters, and no surface was lit for the rest of the frame. Key the
    // block on the actual view+projection now, skip orthographic draws (they never read the block), and
    // rebuild whenever any of frame/camera/list changed.
    if (X.fpBuf && X.colorKey == (void*)X.sceneColorKey && X.depthKey == (void*)X.sceneDepthKey) {
        Matrix4x4 fv, fpj;
        DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, fv);
        DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, fpj);
        const bool perspective = fpj[3][2] != 0.0f;   // w_clip = z_view only for a perspective matrix
        const bool camChanged = !X.fpBuiltCam
            || memcmp(X.fpBuiltView, &fv, sizeof(fv)) != 0
            || memcmp(X.fpBuiltProj, &fpj, sizeof(fpj)) != 0;
        if (perspective && (X.fpBuiltFrame != core_()->FrameIndex() || camChanged || s_fpDirty)) {
            fp_build_block(core_()->FrameSlot(), mut.viewport, X.w, X.h);
            memcpy(X.fpBuiltView, &fv, sizeof(fv));
            memcpy(X.fpBuiltProj, &fpj, sizeof(fpj));
            X.fpBuiltCam = 1;
        }
    }

    VkPipeline pso = pso_for(&mut);
    if (!pso) { vk_queue_unlock(); ++X.st.drawsSkipped; return false; }
    G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pso);

    VkViewport vp = { (float)mut.viewport[0], (float)mut.viewport[1],
                      (float)mut.viewport[2], (float)mut.viewport[3], 0.0f, 1.0f };
    G.vkCmdSetViewport(X.cmd, 0, 1, &vp);
    VkRect2D sc = { { (int32_t)mut.scissor[0], (int32_t)mut.scissor[1] },
                    { mut.scissor[2] > mut.scissor[0] ? mut.scissor[2] - mut.scissor[0] : X.w,
                      mut.scissor[3] > mut.scissor[1] ? mut.scissor[3] - mut.scissor[1] : X.h } };
    G.vkCmdSetScissor(X.cmd, 0, 1, &sc);

    // GeneralsX @bugfix Qoder 23/10/2026 fullscreen forensics: one line per frame for the first draw into
    // the scene target. Viewport/scissor/depth/mask decide whether a batch can reach a pixel at all, and
    // the fullscreen-only black battlefield needs that answer separated from "geometry was never submitted".
    {
        static const bool s_subDbg = getenv("GENERALSX_VK_FORCECLEAR") != nullptr;
        static unsigned long long s_subDbgFrame = ~0ull;
        if (s_subDbg && s_subDbgFrame != X.st.framesBegun
            && X.colorKey == (void*)X.sceneColorKey && X.depthKey == (void*)X.sceneDepthKey) {
            s_subDbgFrame = X.st.framesBegun;
            if (X.st.framesBegun < 400ull) {
                fprintf(stderr,
                    "[VKSUB] f=%llu tag=%u/%u fvf=%08X vp=%d,%d %dx%d sc=%d,%d %dx%d target=%ux%u "
                    "mask=%u z=%u/%u/%u blend=%u lt=%u\n",
                    X.st.framesBegun, mut.key.vs_tag, mut.key.ps_tag, mut.key.fvf,
                    (int)mut.viewport[0], (int)mut.viewport[1], (int)mut.viewport[2], (int)mut.viewport[3],
                    sc.offset.x, sc.offset.y, (int)sc.extent.width, (int)sc.extent.height, X.w, X.h,
                    mut.key.write_mask, mut.key.depth_test, mut.key.depth_write, mut.key.depth_cmp,
                    mut.key.blend_enable, DX8Wrapper::VK_Read_RS(D3DRS_LIGHTING));
                fflush(stderr);
            }
        }
    }

    // LINELOOP has no Vulkan topology and no index-expansion path here. D3D9 has no LINELOOP either
    // (d3d9types.h ends the enum at the fan), so this branch is a guard against a caller inventing one;
    // the wedge-0 census measured only triangle lists and strips.
    if (mut.key.topology > D3D_PT_TRIANGLEFAN || mut.key.topology < D3D_PT_POINTLIST) {
        vk_queue_unlock(); ++X.st.drawsSkipped; ++X.st.topologyRefusals;
        if (X.topologyWarns < 4) {
            ++X.topologyWarns;
            VKWP1_LOG("wedge5 draw refused: topology %u out of range (fvf=%08X)", mut.key.topology, mut.key.fvf);
        }
        return false;
    }

    if (mut.stream_count == 0 || !mut.streams[0].vk_buffer) {
        vk_queue_unlock(); ++X.st.drawsSkipped;
        if (X.streamWarns < 4) {
            ++X.streamWarns;
            VKWP1_LOG("wedge5 draw refused: vertex stream 0 has no backing (fvf=%08X)", mut.key.fvf);
        }
        return false;
    }
    // The facade feeds one vertex stream, which is what every FVF the census measured uses. A second
    // stream would be bound by D3D9 and dropped here, so it is logged rather than silently ignored.
    if (mut.stream_count > 1 && X.streamWarns < 4) {
        ++X.streamWarns;
        VKWP1_LOG("wedge5 note: %u vertex streams, only stream 0 is bound (fvf=%08X)",
            mut.stream_count, mut.key.fvf);
    }
    const VkBuffer vb = (VkBuffer)mut.streams[0].vk_buffer;
    const VkDeviceSize voff = (VkDeviceSize)mut.streams[0].offset;
    // Wedge 6: the fixed-function pipeline declares input binding 1 - the standing default vertex that
    // completes whatever the batch's FVF leaves out - so it has to be bound even though no stream of the
    // batch feeds it. A binding a pipeline names and a draw leaves unbound is undefined behaviour, not a
    // zero read, which is why the dummy buffer is a real allocation rather than an alias of stream 0.
    if (X.ffVs && mut.ff) {
        const VkBuffer     bnds[2] = { vb, X.dummyVb };
        const VkDeviceSize offs[2] = { voff, 0 };
        G.vkCmdBindVertexBuffers(X.cmd, 0, 2, bnds, offs);
    } else {
        G.vkCmdBindVertexBuffers(X.cmd, 0, 1, &vb, &voff);
    }
    s_subPassUs += vkdraw_now_us(tSeg);

    if (!mut.polygon_count) { vk_queue_unlock(); ++X.st.drawsSkipped; return false; }
    unsigned indexCount = vk_index_count_for(mut.key.topology, mut.polygon_count);
    // The facade reports how many indices exist from index_offset; asking for more than that is an engine
    // out-of-range read that D3D9 guarded, so it stays a refusal here too.
    if (mut.index_count && mut.index_count < indexCount) {
        vk_queue_unlock(); ++X.st.drawsSkipped;
        if (X.indexWarns < 4) {
            ++X.indexWarns;
            VKWP1_LOG("wedge5 draw refused: needs %u indices, backing has %u (topo=%u polys=%u)",
                indexCount, mut.index_count, mut.key.topology, mut.polygon_count);
        }
        return false;
    }
    if (!mut.vk_index_buffer) {
        vk_queue_unlock(); ++X.st.drawsSkipped;
        if (X.indexWarns < 4) {
            ++X.indexWarns;
            VKWP1_LOG("wedge5 draw refused: index buffer has no backing");
        }
        return false;
    }
    VkBuffer bindIb = (VkBuffer)mut.vk_index_buffer;
    VkDeviceSize ioff = (VkDeviceSize)mut.index_offset + (VkDeviceSize)(mut.start_index * 2u);

    // ---------------------------------------------------------------------------
    // Range guard. D3D9 clipped a read that ran past the end of a resource; Vulkan does not - it faults
    // and the fault takes the whole device with it. Every batch is therefore checked against the backing
    // it actually binds before the draw is recorded, and the vertex side is checked against the index
    // values themselves, which are in host-visible memory this layer wrote, so the largest one is known
    // here rather than discovered by the GPU. A batch that fails is refused and named: the D3D9 call the
    // facade falls back to still draws it, and the log says which buffer was too small.
    // ---------------------------------------------------------------------------
    {
        const unsigned long long srcIoff = mut.index_offset + (unsigned long long)mut.start_index * 2ull;
        // How far the index read reaches: the drawn range for a list or strip, and for a fan the source
        // needs polygon_count+2 entries because the expansion walks (0, i+1, i+2).
        const unsigned long long idxWords = (mut.key.topology == D3D_PT_TRIANGLEFAN)
            ? (unsigned long long)mut.polygon_count + 2ull : (unsigned long long)indexCount;
        const unsigned long long ibCap = geo_cap_of(bindIb);
        const unsigned long long vbCap = geo_cap_of(vb);
        const unsigned long long stride = mut.streams[0].stride;
        unsigned long long bad = 0;
        if (!ibCap || srcIoff + idxWords * 2ull > ibCap) bad = 1;
        else if (!vbCap || !stride) bad = 2;
        else if (voff >= vbCap) bad = 3;

        unsigned maxIndex = 0;
        if (!bad) {
            // An attribute that reads past its own vertex would read past the last vertex of the batch even
            // when the index maths is in range, so the layout is checked against the stride first.
            for (unsigned i = 0; i < mut.attr_count; ++i) {
                const unsigned long long end =
                    (unsigned long long)mut.attrs[i].offset + vk_attr_bytes(mut.attrs[i].type, mut.attrs[i].components);
                if (end > stride) { bad = 6; break; }
            }
        }
        if (!bad) {
            // The fan scratch ring is written by this function below, not read from a mapping here, so only
            // the source buffer is inspected; a user-memory draw's generated indices are 0..n by
            // construction, which the branch still verifies.
            // GeneralsX @performance Qoder 19/10/2026 R-04: an index is 16 bits, so if the batch whose
            // every index were 65535 still ends inside the vertex buffer, no readback can prove more than
            // that - and this scan of uncached host-mapped memory was 31.6 s of a 60 s session. Take the
            // provable answer for free and only walk the list when the worst case does not fit.
            const unsigned long long worstVertexEnd =
                voff + (65535ull + (unsigned long long)mut.base_vertex + 1ull) * stride;
            if (worstVertexEnd <= vbCap) ++s_scanSkips;
            else {
                // GeneralsX @performance Qoder 19/10/2026 R-05: cached scan answer. The walk below reads
                // uncached host-mapped memory and was 22 s of a 61 s session; but for every WW3D mesh index
                // buffer the answer cannot change after load - the backing is never re-locked. The entry is
                // keyed by the exact range and validated against the backing's write generation, so a lock
                // anywhere in the buffer (which bumps it) makes the entry answer no more, and a recycled
                // VkBuffer handle cannot inherit an old entry because the generation it would compare
                // against belongs to a different backing.
                const unsigned long long cacheKey = idxmax_key(bindIb, srcIoff, idxWords);
                bool answered = false;
                std::unordered_map<unsigned long long, VKDrawState::IdxMaxEnt>::iterator cit =
                    X.idxMax.find(cacheKey);
                if (cit != X.idxMax.end() && cit->second.buf == bindIb &&
                    cit->second.ioff == srcIoff && cit->second.words == idxWords) {
                    std::unordered_map<VkBuffer, VKGeoBack*>::iterator bit = X.geoByBuf.find(bindIb);
                    if (bit != X.geoByBuf.end() && bit->second->gen == cit->second.gen) {
                        maxIndex = cit->second.maxIndex;
                        answered = true;
                        ++s_idxCacheHits;
                    }
                }
                if (!answered) {
                    const void* imap = geo_map_of(bindIb);
                    if (!imap) bad = 4;
                    else {
                        const unsigned short* ip = (const unsigned short*)((const char*)imap + srcIoff);
                        for (unsigned i = 0; i < (unsigned)idxWords; ++i) if (ip[i] > maxIndex) maxIndex = ip[i];
                        std::unordered_map<VkBuffer, VKGeoBack*>::iterator bit = X.geoByBuf.find(bindIb);
                        if (bit != X.geoByBuf.end()) {
                            VKDrawState::IdxMaxEnt e;
                            e.buf = bindIb; e.ioff = srcIoff; e.words = idxWords;
                            e.gen = bit->second->gen; e.maxIndex = maxIndex;
                            X.idxMax[cacheKey] = e;
                        }
                        ++s_idxCacheScans;
                    }
                }
                if (!bad) {
                    const unsigned long long lastVertexEnd =
                        voff + ((unsigned long long)maxIndex + mut.base_vertex + 1ull) * stride;
                    if (lastVertexEnd > vbCap) bad = 5;
                }
            }
        }
        if (bad) {
            // GeneralsX @performance Qoder 19/10/2026 R-05 forensics: a bound handle this layer never
            // created (bad==1 with ibCap==0) is new - the R-05 hash tables made "not in the table" cheap to
            // ask, so ask and answer: is the handle a retired zombie (a live WW3D object still submitting a
            // stale buffer), a live backing, or never seen at all?
            if (bad == 1 && !ibCap && s_unknownHandleWarns < 16u) {
                ++s_unknownHandleWarns;
                size_t live = 0, zin = 0;
                for (std::deque<VKGeoBack>::iterator it = X.geo.begin(); it != X.geo.end(); ++it)
                    if (it->buf == bindIb) ++live;
                for (size_t z = 0; z < X.geoZombies.size(); ++z)
                    if (X.geoZombies[z].buf == bindIb) ++zin;
                VKWP1_LOG("wedge5 unknown handle: ib=%p vb=%p live=%zu zombie=%zu/%zu geo=%zu bybuf=%zu made=%llu dropped=%llu retired=%llu",
                    bindIb, vb, live, zin, X.geoZombies.size(), X.geo.size(), X.geoByBuf.size(),
                    X.st.buffersCreated, X.st.buffersDropped, X.st.backingsRetired);
            }
            vk_queue_unlock(); ++X.st.boundsRefusals;
            if (bad == 5) ++X.st.indexValueRefusals;
            if (X.indexWarns < 24) {
                ++X.indexWarns;
                VKWP1_LOG("wedge5 draw refused (%u): fvf=%08X topo=%u polys=%u idx=%u start=%u ib=%p off=%llu cap=%llu "
                    "maxIdx=%u baseVtx=%u vb=%p voff=%llu vcap=%llu stride=%u",
                    bad, mut.key.fvf, mut.key.topology, mut.polygon_count, indexCount, mut.start_index,
                    bindIb, srcIoff, ibCap, maxIndex, mut.base_vertex, vb, (unsigned long long)voff, vbCap,
                    (unsigned)stride);
            }
            return false;
        }
        if (X.st.draws < 12ull) {
            VKWP1_LOG("wedge5 draw #%llu: fvf=%08X topo=%u polys=%u idx=%u maxIdx=%u baseVtx=%u stride=%u "
                "vb=%p+%llu/%llu ib=%p+%llu/%llu vp=%u,%u,%u,%u pass=%u ext=%ux%u views=%u",
                X.st.draws + 1ull, mut.key.fvf, mut.key.topology, mut.polygon_count, indexCount, maxIndex,
                mut.base_vertex, (unsigned)stride, vb, (unsigned long long)voff, vbCap, bindIb, srcIoff, ibCap,
                mut.viewport[0], mut.viewport[1], mut.viewport[2], mut.viewport[3], X.curPass, X.w, X.h,
                (unsigned)((mut.texture_views[0] ? 1u : 0u) + (mut.texture_views[1] ? 1u : 0u) +
                          (mut.texture_views[2] ? 1u : 0u) + (mut.texture_views[3] ? 1u : 0u) +
                          (mut.texture_views[4] ? 1u : 0u) + (mut.texture_views[5] ? 1u : 0u) +
                          (mut.texture_views[6] ? 1u : 0u) + (mut.texture_views[7] ? 1u : 0u)));
        }
    }

    // GeneralsX @feature Qoder 20/10/2026 native sun shadow: keep every opaque world-space batch that is
    // drawn into the scene, so the sun pass can replay it from the light. Batches for other targets
    // (shadow maps, offscreen RTTs) are not part of this view.
    //
    // GeneralsX @bugfix 23/10/2026: the camera used to be adopted from EVERY non-RHW world batch, i.e.
    // the LAST one won. Decals, alpha-mask passes, line/wireframe extra passes and water tracks all draw
    // world-space batches into the scene, and several of them modify the projection (ZBias is folded into
    // ff.proj, and the extra-pass code changes the camera's Zbuffer range). A late effect batch therefore
    // replaced the main camera with a shifted projection; the fit and the receiver unprojected through
    // it and the shadows jumped/rotated. Adopt only an OPAQUE, depth-writing, colour-writing triangle
    // batch, only once, and let a terrain batch (the true scene camera) replace that first choice if it
    // arrives later. This keeps decals, masks, particles and lines out of the shadow camera.
    if (mut.ff && X.colorKey == (void*)X.sceneColorKey && X.depthKey == (void*)X.sceneDepthKey) {
        const unsigned topo = mut.key.topology;
        const bool opaqueWorld =
            !(mut.ff->flags[0] & VKFF_FLAG_RHW) &&
            !mut.key.blend_enable &&
            mut.key.depth_write &&
            (mut.key.write_mask & 0x7u) &&
            (topo == D3D_PT_TRIANGLELIST || topo == D3D_PT_TRIANGLESTRIP);
        const bool terrainBatch = (mut.key.ps_tag >= 110u && mut.key.ps_tag <= 118u);
        if (opaqueWorld && (!X.shHaveVP || terrainBatch)) {
            if (vkdraw_no_camera(mut.ff->view, mut.ff->proj)) {
                // Grass/smudge/point batches draw camera-space vertices with an identity view; adopting
                // them as the scene camera made the receiver unproject into view space (all shadows
                // dropped while such an effect was last). Name the offender once under the trace env.
                static unsigned s_identSeen[32];
                static unsigned s_identN = 0u;
                const unsigned key = (mut.key.vs_tag << 8) | mut.key.ps_tag;
                bool seen = false;
                for (unsigned i = 0; i < s_identN; ++i) if (s_identSeen[i] == key) { seen = true; break; }
                if (!seen && s_identN < 32u && shadow_trace_on()) {
                    s_identSeen[s_identN++] = key;
                    const float* w = mut.ff->world;
                    fprintf(stderr, "[VKSHADOW] non-camera batch: vs=%u ps=%u fvf=%08X topo=%u world=(%.1f %.1f %.1f)\n",
                        mut.key.vs_tag, mut.key.ps_tag, mut.key.fvf, mut.key.topology,
                        (double)w[12], (double)w[13], (double)w[14]);
                    fflush(stderr);
                }
            } else {
                memcpy(X.shView, mut.ff->view, sizeof(X.shView));
                memcpy(X.shProj, mut.ff->proj, sizeof(X.shProj));
                // Same batch, same viewport: this is the rect the camera's depth was rasterized into.
                X.shViewport[0] = (float)mut.viewport[0];
                X.shViewport[1] = (float)mut.viewport[1];
                X.shViewport[2] = (float)mut.viewport[2];
                X.shViewport[3] = (float)mut.viewport[3];
                X.shHaveVP = 1;
            }
        }
        shadow_record_caster(mut);
    }

    // Triangle fans are the one primitive D3D9 has and Vulkan does not. The fan's own indices are read
    // back out of its host-visible backing (the lock wrote them, they are still there) and expanded into
    // (0,1,2), (0,2,3), ... in the fan scratch ring, which is why this case binds a different buffer.
    s_subScanUs += vkdraw_now_us(tSeg);
    if (mut.key.topology == D3D_PT_TRIANGLEFAN) {
        const unsigned short* fanSrc = nullptr;
        // R-05: the same backing lookup the range guard already did, now O(1) instead of a second walk.
        std::unordered_map<VkBuffer, VKGeoBack*>::iterator fanIt =
            X.geoByBuf.find((VkBuffer)mut.vk_index_buffer);
        if (fanIt != X.geoByBuf.end() && fanIt->second->map) {
            fanSrc = (const unsigned short*)((char*)fanIt->second->map
                + mut.index_offset + mut.start_index * 2ull);
        }
        if (!fanSrc) {
            vk_queue_unlock(); ++X.st.drawsSkipped;
            if (X.indexWarns < 4) {
                ++X.indexWarns;
                VKWP1_LOG("wedge5 fan refused: index backing %p is not host-mapped here", mut.vk_index_buffer);
            }
            return false;
        }
        const unsigned long long need = (unsigned long long)mut.polygon_count * 3u * 2u;
        // A fan wider than the whole ring cannot be wrapped into it: the write below would run past the
        // end of the mapping, which is host memory corruption rather than a refused batch.
        if (need > VKDrawState::SCRATCH_CAP) {
            vk_queue_unlock(); ++X.st.drawsSkipped;
            if (X.indexWarns < 24) {
                ++X.indexWarns;
                VKWP1_LOG("wedge5 fan refused: %u polys need %llu bytes, ring holds %llu",
                    mut.polygon_count, need, (unsigned long long)VKDrawState::SCRATCH_CAP);
            }
            return false;
        }
        if (X.fanCur + need > VKDrawState::SCRATCH_CAP) { X.fanCur = 0; ++X.st.ringRotations; }
        const VkDeviceSize fanOff = (VkDeviceSize)X.fanCur;
        unsigned short* dst = (unsigned short*)((char*)X.fanMap + X.fanCur);
        for (unsigned i = 0; i < mut.polygon_count; ++i) {
            dst[i * 3u + 0u] = fanSrc[0];
            dst[i * 3u + 1u] = fanSrc[i + 1u];
            dst[i * 3u + 2u] = fanSrc[i + 2u];
        }
        X.fanCur += (need + 63ull) & ~63ull;
        bindIb = X.fanBuf; ioff = fanOff;
        indexCount = mut.polygon_count * 3u;
        ++X.st.fanRewrites;
    }
    G.vkCmdBindIndexBuffer(X.cmd, bindIb, ioff, VK_INDEX_TYPE_UINT16);

    // GeneralsX @debug 25/10/2026 ROTORBUF: the rotor discs (12 tris, FVF XYZNDUV2) are submitted and
    // blended yet paint nothing, while the same meshes render when sorting is off. Read the first
    // indices and the first vertex straight out of the host-mapped backings this draw is about to use:
    // correct data here moves the defect to the raster state, zeros move it to the pool's re-upload.
    if (mut.polygon_count == 12u && mut.key.fvf == 0x252u) {
        static int s_rotorBuf = -1;
        if (s_rotorBuf < 0) s_rotorBuf = getenv("GENERALSX_ROTORBUF") ? 1 : 0;
        if (s_rotorBuf && ++s_rotorBuf <= 16) {
            const unsigned short* ip = nullptr;
            std::unordered_map<VkBuffer, VKGeoBack*>::iterator iti = X.geoByBuf.find((VkBuffer)mut.vk_index_buffer);
            if (iti != X.geoByBuf.end() && iti->second->map)
                ip = (const unsigned short*)((const char*)iti->second->map
                    + mut.index_offset + mut.start_index * 2ull);
            const float* vp = nullptr;
            std::unordered_map<VkBuffer, VKGeoBack*>::iterator itv = X.geoByBuf.find((VkBuffer)mut.streams[0].vk_buffer);
            if (itv != X.geoByBuf.end() && itv->second->map) {
                vp = (const float*)((const char*)itv->second->map + mut.streams[0].offset
                    + (size_t)mut.base_vertex * mut.streams[0].stride);
            }
            fprintf(stderr, "[ROTORBUF] idxoff=%llu start=%u base=%u stride=%u ioff=%llu ic=%u voff=%llu "
                "idx=%u,%u,%u vp=%p v0=%.2f,%.2f,%.2f v1=%.2f,%.2f,%.2f\n",
                (unsigned long long)mut.index_offset, (unsigned)mut.start_index, (unsigned)mut.base_vertex,
                (unsigned)mut.streams[0].stride, (unsigned long long)ioff, indexCount,
                (unsigned long long)mut.streams[0].offset,
                ip ? ip[0] : 9999u, ip ? ip[1] : 9999u, ip ? ip[2] : 9999u, (const void*)vp,
                vp ? vp[0] : -999.0f, vp ? vp[1] : -999.0f, vp ? vp[2] : -999.0f,
                vp ? vp[11] : -999.0f, vp ? vp[12] : -999.0f, vp ? vp[13] : -999.0f);
            if (itv != X.geoByBuf.end()) {
                const VKGeoBack& b = *itv->second;
                fprintf(stderr, "[ROTORBUF]   vb map=%p curOff=%llu slot=%u gen=%u gens=%u lockedInFrame=%d\n",
                    (const void*)b.map, (unsigned long long)b.curOffset, (unsigned)b.slot,
                    (unsigned)b.curGen, (unsigned)b.gens, (int)b.lockedInFrame);
                fflush(stderr);
            }
            fflush(stderr);
        }
    }

    // All eight slots travel as one set; on the FF path slots 4..7 are always empty.
    void* const views[VKDRAW_MAX_TEX_SLOTS] = {
        mut.texture_views[0], mut.texture_views[1], mut.texture_views[2], mut.texture_views[3],
        mut.texture_views[4], mut.texture_views[5], mut.texture_views[6], mut.texture_views[7] };
    void* const samps[VKDRAW_MAX_TEX_SLOTS] = {
        mut.texture_samplers[0], mut.texture_samplers[1], mut.texture_samplers[2], mut.texture_samplers[3],
        mut.texture_samplers[4], mut.texture_samplers[5], mut.texture_samplers[6], mut.texture_samplers[7] };
    VKDraw_Const_Bind cb;
    upload_constants(&mut, &cb);
    VkDescriptorSet set = nullptr;
    if (!set_for(views, samps, cb, &set)) { vk_queue_unlock(); ++X.st.drawsBad; return false; }
    // Three dynamic bindings in layout order: vertex constants, pixel constants, lights.
    const uint32_t dyn[3] = { (uint32_t)cb.off[0], (uint32_t)cb.off[1], (uint32_t)cb.off[2] };
    G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.layout, 0, 1, &set, 3, dyn);

    G.vkCmdDrawIndexed(X.cmd, indexCount, 1, 0, (int32_t)mut.base_vertex, 0);
    ++X.st.draws;
    ++s_frameDraws;
    VKPgm_Note_Draw(mut.key.vs_tag, mut.key.ps_tag, 1, mut.texture_views[0] != nullptr);
    if ((X.st.draws & 4095ull) == 0ull) VKDraw_Log_Stats();
    VKTRACE("draw %llu: done idx=%u", X.submitTicks, indexCount);
    s_subTailUs += vkdraw_now_us(tSeg);
    vk_queue_unlock();
    return true;
}

bool VK_Draw_User_Primitive(unsigned primitive_type, const void* vertex_data, unsigned vertex_count,
                           unsigned stride, unsigned polygon_count, const VKDraw_Attr* attrs,
                           unsigned attr_count, const VKDraw_PSO_Key* key)
{
    if (!key) return false;
    // A caller that describes nothing gets the position-only layout the mechanism stages expect.
    static const VKDraw_Attr s_positionOnly = { 0, 0, 3, 0 };
    if (!attrs || !attr_count) { attrs = &s_positionOnly; attr_count = 1; }
    if (attr_count > VKDRAW_MAX_ATTRS) return false;

    VKDraw_Submit s = {};
    s.key = *key;
    s.attr_count = attr_count;
    for (unsigned i = 0; i < attr_count; ++i) s.attrs[i] = attrs[i];
    return VK_Draw_User_Primitive_State(primitive_type, vertex_data, vertex_count, stride, polygon_count, &s);
}

bool VK_Draw_User_Primitive_State(unsigned primitive_type, const void* vertex_data, unsigned vertex_count,
                                  unsigned stride, unsigned polygon_count, const VKDraw_Submit* state)
{
    if (!VKDraw_Ready() || !X.frameOpen || !vertex_data || !vertex_count || !stride || !state) return false;
    if (!state->attr_count || state->attr_count > VKDRAW_MAX_ATTRS || !polygon_count) return false;
    if (primitive_type < D3D_PT_POINTLIST || primitive_type > D3D_PT_TRIANGLEFAN) {
        ++X.st.topologyRefusals; return false;
    }

    // D3D9 generated the indices for a user-memory draw itself; here they are written into the same ring.
    // A fan has no Vulkan topology, so it is expanded while generating rather than by the submit path -
    // which only knows how to expand a fan that lives in a WW3D index buffer.
    const int isFan = (primitive_type == D3D_PT_TRIANGLEFAN) ? 1 : 0;
    const unsigned genCount = isFan ? polygon_count * 3u
                                    : vk_index_count_for(primitive_type, polygon_count);
    const unsigned long long bytes    = (unsigned long long)vertex_count * stride;
    const unsigned long long idxBytes = (unsigned long long)genCount * 2ull;
    // GeneralsX @bugfix Qoder 20/10/2026 BUG-05: the ring advances by the 256-aligned size of each half but
    // used to wrap-test against the unaligned sum, so a batch that only fitted after padding wrote its
    // indices at upCur == SCRATCH_CAP - i.e. memcpy past the end of the host mapping (memory corruption,
    // which the submit-side range guard then refused as bounds=1 but had already performed).
    const unsigned long long vtxPad = (bytes    + 255ull) & ~255ull;
    const unsigned long long idxPad = (idxBytes + 255ull) & ~255ull;
    if (vtxPad + idxPad > VKDrawState::SCRATCH_CAP) {
        ++X.st.drawsSkipped;
        if (X.streamWarns < 4) {
            ++X.streamWarns;
            VKWP1_LOG("wedge5 user primitive too big for the scratch ring: %llu + %llu bytes",
                bytes, idxBytes);
        }
        return false;
    }

    vk_queue_lock();
    if (X.upCur + vtxPad + idxPad > VKDrawState::SCRATCH_CAP) { X.upCur = 0; ++X.st.ringRotations; }
    const unsigned long long vtxOff = X.upCur;
    memcpy((char*)X.upMap + vtxOff, vertex_data, (size_t)bytes);
    X.upCur += vtxPad;
    const unsigned long long idxOff = X.upCur;
    unsigned short* dst = (unsigned short*)((char*)X.upMap + X.upCur);
    for (unsigned i = 0; i < genCount; ++i) {
        if (isFan) {
            const unsigned t = i / 3u;
            dst[i] = (unsigned short)((i % 3u == 0u) ? 0u : (i % 3u == 1u ? t + 1u : t + 2u));
        } else {
            dst[i] = (unsigned short)i;
        }
    }
    X.upCur += idxPad;
    vk_queue_unlock();

    VKDraw_Submit s = *state;
    s.key.topology = isFan ? D3D_PT_TRIANGLELIST : primitive_type;
    s.stream_count = 1;
    s.streams[0].vk_buffer = (void*)X.upBuf;
    s.streams[0].offset = vtxOff;
    s.streams[0].stride = stride;
    s.streams[0].vertex_count = vertex_count;
    s.vk_index_buffer = (void*)X.upBuf;
    s.index_offset = idxOff;
    s.index_count = genCount;
    s.start_index = 0;
    s.polygon_count = polygon_count;
    s.base_vertex = 0;
    // The facade publishes the current viewport with every submit; only a caller that left it empty gets
    // the full target, which is what the older mechanism probes relied on.
    if (!s.viewport[2] || !s.viewport[3]) { s.viewport[2] = X.w; s.viewport[3] = X.h; }
    if (!s.scissor[2] || !s.scissor[3]) { s.scissor[2] = X.w; s.scissor[3] = X.h; }
    return VK_Draw_Submit(&s);
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: indexed user-memory submit. The raw-buffer
// families already build vertices and 16-bit indices in CPU memory before copying them into D3D9 resources;
// this copies the same two ranges into the scratch ring and names the D3D9 base-vertex index, so those
// draws no longer need a mirror of the D3D9 buffer itself. The vertex slice starts at 'base_vertex', and
// the recorded stream therefore begins at index 0 with a zero base - the same geometry D3D9 addressed.
bool VK_Draw_Indexed_User_Primitive_State(unsigned primitive_type,
                                          const void* vertex_data, unsigned vertex_count, unsigned stride,
                                          const void* index_data, unsigned index_count, unsigned base_vertex,
                                          const VKDraw_Submit* state)
{
    if (!VKDraw_Ready() || !X.frameOpen || !vertex_data || !vertex_count || !stride || !state) return false;
    if (!index_data || index_count < 2u) return false;
    if (!state->attr_count || state->attr_count > VKDRAW_MAX_ATTRS) return false;
    if (primitive_type < D3D_PT_POINTLIST || primitive_type > D3D_PT_TRIANGLEFAN) {
        ++X.st.topologyRefusals; return false;
    }

    // D3D9 names the polygon count; the caller hands the index count it built, so derive the same number
    // D3D9 would have consumed from that topology.
    const int isFan = (primitive_type == D3D_PT_TRIANGLEFAN) ? 1 : 0;
    unsigned polygon_count = 0u;
    switch (primitive_type) {
    case D3D_PT_POINTLIST:    polygon_count = index_count; break;
    case D3D_PT_LINELIST:     polygon_count = index_count / 2u; break;
    case D3D_PT_LINESTRIP:    polygon_count = index_count - 1u; break;
    case D3D_PT_TRIANGLELIST: polygon_count = index_count / 3u; break;
    default:                  polygon_count = index_count - 2u; break;   // TRIANGLESTRIP and TRIANGLEFAN
    }
    if (!polygon_count) return false;

    const unsigned genCount = isFan ? polygon_count * 3u : vk_index_count_for(primitive_type, polygon_count);
    const unsigned long long bytes    = (unsigned long long)vertex_count * stride;
    const unsigned long long idxBytes = (unsigned long long)genCount * 2ull;
    // GeneralsX @bugfix Qoder 20/10/2026 BUG-05: same padding-blind wrap test as the non-indexed path -
    // the 24 "wedge5 draw refused (1) fvf=00000104 ... off=8388608 cap=8388608" lines of the R-06 sessions
    // are the sun-shadow occluder sink (stride 24 = sizeof(RHWV)) landing its index half at upCur == cap,
    // which is a memcpy past the end of the 8 MiB host mapping. The guard refused the batch but the write
    // had already happened at staging time.
    const unsigned long long vtxPad = (bytes    + 255ull) & ~255ull;
    const unsigned long long idxPad = (idxBytes + 255ull) & ~255ull;
    if (vtxPad + idxPad > VKDrawState::SCRATCH_CAP) {
        ++X.st.drawsSkipped;
        if (X.streamWarns < 4) {
            ++X.streamWarns;
            VKWP1_LOG("wedge5 indexed user primitive too big for the scratch ring: %llu + %llu bytes",
                bytes, idxBytes);
        }
        return false;
    }

    vk_queue_lock();
    if (X.upCur + vtxPad + idxPad > VKDrawState::SCRATCH_CAP) { X.upCur = 0; ++X.st.ringRotations; }
    const unsigned long long vtxOff = X.upCur;
    memcpy((char*)X.upMap + vtxOff, (const char*)vertex_data + (size_t)base_vertex * stride, (size_t)bytes);
    X.upCur += vtxPad;
    const unsigned long long idxOff = X.upCur;
    unsigned short* dst = (unsigned short*)((char*)X.upMap + X.upCur);
    const unsigned short* src = (const unsigned short*)index_data;
    if (isFan) {
        // Vulkan has no fan topology, so the fan is expanded exactly like the non-indexed path does it.
        for (unsigned i = 0; i < genCount; ++i) {
            const unsigned t = i / 3u;
            dst[i] = (i % 3u == 0u) ? src[0] : src[t + (i % 3u)];
        }
    } else {
        memcpy(dst, src, (size_t)genCount * 2u);
    }
    X.upCur += idxPad;
    vk_queue_unlock();

    VKDraw_Submit s = *state;
    s.key.topology = isFan ? D3D_PT_TRIANGLELIST : primitive_type;
    s.stream_count = 1;
    s.streams[0].vk_buffer = (void*)X.upBuf;
    s.streams[0].offset = vtxOff;
    s.streams[0].stride = stride;
    s.streams[0].vertex_count = vertex_count;
    s.vk_index_buffer = (void*)X.upBuf;
    s.index_offset = idxOff;
    s.index_count = genCount;
    s.start_index = 0;
    s.polygon_count = polygon_count;
    s.base_vertex = 0;   // the copied slice already starts at the caller's base vertex
    if (!s.viewport[2] || !s.viewport[3]) { s.viewport[2] = X.w; s.viewport[3] = X.h; }
    if (!s.scissor[2] || !s.scissor[3]) { s.scissor[2] = X.w; s.scissor[3] = X.h; }
    return VK_Draw_Submit(&s);
}

// ===========================================================================
// present-side post pass: FXAA + bloom (VkPost)
// ===========================================================================
// GeneralsX @feature Qoder 20/10/2026: in the pure-Vulkan wedge the picture is presented by the blit in
// End_Frame, so the two effects the player asked for live here rather than in the WW3D post chain: every
// attachment is 1-sample (no MSAA anywhere), and the retail bloom chain is skipped because its RTT probe
// needs GetRenderTarget, which the D3D9 stub answers with null. Both are present-side effects, so one
// full-screen draw reads the scene mirror once and writes the swapchain image, replacing the blit.
//
// The pass renders directly into a render pass whose single attachment is the swapchain image (CLEAR,
// UNDEFINED -> PRESENT_SRC), so the acquire needs no barrier of its own and the present engine gets the
// layout it expects. Knobs come from DX8Wrapper::VK_Post_* (pushed by W3DDisplay), and both at zero is
// the plain blit exactly as it was: off is a real state, not a slower equivalent.
namespace {

struct VKPostState {
    bool envChecked = false, envOn = true;
    bool built = false, failed = false;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {};
    VkRenderPass rp = nullptr;
    std::vector<VkFramebuffer> fbs;
    std::vector<VkImageView> fbViews;
    VkDescriptorSetLayout dsl = nullptr;
    VkPipelineLayout pl = nullptr;
    VkPipeline pipe = nullptr;
    VkShaderModule vs = nullptr, fs = nullptr;
    VkSampler sampler = nullptr;
    /// Nearest sampler for the scene-depth binding. D32_SFLOAT does not advertise linear filtering on most
    /// drivers, and sampling it with P.sampler (linear) returned 1.0 for every pixel - which silently made
    /// the post pass depth-blind: AO had no surface, and the fog reconstructed its rays at the far plane
    /// and then looked like a flat screen veil. Depth is always read at texel centres here, so nearest is
    /// also the correct filter.
    VkSampler samplerNearest = nullptr;
    VkDescriptorPool pool = nullptr;
    VkDescriptorSet sets[VKGEO_SLOT_COUNT] = {};
    unsigned long long frames = 0, fallbacks = 0;
    // P6 bloom mip chain: five box-downsampled levels, owned by VKRT and re-created on resize.
    VkImageView bloomView[5] = {};
    unsigned    bloomW[5] = {}, bloomH[5] = {};

    // P6 "底片/镜头": the same shader/layout also resolves the 3D scene into the LDR UI overlay. That
    // target is a single VKRT image (not a swapchain image), so it needs its own pass/framebuffer and a
    // pipeline created against that pass; the shader modules, descriptor layout and pipeline layout are
    // shared with the present-side pass above.
    VkRenderPass overlayRp = nullptr;
    VkFramebuffer overlayFb = nullptr;
    VkPipeline overlayPipe = nullptr;
    VkImageView overlayView = nullptr;
    VkFormat overlayFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D overlayExtent = {};
};
VKPostState P;

// ---------------------------------------------------------------------------
// P6 sky cubemap / fullscreen sky PSO
// ---------------------------------------------------------------------------
// The retail sky is five textured faces of new_skybox. This path copies N/E/S/W/T into one cube image
// and draws it with a single fullscreen triangle at depth == 1.0, so it only fills pixels no geometry
// touched. The mesh remains the fallback when a face is unreadable (compressed/odd TGA).
struct VKSkyState {
    bool           ready = false;
    VkImage        image = nullptr;
    VkDeviceMemory mem = nullptr;
    VkImageView    view = nullptr;
    VkSampler      sampler = nullptr;
    VkDescriptorSetLayout dsl = nullptr;
    VkDescriptorPool      pool = nullptr;
    VkDescriptorSet       set = nullptr;
    VkPipelineLayout      pl = nullptr;
    VkPipeline            pipe = nullptr;
    VkRenderPass          pipeRp = nullptr;
    VkShaderModule        vs = nullptr, fs = nullptr;
    unsigned       size = 0;
    const TextureClass* faces[5] = {};
};
VKSkyState S;

void vksky_destroy()
{
    VkDevice dev = dev_();
    if (!dev) return;
    if (S.pipe) G.vkDestroyPipeline(dev, S.pipe, nullptr);
    if (S.pl) G.vkDestroyPipelineLayout(dev, S.pl, nullptr);
    if (S.dsl) G.vkDestroyDescriptorSetLayout(dev, S.dsl, nullptr);
    if (S.pool) G.vkDestroyDescriptorPool(dev, S.pool, nullptr);
    if (S.sampler) G.vkDestroySampler(dev, S.sampler, nullptr);
    if (S.view) G.vkDestroyImageView(dev, S.view, nullptr);
    if (S.image) G.vkDestroyImage(dev, S.image, nullptr);
    if (S.mem) G.vkFreeMemory(dev, S.mem, nullptr);
    if (S.vs) G.vkDestroyShaderModule(dev, S.vs, nullptr);
    if (S.fs) G.vkDestroyShaderModule(dev, S.fs, nullptr);
    S = VKSkyState();
}

bool vksky_upload_faces(TextureClass* n, TextureClass* e, TextureClass* s, TextureClass* w, TextureClass* t)
{
    TextureClass* faces[5] = { n, e, s, w, t };
    SurfaceClass::SurfaceDescription sd = {};
    SurfaceClass* surf0 = faces[0] ? faces[0]->Get_Surface_Level(0) : nullptr;
    if (!surf0) { VKWP1_LOG("[VKSKY] upload fail: no surface for north"); return false; }
    surf0->Get_Description(sd);
    const unsigned size = sd.Width;
    REF_PTR_RELEASE(surf0);
    if (!size || sd.Height != sd.Width) {
        VKWP1_LOG("[VKSKY] upload fail: face extent %ux%u", sd.Width, sd.Height);
        return false;
    }

    // Sky TGA assets are usually DXT-compressed (Get_Bytes_Per_Pixel returns 0 for those). Upload the
    // block data as BC1/BC2/BC3 rather than decoding: it keeps the copy path simple and the GPU filters
    // the compressed cube directly.
    // GeneralsX @feature 26/10/2026 HDR: the sky faces are colour art. In HDR mode the image is created
    // in its sRGB variant so the sampler decodes to linear; the bytes and the copy path are unchanged.
    const bool linearSky = DX8Wrapper::VK_HDR_Enabled();
    VkFormat skyFmt = linearSky ? VK_FORMAT_B8G8R8A8_SRGB : VK_FORMAT_B8G8R8A8_UNORM;
    bool compressed = false;
    unsigned blockBytes = 0;
    switch (sd.Format) {
        case WW3D_FORMAT_DXT1:
            skyFmt = linearSky ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
            compressed = true; blockBytes = 8; break;
        case WW3D_FORMAT_DXT2:
        case WW3D_FORMAT_DXT3:
            skyFmt = linearSky ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK;
            compressed = true; blockBytes = 16; break;
        case WW3D_FORMAT_DXT4:
        case WW3D_FORMAT_DXT5:
            skyFmt = linearSky ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK;
            compressed = true; blockBytes = 16; break;
        default: break;
    }
    const unsigned blocksX = compressed ? (size + 3u) / 4u : size;
    const unsigned blocksY = compressed ? (size + 3u) / 4u : size;
    const size_t layerBytes = compressed
        ? (size_t)blocksX * blocksY * blockBytes
        : (size_t)size * size * 4u;
    if (!compressed) {
        const unsigned bpp0 = sd.Format == WW3D_FORMAT_R8G8B8 ? 3u : 4u;
        if (bpp0 != 3 && bpp0 != 4) {
            VKWP1_LOG("[VKSKY] upload fail: fmt=%d", (int)sd.Format);
            return false;
        }
    }

    VkDevice dev = dev_();
    if (!dev || !X.cmd) return false;

    // Cube layer order follows the Vulkan face convention: +X,-X,+Y,-Y,+Z,-Z.
    // N=-Z, E=+X, S=+Z, W=-X, T=+Y; the missing bottom reuses the top face.
    TextureClass* layerTex[6] = { e, w, t, t, s, n };
    VkBuffer stageBuf = nullptr; VkDeviceMemory stageMem = nullptr; void* stageMap = nullptr;
    if (!make_host_buffer((unsigned long long)(layerBytes * 6u), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          &stageBuf, &stageMem, &stageMap))
        return false;

    bool ok = true;
    for (int layer = 0; layer < 6 && ok; ++layer) {
        TextureClass* tex = layerTex[layer];
        SurfaceClass* surf = tex ? tex->Get_Surface_Level(0) : nullptr;
        if (!surf) { VKWP1_LOG("[VKSKY] upload fail: layer %d no surface", layer); ok = false; break; }
        SurfaceClass::SurfaceDescription d = {};
        surf->Get_Description(d);
        if (d.Width != size || d.Height != size) {
            VKWP1_LOG("[VKSKY] upload fail: layer %d extent %ux%u", layer, d.Width, d.Height);
            REF_PTR_RELEASE(surf); ok = false; break;
        }
        int pitch = 0;
        unsigned char* src = (unsigned char*)surf->Lock(&pitch);
        if (!src) { VKWP1_LOG("[VKSKY] upload fail: layer %d lock", layer); REF_PTR_RELEASE(surf); ok = false; break; }
        unsigned char* dst = (unsigned char*)stageMap + layerBytes * (size_t)layer;
        if (compressed) {
            const unsigned rowBytes = blocksX * blockBytes;
            for (unsigned y = 0; y < blocksY; ++y)
                memcpy(dst + (size_t)y * rowBytes, src + (size_t)y * (size_t)pitch, rowBytes);
        } else {
            const unsigned bpp = (d.Format == WW3D_FORMAT_R8G8B8) ? 3u : 4u;
            for (unsigned y = 0; y < size; ++y) {
                const unsigned char* row = src + (size_t)y * (size_t)pitch;
                unsigned char* out = dst + (size_t)y * size * 4u;
                if (bpp == 4) {
                    memcpy(out, row, (size_t)size * 4u);
                } else {
                    for (unsigned x = 0; x < size; ++x) {
                        out[x * 4u + 0] = row[x * 3u + 0];
                        out[x * 4u + 1] = row[x * 3u + 1];
                        out[x * 4u + 2] = row[x * 3u + 2];
                        out[x * 4u + 3] = 255;
                    }
                }
            }
        }
        surf->Unlock();
        REF_PTR_RELEASE(surf);
    }
    if (!ok) {
        G.vkDestroyBuffer(dev, stageBuf, nullptr);
        G.vkFreeMemory(dev, stageMem, nullptr);
        return false;
    }

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = skyFmt;
    ici.extent = { size, size, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 6; ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!chk("vkCreateImage(vksky)", G.vkCreateImage(dev, &ici, nullptr, &S.image))) ok = false;
    VkMemoryRequirements req = {};
    if (ok) G.vkGetImageMemoryRequirements(dev, S.image, &req);
    const uint32_t mt = ok ? memtype_(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) : 0xFFFFFFFFu;
    if (!ok || mt == 0xFFFFFFFFu) ok = false;
    if (ok) {
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = req.size; mai.memoryTypeIndex = mt;
        if (!chk("vkAllocateMemory(vksky)", G.vkAllocateMemory(dev, &mai, nullptr, &S.mem))) ok = false;
    }
    if (ok && !chk("vkBindImageMemory(vksky)", G.vkBindImageMemory(dev, S.image, S.mem, 0))) ok = false;

    if (ok) {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = S.image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        G.vkCmdPipelineBarrier(X.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy cp[6] = {};
        for (int layer = 0; layer < 6; ++layer) {
            cp[layer].bufferOffset = layerBytes * (VkDeviceSize)layer;
            cp[layer].imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)layer, 1 };
            cp[layer].imageExtent = { size, size, 1 };
        }
        G.vkCmdCopyBufferToImage(X.cmd, stageBuf, S.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 6, cp);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        G.vkCmdPipelineBarrier(X.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               0, 0, nullptr, 0, nullptr, 1, &b);
    }

    // The staging copy is still referenced by the current command buffer; retire it instead of freeing.
    VKGeoZombie z = { stageBuf, stageMem, stageMap, 3u };
    X.geoZombies.push_back(z);

    if (!ok) return false;

    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = S.image; vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
    vci.format = skyFmt;
    vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6 };
    if (!chk("vkCreateImageView(vksky)", G.vkCreateImageView(dev, &vci, nullptr, &S.view))) return false;

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (!chk("vkCreateSampler(vksky)", G.vkCreateSampler(dev, &sci, nullptr, &S.sampler))) return false;

    VkDescriptorSetLayoutBinding db = {};
    db.binding = 0; db.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    db.descriptorCount = 1; db.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dl.bindingCount = 1; dl.pBindings = &db;
    if (!chk("vkCreateDescriptorSetLayout(vksky)", G.vkCreateDescriptorSetLayout(dev, &dl, nullptr, &S.dsl))) return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dp.maxSets = 1; dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    if (!chk("vkCreateDescriptorPool(vksky)", G.vkCreateDescriptorPool(dev, &dp, nullptr, &S.pool))) return false;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    ai.descriptorPool = S.pool; ai.descriptorSetCount = 1; ai.pSetLayouts = &S.dsl;
    if (!chk("vkAllocateDescriptorSets(vksky)", G.vkAllocateDescriptorSets(dev, &ai, &S.set))) return false;
    VkDescriptorImageInfo dii = { S.sampler, S.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    wr.dstSet = S.set; wr.dstBinding = 0; wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wr.pImageInfo = &dii;
    G.vkUpdateDescriptorSets(dev, 1, &wr, 0, nullptr);

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96u };
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plc.setLayoutCount = 1; plc.pSetLayouts = &S.dsl;
    plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pcr;
    if (!chk("vkCreatePipelineLayout(vksky)", G.vkCreatePipelineLayout(dev, &plc, nullptr, &S.pl))) return false;

    VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    sm.codeSize = sizeof(VKSKY_VERT_SPV); sm.pCode = VKSKY_VERT_SPV;
    if (!chk("vkCreateShaderModule(vksky vs)", G.vkCreateShaderModule(dev, &sm, nullptr, &S.vs))) return false;
    sm.codeSize = sizeof(VKSKY_FRAG_SPV); sm.pCode = VKSKY_FRAG_SPV;
    if (!chk("vkCreateShaderModule(vksky fs)", G.vkCreateShaderModule(dev, &sm, nullptr, &S.fs))) return false;

    S.size = size;
    S.faces[0] = n; S.faces[1] = e; S.faces[2] = s; S.faces[3] = w; S.faces[4] = t;
    S.ready = true;
    VKWP1_LOG("[VKSKY] cubemap ready %ux%u (N/E/S/W/T)", size, size);
    return true;
}

bool sky_set_faces_internal(TextureClass* north, TextureClass* east, TextureClass* south,
                          TextureClass* west, TextureClass* top)
{
    if (!north || !east || !south || !west || !top) return false;
    if (S.ready && S.faces[0] == north && S.faces[1] == east && S.faces[2] == south &&
        S.faces[3] == west && S.faces[4] == top)
        return true;
    // Rebuilding is a TOD/weather event, not per-frame: wait so the old cube/sampler are not in flight.
    if (S.ready && dev_()) G.vkDeviceWaitIdle(dev_());
    vksky_destroy();
    return vksky_upload_faces(north, east, south, west, top);
}

bool sky_ready_internal()
{
    return S.ready && S.image && S.view && S.set && S.pl && S.vs && S.fs;
}

// GeneralsX @feature 24/10/2026 P34 C4: the scene descriptor set (bindings 9/10) needs the cube view and
// its linear sampler for the environment reflection; the sky pass keeps its own descriptor set.
void* vksky_view_internal()
{
    return sky_ready_internal() ? (void*)S.view : nullptr;
}

void* vksky_sampler_internal()
{
    return sky_ready_internal() ? (void*)S.sampler : nullptr;
}

bool vksky_ensure_pipeline()
{
    if (!sky_ready_internal() || X.curPass >= X.passes.size()) return false;
    VkRenderPass rp = X.passes[X.curPass].rp;
    if (S.pipe && S.pipeRp == rp) return true;
    if (S.pipe) { G.vkDestroyPipeline(dev_(), S.pipe, nullptr); S.pipe = nullptr; }

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = (VkSampleCountFlagBits)X.passes[X.curPass].samples;
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    dss.depthTestEnable = VK_TRUE; dss.depthWriteEnable = VK_FALSE;
    dss.depthCompareOp = VK_COMPARE_OP_EQUAL;   // far plane only; scene depth clear is 1.0
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo stg[2] = {};
    stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = S.vs; stg[0].pName = "main";
    stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = S.fs; stg[1].pName = "main";
    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2; gpi.pStages = stg;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
    gpi.layout = S.pl; gpi.renderPass = rp; gpi.subpass = 0;
    if (!chk("vkCreateGraphicsPipelines(vksky)", G.vkCreateGraphicsPipelines(dev_(), nullptr, 1, &gpi, nullptr, &S.pipe)))
        return false;
    S.pipeRp = rp;
    return true;
}

bool sky_render_internal(float tintR, float tintG, float tintB)
{
    if (!vksky_ensure_pipeline() || !X.cmd) return false;
    if (!X.depthKey) return false;

    Matrix4x4 view, proj;
    DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, view);
    DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, proj);
    const Matrix4x4 vp = view * proj;
    const Matrix4x4 invVP = vp.Inverse();
    const Matrix4x4 invView = view.Inverse();
    const Vector3 cam(invView[3].X, invView[3].Y, invView[3].Z);

    float pc[24] = {
        invVP[0].X, invVP[0].Y, invVP[0].Z, invVP[0].W,
        invVP[1].X, invVP[1].Y, invVP[1].Z, invVP[1].W,
        invVP[2].X, invVP[2].Y, invVP[2].Z, invVP[2].W,
        invVP[3].X, invVP[3].Y, invVP[3].Z, invVP[3].W,
        cam.X, cam.Y, cam.Z, 1.0f,
        tintR, tintG, tintB, 1.0f
    };

    VkViewport vpRect;
    VkRect2D scRect;
    if (X.shHaveVP && X.shViewport[2] > 0.0f && X.shViewport[3] > 0.0f) {
        vpRect = { X.shViewport[0], X.shViewport[1], X.shViewport[2], X.shViewport[3], 0.0f, 1.0f };
        scRect = { { (int32_t)X.shViewport[0], (int32_t)X.shViewport[1] },
                   { (uint32_t)X.shViewport[2], (uint32_t)X.shViewport[3] } };
    } else {
        vpRect = { 0.0f, 0.0f, (float)X.w, (float)X.h, 0.0f, 1.0f };
        scRect = { { 0, 0 }, { X.w, X.h } };
    }
    G.vkCmdSetViewport(X.cmd, 0, 1, &vpRect);
    G.vkCmdSetScissor(X.cmd, 0, 1, &scRect);
    G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pipe);
    G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.pl, 0, 1, &S.set, 0, nullptr);
    G.vkCmdPushConstants(X.cmd, S.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 96u, pc);
    G.vkCmdDraw(X.cmd, 3, 1, 0, 0);
    {
        static int s_skyDrawLog = 0;
        if (s_skyDrawLog < 3) {
            ++s_skyDrawLog;
            VKWP1_LOG("[VKSKY] fullscreen draw %ux%u cube=%u", (unsigned)vpRect.width,
                (unsigned)vpRect.height, S.size);
        }
    }
    return true;
}

bool vkpost_env_on()
{
    if (!P.envChecked) {
        P.envChecked = true;
        char v[8] = {};
        P.envOn = !(GetEnvironmentVariableA("GENERALSX_VKPOST", v, (DWORD)sizeof(v)) && v[0] == '0');
    }
    return P.envOn;
}

// GeneralsX @feature 23/10/2026 P6 follow-up: AO/grade are player options seeded through
// DX8Wrapper (default off); the env overrides stay for scripted A/B.
float vkpost_ao_strength()
{
    float s = DX8Wrapper::VK_AO_Strength();
    if (const char* a = getenv("GENERALSX_VK_AO")) s = (float)atof(a);
    if (s < 0.0f) s = 0.0f;
    if (s > 1.5f) s = 1.5f;
    return vk_parity_on() ? 0.0f : s;
}

bool vkpost_grade_on()
{
    bool on = DX8Wrapper::VK_Film_Grade();
    if (const char* g = getenv("GENERALSX_VK_GRADE")) on = atof(g) > 0.0;
    return !vk_parity_on() && on;
}

// GeneralsX @feature 26/10/2026 HDR: fitted-ACES pre-exposure. It is a calibration, not a look knob:
// 0.5 makes the curve ~identity over the SDR range so the corrected linear scene re-encodes to the
// retail display value, and only >1 radiance is compressed. Larger values lift mids and flatten the
// curve. GENERALSX_VK_ACES_PRE is QA-only.
//
// GeneralsX @tweak 27/10/2026 (second pass): 0.50 -> 0.80. The road/tree/bib/vegetation per-pixel
// unification removed the old baked-vertex paths (which were brighter in HDR), so the day frame fell to
// 0.1766 vs 0.2170 SDR at 0.50. Measured on the fixed camera: 0.55 -> 0.1849 (-15%), 0.65 -> 0.2019
// (-7%), 0.80 -> 0.2260 (+4.1% over SDR). 0.80 is the shipped default; dusk's EV offset was retuned
// with it (-0.10 -> -0.28) so dusk lands at +3% instead of +18%.
float vkpost_aces_pre()
{
    static float s_cached = -1.0f;
    if (s_cached < 0.0f) {
        const char* e = getenv("GENERALSX_VK_ACES_PRE");
        float v = (e != nullptr && *e != 0) ? (float)atof(e) : 0.80f;
        if (v < 0.1f) v = 0.1f;
        if (v > 2.0f) v = 2.0f;
        s_cached = v;
    }
    return s_cached;
}

// 6B: the five bloom images are allocated/released by W3DRenderGraph (VKDraw_Graph_Post_Target_Desc
// publishes their sizes). This function only records the blit chain into those images; a missing level
// means the graph did not allocate the chain this frame, and the composite falls back to the scene view.
void vkpost_bloom_levels(VkImage scene, void* sceneKey, unsigned w, unsigned h)
{
    for (int i = 0; i < 5; ++i) { P.bloomView[i] = nullptr; P.bloomW[i] = P.bloomH[i] = 0; }
    if (!scene || !sceneKey || !w || !h) return;

    VKRT_Info si = {};
    if (!VKRT_Query(sceneKey, &si) || !si.view) return;

    VkImage srcImage = scene;
    void* srcKey = sceneKey;
    unsigned sw = w, sh = h;
    for (int i = 0; i < 5; ++i) {
        VKRT_Info di = {};
        if (!VKRT_Query(s_bloomKeys[i], &di) || !di.view || !di.image) return;
        const unsigned bw = di.width, bh = di.height;
        if (!bw || !bh) return;

        VKRT_Cmd_Transition(X.cmd, srcKey, VKRT_LAYOUT_TRANSFER_SRC);
        VKRT_Cmd_Transition(X.cmd, s_bloomKeys[i], VKRT_LAYOUT_TRANSFER_DST);
        VkImageBlit blit = {};
        blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.srcOffsets[1] = { (int32_t)sw, (int32_t)sh, 1 };
        blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
        blit.dstOffsets[1] = { (int32_t)bw, (int32_t)bh, 1 };
        G.vkCmdBlitImage(X.cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         (VkImage)di.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &blit, VK_FILTER_LINEAR);
        VKRT_Cmd_Transition(X.cmd, srcKey, VKRT_LAYOUT_SHADER_READ_ONLY);
        VKRT_Cmd_Transition(X.cmd, s_bloomKeys[i], VKRT_LAYOUT_SHADER_READ_ONLY);

        P.bloomView[i] = (VkImageView)di.view;
        P.bloomW[i] = bw; P.bloomH[i] = bh;
        srcImage = (VkImage)di.image;
        srcKey = s_bloomKeys[i];
        sw = bw; sh = bh;
    }
}

// Bind scene / depth / the five bloom levels into this slot's post descriptor set. Missing levels
// fall back to the scene view so the shader always samples a valid image (and the code still runs
// when bloom is off).
// GeneralsX @feature 26/10/2026: fill this slot's fog UBO and bind the scene/depth/bloom chain, the new
// fog block and this frame's sun map. A missing shadow map is replaced with the scene view; the shader
// gates its sun term on the light matrix, which VKDraw_Shadow_Apply zeroes on every skip path.
void vkpost_fill_fog()
{
    if (!X.fogMap || !X.fogBuf) return;
    VKPostFogUbo* f = reinterpret_cast<VKPostFogUbo*>((char*)X.fogMap + X.slot * VKPOST_FOG_SLOT_BYTES);
    memset(f, 0, sizeof(*f));

    Matrix4x4 view;
    DX8Wrapper::_Get_DX8_Transform(D3DTS_VIEW, view);
    const Matrix4x4 inv = view.Inverse();
    {
        Matrix4x4 proj;
        DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, proj);
        // Store the TRANSPOSE of the Matrix4x4 as flat rows: the shader's rows must be the four
        // VECTORS it dots a clip-space vector against, i.e. the columns of the DX8 matrix. (The post
        // pass computes its inverse-projection rows from Matrix4x4::Inverse(), whose [] is the same
        // column access, which is why the naive rows produced a transposed projection and both
        // screen-space effects silently missed.)
        for (int r = 0; r < 4; ++r) {
            f->proj[r * 4 + 0] = proj[0][r];
            f->proj[r * 4 + 1] = proj[1][r];
            f->proj[r * 4 + 2] = proj[2][r];
            f->proj[r * 4 + 3] = proj[3][r];
        }
    }
    for (int r = 0; r < 4; ++r) {
        f->invView[r * 4 + 0] = inv[r].X;
        f->invView[r * 4 + 1] = inv[r].Y;
        f->invView[r * 4 + 2] = inv[r].Z;
        f->invView[r * 4 + 3] = inv[r].W;
    }
    // The game layer publishes the real 3D camera basis (rows right/up/forward, position in .w). Prefer it:
    // D3DTS_VIEW at post time belongs to whatever fullscreen/2D pass ran last and in practice came back as
    // identity, which made the fog integrate in view space and misproject every sun-shadow lookup.
    const VKAtmosphereFogParams& p = DX8Wrapper::VK_Atmosphere_Fog();
    if (p.camWorld[15] != 0.0f) {
        memcpy(f->invView, p.camWorld, sizeof(f->invView));
    }
    memcpy(f->lightVP, X.shLightVP, sizeof(f->lightVP));

    f->color[0] = p.color[0]; f->color[1] = p.color[1]; f->color[2] = p.color[2];
    f->color[3] = (float)p.mode;
    f->params[0] = p.density;
    f->params[1] = p.heightFalloff;
    f->params[2] = p.baseHeight;
    f->params[3] = p.startDistance;
    f->params2[0] = p.fadeLength;
    f->params2[1] = p.maxOpacity;
    f->params2[2] = p.noiseScale;
    // GeneralsX @feature 光影升级2.0 A1 28/10/2026: the fog clock is also the film-grain seed (one pattern
    // per frame). The game layer only publishes `time` on a lighting refresh, so a static scene would
    // freeze the grain into a fixed dirt pattern; drive it from the frame clock instead - render-only,
    // never gameplay time (determinism rule).
    f->params2[3] = (float)(X.st.framesBegun % 200000ull) * 0.033f;
    f->params3[0] = p.sunDir[0]; f->params3[1] = p.sunDir[1]; f->params3[2] = p.sunDir[2];
    f->params3[3] = p.sunScatter;
    f->params4[0] = p.sunColor[0]; f->params4[1] = p.sunColor[1]; f->params4[2] = p.sunColor[2];
    f->params4[3] = p.skyAffect;
    f->params5[0] = p.windSpeed;
    f->params5[1] = p.windSpeed * 0.37f;
    f->params5[2] = (float)p.mode;
    // GeneralsX @feature 光影升级2.0 A3/A8 28/10/2026: post FX strengths. The projection rows below let
    // both effects march the sun direction in view space and project samples back to the screen (the
    // post pass only had the INVERSE projection before).
    f->fx0[0] = p.contactShadows;
    f->fx0[1] = p.godRays;
    f->fx0[2] = p.aoQuality;
    // QA: 1 renders the projected sun UV straight to the frame, which is how A8 was decided
    // ("sun is off-screen in top-down views" vs "projection convention still off").
    {
        static int s_probe = -1;
        if (s_probe < 0) { const char* e = getenv("GENERALSX_VK_SUNPROBE"); s_probe = (e && *e) ? atoi(e) : 0; }
        f->fx0[3] = s_probe ? 1.0f : 0.0f;
    }
    for (int i = 0; i < 16; ++i) f->heatPos[i] = X.fx.heat[i];
    f->heatInfo[0] = (float)X.fx.heatCount;
    {
        static float s_str = -1.0f;
        if (s_str < 0.0f) { const char* e = getenv("GENERALSX_VK_HEAT_STRENGTH"); s_str = (e && *e) ? (float)atof(e) : 1.0f; }
        f->heatInfo[1] = s_str;
    }
    f->heatInfo[2] = 0.0f;
    f->heatInfo[3] = (float)(X.st.framesBegun % 200000ull) * (1.0f / 60.0f);
    // QA: red = mode/3, green = density*1000, blue = final fog amount. Diagnostic only; the normal
    // path writes zero and the shader branch is uniform.
    static int s_fogDebug = -1;
    if (s_fogDebug < 0) {
        const char* ev = getenv("GENERALSX_VK_FOG_DEBUG");
        s_fogDebug = (ev != nullptr) ? atoi(ev) : 0;
        if (s_fogDebug < 0) s_fogDebug = 0;
        if (s_fogDebug > 6) s_fogDebug = 6;
        if (s_fogDebug != 0) {
            fprintf(stderr, "[VKFOGDBG] debug=%d invView rows: %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f | %.2f %.2f %.2f %.2f viewport=%.0fx%.0f\n",
                s_fogDebug, inv[0].X, inv[0].Y, inv[0].Z, inv[0].W,
                inv[1].X, inv[1].Y, inv[1].Z, inv[1].W,
                inv[2].X, inv[2].Y, inv[2].Z, inv[2].W,
                inv[3].X, inv[3].Y, inv[3].Z, inv[3].W,
                f->viewport[2], f->viewport[3]);
            fflush(stderr);
        }
    }
    f->params5[3] = (float)s_fogDebug;
    if (s_fogDebug != 0) {
        static int s_layoutProbe = 2;
        if (s_layoutProbe > 0) {
            --s_layoutProbe;
            fprintf(stderr, "[VKFOGDBG] sizeof=%u params5off=%u wrote params5.w=%.1f mode=%.1f density=%.5f\n",
                (unsigned)sizeof(VKPostFogUbo),
                (unsigned)((const char*)&f->params5[0] - (const char*)f),
                (double)f->params5[3], (double)f->params5[2], (double)f->params[0]);
            fflush(stderr);
        }
    }

    if (X.shViewport[2] > 0.0f && X.shViewport[3] > 0.0f) {
        f->viewport[0] = X.shViewport[0]; f->viewport[1] = X.shViewport[1];
        f->viewport[2] = X.shViewport[2]; f->viewport[3] = X.shViewport[3];
    } else {
        // X.w/X.h still hold the SHADOW MAP size at this point (the shadow pass ran last), so the old
        // fallback handed the shader 4096x4096 for a 2432x1368 target. Every ray was then reconstructed
        // from a quarter-width NDC range and landed next to the camera. The scene target size is the only
        // sane default here.
        f->viewport[0] = 0.0f; f->viewport[1] = 0.0f;
        f->viewport[2] = (float)(X.sceneW ? X.sceneW : X.w);
        f->viewport[3] = (float)(X.sceneH ? X.sceneH : X.h);
    }
}

void vkpost_update_set(void* sceneView, void* depthView, void* shadowView)
{
    vkpost_fill_fog();

    VkDescriptorImageInfo ii[8] = {};
    VkWriteDescriptorSet wr[9] = {};
    int n = 0;
    for (int b = 0; b < 7; ++b, ++n) {
        // Binding 1 is the scene depth: D32_SFLOAT has no linear-filtering guarantee, so it gets the
        // nearest sampler. Sampling it with the linear one returned 1.0 on every pixel (the pass behaved
        // as if the whole screen were the far plane) on this driver.
        ii[n].sampler = (b == 1 && P.samplerNearest) ? P.samplerNearest : P.sampler;
        ii[n].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[n].imageView = (b == 0) ? (VkImageView)sceneView
                        : (b == 1) ? (VkImageView)depthView
                        : (P.bloomView[b - 2] ? P.bloomView[b - 2] : (VkImageView)sceneView);
        wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[n].dstSet = P.sets[X.slot];
        wr[n].dstBinding = (uint32_t)b;
        wr[n].descriptorCount = 1;
        wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[n].pImageInfo = &ii[n];
    }

    VkDescriptorBufferInfo bi = {};
    bi.buffer = X.fogBuf;
    bi.offset = (VkDeviceSize)X.slot * VKPOST_FOG_SLOT_BYTES;
    bi.range = sizeof(VKPostFogUbo);
    wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[n].dstSet = P.sets[X.slot];
    wr[n].dstBinding = 7;
    wr[n].descriptorCount = 1;
    wr[n].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[n].pBufferInfo = &bi;
    ++n;

    // Binding 8: the R32F light-space depth map. The post sampler is linear/clamp, which the receiver
    // also uses; the shader still treats out-of-box samples as fully lit.
    VkDescriptorImageInfo sh = {};
    sh.sampler = P.sampler;
    sh.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    sh.imageView = shadowView ? (VkImageView)shadowView : (VkImageView)sceneView;
    wr[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr[n].dstSet = P.sets[X.slot];
    wr[n].dstBinding = 8;
    wr[n].descriptorCount = 1;
    wr[n].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[n].pImageInfo = &sh;
    ++n;

    G.vkUpdateDescriptorSets(dev_(), (uint32_t)n, wr, 0, nullptr);
}

void vkpost_destroy()
{
    VkDevice dev = dev_();
    if (!dev) return;
    for (size_t i = 0; i < P.fbs.size(); ++i)
        if (P.fbs[i]) G.vkDestroyFramebuffer(dev, P.fbs[i], nullptr);
    P.fbs.clear(); P.fbViews.clear();
    if (P.overlayFb) G.vkDestroyFramebuffer(dev, P.overlayFb, nullptr);
    if (P.overlayRp) G.vkDestroyRenderPass(dev, P.overlayRp, nullptr);
    if (P.overlayPipe) G.vkDestroyPipeline(dev, P.overlayPipe, nullptr);
    P.overlayFb = nullptr; P.overlayRp = nullptr; P.overlayPipe = nullptr;
    P.overlayView = nullptr; P.overlayFormat = VK_FORMAT_UNDEFINED; P.overlayExtent = {};
    if (P.pipe) G.vkDestroyPipeline(dev, P.pipe, nullptr);
    if (P.pl) G.vkDestroyPipelineLayout(dev, P.pl, nullptr);
    if (P.dsl) G.vkDestroyDescriptorSetLayout(dev, P.dsl, nullptr);
    if (P.rp) G.vkDestroyRenderPass(dev, P.rp, nullptr);
    if (P.sampler) G.vkDestroySampler(dev, P.sampler, nullptr);
    if (P.samplerNearest) G.vkDestroySampler(dev, P.samplerNearest, nullptr);
    if (P.pool) G.vkDestroyDescriptorPool(dev, P.pool, nullptr);
    if (P.vs) G.vkDestroyShaderModule(dev, P.vs, nullptr);
    if (P.fs) G.vkDestroyShaderModule(dev, P.fs, nullptr);
    P.pipe = nullptr; P.pl = nullptr; P.dsl = nullptr; P.rp = nullptr;
    P.sampler = nullptr; P.samplerNearest = nullptr; P.pool = nullptr; P.vs = nullptr; P.fs = nullptr;
    memset(P.sets, 0, sizeof(P.sets));
    P.built = false;
}

bool vkpost_build(VkFormat fmt, VkExtent2D e, unsigned count)
{
    VkDevice dev = dev_();
    if (!dev || !count || !e.width || !e.height) return false;

    // One colour attachment, cleared to opaque black on load - that is what paints the letterbox bars
    // around the pillarbox rect, exactly like the explicit vkCmdClearColorImage on the blit path.
    VkAttachmentDescription att = {};
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref = {};
    ref.attachment = 0; ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkSubpassDescription sp = {};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &att;
    rpci.subpassCount = 1; rpci.pSubpasses = &sp;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    if (!chk("vkCreateRenderPass(vkpost)", G.vkCreateRenderPass(dev, &rpci, nullptr, &P.rp))) return false;

    P.fbs.resize(count, nullptr);
    P.fbViews.resize(count, nullptr);
    for (unsigned i = 0; i < count; ++i) {
        VkImageView view = core_()->SwapView(i);
        if (!view) return false;
        VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fci.renderPass = P.rp;
        fci.attachmentCount = 1; fci.pAttachments = &view;
        fci.width = e.width; fci.height = e.height; fci.layers = 1;
        if (!chk("vkCreateFramebuffer(vkpost)", G.vkCreateFramebuffer(dev, &fci, nullptr, &P.fbs[i]))) return false;
        P.fbViews[i] = view;
    }

    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smci.codeSize = sizeof(VKPOST_VERT_SPV); smci.pCode = VKPOST_VERT_SPV;
    if (!chk("vkCreateShaderModule(vkpost vert)", G.vkCreateShaderModule(dev, &smci, nullptr, &P.vs))) return false;
    smci.codeSize = sizeof(VKPOST_FRAG_SPV); smci.pCode = VKPOST_FRAG_SPV;
    if (!chk("vkCreateShaderModule(vkpost frag)", G.vkCreateShaderModule(dev, &smci, nullptr, &P.fs))) return false;

    // Binding 1 is the scene depth for the contact AO; bindings 2..6 are the five bloom mip levels,
    // binding 7 the volumetric-fog UBO and binding 8 this frame's sun depth map.
    VkDescriptorSetLayoutBinding binding[9] = {};
    for (int b = 0; b < 9; ++b) {
        binding[b].binding = (uint32_t)b;
        binding[b].descriptorType = (b == 7) ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                             : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding[b].descriptorCount = 1;
        binding[b].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dlci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlci.bindingCount = 9; dlci.pBindings = binding;
    if (!chk("vkCreateDescriptorSetLayout(vkpost)", G.vkCreateDescriptorSetLayout(dev, &dlci, nullptr, &P.dsl)))
        return false;

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128u };   // p0..p2 + inverse projection + debug
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plci.setLayoutCount = 1; plci.pSetLayouts = &P.dsl;
    plci.pushConstantRangeCount = 1; plci.pPushConstantRanges = &pcr;
    if (!chk("vkCreatePipelineLayout(vkpost)", G.vkCreatePipelineLayout(dev, &plci, nullptr, &P.pl))) return false;

    VkDescriptorPoolSize psz[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)VKGEO_SLOT_COUNT * 8u },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)VKGEO_SLOT_COUNT }
    };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpci.maxSets = (uint32_t)VKGEO_SLOT_COUNT;
    dpci.poolSizeCount = 2; dpci.pPoolSizes = psz;
    if (!chk("vkCreateDescriptorPool(vkpost)", G.vkCreateDescriptorPool(dev, &dpci, nullptr, &P.pool))) return false;

    VkDescriptorSetLayout layouts[VKGEO_SLOT_COUNT] = {};
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) layouts[i] = P.dsl;
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dsai.descriptorPool = P.pool;
    dsai.descriptorSetCount = (uint32_t)VKGEO_SLOT_COUNT;
    dsai.pSetLayouts = layouts;
    if (!chk("vkAllocateDescriptorSets(vkpost)", G.vkAllocateDescriptorSets(dev, &dsai, P.sets))) return false;

    // Clamp at the edges: FXAA and the bloom ring sample outside [0,1] near the border, and wrapping would
    // pull the opposite edge into the picture. Linear is what the off-screen blit used when it had to
    // scale; at a 1:1 dst rect the sample lands on texel centres and returns the same bytes.
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    if (!chk("vkCreateSampler(vkpost)", G.vkCreateSampler(dev, &sci, nullptr, &P.sampler))) return false;
    {
        VkSamplerCreateInfo nsi = sci;
        nsi.magFilter = VK_FILTER_NEAREST;
        nsi.minFilter = VK_FILTER_NEAREST;
        if (!chk("vkCreateSampler(vkpost nearest)", G.vkCreateSampler(dev, &nsi, nullptr, &P.samplerNearest))) return false;
    }

    VkPipelineShaderStageCreateInfo stg[2] = {};
    stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = P.vs; stg[0].pName = "main";
    stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = P.fs; stg[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = (VkColorComponentFlags)(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2; gpi.pStages = stg;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
    gpi.layout = P.pl; gpi.renderPass = P.rp; gpi.subpass = 0;
    if (!chk("vkCreateGraphicsPipelines(vkpost)", G.vkCreateGraphicsPipelines(dev, nullptr, 1, &gpi, nullptr, &P.pipe)))
        return false;

    P.format = fmt; P.extent = e;
    return true;
}

// The swapchain changes shape on a resize that already waited the device idle, and its views are recreated
// with it, so any difference means the cached framebuffers belong to dead images and the whole pass is
// rebuilt - the same lifetime rule the draw layer's passes use.
bool vkpost_ensure(VkFormat fmt, VkExtent2D e)
{
    if (P.failed) return false;
    const unsigned count = core_()->SwapImageCount();
    if (!count) return false;   // no swapchain images yet: not a failure, just nothing to build against
    bool same = P.built && P.format == fmt && P.extent.width == e.width && P.extent.height == e.height
             && P.fbs.size() == (size_t)count;
    if (same) {
        for (unsigned i = 0; i < count; ++i)
            if (P.fbViews[i] != core_()->SwapView(i)) { same = false; break; }
    }
    if (same) return true;

    vkpost_destroy();
    if (!vkpost_build(fmt, e, count)) {
        P.failed = true;
        vkpost_destroy();
        VKWP1_LOG("[VKPOST] off: pass creation failed - present keeps the plain blit");
        return false;
    }
    P.built = true;
    VKWP1_LOG("[VKPOST] ready: %ux%u scene -> %ux%u swapchain, fxaa=%u bloom=%.2f",
        X.sceneW, X.sceneH, e.width, e.height,
        0u, (double)DX8Wrapper::VK_Post_Bloom());   // present-side AA retired: FXAA lives in the overlay resolve
    return true;
}

bool vkpost_render(VkImage sceneImage, void* sceneView, unsigned sceneW, unsigned sceneH,
                   uint32_t imageIndex, unsigned dx, unsigned dy, unsigned dw, unsigned dh)
{
    if (!P.built || imageIndex >= P.fbs.size() || !P.fbs[imageIndex]) return false;
    if (!sceneView || !sceneW || !sceneH || !dw || !dh) return false;

    // The depth binding must always be written; without a scene-depth mirror the scene view stands in
    // and the AO strength is zeroed below, so the shader never samples it.
    VKRT_Info dInfo = {};
    void* depthView = sceneView;
    if (X.depthKey != nullptr && VKRT_Query(X.depthKey, &dInfo) && dInfo.is_depth && dInfo.sample_view) {
        VKRT_Cmd_Transition(X.cmd, X.depthKey, VKRT_LAYOUT_SHADER_READ_ONLY);
        depthView = dInfo.sample_view;
    }
    // P6 bloom mip chain: build the five levels before the composite pass samples them. Auto exposure
    // reads the smallest level, so it forces the chain even when the bloom slider is at zero.
    if (DX8Wrapper::VK_Post_Bloom() > 0.0001f || DX8Wrapper::VK_Auto_Exposure())
        vkpost_bloom_levels((VkImage)sceneImage, X.sceneColorKey, sceneW, sceneH);
    // Fog shafts sample the sun map the receiver just produced. When the shadow pass skipped this
    // frame, keep the scene view as a valid dummy; the zero light matrix disables the term in the shader.
    VKRT_Info shInfo = {};
    void* shadowView = sceneView;
    if (X.shHaveVP && VKRT_Query(X.shColorKey, &shInfo) && shInfo.view) {
        VKRT_Cmd_Transition(X.cmd, X.shColorKey, VKRT_LAYOUT_SHADER_READ_ONLY);
        shadowView = shInfo.view;
    }
    vkpost_update_set(sceneView, depthView, shadowView);
    {
        static int s_postProbe = 0;
        if (s_postProbe < 3) {
            ++s_postProbe;
            fprintf(stderr, "[VKPOST] depth bind: key=%p query=%d isDepth=%d sampleView=%p (scene %ux%u)\n",
                X.depthKey, dInfo.view ? 1 : 0, dInfo.is_depth,
                dInfo.sample_view, sceneW, sceneH);
            fflush(stderr);
        }
    }

    VkClearValue clr = {};
    clr.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = P.rp;
    rbi.framebuffer = P.fbs[imageIndex];
    rbi.renderArea = { { 0, 0 }, { P.extent.width, P.extent.height } };
    rbi.clearValueCount = 1; rbi.pClearValues = &clr;
    G.vkCmdBeginRenderPass(X.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = { (float)dx, (float)dy, (float)dw, (float)dh, 0.0f, 1.0f };
    G.vkCmdSetViewport(X.cmd, 0, 1, &vp);
    VkRect2D sc = { { (int32_t)dx, (int32_t)dy }, { dw, dh } };
    G.vkCmdSetScissor(X.cmd, 0, 1, &sc);
    G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pipe);
    G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl, 0, 1, &P.sets[X.slot], 0, nullptr);

    // p0 = (bloom gain, 1/sceneW, 1/sceneH, fxaa on); p1 = (threshold, radius in scene texels, knee, unused).
    // The gain is the Options.ini BloomIntensity (0..1), so the slider keeps its retail meaning; the
    // threshold sits above mid-grey so only genuinely bright pixels halo.
    // AO / grading knobs: env-gated so both can be A/B-ed without a rebuild.
    const float aoStrength = vkpost_ao_strength();
    const float gradeOn = vkpost_grade_on() ? 1.0f : 0.0f;
    // Auto exposure feeds the tonemap block's EV, so it is meaningless (and would run as a plain
    // exposure pass) on the LDR parity path; same gate as the curve itself.
    const float autoExp = (X.hdrOn && DX8Wrapper::VK_Auto_Exposure()) ? 1.0f : 0.0f;
    const float lutOn = DX8Wrapper::VK_Color_LUT() ? 1.0f : 0.0f;
    const float sharpen = DX8Wrapper::VK_Sharpen();
    // GeneralsX @feature 26/10/2026 HDR: the scene target holds linear radiance once HDR is on. The
    // post shader needs to know so it skips the legacy pow(2.2) decode and encodes on output.
    const float linearIn = X.hdrOn ? 1.0f : 0.0f;
    // No scene-depth mirror means no AO; the dummy binding above would read as colour.
    const bool haveAoDepth = dInfo.is_depth && dInfo.sample_view;
    Matrix4x4 projM;
    DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, projM);
    const Matrix4x4 invProjM = projM.Inverse();
    const float aoScale = fabsf(projM[1].Y) * 0.5f * (float)sceneH;
    float aoDebug = 0.0f;
    if (const char* dbg = getenv("GENERALSX_VK_AO_DEBUG")) aoDebug = (atof(dbg) > 0.0) ? 1.0f : 0.0f;
    float pc[32] = {
        DX8Wrapper::VK_Post_Bloom(),
        sceneW ? 1.0f / (float)sceneW : 0.0f,
        sceneH ? 1.0f / (float)sceneH : 0.0f,
        0.0f,   // present-side AA retired (see the overlay resolve, which runs before the UI)
        linearIn > 0.5f ? 1.0f : 0.62f, vkpost_aces_pre(), autoExp, haveAoDepth ? aoStrength : 0.0f,
        gradeOn + 2.0f * lutOn + 4.0f * linearIn, aoScale, 1.12f, 1.06f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        aoDebug, X.hdrExposure, (float)X.hdrTonemap, sharpen
    };
    for (int r = 0; r < 4; ++r) {
        pc[12 + r * 4 + 0] = invProjM[r].X;
        pc[12 + r * 4 + 1] = invProjM[r].Y;
        pc[12 + r * 4 + 2] = invProjM[r].Z;
        pc[12 + r * 4 + 3] = invProjM[r].W;
    }
    G.vkCmdPushConstants(X.cmd, P.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(pc), pc);
    G.vkCmdDraw(X.cmd, 3, 1, 0, 0);
    G.vkCmdEndRenderPass(X.cmd);
    return true;
}

// ---------------------------------------------------------------------------
// P6 overlay resolve: 3D scene -> LDR overlay, before the UI is drawn.
// ---------------------------------------------------------------------------
void vkpost_overlay_destroy()
{
    VkDevice dev = dev_();
    if (!dev) return;
    if (P.overlayFb) G.vkDestroyFramebuffer(dev, P.overlayFb, nullptr);
    if (P.overlayRp) G.vkDestroyRenderPass(dev, P.overlayRp, nullptr);
    if (P.overlayPipe) G.vkDestroyPipeline(dev, P.overlayPipe, nullptr);
    P.overlayFb = nullptr; P.overlayRp = nullptr; P.overlayPipe = nullptr;
    P.overlayView = nullptr; P.overlayFormat = VK_FORMAT_UNDEFINED; P.overlayExtent = {};
}

bool vkpost_overlay_build(VkImageView view, VkFormat fmt, VkExtent2D e)
{
    VkDevice dev = dev_();
    if (!dev || !view || !e.width || !e.height) return false;

    VkAttachmentDescription att = {};
    att.format = fmt;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = {};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1; sp.pColorAttachments = &ref;
    VkSubpassDependency dep = {};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpci.attachmentCount = 1; rpci.pAttachments = &att;
    rpci.subpassCount = 1; rpci.pSubpasses = &sp;
    rpci.dependencyCount = 1; rpci.pDependencies = &dep;
    if (!chk("vkCreateRenderPass(vkpost overlay)", G.vkCreateRenderPass(dev, &rpci, nullptr, &P.overlayRp)))
        return false;

    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fci.renderPass = P.overlayRp;
    fci.attachmentCount = 1; fci.pAttachments = &view;
    fci.width = e.width; fci.height = e.height; fci.layers = 1;
    if (!chk("vkCreateFramebuffer(vkpost overlay)", G.vkCreateFramebuffer(dev, &fci, nullptr, &P.overlayFb)))
        return false;

    VkPipelineShaderStageCreateInfo stg[2] = {};
    stg[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = P.vs; stg[0].pName = "main";
    stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = P.fs; stg[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vpi = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpi.viewportCount = 1; vpi.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = (VkColorComponentFlags)(VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dy.dynamicStateCount = 2; dy.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gpi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gpi.stageCount = 2; gpi.pStages = stg;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
    gpi.layout = P.pl; gpi.renderPass = P.overlayRp; gpi.subpass = 0;
    if (!chk("vkCreateGraphicsPipelines(vkpost overlay)",
             G.vkCreateGraphicsPipelines(dev, nullptr, 1, &gpi, nullptr, &P.overlayPipe)))
        return false;

    P.overlayView = view; P.overlayFormat = fmt; P.overlayExtent = e;
    VKWP1_LOG("[VKPOST] overlay ready: %ux%u", e.width, e.height);
    return true;
}

bool vkpost_overlay_ensure(VkImageView view, VkFormat fmt, VkExtent2D e)
{
    if (P.overlayFb && P.overlayView == view && P.overlayFormat == fmt
        && P.overlayExtent.width == e.width && P.overlayExtent.height == e.height)
        return true;
    vkpost_overlay_destroy();
    return vkpost_overlay_build(view, fmt, e);
}

bool vkpost_overlay_render(VkImage sceneImage, void* sceneView, unsigned sceneW, unsigned sceneH)
{
    if (!P.overlayFb || !P.overlayPipe || !sceneView || !sceneW || !sceneH) return false;

    // GENERALSX_VKPOST=0 keeps the overlay resolve (UI separation) but makes it a plain copy.
    const bool fxOn = vkpost_env_on();
    VKRT_Info dInfo = {};
    void* depthView = sceneView;
    if (X.depthKey != nullptr && VKRT_Query(X.depthKey, &dInfo) && dInfo.is_depth && dInfo.sample_view) {
        VKRT_Cmd_Transition(X.cmd, X.depthKey, VKRT_LAYOUT_SHADER_READ_ONLY);
        depthView = dInfo.sample_view;
    }
    {
        static int s_dbgDepth = 0;
        if (s_dbgDepth < 3) {
            ++s_dbgDepth;
            fprintf(stderr, "[VKPOST] overlay depth: key=%p query=%d isDepth=%d view=%p sample=%p\n",
                X.depthKey, X.depthKey ? 1 : 0, dInfo.is_depth, dInfo.view, dInfo.sample_view);
            fflush(stderr);
        }
    }
    if (fxOn && (DX8Wrapper::VK_Post_Bloom() > 0.0001f || DX8Wrapper::VK_Auto_Exposure()))
        vkpost_bloom_levels(sceneImage, X.sceneColorKey, sceneW, sceneH);
    VKRT_Info shInfo = {};
    void* shadowView = sceneView;
    if (X.shHaveVP && VKRT_Query(X.shColorKey, &shInfo) && shInfo.view) {
        VKRT_Cmd_Transition(X.cmd, X.shColorKey, VKRT_LAYOUT_SHADER_READ_ONLY);
        shadowView = shInfo.view;
    }
    vkpost_update_set(sceneView, depthView, shadowView);

    VkClearValue clr = {};
    clr.color.float32[3] = 1.0f;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = P.overlayRp;
    rbi.framebuffer = P.overlayFb;
    rbi.renderArea = { { 0, 0 }, { P.overlayExtent.width, P.overlayExtent.height } };
    rbi.clearValueCount = 1; rbi.pClearValues = &clr;
    G.vkCmdBeginRenderPass(X.cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp = { 0.0f, 0.0f, (float)P.overlayExtent.width, (float)P.overlayExtent.height, 0.0f, 1.0f };
    G.vkCmdSetViewport(X.cmd, 0, 1, &vp);
    VkRect2D sc = { { 0, 0 }, { P.overlayExtent.width, P.overlayExtent.height } };
    G.vkCmdSetScissor(X.cmd, 0, 1, &sc);
    G.vkCmdBindPipeline(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.overlayPipe);
    G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, P.pl, 0, 1, &P.sets[X.slot], 0, nullptr);

    const float aoStrength = fxOn ? vkpost_ao_strength() : 0.0f;
    const float gradeOn = (fxOn && vkpost_grade_on()) ? 1.0f : 0.0f;
    // Auto exposure feeds the tonemap block's EV, so it is meaningless (and would run as a plain
    // exposure pass) on the LDR parity path; same gate as the curve itself.
    const float autoExp = (fxOn && X.hdrOn && DX8Wrapper::VK_Auto_Exposure()) ? 1.0f : 0.0f;
    const float lutOn = (fxOn && DX8Wrapper::VK_Color_LUT()) ? 1.0f : 0.0f;
    const float sharpen = fxOn ? DX8Wrapper::VK_Sharpen() : 0.0f;
    // GeneralsX @feature 26/10/2026 HDR: see the present-post site above. fxOn=0 keeps the pass a
    // plain display-space copy, which is what GENERALSX_VKPOST=0 asks for.
    const float linearIn = (fxOn && X.hdrOn) ? 1.0f : 0.0f;
    const bool haveAoDepth = dInfo.is_depth && dInfo.sample_view;
    Matrix4x4 projM;
    DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, projM);
    const Matrix4x4 invProjM = projM.Inverse();
    const float aoScale = fabsf(projM[1].Y) * 0.5f * (float)sceneH;
    float aoDebug = 0.0f;
    if (const char* dbg = getenv("GENERALSX_VK_AO_DEBUG")) aoDebug = (atof(dbg) > 0.0) ? 1.0f : 0.0f;
    float pc[32] = {
        fxOn ? DX8Wrapper::VK_Post_Bloom() : 0.0f,
        sceneW ? 1.0f / (float)sceneW : 0.0f,
        sceneH ? 1.0f / (float)sceneH : 0.0f,
        (fxOn && X.aaMode == 1) ? 1.0f : 0.0f,
        linearIn > 0.5f ? 1.0f : 0.62f, vkpost_aces_pre(), autoExp, haveAoDepth ? aoStrength : 0.0f,
        gradeOn + 2.0f * lutOn + 4.0f * linearIn, aoScale, 1.12f, 1.06f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,
        fxOn ? aoDebug : 0.0f, fxOn ? X.hdrExposure : 0.0f,
        fxOn ? (float)X.hdrTonemap : 0.0f, sharpen
    };
    for (int r = 0; r < 4; ++r) {
        pc[12 + r * 4 + 0] = invProjM[r].X;
        pc[12 + r * 4 + 1] = invProjM[r].Y;
        pc[12 + r * 4 + 2] = invProjM[r].Z;
        pc[12 + r * 4 + 3] = invProjM[r].W;
    }
    G.vkCmdPushConstants(X.cmd, P.pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, (uint32_t)sizeof(pc), pc);
    G.vkCmdDraw(X.cmd, 3, 1, 0, 0);
    G.vkCmdEndRenderPass(X.cmd);
    return true;
}

} // anon

// Sky cubemap entry points need external linkage; the implementation above lives in the post pass's
// anonymous namespace so its state stays private to this translation unit.
bool VKDraw_Sky_Set_Faces(TextureClass* north, TextureClass* east, TextureClass* south,
                          TextureClass* west, TextureClass* top)
{
    return sky_set_faces_internal(north, east, south, west, top);
}

bool VKDraw_Sky_Render(float tintR, float tintG, float tintB)
{
    return sky_render_internal(tintR, tintG, tintB);
}

// ===========================================================================
// P6 "底片/镜头": resolve the 3D scene into the LDR overlay, then point the
// recorder at that overlay so the UI is drawn after tonemap/bloom/AA.
// ===========================================================================
static void vkpost_overlay_skip(const char* why)
{
    static int s = 0;
    if (s < 8) {
        ++s;
        VKWP1_LOG("[VKPOST] overlay skipped: %s", why ? why : "?");
    }
}

bool VKDraw_Begin_UI()
{
    if (!X.frameOpen || !X.cmd) { vkpost_overlay_skip("no-frame"); return false; }
    if (X.overlayActive) return false;
    if (!vkdraw_overlay_wanted()) { vkpost_overlay_skip("parity"); return false; }

    vk_queue_lock();
    close_pass();

    VKRT_Info si = {}, oi = {};
    if (!VKRT_Query(X.sceneColorKey, &si) || !si.view ||
        !VKRT_Query(X.overlayColorKey, &oi) || !oi.view) {
        X.overlayFailed = 1;
        vkpost_overlay_skip("target-query");
        // close_pass() above closed the scene pass; reopen it so the UI still lands on the scene.
        open_pass();
        vk_queue_unlock();
        return false;
    }
    VKRT_Cmd_Transition(X.cmd, X.sceneColorKey, VKRT_LAYOUT_SHADER_READ_ONLY);
    if (X.depthKey) VKRT_Cmd_Transition(X.cmd, X.depthKey, VKRT_LAYOUT_SHADER_READ_ONLY);

    // vkpost_ensure() builds the shared shader/layout/descriptor state and the swapchain framebuffers;
    // the overlay variant only adds its own pass/framebuffer/pipeline on top. A failure here leaves the
    // old single-target path intact (the caller just keeps drawing the UI into the scene mirror, and the
    // present chain refuses to post-process it because X.overlayFailed is set).
    bool ok = vkpost_ensure(core_()->SwapFormat(), core_()->Extent());
    if (ok) {
        ok = vkpost_overlay_ensure((VkImageView)oi.view, (VkFormat)oi.format,
                                   VkExtent2D{ oi.width, oi.height });
    }
    if (ok) {
        // The overlay is a VKRT image that starts life UNDEFINED; its pass declares SHADER_READ as the
        // initial layout, so put it there explicitly before the first resolve.
        VKRT_Cmd_Transition(X.cmd, X.overlayColorKey, VKRT_LAYOUT_SHADER_READ_ONLY);
        ok = vkpost_overlay_render((VkImage)si.image, si.view, si.width, si.height);
    }
    if (!ok) {
        X.overlayFailed = 1;
        // Fall back to the scene target: the UI must still be drawn, and present must not post-process
        // it (X.overlayFailed). Reopen the scene pass that close_pass() closed above.
        X.colorKey = X.sceneColorKey;
        X.depthKey = X.sceneDepthKey;
        X.w = si.width; X.h = si.height;
        X.overlayActive = 0;
        open_pass();
        if (X.openPassWarns < 4) {
            ++X.openPassWarns;
            VKWP1_LOG("[VKPOST] overlay resolve refused - UI stays on the scene target");
        }
        vk_queue_unlock();
        return false;
    }

    X.colorKey = X.overlayColorKey;
    X.depthKey = nullptr;
    X.w = oi.width; X.h = oi.height;
    X.overlayActive = 1;
    X.overlayFailed = 0;
    ok = open_pass();
    if (!ok) {
        X.colorKey = X.sceneColorKey;
        X.depthKey = X.sceneDepthKey;
        X.w = si.width; X.h = si.height;
        X.overlayActive = 0;
        X.overlayFailed = 1;
        open_pass();
    }
    {
        static int s_log = 0;
        if (X.overlayActive && s_log < 4) {
            ++s_log;
            VKWP1_LOG("[VKPOST] overlay active: scene %ux%u -> overlay %ux%u aa=%d hdr=%d tonemap=%d exp=%.2f",
                si.width, si.height, oi.width, oi.height, X.aaMode, X.hdrOn, X.hdrTonemap, (double)X.hdrExposure);
        }
    }
    vk_queue_unlock();
    return ok;
}

// ===========================================================================
// public: end frame, present, stats
// ===========================================================================
void VKDraw_End_Frame()
{
    VK_Forward_End_Frame();
    if (!X.frameOpen || !X.cmd) return;
    VKDraw_UsScope scUs(s_vkdrawEndUs);
    // GeneralsX @feature Qoder 19/10/2026 R-02 support: the isolated single-frame pure-white VKLUMA
    // samples need to be told apart - a frame that recorded zero draws presents whatever the scene
    // mirror holds (stale or mirror-init content), a frame that recorded draws paints white itself.
    if (!s_frameDraws) {
        ++s_blankFrames;
        if (s_blankFrames <= 8) {
            VKWP1_LOG("wedge5 blank frame %llu: zero draws recorded, present will repeat mirror content",
                X.st.framesBegun);
        }
    }
    vk_queue_lock();
    VKSTEP(20);
    close_pass();
    VKTRACE("frame %llu: end pass closed", X.st.framesBegun);

    // The image the player sees is the overlay when the P6 resolve ran (3D tonemap/AA/scale + UI on
    // top), otherwise the scene mirror, exactly as before. It leaves the last pass in SHADER_READ_ONLY;
    // the blit and the forensic copies read it as TRANSFER_SRC. Begin_Frame guarantees the scene mirror
    // exists, so a missing one here means the mirror layer was torn down underneath us.
    void* presentKey = X.overlayActive ? (void*)X.overlayColorKey : (void*)X.sceneColorKey;
    {
        static int s_presentLog = 0;
        if (s_presentLog < 4) {
            ++s_presentLog;
            VKWP1_LOG("[VKPOST] present source: overlay=%d key=%p", X.overlayActive, presentKey);
        }
    }
    VKRT_Info scene = {};
    const bool haveScene = VKRT_Query(presentKey, &scene) && scene.image;

    if (haveScene) {
        uint32_t img = 0;
        const VkSemaphore avail = core_()->SlotImageAvailable(X.slot);
        VKTRACE("frame %llu: acquire", X.st.framesBegun);
        VKSTEP(21);
        const VkResult ar = G.vkAcquireNextImageKHR(dev_(), core_()->SwapHandle(), ~0ull, avail, nullptr, &img);
        VKTRACE("frame %llu: acquire done result=%d", X.st.framesBegun, (int)ar);
        if (ar == VK_SUBOPTIMAL_KHR || ar == VK_ERROR_OUT_OF_DATE_KHR) X.resizePending = 1;
        if (ar == VK_ERROR_DEVICE_LOST) {
            X.broken = true;
            VKWP1_LOG("wedge5 acquire reports device loss - draw path stays off");
        }
        if (ar == VK_SUBOPTIMAL_KHR || ar == VK_SUCCESS) {
            X.acquired = img; X.haveImage = 1;
            if (ar == VK_SUBOPTIMAL_KHR && X.swapWarns < 4) {
                ++X.swapWarns;
                VKWP1_LOG("wedge5 swapchain suboptimal - presenting this frame, resize expected");
            }
            const VkImage dst = core_()->SwapImage(img);
            const VkExtent2D e = core_()->Extent();

            // Destination is the pillarbox fit rect when the facade published one, clamped into the image;
            // no rect means the picture fills the window, which is the common case.
            unsigned dx = X.rectW ? X.rectX : 0u;
            unsigned dy = X.rectW ? X.rectY : 0u;
            unsigned dw = X.rectW ? X.rectW : e.width;
            unsigned dh = X.rectW ? X.rectH : e.height;
            if (dx + dw > e.width)  dw = e.width > dx ? e.width - dx : 1u;
            if (dy + dh > e.height) dh = e.height > dy ? e.height - dy : 1u;

            // GeneralsX @feature Qoder 20/10/2026 present-side post: when either knob is on, one full-screen
            // draw samples the scene mirror and writes the swapchain image itself, replacing the copy below.
            // Everything it needs already exists at this point, and any failure leaves the blit as the whole
            // path - the fallback is the behaviour the game shipped with, not a degraded variant of it.
            bool postDone = false;
            // P0 24/10/2026: present-side post is QA-only now. The overlay resolve is the only production
            // post path, and it runs before the UI; a frame that missed the overlay (load screen, resolve
            // failure) must not post-process the HUD. GENERALSX_VK_PRESENTPOST=1 restores the old path.
            static const bool s_presentPost = (getenv("GENERALSX_VK_PRESENTPOST") != nullptr)
                && (atoi(getenv("GENERALSX_VK_PRESENTPOST")) != 0);
            const bool wantPost = s_presentPost && !X.overlayActive && !X.overlayFailed && vkpost_env_on()
                && (DX8Wrapper::VK_Post_Bloom() > 0.0001f
                    || vkpost_ao_strength() > 0.0f || vkpost_grade_on());
            if (wantPost && vkpost_ensure(core_()->SwapFormat(), e)) {
                VKRT_Cmd_Transition(X.cmd, presentKey, VKRT_LAYOUT_SHADER_READ_ONLY);
                if (vkpost_render((VkImage)scene.image, scene.view, scene.width, scene.height,
                                  img, dx, dy, dw, dh)) {
                    postDone = true;
                    ++P.frames;
                    if (P.frames <= 3ull || (P.frames % 600ull) == 0ull) {
                        VKWP1_LOG("[VKPOST] frame %llu: fxaa=%u bloom=%.2f scene=%ux%u dst=%u,%u %ux%u",
                            X.st.framesBegun, 0u,
                            (double)DX8Wrapper::VK_Post_Bloom(), scene.width, scene.height, dx, dy, dw, dh);
                    }
                    // Luma and the forensic shot are copies out of the scene and both name the source
                    // layout themselves, so put the image back where those copies expect it.
                    VKRT_Cmd_Transition(X.cmd, presentKey, VKRT_LAYOUT_TRANSFER_SRC);
                    VKCore_Luma_Record(X.cmd, (VkImage)scene.image, scene.width, scene.height, X.slot);
                    // GENERALSX_VKSHOT_POST=1 captures the post-processed swapchain image instead of the
                    // scene mirror; the present-side effects are invisible to the scene shot.
                    static const bool s_shotPost = (getenv("GENERALSX_VKSHOT_POST") != nullptr)
                        && (atoi(getenv("GENERALSX_VKSHOT_POST")) != 0);
                    if (s_shotPost)
                        VKCore_Shot_RecordSwap(X.cmd, (void*)dst, e.width, e.height, X.slot, X.st.framesBegun + 1ull);
                    else
                        VKCore_Shot_Record(X.cmd, (VkImage)scene.image, scene.width, scene.height, X.slot, X.st.framesBegun + 1ull);
                    VKTRACE("frame %llu: post+luma recorded", X.st.framesBegun);
                } else {
                    ++P.fallbacks;
                    if (P.fallbacks <= 4ull)
                        VKWP1_LOG("[VKPOST] frame %llu: render refused - blitting instead", X.st.framesBegun);
                }
            }

            if (!postDone) {
                // The image leaves the last pass in SHADER_READ_ONLY; a blit source has to be TRANSFER_SRC.
                VKRT_Cmd_Transition(X.cmd, presentKey, VKRT_LAYOUT_TRANSFER_SRC);

                // The swapchain image's layout is not tracked by the render-target state machine (it is not a
                // mirror), so these two barriers are recorded against the layouts the present chain itself owns.
                VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toDst.image = dst;
                toDst.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                G.vkCmdPipelineBarrier(X.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &toDst);

                // Black first, always: UNDEFINED on entry means the area the blit does not cover would otherwise
                // show whatever the previous swapchain image held, and the letterbox bars are exactly that area.
                VkClearColorValue black = {};
                black.float32[3] = 1.0f;
                VkImageSubresourceRange whole = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                G.vkCmdClearColorImage(X.cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1u, &whole);

                VkImageBlit region = {};
                region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.srcOffsets[0] = { 0, 0, 0 };
                region.srcOffsets[1] = { (int32_t)scene.width, (int32_t)scene.height, 1 };
                region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                region.dstOffsets[0] = { (int32_t)dx, (int32_t)dy, 1 };
                region.dstOffsets[1] = { (int32_t)(dx + dw), (int32_t)(dy + dh), 1 };
                // The D3D path picked POINT for a 1:1 copy and LINEAR when it had to scale, so the blit filter
                // follows the same rule and a non-integer render resolution does not turn into a mosaic.
                const bool exactSize = (dw == scene.width && dh == scene.height && dx == 0u && dy == 0u);
                // vkCmd* entry points return void, so the blit is recorded unconditionally and counted; a failure
                // here surfaces as the validation layer or the following submit rather than a VkResult.
                VKSTEP(22);
                G.vkCmdBlitImage(X.cmd, (VkImage)scene.image,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &region, exactSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
                ++X.st.sceneCopies;
                // The wedge-7 scene sampler rides the same read: the blit above left the scene image in
                // TRANSFER_SRC_OPTIMAL, so a few copies out of it need no barrier of their own.
                VKCore_Luma_Record(X.cmd, (VkImage)scene.image, scene.width, scene.height, X.slot);
                VKCore_Shot_Record(X.cmd, (VkImage)scene.image, scene.width, scene.height, X.slot, X.st.framesBegun + 1ull);
                VKTRACE("frame %llu: blit+luma recorded", X.st.framesBegun);

                VkImageMemoryBarrier toPresent = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
                toPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toPresent.dstAccessMask = 0;
                toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toPresent.image = dst;
                toPresent.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                G.vkCmdPipelineBarrier(X.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &toPresent);
            }
        } else {
            // No image acquired, so nothing to present and nothing to wait on: the command buffer still
            // gets submitted below so the slot's fence is signaled and the NEXT frame cannot deadlock on it.
            X.haveImage = 0;
            ++X.st.framesFailed;
            if (X.swapWarns < 4) {
                ++X.swapWarns;
                VKWP1_LOG("wedge5 acquire failed (VkResult=%d) - frame submitted, not presented", (int)ar);
            }
        }
    } else {
        X.haveImage = 0;
        ++X.st.framesFailed;
        if (X.swapWarns < 4) {
            ++X.swapWarns;
            VKWP1_LOG("wedge5 no scene mirror at end of frame - nothing to present");
        }
    }

    VKTRACE("frame %llu: end command buffer", X.st.framesBegun);
    VKSTEP(23);
    l10_frame_end();
    chk("wedge5 endCmd", G.vkEndCommandBuffer(X.cmd));
    X.frameOpen = 0;
    X.cmd = nullptr;
    // GeneralsX @feature 26/10/2026 HDR: the next frame starts on the scene target. Clearing here (not
    // only in the frame prologue) matters because the water reflection pass runs before Begin_Frame and
    // would otherwise inherit last frame's UI state and sample its textures without sRGB decode.
    X.overlayActive = 0;
    vk_queue_unlock();
    VKTRACE("frame %llu: end released", X.st.framesBegun);
}

bool VKDraw_Present()
{
    if (!X.built || !core_()) return false;
    VKDraw_UsScope scUs(s_vkdrawPresentUs);
    if (X.frameOpen) VKDraw_End_Frame();
    vk_queue_lock();
    const uint32_t slot = X.slot;
    const VkCommandBuffer cmd = core_()->SlotCmd(slot);
    const VkFence f = core_()->SlotFence(slot);
    // The wait covers both present paths: the blit writes in the transfer stage, the post pass writes in
    // the colour-attachment stage. Naming both is cheap and lets the semaphore gate whichever ran.
    const VkPipelineStageFlags waitAt = (VkPipelineStageFlags)(VK_PIPELINE_STAGE_TRANSFER_BIT |
                                                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    const VkSemaphore done = core_()->SlotRenderingDone(slot);
    const VkSemaphore avail = core_()->SlotImageAvailable(slot);

    // The image-available semaphore is waited, and the rendering-done one signalled, only for a frame that
    // actually acquired an image: signalling it without presenting would leave a stale signal for the next
    // frame to consume. The fence, in contrast, is always signaled - Begin_Frame waits it unconditionally.
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    if (X.haveImage) {
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &avail; si.pWaitDstStageMask = &waitAt;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &done;
    }
    VKTRACE("frame %llu: submit (haveImage=%u)", X.st.framesBegun, (unsigned)X.haveImage);
    VKSTEP(24);
    const bool ok = chk("wedge5 submit", G.vkQueueSubmit(core_()->Queue(), 1, &si, f));
    VKTRACE("frame %llu: submit done ok=%d", X.st.framesBegun, ok ? 1 : 0);
    if (!ok) ++X.st.framesFailed;
    // The frame that was just submitted owns the slot it was recorded into; the next one has to use the
    // next. VulkanCore::EndFrame advances its own counter, and this path does not run it, so without this
    // line every wedge-5 frame reuses slot 0: the fence wait serialises the loop, the rotating geometry
    // regions stop rotating, and the mirror reaper never ages.
    if (ok) core_()->Advance_Frame();

    bool presented = false;
    if (ok && X.haveImage) {
        const VkSwapchainKHR sw = core_()->SwapHandle();
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &done;
        pi.swapchainCount = 1; pi.pSwapchains = &sw; pi.pImageIndices = &X.acquired;
    VKTRACE("frame %llu: present", X.st.framesBegun);
        VKSTEP(25);
        const VkResult pr = G.vkQueuePresentKHR(core_()->Queue(), &pi);
    VKTRACE("frame %llu: present done result=%d", X.st.framesBegun, (int)pr);
        presented = (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR);
        if (presented) ++X.st.framesPresented; else ++X.st.framesFailed;
        X.haveImage = 0;
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            X.resizePending = 1;
            if (X.swapWarns < 4) {
                ++X.swapWarns;
                VKWP1_LOG("wedge5 present returned %d - caller should recreate the swapchain", (int)pr);
            }
            vk_queue_unlock();
            return false;
        }
        if (!chk("wedge5 present", pr)) {
            if (pr == VK_ERROR_DEVICE_LOST) { X.broken = true; VKWP1_LOG("wedge5 present reports device loss - draw path stays off"); }
            vk_queue_unlock();
            return false;
        }
    }
    vk_queue_unlock();
    return presented;
}

bool VKDraw_Resize_Needed() { return X.resizePending != 0; }

void VKDraw_Get_Stats(VKDraw_Stats* out) { if (out) *out = X.st; }

// GeneralsX @bugfix Qoder 19/10/2026 R-02 forensics: the wrap history of the three constant rings
// (0 vertex, 1 pixel, 2 fixed-function). 'wraps' counts every in-frame reuse-from-base - each one
// silently overwrites the payloads the frame's earliest batches still point at - and 'peak' is the
// current frame's window high-water, which says how close the ring is to its next wrap. The shot
// writer prints both per frame, so a frame can be classified by what its own bytes did.
void VKDraw_Get_Const_Wraps(unsigned long long wraps[3])
{
    for (int i = 0; i < 3; ++i) wraps[i] = X.built ? X.cr[i].wraps : 0ull;
}

void VKDraw_Get_Const_Spills(unsigned long long spills[3])
{
    for (int i = 0; i < 3; ++i) spills[i] = X.built ? X.cr[i].spills : 0ull;
}

void VKDraw_Get_Const_Peaks(unsigned long long peaks[3])
{
    for (int i = 0; i < 3; ++i) peaks[i] = X.built ? X.cr[i].peak : 0ull;
}

unsigned long long VKDraw_Get_Const_WindowCap(int ring)
{
    return (X.built && ring >= 0 && ring < 3) ? X.cr[ring].winCap : 0ull;
}

static bool vk_fallback_strict()
{
    static int s = -1;
    if (s < 0) s = (getenv("GENERALSX_VK_NO_FALLBACK") != nullptr &&
                    atoi(getenv("GENERALSX_VK_NO_FALLBACK")) != 0) ? 1 : 0;
    return s != 0;
}

void VKDraw_Log_Stats()
{
    unsigned long long cup = 0, cre = 0, cwr = 0, csp = 0;
    for (int i = 0; i < 3; ++i) { cup += X.cr[i].uploads; cre += X.cr[i].reuses; cwr += X.cr[i].wraps; csp += X.cr[i].spills; }
    VKWP1_LOG("VKDRAW stats: draws=%llu skipped=%llu bad=%llu pso=%llu/%llu/%llu passes=%u fbs=%u "
        "geoBuffers=%llu+%llu rot=%llu sets=%llu/%llu clears=%llu deferred=%llu copies=%llu frames=%llu/%llu/%llu "
        "fans=%llu topoRefusals=%llu bounds=%llu/%llu geoZombie=%llu/%llu consts=%llu/%llu/%llu spills=%llu waitUs=%lld "
        "submitUs=%lld/%llu max=%lld endUs=%lld presentUs=%lld segUs=%lld/%lld/%lld/%lld blank=%llu scanSkips=%llu idxCache=%llu/%llu",
        X.st.draws, X.st.drawsSkipped, X.st.drawsBad, X.st.psoCreated, X.st.psoReused, X.st.psoFailed,
        (unsigned)X.passes.size(), (unsigned)X.fbs.size(),
        X.st.buffersCreated, X.st.buffersDropped, X.st.ringRotations,
        X.st.setsBuilt, X.st.setsReused, X.st.clears, X.st.clearsDeferred, X.st.sceneCopies,
        X.st.framesBegun, X.st.framesPresented, X.st.framesFailed,
        X.st.fanRewrites, X.st.topologyRefusals, X.st.boundsRefusals, X.st.indexValueRefusals,
        X.st.backingsRetired, X.st.backingsReaped, cup, cre, cwr, csp, s_vkdrawWaitUs,
        s_vkdrawSubmitUs, s_vkdrawSubmitN, s_vkdrawSubmitMaxUs, s_vkdrawEndUs, s_vkdrawPresentUs,
        s_subPreUs, s_subPassUs, s_subScanUs, s_subTailUs, (unsigned long long)s_blankFrames,
        s_scanSkips, s_idxCacheHits, s_idxCacheScans);
    // Wedge 8: report the registry alongside the draw counters so a run shows both halves of the gate -
    // how many compiled shaders were matched, and how many modules the matched tags stood up.
    VKPgm_Stats pg = {};
    VKPgm_Get_Stats(&pg);
    if (pg.notes) {
        VKWP1_LOG("VKPGM stats: modules=%u notes=%llu matched=%llu unmatched=%llu lookups=%llu hits=%llu resets=%llu ffDraws=%llu ffRefused=%llu",
            (unsigned)X.pgmModules.size(), pg.notes, pg.matched, pg.unmatched, pg.lookups, pg.hits, pg.resets,
            pg.ffDraws, pg.ffRefused);
    }
    // Phase 5A fallback audit: one line per stats window. ffDraws/ffRefused count the retail FF
    // interpreter actually rendering/refusing; unmatched counts shader sources with no translation.
    // The five-face sky mesh fallback is retired, so there is no sky-fallback counter any more.
    {
        VKPgm_Stats pg = {};
        VKPgm_Get_Stats(&pg);
        fprintf(stderr, "[VKFALLBACK] frames=%llu draws=%llu ffDraws=%llu ffRefused=%llu unmatched=%llu retailShadow=%d strict=%d\n",
            X.st.framesBegun, X.st.draws, pg.ffDraws, pg.ffRefused, pg.unmatched,
            X.shOn ? 0 : 1, vk_fallback_strict() ? 1 : 0);
        if (vk_fallback_strict() && (pg.unmatched > 0ull || pg.ffRefused > 0ull || !X.shOn)) {
            fprintf(stderr, "[VKFALLBACK][STRICT] violation: unmatched=%llu refused=%llu retailShadow=%d\n",
                pg.unmatched, pg.ffRefused, X.shOn ? 0 : 1);
        }
        fflush(stderr);
    }
    // GeneralsX @feature Qoder 18/10/2026 wedge 8 path census, kept off the 4096-draw line so the log
    // stays readable: every 65536 recorded draws still lands inside a 45 s benchmark, and the per-tag
    // enter/ok/slot0null rows are cumulative rather than per-window.
    if ((X.st.draws & 65535ull) == 0ull) VKPgm_Log_Census();
}

#endif // SAGE_USE_VULKAN_CORE
#endif // _WIN32
