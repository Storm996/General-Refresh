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
#include "VkShadersDraw.h"
#include "VkShadersFF.h"
#include <cstdio>
#include <cstring>
#include <vector>

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
    // D3DCULL_CW (2) culls clockwise-wound triangles, D3DCULL_CCW (3) the counter-clockwise ones. Vulkan
    // culls front or back instead, so which bit means what depends on how front-facing is declared.
    // DX8 has no D3DRS_FRONTFACE at all (d3d8types.h stops at the cull mode), so the facade pins
    // front_ccw = 0 - clockwise is front, which is the reading under which the engine's default
    // D3DCULL_CW is the ordinary back-face cull the current DXVK stack shows.
    const bool cwIsFront = (front_ccw == 0);
    if (d3d_cull == 2) return cwIsFront ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_FRONT_BIT;
    return cwIsFront ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_BACK_BIT;
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
    unsigned long long slotCap = 0;    // one in-flight slot's window
    unsigned long long cur[VKGEO_SLOT_COUNT] = {};
    unsigned long long lastOff[VKGEO_SLOT_COUNT] = {};
    unsigned           lastBytes[VKGEO_SLOT_COUNT] = {};
    unsigned long long uploads = 0;    // distinct payloads written
    unsigned long long reuses = 0;     // draws that matched the previous payload
    unsigned long long wraps = 0;      // slot window exhausted mid-frame (payloads older than a frame lost)
};

struct VKDrawState {
    bool  envOn = false, envChecked = false, built = false, broken = false;

    std::vector<VKGeoBack> geo;
    std::vector<VKGeoZombie> geoZombies;

    VkShaderModule   vs = nullptr, ps = nullptr;
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the fixed-function vertex stage and the
    // default-vertex stream that backs whichever of its inputs the batch's FVF does not provide. Both are
    // nullptr when GENERALSX_VKFFVERT is off, which keeps the wedge-5 mechanism path intact: a submit that
    // carries no FF state block (the probes) still gets the passthrough vertex stage.
    VkShaderModule   ffVs = nullptr;
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

    // Per-in-flight-slot descriptor storage: reset with the slot, so a set is never recycled while the
    // frame that bound it is still in flight.
    VkDescriptorPool pool[VKGEO_SLOT_COUNT] = {};
    std::vector<VKDrawSetEntry> sets[VKGEO_SLOT_COUNT];

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

    // Render resolution of the scene mirror (0 = follow the swapchain) and the destination rect of the
    // end-of-frame blit (0 width = full extent). Together these are what keeps a game rendered at 1024x768
    // inside a 2560x1440 window letterboxed exactly like the D3D9 path does.
    unsigned sceneW = 0, sceneH = 0;
    unsigned rectX = 0, rectY = 0, rectW = 0, rectH = 0;

    VKDraw_Stats st = {};
    unsigned long long submitTicks = 0;
};
VKDrawState X;

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
    const uint32_t mt = memtype_(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
// constant rings (wedge 5.1)
// ---------------------------------------------------------------------------
// Why not one reservation per draw: the register budgets are 4 KiB vertex + 3.5 KiB pixel + 1 KiB of
// lights, so a per-draw copy is a 9 KiB memcpy on every batch and a ring large enough for one frame
// would be hundreds of megabytes. Why not one region rewritten in place: the frame that bound it may
// still be in flight. The middle ground is a slot-partitioned ring with a payload-equality check -
// consecutive batches share constants far more often than they share textures, so the copy rate is the
// number of distinct payloads, not the number of draws.
bool const_ring_create(VKDrawConstRing& r, unsigned long long cap)
{
    unsigned long long slot = cap / (unsigned long long)VKGEO_SLOT_COUNT;
    slot = (slot / X.constAlign) * X.constAlign;      // a window must start on a legal dynamic offset
    if (!slot) return false;
    r.cap = slot * (unsigned long long)VKGEO_SLOT_COUNT;
    r.slotCap = slot;
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) {
        r.cur[i] = (unsigned long long)i * slot;
        r.lastOff[i] = 0; r.lastBytes[i] = 0;
    }
    return make_host_buffer(r.cap, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &r.buf, &r.mem, &r.map);
}

void const_ring_destroy(VKDrawConstRing& r)
{
    free_host_buffer(r.buf, r.mem, &r.map);
    r.buf = nullptr; r.mem = nullptr; r.map = nullptr; r.cap = 0; r.slotCap = 0;
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) { r.cur[i] = 0; r.lastOff[i] = 0; r.lastBytes[i] = 0; }
}

// Start this slot's window over. Called from Begin_Frame, which has already waited the slot's fence, so
// nothing still in flight can be reading the bytes being handed out again.
void const_ring_reset(VKDrawConstRing& r, unsigned slot)
{
    r.cur[slot] = (unsigned long long)slot * r.slotCap;
    r.lastBytes[slot] = 0;
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
    const unsigned long long end  = base + r.slotCap;
    const unsigned long long need = ((unsigned long long)budget + align - 1) & ~(align - 1);
    if (need > r.slotCap) return false;

    // Reuse test: the same bytes already sit at the offset this slot last wrote. That is the whole point
    // of the ring - consecutive batches overwhelmingly repeat their constants, so the copy rate tracks
    // the number of distinct payloads rather than the number of draws.
    if (r.lastBytes[slot] == used &&
        memcmp((const char*)r.map + r.lastOff[slot], src, (size_t)used) == 0) {
        *offset = r.lastOff[slot]; ++r.reuses; return true;
    }
    unsigned long long off = (r.cur[slot] + align - 1) & ~(align - 1);
    if (off + need > end) { off = base; ++r.wraps; }   // window full: this frame's oldest payloads are gone
    char* dst = (char*)r.map + off;
    memcpy(dst, src, (size_t)used);
    if (used < budget) memset(dst + used, 0, (size_t)(budget - used));
    r.cur[slot] = off + need;
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

VKGeoBack* geo_find(void* key)
{
    for (size_t i = 0; i < X.geo.size(); ++i) if (X.geo[i].key == key) return &X.geo[i];
    return nullptr;
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
    if (!b.buf && !b.mem) { b.map = nullptr; b.cap = 0; b.slotBytes = 0; return; }
    VKGeoZombie z;
    z.buf = b.buf; z.mem = b.mem; z.map = b.map;
    z.slotsLeft = VKGEO_SLOT_COUNT + 1u;
    X.geoZombies.push_back(z);
    b.buf = nullptr; b.mem = nullptr; b.map = nullptr; b.cap = 0; b.slotBytes = 0;
    ++X.st.backingsRetired;
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
    for (size_t i = 0; i < X.geo.size(); ++i) if (X.geo[i].buf == buf) return X.geo[i].cap;
    return 0;
}

// The host mapping behind a bound geometry buffer, so a submit can look at the bytes it is about to hand
// the GPU. nullptr when the buffer is not one this layer mapped.
const void* geo_map_of(VkBuffer buf)
{
    if (!buf) return nullptr;
    if (buf == X.upBuf) return X.upMap;
    if (buf == X.fanBuf) return X.fanMap;
    for (size_t i = 0; i < X.geo.size(); ++i) if (X.geo[i].buf == buf) return X.geo[i].map;
    return nullptr;
}


// ---------------------------------------------------------------------------
// render passes and framebuffers
// ---------------------------------------------------------------------------
unsigned pass_for(unsigned colorFmt, unsigned depthFmt, unsigned samples)
{
    unsigned long long sig = (unsigned long long)colorFmt | ((unsigned long long)depthFmt << 16)
                           | ((unsigned long long)samples << 32) | (1ull << 48);
    for (size_t i = 0; i < X.passes.size(); ++i)
        if (X.passes[i].sig == sig) return (unsigned)i;

    VkAttachmentDescription att[2] = {};
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
bool pso_key_equal(const VKDraw_PSO_Key& a, const VKDraw_PSO_Key& b)
{
    return a.shader_bits == b.shader_bits && a.fvf == b.fvf && a.light_mask == b.light_mask &&
           a.vs_tag == b.vs_tag && a.ps_tag == b.ps_tag && a.topology == b.topology &&
           a.rt_format == b.rt_format && a.depth_format == b.depth_format &&
           a.sample_count == b.sample_count && a.cull_mode == b.cull_mode &&
           a.front_ccw == b.front_ccw && a.fill_mode == b.fill_mode &&
           a.depth_test == b.depth_test && a.depth_write == b.depth_write && a.depth_cmp == b.depth_cmp &&
           a.stencil_test == b.stencil_test && a.blend_enable == b.blend_enable &&
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
    const unsigned wantFF = (X.ffVs && s->ff) ? 1u : 0u;
    for (size_t i = 0; i < X.psos.size(); ++i) {
        const VKDrawPSO& e = X.psos[i];
        if (e.ff == wantFF && e.attr_count == s->attr_count && pso_key_equal(e.key, s->key) &&
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
    if (wantFF) {
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
    // Stencil stays off the state object until the wedge-9 shadow receivers need it: the census measured
    // stencil_test == 0 on every keying draw, so enabling it here would only mint variants nothing uses.
    dss.stencilTestEnable = VK_FALSE;

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
    stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stg[0].module = wantFF ? X.ffVs : X.vs; stg[0].pName = "main";
    stg[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stg[1].module = X.ps; stg[1].pName = "main";
    gpi.stageCount = 2; gpi.pStages = stg;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vpi; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss; gpi.pColorBlendState = &cb; gpi.pDynamicState = &dy;
    gpi.layout = X.layout; gpi.renderPass = X.passes[X.curPass].rp; gpi.subpass = 0;

    VkPipeline pso = nullptr;
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
    VKDrawPSO e; e.key = k; e.attr_count = s->attr_count; e.pso = pso; e.ff = wantFF;
    if (s->attr_count) memcpy(e.attrs, s->attrs, sizeof(VKDraw_Attr) * s->attr_count);
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
bool set_for(void* const views[4], void* const samplers[4], const VKDraw_Const_Bind& cb, VkDescriptorSet* out)
{
    std::vector<VKDrawSetEntry>& cache = X.sets[X.slot];
    for (size_t i = 0; i < cache.size(); ++i) {
        const VKDrawSetEntry& e = cache[i];
        if (memcmp(e.views, views, sizeof(void*) * VKDRAW_MAX_TEX_SLOTS) == 0 &&
            memcmp(e.samplers, samplers, sizeof(void*) * VKDRAW_MAX_TEX_SLOTS) == 0 &&
            memcmp(e.cbuf, cb.buf, sizeof(void*) * 3) == 0 &&
            memcmp(e.cbytes, cb.bytes, sizeof(unsigned long long) * 3) == 0) {
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

    VkWriteDescriptorSet w[4] = {};
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
    G.vkUpdateDescriptorSets(dev_(), 4, w, 0, nullptr);

    VKDrawSetEntry e;
    for (int i = 0; i < VKDRAW_MAX_TEX_SLOTS; ++i) { e.views[i] = views[i]; e.samplers[i] = samplers[i]; }
    for (int i = 0; i < 3; ++i) { e.cbuf[i] = cb.buf[i]; e.cbytes[i] = cb.bytes[i]; }
    e.set = set;
    cache.push_back(e);
    ++X.st.setsBuilt;
    *out = set;
    return true;
}

// ---------------------------------------------------------------------------
// pass boundaries
// ---------------------------------------------------------------------------
void close_pass()
{
    if (!X.inPass) return;
    G.vkCmdEndRenderPass(X.cmd);
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
    VKRT_Cmd_Transition(X.cmd, X.colorKey, VKRT_LAYOUT_SHADER_READ_ONLY);
    if (depthFmt) VKRT_Cmd_Transition(X.cmd, X.depthKey, VKRT_LAYOUT_DEPTH_STENCIL_ATTACHMENT);

    const unsigned passIdx = pass_for(ci.format, depthFmt, 1u);
    if (passIdx == 0xFFFFFFFFu) return false;
    VkFramebuffer fb = fb_for(passIdx, ci.view, depthFmt ? di.view : nullptr, X.w, X.h);
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
void geo_life(void* key, unsigned long long cap, int static_buffer, int born)
{
    static unsigned created = 0, dropped = 0;
    const unsigned n = born ? created : dropped;
    if (n >= 1000u) return;
    if (born) ++created; else ++dropped;
    VKWP1_LOG("wedge5 backing %s: key=%p cap=%llu static=%u counts=%u/%u",
        born ? "created" : "dropped", key, cap, (unsigned)static_buffer, created, dropped);
}

void* VKGeo_Back_Lock(void* key, unsigned int bytes, unsigned int stride, int dynamic, int discard)
{
    if (!VKDraw_Ready()) { geo_lock_fail(key, bytes, dynamic, GEOLF_NOT_READY); return nullptr; }
    if (!key || !bytes) { if (key) geo_lock_fail(key, bytes, dynamic, GEOLF_EMPTY); return nullptr; }
    vk_queue_lock();

    const unsigned long long slotBytes = (bytes + 255ull) & ~255ull;
    const unsigned long long cap = dynamic ? slotBytes * VKGEO_SLOT_COUNT : slotBytes;

    VKGeoBack* b = geo_find(key);
    if (b && b->cap < cap) {            // the object grew: rebuild wider, the old data is being rewritten
        VKWP1_LOG("wedge5 backing rebuilt: key=%p cap=%llu->%llu", key, b->cap, cap);
        geo_retire(*b);
        b = nullptr;
    }
    if (!b) {
        VKGeoBack n = {};
        n.key = key;
        const VkBufferUsageFlags usage = (stride ? VK_BUFFER_USAGE_VERTEX_BUFFER_BIT
                                                 : VK_BUFFER_USAGE_INDEX_BUFFER_BIT)
                                       | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (!make_host_buffer(cap, usage, &n.buf, &n.mem, &n.map)) {
            geo_lock_fail(key, bytes, dynamic, GEOLF_ALLOC);
            vk_queue_unlock();
            return nullptr;
        }
        n.cap = cap; n.slotBytes = slotBytes; n.dynamic = dynamic ? 1 : 0;
        X.geo.push_back(n);
        b = &X.geo[X.geo.size() - 1];
        geo_life(key, cap, dynamic ? 0 : 1, 1);
    }

    // Rotating slot for a dynamic buffer; a static one always writes region 0. 'discard' is the D3D
    // DISCARD hint and has no extra meaning here beyond the rotation the slot index already gives.
    unsigned slot = 0;
    if (b->dynamic) {
        slot = core_()->FrameSlot();
        if (slot != b->slot) ++X.st.ringRotations;
        b->slot = slot;
    }
    b->stride = stride;
    b->curOffset = (unsigned long long)slot * b->slotBytes;
    b->curBytes = slotBytes;
    (void)discard;
    vk_queue_unlock();
    return (char*)b->map + b->curOffset;
}

bool VKGeo_Back_Bind(void* key, void** vk_buffer, unsigned long long* offset, unsigned int* stride,
                     unsigned int* bytes)
{
    if (!key) return false;
    for (size_t i = 0; i < X.geo.size(); ++i) {
        const VKGeoBack& b = X.geo[i];
        if (b.key != key) continue;
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
    for (size_t i = 0; i < X.geo.size(); ++i) {
        if (X.geo[i].key != key) continue;
        geo_life(key, X.geo[i].cap, X.geo[i].dynamic ? 0 : 1, 0);
        geo_retire(X.geo[i]);
        X.geo.erase(X.geo.begin() + (long)i);
        ++X.st.buffersDropped;
        break;
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
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2048u * 4u },
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
    // The scratch ring for DrawPrimitiveUP is bound as a vertex buffer AND as the synthesised sequential
    // index buffer for the same draw, so it needs both usage bits from creation.
    bool ok = vkdraw_build_layout_and_shaders() &&
              make_host_buffer(X.zeroVtxBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &X.zeroVtx, &X.zeroVtxMem, nullptr) &&
              make_host_buffer(X.zeroPixBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &X.zeroPix, &X.zeroPixMem, nullptr) &&
              make_host_buffer(X.zeroLightBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &X.zeroLight, &X.zeroLightMem, nullptr) &&
              (X.constAlign = core_()->UniformAlign(), X.constAlign != 0) &&
              const_ring_create(X.cr[0], 16ull << 20) &&
              const_ring_create(X.cr[1], 16ull << 20) &&
              const_ring_create(X.cr[2], 4ull << 20) &&
              make_host_buffer(VKDrawState::SCRATCH_CAP, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &X.fanBuf, &X.fanMem, &X.fanMap) &&
              make_host_buffer(VKDrawState::SCRATCH_CAP,
                  (VkBufferUsageFlags)(VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT),
                  &X.upBuf, &X.upMem, &X.upMap);
    vk_queue_unlock();
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
    VKWP1_LOG("wedge5 layer ok: scene target %ux%u, slots=%d, scratch=2x%llu MiB",
        X.w, X.h, VKGEO_SLOT_COUNT, VKDrawState::SCRATCH_CAP >> 20);
    return true;
}

void VKDraw_Destroy()
{
    VkDevice dev = dev_();
    vk_queue_lock();
    // Drain before freeing: pipelines, framebuffers and the geometry backings may all still be referenced
    // by a frame in flight. Shutdown happens once per process, so the wait costs nothing.
    if (dev) chk("wedge5 destroyWait", G.vkDeviceWaitIdle(dev));
    X.frameOpen = 0; X.cmd = nullptr; X.inPass = 0; X.haveImage = 0;
    if (dev) {
        for (size_t i = 0; i < X.psos.size(); ++i) G.vkDestroyPipeline(dev, X.psos[i].pso, nullptr);
        for (size_t i = 0; i < X.fbs.size(); ++i) G.vkDestroyFramebuffer(dev, X.fbs[i].fb, nullptr);
        for (size_t i = 0; i < X.passes.size(); ++i) G.vkDestroyRenderPass(dev, X.passes[i].rp, nullptr);
        for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) if (X.pool[i]) G.vkDestroyDescriptorPool(dev, X.pool[i], nullptr);
        if (X.layout) G.vkDestroyPipelineLayout(dev, X.layout, nullptr);
        if (X.vs) G.vkDestroyShaderModule(dev, X.vs, nullptr);
        if (X.ps) G.vkDestroyShaderModule(dev, X.ps, nullptr);
        if (X.ffVs) G.vkDestroyShaderModule(dev, X.ffVs, nullptr);
    }
    X.psos.clear(); X.fbs.clear(); X.passes.clear();
    for (int i = 0; i < VKGEO_SLOT_COUNT; ++i) { X.pool[i] = nullptr; X.sets[i].clear(); }
    X.layout = nullptr; X.vs = nullptr; X.ps = nullptr; X.ffVs = nullptr;
    free_host_buffer(X.dummyVb, X.dummyVbMem, nullptr);
    X.dummyVb = nullptr; X.dummyVbMem = nullptr;
    for (size_t i = 0; i < X.geo.size(); ++i) geo_destroy(X.geo[i]);
    X.geo.clear();
    geo_reap_all();          // the device idle above makes every retired backing safe to destroy now
    vk_queue_unlock();
    free_host_buffer(X.zeroVtx, X.zeroVtxMem, nullptr);
    free_host_buffer(X.zeroPix, X.zeroPixMem, nullptr);
    free_host_buffer(X.zeroLight, X.zeroLightMem, nullptr);
    for (int i = 0; i < 3; ++i) const_ring_destroy(X.cr[i]);
    free_host_buffer(X.fanBuf, X.fanMem, &X.fanMap);
    free_host_buffer(X.upBuf, X.upMem, &X.upMap);
    X.zeroVtx = nullptr; X.zeroPix = nullptr; X.zeroLight = nullptr;
    X.fanBuf = nullptr; X.upBuf = nullptr;
    X.built = false; X.broken = false; X.inPass = 0; X.frameOpen = 0; X.cmd = nullptr;
}

// ===========================================================================
// public: targets, clears
// ===========================================================================
void VKDraw_Set_Targets(void* color_key, void* depth_key, unsigned width, unsigned height)
{
    if (!VKDraw_Ready() || !color_key) return;
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
    X.sceneW = width; X.sceneH = height;
}

void VKDraw_Set_Present_Rect(unsigned x, unsigned y, unsigned width, unsigned height)
{
    X.rectX = x; X.rectY = y; X.rectW = width; X.rectH = height;
}

void VKDraw_Clear(int clear_color, int clear_z_stencil, const float color[4], float z, unsigned stencil,
                  const unsigned* rect)
{
    if (!VKDraw_Ready() || !X.frameOpen) return;
    vk_queue_lock();
    if (!open_pass()) { vk_queue_unlock(); return; }

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
    vk_queue_unlock();
}

// ===========================================================================
// public: frame boundaries and present
// ===========================================================================

// The scene depth attachment format, picked once by asking the physical device what it will actually
// accept as an attachment. D32_SFLOAT is what D3D9's D3DFMT_D32 maps to and every desktop driver has it;
// the fallbacks keep a laptop part with only D24S8 from failing the whole wedge.
static unsigned vkdraw_scene_depth_format()
{
    static unsigned cached = 0;
    if (cached) return cached;
    const unsigned candidates[3] = { (unsigned)VK_FORMAT_D32_SFLOAT,
                                     (unsigned)VK_FORMAT_D24_UNORM_S8_UINT,
                                     (unsigned)VK_FORMAT_D16_UNORM };
    VkFormatProperties fp = {};
    for (int i = 0; i < 3; ++i) {
        G.vkGetPhysicalDeviceFormatProperties(core_()->Phys(), (VkFormat)candidates[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            cached = candidates[i];
            VKWP1_LOG("wedge5 scene depth format = %u (%s)", cached,
                i == 0 ? "D32_SFLOAT" : (i == 1 ? "D24_UNORM_S8_UINT" : "D16_UNORM"));
            return cached;
        }
    }
    VKWP1_LOG("FAIL wedge5: no depth format usable as an attachment");
    return 0;
}

// Make sure the scene mirrors exist with the shape the frame is about to render into. The shape guard
// inside the mirror layer destroys and rebuilds on a size change, so a resize needs no extra bookkeeping
// here beyond asking for the new extent.
static bool vkdraw_ensure_scene()
{
    const unsigned depthFmt = vkdraw_scene_depth_format();
    if (!depthFmt) return false;
    // B8G8R8A8_UNORM matches the swapchain format the present chain picked, so the blit is a straight copy.
    if (!VKRT_Alloc_Color_Format(X.sceneColorKey, X.w, X.h, (unsigned)VK_FORMAT_B8G8R8A8_UNORM)) return false;
    if (!VKRT_Alloc_Depth_Format(X.sceneDepthKey, X.w, X.h, depthFmt)) return false;
    return true;
}

bool VKDraw_Begin_Frame()
{
    if (!VKDraw_Ready()) return false;
    vk_queue_lock();
    const uint32_t slot = core_()->FrameSlot();
    X.slot = slot;
    VkFence f = core_()->SlotFence(slot);
    bool ok = chk("wedge5 waitFence", G.vkWaitForFences(dev_(), 1, &f, VK_TRUE, ~0ull)) &&
              chk("wedge5 resetFence", G.vkResetFences(dev_(), 1, &f));
    if (ok) {
        // The slot's sets may only be recycled once its fence has passed - that is the whole reason the
        // pools are per-slot.
        if (X.pool[slot]) G.vkResetDescriptorPool(dev_(), X.pool[slot], 0);
        X.sets[slot].clear();
        // One more frame boundary retired, so the backings that were dropped while it was in flight are
        // now old enough that no submitted command buffer can still name them.
        geo_reap();
        // Same rule for the constant rings: this slot's window is dead space now that its frame retired,
        // so start writing at the beginning of it again.
        for (int i = 0; i < 3; ++i) const_ring_reset(X.cr[i], (int)slot);
        VkCommandBuffer cmd = core_()->SlotCmd(slot);
        ok = cmd != nullptr && chk("wedge5 resetCmd", G.vkResetCommandBuffer(cmd, 0));
        if (ok) {
            VkCommandBufferBeginInfo bb = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            bb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            ok = chk("wedge5 beginCmd", G.vkBeginCommandBuffer(cmd, &bb));
        }
        if (ok) {
            X.cmd = cmd;
            X.frameOpen = 1;
            X.inPass = 0;
            X.haveImage = 0;
            X.resizePending = 0;   // re-evaluated by this frame's acquire and present
            ++X.st.framesBegun;

            // Every frame starts on the scene target, whatever render target the last one ended on, and
            // the mirrors are standing up here so the end-of-frame blit always has a source.
            VkExtent2D e = core_()->Extent();
            // The mirror follows the render resolution the facade published (see VKDraw_Set_Scene_Extent):
            // the game can render at a resolution the window is not, and a mirror sized to the window would
            // turn every viewport into a crop of the top-left. No render resolution published = swapchain.
            const unsigned rw = X.sceneW ? X.sceneW : e.width;
            const unsigned rh = X.sceneH ? X.sceneH : e.height;
            if (rw && rh) { X.w = rw; X.h = rh; }
            else if (e.width) { X.w = e.width; X.h = e.height; }
            close_pass();
            X.colorKey = X.sceneColorKey; X.depthKey = X.sceneDepthKey;
            if (!vkdraw_ensure_scene()) {
                ok = false;
                if (X.openPassWarns < 4) {
                    ++X.openPassWarns;
                    VKWP1_LOG("wedge5 scene target failed at %ux%u - frame not recorded", X.w, X.h);
                }
            }
        }
    }
    vk_queue_unlock();
    if (!ok) { ++X.st.framesFailed; X.frameOpen = 0; X.cmd = nullptr; }
    return ok;
}

bool VK_Draw_Submit(const VKDraw_Submit* s)
{
    if (!VKDraw_Ready() || !s || !X.frameOpen) return false;
    ++X.submitTicks;
    vk_queue_lock();

    if (!X.cmd) { vk_queue_unlock(); ++X.st.drawsSkipped; return false; }

    if (!open_pass()) { vk_queue_unlock(); ++X.st.drawsSkipped; return false; }

    VKDraw_Submit mut = *s;
    mut.key.rt_format = 0; mut.key.depth_format = 0;
    VKRT_Info ci;
    if (!VKRT_Query(X.colorKey, &ci)) { vk_queue_unlock(); ++X.st.drawsSkipped; return false; }
    mut.key.rt_format = ci.format;
    if (X.depthKey) { VKRT_Info di; if (VKRT_Query(X.depthKey, &di)) mut.key.depth_format = di.format; }

    if (X.w != mut.viewport[2] || X.h != mut.viewport[3]) {
        // A viewport that does not match the target is legal in D3D (partial clears, shadow passes);
        // clamp to the target so scissor and viewport stay inside the framebuffer.
        mut.viewport[2] = X.w; mut.viewport[3] = X.h;
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
            const void* imap = geo_map_of(bindIb);
            if (!imap) bad = 4;
            else {
                const unsigned short* ip = (const unsigned short*)((const char*)imap + srcIoff);
                for (unsigned i = 0; i < (unsigned)idxWords; ++i) if (ip[i] > maxIndex) maxIndex = ip[i];
                const unsigned long long lastVertexEnd =
                    voff + ((unsigned long long)maxIndex + mut.base_vertex + 1ull) * stride;
                if (lastVertexEnd > vbCap) bad = 5;
            }
        }
        if (bad) {
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
                          (mut.texture_views[2] ? 1u : 0u) + (mut.texture_views[3] ? 1u : 0u)));
        }
    }

    // Triangle fans are the one primitive D3D9 has and Vulkan does not. The fan's own indices are read
    // back out of its host-visible backing (the lock wrote them, they are still there) and expanded into
    // (0,1,2), (0,2,3), ... in the fan scratch ring, which is why this case binds a different buffer.
    if (mut.key.topology == D3D_PT_TRIANGLEFAN) {
        const unsigned short* fanSrc = nullptr;
        for (size_t i = 0; i < X.geo.size(); ++i) {
            if ((void*)X.geo[i].buf == mut.vk_index_buffer && X.geo[i].map) {
                fanSrc = (const unsigned short*)((char*)X.geo[i].map + mut.index_offset + mut.start_index * 2ull);
                break;
            }
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

    void* const views[4] = { mut.texture_views[0], mut.texture_views[1], mut.texture_views[2], mut.texture_views[3] };
    void* const samps[4]  = { mut.texture_samplers[0], mut.texture_samplers[1], mut.texture_samplers[2], mut.texture_samplers[3] };
    VKDraw_Const_Bind cb;
    upload_constants(&mut, &cb);
    VkDescriptorSet set = nullptr;
    if (!set_for(views, samps, cb, &set)) { vk_queue_unlock(); ++X.st.drawsBad; return false; }
    // Three dynamic bindings in layout order: vertex constants, pixel constants, lights.
    const uint32_t dyn[3] = { (uint32_t)cb.off[0], (uint32_t)cb.off[1], (uint32_t)cb.off[2] };
    G.vkCmdBindDescriptorSets(X.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, X.layout, 0, 1, &set, 3, dyn);

    G.vkCmdDrawIndexed(X.cmd, indexCount, 1, 0, (int32_t)mut.base_vertex, 0);
    ++X.st.draws;
    if ((X.st.draws & 4095ull) == 0ull) VKDraw_Log_Stats();
    vk_queue_unlock();
    return true;
}

bool VK_Draw_User_Primitive(unsigned primitive_type, const void* vertex_data, unsigned vertex_count,
                           unsigned stride, unsigned polygon_count, const VKDraw_Attr* attrs,
                           unsigned attr_count, const VKDraw_PSO_Key* key)
{
    if (!VKDraw_Ready() || !X.frameOpen || !vertex_data || !vertex_count || !stride || !key) return false;
    // A caller that describes nothing gets the position-only layout the mechanism stages expect.
    static const VKDraw_Attr s_positionOnly = { 0, 0, 3, 0 };
    if (!attrs || !attr_count) { attrs = &s_positionOnly; attr_count = 1; }
    if (attr_count > VKDRAW_MAX_ATTRS || !polygon_count) return false;
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
    if (bytes + idxBytes > VKDrawState::SCRATCH_CAP) {
        ++X.st.drawsSkipped;
        if (X.streamWarns < 4) {
            ++X.streamWarns;
            VKWP1_LOG("wedge5 user primitive too big for the scratch ring: %llu + %llu bytes",
                bytes, idxBytes);
        }
        return false;
    }

    vk_queue_lock();
    if (X.upCur + bytes + idxBytes > VKDrawState::SCRATCH_CAP) { X.upCur = 0; ++X.st.ringRotations; }
    const unsigned long long vtxOff = X.upCur;
    memcpy((char*)X.upMap + vtxOff, vertex_data, (size_t)bytes);
    X.upCur += (bytes + 255ull) & ~255ull;
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
    X.upCur += (idxBytes + 255ull) & ~255ull;
    vk_queue_unlock();

    VKDraw_Submit s = {};
    s.key = *key;
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
    s.attr_count = attr_count;
    for (unsigned i = 0; i < attr_count; ++i) s.attrs[i] = attrs[i];
    s.viewport[2] = X.w; s.viewport[3] = X.h;
    s.scissor[2] = X.w; s.scissor[3] = X.h;
    return VK_Draw_Submit(&s);
}

// ===========================================================================
// public: end frame, present, stats
// ===========================================================================
void VKDraw_End_Frame()
{
    if (!X.frameOpen || !X.cmd) return;
    vk_queue_lock();
    close_pass();

    // The scene colour mirror is what the player sees: it leaves the pass in SHADER_READ_ONLY and a blit
    // source has to be TRANSFER_SRC, which is an explicit barrier this layer owns. Begin_Frame guarantees
    // the mirror exists, so a missing one here means the mirror layer was torn down underneath us.
    VKRT_Info scene = {};
    const bool haveScene = VKRT_Query(X.sceneColorKey, &scene) && scene.image;
    if (haveScene) VKRT_Cmd_Transition(X.cmd, X.sceneColorKey, VKRT_LAYOUT_TRANSFER_SRC);

    if (haveScene) {
        uint32_t img = 0;
        const VkSemaphore avail = core_()->SlotImageAvailable(X.slot);
        const VkResult ar = G.vkAcquireNextImageKHR(dev_(), core_()->SwapHandle(), ~0ull, avail, nullptr, &img);
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

            // Destination is the pillarbox fit rect when the facade published one, clamped into the image;
            // no rect means the picture fills the window, which is the common case.
            unsigned dx = X.rectW ? X.rectX : 0u;
            unsigned dy = X.rectW ? X.rectY : 0u;
            unsigned dw = X.rectW ? X.rectW : e.width;
            unsigned dh = X.rectW ? X.rectH : e.height;
            if (dx + dw > e.width)  dw = e.width > dx ? e.width - dx : 1u;
            if (dy + dh > e.height) dh = e.height > dy ? e.height - dy : 1u;

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
            G.vkCmdBlitImage(X.cmd, (VkImage)scene.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &region, exactSize ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
            ++X.st.sceneCopies;

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

    chk("wedge5 endCmd", G.vkEndCommandBuffer(X.cmd));
    X.frameOpen = 0;
    X.cmd = nullptr;
    vk_queue_unlock();
}

bool VKDraw_Present()
{
    if (!X.built || !core_()) return false;
    if (X.frameOpen) VKDraw_End_Frame();
    vk_queue_lock();
    const uint32_t slot = X.slot;
    const VkCommandBuffer cmd = core_()->SlotCmd(slot);
    const VkFence f = core_()->SlotFence(slot);
    const VkPipelineStageFlags waitAt = VK_PIPELINE_STAGE_TRANSFER_BIT;   // the blit is the wait's purpose
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
    const bool ok = chk("wedge5 submit", G.vkQueueSubmit(core_()->Queue(), 1, &si, f));
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
        const VkResult pr = G.vkQueuePresentKHR(core_()->Queue(), &pi);
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

void VKDraw_Log_Stats()
{
    unsigned long long cup = 0, cre = 0, cwr = 0;
    for (int i = 0; i < 3; ++i) { cup += X.cr[i].uploads; cre += X.cr[i].reuses; cwr += X.cr[i].wraps; }
    VKWP1_LOG("VKDRAW stats: draws=%llu skipped=%llu bad=%llu pso=%llu/%llu/%llu passes=%u fbs=%u "
        "geoBuffers=%llu+%llu rot=%llu sets=%llu/%llu clears=%llu copies=%llu frames=%llu/%llu/%llu "
        "fans=%llu topoRefusals=%llu bounds=%llu/%llu geoZombie=%llu/%llu consts=%llu/%llu/%llu",
        X.st.draws, X.st.drawsSkipped, X.st.drawsBad, X.st.psoCreated, X.st.psoReused, X.st.psoFailed,
        (unsigned)X.passes.size(), (unsigned)X.fbs.size(),
        X.st.buffersCreated, X.st.buffersDropped, X.st.ringRotations,
        X.st.setsBuilt, X.st.setsReused, X.st.clears, X.st.sceneCopies,
        X.st.framesBegun, X.st.framesPresented, X.st.framesFailed,
        X.st.fanRewrites, X.st.topologyRefusals, X.st.boundsRefusals, X.st.indexValueRefusals,
        X.st.backingsRetired, X.st.backingsReaped, cup, cre, cwr);
}

#endif // SAGE_USE_VULKAN_CORE
#endif // _WIN32
