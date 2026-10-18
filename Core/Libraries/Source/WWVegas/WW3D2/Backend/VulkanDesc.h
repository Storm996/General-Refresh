// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 4: descriptor / constant infrastructure.
//
// What this layer is for. Wedges 1-3 gave the engine a Vulkan device, a present chain and mirrored
// resources (geometry ring, static + dynamic textures, render targets with a layout state machine).
// Every one of those resources was, until now, unbindable: a VkImage with no VkImageView and no
// descriptor is a dead allocation, and a shader can only read constants through a buffer that is
// written into a descriptor set. This header is the seam that makes mirrored resources *consumable*,
// which is the precondition for wedge 5's draw pipeline.
//
// Design decisions recorded here (all were pre-resolved in the approved plan):
//  - one descriptor set layout for the whole engine: {0: combinedImageSampler[4], 1: UBO dynamic
//    (vertex constants), 2: UBO dynamic (pixel constants), 3: UBO dynamic (lights)}. The four texture
//    slots mirror the engine's fixed-function stage usage; stage state itself is NOT part of the
//    pipeline key, so 4 slots plus one interpreted pixel shader (wedge 7) is enough.
//    Binding 3 was an SSBO when this layer only had to prove the ring machinery; wedge 5.1 made it a
//    third dynamic uniform block because a draw path rewrites lights per batch and a static-offset
//    descriptor would force a fresh set for every batch.
//  - constants go through host-visible rings with vkCmdBindDescriptorSets dynamic offsets, not push
//    constants: the engine budgets 256 vertex + 224 pixel vec4 registers, far beyond the 128
//    guaranteed push-constant bytes.
//  - samplers are a cached LUT keyed by the D3DSAMP ordinal tuple, handed to each descriptor write
//    rather than baked into the layout as immutable samplers: immutable samplers would force one set
//    layout per sampler combination and explode the pipeline count.
//
// Vulkan-handle-free by contract, like VulkanGeo.h: the facade translation units that include this
// stay free of <vulkan/vulkan.h>. Implementations live in Backend/VulkanCore.cpp, which is a
// deliberate same-TU choice - the loader table and the core singleton are file-static there, and
// extracting them is a wedge-5-sized refactor.
#ifndef GENERALSX_VULKANDESC_H
#define GENERALSX_VULKANDESC_H

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

// A reservation in one of the host-visible constant rings: the mapped pointer the caller writes
// through, the offset to pass as the descriptor's dynamic offset, and the byte size actually
// reserved (the descriptor range is taken from it, so an over-large range can never make validation
// complain about offset+range exceeding the buffer).
struct VKDesc_Region {
    void*            host;
    unsigned long long offset;
    unsigned int     bytes;
};

// Which constant ring a reservation comes from. These mirror the descriptor set layout bindings.
enum VKDesc_Ring {
    VKDESC_RING_VERTEX = 0,   // binding 1, the 256 vertex-shader constant registers
    VKDESC_RING_PIXEL  = 1,   // binding 2, the 224 pixel-shader constant registers
    VKDESC_RING_LIGHTS = 2,   // binding 3, the 8-light dynamic uniform block
    VKDESC_RING_COUNT  = 3
};

// True when the core is up, GENERALSX_VKDESC=1 and the descriptor layer is built.
bool VKDesc_Ready();

// Build the layer (set layout, descriptor pool, sampler LUT, the three constant rings).
// Called from VulkanCore::Init when the gate is on; idempotent. Failure is non-fatal by design:
// the engine keeps rendering through the D3D9 path while the mirror phase is in force.
bool VKDesc_Create();

// Tear everything down. Idempotent, called from VulkanCore::Shutdown before the device dies.
void VKDesc_Destroy();

// Reserve 'size' bytes in one of the constant rings and fill 'out' with the mapped pointer, the
// device offset (already a multiple of the ring's alignment requirement) and the size.
// Returns false when the layer is down or the reservation does not fit; a reservation larger than
// the ring is rejected rather than silently rewinding, because a live draw may still be reading it.
bool VKDesc_Uniform_Reserve(int ring, unsigned int size, VKDesc_Region* out);

// Look up (create on first use) the cached sampler for a D3DSAMP ordinal tuple. The ordinals are the
// D3DTEXF_* and D3DTEXTUREADDRESS_* values the facade already stores in SamplerStates[][], so callers
// pass them through unchanged. Returns nullptr on an unsupported combination.
void* VKDesc_Sampler(unsigned int addrU, unsigned int addrV, unsigned int addrW,
                     unsigned int minFilter, unsigned int magFilter, unsigned int mipFilter,
                     unsigned int maxAnisotropy);

// Allocate one descriptor set and fill all four bindings: four texture slots (a null view binds the
// built-in 1x1 white fallback) plus the three constant regions. *set receives the VkDescriptorSet.
bool VKDesc_Build_Set(void** set, void* const views[4], void* const samplers[4],
                      const VKDesc_Region* vtx, const VKDesc_Region* pix, const VKDesc_Region* lights);

// The ring buffer handles a caller needs when it writes VkDescriptorBufferInfo itself (wedge 5's PSO
// path caches sets per draw and rewrites bindings without going through Build_Set).
void* VKDesc_Ring_Buffer(int ring);

// ---- wedge 5 accessors ---------------------------------------------------------------
// The draw path lives in its own translation unit and must be able to build a VkPipelineLayout, so
// the set layout handle has to be reachable without exposing Vulkan types in this header. Both
// return nullptr while the layer is down; the caller then treats the draw as un-backable.
void* VKDesc_Set_Layout();

// The built-in 1x1 white image view + default sampler. A draw whose texture slot is empty, or whose
// stage has no sampler state yet, binds these instead of a null descriptor - Vulkan forbids an
// incomplete descriptor set at draw time, where D3D9 silently sampled nothing.
void* VKDesc_White_View();
void* VKDesc_Default_Sampler();

// One-shot self-test at co-create time (env GENERALSX_VKDESC): mirrors a 1x1 white texture and a
// 64x64 color + depth render-target pair through the real wedge-3 paths, builds an offscreen render
// pass WITH a depth attachment, compiles the probe SPIR-V, makes a pipeline layout + PSO, binds
// vertex data out of the geometry ring and two descriptor sets at two dynamic offsets, draws a near
// triangle then a far one, reads the target back and compares byte-exact against a CPU reference.
// The draw order is the point: only a working depth attachment keeps the near pixels from being
// overwritten by the later far draw. Logs the gate line
//   VKDESC probe: drawOK=.. drawBad=.. mismatches=.. checked=.. skipped=..
void VKDesc_Run_Probe();

#endif // _WIN32 && SAGE_USE_VULKAN_CORE
#endif // GENERALSX_VULKANDESC_H
