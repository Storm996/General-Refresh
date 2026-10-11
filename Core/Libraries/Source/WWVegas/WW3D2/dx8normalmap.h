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
	void Install(unsigned vs_handle, unsigned ps_handle);

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

	// Restore the fixed pipeline after drawing (FVF + no PS).
	void Restore(unsigned fvf);
}
