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

#endif // _WIN32 && SAGE_USE_VULKAN_CORE
#endif // GENERALSX_VULKANFF_H
