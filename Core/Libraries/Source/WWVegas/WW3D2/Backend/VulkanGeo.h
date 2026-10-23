// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: geometry ring bridge for the DX8 VB/IB lock classes.
// Deliberately free of <vulkan/vulkan.h>: dx8vertexbuffer.cpp / dx8indexbuffer.cpp only need the mapped
// host pointer, never a Vk handle. Definitions live in Backend/VulkanCore.cpp (compiled only when
// SAGE_USE_VULKAN_CORE is defined, win64vk port tree).
#ifndef GENERALSX_VULKANGEO_H
#define GENERALSX_VULKANGEO_H

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

// True when the Vulkan core is up, GENERALSX_VKGEO=1 is set and the ring has enough room for
// 'size' bytes (freshly allocated or rewound). The DX8 lock classes consult this per lock.
bool VKGeo_Ready(unsigned int size);

// Reserve 'size' bytes in the host-visible geometry ring and return the mapped pointer.
// discard==true rewinds the cursor first (D3DLOCK_DISCARD semantics). Returns nullptr when
// the reservation does not fit; caller must then fall back to the D3D9 Lock path.
void* VKGeo_Alloc(unsigned int size, bool discard);

// ---- wedge 3.2: static texture mirror (env GENERALSX_VKTEX) ----
// One packed row-descriptor per mip: bits/pitch come from a D3DLOCK_READONLY re-lock of the
// just-unlocked MANAGED surface (the loader's texels are final at that point).
struct VKTex_Mip_Source {
    const unsigned char* bits;
    unsigned int pitch; // bytes per row of the source surface
};

bool VKTex_Ready();

// Create-or-reuse a device-local VkImage for the D3D texture identified by 'key' (pass the
// IDirect3DBaseTexture9* from Peek_D3D_Texture(), or the raw texture pointer at a procedural
// writer). WW3DFormat-selected VkFormat, all mips staged through the host-visible staging buffer,
// vkCmdCopyBufferToImage, fence-wait, transition to SHADER_READ_ONLY_OPTIMAL. A repeat call with
// the same key re-uploads into the existing image (CPU-dynamic textures: terrain atlas, water
// bump, smudge, tree buffer) instead of leaking a second one - the layout is ping-ponged back from
// SHADER_READ_ONLY first. Every 512th exact-layout write also reads mip0 back and FNV-hashes it
// against the staged bytes. Returns false (and increments the skip counter) for formats without a
// Vulkan storage match or when the mirror budget is exhausted (creation only).
bool VKTex_Upload_2D(void* key, unsigned int width, unsigned int height, unsigned int levels,
                     int ww3d_format, const VKTex_Mip_Source* mips);

// ---- wedge 3.4: render-target / depth mirror + image layout state machine (env GENERALSX_VKRT) ----
// Render targets are the one surface class the mirror phase cannot get bytes from through a CPU lock:
// their content lives on the GPU (written by DXVK's own device, not ours). What 3.4 stands up is the
// Vulkan-side *shape* and *layout* machinery that wedge 5 (the draw pipeline) will consume:
//   - a keyed device-local VkImage per engine render target (COLOR_ATTACHMENT usage) / depth texture
//     (DEPTH_STENCIL_ATTACHMENT usage), created at the two facade factories, with the same
//     recycled-D3D-pointer shape guard as the texture mirror;
//   - an explicit per-image layout state machine (VKRT_Transition) that records each image's current
//     VkImageLayout and emits exactly one correct pipeline barrier on a change - the transitions a
//     real render pass walks (color: UNDEFINED->ATTACHMENT->READ_ONLY->TRANSFER_SRC->GENERAL;
//     depth: UNDEFINED->DEPTH_STENCIL_ATTACHMENT->READ_ONLY);
//   - a readback mirror (VKRT_Mirror_Readback) that funnels the SYSTEMMEM bytes a GetRenderTargetData
//     hands back into the keyed image, so smudge/screenshot RT data converges.
// All of this is proved at device Init by VKRT_Run_Probe(): a synthetic color image goes through the
// full state machine into COLOR_ATTACHMENT, is cleared with an exact integer pattern, copied back to
// host, and FNV-compared; a synthetic depth image goes through the depth aspect transitions + clear.
// This is the only way to confirm the layout machine + readback primitive actually work on the host
// GPU while the engine's own draws still go to D3D9.
//
// Layout ids are an int enum (NOT VkImageLayout): VulkanGeo.h deliberately excludes <vulkan/vulkan.h>
// so the DX8 lock classes that include it stay Vulkan-header-free; VulkanCore.cpp maps these to the
// real VkImageLayout + stage/access masks.
enum VKRT_Layout {
    VKRT_LAYOUT_UNDEFINED = 0,
    VKRT_LAYOUT_TRANSFER_DST,
    VKRT_LAYOUT_TRANSFER_SRC,
    VKRT_LAYOUT_COLOR_ATTACHMENT,
    VKRT_LAYOUT_DEPTH_STENCIL_ATTACHMENT,
    VKRT_LAYOUT_SHADER_READ_ONLY,
    VKRT_LAYOUT_GENERAL,
    VKRT_LAYOUT_COUNT
};

bool VKRT_Ready();

// Allocate (or reuse under the shape guard) a device-local mirror for a color render target / depth
// texture identified by 'key' (the IDirect3DTexture9*). 'ww3d_format' is a WW3DFormat for color,
// 'ww3d_zformat' a WW3DZFormat for depth. Returns false for an unsupported format or no Vulkan core.
bool VKRT_Alloc_Color(void* key, unsigned int width, unsigned int height, int ww3d_format);
bool VKRT_Alloc_Depth(void* key, unsigned int width, unsigned int height, int ww3d_zformat);

// Drive the recorded layout of 'key' from its current state to 'to' with one pipeline barrier.
// No-op + false when there is no such image, or it is already in 'to'.
bool VKRT_Transition(void* key, int to);

// Feed a full-surface GetRenderTargetData SYSTEMMEM result into the keyed color mirror
// (create-or-reuse), leaving it in SHADER_READ_ONLY. 'bits'/'pitch' are the locked system surface.
bool VKRT_Mirror_Readback(void* key, const unsigned char* bits, unsigned int pitch,
                          unsigned int width, unsigned int height, int ww3d_format);

// One-shot self-test run once at device Init when the gate is on; logs pass/fail evidence.
void VKRT_Run_Probe();

// ---- wedge 5 accessors ---------------------------------------------------------------
// The draw path (Backend/VulkanDraw.cpp) has to reach inside a mirror: bind its view as a render
// attachment, sample it, or barrier it as part of an already-recording command buffer. These stay
// handle-free by passing Vulkan objects as void* and VkFormat / VkImageLayout as ints, which keeps
// the deliberate no-<vulkan/vulkan.h> rule of this header intact.
struct VKRT_Info {
    void*    image;      // VkImage
    void*    view;       // VkImageView (color / depth aspect as created)
    // Sampling view: identical to 'view' for color, but DEPTH-only for a depth format that also carries
    // stencil. Vulkan forbids a SAMPLED_IMAGE view from naming both aspects, and a combined D24S8 view
    // silently reads back 0 on some drivers - which made the sun receiver reconstruct garbage.
    void*    sample_view;
    unsigned format;     // VkFormat
    unsigned width, height, levels;
    int      layout;     // VKRT_Layout id, not a VkImageLayout
    int      is_depth;
};

// Allocate a mirror straight from a VkFormat (carried as unsigned, this header stays Vulkan-free).
// The wedge-5 scene target has no WW3D format to translate from - it is a resource this layer owns -
// so it comes in through these instead of the WW3DFormat-taking factories above.
bool VKRT_Alloc_Color_Format(void* key, unsigned int width, unsigned int height, unsigned int vk_format);
bool VKRT_Alloc_Depth_Format(void* key, unsigned int width, unsigned int height, unsigned int vk_format);

// False when there is no mirror for 'key'.
bool VKRT_Query(void* key, VKRT_Info* out);

// Record one pipeline barrier that moves 'key' to layout 'to' inside a command buffer the caller is
// already recording, and update the stored layout. Unlike VKRT_Transition this neither allocates a
// command buffer nor submits, so it is the form a render pass or a blit can be built around.
bool VKRT_Cmd_Transition(void* commandBuffer, void* key, int to);

// Only bookkeeping: a render pass transitions its attachments implicitly through initial/finalLayout,
// and the state machine has to be told the outcome or the next explicit barrier would be recorded
// against a stale layout. Takes the VKRT_Layout id, not a VkImageLayout.
void VKRT_Set_Layout(void* key, int to);

struct VKTex_Info {
    void*    image;
    void*    view;
    unsigned format;
    unsigned width, height, levels;
};

bool VKTex_Query(void* key, VKTex_Info* out);

// GeneralsX @bugfix Qoder 19/10/2026 wedge 11: unlink (and reap) the mirror stored under 'key'. Mirrors
// are keyed by the raw D3D texture pointer, and WW3D reuses freed object addresses: the sun shadow map
// was created at the pointer of a dead normal map and inherited its upload mirror, which then shadowed
// the render-target mirror in every sampled lookup. The owner must delete its mirror when it dies.
void VKTex_Drop(void* key);

// GeneralsX @bugfix Qoder 20/10/2026 R-07: mirror identity, the general form of the drop above. Both
// mirror tables answer lookups by raw D3D object pointer, and the allocator hands a freed pointer to the
// next object it creates. Shape equality cannot tell that reuse apart from a genuine in-place CPU update,
// because a recycled 256x256 DXT1 lands on a 256x256 DXT1: the new owner then samples the dead texture's
// texels (unit skins stamped flat onto the terrain, ocean tiles answering with seafloor). The backend that
// owns the object's lifetime answers this with a token no allocator can hand out twice, and each table
// keeps the token of the object that built the image; a mirror whose token is not the live object's is
// retired on the spot and the caller sees a miss instead of a ghost. Registered by the no-d3d9 stub, the
// only backend that can name its own objects; leaving it unset keeps today's behaviour on the d3d9/DXVK
// path, where the pointer belongs to someone else and 0 matches the 0 every entry was built with.
typedef unsigned long long (*VKMirror_Identify)(void* key);
void VK_Set_Mirror_Identity_Fn(VKMirror_Identify fn);

// GeneralsX @bugfix Qoder 20/10/2026 R-07: the same drop for the render-target table. Its keys are engine
// render targets (the shadow map, the screen-filter RTT pair), and W3DShaderManager::init re-creates that
// pair at the same extent and format on every height-map rebuild, so the shape guard never fires and the
// new target inherits the old one's image. There was no entry point to forget a key; this is it.
void VKRT_Drop(void* key);

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
// GeneralsX @feature Qoder 21/10/2026 multi-match forensics: one release-visible line with the LIVE
// mirror tables (images and bytes), upload/create counters, budget skips and stub-object table
// occupancy. Called at asset-release time (each game reset), so two consecutive matches show whether
// the previous map really left memory behind or whether only the budget accounting was lying.
void VKDiag_Log_Mirror_State(const char* why);

// Occupancy of the stub's live-object identity table (textures + surfaces). Implemented next to the
// table in VKD3DStubImpl.cpp; the counter also catches a full-table insert, which used to be silent.
void VKD3D_Live_Stats(unsigned* used, unsigned* cap, unsigned long long* insertFailures);
#endif

// ---- wedge 5: geometry backing --------------------------------------------------------
// Wedge 3.1's single shared 64 MiB bump ring proved the host-write path, but a ring that hands every
// lock a new offset cannot back a draw: the pipeline needs a (VkBuffer, offset) pair that stays valid
// from the lock that filled it until the draw that consumes it. These are the per-object backings
// wedge 5 binds against. Keys are the WW3D buffer objects themselves, passed as void* so this header
// still needs no Vulkan or DX8 types.
//
// Lock the whole surface of 'key' (creating the backing on first use) and return the mapped pointer.
// 'stride' is only meaningful for vertex buffers; index buffers pass 0. 'dynamic' selects the
// rotating multi-slot form used by the per-frame dynamic buffers, where each write lands in the slot
// belonging to the current frame and the previously returned region stays readable by draws still in
// flight. nullptr means the layer is down or the allocation failed - the caller keeps writing the
// D3D9 resource in that case.
void* VKGeo_Back_Lock(void* key, unsigned int bytes, unsigned int stride, int dynamic, int discard);

// Where a draw has to bind what VKGeo_Back_Lock just filled. *offset is the device offset of the
// region, *stride the vertex stride recorded at lock time.
bool  VKGeo_Back_Bind(void* key, void** vk_buffer, unsigned long long* offset, unsigned int* stride,
                      unsigned int* bytes);

// Drop the backing for a buffer object being destroyed (its address may be recycled afterwards).
void  VKGeo_Back_Drop(void* key);

// Teardown for every dynamic region created so far; called at device shutdown and on a resize that
// invalidates in-flight frames.
void  VKGeo_Back_Reset();

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A forensics: a batch whose buffer has no
// backing falls back to D3D9, and there are only two ways that happens - "the lock ran while the layer
// was down" and "the allocation failed" - both invisible from the draw site. VKGeo_Back_Lock now names
// the reason in a bounded log line, and this dump prints the live table (flagging 'wanted') so one log
// run says which of the two it was and whether the object was ever locked at all.
void  VKGeo_Back_Dump(void* wanted);

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A forensics: the dump above says an object has
// no backing but not whether it was ever locked, and a recycled address makes the table ambiguous. This
// bounded trace prints each WW3D buffer object as it is born and dies ('fvf' is 0 for an index buffer),
// so one run tells a never-locked buffer from a backing dropped while the buffer was still being drawn.
void  VKGeo_Trace_Buffer(void* key, unsigned int fvf, unsigned int count, int born);

#endif // _WIN32 && SAGE_USE_VULKAN_CORE
#endif // GENERALSX_VULKANGEO_H
