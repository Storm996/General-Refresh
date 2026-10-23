// GeneralsX @feature Qoder 20/10/2026 Forward+: per-frame point-light feed from the game layer.
//
// The renderer used to let each drawable's LightEnvironment choose its own top-8 lights. That is a
// per-object cluster with a hard cap: a base with more than eight lamps loses lights, and a point
// light used to disqualify a batch from the per-pixel path entirely. Forward+ removes both limits:
// the scene publishes every active point light once per frame, the draw layer builds a coarse screen
// tile list on the CPU, and the GN pixel shaders iterate only the lights that overlap their tile.
//
// The API is intentionally tiny and has no-op fallbacks so callers do not need preprocessor guards:
// VK_Forward_Begin() once before the scene draw, one VK_Forward_Add_Light() per point light, and the
// draw layer consumes the list at the first scene submit. VK_Forward_Active() reports whether the
// feed has data for this frame; DX8GeomNormal uses it to pick the tile loop over the per-object list.
#pragma once

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

// Start a new frame's light list. Called from RTS3DScene::doRender before DRAW().
void VK_Forward_Begin();

// Add a point light in world space. 'intensity' is folded by the caller; inner/outer are the far
// attenuation radii in world units (inner >= outer or outer <= 0 means no finite falloff).
void VK_Forward_Add_Light(float x, float y, float z, float inner, float outer,
                          float r, float g, float b);

// True when this frame's list has at least one light and the tile block was built.
int VK_Forward_Active();

// True when the forward+ path is enabled at all (env gate, default on once validated).
int VK_Forward_Enabled();

// Graphics Options -> render layer. mode: 0 off, 1 2D tiles, 2 3D clusters. budget: 32..256.
void VK_Forward_SetMode(int mode);
void VK_Forward_SetBudget(int budget);

#else

static inline void VK_Forward_Begin() {}
static inline void VK_Forward_Add_Light(float, float, float, float, float, float, float, float) {}
static inline int VK_Forward_Active() { return 0; }
static inline int VK_Forward_Enabled() { return 0; }
static inline void VK_Forward_SetMode(int) {}
static inline void VK_Forward_SetBudget(int) {}

#endif
