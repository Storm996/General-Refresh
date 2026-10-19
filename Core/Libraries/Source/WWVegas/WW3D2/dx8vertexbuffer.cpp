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
 *                     $Archive:: /Commando/Code/ww3d2/dx8vertexbuffer.cpp                    $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/26/02 5:06p                                             $*
 *                                                                                             *
 *                    $Revision:: 39                                                          $*
 *                                                                                             *
 * 06/26/02 KM VB Vertex format size update for shaders                                       *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//#define VERTEX_BUFFER_LOG

#include "dx8vertexbuffer.h"
#include "dx8wrapper.h"
#include "dx8fvf.h"
#include "dx8caps.h"
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: geometry ring bridge (win64vk port tree only)
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
#include "Backend/VulkanGeo.h"
#endif
#include "WWLib/thread.h"
#include "WWDebug/wwmemlog.h"
#include <d3dx9core.h>
#include <stdio.h>   // GeneralsX @feature wedge 9.J2d: no-d3d9 fall-through probe writes to stderr
#include <stdlib.h>  // getenv

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: declared here rather than included, because
// Backend/VKD3DStubImpl.h carries the whole D3D9 COM stub class set and this TU does not want it.
void VK_NOD3D_Watch_Arm(void* obj);
void VK_NOD3D_Watch_Check(const char* where);
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: forensic dump of the DX9 pool object every
// time the no-d3d9 fall-through touches it. The run shows the object alive (vptr points at a module vtable)
// at one lock and zeroed at the very next one with no destructor ever running, so somebody wrote over the
// block. The window is what names the writer's shape: one qword cleared is a targeted pointer store, a long
// zero run is a memset/recycle, and the qword before the object says whether a neighbouring allocation ran
// off its end. Armed only by GENERALSX_VK_NOD3D; the DXVK path never calls this.
static void vk_nod3d_dump_dx9_object(const char* tag, unsigned long long seq, void* obj)
{
	static int nod3d = -1;
	if (nod3d < 0) nod3d = (getenv("GENERALSX_VK_NOD3D") && atoi(getenv("GENERALSX_VK_NOD3D")) != 0) ? 1 : 0;
	if (!nod3d || seq > 64 || !obj) return;
	const uintptr_t* q = static_cast<const uintptr_t*>(obj);
	fprintf(stderr, "[VKNOD3D] %s #%llu obj=%p q[-1]=%p q0=%p q1=%p q2=%p q3=%p q4=%p q5=%p q6=%p q7=%p\n",
		tag, seq, obj, (const void*)q[-1], (const void*)q[0], (const void*)q[1], (const void*)q[2],
		(const void*)q[3], (const void*)q[4], (const void*)q[5], (const void*)q[6], (const void*)q[7]);
	fflush(stderr);
}
#endif

#define DEFAULT_VB_SIZE 5000

static bool _DynamicSortingVertexArrayInUse=false;
//static VertexFormatXYZNDUV2* _DynamicSortingVertexArray=nullptr;
static SortingVertexBufferClass* _DynamicSortingVertexArray=nullptr;
static unsigned short _DynamicSortingVertexArraySize=0;
static unsigned short _DynamicSortingVertexArrayOffset=0;

static bool _DynamicDX8VertexBufferInUse=false;
static DX8VertexBufferClass* _DynamicDX8VertexBuffer=nullptr;
static unsigned short _DynamicDX8VertexBufferSize=DEFAULT_VB_SIZE;
static unsigned short _DynamicDX8VertexBufferOffset=0;

static const FVFInfoClass _DynamicFVFInfo(dynamic_fvf_type);

// GeneralsX @feature T3-0 P1: separate global pool for the tangent-space dynamic layout so a
// normal-mapped skin batch never disturbs the classic dynamic buffer's stride/state.
static const FVFInfoClass _DynamicFVFInfoNM(dynamic_fvf_type_nm);
static DX8VertexBufferClass* _DynamicDX8VertexBufferNM=nullptr;
static unsigned short _DynamicDX8VertexBufferNMSize=DEFAULT_VB_SIZE;
static unsigned short _DynamicDX8VertexBufferNMOffset=0;
static bool _DynamicDX8VertexBufferNMInUse=false;

static int _DX8VertexBufferCount=0;

static int _VertexBufferCount;
static int _VertexBufferTotalVertices;
static int _VertexBufferTotalSize;

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

VertexBufferClass::VertexBufferClass(unsigned type_, unsigned FVF, unsigned short vertex_count_)
	:
	VertexCount(vertex_count_),
	type(type_),
	engine_refs(0)
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(VertexCount);
	WWASSERT(type==BUFFER_TYPE_DX8 || type==BUFFER_TYPE_SORTING);
	WWASSERT(FVF != 0);
	fvf_info=W3DNEW FVFInfoClass(FVF);

	_VertexBufferCount++;
	_VertexBufferTotalVertices+=VertexCount;
	_VertexBufferTotalSize+=VertexCount*fvf_info->Get_FVF_Size();
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A forensics: pair with the backing create/drop
	// trace so a buffer that never gets a backing is distinguishable from one whose backing was dropped.
	if (type == BUFFER_TYPE_DX8) VKGeo_Trace_Buffer(this, FVF, VertexCount, 1);
#endif
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("New VB, %d vertices, size %d bytes",VertexCount,VertexCount*fvf_info->Get_FVF_Size()));
	WWDEBUG_SAY(("Total VB count: %d, total %d vertices, total size %d bytes",
		_VertexBufferCount,
		_VertexBufferTotalVertices,
		_VertexBufferTotalSize));
#endif
}

// ----------------------------------------------------------------------------

VertexBufferClass::~VertexBufferClass()
{
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.1: the wedge-5 backing is keyed by this
	// object's address, so it must die with the object - otherwise a buffer that is later allocated at
	// the same address inherits this one's VkBuffer and draws its stale geometry.
	VKGeo_Back_Drop(this);
	VKGeo_Trace_Buffer(this, fvf_info ? fvf_info->Get_FVF() : 0u, VertexCount, 0);
#endif
	_VertexBufferCount--;
	_VertexBufferTotalVertices-=VertexCount;
	_VertexBufferTotalSize-=VertexCount*fvf_info->Get_FVF_Size();

#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("Delete VB, %d vertices, size %d bytes",VertexCount,VertexCount*fvf_info->Get_FVF_Size()));
	WWDEBUG_SAY(("Total VB count: %d, total %d vertices, total size %d bytes",
		_VertexBufferCount,
		_VertexBufferTotalVertices,
		_VertexBufferTotalSize));
#endif
	delete fvf_info;
}

unsigned VertexBufferClass::Get_Total_Buffer_Count()
{
	return _VertexBufferCount;
}

unsigned VertexBufferClass::Get_Total_Allocated_Vertices()
{
	return _VertexBufferTotalVertices;
}

unsigned VertexBufferClass::Get_Total_Allocated_Memory()
{
	return _VertexBufferTotalSize;
}


// ----------------------------------------------------------------------------

void VertexBufferClass::Add_Engine_Ref() const
{
	engine_refs++;
}

// ----------------------------------------------------------------------------

void VertexBufferClass::Release_Engine_Ref() const
{
	engine_refs--;
	WWASSERT(engine_refs>=0);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

VertexBufferClass::WriteLockClass::WriteLockClass(VertexBufferClass* VertexBuffer, int flags)
	:
	VertexBufferLockClass(VertexBuffer)
{
	DX8_THREAD_ASSERT();
	WWASSERT(VertexBuffer);
	WWASSERT(!VertexBuffer->Engine_Refs());
	VertexBuffer->Add_Ref();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX8:
#ifdef VERTEX_BUFFER_LOG
		{
		StringClass fvf_name;
		VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("VertexBuffer->Lock(start_index: 0, index_range: 0(%d), fvf_size: %d, fvf: %s)",
			VertexBuffer->Get_Vertex_Count(),
			VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
		DX8_Assert();
		// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: with the Vulkan geometry ring up
		// (GENERALSX_VKGEO=1), vertex data is written straight into host-visible VkDeviceMemory
		// and the D3D9 lock is skipped; D3D9 presentation is already intercepted at wedge 2.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		{
			unsigned geo_size = VertexBuffer->Get_Vertex_Count() * VertexBuffer->FVF_Info().Get_FVF_Size();
			// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.1: once the draw path is up the
			// batch needs the (VkBuffer, offset) THIS lock wrote, and it has to stay valid until the draw
			// reads it - wedge 3.1's shared bump ring gave every lock a new offset and can't back that.
			// The per-object backing is keyed by the WW3D buffer object itself.
			Vertices = VKGeo_Back_Lock(VertexBuffer, geo_size, VertexBuffer->FVF_Info().Get_FVF_Size(), 0,
				(flags & D3DLOCK_DISCARD) != 0);
			if (Vertices) {
				vk_mirrored = true;
				break;
			}
			if (VKGeo_Ready(geo_size)) {
				Vertices = VKGeo_Alloc(geo_size, (flags & D3DLOCK_DISCARD) != 0);
				if (Vertices) {
					vk_mirrored = true;
					break;
				}
			}
		}
#endif
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		VK_NOD3D_Watch_Check("engine VB writelock");
#endif
		DX8_ErrorCode(static_cast<DX8VertexBufferClass*>(VertexBuffer)->Get_DX8_Vertex_Buffer()->Lock(
			0,
			0,
			(void**)&Vertices,
			flags));	//flags
		break;
	case BUFFER_TYPE_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(VertexBuffer)->VertexBuffer;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

VertexBufferClass::WriteLockClass::~WriteLockClass()
{
	DX8_THREAD_ASSERT();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX8:
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		if (vk_mirrored) break;	// wedge 3.1: ring write, no D3D9 resource was locked
#endif
		DX8_Assert();
#ifdef VERTEX_BUFFER_LOG
		WWDEBUG_SAY(("VertexBuffer->Unlock()"));
#endif
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		VK_NOD3D_Watch_Check("engine VB writelock unlock");
#endif
		DX8_ErrorCode(static_cast<DX8VertexBufferClass*>(VertexBuffer)->Get_DX8_Vertex_Buffer()->Unlock());
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	VertexBuffer->Release_Ref();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

VertexBufferClass::AppendLockClass::AppendLockClass(VertexBufferClass* VertexBuffer,unsigned start_index, unsigned index_range)
	:
	VertexBufferLockClass(VertexBuffer)
{
	DX8_THREAD_ASSERT();
	WWASSERT(VertexBuffer);
	WWASSERT(!VertexBuffer->Engine_Refs());
	WWASSERT(start_index+index_range<=VertexBuffer->Get_Vertex_Count());
	VertexBuffer->Add_Ref();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX8:
#ifdef VERTEX_BUFFER_LOG
		{
		StringClass fvf_name;
		VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("VertexBuffer->Lock(start_index: %d, index_range: %d, fvf_size: %d, fvf: %s)",
			start_index,
			index_range,
			VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
		DX8_Assert();
		// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: mirror partial append into a fresh
		// full-buffer-sized ring region so the write lands at base + start_index*fvf.
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		{
			unsigned fvf_size = VertexBuffer->FVF_Info().Get_FVF_Size();
			unsigned geo_size = VertexBuffer->Get_Vertex_Count() * fvf_size;
			// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.1: an append writes into the same
			// per-object backing as the full lock, so several appends of one buffer converge in one VkBuffer
			// instead of scattering across the bump ring.
			char* back_base = (char*)VKGeo_Back_Lock(VertexBuffer, geo_size, fvf_size, 0, false);
			if (back_base) {
				vk_mirrored = true;
				Vertices = back_base + start_index * fvf_size;
				break;
			}
			if (VKGeo_Ready(geo_size)) {
				char* ring_base = (char*)VKGeo_Alloc(geo_size, false);
				if (ring_base) {
					vk_mirrored = true;
					Vertices = ring_base + start_index * fvf_size;
					break;
				}
			}
		}
#endif
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		VK_NOD3D_Watch_Check("engine VB appendlock");
#endif
		DX8_ErrorCode(static_cast<DX8VertexBufferClass*>(VertexBuffer)->Get_DX8_Vertex_Buffer()->Lock(
			start_index*VertexBuffer->FVF_Info().Get_FVF_Size(),
			index_range*VertexBuffer->FVF_Info().Get_FVF_Size(),
			(void**)&Vertices,
			0));	// Default (no) flags
		break;
	case BUFFER_TYPE_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(VertexBuffer)->VertexBuffer+start_index;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

VertexBufferClass::AppendLockClass::~AppendLockClass()
{
	DX8_THREAD_ASSERT();
	switch (VertexBuffer->Type()) {
	case BUFFER_TYPE_DX8:
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		if (vk_mirrored) break;	// wedge 3.1: ring write, no D3D9 resource was locked
#endif
		DX8_Assert();
#ifdef VERTEX_BUFFER_LOG
		WWDEBUG_SAY(("VertexBuffer->Unlock()"));
#endif
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		VK_NOD3D_Watch_Check("engine VB appendlock unlock");
#endif
		DX8_ErrorCode(static_cast<DX8VertexBufferClass*>(VertexBuffer)->Get_DX8_Vertex_Buffer()->Unlock());
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	VertexBuffer->Release_Ref();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

SortingVertexBufferClass::SortingVertexBufferClass(unsigned short VertexCount)
	:
	VertexBufferClass(BUFFER_TYPE_SORTING, dynamic_fvf_type, VertexCount)
{
	WWMEMLOG(MEM_RENDERER);
	VertexBuffer=W3DNEWARRAY VertexFormatXYZNDUV2[VertexCount];
}

// ----------------------------------------------------------------------------

SortingVertexBufferClass::~SortingVertexBufferClass()
{
	delete[] VertexBuffer;
}


// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

//	bool dynamic=false,bool softwarevp=false);

DX8VertexBufferClass::DX8VertexBufferClass(unsigned FVF, unsigned short vertex_count_, UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, FVF, vertex_count_),
	VertexBuffer(nullptr)
{
	Create_Vertex_Buffer(usage);
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::DX8VertexBufferClass(
	const Vector3* vertices,
	const Vector3* normals,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_NORMAL, VertexCount),
	VertexBuffer(nullptr)
{
	WWASSERT(vertices);
	WWASSERT(normals);
	WWASSERT(tex_coords);

	Create_Vertex_Buffer(usage);
	Copy(vertices,normals,tex_coords,0,VertexCount);
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::DX8VertexBufferClass(
	const Vector3* vertices,
	const Vector3* normals,
	const Vector4* diffuse,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_NORMAL|D3DFVF_DIFFUSE, VertexCount),
	VertexBuffer(nullptr)
{
	WWASSERT(vertices);
	WWASSERT(normals);
	WWASSERT(tex_coords);
	WWASSERT(diffuse);

	Create_Vertex_Buffer(usage);
	Copy(vertices,normals,tex_coords,diffuse,0,VertexCount);
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::DX8VertexBufferClass(
	const Vector3* vertices,
	const Vector4* diffuse,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, D3DFVF_XYZ|D3DFVF_TEX1|D3DFVF_DIFFUSE, VertexCount),
	VertexBuffer(nullptr)
{
	WWASSERT(vertices);
	WWASSERT(tex_coords);
	WWASSERT(diffuse);

	Create_Vertex_Buffer(usage);
	Copy(vertices,tex_coords,diffuse,0,VertexCount);
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::DX8VertexBufferClass(
	const Vector3* vertices,
	const Vector2* tex_coords,
	unsigned short VertexCount,
	UsageType usage)
	:
	VertexBufferClass(BUFFER_TYPE_DX8, D3DFVF_XYZ|D3DFVF_TEX1, VertexCount),
	VertexBuffer(nullptr)
{
	WWASSERT(vertices);
	WWASSERT(tex_coords);

	Create_Vertex_Buffer(usage);
	Copy(vertices,tex_coords,0,VertexCount);
}

// ----------------------------------------------------------------------------

DX8VertexBufferClass::~DX8VertexBufferClass()
{
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("VertexBuffer->Release()"));
	_DX8VertexBufferCount--;
	WWDEBUG_SAY(("Current vertex buffer count: %d",_DX8VertexBufferCount));
#endif
	VertexBuffer->Release();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DX8VertexBufferClass::Create_Vertex_Buffer(UsageType usage)
{
	DX8_THREAD_ASSERT();
	WWASSERT(!VertexBuffer);

#ifdef VERTEX_BUFFER_LOG
	StringClass fvf_name;
	FVF_Info().Get_FVF_Name(fvf_name);
	WWDEBUG_SAY(("CreateVertexBuffer(fvfsize=%d, vertex_count=%d, D3DUSAGE_WRITEONLY|%s|%s, fvf: %s, %s)",
		FVF_Info().Get_FVF_Size(),
		VertexCount,
		(usage&USAGE_DYNAMIC) ? "D3DUSAGE_DYNAMIC" : "-",
		(usage&USAGE_SOFTWAREPROCESSING) ? "D3DUSAGE_SOFTWAREPROCESSING" : "-",
		fvf_name,
		(usage&USAGE_DYNAMIC) ? "D3DPOOL_DEFAULT" : "D3DPOOL_MANAGED"));
	_DX8VertexBufferCount++;
	WWDEBUG_SAY(("Current vertex buffer count: %d",_DX8VertexBufferCount));
#endif

	unsigned usage_flags=
		D3DUSAGE_WRITEONLY|
		((usage&USAGE_DYNAMIC) ? D3DUSAGE_DYNAMIC : 0)|
		((usage&USAGE_NPATCHES) ? D3DUSAGE_NPATCHES : 0)|
		((usage&USAGE_SOFTWAREPROCESSING) ? D3DUSAGE_SOFTWAREPROCESSING : 0);
	// New Code
	if (!DX8Wrapper::Get_Current_Caps()->Support_TnL()) {
		usage_flags|=D3DUSAGE_SOFTWAREPROCESSING;
	}

	HRESULT ret=DX8Wrapper::_Get_D3D_Device8()->CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&VertexBuffer, nullptr);
	if (SUCCEEDED(ret)) {
		return;
	}

	WWDEBUG_SAY(("Vertex buffer creation failed, trying to release assets..."));

	// Vertex buffer creation failed, so try releasing least used textures and flushing the mesh cache.

	// Free all textures that haven't been used in the last 5 seconds
	TextureClass::Invalidate_Old_Unused_Textures(5000);

	// Invalidate the mesh cache
	WW3D::_Invalidate_Mesh_Cache();

	//@todo: Find some way to invalidate the textures too
		// GeneralsX @build d3d9 port 11/10/2026 - ResourceManagerDiscardBytes removed in D3D9; runtime handles discard on Reset.

	// Try again...
	ret=DX8Wrapper::_Get_D3D_Device8()->CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&VertexBuffer, nullptr);

	if (SUCCEEDED(ret)) {
		WWDEBUG_SAY(("...Vertex buffer creation successful"));
	}

	// If it still fails it is fatal
	DX8_ErrorCode(ret);

	/* Old Code
	DX8CALL(CreateVertexBuffer(
		FVF_Info().Get_FVF_Size()*VertexCount,
		usage_flags,
		FVF_Info().Get_FVF(),
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&VertexBuffer, nullptr));
	*/
}

// ----------------------------------------------------------------------------

void DX8VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, const Vector2* uv, unsigned first_vertex,unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(uv);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZNUV1);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZNUV1* verts=(VertexFormatXYZNUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZNUV1* verts=(VertexFormatXYZNUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
	}
}

// ----------------------------------------------------------------------------

void DX8VertexBufferClass::Copy(const Vector3* loc, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZ);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZ* verts=(VertexFormatXYZ*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZ* verts=(VertexFormatXYZ*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
		}
	}

}

// ----------------------------------------------------------------------------

void DX8VertexBufferClass::Copy(const Vector3* loc, const Vector2* uv, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(uv);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZUV1);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZUV1* verts=(VertexFormatXYZUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZUV1* verts=(VertexFormatXYZUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
		}
	}
}

// ----------------------------------------------------------------------------

void DX8VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZN);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZN* verts=(VertexFormatXYZN*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZN* verts=(VertexFormatXYZN*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
		}
	}
}

// ----------------------------------------------------------------------------

void DX8VertexBufferClass::Copy(const Vector3* loc, const Vector3* norm, const Vector2* uv, const Vector4* diffuse, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(norm);
	WWASSERT(uv);
	WWASSERT(diffuse);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZNDUV1);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZNDUV1* verts=(VertexFormatXYZNDUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX8Wrapper::Convert_Color(diffuse[v]);
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZNDUV1* verts=(VertexFormatXYZNDUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].nx=(*norm)[0];
			verts[v].ny=(*norm)[1];
			verts[v].nz=(*norm++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX8Wrapper::Convert_Color(diffuse[v]);
		}
	}
}

// ----------------------------------------------------------------------------

void DX8VertexBufferClass::Copy(const Vector3* loc, const Vector2* uv, const Vector4* diffuse, unsigned first_vertex, unsigned count)
{
	WWASSERT(loc);
	WWASSERT(uv);
	WWASSERT(diffuse);
	WWASSERT(count<=VertexCount);
	WWASSERT(FVF_Info().Get_FVF()==DX8_FVF_XYZDUV1);

	if (first_vertex) {
		VertexBufferClass::AppendLockClass l(this,first_vertex,count);
		VertexFormatXYZDUV1* verts=(VertexFormatXYZDUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX8Wrapper::Convert_Color(diffuse[v]);
		}
	}
	else {
		VertexBufferClass::WriteLockClass l(this);
		VertexFormatXYZDUV1* verts=(VertexFormatXYZDUV1*)l.Get_Vertex_Array();
		for (unsigned v=0;v<count;++v) {
			verts[v].x=(*loc)[0];
			verts[v].y=(*loc)[1];
			verts[v].z=(*loc++)[2];
			verts[v].u1=(*uv)[0];
			verts[v].v1=(*uv++)[1];
			verts[v].diffuse=DX8Wrapper::Convert_Color(diffuse[v]);
		}
	}
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DynamicVBAccessClass::DynamicVBAccessClass(unsigned t,unsigned fvf,unsigned short vertex_count_)
	:
	Type(t),
	FVFInfo(fvf==dynamic_fvf_type_nm ? _DynamicFVFInfoNM : _DynamicFVFInfo),
	VertexCount(vertex_count_),
	VertexBuffer(nullptr)
{
	WWASSERT(fvf==dynamic_fvf_type || fvf==dynamic_fvf_type_nm);
	WWASSERT(Type==BUFFER_TYPE_DYNAMIC_DX8 || Type==BUFFER_TYPE_DYNAMIC_SORTING);

	if (Type==BUFFER_TYPE_DYNAMIC_DX8) {
		Allocate_DX8_Dynamic_Buffer();
	}
	else {
		Allocate_Sorting_Dynamic_Buffer();
	}
}

DynamicVBAccessClass::~DynamicVBAccessClass()
{
	if (Type==BUFFER_TYPE_DYNAMIC_DX8) {
		if (FVFInfo.Get_FVF()==dynamic_fvf_type_nm) {
			_DynamicDX8VertexBufferNMInUse=false;
			_DynamicDX8VertexBufferNMOffset+=(unsigned) VertexCount;
		} else {
			_DynamicDX8VertexBufferInUse=false;
			_DynamicDX8VertexBufferOffset+=(unsigned) VertexCount;
		}
	}
	else {
		_DynamicSortingVertexArrayInUse=false;
		_DynamicSortingVertexArrayOffset+=VertexCount;
	}

	REF_PTR_RELEASE (VertexBuffer);
}

// ----------------------------------------------------------------------------

void DynamicVBAccessClass::_Deinit()
{
	WWASSERT ((_DynamicDX8VertexBuffer == nullptr) || (_DynamicDX8VertexBuffer->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicDX8VertexBuffer);
	_DynamicDX8VertexBufferInUse=false;
	_DynamicDX8VertexBufferSize=DEFAULT_VB_SIZE;
	_DynamicDX8VertexBufferOffset=0;

	WWASSERT ((_DynamicDX8VertexBufferNM == nullptr) || (_DynamicDX8VertexBufferNM->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicDX8VertexBufferNM);
	_DynamicDX8VertexBufferNMInUse=false;
	_DynamicDX8VertexBufferNMSize=DEFAULT_VB_SIZE;
	_DynamicDX8VertexBufferNMOffset=0;

	WWASSERT ((_DynamicSortingVertexArray == nullptr) || (_DynamicSortingVertexArray->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicSortingVertexArray);
	WWASSERT(!_DynamicSortingVertexArrayInUse);
	_DynamicSortingVertexArrayInUse=false;
	_DynamicSortingVertexArraySize=0;
	_DynamicSortingVertexArrayOffset=0;
}

void DynamicVBAccessClass::Allocate_DX8_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	const bool nm=(FVFInfo.Get_FVF()==dynamic_fvf_type_nm);
	WWASSERT(nm ? !_DynamicDX8VertexBufferNMInUse : !_DynamicDX8VertexBufferInUse);
	if (nm) {
		_DynamicDX8VertexBufferNMInUse=true;
	} else {
		_DynamicDX8VertexBufferInUse=true;
	}

	// If requesting more vertices than dynamic vertex buffer can fit, delete the vb
	// and adjust the size to the new count.
	if (nm) {
		if (VertexCount>_DynamicDX8VertexBufferNMSize) {
			REF_PTR_RELEASE(_DynamicDX8VertexBufferNM);
			_DynamicDX8VertexBufferNMSize=VertexCount;
			if (_DynamicDX8VertexBufferNMSize<DEFAULT_VB_SIZE) _DynamicDX8VertexBufferNMSize=DEFAULT_VB_SIZE;
		}
	} else if (VertexCount>_DynamicDX8VertexBufferSize) {
		REF_PTR_RELEASE(_DynamicDX8VertexBuffer);
		_DynamicDX8VertexBufferSize=VertexCount;
		if (_DynamicDX8VertexBufferSize<DEFAULT_VB_SIZE) _DynamicDX8VertexBufferSize=DEFAULT_VB_SIZE;
	}

	// Create a new vb if one doesn't exist currently
	if (nm) {
		if (!_DynamicDX8VertexBufferNM) {
			unsigned usage=DX8VertexBufferClass::USAGE_DYNAMIC;
			if (DX8Wrapper::Get_Current_Caps()->Support_NPatches()) {
				usage|=DX8VertexBufferClass::USAGE_NPATCHES;
			}

			_DynamicDX8VertexBufferNM=NEW_REF(DX8VertexBufferClass,(
				dynamic_fvf_type_nm,
				_DynamicDX8VertexBufferNMSize,
				(DX8VertexBufferClass::UsageType)usage));
			_DynamicDX8VertexBufferNMOffset=0;
		}
	} else if (!_DynamicDX8VertexBuffer) {
		unsigned usage=DX8VertexBufferClass::USAGE_DYNAMIC;
		if (DX8Wrapper::Get_Current_Caps()->Support_NPatches()) {
			usage|=DX8VertexBufferClass::USAGE_NPATCHES;
		}

		_DynamicDX8VertexBuffer=NEW_REF(DX8VertexBufferClass,(
			dynamic_fvf_type,
			_DynamicDX8VertexBufferSize,
			(DX8VertexBufferClass::UsageType)usage));
		_DynamicDX8VertexBufferOffset=0;
	}

	// Any room at the end of the buffer?
	if (nm) {
		if (((unsigned)VertexCount+_DynamicDX8VertexBufferNMOffset)>_DynamicDX8VertexBufferNMSize) {
			_DynamicDX8VertexBufferNMOffset=0;
		}
		REF_PTR_SET(VertexBuffer,_DynamicDX8VertexBufferNM);
		VertexBufferOffset=_DynamicDX8VertexBufferNMOffset;
	} else {
		if (((unsigned)VertexCount+_DynamicDX8VertexBufferOffset)>_DynamicDX8VertexBufferSize) {
			_DynamicDX8VertexBufferOffset=0;
		}
		REF_PTR_SET(VertexBuffer,_DynamicDX8VertexBuffer);
		VertexBufferOffset=_DynamicDX8VertexBufferOffset;
	}
}

void DynamicVBAccessClass::Allocate_Sorting_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicSortingVertexArrayInUse);
	_DynamicSortingVertexArrayInUse=true;

	unsigned new_vertex_count=_DynamicSortingVertexArrayOffset+VertexCount;
	WWASSERT(new_vertex_count<65536);
	if (new_vertex_count>_DynamicSortingVertexArraySize) {
		REF_PTR_RELEASE(_DynamicSortingVertexArray);
		_DynamicSortingVertexArraySize=new_vertex_count;
		if (_DynamicSortingVertexArraySize<DEFAULT_VB_SIZE) _DynamicSortingVertexArraySize=DEFAULT_VB_SIZE;
	}

	if (!_DynamicSortingVertexArray) {
		_DynamicSortingVertexArray=NEW_REF(SortingVertexBufferClass,(_DynamicSortingVertexArraySize));
		_DynamicSortingVertexArrayOffset=0;
	}

	REF_PTR_SET(VertexBuffer,_DynamicSortingVertexArray);
	VertexBufferOffset=_DynamicSortingVertexArrayOffset;
}

// ----------------------------------------------------------------------------
static int dx8_lock;
DynamicVBAccessClass::WriteLockClass::WriteLockClass(DynamicVBAccessClass* dynamic_vb_access_)
	:
	DynamicVBAccess(dynamic_vb_access_), vk_mirrored(false)
{
	DX8_THREAD_ASSERT();
	switch (DynamicVBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX8:
#ifdef VERTEX_BUFFER_LOG
		{
		WWASSERT(!dx8_lock);
		dx8_lock++;
		StringClass fvf_name;
		DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Name(fvf_name);
		WWDEBUG_SAY(("DynamicVertexBuffer->Lock(start_index: %d, index_range: %d, fvf_size: %d, fvf: %s)",
			DynamicVBAccess->VertexBufferOffset,
			DynamicVBAccess->Get_Vertex_Count(),
			DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size(),
			fvf_name));
		}
#endif
		WWASSERT(_DynamicDX8VertexBuffer);
//		WWASSERT(!_DynamicDX8VertexBuffer->Engine_Refs());

		DX8_Assert();
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A: the dynamic pool is one shared buffer
		// that many batches append to within a frame, which is exactly what the rotating backing is for:
		// the whole pool surface is mapped per frame slot, so a batch the game queued three frames ago is
		// still readable while this frame's writes go to their own slot. The pool offset is not folded in
		// here - DrawIndexedPrimitive carries it as BaseVertexIndex and the submit filler passes it on.
		{
			const unsigned fvf_size = DynamicVBAccess->VertexBuffer->FVF_Info().Get_FVF_Size();
			const unsigned geo_size = DynamicVBAccess->VertexBuffer->Get_Vertex_Count() * fvf_size;
			char* slot = (char*)VKGeo_Back_Lock(DynamicVBAccess->VertexBuffer, geo_size, fvf_size, 1,
				DynamicVBAccess->VertexBufferOffset == 0);
			if (slot) {
				vk_mirrored = true;
				Vertices = (VertexFormatXYZNDUV2*)(slot + DynamicVBAccess->VertexBufferOffset * fvf_size);
				break;
			}
		}
#endif
		// Lock with discard contents if the buffer offset is zero
		// GeneralsX @bugfix Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: the stride used to come from
		// _DynamicDX8VertexBuffer - the pool for the *non*-normal-mapped dynamic FVF - while the buffer being
		// locked here is DynamicVBAccess->VertexBuffer, which is _DynamicDX8VertexBufferNM whenever this access
		// is the NM variant. The two pools carry the same FVF so the deref was harmless as long as something had
		// ever allocated the non-NM pool; in a run where only the NM pool exists it is a null this and the process
		// dies reading the FVF pointer. Taking the stride off the buffer that is actually locked is both correct
		// and always valid, and is what the code above already does.
		//
		// The three pointers this call dereferences are taken into locals first. That is the only way a no-d3d9
		// run can name the object it faulted on: the inlined form reports a read at a member offset and says
		// nothing about which pointer was zero. The log line is armed only by GENERALSX_VK_NOD3D, so the DXVK
		// path is unchanged and silent.
		{
			VertexBufferClass* pool_vb = DynamicVBAccess->VertexBuffer;
			IDirect3DVertexBuffer9* dx8_vb = pool_vb ? static_cast<DX8VertexBufferClass*>(pool_vb)->Get_DX8_Vertex_Buffer() : nullptr;
			const unsigned pool_fvf_size = pool_vb ? pool_vb->FVF_Info().Get_FVF_Size() : 0;
			static int nod3d = -1;
			if (nod3d < 0) nod3d = (getenv("GENERALSX_VK_NOD3D") && atoi(getenv("GENERALSX_VK_NOD3D")) != 0) ? 1 : 0;
			static unsigned long lock_calls = 0;
			if (nod3d && ++lock_calls <= 64) {
				// vptr is the qword the virtual call reads before it can dispatch: a zero here means dx8_vb is
				// not a constructed COM object, and the "read of 0x58" the VEH reports is the vtable slot fetch,
				// not the buffer. slot11 is the address the call would jump through.
				void* vptr = dx8_vb ? *(void**)(void*)dx8_vb : (void*)0x1;
				void* slot11 = vptr ? ((void**)(void*)vptr)[11] : nullptr;
				fprintf(stderr, "[VKNOD3D] dynamic VB lock #%lu: access=%p vb=%p dx8vb=%p vptr=%p slot11=%p poolNM=%p "
					"type=%u count=%u offset=%u fvf_size=%u\n",
					lock_calls, (void*)DynamicVBAccess, (void*)pool_vb, (void*)dx8_vb, vptr, slot11,
					(void*)_DynamicDX8VertexBufferNM,
					DynamicVBAccess->Type, DynamicVBAccess->VertexCount, DynamicVBAccess->VertexBufferOffset,
					pool_fvf_size);
				fflush(stderr);
			}
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
			vk_nod3d_dump_dx9_object("pre-lock", lock_calls, dx8_vb);
			VK_NOD3D_Watch_Arm(dx8_vb);
#endif
			DX8_ErrorCode(dx8_vb->Lock(
				DynamicVBAccess->VertexBufferOffset*pool_fvf_size,
				DynamicVBAccess->Get_Vertex_Count()*pool_fvf_size,
				(void**)&Vertices,
				D3DLOCK_NOSYSLOCK | (!DynamicVBAccess->VertexBufferOffset ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE)));
		}
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		Vertices=static_cast<SortingVertexBufferClass*>(DynamicVBAccess->VertexBuffer)->VertexBuffer;
		Vertices+=DynamicVBAccess->VertexBufferOffset;
//		vertices=_DynamicSortingVertexArray+_DynamicSortingVertexArrayOffset;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

DynamicVBAccessClass::WriteLockClass::~WriteLockClass()
{
	DX8_THREAD_ASSERT();
	switch (DynamicVBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX8:
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		if (vk_mirrored) break;	// wedge 5.A: wrote the backing, the D3D9 pool was never locked
#endif
#ifdef VERTEX_BUFFER_LOG
		dx8_lock--;
		WWASSERT(!dx8_lock);
		WWDEBUG_SAY(("DynamicVertexBuffer->Unlock()"));
#endif
		DX8_Assert();
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		{
			// The pair of lines around a lock window is what brackets the writer: if the object is live at
			// pre-lock and dead at post-unlock, the engine's own vertex writes are the suspect.
			static unsigned long unlock_seq = 0;
			vk_nod3d_dump_dx9_object("post-unlock", ++unlock_seq,
				static_cast<DX8VertexBufferClass*>(DynamicVBAccess->VertexBuffer)->Get_DX8_Vertex_Buffer());
		}
#endif
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		VK_NOD3D_Watch_Check("engine dyn unlock");
#endif
		DX8_ErrorCode(static_cast<DX8VertexBufferClass*>(DynamicVBAccess->VertexBuffer)->Get_DX8_Vertex_Buffer()->Unlock());
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

void DynamicVBAccessClass::_Reset(bool frame_changed)
{
	_DynamicSortingVertexArrayOffset=0;
	if (frame_changed) {
		_DynamicDX8VertexBufferOffset=0;
		// GeneralsX @bugfix Qoder 10/10/2026 T3-0 P1: wrap the NM pool per frame
		// too. Without this the NM offset only wrapped on overflow, so each
		// wrap-around overwrote skin vertices still queued in the GPU and
		// skinned units (infantry) flickered or vanished, worse at high fps.
		_DynamicDX8VertexBufferNMOffset=0;
	}
}

unsigned short DynamicVBAccessClass::Get_Default_Vertex_Count()
{
	return _DynamicDX8VertexBufferSize;
}

