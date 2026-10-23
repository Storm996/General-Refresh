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

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

// FILE: W3DShaderManager.h /////////////////////////////////////////////////////////
//
// Custom shader system that allows more options and easier device validation than
// possible with W3D2.
//
// Author: Mark Wilczynski, August 2001
//
///////////////////////////////////////////////////////////////////////////////

#pragma once

// GeneralsX @build d3d9 port 11/10/2026 - forward declarations so this header compiles in any
// include order; the real definitions come from <d3d9.h> pulled in by WW3D2/dx8wrapper.h.
struct _D3DVERTEXELEMENT9;
typedef struct _D3DVERTEXELEMENT9 D3DVERTEXELEMENT9;
struct IDirect3DPixelShader9;

#include "WW3D2/texture.h"
#include "MaterialFeatures.h"
enum FilterTypes CPP_11(: Int);
enum FilterModes CPP_11(: Int);
enum CustomScenePassModes CPP_11(: Int);
enum StaticGameLODLevel CPP_11(: Int);
enum ChipsetType CPP_11(: Int);
enum CpuType CPP_11(: Int);
enum GraphicsVenderID CPP_11(: Int);

class TextureClass;	///forward reference
// GeneralsX @feature PS3.0 batch7 13/10/2026 texture stages the shader manager keeps bindings for.
// Must match WW3D2's MAX_TEXTURE_STAGES; W3DShaderManager.cpp static_asserts the two are equal.
enum { SHADER_TEXTURE_STAGES = 16 };
/** System for managing complex rendering settings which are either not handled by
	WW3D2 or need custom paths depending on the video card.  This system will determine
	the proper shader given video card limitations and also allow the app to query the
	hardware for specific features.
*/
class W3DShaderManager
{
public:

	//put any custom shaders (not going through W3D) in here.
	enum ShaderTypes
	{	ST_INVALID,			//invalid shader type.
		ST_TERRAIN_BASE,	//shader to apply base terrain texture only
		ST_TERRAIN_BASE_NOISE1,	//shader to apply base texture and cloud/noise 1.
		ST_TERRAIN_BASE_NOISE2,	//shader to apply base texture and cloud/noise 2.
		ST_TERRAIN_BASE_NOISE12,//shader to apply base texture and both cloud/noise
		ST_SHROUD_TEXTURE,		//shader to apply shroud texture projection.
		ST_MASK_TEXTURE,		//shader to apply alpha mask texture projection.
		ST_ROAD_BASE,	//shader to apply base terrain texture only
		ST_ROAD_BASE_NOISE1,	//shader to apply base texture and cloud/noise 1.
		ST_ROAD_BASE_NOISE2,	//shader to apply base texture and cloud/noise 2.
		ST_ROAD_BASE_NOISE12,//shader to apply base texture and both cloud/noise
		ST_CLOUD_TEXTURE,			//shader to project clouds.
		ST_FLAT_TERRAIN_BASE,	//shader to apply base terrain texture only
		ST_FLAT_TERRAIN_BASE_NOISE1,	//shader to apply base texture and cloud/noise 1.
		ST_FLAT_TERRAIN_BASE_NOISE2,	//shader to apply base texture and cloud/noise 2.
		ST_FLAT_TERRAIN_BASE_NOISE12,//shader to apply base texture and both cloud/noise
		ST_FLAT_SHROUD_TEXTURE,		//shader to apply shroud texture projection.
		ST_MAX
	};


	W3DShaderManager();	///<constructor
	static void init();	///<determine optimal shaders for current device.
	static void shutdown();	///<release resources used by shaders
	static void updateCloud();	///<update the cloud position once every render frame.
	static ChipsetType getChipset();	///<return current device chipset.
	// GeneralsX @feature PS3.0 batch2 12/10/2026 Hardware detection without the user's shader-model
	// ceiling; getChipset() applies the ceiling to whatever this returns. Separated so the vendor-id
	// short-circuits inside cannot bypass -shadermodel / GENERALSX_SHADERMODEL.
	static ChipsetType detectChipsetTier();
	// GeneralsX @feature PS3.0 batch2 12/10/2026 The tier a shader path may emit for, from the caps the
	// device reports. Different from getChipset(): that ladder identifies legacy cards by vendor/device
	// id (every NVIDIA device id >= 0x250 comes back DC_GEFORCE4) and feeds the LOD preset table, so it
	// can sit below DC_GENERIC_PIXEL_SHADER_3_0 on a card that reports PS 3.0. Render paths that branch
	// on shader model must ask this function, not getChipset().
	static ChipsetType getShaderModelTier();
	static GraphicsVenderID getCurrentVendor() {return m_currentVendor;}	///<return current card vendor.
	// GeneralsX @bugfix BenderAI 13/02/2026 Use Int64 instead of __int64 for cross-platform builds
	static Int64 getCurrentDriverVersion() {return m_driverVersion; }	///<return current driver version.
	static Int getShaderPasses(ShaderTypes shader);	///<rendering passes required for shader
	static Int setShader(ShaderTypes shader, Int pass);	///<enable specific shader pass.
	/// GeneralsX @refactor 23/10/2026: one material-selection policy for the terrain/road families.
	/// HeightMap/FlatHeightMap used to repeat the reflection/lightmap/cloud ladder inline.
	static ShaderTypes selectTerrainShader(bool reflectionPass, bool useLightMap, bool doCloud);
	static ShaderTypes selectRoadShader(bool useLightMap, bool doCloud);
	/// Data-driven form: the decision reads the shared material descriptor, not ad-hoc booleans.
	static ShaderTypes selectTerrainShader(const W3DMaterialKey& material, bool reflectionPass);
	static ShaderTypes selectRoadShader(const W3DMaterialKey& material);
	static Int setShroudTex(Int stage);	///<Set shroud in a texture stage.
	// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: uploads the shared [TERRAIN30] per-pixel haze
	// ramp into PS constants c0/c1 (haze rgb + strength / start + 1/length). Used by the HLSL
	// terrain trio and road shader in W3DShaderManager.cpp and by the W3DTreeBuffer TREE30 path.
	static void uploadPerPixelFogConstants();
	// GeneralsX @refactor 23/10/2026: expose the one terrain/GN/tree/water haze recipe as plain
	// float[4] pairs (rgb + strength / start + 1/length). Water used to carry its own 55%/45%, 0.7
	// ramp, which is why a DistanceHaze=0 frame still hazed the sea while the land was clear.
	static void getWeatherFogRamp(float haze[4], float ramp[4]);
	// GeneralsX @feature PS3.0 batch8 G1 14/10/2026: TRUE once the ps_3_0 terrain trio is installed,
	// meaning the shared per-pixel haze ramp is valid. The TREE30 path gates its own HLSL haze on this.
	static Bool arePerPixelFogShadersActive();
	/// GeneralsX @refactor 23/10/2026: weather-scaled native cloud-shadow strength (0.35 clear,
	/// denser in storms). Keeps the receiver's cloud term on the same weather state as the haze.
	static Real getCloudShadowStrength();
	// GeneralsX @feature PS3.0 L2.1 TNM 17/10/2026: TRUE when the TERRAIN30 HLSL trio was installed AND
	// GENERALSX_TERRAIN_NM (default ON) is on. Gates both halves of the terrain normal pass: the packed
	// normal atlas in WorldHeightMap, and the s4/c2 normal modulation inside the terrain pixel shaders.
	// While FALSE the atlas is never built and the shaders are the plain fog trio, so a map with no
	// *_nm.tga sheets - or any device below ps_3_0 - renders exactly as before the feature existed.
	static Bool isTerrainNormalEnabled();
	static void resetShader(ShaderTypes shader);	///<make sure W3D2 gets restored to normal
	///Specify all textures (up to SHADER_TEXTURE_STAGES) which can be accessed by the shaders.
	static void setTexture(Int stage,TextureClass* texture) {m_Textures[stage]=texture;}
	///Return current texture available to shaders.
	static TextureClass *getShaderTexture(Int stage) { return m_Textures[stage];}	///<returns currently selected texture for given stage
	///Return last activated shader.
	static ShaderTypes getCurrentShader() {return m_currentShader;}
	/// Loads a .vso file and creates a vertex shader for it
	// GeneralsX @build d3d9 port 11/10/2026 - declaration is a D3DVERTEXELEMENT9 array and the
	// output handle is an IDirect3D{Vertex,Pixel}Shader9* (void** here) in D3D9; DX8 DWORD handles
	// and token arrays no longer exist.
	static HRESULT LoadAndCreateD3DShader(const char* strFilePath, const D3DVERTEXELEMENT9* pDeclaration, DWORD Usage, Bool ShaderType, void** pHandle);

	// GeneralsX @feature PS3.0 batch7 13/10/2026 -------------------------------------------------------------------------
	// The profile a runtime-compiled HLSL shader should be emitted for, derived from getShaderModelTier()
	// so no call site has to guess. Returns nullptr when the device is below ps_2_0/vs_2_0, which is the
	// signal for a render path to stay on its fixed-function or shipped-asset route.
	static const char* getPixelShaderProfile();
	static const char* getVertexShaderProfile();
	// Compile HLSL source that carries no "// d3d-target:" line for the highest profile the device
	// accepts, retrying one step down the ladder (ps_3_0 -> ps_2_0, vs_3_0 -> vs_2_0) if the device
	// refuses it. Handles the same void** the asset loader uses. ShaderType follows
	// LoadAndCreateD3DShader: false = pixel, true = vertex. logTag prefixes the failure line ("[DOT3]").
	// outProfile receives the profile actually compiled, so the caller can report it. Runtime HLSL
	// compilation is Windows-only (the CompatLib shim drives d3dcompiler), and the definition is guarded
	// accordingly - the profile getters above are available on every platform.
	static HRESULT compileShaderAtBestProfile(const char* hlslSource, Bool ShaderType, const char* logTag,
		const char** outProfile, void** pHandle);

	static Bool testMinimumRequirements(ChipsetType *videoChipType, CpuType *cpuType, Int *cpuFreq, MemValueType *numRAM, Real *intBenchIndex, Real *floatBenchIndex, Real *memBenchIndex);
	static StaticGameLODLevel getGPUPerformanceIndex();
	static Real GetCPUBenchTime();

	// Filter methods
	static Bool filterPreRender(FilterTypes filter, Bool &skipRender, CustomScenePassModes &scenePassMode); ///< Set up at start of render.  Only applies to screen filter shaders.
	static Bool filterPostRender(FilterTypes filter, FilterModes mode, Coord2D &scrollDelta, Bool &doExtraRender); ///< Called after render.  Only applies to screen filter shaders.
	static Bool filterSetup(FilterTypes filter, FilterModes mode);

	// Support routines for filter methods.
	// Support routines for filter methods.
	static Bool canRenderToTexture() { return (m_oldRenderSurface && m_newRenderSurface);}
	static void startRenderToTexture(); ///< Sets render target to texture.
	static IDirect3DTexture9 * endRenderToTexture(); ///< Ends render to texture, & returns texture.
	static IDirect3DTexture9 * getRenderTexture();	///< returns last used render target texture
	static Bool isRenderingToTexture() {return m_renderingToTexture; }
	static void drawViewport(Int color);	///<draws 2 triangles covering the current tactical viewport
	// GeneralsX @feature 05/10/2026 Bloom post-processing filter global toggle.
	static Bool IsBloomFilterEnabled();	///<returns true if the bloom glow filter is enabled.
	static void SetBloomFilterEnabled(Bool enabled);	///<enables/disables the bloom glow filter.
	// GeneralsX @feature PS3.0 Weather W2/W4 16/10/2026: the screen half of the weather system.
	// Two procedural passes over the tactical view rect - a multiply for cloud cover and a vignette,
	// then an additive drizzle/blizzard veil plus the lightning flash. Not a W3DFilterInterface: that
	// system has one filter slot (bloom owns it) and its render-to-texture chain cannot nest, while
	// blending over the finished frame needs neither. Call once per tactical view draw, after the
	// filter block: the rectangles below come from TheTacticalView, so a second caller would paint the
	// effect over the wrong part of the screen. No-ops when the published weather scalars are neutral.
	static void drawWeatherScreen();

	// GeneralsX @feature PS3.0 Weather W5 16/10/2026: the shader half of the weather coupling. Pushes
	// the two scalars that live in the WW3D2 dot3/PBR layer - the weather-scaled haze ramp (the GN/ML
	// units that the one-time shader install would otherwise freeze at scale 1.0) and the rain wetness
	// that the PBR roughness reads through c8.w / c11.w. Called from W3DDisplay::setTimeOfDay(), the
	// single lighting install point, so a weather change lands on the same frame everywhere.
	static void applyWeatherToShaders();

	// GeneralsX @feature Storm996 17/10/2026 PS3.0 haze user switch: the distance haze is a render
	// effect the player can now own. The ramp recipe used to read GENERALSX_GNFOG once into a static,
	// so the knob was a startup-only QA leg and nobody could turn the haze down from inside a match.
	// These two setters hold the live value (enabled + 0..1 strength) that Build_Weather_Fog_Ramp
	// consumes, and re-push the ramp so the shader families see it on the next draw - the graphics
	// options page is an overlay over the live battlefield, so dragging the slider previews immediately.
	// strength 0 (or enabled = false) makes every hazed pixel identical to the no-haze shader.
	static void setDistanceHaze(Bool enabled, Real strength01);
	static Bool isDistanceHazeEnabled();
	static Real getDistanceHazeStrength();

	// GeneralsX @feature Qoder 18/10/2026 PS3.0 live lighting knobs: the bloom composite gain and the
	// sun shadow map darkening strength used to be latched into a function-local static the first time
	// each ran, so the two menu rows could only be read at start-up. They now follow the haze pattern -
	// one lazily-seeded file static per value (the seed still lets OptionPreferences own the env
	// precedence) plus a setter the options page pushes through Display. Consumers read the getter per
	// frame, which is a static load, not an ini parse.
	static void setBloomIntensity(Real intensity01);
	static Real getBloomIntensity();					///< lazy seed from Options.ini / GENERALSX_BLOOM_INTENSITY
	// GeneralsX @feature Qoder 20/10/2026: the pure-Vulkan present pass has no MSAA to enable (every
	// attachment is 1 sample), so the anti-aliasing row drives its FXAA instead. Same live-knob route as
	// bloom: the options page pushes through Display, the backend reads the DX8Wrapper mirror per frame.
	static void setPostAntiAliasing(Bool enabled);
	static void setShadowMapStrength(Real strength01);
	static Real getShadowMapStrength();					///< lazy seed from Options.ini / GENERALSX_SHADOWMAP_INT

	// GeneralsX @feature Storm996 17/10/2026 PS3.0 haze coverage: the ramp above only reaches the
	// batches the custom ps_3_0 families take over (single-pass opaque rigid, no mapper UVs, pure
	// directional light - dx8renderer.cpp:2065/2123). Everything they hand back - skinned meshes,
	// multi-pass composites, vertex-coloured batches, point/spot-lit props, and terrain whenever the
	// HLSL terrain leg is off - draws on the fixed pipeline, which read no haze at all. That is why
	// only SOME distant buildings greyed out. These two calls wrap the fixed-function scene in
	// D3D's own linear fog fed from the same recipe, so the whole world shares one horizon.
	// Programmatic pixel shaders ignore D3DRS_FOGENABLE, so the already-hazed families cannot
	// double up; the pair is called around the 3D scene only, so the 2D scene and every UI pass stay
	// out of it. No-ops while the user switch is off or the strength is 0.
	static void beginFixedFunctionHaze();
	static void endFixedFunctionHaze();

protected:
	static TextureClass *m_Textures[SHADER_TEXTURE_STAGES];	///textures assigned to each of the possible stages
	static ChipsetType m_currentChipset;	///<last video card chipset that was detected.
	static GraphicsVenderID m_currentVendor;	///<last video card vendor
	// GeneralsX @bugfix BenderAI 13/02/2026AI - Use Int64 instead of __int64 (cross-platform)
	static Int64 m_driverVersion;			///<driver version of last chipset.
	static ShaderTypes m_currentShader;	///<last shader that was set.
	static Int m_currentShaderPass;		///<pass of last shader that was set.

	static FilterTypes m_currentFilter; ///< Last filter that was set.
	// Info for a render to texture surface for special effects.
	static Bool m_renderingToTexture;
	static IDirect3DSurface9 *m_oldRenderSurface;	///<previous render target
	static IDirect3DTexture9 *m_renderTexture;		///<texture into which rendering will be redirected.
	static IDirect3DSurface9 *m_newRenderSurface;	///<surface the filters sample from (resolve destination under MSAA)
	static IDirect3DSurface9 *m_oldDepthSurface;	///<previous depth buffer surface
	// GeneralsX @bugfix Storm996 12/10/2026 The render target actually bound while redirected, and the depth
	// buffer bound with it. They always share one sample count, which the old pairing of a single-sample
	// texture with the back buffer's multisampled depth buffer did not. m_rttColorSurface differs from
	// m_newRenderSurface only when a resolve is required.
	static IDirect3DSurface9 *m_rttColorSurface;
	static IDirect3DSurface9 *m_rttDepthSurface;
	// GeneralsX @tweak PS3.0-P2-3 14/10/2026 S6: one transient SetRenderTarget failure used to
	// permanently disable RTT (report §S6 "残留保守项"). Failures are now counted; RTT only
	// switches itself off after the fuse threshold is hit in a row, so a single bad frame no
	// longer kills bloom/monochrome/motion-blur/cross-fade for the whole session.
	static Int m_rttBindFailures;
	// GeneralsX @feature Qoder 18/10/2026 step 9.H: TRUE when init() registered the offscreen pair with
	// the recorder. Registration is what lets vk_publish_render_target follow the redirect; without it the
	// family has to stay on D3D9 or the scene would leave the mirror the present chain reads.
	static Bool m_vkRttReady;
	// GeneralsX @feature Qoder 18/10/2026 step 9.H: set when startRenderToTexture hands the recorder the
	// offscreen pair, so endRenderToTexture knows it owns handing it back to the scene.
	static Bool m_vkRttPublished;
	// GeneralsX @bugfix Storm996 12/10/2026 One owner for the lifetime of the offscreen target: init() runs
	// again on height-map rebuild and a rejected SetRenderTarget switches the feature off at runtime.
	static void releaseRenderToTextureSurfaces();


};

class W3DFilterInterface
{
public:
	virtual Int init() = 0;			///<perform any one time initialization and validation
	virtual Int shutdown() { return TRUE;};			///<release resources used by shader
	virtual Bool preRender(Bool &skipRender, CustomScenePassModes &scenePassMode) {skipRender=false; return false;} ///< Set up at start of render.  Only applies to screen filter shaders.
	virtual Bool postRender(FilterModes mode, Coord2D &scrollDelta, Bool &doExtraRender){return false;} ///< Called after render.  Only applies to screen filter shaders.
	virtual Bool setup(FilterModes mode){return false;} ///< Called when the filter is started, one time before the first prerender.
	// GeneralsX @feature Qoder 18/10/2026 step 9.H: TRUE only when the scene this filter redirects and the
	// quad it draws afterwards are both fixed-function work the Vulkan facade can record. A filter that
	// still binds a runtime-compiled pixel shader has to stay FALSE: publishing its target would move the
	// scene onto an offscreen mirror while the post pass falls back to D3D9, leaving the present chain
	// reading a mirror that never received the frame.
	virtual Bool canVulkanRtt() const { return FALSE; }
protected:
	virtual Int set(FilterModes mode) = 0;		///<setup shader for the specified rendering pass.
	 ///do any custom resetting necessary to bring W3D in sync.
	virtual void reset() = 0;
};


/*=========  ScreenMotionBlurFilter	=============================================================*/
///applies motion blur to viewport.
class ScreenMotionBlurFilter : public W3DFilterInterface
{
public:
	virtual Int set(FilterModes mode) override;		///<setup shader for the specified rendering pass.
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	virtual Int shutdown() override;		///<release resources used by shader
	virtual Bool preRender(Bool &skipRender, CustomScenePassModes &scenePassMode) override; ///< Set up at start of render.  Only applies to screen filter shaders.
	virtual Bool postRender(FilterModes mode, Coord2D &scrollDelta, Bool &doExtraRender) override; ///< Called after render.  Only applies to screen filter shaders.
	virtual Bool setup(FilterModes mode) override; ///< Called when the filter is started, one time before the first prerender.
	// GeneralsX @feature Qoder 18/10/2026 step 9.H: texture read, alpha-combiner quad and additive echo
	// quads are all fixed-function; the whole pass is consumable by the recorder.
	virtual Bool canVulkanRtt() const override { return TRUE; }
	ScreenMotionBlurFilter();

	static void setZoomToPos(const Coord3D *pos) {m_zoomToPos = *pos; m_zoomToValid = true;}

protected:
	enum {MAX_COUNT = 60,
				MAX_LIMIT = 30,
				COUNT_STEP = 5,
				DEFAULT_PAN_FACTOR = 30};
	Int m_maxCount;
	Int m_lastFrame;
	Bool m_decrement;
	Bool m_skipRender;
	Bool m_additive;
	Bool m_doZoomTo;
	Coord2D m_priorDelta;
	Int m_panFactor;


	static Coord3D m_zoomToPos;
	static Bool m_zoomToValid;
} ;

/*=========  ScreenBWFilter	=============================================================*/
///converts viewport to black & white.
class ScreenBWFilter : public W3DFilterInterface
{
	IDirect3DPixelShader9*	m_dwBWPixelShader;		///<D3D9 pixel shader interface which tints texture to black & white (was DX8 DWORD handle).
public:
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Int shutdown() override;		///<release resources used by shader
	virtual Bool preRender(Bool &skipRender, CustomScenePassModes &scenePassMode) override; ///< Set up at start of render.  Only applies to screen filter shaders.
	virtual Bool postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender) override; ///< Called after render.  Only applies to screen filter shaders.
	virtual Bool setup(FilterModes mode) override {return true;} ///< Called when the filter is started, one time before the first prerender.
	static void setFadeParameters(Int fadeFrames, Int direction)
	{
		m_curFadeFrame = 0;
		m_fadeFrames = fadeFrames;
		m_fadeDirection = direction;
	}
protected:
	virtual Int set(FilterModes mode) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	static Int m_fadeFrames;
	static Int m_fadeDirection;
	static Int m_curFadeFrame;
	static Real m_curFadeValue;
};

class ScreenBWFilterDOT3 : public ScreenBWFilter
{
public:
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Int shutdown() override;		///<release resources used by shader
	virtual Bool preRender(Bool &skipRender, CustomScenePassModes &scenePassMode) override; ///< Set up at start of render.  Only applies to screen filter shaders.
	virtual Bool postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender) override; ///< Called after render.  Only applies to screen filter shaders.
	virtual Bool setup(FilterModes mode) override {return true;} ///< Called when the filter is started, one time before the first prerender.
	// GeneralsX @feature Qoder 18/10/2026 step 9.H: this variant draws only through D3DTSS combiners and
	// Draw_User_Primitive, so the recorder can consume its post pass. The base ScreenBWFilter stays FALSE
	// because its pixel shader is compiled at runtime and has no program tag.
	virtual Bool canVulkanRtt() const override { return TRUE; }
protected:
	virtual Int set(FilterModes mode) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
};

/*=========  ScreenCrossFadeFilter	=============================================================*/
///Fades between 2 different rendered frames.
class ScreenCrossFadeFilter : public W3DFilterInterface
{
public:
	virtual Int init() override;			///<perform any one time initialization and validation
	virtual Int shutdown() override;		///<release resources used by shader
	virtual Bool preRender(Bool &skipRender, CustomScenePassModes &scenePassMode) override; ///< Set up at start of render.  Only applies to screen filter shaders.
	virtual Bool postRender(FilterModes mode, Coord2D &scrollDelta,Bool &doExtraRender) override; ///< Called after render.  Only applies to screen filter shaders.
	virtual Bool setup(FilterModes mode) override {return true;} ///< Called when the filter is started, one time before the first prerender.
	// GeneralsX @feature Qoder 18/10/2026 step 9.H: two-texture fixed-function quad; both the offscreen
	// image and the mask are sampleable mirrors, so the post pass is consumable by the recorder.
	virtual Bool canVulkanRtt() const override { return TRUE; }
	static void setFadeParameters(Int fadeFrames, Int direction)
	{
		m_curFadeFrame = 0;
		m_fadeFrames = fadeFrames;
		m_fadeDirection = direction;
	}
	static Real getCurrentFadeValue()	{ return m_curFadeValue;}
	static TextureClass *getCurrentMaskTexture() { return m_fadePatternTexture;}
protected:
	virtual Int set(FilterModes mode) override;		///<setup shader for the specified rendering pass.
	virtual void reset() override;		///<do any custom resetting necessary to bring W3D in sync.
	Bool updateFadeLevel();		///<updated current state of fade and return true if not finished.
	static Int m_fadeFrames;
	static Int m_fadeDirection;
	static Int m_curFadeFrame;
	static Real m_curFadeValue;
	static Bool m_skipRender;
	static TextureClass *m_fadePatternTexture;	///<shape/pattern of the fade
};

// GeneralsX @refactor 23/10/2026: first real material instance. It owns the descriptor, the selected
// terrain shader and its texture set, so HeightMap stops passing loose ShaderTypes/texture pointers
// around. The bind step is still the legacy W3DShaderManager route (fallback preserved).
struct W3DTerrainMaterialInstance
{
	W3DMaterialDesc desc;
	W3DShaderManager::ShaderTypes shader;
	TextureClass *textures[5];
	Int passCount;

	W3DTerrainMaterialInstance();
	void BindTextures() const;
};
