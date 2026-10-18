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
