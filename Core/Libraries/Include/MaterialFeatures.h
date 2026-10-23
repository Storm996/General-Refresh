// GeneralsX @refactor 23/10/2026: backend-neutral material vocabulary.
//
// This is the first half of the MaterialDesc contract: every render family classifies the batch it
// is about to draw with the same family/feature bits, and one census logs the real permutations.
// It deliberately lives in WW3D2 (no GlobalData, no game headers) so terrain, GN, tree, grass,
// water and sky can all speak it without breaking the platform-isolation rule.
#pragma once

#include <cstdio>

enum W3DMaterialFamily
{
	W3DMAT_TERRAIN = 0,
	W3DMAT_STATIC,
	W3DMAT_SKIN,
	W3DMAT_FOLIAGE,
	W3DMAT_GRASS,
	W3DMAT_WATER,
	W3DMAT_SKY,
	W3DMAT_UI,
	W3DMAT_COUNT
};

enum W3DMaterialFeature
{
	W3DMATF_ALPHA_TEST     = 1u << 0,
	W3DMATF_ALPHA_BLEND    = 1u << 1,
	W3DMATF_NORMAL_MAP     = 1u << 2,
	W3DMATF_PBR            = 1u << 3,
	W3DMATF_SKIN           = 1u << 4,
	W3DMATF_INSTANCED      = 1u << 5,
	W3DMATF_RECEIVE_FOG    = 1u << 6,
	W3DMATF_RECEIVE_CLOUD  = 1u << 7,
	W3DMATF_RECEIVE_SHADOW = 1u << 8,
	W3DMATF_CAST_SHADOW    = 1u << 9,
	W3DMATF_LOCAL_LIGHTS   = 1u << 10,
	W3DMATF_REFLECTION     = 1u << 11,
	W3DMATF_UNLIT          = 1u << 12,
	W3DMATF_TWO_SIDED      = 1u << 13,
	W3DMATF_LIGHTMAP       = 1u << 14
};

struct W3DMaterialKey
{
	unsigned family;		///< W3DMaterialFamily
	unsigned features;		///< W3DMaterialFeature bitmask
	unsigned fvf;			///< D3D FVF / dynamic layout, 0 if N/A
	unsigned passCount;		///< material passes
	unsigned variant;		///< family-specific shader variant (terrain ST_*, GN pair, ...)
};

// The descriptor is the public name for the key; the alias keeps old call sites source-compatible
// while the code migrates from ad-hoc booleans to one material record.
typedef W3DMaterialKey W3DMaterialDesc;

// Phase 5B.3: the blend class a material promises to the pipeline cache. It is part of the material
// identity, not a second copy of D3DRS_SRCBLEND/DESTBLEND: the executor still derives the exact
// D3D blend factors from it, while the backend can tell two otherwise identical water passes apart
// (sea alpha vs river alpha vs river additive) without inspecting device state after the fact.
enum W3DMaterialBlendMode
{
	W3DMAT_BLEND_OPAQUE = 0,
	W3DMAT_BLEND_ALPHA,
	W3DMAT_BLEND_ADDITIVE,
	W3DMAT_BLEND_DEST_ALPHA
};

// Phase 5B.3: the explicit key the PSO cache is indexed by for families that have completed the
// material migration (water). It is the descriptor plus the blend class and the material pass; a
// water pipeline can therefore never alias onto a different water kind's pipeline just because the
// executor happened to leave the same render state behind. Families that still share their executor's
// state keep using the legacy state-derived key (the third field group documents what is in use).
struct W3DMaterialPipelineKey
{
	unsigned family;		///< W3DMaterialFamily
	unsigned features;		///< W3DMaterialFeature bitmask
	unsigned variant;		///< water kind for W3DMAT_WATER
	unsigned pass;			///< material pass (0 = main, 1.. = shroud/extra)
	unsigned blend;			///< W3DMaterialBlendMode
};

inline unsigned long long W3D_MaterialPipelineHash(const W3DMaterialPipelineKey& key)
{
	// FNV-1a over the five plain fields. The hash is only the cache bucket; equality is decided by
	// field comparison on the stored key, so a collision cannot alias two pipelines together.
	unsigned long long h = 1469598103934665603ull;
	const unsigned fields[5] = { key.family, key.features, key.variant, key.pass, key.blend };
	for (unsigned i = 0; i < 5u; ++i) {
		h ^= (unsigned long long)fields[i];
		h *= 1099511628211ull;
	}
	return h;
}

inline bool W3D_MaterialPipelineEqual(const W3DMaterialPipelineKey& a, const W3DMaterialPipelineKey& b)
{
	return a.family == b.family && a.features == b.features && a.variant == b.variant &&
		a.pass == b.pass && a.blend == b.blend;
}

enum W3DMaterialVertexLayout
{
	W3DMAT_VL_UNKNOWN = 0,
	W3DMAT_VL_NUV1,		///< rigid XYZNUV1
	W3DMAT_VL_NUV2,		///< rigid XYZNUV2
	W3DMAT_VL_SKIN,		///< dynamic XYZNDUV2 (CPU skin)
	W3DMAT_VL_TG3		///< tangent XYZNDUV1TG3 (merged NM/PBR)
};

// One record for the surface properties the GN mount actually needs. Keeping them named here (rather
// than eight positional booleans at the call site) is the seam a full MaterialDesc will replace.
struct W3DMaterialSurface
{
	W3DMaterialVertexLayout layout;
	unsigned passCount;
	bool nmMapperPlain;
	bool usesNormalMap;
	bool sorted;
	bool alphaOverrideIsOpaque;
	bool materialOverride;
	bool oneZeroBlend;
	bool lenvSafe;
};

inline bool W3D_Material_CanUseGeomNormal(const W3DMaterialSurface& surface)
{
	if (!surface.lenvSafe || surface.passCount != 1u || surface.sorted ||
		!surface.alphaOverrideIsOpaque || surface.materialOverride || !surface.oneZeroBlend) {
		return false;
	}
	switch (surface.layout) {
	case W3DMAT_VL_NUV1:
	case W3DMAT_VL_NUV2:
	case W3DMAT_VL_SKIN:
		return true;
	case W3DMAT_VL_TG3:
		return surface.nmMapperPlain && surface.usesNormalMap;
	default:
		return false;
	}
}

inline bool W3D_Material_CanUseGeomNormal(
	W3DMaterialVertexLayout layout,
	bool nmMapperPlain,
	bool usesNormalMap,
	unsigned passCount,
	bool sorted,
	bool alphaOverrideIsOpaque,
	bool materialOverride,
	bool oneZeroBlend,
	bool lenvSafe)
{
	W3DMaterialSurface surface = {};
	surface.layout = layout;
	surface.passCount = passCount;
	surface.nmMapperPlain = nmMapperPlain;
	surface.usesNormalMap = usesNormalMap;
	surface.sorted = sorted;
	surface.alphaOverrideIsOpaque = alphaOverrideIsOpaque;
	surface.materialOverride = materialOverride;
	surface.oneZeroBlend = oneZeroBlend;
	surface.lenvSafe = lenvSafe;
	return W3D_Material_CanUseGeomNormal(surface);
}

inline const char* W3D_MaterialFamilyName(unsigned family)
{
	switch (family)
	{
	case W3DMAT_TERRAIN: return "terrain";
	case W3DMAT_STATIC:  return "static";
	case W3DMAT_SKIN:    return "skin";
	case W3DMAT_FOLIAGE: return "foliage";
	case W3DMAT_GRASS:   return "grass";
	case W3DMAT_WATER:   return "water";
	case W3DMAT_SKY:     return "sky";
	case W3DMAT_UI:      return "ui";
	default:             return "?";
	}
}

// Inline function-local statics are one object per program in C++, so this is a single process-wide
// census even though every family includes the header. Logs only the first occurrence of a key and
// then counts it; no per-draw allocation and no output flood.
inline void W3D_MaterialCensus(const W3DMaterialKey& key)
{
	struct Entry { unsigned long long key; unsigned count; };
	static Entry entries[128] = {};
	static unsigned used = 0;

	const unsigned long long packed =
		((unsigned long long)(key.family & 0xffu)) |
		((unsigned long long)(key.features & 0xffffu) << 8) |
		((unsigned long long)(key.fvf & 0xffffu) << 24) |
		((unsigned long long)(key.passCount & 0xffu) << 40) |
		((unsigned long long)(key.variant & 0xffffu) << 48);

	for (unsigned i = 0; i < used; ++i) {
		if (entries[i].key == packed) {
			++entries[i].count;
			return;
		}
	}
	if (used >= 128) {
		return;
	}
	entries[used].key = packed;
	entries[used].count = 1;
	++used;
	fprintf(stderr,
		"[MATCENSUS] family=%-7s features=0x%04X fvf=0x%08X passes=%u variant=%u count=1\n",
		W3D_MaterialFamilyName(key.family), key.features, key.fvf, key.passCount, key.variant);
	fflush(stderr);
}
