/***********************************************************************************************
***                                                                                           ***
***  GeneralsX @feature T3-0 法线贴图 dot3 引擎对接 - WW3D2 侧挂载模块                          ***
***                                                                                           ***
***  本模块持有 dot3 可编程着色器的启用状态、VS/PS 句柄 (由设备层运行时汇编后注入)、逐像素       ***
***  强度, 并暴露 Apply/Restore 供 dx8renderer 刚性网格渲染循环在"需法线贴图"的 category 上按需  ***
***  挂载。默认关闭: 未启用时本模块对渲染管线零影响 (严格走原固定管线)。                         ***
***                                                                                           ***
***  铁律: 纯渲染层, 不进 GameLogic/WWMath; 平台代码隔离 - 句柄创建在设备层, 此处仅存句柄与应用。***
***********************************************************************************************/
#pragma once

#include "WWLib/always.h"
#include "WWMath/vector3.h"
#include "WWMath/vector4.h"   // GeneralsX @feature PS3.0-P3-14 14/10/2026: Set_Fog carries Vector4 pairs

class Matrix3D;
class Matrix4x4;
class LightEnvironmentClass;
class TextureClass;

namespace DX8NormalMap
{
	// Called by the device layer after it successfully assembles+creates VS/PS: passing handles marks them
	// available (still requires the opt-in switch to actually mount).
	// GeneralsX @build d3d9 port 11/10/2026 - handles are D3D9 interface pointers carried as UINT_PTR.
	void Install(UINT_PTR vs_handle, UINT_PTR ps_handle);

	// GeneralsX @feature PS3.0-P4-1 15/10/2026 roughness/metalness (PBR) variant pair for the dot3
	// single-light path.  #70 15/10/2026: the pair compiles unconditionally at shader install and
	// mounts whenever Set_PBR_Mode(1/2) is armed and the diffuse carries a "<stem>_rp" companion
	// (R=roughness, G=metalness, see 光影三阶段/粗糙度引擎承接工作清单.md §1) - independently of the
	// normal-map switch. Without handles, at mode 0, or without the companion, the path below runs
	// bit-for-bit - zero regression.
	void Install_PBR(UINT_PTR vs_handle, UINT_PTR ps_handle);

	// GeneralsX @feature PS3.0-P3-14 14/10/2026 A1: eye-depth distance haze for the GN family. colorStrength carries the haze rgb in xyz and the blend strength in w
	// (0 leaves every pixel numerically identical to the pre-fog shaders); startInvLen is
	// (eye-depth fog start, 1/ramp length, -, -). Injected once by the device layer from camera
	// settings - WW3D2 must not read game-side GlobalData itself. Default state is strength 0.
	void Set_Fog(const Vector4& colorStrength, const Vector4& startInvLen);

	// GeneralsX @feature PS3.0 Weather W5: rain-slick. The device layer injects 0..1 (0 = dry, every
	// pixel numerically as before); the roughness-reading PBR shaders pick it up in their free c8.w /
	// c11.w slot and lower roughness by up to 75% at fully wet. WW3D2 must not read game-side weather
	// state itself, same rule as Set_Fog above.
	void Set_Wetness(float wet);

	// Master switch (driven by command line -normalmap / INI). When OFF, Apply is never called.
	void Set_Enabled(bool on);
	bool Is_Enabled();

	// Determined jointly by handle validity from Install + the switch.
	bool Ready_To_Render();

	// GeneralsX @feature PS3.0 #70 15/10/2026: the roughness (PBR) toggle is now independent of the
	// normal-map switch. Mode 0 = off, 1 = M1 roughness-driven Blinn, 2 = M2 GGX metallic-roughness.
	// The device layer injects it once at shader install (OptionPreferences::getPBRMode, whose own
	// GENERALSX_PBR env override keeps QA A/B runs working) and the options menu re-arms it live via
	// gameOptionApply_Roughness. With the normal-map switch OFF but mode > 0, Apply still mounts the
	// PBR pair on RP_Ready meshes with a FLAT tangent normal (c3=(0,0,0,1)) - roughness specular on
	// the geometric normal, no bump.
	void Set_PBR_Mode(int mode);
	int  Get_PBR_Mode();
	bool RP_Ready();   // mode > 0 AND the PBR VS/PS pair installed

	// GeneralsX @feature Qoder 17/10/2026 P2 forensics: companion presence probes used by the
	// renderer's per-mesh-class reconciliation log (the same cached lookups Apply's bump/roughness
	// branches use, so a "yes" here is exactly what a mounted pass would find). Read-only; the
	// #68 team-colour and #87 sub-128px rejections are inside the finders and show through here.
	bool Has_Companion_Normal(TextureClass* diffuse);
	bool Has_Companion_RP(TextureClass* diffuse);

	// Per-pixel perturbation strength (0..2, default 0.8).
	// GeneralsX @bugfix PS3.0 #69 15/10/2026: this is now the BUMP deviation slider (PS c3.x),
	// not the sun-gain multiplier it used to ride on.
	void Set_Strength(float s);
	float Get_Strength();

	// GeneralsX @feature T3-0 P1: global sun fallback for meshes without a per-mesh light
	// environment. Injected by the display layer on time-of-day changes. world_dir follows the
	// LightEnvironmentClass sign convention (direction the light travels toward the surface is
	// negated: pass -lightZ), ambient is the scene object ambient (0..1 per channel).
	void Set_Global_Fallback_Light(const Vector3& world_dir, const Vector3& ambient);

	// GeneralsX @feature PS3.0 L1 13/10/2026: global sky fill light for the dot3 single-light path,
	// injected by the display layer on time-of-day changes alongside the fallback sun. dir follows
	// the same "toward the light" convention; color is the premultiplied fill diffuse. A zero
	// color (or a degenerate direction) keeps the single-key look bit-for-bit.
	void Set_Global_Fill_Light(const Vector3& world_dir, const Vector3& color);

}

//=============================================================================
// GeneralsX @feature PS3.0 L2 14/10/2026 geometric-normal per-pixel diffuse.
//
// The fixed pipeline lights rigid meshes PER VERTEX (Gouraud): on the big flat polys of tanks
// and buildings the key/fill lambert term is a vertex average, so light boundaries smear and
// curved hulls read as flat paper. This module mounts a lightweight VS/PS pair on plain
// XYZNDUV1/XYZNDUV2 rigid batches (the ones the dot3 path above never touches - no normal map)
// and applies a per-pixel CORRECTION to the two global lambert lobes:
//
//   out.rgb = tex.rgb * saturate(vertexColor.rgb + key*(ndl_pixel - ndl_vertex)
//                                              + fill*(ndf_pixel - ndf_vertex))
//
// The vertex color keeps every other fixed-pipeline contribution intact (ambient, bounce,
// attached/point lights, emissive, material tint, selection flash); only the sun and the L1
// sky fill are re-evaluated against the interpolated geometric normal. ndl_vertex is computed
// in the VS with the SAME light direction the fixed pipeline used, so the correction is a
// numerical no-op on triangles that happen to be flat-shaded.
//
// GeneralsX @tweak Qoder 17/10/2026: this paragraph said "Default OFF". That stopped being true on
// 14/10 when the pass shipped default-ON (dx8normalmap.cpp Install()). It is now the engine's
// PER-PIXEL LIGHTING MASTER SWITCH and is user-facing: Options.ini "PerPixelLighting" (Graphics
// Options page row "逐像素光照", live) seeds it through Set_Enabled, and the
// GENERALSX_GEOMNORMAL environment variable still wins over the ini for scripted A/B legs.
// Mounting additionally needs the device layer to have installed the shader pair; any precondition
// missing leaves the mesh on the fixed pipeline.
//=============================================================================
class VertexMaterialClass;

namespace DX8GeomNormal
{
	// Device layer installs after compiling the pair (two VS variants for the two FVF layouts,
	// one shared PS). Zero handles disable the module.
	void Install(UINT_PTR vs_duv1, UINT_PTR vs_duv2, UINT_PTR ps_handle);

	bool Ready_To_Render();

	// GeneralsX @feature Qoder 17/10/2026: the per-pixel master switch, as the Graphics Options page
	// row "逐像素光照" (Options.ini PerPixelLighting). Set_Enabled flips the module on/off at any time;
	// Ready_To_Render() is consulted per batch, so OFF drops those batches back to the fixed pipeline
	// on the very next frame and ON restores them without a restart. The device layer seeds the value
	// from OptionPreferences::getPerPixelLighting() right after Install(). Independent of the normal-map
	// and roughness rows: this gates the per-pixel LIGHTING only, never the _nm/_rp companion layers.
	void Set_Enabled(bool enabled);
	bool Is_Enabled();

	// Global key sun (direction TOWARD the light, same convention as DX8NormalMap's fallback
	// light) and its diffuse color, plus the L1 sky fill (zero color = off). Injected by the
	// display layer on time-of-day changes.
	void Set_Global_Key_Light(const Vector3& world_dir, const Vector3& color);
	void Set_Global_Fill_Light(const Vector3& world_dir, const Vector3& color);

	// Scene object ambient (the value fed to Set_Ambient_Light). The GN VS multiplies it by the
	// material ambient and adds material emissive to rebuild the fixed-pipeline Gouraud base.
	void Set_Global_Ambient(const Vector3& ambient);

	// GeneralsX @feature PS3.0 P3-21b 14/10/2026 sun shadow map reception INSIDE the GN pass.
	// Forensics (RECMODE 5/6 discriminator, 14/10): pixel-shader texture reads return 0 for ANY
	// bound texture in the game layer's raw post-scene receiver draw (constant-PS draw fine,
	// plain A8R8G8B8 dummy echo reads 0), while every sampling pass that uses a programmable VS
	// (GN/dot3/bloom) samples correctly. The shadow map therefore gets sampled here.
	//
	// Device layer compiles the shadow-aware pair (two VS variants + one PS) and installs it;
	// zero handles or an uninstalled pair keeps the plain GN shaders. The game layer then feeds
	// the per-frame state (map texture borrowed until Clear, world->light-clip matrix in the SAME
	// convention the terrain receiver uses with Matrix4x4::Transform_Vector, tap spread in texels,
	// depth bias in [0,1] space, strength 0..1). Active only when a non-null map with
	// strength > 0 is set; Apply then mounts the variant on qualifying batches, binds the map to
	// texture stage 2 (PS sampler s2), uploads the combined light matrix as VS c7..c10 and the
	// knobs as PS c15, and attenuates ONLY the light-0 diffuse and specular terms (fill lights,
	// ambient and the multiplicative terrain decal shadows stay untouched).
	void Install_Sun_Shadow(UINT_PTR vs_duv1, UINT_PTR vs_duv2, UINT_PTR ps_handle);

	// GeneralsX @feature PS3.0-P3-22 14/10/2026: optional 25-tap (5x5) PCF pixel shader for the
	// sun-shadow path. Installed as a sibling of the 3x3 pair; only mounted when the process was
	// started with GENERALSX_SMAP_PCF=5 AND this handle is valid - otherwise the 3x3 runs unchanged.
	void Install_Sun_Shadow_PCF5(UINT_PTR ps_handle);

	// GeneralsX @feature Qoder 17/10/2026 P2: skinned-batch variant pair (光影T0阶段 P2). The CPU
	// skinning pass submits the dynamic layout (D3DFVF_XYZ|NORMAL|DIFFUSE|TEX2 = DX8_FVF_XYZNDUV2)
	// whose COLOR0 channel the rigid GN pair does not carry, so skins need their own VS (relay of
	// the vertex diffuse) and PS (GN lighting multiplied by it, FF-parity chain like the dot3 pair).
	// Zero handles or an uninstalled pair leave skin batches on the fixed pipeline. Skins never take
	// the sun-shadow variants in P2 - those keep the no-COLOR0 rigid layouts.
	void Install_Skin(UINT_PTR vs_handle, UINT_PTR ps_handle);

	// GeneralsX @feature Qoder 17/10/2026 P3 (逐像素光照总闸): merged pair for the TG3 tangent-space
	// layout (光影T0阶段 P3 底座吸收细节层). ONE program = the plain GN lighting equation (8 directional
	// lights, hemi, fog, T1 spec, #73 flip) plus three uniform branches: s1 _nm bump (c27.y), s2 sun
	// shadow (c27.z, the GNS 3x3 kernel), s3 _rp roughness (c27.w; M1 Blinn / M2 GGX per c28, the old
	// PBR c8 contract re-expressed). Rigid bump meshes AND tangent skin batches (dynamic_fvf_type_nm)
	// share the layout and this pair; zero handles leave both on the fixed pipeline.
	void Install_Merged(UINT_PTR vs_handle, UINT_PTR ps_handle);

	void Set_Sun_Shadow(TextureClass* map, const Matrix4x4& worldToLightClip,
		float spreadTexels, float bias, float strength);
	void Clear_Sun_Shadow();
	bool Sun_Shadow_Ready();   // shadow variants installed + GN module enabled (map set not required)

	// Mount before drawing a plain realtime-lit rigid batch: uploads WVP (VS c0..c3), the world
	// rotation rows (VS c4..c6) and, on the PS, the material-scaled ambient (c4) plus up to four
	// world directional lights - dirs c5..c8 and matDiffuse-scaled colors c9..c12 - read from the
	// batch's OWN LightEnvironment via the same Get_Light_Direction/Get_Light_Diffuse/
	// Get_Equivalent_Ambient accessors the fixed Set_Light_Environment path feeds to D3D, so the
	// per-pixel Gouraud matches retail lighting except for the finer normal interpolation. When
	// lenv is null it falls back to the injected global key+fill+ambient. PS c13 carries the world
	// eye direction (w = spec power) and c14 the spec intensity, restoring the light0 Blinn-Phong
	// highlight the fixed pipeline gets from its white D3DLIGHT.Specular (knobs:
	// GENERALSX_GEOMNORMAL_SPECPOWER / GENERALSX_GEOMNORMAL_SPEC). fvf must be
	// DX8_FVF_XYZNUV1 or DX8_FVF_XYZNUV2 (realtime-lit, no vertex-color channel) - or, when the
	// P2 skin pair is installed, DX8_FVF_XYZNDUV2 (the dynamic skin layout; its COLOR0 rides
	// through the skin pair's FF-parity multiply and it never takes the sun-shadow variants) - or,
	// when the P3 merged pair is installed, DX8_FVF_XYZNDUV1TG3 (the tangent-space layout of bump/
	// roughness meshes and tangent skin batches): Apply then probes the _nm/_rp companions over
	// DX8NormalMap's cached finders (#68/#87 rejections carry through) and mounts the merged base,
	// diffuse being that batch's stage-0 texture (ignored by the three non-TG3 layouts). Returns
	// false (state untouched) otherwise. The caller (dx8renderer) guarantees every light is
	// directional and count<=MAX_LIGHTS before invoking this.
	bool Apply(const Matrix3D& world, const VertexMaterialClass* vm, int fvf, LightEnvironmentClass* lenv, TextureClass* diffuse);

	// Restore the fixed pipeline after drawing (FVF + no VS/PS).
	void Restore(UINT_PTR fvf);
}
