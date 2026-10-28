// GeneralsX @refactor 23/10/2026: skybox renderer split out of W3DWater.cpp. The code is the same
// five-face cubemap path; only its owner changed, so a map without water still has a sky and the
// water object no longer has to know about skybox assets.

#include "W3DDevice/GameClient/W3DSkyRenderer.h"

#include "W3DDevice/GameClient/W3DAssetManager.h"
#include "W3DDevice/GameClient/W3DRenderLighting.h"
#include "W3DDevice/GameClient/W3DMaterialInstance.h"
#include "WW3D2/assetmgr.h"
#include "WW3D2/rinfo.h"
#include "WW3D2/dx8wrapper.h"
#include "WW3D2/mesh.h"
#include "WW3D2/meshmdl.h"
#include "WW3D2/hlod.h"
#include "WW3D2/matinfo.h"
#include "WW3D2/texture.h"
#include "Common/GlobalData.h"

#include <cstdio>
#include <cstdlib>

W3DSkyRenderer *TheSkyRenderer = nullptr;

// new_skybox can load as a MeshClass or an HLOD wrapper depending on the asset/cache path. The
// cubemap upload needs the concrete mesh model, so walk the LOD tree instead of assuming CLASSID_MESH.
static MeshClass *W3D_FindSkyboxMesh(RenderObjClass *obj)
{
	if (!obj) return nullptr;
	if (obj->Class_ID() == RenderObjClass::CLASSID_MESH)
		return static_cast<MeshClass *>(obj);
	if (obj->Class_ID() == RenderObjClass::CLASSID_HLOD) {
		HLodClass *hlod = static_cast<HLodClass *>(obj);
		const int lods = hlod->Get_Lod_Count();
		for (int lod = 0; lod < lods; ++lod) {
			const int models = hlod->Get_Lod_Model_Count(lod);
			for (int m = 0; m < models; ++m) {
				if (MeshClass *mesh = W3D_FindSkyboxMesh(hlod->Get_Lod_Model(lod, m)))
					return mesh;
			}
		}
	}
	return nullptr;
}

W3DSkyRenderer::W3DSkyRenderer()
	: m_skyBox(nullptr)
{
}

W3DSkyRenderer::~W3DSkyRenderer()
{
	reset();
}

void W3DSkyRenderer::init()
{
	if (m_skyBox) return;
	if (!TheGlobalData) return;
	m_skyBox = ((W3DAssetManager*)W3DAssetManager::Get_Instance())->Create_Render_Obj(
		"new_skybox", TheGlobalData->m_skyBoxScale, 0);
	if (!m_skyBox) {
		fprintf(stderr, "[VKSKY] new_skybox asset unavailable; sky pass skipped\n");
		fflush(stderr);
	}
	if (m_skyBox) {
		// Enable clamping on all textures used by the skybox (to reduce corner seams).
		if (m_skyBox->Class_ID() == RenderObjClass::CLASSID_MESH) {
			MeshClass *mesh = (MeshClass*)m_skyBox;
			MaterialInfoClass *material = mesh->Get_Material_Info();
			if (material) {
				for (Int i = 0; i < material->Texture_Count(); ++i) {
					if (TextureClass *texture = material->Peek_Texture(i)) {
						texture->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
						texture->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
					}
				}
				REF_PTR_RELEASE(material);
			}
		}
	}
}

void W3DSkyRenderer::reset()
{
	REF_PTR_RELEASE(m_skyBox);
	m_skyBox = nullptr;
}

void W3DSkyRenderer::replaceTexture(const AsciiString& oldTexName, const AsciiString& newTexName)
{
	if (!m_skyBox) return;

	W3DAssetManager *assetManager = ((W3DAssetManager*)W3DAssetManager::Get_Instance());
	assetManager->replacePrototypeTexture(m_skyBox, oldTexName.str(), newTexName.str());

	if (m_skyBox->Class_ID() == RenderObjClass::CLASSID_MESH) {
		MeshClass *mesh = (MeshClass*)m_skyBox;
		MaterialInfoClass *material = mesh->Get_Material_Info();
		if (material) {
			for (Int i = 0; i < material->Texture_Count(); ++i) {
				if (TextureClass *texture = material->Peek_Texture(i)) {
					texture->Get_Filter().Set_U_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
					texture->Get_Filter().Set_V_Addr_Mode(TextureFilterClass::TEXTURE_ADDRESS_CLAMP);
				}
			}
			REF_PTR_RELEASE(material);
		}
	}
}

void W3DSkyRenderer::render(RenderInfoClass &rinfo)
{
	(void)rinfo;
	// Guard against null m_skyBox: Create_Render_Obj("new_skybox") can fail when the skybox asset is
	// unavailable (e.g. base Generals archives not found). The retired five-face mesh fallback is not
	// restored: a cubemap failure leaves the scene clear/fog colour in place.
	// GENERALSX_VK_SKY_FORCE=1 is the QA override that lets a DrawSkyBox=0 map exercise the cubemap
	// pass; it has no effect on a normal run.
	static int s_forceSky = -1;
	if (s_forceSky < 0) {
		const char* env = getenv("GENERALSX_VK_SKY_FORCE");
		s_forceSky = (env && atoi(env) != 0) ? 1 : 0;
	}
	if (!TheGlobalData || (!TheGlobalData->m_drawSkyBox && !s_forceSky) || !m_skyBox)
		return;

	W3DMaterialInstance skyMat;
	skyMat.desc.family = W3DMAT_SKY;
	skyMat.desc.features = W3DMATF_UNLIT;
	skyMat.desc.fvf = 0u;
	skyMat.desc.passCount = 1u;
	skyMat.desc.variant = 0u;
	W3D_MaterialCensus(skyMat.desc);

	const W3DRenderLightingState &atmos = W3D_GetRenderLighting();
	Vector3 tint(1.0f, 1.0f, 1.0f);
	if (atmos.revision > 0u) {
		// Ambient keeps the sky's overall exposure; the haze colour pulls the horizon toward the
		// same band terrain/GN/water fade into, so TOD/weather changes do not leave a mismatched sky.
		// GeneralsX @feature 26/10/2026 HDR: the tonemap owns exposure in the linear pipeline, so the
		// sky must not pre-multiply the EV (that applied it twice and in the wrong unit). The tint is
		// linear light now and keeps headroom so the tonemap/ACES have something to roll off.
		const bool linear = DX8Wrapper::VK_HDR_Enabled();
		float exposure = linear ? 1.0f : atmos.exposure;
		if (exposure < 0.5f) exposure = 0.5f;
		if (exposure > 2.0f) exposure = 2.0f;
		tint.Set(
			(0.70f + 0.20f * atmos.ambient[0] + 0.10f * atmos.fogColor[0]) * exposure,
			(0.70f + 0.20f * atmos.ambient[1] + 0.10f * atmos.fogColor[1]) * exposure,
			(0.70f + 0.20f * atmos.ambient[2] + 0.10f * atmos.fogColor[2]) * exposure);
		const float tintMax = linear ? 4.0f : 1.0f;
		if (tint.X > tintMax) tint.X = tintMax;
		if (tint.Y > tintMax) tint.Y = tintMax;
		if (tint.Z > tintMax) tint.Z = tintMax;
	}
	// Sky textures still go through the same material binder as water/grass. The mesh owns the
	// per-pass material, while the instance records/binds the five skybox faces for the cubemap
	// path (N/E/S/W/T = passes 0..4).
	TextureClass *skyFaces[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
	if (MeshClass *mesh = W3D_FindSkyboxMesh(m_skyBox)) {
		// new_skybox carries the five faces in the mesh material info, not as five passes: the retail
		// D3D8 draw submitted each face with its own material, while one MeshModel has a single pass.
		// MaterialInfo is the stable place to read all five.
		MaterialInfoClass *material = mesh->Get_Material_Info();
		if (material) {
			const int ntex = material->Texture_Count();
			for (int i = 0; i < ntex && i < 5; ++i)
				skyFaces[i] = material->Peek_Texture(i);
			for (int i = 0; i < ntex && i < 8; ++i)
				skyMat.textures[i] = material->Peek_Texture(i);
			REF_PTR_RELEASE(material);
		}
		if (!skyFaces[0]) {
			MeshModelClass *model = mesh->Peek_Model();
			if (model) skyFaces[0] = model->Peek_Texture(0, 0, 0);
		}
	}
	{
		static int s_skyDbg = 0;
		if (s_skyDbg < 4) {
			++s_skyDbg;
			fprintf(stderr, "[VKSKY] skybox faces: n=%p e=%p s=%p w=%p t=%p class=%d\n",
				(void*)skyFaces[0], (void*)skyFaces[1], (void*)skyFaces[2],
				(void*)skyFaces[3], (void*)skyFaces[4], (int)m_skyBox->Class_ID());
			fflush(stderr);
		}
	}
	W3DMaterialBinder::Bind(skyMat);
	bool usedCube = false;
	if (skyFaces[0] && skyFaces[1] && skyFaces[2] && skyFaces[3] && skyFaces[4] &&
		DX8Wrapper::VK_Sky_Set_Faces(skyFaces[0], skyFaces[1], skyFaces[2], skyFaces[3], skyFaces[4])) {
		DX8Wrapper::VK_Set_ShadowCaster(false);
		DX8Wrapper::VK_Set_ShadowReceiver(false);
		usedCube = DX8Wrapper::VK_Sky_Render(tint.X, tint.Y, tint.Z);
		DX8Wrapper::VK_Set_ShadowCaster(true);
		DX8Wrapper::VK_Set_ShadowReceiver(true);
	}
	if (!usedCube) {
		static int s_skyFailLog = 0;
		if (s_skyFailLog < 4) {
			++s_skyFailLog;
			fprintf(stderr, "[VKSKY] cubemap unavailable; sky skipped (mesh fallback removed)\n");
			fflush(stderr);
		}
	}
}
