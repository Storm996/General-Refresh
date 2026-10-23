// GeneralsX @feature Qoder-B2 18/10/2026 wedge 8 bundle B2: GN raw/skinned/sampled variants, the
// GNMerged VS+PS pair, the legacy PBR trio, the water quad family, and the sun-shadow + blur family.
// Owned row file; VKPGM_COUNT and VKPgm_Program are already defined by VulkanPgm.cpp.
//
// Hashes are the pgm_hash.py output over the exported blocks in 引擎改造/Vulkan/文档/内联HLSL/ (byte counts
// quoted per row).  Every export block was byte-verified against the C++ literal it came from before
// hashing; the four water blocks carry the strongest check because the work order flagged the river and
// trapezoid strings as possibly runtime-assembled - they are static adjacent literals in THIS tree
// (W3DWater.cpp:1121/1138) and the exports match them byte for byte.
//
// Deliberate gaps in this table:
// - tag 245 (Generals base-game kShadowBlurShader_2, W3DProjectedShadow.cpp:123) has NO row: its HLSL is
//   byte-identical to the ZH blur (hash 0x13415A51DB2155DF for both), so the tag 244 row already serves
//   any build that compiles the base copy.  The second #define is therefore not emitted here.
// - tags 220/221 (single-light PBR pair) compile at startup but have not been MOUNTED since PS3.0-P3
//   (the merged GN base owns roughness now); the rows exist so a re-mounted variant still hits
//   instead of rejecting its batch.
#include "VkShadersPgmB2.h"

// #define lines emitted verbatim by scripts/pgm_hash.py over the exported blocks (see byte counts on the
// rows below).
#define VKPGM_HASH_KPIXELSHADERGNRAW       0x520BCEDB2C152C8CULL   // kPixelShaderGNRaw.hlsl (372 bytes)
#define VKPGM_HASH_KPIXELSHADERGNS5        0x1E3A2FB6435A8F23ULL   // kPixelShaderGNS5.hlsl (4271 bytes)
#define VKPGM_HASH_KVERTEXSHADERGN_SKIN    0xCD81361114287784ULL   // kVertexShaderGN_SKIN.hlsl (750 bytes)
#define VKPGM_HASH_KPIXELSHADERGNSKIN      0xA3FC8BF90AAF5B82ULL   // kPixelShaderGNSkin.hlsl (1738 bytes)
#define VKPGM_HASH_KVERTEXSHADERGNMERGED_TG3 0x4A0F255C6914588DULL // kVertexShaderGNMerged_TG3.hlsl (1320 bytes)
#define VKPGM_HASH_KPIXELSHADERGNMERGED    0xF4DB3998076F626EULL   // kPixelShaderGNMerged.hlsl (6150 bytes)
#define VKPGM_HASH_KVERTEXSHADERPBR        0x897C5E5C552F0011ULL   // kVertexShaderPBR.hlsl (1370 bytes)
#define VKPGM_HASH_KPIXELSHADERPBR         0xAE1074630B614065ULL   // kPixelShaderPBR.hlsl (2204 bytes)
#define VKPGM_HASH_KWAVEVERTEXSHADERHLSL   0x8F49BD71B7E1098DULL   // kWaveVertexShaderHLSL.hlsl (494 bytes)
#define VKPGM_HASH_KWAVEPIXELSHADERHLSL    0xD6D796FBBF123DC2ULL   // kWavePixelShaderHLSL.hlsl (1705 bytes)
#define VKPGM_HASH_COMPILEWATERPIXELSHADER_RIVER 0x692F748A361FDF35ULL // CompileWaterPixelShader_river.hlsl (520 bytes)
#define VKPGM_HASH_COMPILEWATERPIXELSHADER_TRAPEZOID 0xE2AF9926E0341A12ULL // CompileWaterPixelShader_trapezoid.hlsl (459 bytes)
#define VKPGM_HASH_KDEPTHWRITESHADER       0x7CB9F31AACAE502CULL   // kDepthWriteShader.hlsl (84 bytes)
#define VKPGM_HASH_KFILLFARSHADER          0x4A999FD661CF92C7ULL   // kFillFarShader.hlsl (62 bytes)
#define VKPGM_HASH_KPCFRECEIVESHADER       0x41165B74F0748203ULL   // kPcfReceiveShader.hlsl (1210 bytes)
#define VKPGM_HASH_KRECVPASSTHROUGHVSSHADER 0x26620FFA8907CBB0ULL  // kRecvPassthroughVSShader.hlsl (267 bytes)
#define VKPGM_HASH_KSHADOWBLURSHADER       0x13415A51DB2155DFULL   // kShadowBlurShader.hlsl (352 bytes)

static const VKPgm_Program kPgmB2Programs[] = {
    // GN diagnostics + skinned pair (W3DShaderManager.cpp:5227/5398/5128/5156)
    { 200u, 1u, VKPGM_HASH_KPIXELSHADERGNRAW, VKPGMB2_GN_RAW_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_GN_RAW_FRAG_SPV), "b2.gn.raw" },                       // 372 B
    { 201u, 1u, VKPGM_HASH_KPIXELSHADERGNS5,  VKPGMB2_GNS5_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_GNS5_FRAG_SPV),  "b2.gn.s5" },                         // 4271 B (live log's "[GN] unmatched 4271 bytes")
    { 202u, 0u, VKPGM_HASH_KVERTEXSHADERGN_SKIN, VKPGMB2_GN_SKIN_VERT_SPV,
      VKPGM_COUNT(VKPGMB2_GN_SKIN_VERT_SPV), "b2.gn.skin_vert" },                // 750 B
    { 203u, 1u, VKPGM_HASH_KPIXELSHADERGNSKIN,  VKPGMB2_GN_SKIN_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_GN_SKIN_FRAG_SPV),  "b2.gn.skin_frag" },               // 1738 B

    // GNMerged VS+PS pair, mounted together per DX8GeomNormal::Apply (5501/5540) - building bodies
    { 210u, 0u, VKPGM_HASH_KVERTEXSHADERGNMERGED_TG3, VKPGMB2_GNM_VERT_SPV,
      VKPGM_COUNT(VKPGMB2_GNM_VERT_SPV), "b2.gnm.vert" },                        // 1320 B
    { 211u, 1u, VKPGM_HASH_KPIXELSHADERGNMERGED,      VKPGMB2_GNM_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_GNM_FRAG_SPV),  "b2.gnm.frag" },                       // 6150 B

    // legacy PBR pair (4676/4729); 220/221 unmounted since P3
    { 220u, 0u, VKPGM_HASH_KVERTEXSHADERPBR, VKPGMB2_PBR_VERT_SPV,
      VKPGM_COUNT(VKPGMB2_PBR_VERT_SPV), "b2.pbr.vert" },                        // 1370 B
    { 221u, 1u, VKPGM_HASH_KPIXELSHADERPBR,  VKPGMB2_PBR_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_PBR_FRAG_SPV),  "b2.pbr.frag" },                       // 2204 B

    // water family (W3DWater.cpp:921/937/1121/1138); wave VS+PS pair, river/trap are pure-PS draws
    { 230u, 0u, VKPGM_HASH_KWAVEVERTEXSHADERHLSL, VKPGMB2_WAVE_VERT_SPV,
      VKPGM_COUNT(VKPGMB2_WAVE_VERT_SPV), "b2.wave.vert" },                      // 494 B
    { 231u, 1u, VKPGM_HASH_KWAVEPIXELSHADERHLSL,  VKPGMB2_WAVE_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_WAVE_FRAG_SPV),  "b2.wave.frag" },                     // 1705 B
    { 232u, 1u, VKPGM_HASH_COMPILEWATERPIXELSHADER_RIVER, VKPGMB2_WATER_RIVER_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_WATER_RIVER_FRAG_SPV), "b2.water.river" },             // 520 B
    { 233u, 1u, VKPGM_HASH_COMPILEWATERPIXELSHADER_TRAPEZOID, VKPGMB2_WATER_TRAP_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_WATER_TRAP_FRAG_SPV), "b2.water.trap" },               // 459 B

    // sun shadow map family (W3DSunShadowMap.cpp:93/101/119/153); receiver VS+PS pair at 243/242
    { 240u, 1u, VKPGM_HASH_KDEPTHWRITESHADER, VKPGMB2_SHADOW_DEPTH_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_SHADOW_DEPTH_FRAG_SPV), "b2.shadow.depth" },           // 84 B
    { 241u, 1u, VKPGM_HASH_KFILLFARSHADER,    VKPGMB2_SHADOW_FILL_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_SHADOW_FILL_FRAG_SPV),  "b2.shadow.fill" },            // 62 B
    { 242u, 1u, VKPGM_HASH_KPCFRECEIVESHADER, VKPGMB2_SHADOW_PCF_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_SHADOW_PCF_FRAG_SPV),  "b2.shadow.pcf" },              // 1210 B
    { 243u, 0u, VKPGM_HASH_KRECVPASSTHROUGHVSSHADER, VKPGMB2_SHADOW_RECV_VERT_SPV,
      VKPGM_COUNT(VKPGMB2_SHADOW_RECV_VERT_SPV), "b2.shadow.recv_vert" },        // 267 B

    // projected-shadow atlas blur (W3DProjectedShadow.cpp:124 ZH); the Generals copy (tag 245 reserved,
    // kShadowBlurShader_2.hlsl, hash 0x13415A51DB2155DF, 352 B) is byte-identical - see banner.
    { 244u, 1u, VKPGM_HASH_KSHADOWBLURSHADER, VKPGMB2_SHADOW_BLUR_FRAG_SPV,
      VKPGM_COUNT(VKPGMB2_SHADOW_BLUR_FRAG_SPV), "b2.shadow.blur" },             // 352 B
};
