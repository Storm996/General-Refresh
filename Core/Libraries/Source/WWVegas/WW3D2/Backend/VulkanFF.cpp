// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: CPU reference for the fixed-function vertex stage.
//
// See Backend/VulkanFF.h for what this file is for and why the equations are transcribed from the D3D9
// documentation rather than copied out of shaders/vkff.vert.
//
// Math note: this uses the plain CRT float intrinsics instead of the WWMath wrappers the determinism rules
// otherwise require. That is deliberate and bounded: the file is compiled only into the win64vk target, only
// inside the render device, and nothing it computes can reach the simulation, the replay CRC or a network
// packet - it exists to check a shader. Keeping it on the same intrinsics the GLSL maps to (cosf for cos,
// powf for pow) is what makes the comparison meaningful; routing it through WWMath::Cos would compare the
// vertex stage against a *different* function and call the difference a pass.
#include "VulkanFF.h"

#include <cmath>
#include <cstring>

namespace {

// D3DMATERIALSOURCE ordinals from d3d9types.h, spelled out because this translation unit deliberately does
// not include dx8wrapper.h - the reference consumes the packed state block, not the facade's shadows.
enum {
    BB_D3DMCS_MATERIAL = 0,
    BB_D3DMCS_COLOR1   = 1,
    BB_D3DMCS_COLOR2   = 2
};

inline void v3_copy(float* d, const float* s) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; }
inline float v3_dot(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline float v3_len(const float* a) { return sqrtf(v3_dot(a, a)); }

inline void v3_sub(float* d, const float* a, const float* b)
{
    d[0] = a[0] - b[0]; d[1] = a[1] - b[1]; d[2] = a[2] - b[2];
}
inline void v3_add_eps(float* d, const float* a, float e)
{
    d[0] = a[0] + e; d[1] = a[1] + e; d[2] = a[2] + e;
}
inline void v3_norm(float* d, const float* a)
{
    const float l = v3_len(a);
    if (l > 0.0f) { d[0] = a[0] / l; d[1] = a[1] / l; d[2] = a[2] / l; }
    else { d[0] = d[1] = d[2] = 0.0f; }
}

inline void c4_copy(float* d, const float* s) { memcpy(d, s, 4 * sizeof(float)); }
inline void c4_zero(float* d) { d[0] = d[1] = d[2] = d[3] = 0.0f; }
inline void c4_add(float* d, const float* a, const float* b)
{
    for (int i = 0; i < 4; ++i) d[i] = a[i] + b[i];
}
inline void c4_scale(float* d, const float* a, float s)
{
    for (int i = 0; i < 4; ++i) d[i] = a[i] * s;
}
inline void c4_mul(float* d, const float* a, const float* b)
{
    for (int i = 0; i < 4; ++i) d[i] = a[i] * b[i];
}
inline void c4_madd(float* d, const float* acc, const float* a, float s)
{
    // d = acc + a * s, with d allowed to alias acc only because every element is written from a temporary.
    float t[4];
    for (int i = 0; i < 4; ++i) t[i] = acc[i] + a[i] * s;
    memcpy(d, t, sizeof(t));
}
inline float f_clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }

// D3DMATERIALSOURCE: MATERIAL names the material's own member, COLOR1 the vertex diffuse register and
// COLOR2 the vertex specular register - the same three-way choice D3DRS_DIFFUSEMATERIALSOURCE et al. expose,
// and the ordinals the facade shadow carries verbatim (d3d9types.h: MATERIAL 0, COLOR1 1, COLOR2 2).
inline void pick_source(unsigned src, const float* matColour, const float* vcol1, const float* vcol2, float* out)
{
    if (src == BB_D3DMCS_COLOR1) { c4_copy(out, vcol1); return; }
    if (src == BB_D3DMCS_COLOR2) { c4_copy(out, vcol2); return; }
    c4_copy(out, matColour);
}

// The three light-type ordinals of d3d9types.h, spelled here so the reference reads the way the block is
// documented: the type field carries D3DLIGHT9::Type unchanged.
enum {
    BB_LIGHT_POINT        = 1,
    BB_LIGHT_SPOT         = 2,
    BB_LIGHT_DIRECTIONAL  = 3
};

// The four vertex fog modes of D3DFOGMODE, same reason.
enum {
    BB_FOG_NONE  = 0,
    BB_FOG_EXP   = 1,
    BB_FOG_EXP2  = 2,
    BB_FOG_LINEAR = 3
};

// ----------------------------------------------------------------------------
// wedge 7: the texture combiner
//
// The D3DTOP ordinals of d3d8types.h / d3d9types.h - the two headers number them identically, which was
// checked against Windows Kits/10/Include/10.0.26100.0/shared/d3d9types.h before this table was written,
// because an off-by-one here turns MODULATE into SELECTARG2 and every textured surface in the game into a
// colour block. The row-by-row results below are the documented ones; three corners of the table depend on
// registers the state block does not carry (D3DTSS_CONSTANT, the bump-env matrices) and are documented as
// approximations in both vkff.frag and the reference, so a visual difference names the same corner twice.
// ----------------------------------------------------------------------------
enum {
    BB_TOP_DISABLE = 1,   BB_TOP_SELECTARG1,        BB_TOP_SELECTARG2,     BB_TOP_MODULATE,
    BB_TOP_MODULATE2X,    BB_TOP_MODULATE4X,        BB_TOP_ADD,            BB_TOP_ADDSIGNED,
    BB_TOP_ADDSIGNED2X,   BB_TOP_SUBTRACT,          BB_TOP_ADDSMOOTH,      BB_TOP_BLENDDIFFUSEALPHA,
    BB_TOP_BLENDTEXTUREALPHA, BB_TOP_BLENDFACTORALPHA, BB_TOP_BLENDTEXTUREALPHAPM, BB_TOP_BLENDCURRENTALPHA,
    BB_TOP_PREMODULATE,   BB_TOP_MODULATEALPHA_ADDCOLOR, BB_TOP_MODULATECOLOR_ADDALPHA,
    BB_TOP_MODULATEINVALPHA_ADDCOLOR, BB_TOP_MODULATEINVCOLOR_ADDALPHA,
    BB_TOP_BUMPENVMAP,    BB_TOP_BUMPENVMAPLUMINANCE, BB_TOP_DOTPRODUCT3,  BB_TOP_MULTIPLYADD,
    BB_TOP_LERP           // = 26
};

enum {
    BB_TA_DIFFUSE = 0, BB_TA_CURRENT = 1, BB_TA_TEXTURE = 2, BB_TA_TFACTOR = 3,
    BB_TA_SPECULAR = 4, BB_TA_TEMP = 5, BB_TA_CONSTANT = 6,
    BB_TA_COMPLEMENT = 0x10, BB_TA_ALPHAREPLICATE = 0x20
};

// The 0-based compare table the folded-in alpha test decodes out of flags[2]. d3d8types.h numbers
// D3DCMP_NEVER as 1, so VKFF_PACK_ALPHA_TEST subtracts one before packing.
enum {
    BB_CMP_NEVER = 0, BB_CMP_LESS, BB_CMP_EQUAL, BB_CMP_LESSEQUAL,
    BB_CMP_GREATER, BB_CMP_NOTEQUAL, BB_CMP_GREATEREQUAL, BB_CMP_ALWAYS
};

inline float bb_sat(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline void  bb_mix(float* d, const float* a, const float* b, float t)
{
    for (int i = 0; i < 3; ++i) d[i] = a[i] + (b[i] - a[i]) * t;
}

// One D3DTA_* argument, selector then modifiers. The documented order matters: ALPHAREPLICATE broadcasts
// the alpha into all four channels and COMPLEMENT then inverts that, which is what makes
// TEXTURE|ALPHAREPLICATE|COMPLEMENT mean "one minus the texel alpha" in every channel.
void bb_arg(unsigned bits, const VKDraw_FF_State& ff, const float* texel, const float* current,
            const float* temp, const float* vcolor, const float* vspec, float* out)
{
    switch (bits & 0x0Fu) {
    case BB_TA_DIFFUSE:  c4_copy(out, vcolor); break;
    case BB_TA_CURRENT:  c4_copy(out, current); break;
    case BB_TA_TEXTURE:  c4_copy(out, texel); break;
    case BB_TA_TFACTOR:  c4_copy(out, ff.tex_factor); break;
    case BB_TA_SPECULAR: c4_copy(out, vspec); break;
    case BB_TA_TEMP:     c4_copy(out, temp); break;
    default:             c4_zero(out); break;      // BB_TA_CONSTANT: not carried in the block
    }
    if ((bits & BB_TA_ALPHAREPLICATE) != 0u) { const float a = out[3]; out[0] = out[1] = out[2] = a; }
    if ((bits & BB_TA_COMPLEMENT) != 0u)     { for (int i = 0; i < 4; ++i) out[i] = 1.0f - out[i]; }
}

// The three scalars the blend and add-family rows read from elsewhere: the diffuse alpha (D3DTA_DIFFUSE's
// own channel), the current alpha and the texture factor's alpha.
void bb_top_rgb(unsigned op, const float* a0, const float* a1, const float* a2,
                float a1a, float a2a, float difA, float curA, float tfA, float* rgb)
{
    switch (op) {
    case BB_TOP_SELECTARG1: case BB_TOP_DISABLE:              rgb[0] = a1[0]; rgb[1] = a1[1]; rgb[2] = a1[2]; break;
    case BB_TOP_SELECTARG2:                                   rgb[0] = a2[0]; rgb[1] = a2[1]; rgb[2] = a2[2]; break;
    case BB_TOP_MODULATE: case BB_TOP_PREMODULATE:            for (int i = 0; i < 3; ++i) rgb[i] = a1[i] * a2[i]; break;
    // The four "specular mapping" rows, transcribed from the register comment in d3d9types.h rather than
    // from prose: MODULATEALPHA_ADDCOLOR is Arg1.RGB + Arg1.A*Arg2.RGB, MODULATECOLOR_ADDALPHA is
    // Arg1.RGB*Arg2.RGB + Arg1.A, and the two INV variants swap which argument is complemented. All four
    // are marked COLOROP only there, which is why bb_top_alpha keeps a plain product for them and the probe
    // never names them in an alpha group. Getting these right matters: every first transcription (this
    // file's and the GLSL's) dropped the additive term and kept only the product.
    case BB_TOP_MODULATEALPHA_ADDCOLOR:    for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + a1a * a2[i]; break;
    case BB_TOP_MODULATECOLOR_ADDALPHA:    for (int i = 0; i < 3; ++i) rgb[i] = a1[i] * a2[i] + a1a; break;
    case BB_TOP_MODULATEINVALPHA_ADDCOLOR: for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + (1.0f - a1a) * a2[i]; break;
    case BB_TOP_MODULATEINVCOLOR_ADDALPHA: for (int i = 0; i < 3; ++i) rgb[i] = a1a + (1.0f - a1[i]) * a2a; break;
    case BB_TOP_MODULATE2X:          for (int i = 0; i < 3; ++i) rgb[i] = 2.0f * a1[i] * a2[i]; break;
    case BB_TOP_MODULATE4X:          for (int i = 0; i < 3; ++i) rgb[i] = 4.0f * a1[i] * a2[i]; break;
    case BB_TOP_ADD:                 for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + a2[i]; break;
    case BB_TOP_ADDSIGNED:           for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + a2[i] - 0.5f; break;
    case BB_TOP_ADDSIGNED2X:         for (int i = 0; i < 3; ++i) rgb[i] = 2.0f * (a1[i] + a2[i] - 0.5f); break;
    case BB_TOP_SUBTRACT:            for (int i = 0; i < 3; ++i) rgb[i] = a1[i] - a2[i]; break;
    case BB_TOP_ADDSMOOTH:           for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + a2[i] * (1.0f - a1[i]); break;
    case BB_TOP_BLENDDIFFUSEALPHA:   for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + (a2[i] - a1[i]) * difA; break;
    case BB_TOP_BLENDTEXTUREALPHA:   for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + (a2[i] - a1[i]) * a2a; break;
    case BB_TOP_BLENDFACTORALPHA:    for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + (a2[i] - a1[i]) * tfA; break;
    case BB_TOP_BLENDTEXTUREALPHAPM: for (int i = 0; i < 3; ++i) rgb[i] = a1[i] * (1.0f - a2a) + a2[i]; break;
    case BB_TOP_BLENDCURRENTALPHA:   for (int i = 0; i < 3; ++i) rgb[i] = a1[i] + (a2[i] - a1[i]) * curA; break;
    // The SDK header's own description of the op, quoted in vkff.frag: the sum of the three component
    // products, each component first scaled and offset to be signed (c' = 2c - 1), the scalar replicated
    // into the colour channels, colour-only. The census never shows this op - the engine's dot3 normal
    // path is a real pixel shader, not a combiner - but the row is probed anyway, because a row nobody
    // measured is a row nobody may claim.
    case BB_TOP_DOTPRODUCT3: {
        const float d = (2.0f * a1[0] - 1.0f) * (2.0f * a2[0] - 1.0f)
                      + (2.0f * a1[1] - 1.0f) * (2.0f * a2[1] - 1.0f)
                      + (2.0f * a1[2] - 1.0f) * (2.0f * a2[2] - 1.0f);
        rgb[0] = rgb[1] = rgb[2] = d;
        break;
    }
    case BB_TOP_MULTIPLYADD:         for (int i = 0; i < 3; ++i) rgb[i] = a0[i] + a1[i] * a2[i]; break;
    case BB_TOP_LERP:                for (int i = 0; i < 3; ++i) rgb[i] = a0[i] + (a2[i] - a0[i]) * a1[i]; break;
    // The two bump rows need D3DTSS_BUMPENVMAT00..11, which the block does not carry; at zero matrices the
    // documented equation is exactly arg1's colour, so that is what they do here and in the shader.
    default:                         rgb[0] = a1[0]; rgb[1] = a1[1]; rgb[2] = a1[2]; break;
    }
}

float bb_top_alpha(unsigned op, float a0, float a1, float a2, float difA, float curA, float tfA)
{
    // The four ADDCOLOR/ADDALPHA rows and DOTPRODUCT3 are marked COLOROP only in d3d9types.h, so their
    // behaviour as an ALPHAOP is not specified anywhere. This table and vkff.frag's agree on a plain
    // product for them - which is the only thing a scalar-only signature can do - and the probe never
    // names them in an alpha group.
    switch (op) {
    case BB_TOP_SELECTARG1: case BB_TOP_DISABLE:  return a1;
    case BB_TOP_SELECTARG2:                       return a2;
    case BB_TOP_MODULATE: case BB_TOP_PREMODULATE:
    case BB_TOP_MODULATEALPHA_ADDCOLOR: case BB_TOP_MODULATECOLOR_ADDALPHA:
    case BB_TOP_DOTPRODUCT3:                      return a1 * a2;
    case BB_TOP_MODULATEINVALPHA_ADDCOLOR:        return (1.0f - a1) * a2;
    case BB_TOP_MODULATEINVCOLOR_ADDALPHA:        return a1 * (1.0f - a2);
    case BB_TOP_MODULATE2X:                       return 2.0f * a1 * a2;
    case BB_TOP_MODULATE4X:                       return 4.0f * a1 * a2;
    case BB_TOP_ADD:                              return a1 + a2;
    case BB_TOP_ADDSIGNED:                        return a1 + a2 - 0.5f;
    case BB_TOP_ADDSIGNED2X:                      return 2.0f * (a1 + a2 - 0.5f);
    case BB_TOP_SUBTRACT:                         return a1 - a2;
    case BB_TOP_ADDSMOOTH:                        return a1 + a2 * (1.0f - a1);
    case BB_TOP_BLENDDIFFUSEALPHA:                return a1 + (a2 - a1) * difA;
    case BB_TOP_BLENDTEXTUREALPHA:                return a1 + (a2 - a1) * a2;
    case BB_TOP_BLENDFACTORALPHA:                 return a1 + (a2 - a1) * tfA;
    case BB_TOP_BLENDTEXTUREALPHAPM:              return a1 * (1.0f - a2) + a2;
    case BB_TOP_BLENDCURRENTALPHA:                return a1 + (a2 - a1) * curA;
    case BB_TOP_MULTIPLYADD:                      return a0 + a1 * a2;
    case BB_TOP_LERP:                             return a0 + (a2 - a0) * a1;
    default:                                      return a1;
    }
}

} // anon

void VKFF_Vertex_Ref(const VKDraw_FF_State& ff, const VKFF_Vertex_In& in, VKFF_Vertex_Out* out)
{
    const unsigned fx = ff.flags[0];
    const unsigned fy = ff.flags[1];

    // ---- normal ------------------------------------------------------------------------------
    // A layout without D3DFVF_NORMAL gets the same fallback the vertex stage's default stream carries, and
    // NORMALIZENORMALS is applied by the caller because the caller owns the FVF. The trailing epsilon in the
    // shader's normalize is reproduced here so a degenerate normal divides by the same number on both sides.
    float nWorld[3]; v3_copy(nWorld, in.normal);
    if ((fx & VKFF_FLAG_HAS_NORMAL) == 0u) { nWorld[0] = 0.0f; nWorld[1] = 0.0f; nWorld[2] = -1.0f; }
    float nEps[3]; v3_add_eps(nEps, nWorld, 1e-20f);
    float nUnit[3]; v3_norm(nUnit, nEps);

    // ---- colour sources ------------------------------------------------------------------------
    const float gate = (fx & VKFF_FLAG_COLORVERTEX) != 0u ? 1.0f : 0.0f;
    float vcol1[4], vcol2[4];
    if ((fx & VKFF_FLAG_HAS_DIFFUSE) != 0u) c4_copy(vcol1, in.diffuse);
    else { vcol1[0] = vcol1[1] = vcol1[2] = vcol1[3] = 1.0f; }
    if ((fx & VKFF_FLAG_HAS_SPECULAR) != 0u) c4_copy(vcol2, in.specular);
    else { vcol2[0] = vcol2[1] = vcol2[2] = 0.0f; vcol2[3] = 1.0f; }
    const unsigned srcAmb  = (unsigned)(gate * (float)VKFF_AMBIENT_SRC(fy));
    const unsigned srcDiff = (unsigned)(gate * (float)VKFF_DIFFUSE_SRC(fy));
    const unsigned srcSpec = (unsigned)(gate * (float)VKFF_SPECULAR_SRC(fy));
    const unsigned srcEmis = (unsigned)(gate * (float)VKFF_EMISSIVE_SRC(fy));
    float ambCol[4], diffCol[4], specCol[4], emisCol[4];
    pick_source(srcAmb,  ff.mat_ambient,  vcol1, vcol2, ambCol);
    pick_source(srcDiff, ff.mat_diffuse,  vcol1, vcol2, diffCol);
    pick_source(srcSpec, ff.mat_specular, vcol1, vcol2, specCol);
    pick_source(srcEmis, ff.mat_emissive, vcol1, vcol2, emisCol);

    // ---- fog ---------------------------------------------------------------------------------
    // D3DRS_FOGENABLE off, or on with the VERTEX mode NONE, leaves the factor at 1.0; only the vertex term
    // is evaluated here, because the table-fog modes belong to the pixel stage of wedge 7.
    float fog = 1.0f;
    if ((fx & VKFF_FLAG_FOG_ENABLE) != 0u) {
        const float depth = fabsf(in.view[2]);
        const unsigned vm = VKFF_FOG_VERTEX(fy);
        if (vm == BB_FOG_LINEAR) {
            const float start = ff.fog_params[0], end = ff.fog_params[1];
            fog = (end == start) ? 1.0f : (end - depth) / (end - start);
        } else if (vm == BB_FOG_EXP) {
            fog = expf(-ff.fog_params[2] * depth);
        } else if (vm == BB_FOG_EXP2) {
            const float d = ff.fog_params[2] * depth;
            fog = expf(-d * d);
        }
        fog = f_clamp01(fog);
    }
    out->fog = fog;

    // ---- the light loop ------------------------------------------------------------------------
    float sumAmbient[4], sumDiffuse[4], sumSpecular[4];
    c4_zero(sumAmbient); c4_zero(sumDiffuse); c4_zero(sumSpecular);

    if ((fx & VKFF_FLAG_LIGHTING) == 0u) {
        // D3DRS_LIGHTING off is not "black": the diffuse register is the colour, unlit. Every sprite, UI
        // quad and decal in the game takes this branch, which is why it is checked first, not last.
        c4_copy(out->color, diffCol);
        c4_zero(out->specular);
        return;
    }

    const unsigned lightMask = VKFF_LIGHT_MASK(fy);
    const bool specularOn = (fx & VKFF_FLAG_SPECULAR_ENABLE) != 0u;
    const float power = ff.mat_power[0] > 1.0f ? ff.mat_power[0] : 1.0f;

    float eye[3];
    if (ff.eye_position[3] > 0.5f) v3_copy(eye, ff.eye_position);
    else { eye[0] = in.world[0]; eye[1] = in.world[1]; eye[2] = in.world[2] + 1.0f; }
    float toEye[3]; v3_sub(toEye, eye, in.world);
    float viewDir[3]; v3_norm(viewDir, toEye);

    for (int i = 0; i < 8; ++i) {
        if (((lightMask >> (unsigned)i) & 1u) == 0u) continue;
        const VKDraw_FF_Light& L = ff.lights[i];
        const unsigned type = (unsigned)L.type_att[0];

        float toLight[3];
        float dist = 1.0f;
        if (type == BB_LIGHT_DIRECTIONAL) {
            const float dir[3] = { L.dir_theta[0], L.dir_theta[1], L.dir_theta[2] };
            const float neg[3] = { -dir[0], -dir[1], -dir[2] };   // Direction is where the light travels
            v3_norm(toLight, neg);
        } else {
            float d[3]; v3_sub(d, L.pos_range, in.world);
            dist = v3_len(d);
            const float safe = dist > 1e-20f ? dist : 1e-20f;
            toLight[0] = d[0] / safe; toLight[1] = d[1] / safe; toLight[2] = d[2] / safe;
        }

        float atten = 1.0f;
        if (type != BB_LIGHT_DIRECTIONAL) {
            // 1 / (att0 + att1*d + att2*d^2) with d the ABSOLUTE distance: DX8Wrapper::Set_Light fills
            // Attenuation1 with 1/inner-radius, which puts half intensity exactly on that radius, and no
            // term in the retail equation divides by Range. The clamp to 1/att0 is the documented ceiling
            // on the curve (it stops a near-zero distance from blowing the factor past 1).
            const float a = L.type_att[1] + L.type_att[2] * dist + L.type_att[3] * dist * dist;
            atten = a > 1e-20f ? 1.0f / a : 0.0f;
            if (L.type_att[1] > 0.0f && atten > 1.0f / L.type_att[1]) atten = 1.0f / L.type_att[1];

            if (type == BB_LIGHT_SPOT) {
                // Theta is the inner cone, Phi the outer, both radians, and both are measured from the
                // travel direction. This engine installs the same angle in both (Set_Light copies
                // Spot_Angle into the pair), so the ramp has zero width and the cone is an edge; the
                // general form stays because a mod that separates the two gets D3D9's smooth rim.
                const float ci = cosf(L.dir_theta[3]);
                const float co = cosf(L.phi_falloff[0]);
                const float ldir[3] = { L.dir_theta[0], L.dir_theta[1], L.dir_theta[2] };
                const float neg[3] = { -ldir[0], -ldir[1], -ldir[2] };
                float axis[3]; v3_norm(axis, neg);
                const float cd = v3_dot(toLight, axis);
                float sd;
                if (ci - co > 1e-6f) sd = f_clamp01((cd - co) / (ci - co));
                else sd = (cd >= ci) ? 1.0f : 0.0f;
                if (sd > 0.0f && L.phi_falloff[1] > 0.0f) sd = powf(sd, L.phi_falloff[1]);
                atten *= sd;
            }
        }

        c4_add(sumAmbient, sumAmbient, L.ambient);

        const float ndotl = v3_dot(nUnit, toLight);
        if (ndotl > 0.0f) {
            c4_madd(sumDiffuse, sumDiffuse, L.diffuse, ndotl * atten);
            if (specularOn) {
                // Blinn, as D3D9 documents it: the half vector is compared through the specular power, and
                // the reflection form below is the algebraically identical way to write it.
                float refl[3];
                for (int k = 0; k < 3; ++k) refl[k] = 2.0f * ndotl * nUnit[k] - toLight[k];
                float reflN[3]; v3_norm(reflN, refl);
                const float rdotv = v3_dot(reflN, viewDir);
                if (rdotv > 0.0f) c4_madd(sumSpecular, sumSpecular, L.specular, powf(rdotv, power) * atten);
            }
        }
    }

    float ambTerm[4]; c4_add(ambTerm, ff.scene_ambient, sumAmbient);
    float t1[4], t2[4];
    c4_mul(t1, ambCol, ambTerm);
    c4_mul(t2, diffCol, sumDiffuse);
    c4_add(out->color, t1, t2);
    c4_add(out->color, out->color, emisCol);
    out->color[3] = diffCol[3];    // the vertex alpha rides through; light alphas are 1.0 by convention
    c4_mul(out->specular, specCol, sumSpecular);
}

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7: the combiner interpreter's second side.
//
// The eight-stage loop, exactly as D3D8 documents it: the current register starts at the diffuse vertex
// colour (which is what the vertex stage's vColor is, lit or unlit), each stage reads its texel from the
// texture slot of the same index, and D3DTSS_RESULTARG decides whether the result lands in the current
// register or in the per-stage temp that the next stage's D3DTA_TEMP reads. A stage whose colour AND alpha
// ops are both DISABLE writes nothing at all - the census found that configuration on the majority of
// draws, because the engine disables every stage above the ones a pass uses.
int VKFF_Combine_Ref(const VKDraw_FF_State& ff, const VKFF_Pixel_In& in, float* out_rgba)
{
    float current[4], temp[4], result[4], texel[4];
    c4_copy(current, in.color);
    c4_zero(temp);
    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    for (int s = 0; s < (int)VKFF_MAX_STAGES; ++s) {
        const VKDraw_FF_TSS& T = ff.tss[s];
        if (T.col[0] == BB_TOP_DISABLE && T.alp[0] == BB_TOP_DISABLE) continue;

        // Slot 's' for the first four stages; the descriptor set carries four sampler bindings, so a
        // fixed-function stage above that samples the built-in white - the same convention vkff.frag uses
        // and the same one the facade counts before it lets a batch through.
        c4_copy(texel, (s < 4) ? in.texel[s] : white);

        float a0c[4], a1c[4], a2c[4], a0a[4], a1a[4], a2a[4];
        bb_arg(T.col[1], ff, texel, current, temp, in.color, in.specular, a0c);
        bb_arg(T.col[2], ff, texel, current, temp, in.color, in.specular, a1c);
        bb_arg(T.col[3], ff, texel, current, temp, in.color, in.specular, a2c);
        bb_arg(T.alp[1], ff, texel, current, temp, in.color, in.specular, a0a);
        bb_arg(T.alp[2], ff, texel, current, temp, in.color, in.specular, a1a);
        bb_arg(T.alp[3], ff, texel, current, temp, in.color, in.specular, a2a);

        c4_copy(result, current);
        if (T.col[0] != BB_TOP_DISABLE) {
            float rgb[3];
            bb_top_rgb(T.col[0], a0c, a1c, a2c, a1c[3], a2c[3], in.color[3], current[3], ff.tex_factor[3], rgb);
            for (int i = 0; i < 3; ++i) result[i] = bb_sat(rgb[i]);
        }
        if (T.alp[0] != BB_TOP_DISABLE) {
            result[3] = bb_sat(bb_top_alpha(T.alp[0], a0a[3], a1a[3], a2a[3],
                                            in.color[3], current[3], ff.tex_factor[3]));
        }

        if (T.res[0] == BB_TA_TEMP) c4_copy(temp, result);
        else                        c4_copy(current, result);
    }

    // D3D9 adds the vertex specular register after the last stage. With SPECULARENABLE off the vertex stage
    // writes a zero vSpecular, so this is a no-op for the batches the census says dominate.
    for (int i = 0; i < 3; ++i) current[i] = bb_sat(current[i] + in.specular[i]);

    // The alpha test Vulkan has no pipeline state for, read out of the word wedge 6 left reserved. The
    // pixel's alpha is truncated to the 8-bit scale D3DRS_ALPHAREF is written in, which makes a pixel
    // exactly on the reference fail a GREATER compare - the fixed-function unit's behaviour, and the knife
    // edge the probe's rigs stay away from. A discard writes nothing at all, so the caller is told.
    if (VKFF_ALPHA_TEST_ENABLE(ff.flags[2])) {
        const float a = bb_sat(current[3]);
        const float pa = floorf(a * 255.0f);
        const float pr = (float)VKFF_ALPHA_REF(ff.flags[2]);
        const unsigned cmp = VKFF_ALPHA_FUNC(ff.flags[2]);
        bool pass;
        switch (cmp) {
        case BB_CMP_LESS:         pass = pa <  pr; break;
        case BB_CMP_EQUAL:        pass = pa == pr; break;
        case BB_CMP_LESSEQUAL:    pass = pa <= pr; break;
        case BB_CMP_GREATER:      pass = pa >  pr; break;
        case BB_CMP_NOTEQUAL:     pass = pa != pr; break;
        case BB_CMP_GREATEREQUAL: pass = pa >= pr; break;
        case BB_CMP_NEVER:        pass = false;    break;
        default:                  pass = true;     break;   // BB_CMP_ALWAYS, and any other ordinal
        }
        if (!pass) return 0;      // the shader writes nothing either: the target keeps what was already there
    }

    // Fog blends the colour channels toward D3DRS_FOGCOLOR and leaves alpha alone - the documented fixed
    // function behaviour, and the reason a fogged surface keeps its transparency. The table mode wins over
    // the vertex mode when both are set, because the table value supersedes the interpolated one.
    if ((ff.flags[0] & VKFF_FLAG_FOG_ENABLE) != 0u) {
        const unsigned tm = VKFF_FOG_TABLE(ff.flags[1]);
        float f = 1.0f;
        if (tm != BB_FOG_NONE) {
            const float depth = in.fog_depth;
            if (tm == BB_FOG_LINEAR) {
                const float start = ff.fog_params[0], end = ff.fog_params[1];
                f = (end == start) ? 1.0f : (end - depth) / (end - start);
            } else if (tm == BB_FOG_EXP) {
                f = expf(-ff.fog_params[2] * depth);
            } else if (tm == BB_FOG_EXP2) {
                const float d = ff.fog_params[2] * depth;
                f = expf(-d * d);
            }
            f = bb_sat(f);
        } else {
            f = in.fog_vertex;
        }
        float rgb[3];
        bb_mix(rgb, ff.fog_color, current, f);
        for (int i = 0; i < 3; ++i) current[i] = rgb[i];
    }

    c4_copy(out_rgba, current);
    return 1;
}
