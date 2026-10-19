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
 *                 Project Name : ww3d                                                         *
 *                                                                                             *
 *                     $Archive:: /Commando/Code/ww3d2/dx8wrapper.h                           $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 08/05/02 2:40p                                              $*
 *                                                                                             *
 *                    $Revision:: 92                                                          $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 * 06/27/02 KM Render to shadow buffer texture support														*
 * 08/05/02 KM Texture class redesign
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#pragma once

#include "WWLib/always.h"
#include "dllist.h"
// GeneralsX @build BenderAI 10/02/2026
// Use angle brackets (not quotes) to skip local d3d8.h stub and get DXVK headers
// DXVK's d3d8.h includes d3d8types.h internally - no manual includes needed
#include <d3d9.h>
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
#include <intrin.h>   // GeneralsX @debug Qoder 19/10/2026 BUG-03: _ReturnAddress for the render-state watch ring
#endif
#include "WWMath/matrix4.h"
#include "statistics.h"
#include "WWLib/wwstring.h"
#include "WW3D2/lightenvironment.h"
#include "WW3D2/shader.h"
#include "WWMath/vector4.h"
#include "WWLib/cpudetect.h"
#include "dx8caps.h"

#include "texture.h"
#include "Backend/VulkanTexMirror.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"
#include "WW3D2/vertmaterial.h"

// GeneralsX @build d3d9 port 11/10/2026 - D3D9 removed D3DVSD_REG token macros; vertex declarations
// are D3DVERTEXELEMENT9 arrays. SDK only provides D3DDECL_END(), so this helper builds one element:
// { Stream, Offset, Type, Method, Usage, UsageIndex } with explicit byte offset (D3D9 requires it).
#define D3DDECL_USAGE(type, usage, usage_index, offset) { 0, (WORD)(offset), (BYTE)(type), D3DDECLMETHOD_DEFAULT, (BYTE)(usage), (BYTE)(usage_index) }

/*
** Registry value names
*/
#define	VALUE_NAME_RENDER_DEVICE_NAME					"RenderDeviceName"
#define	VALUE_NAME_RENDER_DEVICE_WIDTH				"RenderDeviceWidth"
#define	VALUE_NAME_RENDER_DEVICE_HEIGHT				"RenderDeviceHeight"
#define	VALUE_NAME_RENDER_DEVICE_DEPTH				"RenderDeviceDepth"
#define	VALUE_NAME_RENDER_DEVICE_WINDOWED			"RenderDeviceWindowed"
#define	VALUE_NAME_RENDER_DEVICE_TEXTURE_DEPTH		"RenderDeviceTextureDepth"

// GeneralsX @feature PS3.0 batch3 12/10/2026 The three per-stage caches below are the wrapper's
// dirty-check tables. Anything at or above MAX_TEXTURE_STAGES bypasses them and is pushed to the
// device on every call, so the bound has to cover the whole programmable-shader range: ps_1_4 gives
// 6 samplers, ps_2_0 and ps_3_0 give 16. Fixed-function passes are unaffected - they keep being
// limited by D3DCAPS9::MaxSimultaneousTextures through Get_Max_Textures_Per_Pass(), which is what the
// clearing loops in Apply_Default_State and the lightmap passes iterate over.
const unsigned MAX_TEXTURE_STAGES=16;
const unsigned MAX_VERTEX_STREAMS=2;
// GeneralsX @feature PS3.0 batch1 12/10/2026 Shader constant caches used to be sized for the ps_1_x
// budget (8 pixel / 96 vertex registers), which is the first wall any ps_2_0+ shader hits. The caches
// are now allocated at the shader model 3.0 spec sizes; the registers a shader may actually address
// are the per-device limits DX8Wrapper::Get_*_Shader_Constant_Limit() report - the pixel count comes
// from the reported shader model, the vertex count from D3DCAPS9, both floored at the legacy budget.
const unsigned MAX_PIXEL_SHADER_CONSTANTS_PS1X=8;			// ps_1_1..1_4 c0..c7
const unsigned MAX_VERTEX_SHADER_CONSTANTS_PS1X=96;		// legacy DX8 budget, kept as a floor
const unsigned MAX_PIXEL_SHADER_CONSTANTS_SM3=224;		// ps_3_0 c0..c223 - the cache size
const unsigned MAX_VERTEX_SHADER_CONSTANTS_SM3=256;		// vs_3_0 gives 256 (vs_2_0 gives 128)
const unsigned MAX_VERTEX_SHADER_CONSTANTS=MAX_VERTEX_SHADER_CONSTANTS_SM3;
const unsigned MAX_PIXEL_SHADER_CONSTANTS=MAX_PIXEL_SHADER_CONSTANTS_SM3;
const unsigned MAX_SHADOW_MAPS=1;

#define prevVer
#define nextVer


enum {
	BUFFER_TYPE_DX8,
	BUFFER_TYPE_SORTING,
	BUFFER_TYPE_DYNAMIC_DX8,
	BUFFER_TYPE_DYNAMIC_SORTING,
	BUFFER_TYPE_INVALID
};

class VertexMaterialClass;
class CameraClass;
class LightEnvironmentClass;
class RenderDeviceDescClass;
class VertexBufferClass;
class DynamicVBAccessClass;
class IndexBufferClass;
class DynamicIBAccessClass;
class TextureClass;
class ZTextureClass;
class LightClass;
class SurfaceClass;
class DX8Caps;
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the fixed-function state block that
// Backend/VulkanDraw.h defines (and that header is only pulled in by the win64 Vulkan build). Declared
// here as an incomplete type so this header stays free of the Vulkan seam, exactly like the void*
// handles the rest of the wedge API uses.
struct VKDraw_FF_State;
struct VKDraw_Submit;

struct DX8FrameStatistics
{
	DX8FrameStatistics() :
		matrix_changes(0),
		material_changes(0),
		vertex_buffer_changes(0),
		index_buffer_changes(0),
		light_changes(0),
		texture_changes(0),
		render_state_changes(0),
		texture_stage_state_changes(0),
		dx8_calls(0),
		draw_calls(0)
	{
	}

	unsigned matrix_changes;
	unsigned material_changes;
	unsigned vertex_buffer_changes;
	unsigned index_buffer_changes;
	unsigned light_changes;
	unsigned texture_changes;
	unsigned render_state_changes;
	unsigned texture_stage_state_changes;
	unsigned dx8_calls;
	unsigned draw_calls;
};

#define DX8_RECORD_MATRIX_CHANGE()				FrameStatistics.matrix_changes++
#define DX8_RECORD_MATERIAL_CHANGE()			FrameStatistics.material_changes++
#define DX8_RECORD_VERTEX_BUFFER_CHANGE()		FrameStatistics.vertex_buffer_changes++
#define DX8_RECORD_INDEX_BUFFER_CHANGE()		FrameStatistics.index_buffer_changes++
#define DX8_RECORD_LIGHT_CHANGE()				FrameStatistics.light_changes++
#define DX8_RECORD_TEXTURE_CHANGE()				FrameStatistics.texture_changes++
#define DX8_RECORD_RENDER_STATE_CHANGE()		FrameStatistics.render_state_changes++
#define DX8_RECORD_TEXTURE_STAGE_STATE_CHANGE() FrameStatistics.texture_stage_state_changes++
#define DX8_RECORD_DX8_CALLS()					FrameStatistics.dx8_calls++
#define DX8_RECORD_DRAW_CALLS()					FrameStatistics.draw_calls++

extern bool _DX8SingleThreaded;

void DX8_Assert();
void Log_DX8_ErrorCode(unsigned res);

WWINLINE void DX8_ErrorCode(unsigned res)
{
	if (res==D3D_OK) return;
	Log_DX8_ErrorCode(res);
}

#ifdef WWDEBUG
#define DX8CALL_HRES(x,res) DX8_Assert(); res = DX8Wrapper::_Get_D3D_Device8()->x; DX8_ErrorCode(res); DX8Wrapper::Increment_DX8_CallCount();
#define DX8CALL(x) DX8_Assert(); DX8_ErrorCode(DX8Wrapper::_Get_D3D_Device8()->x); DX8Wrapper::Increment_DX8_CallCount();
#define DX8CALL_D3D(x) DX8_Assert(); DX8_ErrorCode(DX8Wrapper::_Get_D3D8()->x); DX8Wrapper::Increment_DX8_CallCount();
#define DX8_THREAD_ASSERT() if (_DX8SingleThreaded) { WWASSERT_PRINT(DX8Wrapper::_Get_Main_Thread_ID()==ThreadClass::_Get_Current_Thread_ID(),"DX8Wrapper::DX8 calls must be called from the main thread!"); }
#else
#define DX8CALL_HRES(x,res) res = DX8Wrapper::_Get_D3D_Device8()->x; DX8Wrapper::Increment_DX8_CallCount();
#define DX8CALL(x) DX8Wrapper::_Get_D3D_Device8()->x; DX8Wrapper::Increment_DX8_CallCount();
#define DX8CALL_D3D(x) DX8Wrapper::_Get_D3D8()->x; DX8Wrapper::Increment_DX8_CallCount();
#define DX8_THREAD_ASSERT() ;
#endif


#define no_EXTENDED_STATS
// EXTENDED_STATS collects additional timing statistics by turning off parts
// of the 3D drawing system (terrain, objects, etc.)
#ifdef EXTENDED_STATS
class DX8_Stats
{
public:
	bool m_showingStats;
	bool m_disableTerrain;
	bool m_disableWater;
	bool m_disableObjects;
	bool m_disableOverhead;
	bool m_disableConsole;
	int  m_debugLinesToShow;
	int	 m_sleepTime;
public:
	DX8_Stats::DX8_Stats() {
		m_disableConsole = m_showingStats = m_disableTerrain = m_disableWater = m_disableOverhead = m_disableObjects = false;
		m_sleepTime = 0;
		m_debugLinesToShow = -1; // -1 means show all expected lines of output
	}
};
#endif


// This virtual interface was added for the Generals RTS.
// It is called before resetting the dx8 device to ensure
// that all dx8 resources are released.  Otherwise reset fails. jba.
class DX8_CleanupHook
{
public:
	virtual void ReleaseResources()=0;
	virtual void ReAcquireResources()=0;
};


struct RenderStateStruct
{
	ShaderClass shader;
	VertexMaterialClass* material;
	TextureBaseClass * Textures[MAX_TEXTURE_STAGES];
	D3DLIGHT9 Lights[8];	// GeneralsX @feature PS3.0-P3-18 14/10/2026: 4 -> 8, see LightEnvironmentClass::MAX_LIGHTS
	bool LightEnable[8];
  //unsigned lightsHash;
	// GeneralsX @refactor BenderAI 10/02/2026
	// Changed from D3DMATRIX to Matrix4x4 - fighter19 pattern eliminates conversion functions
	// Matrices stored transposed, cast to D3DMATRIX* when calling D3D APIs
	Matrix4x4 world;
	Matrix4x4 view;
	unsigned vertex_buffer_types[MAX_VERTEX_STREAMS];
	unsigned index_buffer_type;
	unsigned short vba_offset;
	unsigned short vba_count;
	unsigned short iba_offset;
	VertexBufferClass* vertex_buffers[MAX_VERTEX_STREAMS];
	IndexBufferClass* index_buffer;
	unsigned short index_base_offset;

	RenderStateStruct();
	~RenderStateStruct();

	RenderStateStruct& operator= (const RenderStateStruct& src);
};

/**
** DX8Wrapper
**
** DX8 interface wrapper class.  This encapsulates the DX8 interface; adding redundant state
** detection, stat tracking, etc etc.  In general, we will wrap all DX8 calls with at least
** an WWINLINE function so that we can add stat tracking, etc if needed.  Direct access to the
** D3D device will require "friend" status and should be granted only in extreme circumstances :-)
*/
class DX8Wrapper
{
	enum ChangedStates {
		WORLD_CHANGED	=	1<<0,
		VIEW_CHANGED	=	1<<1,
		LIGHT0_CHANGED	=	1<<2,
		LIGHT1_CHANGED	=	1<<3,
		LIGHT2_CHANGED	=	1<<4,
		LIGHT3_CHANGED	=	1<<5,
		TEXTURE0_CHANGED=	1<<6,
		TEXTURE1_CHANGED=	1<<7,
		TEXTURE2_CHANGED=	1<<8,
		TEXTURE3_CHANGED=	1<<9,
		// GeneralsX @feature PS3.0-P3-18 14/10/2026: per-slot change bits for light units 4..7,
		// placed in the 1<<10..1<<13 gap the old layout left unused.
		LIGHT4_CHANGED	=	1<<10,
		LIGHT5_CHANGED	=	1<<11,
		LIGHT6_CHANGED	=	1<<12,
		LIGHT7_CHANGED	=	1<<13,
		MATERIAL_CHANGED=	1<<14,
		SHADER_CHANGED	=	1<<15,
		VERTEX_BUFFER_CHANGED = 1<<16,
		INDEX_BUFFER_CHANGED = 1 << 17,
		WORLD_IDENTITY=	1<<18,
		VIEW_IDENTITY=		1<<19,

		TEXTURES_CHANGED=
			TEXTURE0_CHANGED|TEXTURE1_CHANGED|TEXTURE2_CHANGED|TEXTURE3_CHANGED,
		LIGHTS_CHANGED=
			LIGHT0_CHANGED|LIGHT1_CHANGED|LIGHT2_CHANGED|LIGHT3_CHANGED|
			LIGHT4_CHANGED|LIGHT5_CHANGED|LIGHT6_CHANGED|LIGHT7_CHANGED,
	};

	// GeneralsX @feature PS3.0-P3-18 14/10/2026: the old mark/apply sites computed the per-unit bit as
	// LIGHT0_CHANGED << index, which silently collided with TEXTURE0..3 once indices passed 3 (the
	// light bits deliberately live in the 1<<10..1<<13 gap now). Every site looks this up instead.
	static unsigned Get_Light_Changed_Bit(unsigned index)
	{
		static const unsigned bits[8] = {
			LIGHT0_CHANGED, LIGHT1_CHANGED, LIGHT2_CHANGED, LIGHT3_CHANGED,
			LIGHT4_CHANGED, LIGHT5_CHANGED, LIGHT6_CHANGED, LIGHT7_CHANGED,
		};
		return (index < 8) ? bits[index] : 0;
	}

	static void Draw_Sorting_IB_VB(
		unsigned primitive_type,
		unsigned short start_index,
		unsigned short polygon_count,
		unsigned short min_vertex_index,
		unsigned short vertex_count);

	static void Draw(
		unsigned primitive_type,
		unsigned short start_index,
		unsigned short polygon_count,
		unsigned short min_vertex_index=0,
		unsigned short vertex_count=0);

public:
#ifdef EXTENDED_STATS
	static DX8_Stats stats;
#endif

	static bool Init(void * hwnd, bool lite = false);
	static void Shutdown();

	static void SetCleanupHook(DX8_CleanupHook *pCleanupHook) {m_pCleanupHook = pCleanupHook;};
	/*
	** Some WW3D sub-systems need to be initialized after the device is created and shutdown
	** before the device is released.
	*/
	static void	Do_Onetime_Device_Dependent_Inits();
	static void Do_Onetime_Device_Dependent_Shutdowns();

	static bool Is_Device_Lost() { return IsDeviceLost; }
	static bool Is_Initted() { return IsInitted; }

	static bool Has_Stencil ();
	// GeneralsX @feature PS3.0-P2-7 14/10/2026 I6: expose the Compute_Caps [SM3_CAPS]/[DEPTH_BIAS]
	// probe results as values so callers (shadow passes, PS3.0 features) can gate on real device
	// replies instead of only the stderr line. Defaults are conservative (false/0) until probed.
	static bool Is_Depth_Bias_Available();
	static unsigned Get_PS20_Caps_Flags();
	static void Get_Format_Name(unsigned int format, StringClass *tex_format);

	/*
	** Rendering
	*/
	static void Begin_Scene();
	static void End_Scene(bool flip_frame = true);

	// Flip until the primary buffer is visible.
	static void Flip_To_Primary();

	static void Clear(bool clear_color, bool clear_z_stencil, const Vector3 &color, float dest_alpha=0.0f, float z=1.0f, unsigned int stencil=0);

	static void	Set_Viewport(CONST D3DVIEWPORT9* pViewport);

	static void Set_Vertex_Buffer(const VertexBufferClass* vb, unsigned stream=0);
	static void Set_Vertex_Buffer(const DynamicVBAccessClass& vba);
	static void Set_Index_Buffer(const IndexBufferClass* ib,unsigned short index_base_offset);
	static void Set_Index_Buffer(const DynamicIBAccessClass& iba,unsigned short index_base_offset);
	static void Set_Index_Buffer_Index_Offset(unsigned offset);

	static void Get_Render_State(RenderStateStruct& state);
	static void Set_Render_State(const RenderStateStruct& state);
	static void Release_Render_State();

	static void Set_DX8_Material(const D3DMATERIAL9* mat);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: read-side of the material shadow. D3D has no
	// GetMaterial, and VertexMaterialClass::Apply() pushes the W3D material straight through
	// Set_DX8_Material to the device, so the fixed-function vertex stage - which needs ambient, diffuse,
	// specular, emissive and the power term for every lit batch - can only be fed from here. Falls back to
	// the retail default (the values VertexMaterialClass::Apply_Null installs) until the first material
	// has been applied, which is also what the D3D9 path draws with in that window.
	static const D3DMATERIAL9& Peek_DX8_Material();

	static void Set_Gamma(float gamma,float bright,float contrast,bool calibrate=true,bool uselimit=true);

	// Set_ and Get_Transform() functions take the matrix in Westwood convention format.

	static void Set_DX8_ZBias(int zbias);
	static void Set_Projection_Transform_With_Z_Bias(const Matrix4x4& matrix,float znear, float zfar);	// pointer to 16 matrices

	// GeneralsX @feature PS3.0 batch7 13/10/2026 the two hardware depth-bias render states. DX8 never
	// offered them: the only bias the legacy API had was D3DRS_ZBIAS, an integer 0..15 that the d3d9 port
	// now emulates in the projection matrix (Set_DX8_ZBias above). Slope-scaled bias is a per-primitive
	// offset that the rasteriser applies from the surface gradient, so unlike the matrix emulation it can
	// be changed per draw call without rebuilding the projection, and it is what a shadow-map pass needs
	// to stop the shadow acne on shallow-angle geometry. Both are float render states (D3DRS_DEPTHBIAS
	// 195, D3DRS_SLOPESCALEDEPTHBIAS 175), which puts them inside the RenderStates[256] dirty cache, so
	// they travel through the same cached setter as every other state and get restored by
	// Apply_Default_State.
	static void Set_Depth_Bias(float bias);
	static void Set_Slope_Scale_Depth_Bias(float scale);
	static float Get_Depth_Bias() { return DepthBias; }
	static float Get_Slope_Scale_Depth_Bias() { return SlopeScaleDepthBias; }

	static void Set_Transform(D3DTRANSFORMSTATETYPE transform,const Matrix4x4& m);
	static void Set_Transform(D3DTRANSFORMSTATETYPE transform,const Matrix3D& m);
	static void Get_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& m);
	static void Set_World_Identity();
	static void Set_View_Identity();
	static bool Is_World_Identity();
	static bool Is_View_Identity();

	// Note that *_DX8_Transform() functions take the matrix in DX8 format - transposed from Westwood convention.

	static void _Set_DX8_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m);
	// GeneralsX @bugfix BenderAI 13/02/2026 Matrix4x4 for GLM compatibility (fighter19 pattern)
	static void _Get_DX8_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& m);

	static void Set_DX8_Light(int index,D3DLIGHT9* light);
	static void Set_DX8_Render_State(D3DRENDERSTATETYPE state, unsigned value);
	static void Set_DX8_Clip_Plane(DWORD Index, CONST float* pPlane);
	static void Set_DX8_Texture_Stage_State(unsigned stage, D3DTEXTURESTAGESTATETYPE state, unsigned value);
	// GeneralsX @build d3d9 port 11/10/2026 D3DSAMP_* values alias D3DTSS_* numerically in D3D9;
	// sampler states must reach the driver through SetSamplerState, so they get their own cache path.
	static void Set_DX8_Sampler_State(unsigned stage, D3DSAMPLERSTATETYPE state, unsigned value);
	static void Set_DX8_Texture(unsigned int stage, IDirect3DBaseTexture9* texture);
	static void Set_Light_Environment(LightEnvironmentClass* light_env);
	static LightEnvironmentClass* Get_Light_Environment() { return Light_Environment; }
	static void Set_Fog(bool enable, const Vector3 &color, float start, float end);

	static WWINLINE const D3DLIGHT9& Peek_Light(unsigned index);
	static WWINLINE bool Is_Light_Enabled(unsigned index);

	static bool Validate_Device();

	// Deferred

	static void Set_Shader(const ShaderClass& shader);
	static void Get_Shader(ShaderClass& shader);
	static void Set_Texture(unsigned stage,TextureBaseClass* texture);
	static void Set_Material(const VertexMaterialClass* material);
	static void Set_Light(unsigned index,const D3DLIGHT9* light);
	static void Set_Light(unsigned index,const LightClass &light);

	static void Apply_Render_State_Changes();	// Apply deferred render state changes (will be called automatically by Draw...)

	static void Draw_Triangles(
		unsigned buffer_type,
		unsigned short start_index,
		unsigned short polygon_count,
		unsigned short min_vertex_index,
		unsigned short vertex_count);
	static void Draw_Triangles(
		unsigned short start_index,
		unsigned short polygon_count,
		unsigned short min_vertex_index,
		unsigned short vertex_count);
	static void Draw_Strip(
		unsigned short start_index,
		unsigned short index_count,
		unsigned short min_vertex_index,
		unsigned short vertex_count);
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: DrawPrimitiveUP behind the facade, so the
	// Vulkan recorder sees the same shadow state as an indexed batch. With the path up the call is consumed
	// by Vulkan and no D3D9 draw is issued; otherwise it falls through to DX8CALL(DrawPrimitiveUP) and
	// returns false. Wedge 9.G added the verdict for callers that need to prove which side drew.
	static bool Draw_User_Primitive(
		unsigned primitive_type,
		unsigned primitive_count,
		const void* vertex_data,
		unsigned stride);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: indexed user-memory draw for the families
	// that own a raw D3D9 vertex/index buffer pair. The caller keeps its CPU mirror current as it fills the
	// D3D9 buffers; this hands the same ranges to the recorder and returns false when the batch has to stay
	// on D3D9 (offscreen target, untranslated program, unmirrored texture, unsupported layout). 'fvf' is the
	// declaration the raw draw was issued with, and 'base_vertex' is D3D9's BaseVertexIndex.
	static bool Draw_Indexed_User_Primitive(
		unsigned primitive_type,
		const void* vertex_data,
		unsigned vertex_count,
		unsigned stride,
		unsigned fvf,
		const void* index_data,
		unsigned index_count,
		unsigned base_vertex);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: DrawIndexedPrimitive issued against a raw
	// engine buffer pair that was bound behind the facade's back (the volumetric shadow pools). The
	// wrappers own mirrored backings, so the recorder can describe the batch without a CPU copy of the
	// geometry. Returns false when the batch must stay on D3D9; the caller then issues its own raw call
	// with the same arguments.
	static bool Draw_Indexed_Primitive_From_Buffers(
		void* vertex_buffer,
		void* index_buffer,
		unsigned primitive_type,
		unsigned first_index,
		unsigned polygon_count,
		unsigned min_vertex_index,
		unsigned vertex_count,
		unsigned base_vertex);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.G: opt a render-target texture into Vulkan
	// ownership. When every Set_Render_Target call to that texture publishes it to the recorder, the
	// batches drawn there also have to be consumable by the recorder; a family that still has raw device
	// draws must stay out of this list, which keeps its target switch invisible to Vulkan.
	static void VK_Allow_Render_Target(void* texture_key);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.G: allocate recorder mirrors for a render
	// target whose D3D9 resources were created raw (the sun shadow map) and opt them in. The colour key is
	// the D3D9 texture and its format is a D3DFORMAT; the depth key may be a raw depth surface that has no
	// texture container, so the map cannot be a Vulkan target without that key. Returns false when the
	// format is outside the mirror's table, which leaves the family on D3D9.
	static bool VK_Register_Render_Target(void* color_key, unsigned width, unsigned height,
		int d3d_color_format, void* depth_key, int ww3d_zformat);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.G: publish a target pair to the recorder
	// without touching DX8Wrapper's D3D9 RT cache. A pass that runs inside an already-open scene and
	// switches targets with raw device calls (the sun shadow occluder) must not use Set_Render_Target:
	// the wrapper owns exactly one custom-target slot and pillarbox already occupies it. Passing null
	// colour publishes the scene mirrors and restores recorder ownership of the scene target.
	// Returns false when the pair is not a registered, allowed mirror: the recorder then stays where it
	// was and refuses facade draws until the scene target is published again.
	static bool VK_Publish_Render_Target(void* color_surface, void* depth_surface);
	static void VK_Publish_Scene_Target();

	// GeneralsX @feature Qoder 19/10/2026 wedge 11 forensics: ask the recorder to copy the Vulkan mirror
	// behind this texture key to host staging at the next frame boundary and log its R-channel census
	// (the sun shadow map's "did the fill land" question). No-op unless GENERALSX_VKRTPROBE=1.
	static void VK_RT_Probe(void* texture_key);

	// GeneralsX @bugfix Qoder 18/10/2026 pure-Vulkan wedge 9: a post chain whose offscreen targets were
	// not opted in still binds D3D9 textures. The recorder is left on the scene mirror during that chain,
	// so without this gate its fixed-function copy quad is recorded into the scene with a source Vulkan
	// never wrote. Suspend owns the facade for the whole chain; publishing the scene restores it.
	static void VK_Suspend_Scene_Target();

	/*
	** Resources
	*/

	static IDirect3DVolumeTexture9* _Create_DX8_Volume_Texture
	(
		unsigned int width,
		unsigned int height,
		unsigned int depth,
		WW3DFormat format,
		MipCountType mip_level_count,
		D3DPOOL pool=D3DPOOL_MANAGED
	);

	static IDirect3DCubeTexture9* _Create_DX8_Cube_Texture
	(
		unsigned int width,
		unsigned int height,
		WW3DFormat format,
		MipCountType mip_level_count,
		D3DPOOL pool=D3DPOOL_MANAGED,
		bool rendertarget=false
	);


	static IDirect3DTexture9* _Create_DX8_ZTexture
	(
		unsigned int width,
		unsigned int height,
		WW3DZFormat zformat,
		MipCountType mip_level_count,
		D3DPOOL pool=D3DPOOL_MANAGED
	);


	static IDirect3DTexture9 * _Create_DX8_Texture
	(
		unsigned int width,
		unsigned int height,
		WW3DFormat format,
		MipCountType mip_level_count,
		D3DPOOL pool=D3DPOOL_MANAGED,
		bool rendertarget=false
	);
	static IDirect3DTexture9 * _Create_DX8_Texture(const char *filename, MipCountType mip_level_count);
	static IDirect3DTexture9 * _Create_DX8_Texture(IDirect3DSurface9 *surface, MipCountType mip_level_count);

	static IDirect3DSurface9 * _Create_DX8_Surface(unsigned int width, unsigned int height, WW3DFormat format);
	static IDirect3DSurface9 * _Create_DX8_Surface(const char *filename);
	static IDirect3DSurface9 * _Get_DX8_Front_Buffer();
	static SurfaceClass * _Get_DX8_Back_Buffer(unsigned int num=0);

	static void _Copy_DX8_Rects(
			IDirect3DSurface9* pSourceSurface,
			CONST RECT* pSourceRectsArray,
			UINT cRects,
			IDirect3DSurface9* pDestinationSurface,
			CONST POINT* pDestPointsArray
	);

	static void _Update_Texture(TextureClass *system, TextureClass *video);
	static void Flush_DX8_Resource_Manager(unsigned int bytes=0);
	static unsigned int Get_Free_Texture_RAM();

	static unsigned _Get_Main_Thread_ID() { return _MainThreadID; }
	static const D3DADAPTER_IDENTIFIER9& Get_Current_Adapter_Identifier() { return CurrentAdapterIdentifier; }

	/*
	** Statistics
	*/
	static void Begin_Statistics();
	static void End_Statistics();
	static const DX8FrameStatistics& Get_Last_Frame_Statistics();
	static unsigned long Get_FrameCount();
	static void Increment_DX8_CallCount() { DX8_RECORD_DX8_CALLS(); }

	// Needed by shader class
	static bool						Get_Fog_Enable() { return FogEnable; }
	static D3DCOLOR				Get_Fog_Color() { return FogColor; }

	// Utilities
	static Vector4 Convert_Color(unsigned color);
	static unsigned int Convert_Color(const Vector4& color);
	static unsigned int Convert_Color(const Vector3& color, const float alpha);
	static void Clamp_Color(Vector4& color);
	static unsigned int Convert_Color_Clamp(const Vector4& color);

	static void			  Set_Alpha (const float alpha, unsigned int &color);

	static void _Enable_Triangle_Draw(bool enable) { _EnableTriangleDraw=enable; }
	static bool _Is_Triangle_Draw_Enabled() { return _EnableTriangleDraw; }

	/*
	** Additional swap chain interface
	**
	**		Use this interface to render to multiple windows (in windowed mode).
	**	To render to an additional window, the sequence of calls should look
	**	something like this:
	**
	**	DX8Wrapper::Set_Render_Target (swap_chain_ptr);
	**
	**	WW3D::Begin_Render (true, true, Vector3 (0, 0, 0));
	**	WW3D::Render (scene, camera, FALSE, FALSE);
	**	WW3D::End_Render ();
	**
	**	swap_chain_ptr->Present (nullptr, nullptr, nullptr, nullptr);
	**
	**	DX8Wrapper::Set_Render_Target ((IDirect3DSurface9 *)nullptr);
	**
	*/
	static IDirect3DSwapChain9 *	Create_Additional_Swap_Chain (HWND render_window);

	/*
	** Render target interface. If render target format is WW3D_FORMAT_UNKNOWN, current display format is used.
	*/
	static TextureClass *	Create_Render_Target (int width, int height, WW3DFormat format = WW3D_FORMAT_UNKNOWN);

	static void					Set_Render_Target (IDirect3DSurface9 *render_target, bool use_default_depth_buffer = false);
	static void					Set_Render_Target (IDirect3DSurface9* render_target, IDirect3DSurface9* dpeth_buffer);

	static void					Set_Render_Target (IDirect3DSwapChain9 *swap_chain);
	static bool					Is_Render_To_Texture() { return IsRenderToTexture; }

	// Platform display size provider - set by SDL3 layer at init
	typedef bool (*DisplaySizeFunc)(int& outW, int& outH, float& outDensity);
	static void					Set_Display_Size_Provider(DisplaySizeFunc nativeSize, DisplaySizeFunc windowSize);
	static bool					GetNativeDisplaySize(int& outW, int& outH, float& outDensity);
	static bool					GetWindowSize(int& outW, int& outH, float& outDensity);

	// Pillarbox: render game at chosen resolution, blit centered onto backbuffer
	static bool					Pillarbox_Get_Rect(int& x, int& y, int& w, int& h);
	static void					Pillarbox_Begin();
	static void					Pillarbox_End();
	static void					Pillarbox_Process_Resize();

	// for depth map support KJM V
	static void Create_Render_Target
	(
		int width,
		int height,
		WW3DFormat format,
		WW3DZFormat zformat,
		TextureClass** target,
		ZTextureClass** depth_buffer
	);
	static void					Set_Render_Target_With_Z (TextureClass * texture, ZTextureClass* ztexture=nullptr);

	static void Set_Shadow_Map(int idx, ZTextureClass* ztex) { Shadow_Map[idx]=ztex; }
	static ZTextureClass* Get_Shadow_Map(int idx) { return Shadow_Map[idx]; }
	// for depth map support KJM ^

	// shader system updates KJM v
	static void Apply_Default_State();

	// GeneralsX @build d3d9 port 11/10/2026 D3D9 shader handles are interface pointers, so they are
	// carried as UINT_PTR (a DWORD would truncate x64 pointers). FVF and declaration paths are split
	// out because D3D9 removed the D3D8 combined FVF-as-shader convention.
	static void Set_Vertex_Shader(UINT_PTR vertex_shader);
	static void Set_Vertex_FVF(DWORD fvf);
	static void Set_Vertex_Declaration(IDirect3DVertexDeclaration9* decl);
	static void Set_Pixel_Shader(UINT_PTR pixel_shader);

	static void Set_Vertex_Shader_Constant(int reg, const void* data, int count);
	static void Set_Pixel_Shader_Constant(int reg, const void* data, int count);

	static DWORD Get_Vertex_Processing_Behavior() { return Vertex_Processing_Behavior; }

	// Needed by scene lighting class
	static void						Set_Ambient(const Vector3& color);
	static const Vector3&		Get_Ambient() { return Ambient_Color; }
	// shader system updates KJM ^




	static IDirect3DDevice9* _Get_D3D_Device8() { return D3DDevice; }
	static IDirect3D9* _Get_D3D8() { return D3DInterface; }
	/// Returns the display format - added by TR for video playback - not part of W3D
	static WW3DFormat	getBackBufferFormat();
	static bool Reset_Device(bool reload_assets=true);

	static const DX8Caps*	Get_Current_Caps() { WWASSERT(CurrentCaps); return CurrentCaps; }

	// GeneralsX @feature PS3.0 batch1 12/10/2026 addressable shader constant registers for this
	// device, and the refusal path for writes outside them. An out-of-range register would index
	// past Pixel_Shader_Constants/Vertex_Shader_Constants into the statics behind them, which is the
	// hardest class of corruption to trace, so the setters reject it in release builds too.
	static unsigned Get_Vertex_Shader_Constant_Limit() { return Vertex_Shader_Constant_Limit; }
	static unsigned Get_Pixel_Shader_Constant_Limit() { return Pixel_Shader_Constant_Limit; }
	static void Report_Shader_Constant_Range(const char* setter,int reg,int count);

	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0 (pure-Vulkan port): render-state census.
	// The port has to turn this facade's lazy state replay (Apply_Render_State_Changes) into a bounded
	// set of Vulkan pipeline state objects, and the only honest way to size that set is to count what
	// the running game actually asks for. With GENERALSX_VKCENSUS=1 every draw digests the shadow
	// state into two keys: one that ignores texture stage / sampler state (the PSO key, once stage
	// state moves into an interpreted pixel shader) and one that includes it. Both are histogrammed,
	// along with the FVF set, the light-enable masks, the fixed-function vs programmable split, the
	// combinator op/arg sets and the render-state/stage-state/sampler token sets a translator must
	// cover. Off, the cost is the touched-token stores below (one byte store per setter call).
	// Periodic + shutdown dumps land in Vulkan/artifacts/census.csv with a stderr gate line.
	static void VK_Census_Draw(unsigned prim_type, unsigned short polys, unsigned short verts);
	static void VK_Census_Dump(const char* reason);
	static bool VK_Census_On();

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.C: the facade-side submit filler. With the
	// draw path up (GENERALSX_VKDRAW=1) the indexed-draw sites hand it the batch and this turns the shadow
	// state (FVF, render states, constants, buffers) into a VKDraw_Submit and records it. It returns true
	// when the batch was consumed by Vulkan - the caller then skips DX8CALL(DrawIndexedPrimitive) - and
	// false while the path is off or the batch cannot be backed, in which case the D3D9 call still runs.
	// 'first_index' and 'base_vertex' are already D3D's StartIndex(+iba_offset) and BaseVertexIndex, so the
	// vertex/index buffer offsets fold into those and the streams bind at the raw backing offset.
	static bool VK_Emit_Indexed_Draw(void* vb, void* ib, unsigned primitive_type,
		unsigned short first_index, unsigned short polygon_count,
		unsigned short min_vertex_index, unsigned short vertex_count, unsigned long long base_vertex);

	// GeneralsX @refactor Qoder 18/10/2026 pure-Vulkan wedge 9: the shadow-to-submit translation both draw
	// paths share. It fills everything except the vertex/index source, which is what differs between an
	// indexed batch and a user-memory one. 'ff_state' is the caller's storage for the packed fixed-function
	// block the submit points at, so it has to outlive the submit call.
	static void VK_Fill_Submit_State(VKDraw_Submit& submit, VKDraw_FF_State& ff_state,
		const FVFInfoClass& info, unsigned vs_tag, unsigned ps_tag, unsigned primitive_type);

	// GeneralsX @bugfix Qoder 19/10/2026 R-02 forensics: the untextured fixed-function submit census, run
	// from the draw sites once the submit carries its real stream binding. Names the passes that paint a
	// vertex-diffuse silhouette, resolves the material sources and alpha-test block, and samples the
	// batch's own first DIFFUSE dwords - the field the white-silhouette attribution turns on.
	static void VK_FF_NTX_Census(VKDraw_Submit& submit, const FVFInfoClass& info,
		unsigned primitive_type, unsigned ps_tag, void* vertex_buffer, const void* vertex_data);

	// GeneralsX @debug Qoder 19/10/2026 BUG-02: the screen-space batch stack census. One line per
	// XYZRHW submit (bounding box, texture, recipe, vertex colours) for a few named frames, so a black
	// rectangle on screen is attributed to the last batch whose box covers it. Env gate GENERALSX_VKBAR.
	static void VK_BAR_Census(VKDraw_Submit& submit, const FVFInfoClass& info,
		unsigned primitive_type, void* vertex_buffer, const void* vertex_data, unsigned vertex_count);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: the user-primitive emitter. It refuses the
	// batches this layer cannot describe - a render target the facade is not publishing, stencil state, a
	// bound texture without a Vulkan view, an untranslated program - by returning false, which makes the
	// caller run the D3D9 call instead.
	static bool VK_Emit_User_Primitive(unsigned primitive_type, const void* vertex_data,
		unsigned stride, unsigned polygon_count);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: indexed twin for raw-buffer families. Same
	// refusal contract as VK_Emit_User_Primitive; geometry is copied from the caller's CPU mirror.
	static bool VK_Emit_Indexed_User_Primitive(unsigned primitive_type,
		const void* vertex_data, unsigned vertex_count, unsigned stride, unsigned fvf,
		const void* index_data, unsigned index_count, unsigned base_vertex);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.C: the viewport shadow the submit filler
	// binds per draw. Vulkan has no device viewport state - the recorder is handed a rectangle with the
	// batch - so Set_Viewport has to publish it here as well as to D3D9 while both paths are alive.
	static void VK_Shadow_Viewport(const D3DVIEWPORT9* viewport);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: read one shadowed render state as the value
	// the device would have used. Invalidate_Cached_Render_States fills RenderStates with the 0x12345678
	// sentinel, and a slot the game never wrote after that has no shadow at all - which is not zero. See
	// the implementation for the measurement that makes this more than a tidy-up.
	static unsigned VK_Read_RS(unsigned token);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: the two reads the same sentinel logic needs
	// for the states that are not plain ordinals - a float carried as its own bit pattern, and a texture
	// stage register whose documented default is not zero.
	static unsigned VK_Read_RS_Default_bits(unsigned token, float fallback);
	static unsigned VK_Read_TSS(unsigned stage, unsigned token, unsigned fallback);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7: the sampler shadow read the pixel side needs.
	// The filter and address tokens arrive here as the D3D8 ordinals the game wrote - the same numbers the
	// census put in csv - so this only answers the sentinel question; decoding them is vkdesc_make_sampler's
	// job and it decodes D3D8 ordinals, nothing else.
	static unsigned VK_Read_Sampler(unsigned stage, unsigned token, unsigned fallback);

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: pack the fixed-function vertex-stage block
	// (VKDraw_FF_State, the third dynamic uniform of every submit) out of the facade's shadows for one
	// batch: the three matrices with the ZBias emulation the D3D path applies, the material the engine
	// last installed, the eight light slots, fog, the material colour sources and the per-stage texture
	// coordinate generators. A member because the state it reads is protected here.
	static void VK_FF_Pack(VKDraw_FF_State* out, const FVFInfoClass& info);

	static bool Registry_Save_Render_Device( const char * sub_key );
	static bool Registry_Load_Render_Device( const char * sub_key, bool resize_window );

	static const char* Get_DX8_Render_State_Name(D3DRENDERSTATETYPE state);
	static const char* Get_DX8_Texture_Stage_State_Name(D3DTEXTURESTAGESTATETYPE state);
	// GeneralsX @build d3d9 port 11/10/2026 - sampler state name tables (D3DSAMP_* split out of D3DTSS_* in D3D9)
	static const char* Get_DX8_Sampler_State_Name(D3DSAMPLERSTATETYPE state);
	static unsigned Get_DX8_Render_State(D3DRENDERSTATETYPE state) { return RenderStates[state]; }

	// Names of the specific values of render states and texture stage states
	static void Get_DX8_Texture_Stage_State_Value_Name(StringClass& name, D3DTEXTURESTAGESTATETYPE state, unsigned value);
	static void Get_DX8_Sampler_State_Value_Name(StringClass& name, D3DSAMPLERSTATETYPE state, unsigned value);
	static void Get_DX8_Render_State_Value_Name(StringClass& name, D3DRENDERSTATETYPE state, unsigned value);

	static const char* Get_DX8_Texture_Address_Name(unsigned value);
	static const char* Get_DX8_Texture_Filter_Name(unsigned value);
	static const char* Get_DX8_Texture_Arg_Name(unsigned value);
	static const char* Get_DX8_Texture_Op_Name(unsigned value);
	static const char* Get_DX8_Texture_Transform_Flag_Name(unsigned value);
	static const char* Get_DX8_ZBuffer_Type_Name(unsigned value);
	static const char* Get_DX8_Fill_Mode_Name(unsigned value);
	static const char* Get_DX8_Shade_Mode_Name(unsigned value);
	static const char* Get_DX8_Blend_Name(unsigned value);
	static const char* Get_DX8_Cull_Mode_Name(unsigned value);
	static const char* Get_DX8_Cmp_Func_Name(unsigned value);
	static const char* Get_DX8_Fog_Mode_Name(unsigned value);
	static const char* Get_DX8_Stencil_Op_Name(unsigned value);
	static const char* Get_DX8_Material_Source_Name(unsigned value);
	static const char* Get_DX8_Vertex_Blend_Flag_Name(unsigned value);
	static const char* Get_DX8_Patch_Edge_Style_Name(unsigned value);
	static const char* Get_DX8_Debug_Monitor_Token_Name(unsigned value);
	static const char* Get_DX8_Blend_Op_Name(unsigned value);

	static void Invalidate_Cached_Render_States();
	static void Set_Transform_Dirty();
	// GeneralsX @refactor PS3.0-P3-33 14/10/2026 Set_Draw_Polygon_Low_Bound_Limit removed with its
	// dead DrawPolygonLowBoundLimit cull (no caller ever set the limit).

	// GeneralsX @feature PS3.0-P3-25a 14/10/2026 creation-time present interval (0..3) injected by
	// the device layer from Options.ini VSync (or the GENERALSX_SWAPINTERVAL env). Consumed once in
	// device creation, before any device exists, so unlike Set_Swap_Interval it never resets.
	static void	Set_User_Present_Interval(int swap);

protected:

	static bool	Create_Device();
	static void Release_Device();

	static void Reset_Statistics();
	static void Enumerate_Devices();
	static void Set_Default_Global_Render_States();

	/*
	** Device Selection Code.
	** For backward compatibility, the public interface for these functions is in the ww3d.
	** header file.  These functions are protected so that we aren't exposing two interfaces.
	*/
	static bool Set_Any_Render_Device();
	static bool	Set_Render_Device(const char * dev_name,int width=-1,int height=-1,int bits=-1,int windowed=-1,bool resize_window=false);
	static bool	Set_Render_Device(int dev=-1,int resx=-1,int resy=-1,int bits=-1,int windowed=-1,bool resize_window = false, bool reset_device = false, bool restore_assets=true);
	static bool Set_Next_Render_Device();
	static bool Toggle_Windowed();

	static int	Get_Render_Device_Count();
	static int	Get_Render_Device();
	static const RenderDeviceDescClass & Get_Render_Device_Desc(int deviceidx);
	static const char * Get_Render_Device_Name(int device_index);
	static bool Set_Device_Resolution(int width=-1,int height=-1,int bits=-1,int windowed=-1, bool resize_window=false);
	static void Get_Device_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed);
	static void Get_Render_Target_Resolution(int & set_w,int & set_h,int & set_bits,bool & set_windowed);
	static int	Get_Device_Resolution_Width() { return ResolutionWidth; }
	static int	Get_Device_Resolution_Height() { return ResolutionHeight; }

	static bool Registry_Save_Render_Device( const char *sub_key, int device, int width, int height, int depth, bool windowed, int texture_depth);
	static bool Registry_Load_Render_Device( const char * sub_key, char *device, int device_len, int &width, int &height, int &depth, int &windowed, int &texture_depth);
	static bool Is_Windowed() { return IsWindowed; }

	static void	Set_Texture_Bitdepth(int depth)	{ WWASSERT(depth==16 || depth==32); TextureBitDepth = depth; }
	static int	Get_Texture_Bitdepth()			{ return TextureBitDepth; }

	static void Set_MSAA_Mode(D3DMULTISAMPLE_TYPE mode) { MultiSampleAntiAliasing = mode; }
	static D3DMULTISAMPLE_TYPE Get_MSAA_Mode() { return MultiSampleAntiAliasing; }

	static void	Set_Swap_Interval(int swap);
	static int	Get_Swap_Interval();
	static void Set_Polygon_Mode(int mode);

	/*
	** Internal functions
	*/
	static void Resize_And_Position_Window();
	static bool Find_Color_And_Z_Mode(int resx,int resy,int bitdepth,D3DFORMAT * set_colorbuffer,D3DFORMAT * set_backbuffer, D3DFORMAT * set_zmode);
	static bool Find_Color_Mode(D3DFORMAT colorbuffer, int resx, int resy, UINT *mode);
	static bool Find_Z_Mode(D3DFORMAT colorbuffer,D3DFORMAT backbuffer, D3DFORMAT *zmode);
	static bool Test_Z_Mode(D3DFORMAT colorbuffer,D3DFORMAT backbuffer, D3DFORMAT zmode);
	static void Compute_Caps(WW3DFormat display_format);

	/*
	** Protected Member Variables
	*/

	static DX8_CleanupHook *m_pCleanupHook;

	static RenderStateStruct			render_state;
	static unsigned						render_state_changed;
	// GeneralsX @refactor BenderAI 10/02/2026 - fighter19 pattern Matrix4x4 not D3DMATRIX
	static Matrix4x4						DX8Transforms[D3DTS_WORLD+1];

	static bool								IsInitted;
	static bool								IsDeviceLost;
	static void *							Hwnd;
	static unsigned						_MainThreadID;

	static bool								_EnableTriangleDraw;

	static int								CurRenderDevice;
	static int								ResolutionWidth;
	static int								ResolutionHeight;
	static int								BitDepth;
	static int								TextureBitDepth;
	static bool								IsWindowed;
	static D3DFORMAT					DisplayFormat;
	static D3DMULTISAMPLE_TYPE	MultiSampleAntiAliasing;

	static D3DMATRIX						old_world;
	static D3DMATRIX						old_view;
	static D3DMATRIX						old_prj;

	// shader system updates KJM v
	// GeneralsX @build d3d9 port 11/10/2026 handles are D3D9 interface pointers stored losslessly
	static UINT_PTR						Vertex_Shader;
	static UINT_PTR						Pixel_Shader;
	static IDirect3DVertexDeclaration9*	Vertex_Declaration;
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0/5: Set_Vertex_FVF used to forward straight to
	// SetFVF, so the facade never knew the current vertex format. The Vulkan PSO key needs it (FVF is
	// the vertex input layout there, not a device state), and so does the census, so mirror it here.
	static DWORD							Vertex_Format;

	static Vector4							Vertex_Shader_Constants[MAX_VERTEX_SHADER_CONSTANTS];
	static Vector4							Pixel_Shader_Constants[MAX_PIXEL_SHADER_CONSTANTS];
	// GeneralsX @feature PS3.0 batch1 12/10/2026 registers the caches above that this device may
	// actually be addressed with; recomputed by Compute_Caps() whenever caps are rebuilt.
	static unsigned							Vertex_Shader_Constant_Limit;
	static unsigned							Pixel_Shader_Constant_Limit;

	static LightEnvironmentClass*		Light_Environment;
	static RenderInfoClass*				Render_Info;

	static DWORD							Vertex_Processing_Behavior;

	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: shadow of the last material handed to
	// Set_DX8_Material. Written unconditionally (a 68-byte copy per material change, against a facade that
	// already logs thousands of state calls per frame) so the non-Vulkan builds keep identical behaviour -
	// nothing reads it there.
	static D3DMATERIAL9					Material_Shadow;
	static bool								Material_Shadow_Valid;

	static ZTextureClass*				Shadow_Map[MAX_SHADOW_MAPS];

	static Vector3							Ambient_Color;
	// shader system updates KJM ^

	static bool								world_identity;
	static unsigned						RenderStates[256];
	// GeneralsX @build d3d9 port 11/10/2026 D3DTSS_CONSTANT=32 needs index 32; sampler states cached separately
	static unsigned						TextureStageStates[MAX_TEXTURE_STAGES][33];
	static unsigned						SamplerStates[MAX_TEXTURE_STAGES][16];
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0: which token slots the game ever writes. The
	// census digests only these instead of all 256*33*16 slots, and the translated set is exactly what
	// wedge 5's state table and wedge 7's interpreted pixel shader have to cover.
	static unsigned char				VK_RS_Touched[256];
	static unsigned char				VK_TSS_Touched[33];
	static unsigned char				VK_Samp_Touched[16];

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @debug Qoder 19/10/2026 BUG-03: watch-ring for ALPHABLENDENABLE/COLORWRITEENABLE/
	// STENCILENABLE writes - see the push in Set_DX8_Render_State and the replay in the red census.
	struct VK_RS_Watch_Entry {
		unsigned long long frame;
		unsigned state;
		unsigned value;
		void *ret;
	};
	static VK_RS_Watch_Entry			VK_RS_Watch[64];
	static unsigned						VK_RS_Watch_N;
	static void VK_RS_Watch_Push(unsigned state, unsigned value, void *ret);
#endif

	static IDirect3DBaseTexture9 *	Textures[MAX_TEXTURE_STAGES];

	// These fog settings are constant for all objects in a given scene,
	// unlike the matching renderstates which vary based on shader settings.
	static bool								FogEnable;
	static D3DCOLOR						FogColor;

	static DX8FrameStatistics			FrameStatistics;
	static bool								CurrentDX8LightEnables[8];	// GeneralsX @feature PS3.0-P3-18 14/10/2026: 4 -> 8 (light units)

	static unsigned long FrameCount;

	static DX8Caps*						CurrentCaps;

	static D3DADAPTER_IDENTIFIER9		CurrentAdapterIdentifier;

	static IDirect3D9 *					D3DInterface;			//d3d8;
	static IDirect3DDevice9 *			D3DDevice;				//d3ddevice8;

	static IDirect3DSurface9 *			CurrentRenderTarget;
	static IDirect3DSurface9 *			CurrentDepthBuffer;
	static IDirect3DSurface9 *			DefaultRenderTarget;
	static IDirect3DSurface9 *			DefaultDepthBuffer;
	// GeneralsX @refactor PS3.0-P3-33 14/10/2026 DrawPolygonLowBoundLimit static removed (dead batch-disable hack).

	static bool								IsRenderToTexture;

	// Display size providers (set by SDL3 layer, called by pillarbox)
	static DisplaySizeFunc				s_getNativeDisplaySize;
	static DisplaySizeFunc				s_getWindowSize;

	// Pillarbox state
	static bool								s_pillarboxEnabled;
	static bool								s_pillarboxActive;
	static int								s_bbW, s_bbH;
	static int								s_dstX, s_dstY, s_dstW, s_dstH;
	static float							s_pixelDensity;
	static IDirect3DTexture9*			s_offscreenTex;
	static IDirect3DSurface9*			s_offscreenSurf;
	static IDirect3DSurface9*			s_depthSurf;
	static IDirect3DSurface9*			s_savedBackbuffer;
	static IDirect3DSurface9*			s_savedDepth;

	static bool								Pillarbox_Setup(int gameW, int gameH);
	static void								Pillarbox_Cleanup();

	static int								ZBias;
	// GeneralsX @feature PS3.0 batch7 13/10/2026 fed by Set_Depth_Bias / Set_Slope_Scale_Depth_Bias
	static float							DepthBias;
	static float							SlopeScaleDepthBias;
	static float							ZNear;
	static float							ZFar;
	// GeneralsX @refactor BenderAI 10/02/2026
	// Changed from D3DMATRIX to Matrix4x4 - fighter19 pattern
	static Matrix4x4					ProjectionMatrix;

	friend void DX8_Assert();
	friend class WW3D;
	friend class DX8IndexBufferClass;
	friend class DX8VertexBufferClass;
};

// shader system updates KJM v
// GeneralsX @build d3d9 port 11/10/2026 - D3D9 splits D3D8's combined FVF/shader handle:
// Set_Vertex_Shader now takes an IDirect3DVertexShader9* (stored as UINT_PTR), while fixed
// function vertex formats go through Set_Vertex_FVF (SetFVF). Callers that previously passed
// FVF constants must switch to Set_Vertex_FVF.
WWINLINE void DX8Wrapper::Set_Vertex_Shader(UINT_PTR vertex_shader)
{
	Vertex_Shader=vertex_shader;
	DX8CALL(SetVertexShader(reinterpret_cast<IDirect3DVertexShader9*>(vertex_shader)));
}

// GeneralsX @build d3d9 port 12/10/2026 - measured contract (native d3d9 and DXVK 2.6 agree; the
// standalone setfvfprobe tool in the port-notes folder records it): SetFVF does NOT unbind a custom
// vertex shader the way D3D8's single FVF-or-shader handle implied. D3D9 keeps the vertex format and
// the vertex shader as two orthogonal states, so a custom shader stays active until
// Set_Vertex_Shader(0) is called.
// Fixed-function batches are unaffected because a FVF-bound shader only matters while a custom
// shader is explicitly installed - and every path that installs one (dot3, trees, sea waves) must
// therefore unbind it in its own restore pair. Clearing here instead would strip the dot3 vertex
// shader mid-batch: Apply_Render_State_Changes re-asserts the buffer FVF on every draw
// (dx8wrapper.cpp:2740), which is also why W3DTreeBuffer.cpp:1722 rebinds after applying state.
WWINLINE void DX8Wrapper::Set_Vertex_FVF(DWORD fvf)
{
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0/5: mirror the format so the facade knows it.
	Vertex_Format=fvf;
	DX8CALL(SetFVF(fvf));
}

WWINLINE void DX8Wrapper::Set_Vertex_Declaration(IDirect3DVertexDeclaration9* decl)
{
	if (Vertex_Declaration==decl) return;
	Vertex_Declaration=decl;
	DX8CALL(SetVertexDeclaration(decl));
}

WWINLINE void DX8Wrapper::Set_Pixel_Shader(UINT_PTR pixel_shader)
{
	// GeneralsX @bugfix PS3.0 batch1 12/10/2026 the old "same handle, nothing to do" fast path is not
	// safe in D3D9: IDirect3DPixelShader9* addresses get recycled, and after a Reset the device holds
	// no shader while the cache still names the old pointer - the fast path then skips a binding the
	// frame really needs. SetPixelShader with an unchanged pointer is cheap, so always forward it.
	Pixel_Shader=pixel_shader;
	DX8CALL(SetPixelShader(reinterpret_cast<IDirect3DPixelShader9*>(pixel_shader)));
}

// GeneralsX @build d3d9 port 11/10/2026 - DXVK upstream confirms D3D8's ConstantCount here is
// already a Vector4 count, identical to SetVertexShaderConstantF/SetPixelShaderConstantF semantics.
WWINLINE void DX8Wrapper::Set_Vertex_Shader_Constant(int reg, const void* data, int count)
{
	// GeneralsX @feature PS3.0 batch1 12/10/2026 budget guard (see Get_*_Shader_Constant_Limit)
	if (reg < 0 || count <= 0 || (unsigned)(reg + count) > Get_Vertex_Shader_Constant_Limit()) {
		Report_Shader_Constant_Range("Set_Vertex_Shader_Constant",reg,count);
		return;
	}

	int memsize=sizeof(Vector4)*count;

	// may be incorrect if shaders are created and destroyed dynamically
	if (memcmp(data, &Vertex_Shader_Constants[reg],memsize)==0) return;

	memcpy(&Vertex_Shader_Constants[reg],data,memsize);
	DX8CALL(SetVertexShaderConstantF(reg,static_cast<const float*>(data),count));
}

WWINLINE void DX8Wrapper::Set_Pixel_Shader_Constant(int reg, const void* data, int count)
{
	// GeneralsX @feature PS3.0 batch1 12/10/2026 budget guard (see Get_*_Shader_Constant_Limit)
	if (reg < 0 || count <= 0 || (unsigned)(reg + count) > Get_Pixel_Shader_Constant_Limit()) {
		Report_Shader_Constant_Range("Set_Pixel_Shader_Constant",reg,count);
		return;
	}

	int memsize=sizeof(Vector4)*count;

	// may be incorrect if shaders are created and destroyed dynamically
	if (memcmp(data, &Pixel_Shader_Constants[reg],memsize)==0) return;

	memcpy(&Pixel_Shader_Constants[reg],data,memsize);
	DX8CALL(SetPixelShaderConstantF(reg,static_cast<const float*>(data),count));
}
// shader system updates KJM ^
// GeneralsX @refactor BenderAI 10/02/2026 - fighter19 pattern Matrix4x4 parameter not D3DMATRIX
WWINLINE void DX8Wrapper::_Set_DX8_Transform(D3DTRANSFORMSTATETYPE transform, const Matrix4x4& m)
{
	WWASSERT(transform<=D3DTS_WORLD);
#if 0 // (gth) this optimization is breaking generals because they set the transform behind our backs.
	if (mtx!=DX8Transforms[transform])
#endif
	{
		DX8Transforms[transform]=m;
		SNAPSHOT_SAY(("DX8 - SetTransform %d [%f,%f,%f,%f][%f,%f,%f,%f][%f,%f,%f,%f][%f,%f,%f,%f]",
			transform,
			m[0][0],m[0][1],m[0][2],m[0][3],
			m[1][0],m[1][1],m[1][2],m[1][3],
			m[2][0],m[2][1],m[2][2],m[2][3],
			m[3][0],m[3][1],m[3][2],m[3][3]));
		DX8_RECORD_MATRIX_CHANGE();
		DX8CALL(SetTransform(transform,(D3DMATRIX*)&m));
	}
}

// GeneralsX @bugfix BenderAI 13/02/2026 Matrix4x4 for GLM compatibility (fighter19 pattern)
WWINLINE void DX8Wrapper::_Get_DX8_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& m)
{
	DX8CALL(GetTransform(transform,(D3DMATRIX*)&m));
}

// ----------------------------------------------------------------------------
//
// Set the index offset for the current index buffer
//
// ----------------------------------------------------------------------------

WWINLINE void DX8Wrapper::Set_Index_Buffer_Index_Offset(unsigned offset)
{
	if (render_state.index_base_offset==offset) return;
	render_state.index_base_offset=offset;
	render_state_changed|=INDEX_BUFFER_CHANGED;
}

// ----------------------------------------------------------------------------
// Set the fog settings. This function should be used, rather than setting the
// appropriate renderstates directly, because the shader sets some of the
// renderstates on a per-mesh / per-pass basis depending on global fog states
// (stored in the wrapper) as well as the shader settings.
// This function should be called rarely - once per scene would be appropriate.
// ----------------------------------------------------------------------------

WWINLINE void DX8Wrapper::Set_Fog(bool enable, const Vector3 &color, float start, float end)
{
	// Set global states
	FogEnable = enable;
	FogColor = Convert_Color(color,0.0f);

	// Invalidate the current shader (since the renderstates set by the shader
	// depend on the global fog settings as well as the actual shader settings)
	ShaderClass::Invalidate();

	// Set renderstates which are not affected by the shader
	Set_DX8_Render_State(D3DRS_FOGSTART, *(DWORD *)(&start));
	Set_DX8_Render_State(D3DRS_FOGEND,   *(DWORD *)(&end));
}


WWINLINE void DX8Wrapper::Set_Ambient(const Vector3& color)
{
	Ambient_Color=color;
	Set_DX8_Render_State(D3DRS_AMBIENT, DX8Wrapper::Convert_Color(color,0.0f));
}

// ----------------------------------------------------------------------------
//
// Set vertex buffer to be used in the subsequent render calls. If there was
// a vertex buffer being used earlier, release the reference to it. Passing
// nullptr just will release the vertex buffer.
//
// ----------------------------------------------------------------------------

WWINLINE void DX8Wrapper::Set_DX8_Material(const D3DMATERIAL9* mat)
{
	DX8_RECORD_MATERIAL_CHANGE();
	WWASSERT(mat);
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 6: mirror the material for the Vulkan
	// fixed-function vertex stage before handing it to D3D, because this is the only place the W3D
	// material's resolved D3DMATERIAL9 ever exists in one piece.
	Material_Shadow = *mat;
	Material_Shadow_Valid = true;
	SNAPSHOT_SAY(("DX8 - SetMaterial"));
	DX8CALL(SetMaterial(mat));
}

WWINLINE const D3DMATERIAL9& DX8Wrapper::Peek_DX8_Material()
{
	return Material_Shadow;
}

WWINLINE void DX8Wrapper::Set_DX8_Light(int index, D3DLIGHT9* light)
{
	if (light) {
		DX8_RECORD_LIGHT_CHANGE();
		DX8CALL(SetLight(index,light));
		DX8CALL(LightEnable(index,TRUE));
		CurrentDX8LightEnables[index]=true;
		SNAPSHOT_SAY(("DX8 - SetLight %d",index));
	}
	else if (CurrentDX8LightEnables[index]) {
		DX8_RECORD_LIGHT_CHANGE();
		CurrentDX8LightEnables[index]=false;
		DX8CALL(LightEnable(index,FALSE));
		SNAPSHOT_SAY(("DX8 - DisableLight %d",index));
	}
}

WWINLINE void DX8Wrapper::Set_DX8_Render_State(D3DRENDERSTATETYPE state, unsigned value)
{
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0: mark the slot before the dedupe return so the
	// census sees every token the game tries to write, not just the ones that changed.
	VK_RS_Touched[state]=1;

	// Can't monitor state changes because setShader call to GERD may change the states!
	if (RenderStates[state]==value) return;

#ifdef DEBUG_LOGGING
	if (WW3D::Is_Snapshot_Activated()) {
		StringClass value_name(0,true);
		Get_DX8_Render_State_Value_Name(value_name,state,value);
		SNAPSHOT_SAY(("DX8 - SetRenderState(state: %s, value: %s)",
			Get_DX8_Render_State_Name(state),
			value_name.str()));
	}
#endif

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @debug Qoder 19/10/2026 BUG-03: ring of every accepted write to the three states that
	// decide whether an offscreen-looking pass paints to the screen (blend fake, colour mask, stencil).
	// The red-batch census dump replays the tail of this ring with the submitter's return address.
	if (state == D3DRS_ALPHABLENDENABLE || state == D3DRS_COLORWRITEENABLE || state == D3DRS_STENCILENABLE) {
		VK_RS_Watch_Push((unsigned)state, value, _ReturnAddress());
	}
#endif

	RenderStates[state]=value;
	DX8CALL(SetRenderState( state, value ));
	DX8_RECORD_RENDER_STATE_CHANGE();
}

WWINLINE void DX8Wrapper::Set_DX8_Clip_Plane(DWORD Index, CONST float* pPlane)
{
	DX8CALL(SetClipPlane( Index, pPlane ));
}

WWINLINE void DX8Wrapper::Set_DX8_Texture_Stage_State(unsigned stage, D3DTEXTURESTAGESTATETYPE state, unsigned value)
{
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0: mark the slot before the early-out so a value
	// that was already correct still shows up in the census token set. D3DTSS ordinals start at 1 and
	// D3DTSS_CONSTANT is 32, matching the 33-wide cache.
	if (state > 0 && state < 33) VK_TSS_Touched[state]=1;

  	if (stage >= MAX_TEXTURE_STAGES)
  	{	DX8CALL(SetTextureStageState( stage, state, value ));
  		return;
  	}

	// Can't monitor state changes because setShader call to GERD may change the states!
	if (TextureStageStates[stage][(unsigned int)state]==value) return;

#ifdef DEBUG_LOGGING
	if (WW3D::Is_Snapshot_Activated()) {
		StringClass value_name(0,true);
		Get_DX8_Texture_Stage_State_Value_Name(value_name,state,value);
		SNAPSHOT_SAY(("DX8 - SetTextureStageState(stage: %d, state: %s, value: %s)",
			stage,
			Get_DX8_Texture_Stage_State_Name(state),
			value_name.str()));
	}
#endif

	TextureStageStates[stage][(unsigned int)state]=value;
	DX8CALL(SetTextureStageState( stage, state, value ));
	DX8_RECORD_TEXTURE_STAGE_STATE_CHANGE();
}

// GeneralsX @build d3d9 port 11/10/2026 - D3D9 sampler states (filters, addresses, LOD bias) are a
// separate device API with their own numeric space aliasing D3DTSS_*; route them to SetSamplerState.
WWINLINE void DX8Wrapper::Set_DX8_Sampler_State(unsigned stage, D3DSAMPLERSTATETYPE state, unsigned value)
{
	// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 0: sampler slots feed the immutable-sampler LUT
	// the Vulkan port needs, so record which D3DSAMP_* tokens the game writes at all.
	if (state > 0 && state < 16) VK_Samp_Touched[state]=1;

  	if (stage >= MAX_TEXTURE_STAGES)
  	{	DX8CALL(SetSamplerState( stage, state, value ));
  		return;
  	}

	if (SamplerStates[stage][(unsigned int)state]==value) return;

#ifdef DEBUG_LOGGING
	if (WW3D::Is_Snapshot_Activated()) {
		SNAPSHOT_SAY(("DX8 - SetSamplerState(stage: %d, state: %d, value: %d)", stage, (int)state, value));
	}
#endif

	SamplerStates[stage][(unsigned int)state]=value;
	DX8CALL(SetSamplerState( stage, state, value ));
	DX8_RECORD_TEXTURE_STAGE_STATE_CHANGE();
}

WWINLINE void DX8Wrapper::Set_DX8_Texture(unsigned int stage, IDirect3DBaseTexture9* texture)
{
  	if (stage >= MAX_TEXTURE_STAGES)
  	{	DX8CALL(SetTexture(stage, texture));
  		return;
  	}

	if (Textures[stage]==texture) return;

	SNAPSHOT_SAY(("DX8 - SetTexture(%x) ",texture));

	if (Textures[stage]) Textures[stage]->Release();
	Textures[stage] = texture;
	if (Textures[stage]) Textures[stage]->AddRef();
	DX8CALL(SetTexture(stage, texture));
	DX8_RECORD_TEXTURE_CHANGE();
}

// GeneralsX @build d3d9 port 11/10/2026 - D3D9 removed CopyRects. Full-surface copies of render
// targets/back buffers use GetRenderTargetData (the destination must then be a system-memory
// plain surface); rect-list copies go through UpdateSurface.
// GeneralsX @bugfix d3d9 port 12/10/2026 System-memory sources (font glyph atlas surfaces in
// Render2DSentenceClass) are copied via LockRect/staging-texture paths: DXVK's
// GetRenderTargetData access-violates when the source is not a render target.
// GeneralsX @bugfix d3d9 port 12/10/2026 Real pixel size of a locked rectangle. The callers of
// _Copy_DX8_Rects hand over 1- and 2-byte formats too (font glyph atlases are L8/A8 class, W3DSmudge
// and the shroud use A4R4G4B4), so the previous "A4R4G4B4 or 4 bytes" guess read several times further
// than the source row and faulted inside memcpy while the shell map was loading. Block-compressed,
// paletted-depth and unknown formats report 0, which makes the copy sites refuse the CPU path instead
// of inventing a stride.
WWINLINE UINT DX8_Format_Bytes_Per_Pixel(D3DFORMAT format)
{
	switch (format) {
	case D3DFMT_A8R8G8B8:
	case D3DFMT_X8R8G8B8:
	case D3DFMT_A8B8G8R8:
	case D3DFMT_X8B8G8R8:
	case D3DFMT_A2B10G10R10:
	case D3DFMT_A2R10G10B10:
	case D3DFMT_X8L8V8U8:
	case D3DFMT_A16B16G16R16:
		return 4;
	case D3DFMT_R8G8B8:
		return 3;
	case D3DFMT_R5G6B5:
	case D3DFMT_X1R5G5B5:
	case D3DFMT_A1R5G5B5:
	case D3DFMT_A4R4G4B4:
	case D3DFMT_X4R4G4B4:
	case D3DFMT_A8R3G3B2:
	case D3DFMT_A8P8:
	case D3DFMT_A8L8:
	case D3DFMT_V8U8:
	case D3DFMT_L6V5U5:
	case D3DFMT_G16R16:
		return 2;
	case D3DFMT_L8:
	case D3DFMT_A8:
	case D3DFMT_P8:
	case D3DFMT_A4L4:
	case D3DFMT_R3G3B2:
		return 1;
	default:
		return 0;
	}
}

// GeneralsX @bugfix d3d9 port 12/10/2026 Block size of a block-compressed format, 0 when the
// format is not compressed. DX8 CopyRects accepted same-format DXT copies (the device just moved
// 4x4 blocks), and the house-color remapper feeds exactly those through here. Reporting only
// per-pixel sizes made every compressed copy look unacceptable and got it skipped, which left the
// destination surface uninitialised - the retail symptom is a solid black unit texture.
WWINLINE UINT DX8_Format_Bytes_Per_Block(D3DFORMAT format)
{
	switch (format) {
	case D3DFMT_DXT1:
		return 8;
	case D3DFMT_DXT2:
	case D3DFMT_DXT3:
	case D3DFMT_DXT4:
	case D3DFMT_DXT5:
		return 16;
	default:
		return 0;
	}
}

// GeneralsX @bugfix d3d9 port 12/10/2026 Single row-copy primitive for the legal CPU routes below.
// A lock whose pitch cannot hold the row, or a format whose pixel size is unknown, is reported as a
// refusal so the caller can pick another route - copying anyway is what corrupted the heap.
WWINLINE bool DX8_Copy_Locked_Rect_Rows(
	const D3DLOCKED_RECT& destinationLock,
	const D3DLOCKED_RECT& sourceLock,
	UINT rows,
	UINT columns,
	D3DFORMAT format)
{
	const UINT bytesPerBlock = DX8_Format_Bytes_Per_Block(format);
	bool acceptableDims = false;
	UINT rowBytes = 0;
	UINT copyRows = rows;

	if (bytesPerBlock != 0) {
		// Compressed: rows become block rows and a row is whole 4x4 blocks. D3D9 refuses to lock
		// a non block-aligned rectangle, so a partial rect reaching this path is already rejected
		// by LockRect above the caller.
		acceptableDims = rows != 0 && columns != 0;
		copyRows = (rows + 3) / 4;
		rowBytes = ((columns + 3) / 4) * bytesPerBlock;
	} else {
		const UINT bytesPerPixel = DX8_Format_Bytes_Per_Pixel(format);
		acceptableDims = bytesPerPixel != 0 && rows != 0 && columns != 0;
		rowBytes = columns * bytesPerPixel;
	}

	const bool acceptable = acceptableDims
		&& destinationLock.Pitch > 0 && sourceLock.Pitch > 0
		&& (UINT)destinationLock.Pitch >= rowBytes && (UINT)sourceLock.Pitch >= rowBytes;

	if (!acceptable) {
		return false;
	}
	for (UINT row = 0; row < copyRows; ++row) {
		memcpy((unsigned char*)destinationLock.pBits + (INT_PTR)row * destinationLock.Pitch,
			(const unsigned char*)sourceLock.pBits + (INT_PTR)row * sourceLock.Pitch,
			rowBytes);
	}
	return true;
}

WWINLINE void DX8Wrapper::_Copy_DX8_Rects(
  IDirect3DSurface9* pSourceSurface,
  CONST RECT* pSourceRectsArray,
  UINT cRects,
  IDirect3DSurface9* pDestinationSurface,
  CONST POINT* pDestPointsArray
)
{
	if (cRects == 0) {
		DX8_RECORD_DX8_CALLS();

		// GeneralsX @bugfix d3d9 port 12/10/2026 Emulate DX8 CopyRects faithfully:
		// - system-memory source (font glyph atlas surfaces) copies through a system-memory
		//   staging texture, because D3D9 forbids LockRect on D3DPOOL_DEFAULT texture levels and
		//   DXVK's GetRenderTargetData access-violates when the source is not a render target.
		// - render-target sources keep the GetRenderTargetData path (destination must then be a
		//   system-memory plain surface, as all such callers create it).
		D3DSURFACE_DESC srcDesc;
		ZeroMemory(&srcDesc, sizeof(srcDesc));
		DX8_ErrorCode(pSourceSurface->GetDesc(&srcDesc));

		D3DSURFACE_DESC dstDesc;
		ZeroMemory(&dstDesc, sizeof(dstDesc));
		DX8_ErrorCode(pDestinationSurface->GetDesc(&dstDesc));

		if (srcDesc.Pool == D3DPOOL_SYSTEMMEM) {
			if (dstDesc.Pool == D3DPOOL_SYSTEMMEM) {
				D3DLOCKED_RECT srcLock;
				HRESULT hr = pSourceSurface->LockRect(&srcLock, nullptr, D3DLOCK_READONLY);
				if (SUCCEEDED(hr)) {
					D3DLOCKED_RECT dstLock;
					hr = pDestinationSurface->LockRect(&dstLock, nullptr, 0);
					if (SUCCEEDED(hr)) {
						DX8_Copy_Locked_Rect_Rows(dstLock, srcLock,
							srcDesc.Height < dstDesc.Height ? srcDesc.Height : dstDesc.Height,
							srcDesc.Width < dstDesc.Width ? srcDesc.Width : dstDesc.Width,
							dstDesc.Format);
						pDestinationSurface->UnlockRect();
					}
					pSourceSurface->UnlockRect();
				}
				if (FAILED(hr)) {
					WWDEBUG_SAY(("_Copy_DX8_Rects system-memory copy failed, hr = 0x%08X", hr));
				}
				return;
			}

			IDirect3DTexture9* dstTexture = nullptr;
			if (FAILED(pDestinationSurface->GetContainer(__uuidof(IDirect3DTexture9), (void**)&dstTexture)) || dstTexture == nullptr) {
				WWDEBUG_SAY(("_Copy_DX8_Rects: system-memory source has a non-texture destination"));
				return;
			}
			// GeneralsX @bugfix d3d9 port 12/10/2026 The upload path depends on the destination pool:
			// D3D9 forbids UpdateSurface/UpdateTexture into a D3DPOOL_MANAGED texture (DXVK answers
			// D3DERR_INVALIDCALL, verified with an isolated d3d9 probe), so managed destinations are
			// filled by locking the texture level and copying bytes. DEFAULT destinations go through
			// a system-memory staging texture + device UpdateTexture, which the probe verified for
			// A4R4G4B4 and X8R8G8B8.
			IDirect3DSurface9* dstLevel = nullptr;
			HRESULT hr = dstTexture->GetSurfaceLevel(0, &dstLevel);
			if (SUCCEEDED(hr)) {
				const UINT copyRows = srcDesc.Height < dstDesc.Height ? srcDesc.Height : dstDesc.Height;
				const UINT copyColumns = srcDesc.Width < dstDesc.Width ? srcDesc.Width : dstDesc.Width;
				if (dstDesc.Pool == D3DPOOL_MANAGED) {
					D3DLOCKED_RECT srcLock;
					hr = pSourceSurface->LockRect(&srcLock, nullptr, D3DLOCK_READONLY);
					if (SUCCEEDED(hr)) {
						D3DLOCKED_RECT dstLock;
						hr = dstLevel->LockRect(&dstLock, nullptr, 0);
						if (SUCCEEDED(hr)) {
							DX8_Copy_Locked_Rect_Rows(dstLock, srcLock, copyRows, copyColumns,
								dstDesc.Format);
							dstLevel->UnlockRect();
						}
						pSourceSurface->UnlockRect();
					}
					// GeneralsX @bugfix Qoder 18/10/2026 A CPU surface copied into a managed
					// texture never reaches the loader, so mirror the finished bytes here.
					if (SUCCEEDED(hr)) {
						VKTex_Mirror_D3D_Texture(dstTexture);
					}
				} else {
					DWORD dstLevels = dstTexture->GetLevelCount();
					IDirect3DTexture9* stageTexture = nullptr;
					IDirect3DSurface9* stageLevel = nullptr;
					hr = _Get_D3D_Device8()->CreateTexture(dstDesc.Width, dstDesc.Height, dstLevels, 0,
					                                       dstDesc.Format, D3DPOOL_SYSTEMMEM, &stageTexture, nullptr);
					if (SUCCEEDED(hr)) {
						hr = stageTexture->GetSurfaceLevel(0, &stageLevel);
					}
					if (SUCCEEDED(hr)) {
						D3DLOCKED_RECT srcLock;
						hr = pSourceSurface->LockRect(&srcLock, nullptr, D3DLOCK_READONLY);
						if (SUCCEEDED(hr)) {
							D3DLOCKED_RECT stageLock;
							hr = stageLevel->LockRect(&stageLock, nullptr, 0);
							if (SUCCEEDED(hr)) {
								const bool staged = DX8_Copy_Locked_Rect_Rows(stageLock, srcLock, copyRows, copyColumns,
									dstDesc.Format);
								stageLevel->UnlockRect();
								if (staged) {
									hr = _Get_D3D_Device8()->UpdateTexture(stageTexture, dstTexture);
									// GeneralsX @bugfix Qoder 18/10/2026 The destination is
									// D3DPOOL_DEFAULT and cannot be locked, so the Vulkan mirror
									// was never fed - the 3D shroud mask sampled white and the
									// fog never darkened the terrain. Upload the staging bytes
									// under the destination key.
									if (SUCCEEDED(hr)) {
										VKTex_Mirror_Texture_As(dstTexture, stageTexture, 1);
									}
								}
							}
							pSourceSurface->UnlockRect();
						}
					}
					if (stageLevel) {
						stageLevel->Release();
					}
					if (stageTexture) {
						stageTexture->Release();
					}
				}
			}
			if (FAILED(hr)) {
				WWDEBUG_SAY(("_Copy_DX8_Rects system-memory to texture upload failed, hr = 0x%08X", hr));
			}
			if (dstLevel) {
				dstLevel->Release();
			}
			dstTexture->Release();
			return;
		}

		HRESULT hr = _Get_D3D_Device8()->GetRenderTargetData(pSourceSurface, pDestinationSurface);
		if (FAILED(hr)) {
			// Source is not a render target (e.g. plain offscreen surface): plain copy works too.
			DX8CALL(UpdateSurface(pSourceSurface, nullptr, pDestinationSurface, nullptr));
		}
		return;
	}

	// GeneralsX @bugfix d3d9 port 12/10/2026 The rect list needs the same pool reasoning as the
	// full-surface path above, and D3D9 is stricter here than DX8's CopyRects ever was. Measured on
	// native d3d9 and DXVK 2.6 with the standalone rectcopyprobe in the port-notes folder - the two
	// stacks agree on every row:
	//   UpdateSurface   SYSTEMMEM -> DEFAULT            OK   (the only pair it accepts)
	//   UpdateSurface   SYSTEMMEM -> MANAGED/SYSTEMMEM  D3DERR_INVALIDCALL
	//   UpdateSurface   DEFAULT -> anything             D3DERR_INVALIDCALL
	//   StretchRect     DEFAULT -> DEFAULT              OK only when the destination has
	//                                                   D3DUSAGE_RENDERTARGET; a plain (usage 0)
	//                                                   DEFAULT texture level is refused
	//   LockRect(rect)  MANAGED/SYSTEMMEM               OK, and pBits names the rect origin
	//   GetRenderTargetData  RT / DEFAULT|RT level / back buffer -> SYSTEMMEM plain   OK
	// So a GPU-side source either stays on the GPU (StretchRect into a render-target destination,
	// which is what W3DSmudge's background texture is) or is staged into system memory first
	// (the profiler path, whose destination is a lockable plain surface).
	D3DSURFACE_DESC rectSrcDesc;
	ZeroMemory(&rectSrcDesc, sizeof(rectSrcDesc));
	DX8_ErrorCode(pSourceSurface->GetDesc(&rectSrcDesc));

	D3DSURFACE_DESC rectDstDesc;
	ZeroMemory(&rectDstDesc, sizeof(rectDstDesc));
	DX8_ErrorCode(pDestinationSurface->GetDesc(&rectDstDesc));

	// DX8 CopyRects demanded identical pixel formats, and the byte-level routes below can only
	// honour that. A conversion is D3DXLoadSurfaceFromSurface's job (SurfaceClass::Copy takes that
	// branch the moment the formats differ), so refusing here is both faithful and loud.
	if (rectSrcDesc.Format != rectDstDesc.Format) {
		WWDEBUG_SAY(("_Copy_DX8_Rects source format 0x%X differs from destination format 0x%X",
			(unsigned)rectSrcDesc.Format, (unsigned)rectDstDesc.Format));
		return;
	}

	IDirect3DSurface9* stagedSource = pSourceSurface;
	IDirect3DSurface9* stagingSurface = nullptr;
	// D3DPOOL_MANAGED texture levels are CPU-lockable, so only a DEFAULT-pool source needs a
	// staging readback. Asking GetRenderTargetData for anything else is what used to fault.
	if (rectSrcDesc.Pool == D3DPOOL_DEFAULT && rectDstDesc.Pool != D3DPOOL_DEFAULT) {
		const HRESULT stageHr = _Get_D3D_Device8()->CreateOffscreenPlainSurface(
			rectSrcDesc.Width, rectSrcDesc.Height, rectSrcDesc.Format, D3DPOOL_SYSTEMMEM, &stagingSurface, nullptr);
		if (SUCCEEDED(stageHr) && stagingSurface != nullptr) {
			const HRESULT readHr = _Get_D3D_Device8()->GetRenderTargetData(pSourceSurface, stagingSurface);
			if (SUCCEEDED(readHr)) {
				stagedSource = stagingSurface;
			} else {
				WWDEBUG_SAY(("_Copy_DX8_Rects staging readback failed, hr = 0x%08X", readHr));
				stagingSurface->Release();
				stagingSurface = nullptr;
			}
		}
	}

	const BOOL lockableDestination = (rectDstDesc.Pool == D3DPOOL_MANAGED || rectDstDesc.Pool == D3DPOOL_SYSTEMMEM);

	for (UINT r = 0; r < cRects; ++r) {
		const RECT& src = pSourceRectsArray[r];
		const POINT dst = pDestPointsArray ? pDestPointsArray[r] : POINT{ 0, 0 };
		const LONG rectWidth = src.right - src.left;
		const LONG rectHeight = src.bottom - src.top;
		HRESULT hr = D3D_OK;

		if (rectWidth <= 0 || rectHeight <= 0) {
			continue;
		}

		if (rectSrcDesc.Pool != D3DPOOL_SYSTEMMEM && stagedSource == pSourceSurface && rectDstDesc.Pool == D3DPOOL_DEFAULT) {
			// GPU to GPU, no round trip: pBits cannot be taken from a DEFAULT surface.
			const RECT dstRect = { dst.x, dst.y, (LONG)(dst.x + rectWidth), (LONG)(dst.y + rectHeight) };
			hr = _Get_D3D_Device8()->StretchRect(pSourceSurface, &src, pDestinationSurface, &dstRect, D3DTEXF_NONE);
		} else if (lockableDestination) {
			// GeneralsX @bugfix d3d9 port 12/10/2026 DX8 CopyRects never clamped, so a caller whose
			// destination point plus rect runs past the destination edge used to overflow the locked
			// buffer. The copy is trimmed to the destination instead, which keeps the visible region
			// correct and the memory intact.
			const LONG columns = (LONG)rectDstDesc.Width - dst.x;
			const LONG rows = (LONG)rectDstDesc.Height - dst.y;
			const LONG copyColumns = rectWidth < columns ? rectWidth : (columns > 0 ? columns : 0);
			const LONG copyRows = rectHeight < rows ? rectHeight : (rows > 0 ? rows : 0);
			D3DLOCKED_RECT srcLock;
			hr = stagedSource->LockRect(&srcLock, &src, D3DLOCK_READONLY);
			if (SUCCEEDED(hr)) {
				const RECT dstRect = { dst.x, dst.y, (LONG)(dst.x + rectWidth), (LONG)(dst.y + rectHeight) };
				D3DLOCKED_RECT dstLock;
				hr = pDestinationSurface->LockRect(&dstLock, &dstRect, 0);
				if (SUCCEEDED(hr)) {
					if (!DX8_Copy_Locked_Rect_Rows(dstLock, srcLock, (UINT)copyRows, (UINT)copyColumns,
							rectDstDesc.Format)) {
						hr = D3DERR_INVALIDCALL;
					}
					pDestinationSurface->UnlockRect();
				}
				stagedSource->UnlockRect();
			}
		} else {
			hr = _Get_D3D_Device8()->UpdateSurface(stagedSource, &src, pDestinationSurface, &dst);
		}

		DX8_RECORD_DX8_CALLS();
		DX8_ErrorCode(hr);
	}

	if (stagingSurface) {
		stagingSurface->Release();
	}
}

WWINLINE Vector4 DX8Wrapper::Convert_Color(unsigned color)
{
	Vector4 col;
	col[3]=((color&0xff000000)>>24)/255.0f;
	col[0]=((color&0xff0000)>>16)/255.0f;
	col[1]=((color&0xff00)>>8)/255.0f;
	col[2]=((color&0xff)>>0)/255.0f;
//	col=Vector4(1.0f,1.0f,1.0f,1.0f);
	return col;
}

#if 0
WWINLINE unsigned int DX8Wrapper::Convert_Color(const Vector3& color, const float alpha)
{
	WWASSERT(color.X<=1.0f);
	WWASSERT(color.Y<=1.0f);
	WWASSERT(color.Z<=1.0f);
	WWASSERT(alpha<=1.0f);
	WWASSERT(color.X>=0.0f);
	WWASSERT(color.Y>=0.0f);
	WWASSERT(color.Z>=0.0f);
	WWASSERT(alpha>=0.0f);

	return D3DCOLOR_COLORVALUE(color.X,color.Y,color.Z,alpha);
}
WWINLINE unsigned int DX8Wrapper::Convert_Color(const Vector4& color)
{
	WWASSERT(color.X<=1.0f);
	WWASSERT(color.Y<=1.0f);
	WWASSERT(color.Z<=1.0f);
	WWASSERT(color.W<=1.0f);
	WWASSERT(color.X>=0.0f);
	WWASSERT(color.Y>=0.0f);
	WWASSERT(color.Z>=0.0f);
	WWASSERT(color.W>=0.0f);

	return D3DCOLOR_COLORVALUE(color.X,color.Y,color.Z,color.W);
}
#else

// ----------------------------------------------------------------------------
//
// Convert RGBA color from float vector to 32 bit integer
// Note: Color vector needs to be clamped to [0...1] range!
//
// ----------------------------------------------------------------------------

WWINLINE unsigned int DX8Wrapper::Convert_Color(const Vector3& color,float alpha)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	const float scale = 255.0;
	unsigned int col;

	// Multiply r, g, b and a components (0.0,...,1.0) by 255 and convert to integer. Or the integer values togerher
	// such that 32 bit integer has AAAAAAAARRRRRRRRGGGGGGGGBBBBBBBB.
	__asm
	{
		sub	esp,20					// space for a, r, g and b float plus fpu rounding mode

		// Store the fpu rounding mode

		fwait
		fstcw		[esp+16]				// store control word to stack
		mov		eax,[esp+16]		// load it to eax
		mov		edi,eax				// take copy
		and		eax,~(1024|2048)	// mask out certain bits
		or			eax,(1024|2048)	// or with precision control value "truncate"
		sub		edi,eax				// did it change?
		jz			skip					// .. if not, skip
		mov		[esp],eax			// .. change control word
		fldcw		[esp]
skip:

		// Convert the color

		mov	esi,dword ptr color
		fld	dword ptr[scale]

		fld	dword ptr[esi]			// r
		fld	dword ptr[esi+4]		// g
		fld	dword ptr[esi+8]		// b
		fld	dword ptr[alpha]		// a
		fld	st(4)
		fmul	st(4),st
		fmul	st(3),st
		fmul	st(2),st
		fmulp	st(1),st
		fistp	dword ptr[esp+0]		// a
		fistp	dword ptr[esp+4]		// b
		fistp	dword ptr[esp+8]		// g
		fistp	dword ptr[esp+12]		// r
		mov	ecx,[esp]				// a
		mov	eax,[esp+4]				// b
		mov	edx,[esp+8]				// g
		mov	ebx,[esp+12]			// r
		shl	ecx,24					// a << 24
		shl	ebx,16					// r << 16
		shl	edx,8						//	g << 8
		or		eax,ecx					// (a << 24) | b
		or		eax,ebx					// (a << 24) | (r << 16) | b
		or		eax,edx					// (a << 24) | (r << 16) | (g << 8) | b

		fstp	st(0)

		// Restore fpu rounding mode

		cmp	edi,0					// did we change the value?
		je		not_changed			// nope... skip now...
		fwait
		fldcw	[esp+16];
not_changed:
		add	esp,20

		mov	col,eax
	}
	return col;
#else
	return color.Convert_To_ARGB(alpha);
#endif // defined(_MSC_VER) && _MSC_VER < 1300
}

// ----------------------------------------------------------------------------
//
// Clamp color vector to [0...1] range
//
// ----------------------------------------------------------------------------

WWINLINE void DX8Wrapper::Clamp_Color(Vector4& color)
{
#if defined(_MSC_VER) && _MSC_VER < 1300
	if (CPUDetectClass::Has_CMOV_Instruction()) {
	__asm
	{
		mov	esi,dword ptr color

		mov edx,0x3f800000

		mov edi,dword ptr[esi]
		mov ebx,edi
		sar edi,31
		not edi			// mask is now zero if negative value
		and edi,ebx
		cmp edi,edx		// if no less than 1.0 set to 1.0
		cmovnb edi,edx
		mov dword ptr[esi],edi

		mov edi,dword ptr[esi+4]
		mov ebx,edi
		sar edi,31
		not edi			// mask is now zero if negative value
		and edi,ebx
		cmp edi,edx		// if no less than 1.0 set to 1.0
		cmovnb edi,edx
		mov dword ptr[esi+4],edi

		mov edi,dword ptr[esi+8]
		mov ebx,edi
		sar edi,31
		not edi			// mask is now zero if negative value
		and edi,ebx
		cmp edi,edx		// if no less than 1.0 set to 1.0
		cmovnb edi,edx
		mov dword ptr[esi+8],edi

		mov edi,dword ptr[esi+12]
		mov ebx,edi
		sar edi,31
		not edi			// mask is now zero if negative value
		and edi,ebx
		cmp edi,edx		// if no less than 1.0 set to 1.0
		cmovnb edi,edx
		mov dword ptr[esi+12],edi
	}
	return;
	}
#endif // defined(_MSC_VER) && _MSC_VER < 1300

	for (int i=0;i<4;++i) {
		float f=(color[i]<0.0f) ? 0.0f : color[i];
		color[i]=(f>1.0f) ? 1.0f : f;
	}
}

// ----------------------------------------------------------------------------
//
// Convert RGBA color from float vector to 32 bit integer
//
// ----------------------------------------------------------------------------

WWINLINE unsigned int DX8Wrapper::Convert_Color(const Vector4& color)
{
	return Convert_Color(reinterpret_cast<const Vector3&>(color),color[3]);
}

WWINLINE unsigned int DX8Wrapper::Convert_Color_Clamp(const Vector4& color)
{
	Vector4 clamped_color=color;
	DX8Wrapper::Clamp_Color(clamped_color);
	return Convert_Color(reinterpret_cast<const Vector3&>(clamped_color),clamped_color[3]);
}

#endif


WWINLINE void DX8Wrapper::Set_Alpha (const float alpha, unsigned int &color)
{
	unsigned char *component = (unsigned char*) &color;

	component [3] = 255.0f * alpha;
}

WWINLINE void DX8Wrapper::Get_Render_State(RenderStateStruct& state)
{
	state=render_state;
}

WWINLINE void DX8Wrapper::Get_Shader(ShaderClass& shader)
{
	shader=render_state.shader;
}

WWINLINE void DX8Wrapper::Set_Texture(unsigned stage,TextureBaseClass* texture)
{
	// GeneralsX @feature PS3.0 batch7 13/10/2026 the deferred dirty bitmask above only reserves bits
	// 6..13, i.e. stages 0..7 - which is exactly what the device advertises as simultaneous textures -
	// and bit 14 onward is MATERIAL_CHANGED/SHADER_CHANGED/vertex and index buffer. ps_3_0 samplers go to
	// 16 slots, so a shader binding stage 8..15 through this call used to write a bit belonging to another
	// state: the texture never arrived and the material or shader got re-applied instead. Send the high
	// stages straight to the cached stage path, which covers all MAX_TEXTURE_STAGES.
	if (stage >= 8)
	{
		Set_DX8_Texture(stage,texture ? texture->Peek_D3D_Base_Texture() : nullptr);
		return;
	}

	if (texture==render_state.Textures[stage]) return;
	REF_PTR_SET(render_state.Textures[stage],texture);
	render_state_changed|=(TEXTURE0_CHANGED<<stage);
}

WWINLINE void DX8Wrapper::Set_Material(const VertexMaterialClass* material)
{
/*	if (material && render_state.material &&
		// !stricmp(material->Get_Name(),render_state.material->Get_Name())) {
		material->Get_CRC()!=render_state.material->Get_CRC()) {
		return;
	}
*/
//	if (material==render_state.material) {
//		return;
//	}
	REF_PTR_SET(render_state.material,const_cast<VertexMaterialClass*>(material));
	render_state_changed|=MATERIAL_CHANGED;
	SNAPSHOT_SAY(("DX8Wrapper::Set_Material(%s)",material ? material->Get_Name() : "null"));
}

WWINLINE void DX8Wrapper::Set_Shader(const ShaderClass& shader)
{
	if (!ShaderClass::ShaderDirty && ((unsigned&)shader==(unsigned&)render_state.shader)) {
		return;
	}
	render_state.shader=shader;
	render_state_changed|=SHADER_CHANGED;

	SNAPSHOT_SAY(("DX8Wrapper::Set_Shader(%s)",shader.Get_Description().str()));
}

// GeneralsX @refactor BenderAI 10/02/2026
// Applied fighter19 pattern: store Matrix4x4 transposed, cast to D3DMATRIX* for D3D calls
WWINLINE void DX8Wrapper::Set_Projection_Transform_With_Z_Bias(const Matrix4x4& matrix, float znear, float zfar)
{
	ZFar=zfar;
	ZNear=znear;
	ProjectionMatrix=matrix.Transpose();

	if (!Get_Current_Caps()->Support_ZBias() && ZNear!=ZFar) {
		Matrix4x4 tmp=ProjectionMatrix;
		float tmp_zbias=ZBias;
		tmp_zbias*=(1.0f/16.0f);
		tmp_zbias*=1.0f / (ZFar - ZNear);
		tmp[2][2]-=tmp_zbias*tmp[3][2];
		DX8CALL(SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&tmp));
	}
	else {
		DX8CALL(SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&ProjectionMatrix));
	}
}

WWINLINE void DX8Wrapper::Set_DX8_ZBias(int zbias)
{
	if (zbias==ZBias) return;
	if (zbias>15) zbias=15;
	if (zbias<0) zbias=0;
	ZBias=zbias;

	// GeneralsX @build d3d9 port 11/10/2026 - D3D9 removed D3DRS_ZBIAS; the projection-matrix
	// emulation (already used for hardware without ZBias caps) is now the only path. Callers keep
	// the same visual result for perspective projections; degenerate ZNear==ZFar passes (2D/UI,
	// where Z testing is off) intentionally skip the bias.
	if (ZNear!=ZFar) {
		// GeneralsX @refactor BenderAI 10/02/2026
		// Changed D3DMATRIX -> Matrix4x4 to match ProjectionMatrix type change
		Matrix4x4 tmp=ProjectionMatrix;
		float tmp_zbias=ZBias;
		tmp_zbias*=(1.0f/16.0f);
		tmp_zbias*=1.0f / (ZFar - ZNear);
		tmp[2][2]-=tmp_zbias*tmp[3][2];
		DX8CALL(SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&tmp));
	}
}

// GeneralsX @feature PS3.0 batch7 13/10/2026 float render states travel as the bit pattern of the
// float, the same way the fog range states above already do.
WWINLINE void DX8Wrapper::Set_Depth_Bias(float bias)
{
	if (bias==DepthBias) return;
	DepthBias=bias;
	Set_DX8_Render_State(D3DRS_DEPTHBIAS,*(DWORD *)(&bias));
}

WWINLINE void DX8Wrapper::Set_Slope_Scale_Depth_Bias(float scale)
{
	if (scale==SlopeScaleDepthBias) return;
	SlopeScaleDepthBias=scale;
	Set_DX8_Render_State(D3DRS_SLOPESCALEDEPTHBIAS,*(DWORD *)(&scale));
}

// GeneralsX @refactor BenderAI 10/02/2026
// Applied fighter19 pattern: transpose and store as Matrix4x4, cast pointer for D3D calls
WWINLINE void DX8Wrapper::Set_Transform(D3DTRANSFORMSTATETYPE transform,const Matrix4x4& m)
{
	switch ((int)transform) {
	case D3DTS_WORLD:
		render_state.world=m.Transpose();
		render_state_changed|=(unsigned)WORLD_CHANGED;
		render_state_changed&=~(unsigned)WORLD_IDENTITY;
		break;
	case D3DTS_VIEW:
		render_state.view=m.Transpose();
		render_state_changed|=(unsigned)VIEW_CHANGED;
		render_state_changed&=~(unsigned)VIEW_IDENTITY;
		break;
	case D3DTS_PROJECTION:
		{
			// GeneralsX @bugfix Qoder 18/10/2026 This used to declare a local named ProjectionMatrix,
			// shadowing the static member. D3D9 still received the matrix, but the facade shadow that
			// VK_FF_Pack feeds to the Vulkan FF shader kept the last camera projection - so every
			// NDC-space 2D batch (Render2DClass UI, dazzle, sun-shadow receiver) was transformed by a
			// stale perspective matrix and left the screen. Assign the member so both paths agree.
			ProjectionMatrix=m.Transpose();
			ZFar=0.0f;
			ZNear=0.0f;
			DX8CALL(SetTransform(D3DTS_PROJECTION,(D3DMATRIX*)&ProjectionMatrix));
		}
		break;
	default:
		DX8_RECORD_MATRIX_CHANGE();
		Matrix4x4 m2=m.Transpose();
		DX8CALL(SetTransform(transform,(D3DMATRIX*)&m2));
		break;
	}
}

// GeneralsX @refactor BenderAI 10/02/2026
// Applied fighter19 pattern: convert Matrix3D to Matrix4x4, transpose, cast for D3D calls
WWINLINE void DX8Wrapper::Set_Transform(D3DTRANSFORMSTATETYPE transform,const Matrix3D& m)
{
	switch ((int)transform) {
	case D3DTS_WORLD:
		render_state.world=Matrix4x4(m).Transpose();
		render_state_changed|=(unsigned)WORLD_CHANGED;
		render_state_changed&=~(unsigned)WORLD_IDENTITY;
		break;
	case D3DTS_VIEW:
		render_state.view=Matrix4x4(m).Transpose();
		render_state_changed|=(unsigned)VIEW_CHANGED;
		render_state_changed&=~(unsigned)VIEW_IDENTITY;
		break;
	default:
		DX8_RECORD_MATRIX_CHANGE();
		Matrix4x4 m2=Matrix4x4(m).Transpose();
		DX8CALL(SetTransform(transform,(D3DMATRIX*)&m2));
		break;
	}
}

WWINLINE bool DX8Wrapper::Is_World_Identity()
{
	return !!(render_state_changed&(unsigned)WORLD_IDENTITY);
}

WWINLINE bool DX8Wrapper::Is_View_Identity()
{
	return !!(render_state_changed&(unsigned)VIEW_IDENTITY);
}

// GeneralsX @refactor BenderAI 10/02/2026
// Applied fighter19 pattern: transpose back when reading stored matrices
WWINLINE void DX8Wrapper::Get_Transform(D3DTRANSFORMSTATETYPE transform, Matrix4x4& m)
{
	D3DMATRIX mat;

	switch ((int)transform) {
	case D3DTS_WORLD:
		if (render_state_changed&WORLD_IDENTITY) m.Make_Identity();
		else m=render_state.world.Transpose();
		break;
	case D3DTS_VIEW:
		if (render_state_changed&VIEW_IDENTITY) m.Make_Identity();
		else m=render_state.view.Transpose();
		break;
	default:
		DX8CALL(GetTransform(transform,&mat));
		m=*(Matrix4x4*)&mat;
		m=m.Transpose();
		break;
	}
}

WWINLINE const D3DLIGHT9& DX8Wrapper::Peek_Light(unsigned index)
{
	return render_state.Lights[index];
}

WWINLINE bool DX8Wrapper::Is_Light_Enabled(unsigned index)
{
	return render_state.LightEnable[index];
}


WWINLINE void DX8Wrapper::Set_Render_State(const RenderStateStruct& state)
{
	int i;

	if (render_state.index_buffer) {
		render_state.index_buffer->Release_Engine_Ref();
	}

	for (i=0;i<MAX_VERTEX_STREAMS;++i)
	{
		if (render_state.vertex_buffers[i])
		{
			render_state.vertex_buffers[i]->Release_Engine_Ref();
		}
	}

	render_state=state;
	render_state_changed=0xffffffff;

	if (render_state.index_buffer) {
		render_state.index_buffer->Add_Engine_Ref();
	}

	for (i=0;i<MAX_VERTEX_STREAMS;++i)
	{
		if (render_state.vertex_buffers[i])
		{
			render_state.vertex_buffers[i]->Add_Engine_Ref();
		}
	}
}

WWINLINE void DX8Wrapper::Release_Render_State()
{
	int i;

	if (render_state.index_buffer) {
		render_state.index_buffer->Release_Engine_Ref();
	}

	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		if (render_state.vertex_buffers[i]) {
			render_state.vertex_buffers[i]->Release_Engine_Ref();
		}
	}

	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		REF_PTR_RELEASE(render_state.vertex_buffers[i]);
	}
	REF_PTR_RELEASE(render_state.index_buffer);
	REF_PTR_RELEASE(render_state.material);


	for (i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		REF_PTR_RELEASE(render_state.Textures[i]);
	}
}


WWINLINE RenderStateStruct::RenderStateStruct()
	:
	material(0),
	index_buffer(0)
{
	unsigned i;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) vertex_buffers[i]=0;
	for (i=0;i<MAX_TEXTURE_STAGES;++i) Textures[i]=0;
  //lightsHash = (unsigned)this;
}

WWINLINE RenderStateStruct::~RenderStateStruct()
{
	unsigned i;
	REF_PTR_RELEASE(material);
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		REF_PTR_RELEASE(vertex_buffers[i]);
	}
	REF_PTR_RELEASE(index_buffer);

	for (i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		REF_PTR_RELEASE(Textures[i]);
	}
}


WWINLINE unsigned flimby( char* name, unsigned crib )
{
  unsigned lnt prevVer = 0x00000000;
  unsigned D3D2_BASE_VEC nextVer = 0;
  for( unsigned t = 0; t < crib; ++t )
  {
    (D3D2_BASE_VEC)nextVer += name[t];
    (D3D2_BASE_VEC)nextVer %= 32;
    (D3D2_BASE_VEC)nextVer-- ;
    (lnt) prevVer ^=  ( 1 << (D3D2_BASE_VEC)prevVer );
  }
  return (lnt) prevVer;
}

WWINLINE RenderStateStruct& RenderStateStruct::operator= (const RenderStateStruct& src)
{
	unsigned i;
	REF_PTR_SET(material,src.material);
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		REF_PTR_SET(vertex_buffers[i],src.vertex_buffers[i]);
	}
	REF_PTR_SET(index_buffer,src.index_buffer);

	for (i=0;i<MAX_TEXTURE_STAGES;++i)
	{
		REF_PTR_SET(Textures[i],src.Textures[i]);
	}

	// GeneralsX @refactor PS3.0-P3-18 14/10/2026: the hand-written 4-deep enable chain becomes loops
	// now that the fixed-function state carries 8 light slots. Same early-exit semantics as the retail
	// nesting: an enabled slot is only copied while every lower slot is enabled too, which is exactly
	// the order Set_Light_Environment fills them in.
	for (i = 0; i < 8; ++i) {
		LightEnable[i] = src.LightEnable[i];
	}
	for (i = 0; i < 8; ++i) {
		if (!LightEnable[i]) break;
		Lights[i] = src.Lights[i];
	}

    //lightsHash = flimby((char*)(&Lights[0]), sizeof(D3DLIGHT9)-1 );

	shader=src.shader;
	world=src.world;
	view=src.view;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		vertex_buffer_types[i]=src.vertex_buffer_types[i];
	}
	index_buffer_type=src.index_buffer_type;
	vba_offset=src.vba_offset;
	vba_count=src.vba_count;
	iba_offset=src.iba_offset;
	index_base_offset=src.index_base_offset;

	return *this;
}
