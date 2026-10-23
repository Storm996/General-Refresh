// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.3: single choke point that re-locks every mip
// level of a D3D texture READONLY and mirrors its final texels into the keyed Vulkan image.
//
// Before this, only the disk loader (textureloader.cpp Unlock_Surfaces) reached the Vulkan mirror.
// Procedural writers in the game-device layer - the terrain blend/flat atlas (TerrainTex), the
// water bump maps (W3DWater), decals (W3DSmudge) and the tree atlas (W3DTreeBuffer) - build their
// textures in memory and hand them straight to D3D, so the Vulkan side never saw their bytes. This
// helper gives every CPU-written texture the same convergence the loader uses, keyed by the D3D
// texture pointer so a repeat update re-uploads into the existing VkImage instead of leaking one.
//
// The declaration is intentionally UNCONDITIONAL (no SAGE_USE_VULKAN_CORE guard): the callers live in
// z_gameenginedevice, which is NOT compiled with the Vulkan-backend define, so a guarded symbol would
// silently drop the call (this is exactly what the first 3.3 build hit - 0 dynamic mirrors at runtime).
// The function always exists and always links; on non-Vulkan builds its body is a cheap `return false`.
#ifndef GENERALSX_VULKANTEXMIRROR_H
#define GENERALSX_VULKANTEXMIRROR_H

// d3d_texture is an IDirect3DTexture9* (from TextureClass::Peek_D3D_Texture() or a raw
// DX8Wrapper::_Create_DX8_Texture result). Returns false (no-op) when the Vulkan mirror is off.
bool VKTex_Mirror_D3D_Texture(void* d3d_texture);

// Same convergence, but the texels are read from a second texture (the D3D9 staging copy) and
// uploaded under 'key'. D3DPOOL_DEFAULT destinations cannot be re-locked, so the rect-copy path
// hands over its staging texture instead of the destination pointer.
bool VKTex_Mirror_Texture_As(void* key, void* source_texture, unsigned levels);

// GeneralsX @bugfix Qoder 21/10/2026 SHADOWRT: copy a live render-target mirror into a texture
// mirror on the GPU. The projected-shadow bake reads the shared shadow render target back through
// IDirect3DDevice9::GetRenderTargetData, whose pure-Vulkan stub deliberately returns CPU zeros (the
// RT content lives in the VkImage), so every baked shadow texture mirrored black and the decals
// multiplied the terrain to a fixed black square. CPU staging cannot be used here because the bake
// is already recorded in the VkImage and a locked surface would only see the empty stub store.
bool VKTex_Copy_RT_To_Texture(void* dst_texture, void* src_render_target,
                              unsigned width, unsigned height);


#endif // GENERALSX_VULKANTEXMIRROR_H
