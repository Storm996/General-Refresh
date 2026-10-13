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

	// GeneralsX @feature PS3.0 T5 13/10/2026 optional per-pixel multi-light pair (world TBN + up to
	// 4 point lights from the mesh's LightEnvironment).  Mounted by Apply only when the mesh
	// actually collects at least one point light; without it the single-light path above is used.
	void Install_ML(UINT_PTR vs_handle, UINT_PTR ps_handle);

	// Master switch (driven by command line -normalmap / INI). When OFF, Apply is never called.
	void Set_Enabled(bool on);
	bool Is_Enabled();

	// Determined jointly by handle validity from Install + the switch.
	bool Ready_To_Render();

	// Per-pixel perturbation strength (0..2, default 0.8).
	void Set_Strength(float s);
	float Get_Strength();

	// Mount before drawing a uses-normal-map rigid mesh: inject WVP / model-space key light / ambient
	// constants, bind the companion normal texture to stage1, switch VS/PS. fvf is the fixed-pipeline FVF
	// (DX8_FVF_XYZNDUV1TG3). Returns true on success; if any precondition fails (missing handle / no
	// companion texture) returns false and leaves state untouched. lenv may be null: the injected
	// global fallback light is used then (battlefield meshes rely on the global sun).
	bool Apply(const Matrix3D& world, LightEnvironmentClass* lenv, TextureClass* diffuse);

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

	// Restore the fixed pipeline after drawing (FVF + no PS).
	void Restore(UINT_PTR fvf);
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
// Default OFF: mounts only when GENERALSX_GEOMNORMAL=1 is set in the process environment at
// startup AND the device layer installed the shader pair. Any precondition missing leaves the
// mesh on the fixed pipeline - zero regression.
//=============================================================================
class VertexMaterialClass;

namespace DX8GeomNormal
{
	// Device layer installs after compiling the pair (two VS variants for the two FVF layouts,
	// one shared PS). Zero handles disable the module.
	void Install(UINT_PTR vs_duv1, UINT_PTR vs_duv2, UINT_PTR ps_handle);

	bool Ready_To_Render();

	// Global key sun (direction TOWARD the light, same convention as DX8NormalMap's fallback
	// light) and its diffuse color, plus the L1 sky fill (zero color = off). Injected by the
	// display layer on time-of-day changes.
	void Set_Global_Key_Light(const Vector3& world_dir, const Vector3& color);
	void Set_Global_Fill_Light(const Vector3& world_dir, const Vector3& color);

	// Scene object ambient (the value fed to Set_Ambient_Light). The GN VS multiplies it by the
	// material ambient and adds material emissive to rebuild the fixed-pipeline Gouraud base.
	void Set_Global_Ambient(const Vector3& ambient);

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
	// DX8_FVF_XYZNUV1 or DX8_FVF_XYZNUV2 (realtime-lit, no vertex-color channel). Returns false
	// (state untouched) otherwise. The caller (dx8renderer) guarantees every light is directional
	// and count<=4 before invoking this.
	bool Apply(const Matrix3D& world, const VertexMaterialClass* vm, int fvf, LightEnvironmentClass* lenv);

	// Restore the fixed pipeline after drawing (FVF + no VS/PS).
	void Restore(UINT_PTR fvf);
}
