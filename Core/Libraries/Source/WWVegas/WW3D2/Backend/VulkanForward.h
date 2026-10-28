// GeneralsX @feature Qoder 20/10/2026 Forward+: per-frame point-light feed from the game layer.
//
// The renderer used to let each drawable's LightEnvironment choose its own top-8 lights. That is a
// per-object cluster with a hard cap: a base with more than eight lamps loses lights, and a point
// light used to disqualify a batch from the per-pixel path entirely. Forward+ removes both limits:
// the scene publishes every active point light once per frame, the draw layer builds a screen-tile x
// eye-depth cluster list on the CPU, and the GN pixel shaders iterate only the lights that overlap
// their cluster. The block itself is a read-only SSBO (binding 4, 90,272 bytes): a UBO would exceed
// NVIDIA's 65,536-byte maxUniformBufferRange.
//
// The API is intentionally tiny and has no-op fallbacks so callers do not need preprocessor guards:
// VK_Forward_Begin() once before the scene draw, one VK_Forward_Add_Light() per point light, and the
// draw layer consumes the list at the first scene submit. VK_Forward_Active() reports whether the
// feed has data for this frame; DX8GeomNormal uses it to pick the cluster loop over the per-object list.
#pragma once

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

// Start a new frame's light list. Called from RTS3DScene::doRender before DRAW().
void VK_Forward_Begin();

// Add a point light in world space. 'intensity' is folded by the caller; inner/outer are the far
// attenuation radii in world units (inner >= outer or outer <= 0 means no finite falloff).
void VK_Forward_Add_Light(float x, float y, float z, float inner, float outer,
                          float r, float g, float b);

// GeneralsX @feature Qoder 24/10/2026 P4 D2: publish a spot light. 'dir' is the cone axis in world
// space pointing away from the fixture (it does not have to be unit length). cosInner/cosOuter are the
// cosine of the inner/outer half-angles (cosInner >= cosOuter); 'exponent' shapes the falloff inside
// the cone (1 = linear smoothstep, larger = tighter core). 'cookieId' selects the procedural gobo
// (VkLighting::kCookie*). The record travels in the same SSBO as the light (VkLightingLayout.h v2), so
// the per-object eight-light fallback still treats it as a point light - an explicit degradation, not a
// silent drop, and exactly what the P4 audit allows while Forward+ is off.
void VK_Forward_Add_Spot(float x, float y, float z, float inner, float outer,
                         float r, float g, float b,
                         float dirX, float dirY, float dirZ,
                         float cosInner, float cosOuter, float exponent, int cookieId);

// True when this frame's list has at least one light and the cluster block was built.
int VK_Forward_Active();

// GeneralsX @feature 24/10/2026 P34 E3: remember the scene's published list. W3DDisplay renders more
// than one scene per frame (world, then the 3D interface/overlay), and every RTS3DScene::doRender calls
// VK_Forward_Begin - so by the time the shadow pass runs the live list is the overlay's (usually empty)
// and real lamps could never get a shadow map. Called at the end of the publish block, this keeps the
// last non-empty list for the light-shadow pass only; it never feeds the pixel shaders.
void VK_Forward_Snapshot();

// True when the forward+ path is enabled at all (env gate, default on once validated).
int VK_Forward_Enabled();

// Graphics Options -> render layer. mode: 0 off, 1 3D clusters, 2 direct 256-light loop. budget: 32..256.
void VK_Forward_SetMode(int mode);
void VK_Forward_SetBudget(int budget);

#else

static inline void VK_Forward_Begin() {}
static inline void VK_Forward_Add_Light(float, float, float, float, float, float, float, float) {}
static inline void VK_Forward_Add_Spot(float, float, float, float, float, float, float, float,
                                       float, float, float, float, float, float, int) {}
static inline int VK_Forward_Active() { return 0; }
static inline int VK_Forward_Enabled() { return 0; }
static inline void VK_Forward_Snapshot() {}
static inline void VK_Forward_SetMode(int) {}
static inline void VK_Forward_SetBudget(int) {}

#endif
