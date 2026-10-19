// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the fixed-function vertex stage's second opinion.
//
// What this layer is. Wedge 6 replaces the D3D9 device's generated vertex shader with one written by hand
// (Vulkan/shaders/vkff.vert, baked to Backend/VkShadersFF.h). A hand-written fixed-function pipeline has the
// same problem as a hand-written math library: it looks right, and only a number says whether it is. This
// file holds the CPU reference for exactly the equations the vertex stage implements - the eight light slots,
// the material colour sources, the attenuation curve, the spot cone and the vertex fog factors - so the
// acceptance probe can rasterise a synthetic rig with the real pipeline and compare the readback against
// bytes the CPU produced from the documented D3D9 behaviour, not from the GLSL.
//
// Why the reference is written from the specifications and not from the shader: a reference copied off the
// implementation proves the implementation equals itself. Both sides here were derived from the same source
// of truth (the D3D9 fixed-function documentation) plus the two engine-specific facts the census and
// DX8Wrapper::Set_Light established - attenuation uses the ABSOLUTE distance with no Range normalisation,
// and Spot_Angle arrives as a half-angle in radians rather than the D3D9 degrees that would be halved. Where
// those two decisions are wrong, both sides are wrong together and the visual A/B owns them; where the GLSL
// has a typo, a sign, or a swizzle in it, this file catches it.
//
// Vulkan-handle-free like VulkanGeo.h and VulkanDesc.h: it consumes VKDraw_FF_State (the block the facade
// packs) and plain float arrays, so no Vulkan type crosses into this translation unit.
#ifndef GENERALSX_VULKANFF_H
#define GENERALSX_VULKANFF_H

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

#include "VulkanDraw.h"   // VKDraw_FF_State and the VKFF_FLAG_* / VKFF_* bit fields

// One vertex as the lighting reference sees it. The caller (the probe, or the wedge-7 readback tooling) has
// already run the transforms, because the position path is geometry, not lighting, and is checked by where
// the pixels land rather than by arithmetic here.
struct VKFF_Vertex_In {
    float world[3];       // world-space position - what the lights are measured against
    float view[3];        // camera-space position; -z is the distance fog and the specular eye vector use
    float normal[3];      // world-space normal, after the world transform's rotation (and after
                          // NORMALIZENORMALS, which the caller applies because the shader does)
    float diffuse[4];     // the D3DFVF_DIFFUSE register unpacked to 0..1 floats, or the FVF default
    float specular[4];    // the D3DFVF_SPECULAR register, same convention
};

struct VKFF_Vertex_Out {
    float color[4];       // the vColor the vertex stage would emit
    float specular[4];    // the vSpecular it would emit (zero whenever SPECULARENABLE is off)
    float fog;            // the vertex fog factor in vFog.y (1.0 when fog is off or table-only)
};

// Evaluate the D3D8 fixed-function vertex lighting + vertex fog equations for one vertex against one packed
// state block. Deterministic, allocation-free, and deliberately written as an independent transcription of
// the documented equations: see the block comment above for why it is not a port of the GLSL.
void VKFF_Vertex_Ref(const VKDraw_FF_State& ff, const VKFF_Vertex_In& in, VKFF_Vertex_Out* out);

// The wedge-6 acceptance probe: a synthetic rig of eight bands, one draw per band, each with its own packed
// VKDraw_FF_State in the lights ring, rendered by the real vkff.vert into a float (or half-float, or RGBA8 -
// whichever this GPU will accept as a colour attachment) RT mirror, read back and compared against
// VKFF_Vertex_Ref interpolated to the pixel centre. Logs the gate line
//   VKFF probe: fmt=.. bands=.. checked=.. skipped=.. mismatches=.. maxAbsErr=..
// and runs once at co-create time behind GENERALSX_VKFFVERT=1.
void VKFF_Run_Probe();

// ---- wedge 7: the texture combiner -----------------------------------------------------------------
//
// What the second side is now. vkff.frag replaces the pixel program the D3D9 device generated per
// texture-stage combination with one interpreter that replays D3DTSS_* out of the same packed block. The
// reference below is that table again, transcribed from the D3D8/9 documentation rather than from the
// GLSL, for the same reason the vertex equations are: a reference copied off the implementation only
// proves the implementation equals itself. It covers the eight-stage loop, the D3DTA_* selectors with
// their two modifier bits, the 26 D3DTOP rows for both channel groups, D3DTSS_RESULTARG routing, the
// post-combiner specular add, the alpha test folded in from flags[2] and the table/vertex fog choice.
//
// Everything it consumes is already interpolated, because interpolation is the rasteriser's business and
// the probe's rigs make every vertex of a band emit the same value (see VulkanCore.cpp's FF probe). The
// four texels are the four mirrored texture slots as the sampler returned them; a stage above the fourth
// gets white, which is the descriptor set's real limit until wedge 8 widens the layout.
struct VKFF_Pixel_In {
    float color[4];      // vColor: the colour the combiner's current register starts from
    float specular[4];   // vSpecular: added after the last stage
    float texel[4][4];   // slots 0..3, already sampled
    float fog_depth;     // eye depth at this pixel, for the table fog modes
    float fog_vertex;    // vFog.y, the interpolated vertex fog factor
};

// Returns 1 when the pixel survives the alpha test and out_rgba is what lands in the target, 0 when it was
// discarded - which the probe needs, because a discarded pixel keeps the clear value rather than any colour
// arithmetic can produce.
int VKFF_Combine_Ref(const VKDraw_FF_State& ff, const VKFF_Pixel_In& in, float* out_rgba);

// The wedge-7 acceptance probe: one band per combiner construct the interpreter can reach, drawn through
// the real vkff.vert + vkff.frag pipeline into an RT mirror, with two 1x1 textures uploaded through the
// real wedge-3.2 mirror path, read back and compared against VKFF_Combine_Ref. Logs
//   VKFFPIX probe: fmt=.. bands=.. checked=.. skipped=.. mismatches=.. ...
// and runs once at co-create time behind GENERALSX_VKFFPIX=1.
void VKFF_Run_Pixel_Probe();

#endif // _WIN32 && SAGE_USE_VULKAN_CORE
#endif // GENERALSX_VULKANFF_H
