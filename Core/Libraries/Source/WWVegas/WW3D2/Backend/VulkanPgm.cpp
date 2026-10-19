// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 8: translated-program registry implementation.
//
// See VulkanPgm.h for the contract. Two jobs live here and nothing else:
//  1. map the exact HLSL source bytes the facade compiled to a stable program tag (hash row lookup);
//  2. map the resulting IDirect3D*Shader9 handle to that tag for the draw path, and drop the map when a
//     device reset invalidates the handles.
// The Vulkan shader modules themselves are created by VulkanDraw.cpp from the same rows, so this file
// stays free of Vulkan handles and can be included from the facade's side of the seam.
#include "VulkanPgm.h"

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

#include "VulkanFns.h"
#include "VkShadersPgm.h"
#include <cstring>

namespace {

// Hashes of the exact HLSL literals, produced by scripts/pgm_hash.py from 文档/内联HLSL/. Regenerate
// those two steps together: a hash computed from a hand-retyped source will silently never match.
#define VKPGM_HASH_KVERTEXSHADER          0x64A9E1B85A02FD8BULL   // kVertexShader    (dot3 VS, 1275 B)
#define VKPGM_HASH_KPIXELSHADER           0x6C2D97737AA27A4FULL   // kPixelShader     (dot3 PS,  940 B)
#define VKPGM_HASH_KBLOOMBRIGHTSHADER     0xA1EE5C205823D4CEULL   // kBloomBright     (PS, 285 B)
#define VKPGM_HASH_KBLOOMBLURSHADER       0x1841D6FAE6469C26ULL   // kBloomBlur       (PS, 638 B)
#define VKPGM_HASH_KBLOOMCOMPOSITESHADER  0xAA1457757ABAC685ULL   // kBloomComposite  (PS, 150 B)
#define VKPGM_HASH_KBLOOMGAMMACOPYSHADER  0xB5043807E0131D16ULL   // kBloomGammaCopy  (PS, 182 B)

// Live demo pair. The dot3 pair above is the VS/PS translation reference but is never mounted any more
// (DX8NormalMap::Apply serves only the multi-light family since P3); these are the shaders the draw path
// actually selects today, so they are the ones that can produce a real wedge-8 draw.
#define VKPGM_HASH_KVERTEXSHADERGN_NUV1   0x84A871F637DFD531ULL   // kVertexShaderGN_NUV1 (VS, 647 B)
#define VKPGM_HASH_KVERTEXSHADERGN_NUV2   0x7C5E9CD2C8697F14ULL   // kVertexShaderGN_NUV2 (VS, 671 B)
#define VKPGM_HASH_KPIXELSHADERGN         0x0AC233A339FF52F2ULL   // kPixelShaderGN       (PS, 1664 B)

// Shadow reception variants of the GN pair. DX8GeomNormal::Apply swaps to these whenever a sun shadow
// map is active, which on a normal battlefield is always - so these, not the plain pair above, are the
// handles the draw path actually sets there.
#define VKPGM_HASH_KVERTEXSHADERGNS_NUV1  0x829192A8ABB1F6B8ULL   // kVertexShaderGNS_NUV1 (VS, 932 B)
#define VKPGM_HASH_KVERTEXSHADERGNS_NUV2  0xF21D50243951DFA3ULL   // kVertexShaderGNS_NUV2 (VS, 956 B)
#define VKPGM_HASH_KPIXELSHADERGNS        0x4B5D82207D35057DULL   // kPixelShaderGNS       (PS, 2769 B)

#define VKPGM_COUNT(a) (unsigned)(sizeof(a) / sizeof((a)[0]))

// The registry. One row per translated stage. Tag order is arbitrary but must stay stable: it is part of
// VKDraw_PSO_Key, and the pipeline cache treats two tags as two pipelines.
//
// Parallel work: this base table is the integrator's (the two demo families live here). A translation
// bundle owns a sibling row file instead of editing this one - VkPgmB1.inc / VkPgmB2.inc, each defining
// `static const VKPgm_Program kPgmB1Programs[]` / `kPgmB2Programs[]` with the tags of its range. The
// include is conditional so a bundle can land before the other one exists. Row files and their generated
// shader headers are the only files a bundle adds under Backend/.
const VKPgm_Program kProgramsBase[] = {
    { 1u, 0u, VKPGM_HASH_KVERTEXSHADER,         VKPGM_DOT3_VERT_SPV,
      VKPGM_COUNT(VKPGM_DOT3_VERT_SPV),         "dot3.vert" },
    { 2u, 1u, VKPGM_HASH_KPIXELSHADER,          VKPGM_DOT3_FRAG_SPV,
      VKPGM_COUNT(VKPGM_DOT3_FRAG_SPV),         "dot3.frag" },
    { 3u, 1u, VKPGM_HASH_KBLOOMBRIGHTSHADER,    VKPGM_BLOOM_BRIGHT_FRAG_SPV,
      VKPGM_COUNT(VKPGM_BLOOM_BRIGHT_FRAG_SPV), "bloom.bright" },
    { 4u, 1u, VKPGM_HASH_KBLOOMBLURSHADER,      VKPGM_BLOOM_BLUR_FRAG_SPV,
      VKPGM_COUNT(VKPGM_BLOOM_BLUR_FRAG_SPV),   "bloom.blur" },
    { 5u, 1u, VKPGM_HASH_KBLOOMCOMPOSITESHADER, VKPGM_BLOOM_COMPOSITE_FRAG_SPV,
      VKPGM_COUNT(VKPGM_BLOOM_COMPOSITE_FRAG_SPV), "bloom.composite" },
    { 6u, 1u, VKPGM_HASH_KBLOOMGAMMACOPYSHADER, VKPGM_BLOOM_GAMMA_FRAG_SPV,
      VKPGM_COUNT(VKPGM_BLOOM_GAMMA_FRAG_SPV),  "bloom.gamma" },
    // The two GN vertex sources differ only by an unused uv1 input, so both rows share one twin and one
    // tag: the pipeline key is the tag, and the SPIR-V is identical for the two FVFs.
    { 7u, 0u, VKPGM_HASH_KVERTEXSHADERGN_NUV1,  VKPGM_GN_VERT_SPV,
      VKPGM_COUNT(VKPGM_GN_VERT_SPV),           "gn.vert" },
    { 7u, 0u, VKPGM_HASH_KVERTEXSHADERGN_NUV2,  VKPGM_GN_VERT_SPV,
      VKPGM_COUNT(VKPGM_GN_VERT_SPV),           "gn.vert(nuv2)" },
    { 8u, 1u, VKPGM_HASH_KPIXELSHADERGN,        VKPGM_GN_FRAG_SPV,
      VKPGM_COUNT(VKPGM_GN_FRAG_SPV),           "gn.frag" },
    { 9u, 0u, VKPGM_HASH_KVERTEXSHADERGNS_NUV1, VKPGM_GNS_VERT_SPV,
      VKPGM_COUNT(VKPGM_GNS_VERT_SPV),          "gns.vert" },
    { 9u, 0u, VKPGM_HASH_KVERTEXSHADERGNS_NUV2, VKPGM_GNS_VERT_SPV,
      VKPGM_COUNT(VKPGM_GNS_VERT_SPV),          "gns.vert(nuv2)" },
    { 10u, 1u, VKPGM_HASH_KPIXELSHADERGNS,      VKPGM_GNS_FRAG_SPV,
      VKPGM_COUNT(VKPGM_GNS_FRAG_SPV),          "gns.frag" },
};

// Bundle row files. Each is optional: a missing one simply contributes no programs, so B1 and B2 can be
// integrated independently, in either order, without touching this file.
#if defined(__has_include)
#  if __has_include("VkPgmB1.inc")
#    include "VkPgmB1.inc"
#    define VKPGM_HAVE_B1 1
#  endif
#  if __has_include("VkPgmB2.inc")
#    include "VkPgmB2.inc"
#    define VKPGM_HAVE_B2 1
#  endif
#endif

// The flat view the rest of the layer sees: base rows first, then each bundle's table. Keeping the tables
// separate is what lets two contributors append without a merge; the enumeration below is the only place
// that has to know they are a list.
unsigned program_count_total()
{
    unsigned n = VKPGM_COUNT(kProgramsBase);
#ifdef VKPGM_HAVE_B1
    n += VKPGM_COUNT(kPgmB1Programs);
#endif
#ifdef VKPGM_HAVE_B2
    n += VKPGM_COUNT(kPgmB2Programs);
#endif
    return n;
}

const VKPgm_Program* program_at_total(unsigned index)
{
    if (index < VKPGM_COUNT(kProgramsBase)) return &kProgramsBase[index];
    index -= VKPGM_COUNT(kProgramsBase);
#ifdef VKPGM_HAVE_B1
    if (index < VKPGM_COUNT(kPgmB1Programs)) return &kPgmB1Programs[index];
    index -= VKPGM_COUNT(kPgmB1Programs);
#endif
#ifdef VKPGM_HAVE_B2
    if (index < VKPGM_COUNT(kPgmB2Programs)) return &kPgmB2Programs[index];
#endif
    return nullptr;
}

struct VKPgmBinding {
    void*    handle;
    unsigned tag;
    unsigned is_pixel;
};
VKPgmBinding sBind[VKPGM_MAX_BINDINGS];
unsigned     sBindCount = 0u;

VKPgm_Stats sStats = {};

// Unmatched sources are logged once each: at startup the registry is offered every shader the engine
// compiles, and repeating the same hash on every device reset would bury the line that matters. The row
// keeps a count and the caller's tag so the census can say which family is still untranslated.
struct VKPgm_UnmatchedRow {
    unsigned long long hash;
    unsigned           is_pixel;
    unsigned long long count;
    char               tag[32];
};
VKPgm_UnmatchedRow sUnmatched[64];
unsigned           sUnmatchedCount = 0u;

// GeneralsX @feature Qoder 18/10/2026 wedge 8 path census. Tag 0 means fixed function; anything else is
// a registry row. `enter` counts the batches that reached the submit path, `ok` the ones recorded. A row
// with enter far above ok is a family the Vulkan side is refusing (invisible under VKPRESENT), and a row
// with slot0null close to enter is drawing without its base texture. Both are read-only observations.
#define VKPGM_CENSUS_TAGS 512u
struct VKPgm_CensusRow { unsigned long long enter, ok, slot0Null; };
VKPgm_CensusRow sCensusVs[VKPGM_CENSUS_TAGS];
VKPgm_CensusRow sCensusPs[VKPGM_CENSUS_TAGS];
unsigned long long sCensusFFEnter = 0u, sCensusFFOk = 0u, sCensusFFSlot0Null = 0u;

unsigned long long fnv1a64(const char* text)
{
    unsigned long long h = 0xCBF29CE484222325ULL;
    const unsigned char* p = (const unsigned char*)text;
    while (*p) { h ^= (unsigned long long)*p++; h *= 0x100000001B3ULL; }
    return h;
}

const VKPgm_Program* find_hash(unsigned long long hash, unsigned is_pixel)
{
    const unsigned n = program_count_total();
    for (unsigned i = 0; i < n; ++i) {
        const VKPgm_Program* p = program_at_total(i);
        if (p->hash == hash && p->is_pixel == is_pixel) return p;
    }
    return nullptr;
}

void log_registry_once()
{
    static bool done = false;
    if (done) return;
    done = true;
    const unsigned n = program_count_total();
    VKWP1_LOG("wedge8 program registry: %u translated stages", n);
    for (unsigned i = 0; i < n; ++i) {
        const VKPgm_Program* p = program_at_total(i);
        VKWP1_LOG("  tag=%u %-20s stage=%s hash=%016llx spv=%u words",
            p->tag, p->name, p->is_pixel ? "PS" : "VS", p->hash, p->words);
    }
}

// Records an unmatched source. Returns true when this hash has been seen before, so the caller only
// prints the full line once.
bool unmatched_seen_before(unsigned long long hash, unsigned is_pixel, const char* logTag)
{
    for (unsigned i = 0; i < sUnmatchedCount; ++i) {
        if (sUnmatched[i].hash == hash && sUnmatched[i].is_pixel == is_pixel) {
            ++sUnmatched[i].count;
            return true;
        }
    }
    if (sUnmatchedCount < VKPGM_COUNT(sUnmatched)) {
        VKPgm_UnmatchedRow& r = sUnmatched[sUnmatchedCount++];
        r.hash = hash;
        r.is_pixel = is_pixel;
        r.count = 1ull;
        unsigned n = 0u;
        const char* src = logTag ? logTag : "?";
        while (src[n] && n < VKPGM_COUNT(r.tag) - 1u) { r.tag[n] = src[n]; ++n; }
        r.tag[n] = 0;
    }
    return false;
}

const char* program_name_for_tag(unsigned tag, unsigned is_pixel)
{
    const unsigned n = program_count_total();
    for (unsigned i = 0; i < n; ++i) {
        const VKPgm_Program* p = program_at_total(i);
        if (p->tag == tag && p->is_pixel == is_pixel) return p->name;
    }
    return "?";
}

} // anon

bool VKPgm_On()
{
    static int on = -1;
    if (on < 0) on = vk_env_gate("GENERALSX_VKPGM") ? 1 : 0;
    return on == 1;
}

unsigned VKPgm_Program_Count()
{
    if (!VKPgm_On()) return 0u;
    log_registry_once();
    return program_count_total();
}

const VKPgm_Program* VKPgm_Program_At(unsigned index)
{
    if (!VKPgm_On()) return nullptr;
    return program_at_total(index);
}

unsigned VKPgm_Note_Compiled(void* handle, int isPixel, const char* hlslSource, const char* logTag)
{
    if (!VKPgm_On()) return 0u;
    if (!handle || !hlslSource || !hlslSource[0]) return 0u;
    ++sStats.notes;

    const unsigned long long hash = fnv1a64(hlslSource);
    const VKPgm_Program* p = find_hash(hash, isPixel ? 1u : 0u);
    if (!p) {
        ++sStats.unmatched;
        if (!unmatched_seen_before(hash, isPixel ? 1u : 0u, logTag)) {
            VKWP1_LOG("wedge8 unmatched %s source %s hash=%016llx bytes=%u - no translation, D3D9 path stays",
                isPixel ? "PS" : "VS", logTag ? logTag : "?", hash, (unsigned)strlen(hlslSource));
        }
        return 0u;
    }

    // One entry per (handle, stage). A recompile over a released handle replaces the old row.
    for (unsigned i = 0; i < sBindCount; ++i) {
        if (sBind[i].handle == handle && sBind[i].is_pixel == (unsigned)(isPixel ? 1 : 0)) {
            sBind[i].tag = p->tag;
            ++sStats.matched;
            return p->tag;
        }
    }
    if (sBindCount >= VKPGM_MAX_BINDINGS) {
        VKWP1_LOG("wedge8 binding table full (%u) - %s stays on the D3D9 path", sBindCount, p->name);
        ++sStats.unmatched;
        return 0u;
    }
    sBind[sBindCount].handle   = handle;
    sBind[sBindCount].tag      = p->tag;
    sBind[sBindCount].is_pixel = (unsigned)(isPixel ? 1 : 0);
    ++sBindCount;
    ++sStats.matched;
    VKWP1_LOG("wedge8 program bound: %s tag=%u handle=%p", p->name, p->tag, handle);
    return p->tag;
}

unsigned VKPgm_Tag_For_Handle(void* handle, int isPixel)
{
    if (!VKPgm_On() || !handle) return 0u;
    ++sStats.lookups;
    const unsigned pix = (unsigned)(isPixel ? 1 : 0);
    for (unsigned i = 0; i < sBindCount; ++i) {
        if (sBind[i].handle == handle && sBind[i].is_pixel == pix) {
            ++sStats.hits;
            return sBind[i].tag;
        }
    }
    return 0u;
}

void VKPgm_Reset_All()
{
    if (!VKPgm_On()) return;
    memset(sBind, 0, sizeof(sBind));
    sBindCount = 0u;
    ++sStats.resets;
    VKWP1_LOG("wedge8 program bindings dropped on device reset");
}

void VKPgm_Get_Stats(VKPgm_Stats* out)
{
    if (!out) return;
    *out = sStats;
    out->resets = sStats.resets;
}

void VKPgm_Log_Stats()
{
    if (!VKPgm_On()) return;
    VKWP1_LOG("wedge8 registry: tables=%u bound=%u notes=%llu matched=%llu unmatched=%llu lookups=%llu hits=%llu resets=%llu",
        program_count_total(), sBindCount, sStats.notes, sStats.matched, sStats.unmatched,
        sStats.lookups, sStats.hits, sStats.resets);
}

void VKPgm_Note_Draw(unsigned vsTag, unsigned psTag, int submitted, int slot0Present)
{
    if (!VKPgm_On()) return;
    if (!vsTag && !psTag) {
        if (submitted) ++sCensusFFOk; else ++sCensusFFEnter;
        if (!submitted && !slot0Present) ++sCensusFFSlot0Null;
        return;
    }
    if (vsTag && vsTag < VKPGM_CENSUS_TAGS) {
        VKPgm_CensusRow& r = sCensusVs[vsTag];
        if (submitted) ++r.ok; else ++r.enter;
        if (!submitted && !slot0Present) ++r.slot0Null;
    }
    if (psTag && psTag < VKPGM_CENSUS_TAGS) {
        VKPgm_CensusRow& r = sCensusPs[psTag];
        if (submitted) ++r.ok; else ++r.enter;
        if (!submitted && !slot0Present) ++r.slot0Null;
    }
}

void VKPgm_Log_Census()
{
    if (!VKPgm_On()) return;
    // One line per row that was actually used this run. Fixed function is aggregated separately: the
    // draw path reports no tag for it, and it is the leg that must keep the retail look.
    VKWP1_LOG("census ff: enter=%llu ok=%llu slot0null=%llu",
        sCensusFFEnter, sCensusFFOk, sCensusFFSlot0Null);
    for (unsigned t = 1u; t < VKPGM_CENSUS_TAGS; ++t) {
        if (sCensusVs[t].enter) {
            VKWP1_LOG("census vs tag=%u %s: enter=%llu ok=%llu slot0null=%llu", t,
                program_name_for_tag(t, 0u), sCensusVs[t].enter, sCensusVs[t].ok, sCensusVs[t].slot0Null);
        }
        if (sCensusPs[t].enter) {
            VKWP1_LOG("census ps tag=%u %s: enter=%llu ok=%llu slot0null=%llu", t,
                program_name_for_tag(t, 1u), sCensusPs[t].enter, sCensusPs[t].ok, sCensusPs[t].slot0Null);
        }
    }
    // The registry already prints each unmatched source once at compile time; the repeat count here says
    // whether it was a one-off hot reload or a family the draw path keeps asking for.
    for (unsigned i = 0; i < sUnmatchedCount; ++i) {
        const VKPgm_UnmatchedRow& u = sUnmatched[i];
        VKWP1_LOG("census unmatched %s %s hash=%016llx x%llu",
            u.is_pixel ? "PS" : "VS", u.tag, u.hash, u.count);
    }
}

#else // no pure-Vulkan backend on this platform

// Shared facade code (W3DShaderManager.cpp) calls into this registry on every platform, so the symbol has
// to exist even where zero programs can be registered. Answering "no translation" everywhere keeps the
// D3D9 path in charge, exactly like the gate being off.
bool VKPgm_On() { return false; }
unsigned VKPgm_Program_Count() { return 0u; }
const VKPgm_Program* VKPgm_Program_At(unsigned) { return nullptr; }
unsigned VKPgm_Note_Compiled(void*, int, const char*, const char*) { return 0u; }
unsigned VKPgm_Tag_For_Handle(void*, int) { return 0u; }
void VKPgm_Reset_All() {}

void VKPgm_Get_Stats(VKPgm_Stats* out)
{
    if (out) { out->notes = out->matched = out->unmatched = out->lookups = out->hits = out->resets = 0ull; }
}
void VKPgm_Log_Stats() {}
void VKPgm_Note_Draw(unsigned, unsigned, int, int) {}
void VKPgm_Log_Census() {}

#endif // _WIN32 && SAGE_USE_VULKAN_CORE
