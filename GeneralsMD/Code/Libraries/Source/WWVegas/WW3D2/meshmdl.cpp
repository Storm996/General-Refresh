/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/***********************************************************************************************
 ***              C O N F I D E N T I A L  ---  W E S T W O O D  S T U D I O S               ***
 ***********************************************************************************************
 *                                                                                             *
 *                 Project Name : WW3D                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/meshmdl.cpp                            $*
 *                                                                                             *
 *                    Org Author:: Greg Hjelstrom                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/26/02 4:04p                                             $*
 *                                                                                             *
 *                    $Revision:: 48                                                          $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "meshmdl.h"
#include "matinfo.h"
#include "aabtree.h"
#include "htree.h"
#include "WWMath/vp.h"
#include "visrasterizer.h"
#include "dx8polygonrenderer.h"
#include "bwrender.h"
#include "camera.h"
#include "dx8renderer.h"
#include "sunsink.h"	// GeneralsX @feature Qoder 13/10/2026 PS3.0 T6 sun shadow map sink interface
#include "WWLib/hashtemplate.h"
// GeneralsX @feature Qoder 10/10/2026 T3-0 M2 (段D): companion normal-map detection needs the
// WW3D file factory (existence test), the DX8 caps (Support_Dot3), texture names and the
// tangent-space generator.
#include "texture.h"
#include "dx8caps.h"
#include "dx8wrapper.h"
#include "WWLib/ffactory.h"
#include "WWLib/WWFILE.h"
#include "meshtangent.h"
#include <cstdio>
#include <cstring>


/*
** Temporary Buffers
** These buffers are used by the skin code for temporary storage of the deformed vertices and
** vertex normals.
*/
static DynamicVectorClass<Vector3>	_TempVertexBuffer;
static DynamicVectorClass<Vector3>	_TempNormalBuffer;
static DynamicVectorClass<Vector4>	_TempTransformedVertexBuffer;
static DynamicVectorClass<unsigned long> _TempClipFlagBuffer;


/*
**
** MeshModelClass Implementation
**
*/


MeshModelClass::MeshModelClass() :
	DefMatDesc(nullptr),
	AlternateMatDesc(nullptr),
	CurMatDesc(nullptr),
	MatInfo(nullptr),
	GapFiller(nullptr),
	UsesNormalMap(false),
	VertexTangent(nullptr),
	VertexBinormal(nullptr)
{
	Set_Flag(DIRTY_BOUNDS,true);

	DefMatDesc = W3DNEW MeshMatDescClass;
	CurMatDesc = DefMatDesc;

	MatInfo = NEW_REF( MaterialInfoClass, () );
}

MeshModelClass::MeshModelClass(const MeshModelClass & that) :
	MeshGeometryClass(that),
	DefMatDesc(nullptr),
	AlternateMatDesc(nullptr),
	CurMatDesc(nullptr),
	MatInfo(nullptr),
	GapFiller(nullptr),
	HasBeenInUse(false),
	UsesNormalMap(false),
	VertexTangent(nullptr),
	VertexBinormal(nullptr)
{
	DefMatDesc = W3DNEW MeshMatDescClass(*(that.DefMatDesc));
	if (that.AlternateMatDesc != nullptr) {
		AlternateMatDesc = W3DNEW MeshMatDescClass(*(that.AlternateMatDesc));
	}
	CurMatDesc = DefMatDesc;

	clone_materials(that);

	// GeneralsX @feature Qoder 10/10/2026 T3-0 M2 (段D): replicate tangent-space basis if present.
	Copy_Normal_Map_Data(that);
}

MeshModelClass::~MeshModelClass()
{

	Reset(0,0,0);
	REF_PTR_RELEASE(MatInfo);

	delete DefMatDesc;
	delete AlternateMatDesc;

}

MeshModelClass & MeshModelClass::operator = (const MeshModelClass & that)
{
	if (this != &that) {
		// Remove all polygon renderers, this will remove the mesh from the rendering system.
		// The mesh will be initialized to rendering system the next time it is rendered.
		TheDX8MeshRenderer.Unregister_Mesh_Type(this);

		MeshGeometryClass::operator = (that);

		*DefMatDesc = *(that.DefMatDesc);
		CurMatDesc = DefMatDesc;

		delete AlternateMatDesc;
		AlternateMatDesc = nullptr;

		if (that.AlternateMatDesc != nullptr) {
			AlternateMatDesc = W3DNEW MeshMatDescClass(*(that.AlternateMatDesc));
		}

		clone_materials(that);

		// DMS - using appropriate deallocation method
		delete GapFiller;
		GapFiller=nullptr;

		if (that.GapFiller)
			GapFiller=W3DNEW GapFillerClass(*that.GapFiller);

		// GeneralsX @feature Qoder 10/10/2026 T3-0 M2 (段D): replicate tangent-space basis if present.
		Copy_Normal_Map_Data(that);
	}
	return * this;
}

void MeshModelClass::Reset(int polycount,int vertcount,int passcount)
{
	//DMS - We must delete the gapfiller object BEFORE the geometry is reset.  Otherwise,
	// the number of stages and passes gets reset and the gapfiller cannot deallocate properly.
	delete GapFiller;
	GapFiller=nullptr;

	// GeneralsX @feature Qoder 10/10/2026 T3-0 M2 (段D): drop any tangent-space basis with the geometry.
	delete[] VertexTangent;
	VertexTangent=nullptr;
	delete[] VertexBinormal;
	VertexBinormal=nullptr;
	UsesNormalMap=false;

	Reset_Geometry(polycount,vertcount);

	// Release everything we have and reset to initial state

	TheDX8MeshRenderer.Unregister_Mesh_Type(this);

	MatInfo->Reset();
	DefMatDesc->Reset(polycount,vertcount,passcount);

	delete AlternateMatDesc;
	AlternateMatDesc = nullptr;

	CurMatDesc = DefMatDesc;
}

// GeneralsX @feature Qoder 10/10/2026 T3-0 M2 (段D)
// Deep-copy the tangent-space basis from a source model into this one, matching the current
// vertex count. If the source has no normal map, this model is cleared to the fixed-pipeline
// state. Never touches GameLogic.
void MeshModelClass::Copy_Normal_Map_Data(const MeshModelClass & srcmesh)
{
	delete[] VertexTangent;
	VertexTangent = nullptr;
	delete[] VertexBinormal;
	VertexBinormal = nullptr;
	UsesNormalMap = false;

	if (!srcmesh.UsesNormalMap || !srcmesh.VertexTangent || !srcmesh.VertexBinormal) {
		return;
	}

	const int vertCount = Get_Vertex_Count();
	const int srcVerts = srcmesh.Get_Vertex_Count();
	if (vertCount <= 0 || vertCount != srcVerts) {
		// Vertex counts must match for a shared-geometry clone; otherwise drop the basis.
		return;
	}

	VertexTangent = W3DNEWARRAY Vector3[vertCount];
	VertexBinormal = W3DNEWARRAY Vector3[vertCount];
	if (!VertexTangent || !VertexBinormal) {
		delete[] VertexTangent;
		delete[] VertexBinormal;
		VertexTangent = nullptr;
		VertexBinormal = nullptr;
		return;
	}

	memcpy(VertexTangent, srcmesh.VertexTangent, vertCount * sizeof(Vector3));
	memcpy(VertexBinormal, srcmesh.VertexBinormal, vertCount * sizeof(Vector3));
	UsesNormalMap = true;
}

// GeneralsX @feature Qoder 10/10/2026 T3-0 P3: strip a state-variant suffix
// (_d/_e/_s/_g/_r/_n and combinations like _ds/_dng with an optional digit)
// from a texture base name, so a single base normal map serves all
// damage/garrison/snow/night variants that share the base UV layout.
static void Strip_State_Variant_Suffix(char* name)
{
	char* us = strrchr(name, '_');
	if (us == nullptr) {
		return;
	}
	char* tok = us + 1;
	int tok_len = (int)strlen(tok);
	while (tok_len > 0 && tok[tok_len - 1] >= '0' && tok[tok_len - 1] <= '9') {
		tok_len--;
	}
	if (tok_len == 0 || tok_len > 3) {
		return;
	}
	for (int i = 0; i < tok_len; ++i) {
		const char c = tok[i];
		if (c != 'd' && c != 'e' && c != 'g' && c != 'n' && c != 'r' && c != 's') {
			return;
		}
	}
	*us = 0;
}

// GeneralsX @bugfix Qoder 16/10/2026 #87: read the short side of a TGA or DDS from its file header
// only - no pixel decode, no texture init - so the load probe can enforce the surface detail
// contract before any mount flag exists. Returns false when the format is not recognised, which
// the caller must treat as "dimension unknown", never as "reject".
static bool Peek_Image_Min_Side(const char* filename, int& minSide)
{
	minSide = 0;
	if (filename == nullptr || filename[0] == 0) {
		return false;
	}
	FileClass* file = _TheFileFactory->Get_File(filename);
	if (file == nullptr) {
		return false;
	}
	bool ok = false;
	file->Open();
	if (file->Is_Available()) {
		unsigned char hdr[20];
		if (file->Read(hdr, 20) == 20) {
			if (hdr[0] == 'D' && hdr[1] == 'D' && hdr[2] == 'S' && hdr[3] == ' ') {
				// DDS_HEADER: dwHeight at 12, dwWidth at 16, little-endian 32-bit.
				const int h = hdr[12] | (hdr[13] << 8) | (hdr[14] << 16) | (hdr[15] << 24);
				const int w = hdr[16] | (hdr[17] << 8) | (hdr[18] << 16) | (hdr[19] << 24);
				if (w > 0 && h > 0) {
					minSide = (w < h) ? w : h;
					ok = true;
				}
			} else {
				// TGA: fixed 18-byte header, width at 12, height at 14, little-endian 16-bit.
				const int w = hdr[12] | (hdr[13] << 8);
				const int h = hdr[14] | (hdr[15] << 8);
				if (w > 0 && h > 0) {
					minSide = (w < h) ? w : h;
					ok = true;
				}
			}
		}
	}
	file->Close();
	_TheFileFactory->Return_File(file);
	return ok;
}

// GeneralsX @feature Qoder 10/10/2026 T3-0 M2 (段D)
// Look for a "_nm" companion for the pass-0 / stage-0 diffuse texture(s). Naming rule is strict:
// only "_nm" and "_nm_ai" are recognised (NOT "_n", which Zero Hour already uses for its 124
// night maps). When a companion exists and the device supports dot3, build the per-vertex tangent
// basis and flag the model so Define_FVF upgrades it to the TG3 vertex format.
void MeshModelClass::Detect_And_Build_Normal_Map()
{
	// Clear any prior state (Reset already did this at load time, but be defensive).
	delete[] VertexTangent;
	VertexTangent = nullptr;
	delete[] VertexBinormal;
	VertexBinormal = nullptr;
	UsesNormalMap = false;

	// GeneralsX @feature T3-0 P1: skinned meshes are no longer excluded. Their tangent basis is
	// built in model space here; at render time the skin deformation loop transforms the basis
	// with the same bone matrices as the normals (see DX8SkinFVFCategoryContainer::Render).
	const DX8Caps* caps = DX8Wrapper::Get_Current_Caps();
	if (!caps || !caps->Support_Dot3()) {
		return;
	}

	if (Get_Pass_Count() < 1) {
		return;
	}

	// Collect a representative stage-0 diffuse name from pass 0 (single or first non-null array entry).
	StringClass diffuse_name;
	TextureClass* single = Peek_Single_Texture(0, 0);
	if (single != nullptr) {
		diffuse_name = single->Get_Texture_Name();
	} else if (Has_Texture_Array(0, 0)) {
		const int polyCount = Get_Polygon_Count();
		for (int p = 0; p < polyCount; ++p) {
			TextureClass* t = Peek_Texture(p, 0, 0);
			if (t != nullptr) {
				diffuse_name = t->Get_Texture_Name();
				break;
			}
		}
	}

	if (diffuse_name.Is_Empty()) {
		return;
	}

	// Strip the extension to obtain the base name.
	const char* raw = (const char*)diffuse_name;
	char base[256];
	int i = 0;
	for (; raw[i] != 0 && i < 255; ++i) {
		base[i] = raw[i];
	}
	base[i] = 0;
	char* dot = strrchr(base, '.');
	if (dot != nullptr) {
		*dot = 0;
	}

	// Guard against re-probing a texture that is itself already a normal map.
	const int baseLen = (int)strlen(base);
	if (baseLen >= 3 && strcmp(base + baseLen - 3, "_nm") == 0) {
		return;
	}
	if (baseLen >= 6 && strcmp(base + baseLen - 6, "_nm_ai") == 0) {
		return;
	}

	static const char* const kSuffixes[] = { "_nm_ai", "_nm" };
	// GeneralsX @tweak Qoder 11/10/2026 T3-0 P3: prefer .dds first - the offline
	// batch bakes a RENORMALIZED full mip chain into the DDS, while the bare TGA
	// makes the engine build mips at runtime without renormalization (causes
	// brightness popping / shimmer on far buildings).
	static const char* const kExtensions[] = { ".dds", ".tga" };

	// GeneralsX @feature Qoder 10/10/2026 T3-0 P3: when the literal diffuse name has
	// no companion, retry with state-variant suffixes stripped (xxx_d -> xxx), so one
	// base normal map serves all damage/garrison/snow/night states of a model.
	const char* candidates[2];
	int num_candidates = 0;
	candidates[num_candidates++] = base;
	char base_stripped[256];
	strcpy(base_stripped, base);
	Strip_State_Variant_Suffix(base_stripped);
	if (strcmp(base_stripped, base) != 0) {
		candidates[num_candidates++] = base_stripped;
	}

	bool companion_found = false;
	char companion[300];
	char tried[768];
	tried[0] = 0;
	for (int ci = 0; ci < num_candidates && !companion_found; ++ci) {
		for (int s = 0; s < 2 && !companion_found; ++s) {
			for (int e = 0; e < 2 && !companion_found; ++e) {
				snprintf(companion, sizeof(companion), "%s%s%s", candidates[ci], kSuffixes[s], kExtensions[e]);
				if (tried[0] != 0) strcat(tried, ",");
				strcat(tried, companion);

				FileClass* file = _TheFileFactory->Get_File(companion);
				if (file != nullptr) {
					file->Open();
					companion_found = file->Is_Available();
					file->Close();
					_TheFileFactory->Return_File(file);
				}

				if (companion_found) {
					fprintf(stderr, "[NM_MESH] mesh='%s' diffuse='%s' companion='%s'\n", Get_Name(), (const char*)diffuse_name, companion);
					fflush(stderr);
				}
			}
		}
	}

	if (!companion_found) {
		// GeneralsX @debug Qoder 11/10/2026 T3-0 P3: silent misses hid the mount gap.
		// Log every loaded mesh whose diffuse has no _nm companion so the P3 asset
		// audit can tell "model never loaded" (no line at all) from "companion missing".
		fprintf(stderr, "[NM_MISS] mesh='%s' diffuse='%s' tried=%s\n", Get_Name(), (const char*)diffuse_name, tried);
		fflush(stderr);
		return;
	}

	// GeneralsX @bugfix Qoder 16/10/2026 #87: SURFACE DETAIL CONTRACT.
	// The full-library companion rollout gave every diffuse stem an "_nm"/"_rp" file, so
	// "companion exists" no longer says a mesh is a lit SURFACE - it only says a file was
	// dropped. Baked sprite parts (bayonet sparkles, glow backpacks, flag cloth, 32-64px
	// soldier skins, 16px FX) therefore mounted dot3/PBR and got re-lit by the injected sun
	// constants on top of their baked shading - the floating colored patches seen between
	// facing infantry and against the sky whenever infantry spawn. Two asset-intrinsic tests
	// replace file existence as the mount authority:
	//   1) diffuse AND companion must be >= 128px on the short side: at battlefield zoom a
	//      normal/RP map below that cannot resolve as surface detail - it is noise by
	//      construction;
	//   2) the mesh must have >= 48 polygons: anything built from a handful of quads is a
	//      sprite/overlay/decal, not a surface, and tangent-space re-lighting there replaces
	//      art-authored shading (unit bodies are 900+ polys, the offending parts 8-35).
	// Class-level by construction: no per-file moves, and both feature arms stay fully on.
	{
		const int polys = Get_Polygon_Count();
		int nmSide = 0;
		int diffuseSide = 0;
		const bool nmKnown = Peek_Image_Min_Side(companion, nmSide);
		const bool diffuseKnown = Peek_Image_Min_Side((const char*)diffuse_name, diffuseSide);
		if ((nmKnown && nmSide < 128) || (diffuseKnown && diffuseSide < 128) || polys < 48) {
			fprintf(stderr, "[NM_MESH] mesh='%s' diffuse='%s' companion='%s' REJECT contract nmSide=%d diffuseSide=%d polys=%d\n",
				Get_Name(), (const char*)diffuse_name, companion, nmSide, diffuseSide, polys);
			fflush(stderr);
			return;
		}
	}

	if (Compute_Mesh_Tangents(this, &VertexTangent, &VertexBinormal)) {
		UsesNormalMap = true;
		fprintf(stderr, "[NM_MESH] mesh='%s' tangent basis built for %d vertices (TG3 vertex format requested)\n",
			Get_Name(), Get_Vertex_Count());
		fflush(stderr);
	}
}

// GeneralsX @feature T3-0 P1: skin deformation for the tangent-space basis. Mirrors the batch
// loop of MeshGeometryClass::get_deformed_vertices (vertices are pre-sorted by bone index) and
// additionally transforms tangent/binormal with the translation-free bone matrix, exactly like
// the normals, so the basis follows the skeleton at render time.
void MeshModelClass::Get_Deformed_TBN(Vector3* dst_vert, Vector3* dst_norm, Vector3* dst_tan, Vector3* dst_bin, const HTreeClass* htree)
{
	int vertex_count = Get_Vertex_Count();
	Vector3* src_vert = Vertex->Get_Array();
	Vector3* src_norm = VertexNorm->Get_Array();
	uint16* bonelink = VertexBoneLink->Get_Array();

	for (int vi = 0; vi < vertex_count;) {
		const Matrix3D& tm = htree->Get_Transform(bonelink[vi]);
		Matrix3D mytm = tm;

		int idx = bonelink[vi];
		int cnt;
		for (cnt = vi; cnt < vertex_count; cnt++) {
			if (idx != bonelink[cnt]) {
				break;
			}
		}

		VectorProcessorClass::Transform(dst_vert + vi, src_vert + vi, mytm, cnt - vi);
		mytm.Set_Translation(Vector3(0.0f, 0.0f, 0.0f));
		VectorProcessorClass::Transform(dst_norm + vi, src_norm + vi, mytm, cnt - vi);
		if (dst_tan && dst_bin && VertexTangent && VertexBinormal) {
			VectorProcessorClass::Transform(dst_tan + vi, VertexTangent + vi, mytm, cnt - vi);
			VectorProcessorClass::Transform(dst_bin + vi, VertexBinormal + vi, mytm, cnt - vi);
		}
		vi = cnt;
	}
}

void MeshModelClass::Register_For_Rendering()
{
	HasBeenInUse=true;
//WW3D::Set_NPatches_Level(1);
	if (WW3D::Get_NPatches_Level()>1) {
		if (WW3D::Get_NPatches_Gap_Filling_Mode()!=WW3D::NPATCHES_GAP_FILLING_DISABLED) {
			Init_For_NPatch_Rendering();
		}
		else {
			delete GapFiller;
			GapFiller=nullptr;
		}
	}
	else {
		if (WW3D::Get_NPatches_Gap_Filling_Mode()==WW3D::NPATCHES_GAP_FILLING_FORCE) {
			Init_For_NPatch_Rendering();
		}
		else {
			delete GapFiller;
			GapFiller=nullptr;
		}
	}

	TheDX8MeshRenderer.Register_Mesh_Type(this);
}

void MeshModelClass::Replace_Texture(TextureClass* texture,TextureClass* new_texture)
{
	WWASSERT(texture);
	WWASSERT(new_texture);
	for (int stage=0;stage<MeshMatDescClass::MAX_TEX_STAGES;++stage) {
		for (int pass=0;pass<Get_Pass_Count();++pass) {
			if (Has_Texture_Array(pass,stage)) {
				for (int i=0;i<Get_Polygon_Count();++i) {
					if (Peek_Texture(i,pass,stage)==texture) {
						Set_Texture(i,new_texture,pass,stage);
					}
				}
			}
			else {
				if (Peek_Single_Texture(pass,stage)==texture) {
					Set_Single_Texture(new_texture,pass,stage);
				}
			}
			// If this mesh model has been initialized for rendering we need to tell the rendering
			// system to change texturing as well.
			DX8FVFCategoryContainer* fvf_category=Peek_FVF_Category_Container();
			if (fvf_category) {
				fvf_category->Change_Polygon_Renderer_Texture(PolygonRendererList,texture,new_texture,pass,stage);
			}
		}
	}
}

void MeshModelClass::Replace_VertexMaterial(VertexMaterialClass* vmat,VertexMaterialClass* new_vmat)
{
	WWASSERT(vmat);
	WWASSERT(new_vmat);

	for (int pass=0;pass<Get_Pass_Count();++pass) {
		if (Has_Material_Array(pass)) {
			for (int i=0;i<Get_Vertex_Count();++i) {
				if (Peek_Material(i,pass)==vmat) {
					Set_Material(i,new_vmat,pass);
				}
			}
		}
		else {
			if (Peek_Single_Material(pass)==vmat) {
				Set_Single_Material(new_vmat,pass);
			}
		}
		// If this mesh model has been initialized for rendering we need to tell the rendering
		// system to change texturing as well.
		DX8FVFCategoryContainer* fvf_category=Peek_FVF_Category_Container();
		if (fvf_category) {
			fvf_category->Change_Polygon_Renderer_Material(PolygonRendererList,vmat,new_vmat,pass);
		}
	}
}

DX8FVFCategoryContainer* MeshModelClass::Peek_FVF_Category_Container()
{
	if (PolygonRendererList.Is_Empty()) return nullptr;
	DX8PolygonRendererClass* polygon_renderer=PolygonRendererList.Get_Head();
	WWASSERT(polygon_renderer);
	DX8TextureCategoryClass* texture_category=polygon_renderer->Get_Texture_Category();
	WWASSERT(texture_category);
	DX8FVFCategoryContainer* fvf_category=texture_category->Get_Container();
	WWASSERT(fvf_category);
	return fvf_category;
}

void MeshModelClass::Shadow_Render(SpecialRenderInfoClass & rinfo,const Matrix3D & tm,const HTreeClass * htree)
{
	// GeneralsX @feature Qoder 13/10/2026 PS3.0 T6 sun shadow map: the occluder pass reuses this
	// same Special_Render plumbing that the (dead) BWRenderer atlas path used. The helper outputs
	// GL-convention clip space (z in [-1,1], ortho so w==1); the sink rebases to D3D [0,1] itself.
	if (rinfo.SunSink != nullptr) {
		if (_TempTransformedVertexBuffer.Length() < VertexCount) _TempTransformedVertexBuffer.Resize(VertexCount);
		Vector4* transf_ptr=&(_TempTransformedVertexBuffer[0]);
		get_deformed_screenspace_vertices(transf_ptr,rinfo,tm,htree);

		rinfo.SunSink->Submit_Screenspace_Triangles(transf_ptr,VertexCount,
			reinterpret_cast<const unsigned short*>(Poly->Get_Array()),PolyCount);
		return;
	}

	if (rinfo.BWRenderer != nullptr) {
		if (_TempTransformedVertexBuffer.Length() < VertexCount) _TempTransformedVertexBuffer.Resize(VertexCount);
		Vector4* transf_ptr=&(_TempTransformedVertexBuffer[0]);
		get_deformed_screenspace_vertices(transf_ptr,rinfo,tm,htree);

		Vector2* tptr = reinterpret_cast<Vector2 *>(transf_ptr);
		Vector4* optr = transf_ptr;
		for (int a=0;a<VertexCount;++a,++optr) *tptr++=Vector2((*optr)[0],-(*optr)[1]);

		rinfo.BWRenderer->Set_Vertex_Locations(reinterpret_cast<Vector2*>(transf_ptr),VertexCount);
		rinfo.BWRenderer->Render_Triangles(reinterpret_cast<const unsigned long*>(Poly->Get_Array()),PolyCount*3);
		return;
	}
}

void MeshModelClass::Make_Geometry_Unique()
{
	WWASSERT(Vertex);

	ShareBufferClass<Vector3> * unique_verts = NEW_REF(ShareBufferClass<Vector3>,(*Vertex));
	REF_PTR_SET(Vertex,unique_verts);
	REF_PTR_RELEASE(unique_verts);

	ShareBufferClass<Vector3> * norms = NEW_REF(ShareBufferClass<Vector3>,(*VertexNorm));
	REF_PTR_SET(VertexNorm,norms);
	REF_PTR_RELEASE(norms);

#if (!OPTIMIZE_PLANEEQ_RAM)
	ShareBufferClass<Vector4> * peq = NEW_REF(ShareBufferClass<Vector4>,(*PlaneEq, "MeshModelClass::PlaneEq"));
	REF_PTR_SET(PlaneEq,peq);
	REF_PTR_RELEASE(peq);
#endif
}

void MeshModelClass::Make_UV_Array_Unique(int pass,int stage)
{
	CurMatDesc->Make_UV_Array_Unique(pass,stage);
}

void MeshModelClass::Make_Color_Array_Unique(int array_index)
{
	CurMatDesc->Make_Color_Array_Unique(array_index);
}

void MeshModelClass::Enable_Alternate_Material_Description(bool onoff)
{
	if ((onoff == true) && (AlternateMatDesc != nullptr)) {
		if (CurMatDesc != AlternateMatDesc) {
			CurMatDesc = AlternateMatDesc;

			if (Get_Flag(SORT) && WW3D::Is_Munge_Sort_On_Load_Enabled())
				compute_static_sort_levels();

			if (WW3D::Is_Overbright_Modify_On_Load_Enabled())
				modify_for_overbright();

			// TODO: Invalidate just this meshes DX8 data!!!
			TheDX8MeshRenderer.Invalidate();
		}
	} else {
		if (CurMatDesc != DefMatDesc) {
			CurMatDesc = DefMatDesc;

			if (Get_Flag(SORT) && WW3D::Is_Munge_Sort_On_Load_Enabled())
				compute_static_sort_levels();

			if (WW3D::Is_Overbright_Modify_On_Load_Enabled())
				modify_for_overbright();

			// TODO: Invalidate this meshes DX8 data!!!
			TheDX8MeshRenderer.Invalidate();
		}
	}
}

bool MeshModelClass::Is_Alternate_Material_Description_Enabled()
{
	return CurMatDesc == AlternateMatDesc;
}
/*
void MeshModelClass::Process_Texture_Reduction()
{
	MatInfo->Process_Texture_Reduction();
}
*/
bool MeshModelClass::Needs_Vertex_Normals()
{
	if (Get_Flag(MeshModelClass::PRELIT_MASK) == 0) {
		return true;
	}
	return CurMatDesc->Do_Mappers_Need_Normals();
}

struct TriangleSide
{
	Vector3 loc1;
	Vector3 loc2;
	TriangleSide(const Vector3& l1,const Vector3& l2)
	{
		int i1=*(int*)&l1[0];
		i1=37*i1+*(int*)&l1[1];
		i1=37*i1+*(int*)&l1[2];
		int i2=*(int*)&l2[0];
		i2=37*i2+*(int*)&l2[1];
		i2=37*i2+*(int*)&l2[2];
		if (i1<i2) {
			loc1=l1;
			loc2=l2;
		}
		else {
			loc2=l1;
			loc1=l2;
		}
	}
	TriangleSide() {}

	bool operator== (const TriangleSide& s)
	{
		unsigned i=*(unsigned*)&loc1[0]^*(unsigned*)&s.loc1[0];
		i|=*(unsigned*)&loc1[1]^*(unsigned*)&s.loc1[1];
		i|=*(unsigned*)&loc1[2]^*(unsigned*)&s.loc1[2];
		i|=*(unsigned*)&loc2[0]^*(unsigned*)&s.loc2[0];
		i|=*(unsigned*)&loc2[1]^*(unsigned*)&s.loc2[1];
		i|=*(unsigned*)&loc2[2]^*(unsigned*)&s.loc2[2];
		return !i;
	}
};

// Get_Hash_Value specialization for Vector3.

template <> inline unsigned int HashTemplateKeyClass<Vector3>::Get_Hash_Value(const Vector3& location)
{
	const unsigned char* buffer=(const unsigned char*)&location;
	unsigned int hval=0;
	for (unsigned int a=0;a<sizeof(Vector3);++a) {
		hval+=37*hval+buffer[a];
	}
	return hval;
}

// Get_Hash_Value specialization for TriangleSide.

template <> inline unsigned int HashTemplateKeyClass<TriangleSide>::Get_Hash_Value(const TriangleSide& side)
{
	const unsigned char* buffer=(const unsigned char*)&side;
	unsigned int hval=0;
	for (unsigned int a=0;a<sizeof(TriangleSide);++a) {
		hval+=37*hval+buffer[a];
	}
	return hval;
}

struct SideIndexInfo
{
	unsigned short vidx1;
	unsigned short vidx2;
	unsigned polygon_index;
	SideIndexInfo() {}
	SideIndexInfo(int i) { WWASSERT(0); }
};


HashTemplateClass<Vector3, unsigned> LocationHash;
HashTemplateClass<Vector3, unsigned> DuplicateLocationHash;
HashTemplateClass<TriangleSide,SideIndexInfo> SideHash;

// ----------------------------------------------------------------------------
//
// Allocate a gap-filler object. The constructor allocates memory for the
// maximum possible amount of gap polygons, which is quite much. After all
// the gap polygons have been added to the arrays, Shrink_Arrays() should
// be called to free up all unneeded memory.
//
// ----------------------------------------------------------------------------

GapFillerClass::GapFillerClass(MeshModelClass* mmc_) : mmc(nullptr), PolygonCount(0)
{
	//DMS - We cannot take a reference to the mesh model here!  This is because the mesh model
	// class OWNS the GapFiller class (allocated via NEW).  If we take a reference here, there
	// will be an extra reference on the parent object, which will result in the parent object
	// not being destroyed.
	//
//	REF_PTR_SET(mmc,mmc_);
	mmc = mmc_;

	ArraySize=mmc->Get_Polygon_Count()*6;	// Each side of each triangle can have 2 polygons added, in the worst case
	PolygonArray=W3DNEWARRAY TriIndex[ArraySize];
	for (int pass=0;pass<mmc->Get_Pass_Count();++pass) {
		for (int stage=0;stage<MeshMatDescClass::MAX_TEX_STAGES;++stage) {
			if (mmc->Has_Texture_Array(pass,stage)) {
				TextureArray[pass][stage]=W3DNEWARRAY TextureClass*[ArraySize];
			}
			else TextureArray[pass][stage]=nullptr;
		}

		if (mmc->Has_Material_Array(pass)) {
			MaterialArray[pass]=W3DNEWARRAY VertexMaterialClass*[ArraySize];
		}
		else MaterialArray[pass]=nullptr;

		if (mmc->Has_Shader_Array(pass)) {
			ShaderArray[pass]=W3DNEWARRAY ShaderClass[ArraySize];
		}
		else ShaderArray[pass]=nullptr;
	}
}

GapFillerClass::GapFillerClass(const GapFillerClass& that) : mmc(nullptr), PolygonCount(that.PolygonCount)
{
	//DMS - We cannot take a reference to the mesh model here!  This is because the mesh model
	// class OWNS the GapFiller class (allocated via NEW).  If we take a reference here, there
	// will be an extra reference on the parent object, which will result in the parent object
	// not being destroyed.
	//
//	REF_PTR_SET(mmc,that.mmc);
	mmc = that.mmc;

	ArraySize=that.ArraySize;
	PolygonArray=W3DNEWARRAY TriIndex[ArraySize];
	for (int pass=0;pass<mmc->Get_Pass_Count();++pass) {
		for (int stage=0;stage<MeshMatDescClass::MAX_TEX_STAGES;++stage) {
			if (that.TextureArray[pass][stage]) {
				TextureArray[pass][stage]=W3DNEWARRAY TextureClass*[ArraySize];
				for (unsigned i=0;i<PolygonCount;++i) {
					TextureArray[pass][stage][i]=that.TextureArray[pass][stage][i];
					TextureArray[pass][stage][i]->Add_Ref();
				}
			}
			else TextureArray[pass][stage]=nullptr;
		}

		if (that.MaterialArray[pass]) {
			MaterialArray[pass]=W3DNEWARRAY VertexMaterialClass*[ArraySize];
			for (unsigned i=0;i<PolygonCount;++i) {
				MaterialArray[pass][i]=that.MaterialArray[pass][i];
				MaterialArray[pass][i]->Add_Ref();
			}
		}
		else MaterialArray[pass]=nullptr;

		if (that.ShaderArray[pass]) {
			ShaderArray[pass]=W3DNEWARRAY ShaderClass[ArraySize];
			for (unsigned i=0;i<PolygonCount;++i) {
				ShaderArray[pass][i]=that.ShaderArray[pass][i];
			}
		}
		else ShaderArray[pass]=nullptr;
	}
}

// ----------------------------------------------------------------------------
//
// Destruct gap-filler object. Release references to all textures and release
// the arrays.
//
// ----------------------------------------------------------------------------

GapFillerClass::~GapFillerClass()
{
	delete[] PolygonArray;

	for (int pass=0;pass<mmc->Get_Pass_Count();++pass) {
		for (int stage=0;stage<MeshMatDescClass::MAX_TEX_STAGES;++stage) {
			if (TextureArray[pass][stage]) {

				for (unsigned i=0;i<PolygonCount;++i) {
					REF_PTR_RELEASE(TextureArray[pass][stage][i]);
				}
				delete[] TextureArray[pass][stage];
			}
		}

		if (MaterialArray[pass]) {
			for (unsigned i=0;i<PolygonCount;++i) {
				REF_PTR_RELEASE(MaterialArray[pass][i]);
			}
			delete[] MaterialArray[pass];
		}

		delete[] ShaderArray[pass];
	}

	// DMS - Removed - See constructor for details.
//	REF_PTR_RELEASE(mmc);
}

// ----------------------------------------------------------------------------
//
// Add polygon to gap filler.
//
// ----------------------------------------------------------------------------

void GapFillerClass::Add_Polygon(unsigned polygon_index,unsigned vidx1,unsigned vidx2, unsigned vidx3)
{
	WWASSERT(PolygonCount<ArraySize);
	WWASSERT(vidx1!=vidx2 && vidx1!=vidx3 && vidx2!=vidx3);
Vector3 loc1=mmc->Get_Vertex_Array()[vidx1];
Vector3 loc2=mmc->Get_Vertex_Array()[vidx2];
Vector3 loc3=mmc->Get_Vertex_Array()[vidx3];
WWASSERT(loc1==loc2 || loc1==loc3 || loc2==loc3);
//sdflksdjflsdkf
//vidx1=mmc->Get_Polygon_Array()[polygon_index][0];
//vidx2=mmc->Get_Polygon_Array()[polygon_index][1];
//vidx3=mmc->Get_Polygon_Array()[polygon_index][2];

	PolygonArray[PolygonCount]=TriIndex(vidx1,vidx2,vidx3);
	for (int pass=0;pass<mmc->Get_Pass_Count();++pass) {
		if (mmc->Has_Shader_Array(pass)) {
			ShaderArray[pass][PolygonCount]=mmc->Get_Shader(polygon_index,pass);
		}
		if (mmc->Has_Material_Array(pass)) {
//			MaterialArray[pass][PolygonCount]=mmc->Get_Material(polygon_index,pass);
			MaterialArray[pass][PolygonCount]=mmc->Get_Material(mmc->Get_Polygon_Array()[polygon_index][0],pass);
		}
		for (int stage=0;stage<MeshMatDescClass::MAX_TEX_STAGES;++stage) {
			if (mmc->Has_Texture_Array(pass,stage)) {
				TextureArray[pass][stage][PolygonCount]=mmc->Get_Texture(polygon_index,pass,stage);
			}
		}
	}
	PolygonCount++;
}

// ----------------------------------------------------------------------------
//
// Resize buffers to match the polygon count exactly. After this call no more
// polygons can be added to the buffers.
//
// ----------------------------------------------------------------------------

void GapFillerClass::Shrink_Buffers()
{
	if (PolygonCount==ArraySize) return;

	// Shrink the polygon array
	TriIndex* new_polygon_array=W3DNEWARRAY TriIndex[PolygonCount];
	memcpy(new_polygon_array,PolygonArray,PolygonCount*sizeof(TriIndex));
	delete[] PolygonArray;
	PolygonArray=new_polygon_array;

	for (int pass=0;pass<mmc->Get_Pass_Count();++pass) {
		for (int stage=0;stage<MeshMatDescClass::MAX_TEX_STAGES;++stage) {
			if (TextureArray[pass][stage]) {
				// Shrink the texture array
				TextureClass** new_texture_array=W3DNEWARRAY TextureClass*[PolygonCount];
				memcpy(new_texture_array,TextureArray[pass][stage],PolygonCount*sizeof(TextureClass*));
				delete[] TextureArray[pass][stage];
				TextureArray[pass][stage]=new_texture_array;
			}
		}

		if (MaterialArray[pass]) {
			// Shrink the material array
			VertexMaterialClass** new_material_array=W3DNEWARRAY VertexMaterialClass*[PolygonCount];
			memcpy(new_material_array,MaterialArray[pass],PolygonCount*sizeof(VertexMaterialClass*));
			delete[] MaterialArray[pass];
			MaterialArray[pass]=new_material_array;
		}

		if (ShaderArray[pass]) {
			// Shrink the shader array
			ShaderClass* new_shader_array=W3DNEWARRAY ShaderClass[PolygonCount];
			memcpy(new_shader_array,ShaderArray[pass],PolygonCount*sizeof(ShaderClass));
			delete[] ShaderArray[pass];
			ShaderArray[pass]=new_shader_array;
		}
	}
	ArraySize=PolygonCount;
}

// ----------------------------------------------------------------------------
//
// Hard edges cause gaps to n-patches meshes. This code searches for hard edges
// and adds gaps filler polygons, using existing vertices.
//
// ----------------------------------------------------------------------------

void MeshModelClass::Init_For_NPatch_Rendering()
{
	if (!DX8Wrapper::Get_Current_Caps()->Support_NPatches()) return;
	if (!Get_Flag(MeshGeometryClass::ALLOW_NPATCHES)) return;
	if (GapFiller) return;

	const Vector3* locations=Get_Vertex_Array();
	unsigned vertex_count=Get_Vertex_Count();
	const TriIndex* polygon_indices=Get_Polygon_Array();
	unsigned polygon_count=Get_Polygon_Count();

	LocationHash.Remove_All();
	DuplicateLocationHash.Remove_All();
	SideHash.Remove_All();

	unsigned i=0;
	for (;i<vertex_count;++i) {
		if (LocationHash.Exists(locations[i])) {
			if (!DuplicateLocationHash.Exists(locations[i])) {
				DuplicateLocationHash.Insert(locations[i],i);
			}
		}
		else {
			LocationHash.Insert(locations[i],i);
		}
	}

	for (i=0;i<polygon_count;++i) {
		bool duplicates[3];
		duplicates[0]=DuplicateLocationHash.Exists(locations[polygon_indices[i][0]]);
		duplicates[1]=DuplicateLocationHash.Exists(locations[polygon_indices[i][1]]);
		duplicates[2]=DuplicateLocationHash.Exists(locations[polygon_indices[i][2]]);
		if (duplicates[0] && duplicates[1]) {
			TriangleSide tri(locations[polygon_indices[i][0]],locations[polygon_indices[i][1]]);
			if (SideHash.Exists(tri)) {
				SideIndexInfo side_index=SideHash.Get(tri);
				unsigned idx1=side_index.vidx1;
				unsigned idx2=side_index.vidx2;
				unsigned idx3=polygon_indices[i][0];
				unsigned idx4=polygon_indices[i][1];
				bool diff=!(idx1^idx3)|!(idx1^idx4)|!(idx2^idx3)|!(idx2^idx4);
				if (!diff) {
					if (!GapFiller) GapFiller=W3DNEW GapFillerClass(this);
					GapFiller->Add_Polygon(i,idx4,idx2,idx1);
					GapFiller->Add_Polygon(side_index.polygon_index,idx3,idx2,idx4);
				}
			}
			else {
				SideIndexInfo side_index;
				side_index.vidx1=polygon_indices[i][0];
				side_index.vidx2=polygon_indices[i][1];
				side_index.polygon_index=i;
				SideHash.Insert(tri,side_index);
			}
		}
		if (duplicates[1] && duplicates[2]) {
			TriangleSide tri(locations[polygon_indices[i][1]],locations[polygon_indices[i][2]]);
			if (SideHash.Exists(tri)) {
				SideIndexInfo side_index=SideHash.Get(tri);
				unsigned idx1=side_index.vidx1;
				unsigned idx2=side_index.vidx2;
				unsigned idx3=polygon_indices[i][1];
				unsigned idx4=polygon_indices[i][2];
				bool diff=!(idx1^idx3)|!(idx1^idx4)|!(idx2^idx3)|!(idx2^idx4);
				if (!diff) {
					if (!GapFiller) GapFiller=W3DNEW GapFillerClass(this);
					GapFiller->Add_Polygon(i,idx4,idx2,idx1);
					GapFiller->Add_Polygon(side_index.polygon_index,idx3,idx2,idx4);
				}
			}
			else {
				SideIndexInfo side_index;
				side_index.vidx1=polygon_indices[i][1];
				side_index.vidx2=polygon_indices[i][2];
				side_index.polygon_index=i;
				SideHash.Insert(tri,side_index);
			}
		}
		if (duplicates[2] && duplicates[0]) {
			TriangleSide tri(locations[polygon_indices[i][2]],locations[polygon_indices[i][0]]);
			if (SideHash.Exists(tri)) {
				SideIndexInfo side_index=SideHash.Get(tri);
				unsigned idx1=side_index.vidx1;
				unsigned idx2=side_index.vidx2;
				unsigned idx3=polygon_indices[i][2];
				unsigned idx4=polygon_indices[i][0];
				bool diff=!(idx1^idx3)|!(idx1^idx4)|!(idx2^idx3)|!(idx2^idx4);
				if (!diff) {
					if (!GapFiller) GapFiller=W3DNEW GapFillerClass(this);
					GapFiller->Add_Polygon(i,idx4,idx2,idx1);
					GapFiller->Add_Polygon(side_index.polygon_index,idx3,idx2,idx4);
				}
			}
			else {
				SideIndexInfo side_index;
				side_index.vidx1=polygon_indices[i][2];
				side_index.vidx2=polygon_indices[i][0];
				side_index.polygon_index=i;
				SideHash.Insert(tri,side_index);
			}
		}
	}

	LocationHash.Remove_All();
	DuplicateLocationHash.Remove_All();
	SideHash.Remove_All();

	if (GapFiller) GapFiller->Shrink_Buffers();
}
