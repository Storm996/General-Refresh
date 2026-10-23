// GeneralsX @refactor 23/10/2026: shared material-instance record for the families that do not use
// the terrain/model binders yet (water, grass, sky). It carries the same descriptor/census contract;
// the legacy shader path remains the executor until those families get a full binder.
#pragma once

#include "MaterialFeatures.h"

class TextureClass;

struct W3DMaterialInstance
{
	W3DMaterialDesc desc;
	unsigned shaderVariant;
	unsigned blendMode;		///< W3DMaterialBlendMode; water migrates its blend class into this field
	TextureClass *textures[8];
	void *nativeTextures[8];	///< raw IDirect3DTexture9* for procedural/owned textures (water bump)

	W3DMaterialInstance()
		: shaderVariant(0u), blendMode(W3DMAT_BLEND_OPAQUE)
	{
		for (unsigned i = 0; i < 8; ++i) {
			textures[i] = nullptr;
			nativeTextures[i] = nullptr;
		}
	}

	// Bind one slot through the facade (so Vulkan's texture mirror sees it). Count is bounded by the
	// legacy 8-stage limit; families not yet fully migrated can keep their direct binds elsewhere.
	void BindTextureSlot(unsigned slot) const;
	void BindNativeTextureSlot(unsigned slot) const;
	void BindAllTextures() const;
	/// The material key published to the backend. Only water uses it as pipeline identity today; the
	/// other families still share the executor's state-derived cache key (see W3DMaterialBinder::Bind).
	W3DMaterialPipelineKey MakePipelineKey(unsigned pass) const;
	/// Unified material entry point: texture set now, pipeline/variant later. Water, sky and grass
	/// all go through this instead of each family poking DX8Wrapper::Set_Texture directly.
	void Bind() const { BindAllTextures(); }
};

// Phase 5A: one material entry point for every family. It binds the texture set, publishes the
// material key to the backend (family/features/variant/pass), and applies the shared shadow flags.
// The actual executor (DX8Renderer, terrain PS30, TREE30, vegetation, water, sky) is still the legacy
// path until Phase 5B replaces it; this is the seam that lets those executors disappear one at a time.
class W3DMaterialBinder
{
public:
	static void Bind(const W3DMaterialInstance& instance, unsigned pass = 0u);
};
