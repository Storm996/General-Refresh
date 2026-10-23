// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 8: translated-program registry.
//
// What this is. Wedge 8 replaces D3D9 programmable shaders with hand-translated GLSL/SPIR-V twins. The
// engine names those shaders only by an opaque IDirect3D*Shader9 handle, so something has to answer "this
// handle is program tag N" before the draw path can choose a module. This registry is that something, and
// it is deliberately Vulkan-handle-free (like VulkanDraw.h / VulkanDesc.h) because dx8wrapper.cpp and
// W3DShaderManager.cpp include it.
//
// How a program is identified. By the exact bytes of the HLSL source that compiled into the handle, hashed
// with FNV-1a 64. That is stable across runs and machines, unlike a handle address, and it makes an
// untranslated shader fail closed: no matching row means no tag, and the facade keeps the D3D9 call.
// The hash every row carries is produced by scripts/pgm_hash.py from the exported block under
// 文档/内联HLSL/, which is byte-identical to the C++ literal the facade compiles (CRLF normalised).
//
// What contributors do NOT touch: this file is the receiver's insertion point. A family bundle delivers
// HLSL + GLSL twins + a uniform map; the integrator bakes the GLSL (VkShadersPgm.h) and adds one row here.
//
// Linkage. The declarations are visible on every platform, because W3DShaderManager.cpp is shared code
// and GameEngineDevice does not carry SAGE_USE_VULKAN_CORE. Backend/VulkanPgm.cpp is compiled into
// ww3d2 unconditionally: on the Vulkan-Windows build it holds the registry, everywhere else it supplies
// the no-op definitions below. That is what keeps the facade's call site platform-neutral and, unlike
// header-inline fallbacks, makes the real registry reachable from the shared TU.
#ifndef GENERALSX_VULKANPGM_H
#define GENERALSX_VULKANPGM_H

#include <cstdint>

#define VKPGM_MAX_PROGRAMS 128      // families are ~51 blocks; this is the ceiling with headroom
#define VKPGM_MAX_BINDINGS 256      // handles alive at once: bloom 4 + dot3 pair + probes, times resets

// One translated program. Tag 0 is reserved for "fixed function", so the first real tag is 1.
struct VKPgm_Program {
    unsigned          tag;        // registry index used in VKDraw_PSO_Key::vs_tag / ps_tag
    unsigned          is_pixel;   // 1 = fragment stage, 0 = vertex stage
    unsigned long long hash;      // FNV-1a 64 of the exact HLSL source
    const uint32_t*   spv;        // baked SPIR-V words (VkShadersPgm.h)
    unsigned          words;      // word count, not bytes
    const char*       name;       // diagnostic name, e.g. "dot3.vert"
};

// GENERALSX_VKPGM=1. When off, nothing is registered and every programmable batch falls back to D3D9,
// which is what keeps this wedge reversible like the ones before it.
bool VKPgm_On();

unsigned             VKPgm_Program_Count();
const VKPgm_Program* VKPgm_Program_At(unsigned index);

// Called by W3DShaderManager::compileShaderAtBestProfile after a successful profile compile, with the
// raw source (before the "// d3d-target:" prefix it adds). Returns the tag the source matched, or 0 when
// the source has not been translated yet. An unmatched source is logged once, with the hash an
// integrator needs to add the row.
unsigned VKPgm_Note_Compiled(void* handle, int isPixel, const char* hlslSource, const char* logTag);

// Called by the draw facade for every batch that names a custom shader. 0 = no translation, so refuse and
// let the D3D9 call run.
unsigned VKPgm_Tag_For_Handle(void* handle, int isPixel);

// A D3D device reset invalidates every IDirect3D*Shader9 handle; the mapping must not survive it.
void VKPgm_Reset_All();

struct VKPgm_Stats {
    unsigned long long notes;      // compiled shaders offered to the registry
    unsigned long long matched;    // ... whose source had a row
    unsigned long long unmatched;  // ... whose source did not (the work still to do)
    unsigned long long lookups;    // handle -> tag queries from the draw path
    unsigned long long hits;       // ... answered with a tag
    unsigned long long resets;     // handle maps dropped on device reset
    // Phase 5A fallback audit: fixed-function draws that reached the submit path. ffDraws means the
    // retail FF interpreter actually rendered; ffRefused means the draw was refused. Both are zero
    // only when every batch has a translated program and no FF path is needed.
    unsigned long long ffDraws;
    unsigned long long ffRefused;
};
void VKPgm_Get_Stats(VKPgm_Stats* out);
void VKPgm_Log_Stats();

// GeneralsX @feature Qoder 18/10/2026 wedge 8 path census (read-only). The draw path reports each batch
// twice: once when it enters VK_Draw_Submit and once when the draw is recorded. The gap between the two
// is a batch the Vulkan side refused, which is invisible on screen under VKPRESENT; slot0null counts
// batches whose first texture slot was empty, which draws untextured. Neither call changes rendering.
void VKPgm_Note_Draw(unsigned vsTag, unsigned psTag, int submitted, int slot0Present);
void VKPgm_Log_Census();       // cumulative per-tag summary; called by the draw path periodically

#endif // GENERALSX_VULKANPGM_H
