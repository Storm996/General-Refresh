// GeneralsX @feature Qoder-B1 18/10/2026 wedge 8 bundle B1: weather, terrain30 (plain trio), road, tree,
// dot3 diagnostic probes, ML multi-light pair, monochrome screen filter.
// Owned row file; VKPGM_COUNT and VKPgm_Program are already defined by VulkanPgm.cpp.
//
// Hashes are the pgm_hash.py output over the exported blocks in 引擎改造/Vulkan/文档/内联HLSL/ (byte counts
// quoted per row). Terrain NM/NMPPL mirrors (source tags 113-118) sample register(s4); they were blocked
// while the descriptor layer carried four slots, and are live now that it carries eight
// (VKDRAW_MAX_TEX_SLOTS = 8) and the facade ferries every stage. The normal atlas reaches slot 4 through
// the stage-4 facade bind in TerrainShaderPixelShader::set.
// The profiler frame-capture PS (tag 151 reserved, shader.hlsl, hash 0xBA7D2A1D9DE5D56A, 250 B) is left
// untranslated per the work order's recommendation: debug-only draw.
#include "VkShadersPgmB1.h"

// #define lines emitted verbatim by scripts/pgm_hash.py over the exported blocks (see byte counts on the
// rows below). The profiler capture hash is recorded in the bundle report but intentionally not defined
// here: no row consumes it yet.
#define VKPGM_HASH_KWEATHERCLOUDPS         0x9F814F0E2887CCF6ULL   // kWeatherCloudPS.hlsl (316 bytes)
#define VKPGM_HASH_KWEATHERVEILPS          0x091DAFF0E5737B83ULL   // kWeatherVeilPS.hlsl (976 bytes)
#define VKPGM_HASH_KTERRAINPS30BASE        0xED2F73A9F33FA28BULL   // kTerrainPS30Base.hlsl (514 bytes)
#define VKPGM_HASH_KTERRAINPS30NOISE       0x550BD8CFB4E1CFC2ULL   // kTerrainPS30Noise.hlsl (574 bytes)
#define VKPGM_HASH_KTERRAINPS30NOISE12     0x5E5263BDB8ACFDE0ULL   // kTerrainPS30Noise12.hlsl (659 bytes)
#define VKPGM_HASH_KTERRAINPS30BASENM      0xF429302880E958DFULL   // kTerrainPS30BaseNM.hlsl (713 bytes)
#define VKPGM_HASH_KTERRAINPS30NOISENM     0x077DF8E02DC2FF4CULL   // kTerrainPS30NoiseNM.hlsl (773 bytes)
#define VKPGM_HASH_KTERRAINPS30NOISE12NM   0xDACFBF8347EE6B10ULL   // kTerrainPS30Noise12NM.hlsl (858 bytes)
#define VKPGM_HASH_KTERRAINPS30BASENMPPL   0x1460C7ABC1ACBDC3ULL   // kTerrainPS30BaseNMPPL.hlsl (996 bytes)
#define VKPGM_HASH_KTERRAINPS30NOISENMPPL  0x863A4CC03913D00EULL   // kTerrainPS30NoiseNMPPL.hlsl (1056 bytes)
#define VKPGM_HASH_KTERRAINPS30NOISE12NMPPL 0x47F9BBA89FCFC288ULL  // kTerrainPS30Noise12NMPPL.hlsl (1141 bytes)
#define VKPGM_HASH_KROADPS30               0xFE7AFB245A477C9BULL   // kRoadPS30.hlsl (526 bytes)
#define VKPGM_HASH_KTREEPS30               0x77FCA405991559E8ULL   // kTreePS30.hlsl (518 bytes)
#define VKPGM_HASH_KPIXELSHADERNOSTAGE1    0xF36101AF52714116ULL   // kPixelShaderNoStage1.hlsl (439 bytes)
#define VKPGM_HASH_KPIXELSHADERRAW         0x2F30B57679A2ACBDULL   // kPixelShaderRaw.hlsl (210 bytes)
#define VKPGM_HASH_KPIXELSHADERLAMBERTRAW  0x1FDC60EF1CABA791ULL   // kPixelShaderLambertRaw.hlsl (492 bytes)
#define VKPGM_HASH_KVERTEXSHADERML         0x6564C0A831086FBCULL   // kVertexShaderML.hlsl (1118 bytes)
#define VKPGM_HASH_KPIXELSHADERML          0x6C88328455A58706ULL   // kPixelShaderML.hlsl (2044 bytes)
#define VKPGM_HASH_KMONOCHROMESHADERBODY   0x8E7D0DEE57C064C3ULL   // kMonochromeShaderBody.hlsl (340 bytes)

static const VKPgm_Program kPgmB1Programs[] = {
    // weather (W3DShaderManager.cpp:6375/6394)
    { 100u, 1u, VKPGM_HASH_KWEATHERCLOUDPS, VKPGMB1_WEATHER_CLOUD_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_WEATHER_CLOUD_FRAG_SPV), "b1.weather.cloud" },        // 316 B
    { 101u, 1u, VKPGM_HASH_KWEATHERVEILPS,  VKPGMB1_WEATHER_VEIL_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_WEATHER_VEIL_FRAG_SPV),  "b1.weather.veil" },         // 976 B

    // terrain30 plain trio (3104/3125/3148) and its NM mirrors (3188/3215/3244), which sample s4
    { 110u, 1u, VKPGM_HASH_KTERRAINPS30BASE,   VKPGMB1_TERRAIN_BASE_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_BASE_FRAG_SPV),   "b1.terrain.base" },        // 514 B
    { 111u, 1u, VKPGM_HASH_KTERRAINPS30NOISE,  VKPGMB1_TERRAIN_NOISE_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_NOISE_FRAG_SPV),  "b1.terrain.noise" },       // 574 B
    { 112u, 1u, VKPGM_HASH_KTERRAINPS30NOISE12, VKPGMB1_TERRAIN_NOISE12_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_NOISE12_FRAG_SPV), "b1.terrain.noise12" },    // 659 B
    { 113u, 1u, VKPGM_HASH_KTERRAINPS30BASENM, VKPGMB1_TERRAIN_BASE_NM_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_BASE_NM_FRAG_SPV), "b1.terrain.base_nm" },    // 713 B
    { 114u, 1u, VKPGM_HASH_KTERRAINPS30NOISENM, VKPGMB1_TERRAIN_NOISE_NM_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_NOISE_NM_FRAG_SPV), "b1.terrain.noise_nm" },  // 773 B
    { 115u, 1u, VKPGM_HASH_KTERRAINPS30NOISE12NM, VKPGMB1_TERRAIN_NOISE12_NM_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_NOISE12_NM_FRAG_SPV), "b1.terrain.noise12_nm" }, // 858 B

    // terrain30 PPL normal-mapped trio (3302/3336/3372): derivative macro normal, constants c3..c5
    { 116u, 1u, VKPGM_HASH_KTERRAINPS30BASENMPPL, VKPGMB1_TERRAIN_BASE_NM_PPL_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_BASE_NM_PPL_FRAG_SPV), "b1.terrain.base_nm_ppl" }, // 996 B
    { 117u, 1u, VKPGM_HASH_KTERRAINPS30NOISENMPPL, VKPGMB1_TERRAIN_NOISE_NM_PPL_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_NOISE_NM_PPL_FRAG_SPV), "b1.terrain.noise_nm_ppl" }, // 1056 B
    { 118u, 1u, VKPGM_HASH_KTERRAINPS30NOISE12NMPPL, VKPGMB1_TERRAIN_NOISE12_NM_PPL_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TERRAIN_NOISE12_NM_PPL_FRAG_SPV), "b1.terrain.noise12_nm_ppl" }, // 1141 B

    // road + tree (W3DShaderManager.cpp:3913, W3DTreeBuffer.cpp:270)
    { 120u, 1u, VKPGM_HASH_KROADPS30, VKPGMB1_ROAD_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_ROAD_FRAG_SPV), "b1.road" },                          // 526 B
    { 121u, 1u, VKPGM_HASH_KTREEPS30, VKPGMB1_TREE_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_TREE_FRAG_SPV), "b1.tree" },                          // 518 B

    // dot3 diagnostic probes, paired with the base registry's dot3 VS (tag 1)
    { 130u, 1u, VKPGM_HASH_KPIXELSHADERNOSTAGE1,   VKPGMB1_DOT3_NOSTAGE1_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_DOT3_NOSTAGE1_FRAG_SPV),   "b1.dot3.nostage1" },      // 439 B
    { 131u, 1u, VKPGM_HASH_KPIXELSHADERRAW,        VKPGMB1_DOT3_RAW_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_DOT3_RAW_FRAG_SPV),        "b1.dot3.raw" },           // 210 B
    { 132u, 1u, VKPGM_HASH_KPIXELSHADERLAMBERTRAW, VKPGMB1_DOT3_LAMBERT_RAW_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_DOT3_LAMBERT_RAW_FRAG_SPV), "b1.dot3.lambert_raw" },  // 492 B

    // ML multi-light pair (4573/4607): one tag per stage, kVertexShaderML has no variants
    { 140u, 0u, VKPGM_HASH_KVERTEXSHADERML, VKPGMB1_ML_VERT_SPV,
      VKPGM_COUNT(VKPGMB1_ML_VERT_SPV), "b1.ml.vert" },                         // 1118 B
    { 141u, 1u, VKPGM_HASH_KPIXELSHADERML,  VKPGMB1_ML_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_ML_FRAG_SPV),  "b1.ml.frag" },                        // 2044 B

    // misc (953, compiled verbatim at 1022); tag 151 reserved for the profiler capture PS
    { 150u, 1u, VKPGM_HASH_KMONOCHROMESHADERBODY, VKPGMB1_MONOCHROME_FRAG_SPV,
      VKPGM_COUNT(VKPGMB1_MONOCHROME_FRAG_SPV), "b1.monochrome" },              // 340 B
};
