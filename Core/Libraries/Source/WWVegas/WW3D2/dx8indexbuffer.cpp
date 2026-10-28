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
 *                     $Archive:: /Commando/Code/ww3d2/dx8indexbuffer.cpp                     $*
 *                                                                                             *
 *              Original Author:: Jani Penttinen                                               *
 *                                                                                             *
 *                      $Author:: Jani_p                                                      $*
 *                                                                                             *
 *                     $Modtime:: 11/09/01 3:12p                                              $*
 *                                                                                             *
 *                    $Revision:: 26                                                          $*
 *                                                                                             *
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

//#define INDEX_BUFFER_LOG

#include "dx8indexbuffer.h"
#include "dx8wrapper.h"
#include "dx8caps.h"
// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: geometry ring bridge (win64vk port tree only)
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
#include "Backend/VulkanGeo.h"
#endif
#include "WWMath/sphere.h"
#include "WWLib/thread.h"
#include "WWDebug/wwmemlog.h"
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9.J2d: the no-d3d9 vptr write watch lives in
// Backend/VKD3DStubImpl.cpp; declared here rather than included, because that header carries the whole D3D9
// stub class set and this TU does not want it.
void VK_NOD3D_Watch_Check(const char* where);
#endif

#define DEFAULT_IB_SIZE 5000

static bool _DynamicSortingIndexArrayInUse=false;
static SortingIndexBufferClass* _DynamicSortingIndexArray;
static unsigned short _DynamicSortingIndexArraySize=0;
static unsigned short _DynamicSortingIndexArrayOffset=0;

static bool _DynamicDX8IndexBufferInUse=false;
static DX8IndexBufferClass* _DynamicDX8IndexBuffer=nullptr;
static unsigned short _DynamicDX8IndexBufferSize=DEFAULT_IB_SIZE;
static unsigned short _DynamicDX8IndexBufferOffset=0;

static int _IndexBufferCount;
static int _IndexBufferTotalIndices;
static int _IndexBufferTotalSize;

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

IndexBufferClass::IndexBufferClass(unsigned type_, unsigned short index_count_)
	:
	index_count(index_count_),
	type(type_),
	engine_refs(0)
{
	WWASSERT(type==BUFFER_TYPE_DX8 || type==BUFFER_TYPE_SORTING);
	WWASSERT(index_count);

	_IndexBufferCount++;
	_IndexBufferTotalIndices+=index_count;
	_IndexBufferTotalSize+=index_count*sizeof(unsigned short);
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A forensics: fvf 0 marks an index buffer in
	// the trace; see the vertex-buffer twin for why birth/death lines are paired with the backing table.
	if (type == BUFFER_TYPE_DX8) VKGeo_Trace_Buffer(this, 0u, index_count, 1);
#endif
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("New IB, %d indices, size %d bytes",index_count,index_count*sizeof(unsigned short)));
	WWDEBUG_SAY(("Total IB count: %d, total %d indices, total size %d bytes",
		_IndexBufferCount,
		_IndexBufferTotalIndices,
		_IndexBufferTotalSize));
#endif
}

IndexBufferClass::~IndexBufferClass()
{
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
	// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.1: drop the per-object backing keyed by
	// 'this'. The address may be recycled by the next IB, so the backing must not outlive the object.
	VKGeo_Back_Drop(this);
	VKGeo_Trace_Buffer(this, 0u, index_count, 0);
#endif
	_IndexBufferCount--;
	_IndexBufferTotalIndices-=index_count;
	_IndexBufferTotalSize-=index_count*sizeof(unsigned short);
#ifdef VERTEX_BUFFER_LOG
	WWDEBUG_SAY(("Delete IB, %d indices, size %d bytes",index_count,index_count*sizeof(unsigned short)));
	WWDEBUG_SAY(("Total IB count: %d, total %d indices, total size %d bytes",
		_IndexBufferCount,
		_IndexBufferTotalIndices,
		_IndexBufferTotalSize));
#endif
}

unsigned IndexBufferClass::Get_Total_Buffer_Count()
{
	return _IndexBufferCount;
}

unsigned IndexBufferClass::Get_Total_Allocated_Indices()
{
	return _IndexBufferTotalIndices;
}

unsigned IndexBufferClass::Get_Total_Allocated_Memory()
{
	return _IndexBufferTotalSize;
}

void IndexBufferClass::Add_Engine_Ref() const
{
	engine_refs++;
}

void IndexBufferClass::Release_Engine_Ref() const
{
	engine_refs--;
	WWASSERT(engine_refs>=0);
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void IndexBufferClass::Copy(unsigned int* indices,unsigned first_index,unsigned count)
{
	WWASSERT(indices);

	if (first_index) {
		DX8IndexBufferClass::AppendLockClass l(this,first_index,count);
		unsigned short* inds=l.Get_Index_Array();
		for (unsigned v=0;v<count;++v) {
			*inds++=(unsigned short)(*indices++);
		}
	}
	else {
		DX8IndexBufferClass::WriteLockClass l(this);
		unsigned short* inds=l.Get_Index_Array();
		for (unsigned v=0;v<count;++v) {
			*inds++=(unsigned short)(*indices++);
		}
	}
}

// ----------------------------------------------------------------------------

void IndexBufferClass::Copy(unsigned short* indices,unsigned first_index,unsigned count)
{
	WWASSERT(indices);

	if (first_index) {
		DX8IndexBufferClass::AppendLockClass l(this,first_index,count);
		unsigned short* inds=l.Get_Index_Array();
		for (unsigned v=0;v<count;++v) {
			*inds++=*indices++;
		}
	}
	else {
		DX8IndexBufferClass::WriteLockClass l(this);
		unsigned short* inds=l.Get_Index_Array();
		for (unsigned v=0;v<count;++v) {
			*inds++=*indices++;
		}
	}
}

// ----------------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------------


IndexBufferClass::WriteLockClass::WriteLockClass(IndexBufferClass* index_buffer_, int flags) : index_buffer(index_buffer_), vk_mirrored(false)
{
	DX8_THREAD_ASSERT();
	WWASSERT(index_buffer);
	WWASSERT(!index_buffer->Engine_Refs());
	index_buffer->Add_Ref();
	switch (index_buffer->Type()) {
	case BUFFER_TYPE_DX8:
		DX8_Assert();
		// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: index data mirrored into the
		// host-visible geometry ring; D3D9 lock skipped while the ring is up (see dx8vertexbuffer.cpp).
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		{
			unsigned geo_size = index_buffer->Get_Index_Count() * sizeof(unsigned short);
			// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.1: per-object backing first - a draw
			// binds the (VkBuffer, offset) of this very index buffer, which wedge 3.1's shared ring cannot
			// offer. The ring stays as the mirror-only fallback while GENERALSX_VKDRAW is off.
			// GeneralsX @bugfix Qoder 24/10/2026: same per-frame-slot rule as the vertex buffer - a
			// USAGE_DYNAMIC index buffer is rewritten every frame (the tree/bush buffer is), so it must
			// not share one region with the frames still in flight.
			const int geo_dynamic =
				(static_cast<DX8IndexBufferClass*>(index_buffer)->Get_Usage() & DX8IndexBufferClass::USAGE_DYNAMIC) ? 1 : 0;
			indices = (unsigned short*)VKGeo_Back_Lock(index_buffer, geo_size, 0, geo_dynamic,
				(flags & D3DLOCK_DISCARD) != 0);
			if (indices) {
				vk_mirrored = true;
				break;
			}
			if (VKGeo_Ready(geo_size)) {
				indices = (unsigned short*)VKGeo_Alloc(geo_size, (flags & D3DLOCK_DISCARD) != 0);
				if (indices) {
					vk_mirrored = true;
					break;
				}
			}
		}
#endif
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		VK_NOD3D_Watch_Check("engine IB writelock");
#endif
		DX8_ErrorCode(static_cast<DX8IndexBufferClass*>(index_buffer)->Get_DX8_Index_Buffer()->Lock(
			0,
			index_buffer->Get_Index_Count()*sizeof(WORD),
			(void**)&indices,
			flags));
		break;
	case BUFFER_TYPE_SORTING:
		indices=static_cast<SortingIndexBufferClass*>(index_buffer)->index_buffer;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------
//
//
// ----------------------------------------------------------------------------

IndexBufferClass::WriteLockClass::~WriteLockClass()
{
	DX8_THREAD_ASSERT();
	switch (index_buffer->Type()) {
	case BUFFER_TYPE_DX8:
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		if (vk_mirrored) break;	// wedge 3.1: ring write, no D3D9 resource was locked
#endif
		DX8_Assert();
		DX8_ErrorCode(static_cast<DX8IndexBufferClass*>(index_buffer)->index_buffer->Unlock());
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	index_buffer->Release_Ref();
}

// ----------------------------------------------------------------------------

IndexBufferClass::AppendLockClass::AppendLockClass(IndexBufferClass* index_buffer_,unsigned start_index, unsigned index_range)
	:
	index_buffer(index_buffer_), vk_mirrored(false)
{
	DX8_THREAD_ASSERT();
	WWASSERT(start_index+index_range<=index_buffer->Get_Index_Count());
	WWASSERT(index_buffer);
	WWASSERT(!index_buffer->Engine_Refs());
	index_buffer->Add_Ref();
	switch (index_buffer->Type()) {
	case BUFFER_TYPE_DX8:
		DX8_Assert();
		// GeneralsX @feature Qoder 17/10/2026 WP1 wedge 3.1: fresh full-buffer region, write at base+start
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		{
			unsigned geo_size = index_buffer->Get_Index_Count() * sizeof(unsigned short);
			// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.1: append writes land in the same
			// per-object backing as the full lock, offset by start_index.
			unsigned short* back_base = (unsigned short*)VKGeo_Back_Lock(index_buffer, geo_size, 0, 0, false);
			if (back_base) {
				vk_mirrored = true;
				indices = back_base + start_index;
				break;
			}
			if (VKGeo_Ready(geo_size)) {
				unsigned short* ring_base = (unsigned short*)VKGeo_Alloc(geo_size, false);
				if (ring_base) {
					vk_mirrored = true;
					indices = ring_base + start_index;
					break;
				}
			}
		}
#endif
		DX8_ErrorCode(static_cast<DX8IndexBufferClass*>(index_buffer)->index_buffer->Lock(
			start_index*sizeof(unsigned short),
			index_range*sizeof(unsigned short),
			(void**)&indices,
			0));
		break;
	case BUFFER_TYPE_SORTING:
		indices=static_cast<SortingIndexBufferClass*>(index_buffer)->index_buffer+start_index;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

// ----------------------------------------------------------------------------

IndexBufferClass::AppendLockClass::~AppendLockClass()
{
	DX8_THREAD_ASSERT();
	switch (index_buffer->Type()) {
	case BUFFER_TYPE_DX8:
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		if (vk_mirrored) break;	// wedge 3.1: ring write, no D3D9 resource was locked
#endif
		DX8_Assert();
		DX8_ErrorCode(static_cast<DX8IndexBufferClass*>(index_buffer)->index_buffer->Unlock());
		break;
	case BUFFER_TYPE_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	index_buffer->Release_Ref();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DX8IndexBufferClass::DX8IndexBufferClass(unsigned short index_count_,UsageType usage)
	:
	IndexBufferClass(BUFFER_TYPE_DX8,index_count_)
{
	DX8_THREAD_ASSERT();
	WWASSERT(index_count);
	m_usage = usage;	// GeneralsX @bugfix Qoder 24/10/2026 see Get_Usage()
	unsigned usage_flags=
		D3DUSAGE_WRITEONLY|
		((usage&USAGE_DYNAMIC) ? D3DUSAGE_DYNAMIC : 0)|
		((usage&USAGE_NPATCHES) ? D3DUSAGE_NPATCHES : 0)|
		((usage&USAGE_SOFTWAREPROCESSING) ? D3DUSAGE_SOFTWAREPROCESSING : 0);
	if (!DX8Wrapper::Get_Current_Caps()->Support_TnL()) {
		usage_flags|=D3DUSAGE_SOFTWAREPROCESSING;
	}

	HRESULT ret=DX8Wrapper::_Get_D3D_Device8()->CreateIndexBuffer(
		sizeof(WORD)*index_count,
		usage_flags,
		D3DFMT_INDEX16,
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&index_buffer, nullptr);

	if (SUCCEEDED(ret)) {
		return;
	}

	WWDEBUG_SAY(("Index buffer creation failed, trying to release assets..."));

	// Index buffer creation failed, so try releasing least used textures and flushing the mesh cache.

	// Free all textures that haven't been used in the last 5 seconds
	TextureClass::Invalidate_Old_Unused_Textures(5000);

	// Invalidate the mesh cache
	WW3D::_Invalidate_Mesh_Cache();

	// Try again...
	ret=DX8Wrapper::_Get_D3D_Device8()->CreateIndexBuffer(
		sizeof(WORD)*index_count,
		usage_flags,
		D3DFMT_INDEX16,
		(usage&USAGE_DYNAMIC) ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED,
		&index_buffer, nullptr);

	if (SUCCEEDED(ret)) {
		WWDEBUG_SAY(("...Index buffer creation successful"));
	}

	// If it still fails it is fatal
	DX8_ErrorCode(ret);
}

// ----------------------------------------------------------------------------

DX8IndexBufferClass::~DX8IndexBufferClass()
{
	index_buffer->Release();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

SortingIndexBufferClass::SortingIndexBufferClass(unsigned short index_count_)
	:
	IndexBufferClass(BUFFER_TYPE_SORTING,index_count_)
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(index_count);

	index_buffer=W3DNEWARRAY unsigned short[index_count];
}

// ----------------------------------------------------------------------------

SortingIndexBufferClass::~SortingIndexBufferClass()
{
	delete[] index_buffer;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DynamicIBAccessClass::DynamicIBAccessClass(unsigned short type_, unsigned short index_count_)
	:
	IndexCount(index_count_),
	IndexBuffer(nullptr),
	Type(type_)
{
	WWASSERT(Type==BUFFER_TYPE_DYNAMIC_DX8 || Type==BUFFER_TYPE_DYNAMIC_SORTING);
	if (Type==BUFFER_TYPE_DYNAMIC_DX8) {
		Allocate_DX8_Dynamic_Buffer();
	}
	else {
		Allocate_Sorting_Dynamic_Buffer();
	}
}

DynamicIBAccessClass::~DynamicIBAccessClass()
{
	REF_PTR_RELEASE(IndexBuffer);
	if (Type==BUFFER_TYPE_DYNAMIC_DX8) {
		_DynamicDX8IndexBufferInUse=false;
		_DynamicDX8IndexBufferOffset+=IndexCount;
	}
	else {
		_DynamicSortingIndexArrayInUse=false;
		_DynamicSortingIndexArrayOffset+=IndexCount;
	}
}

void DynamicIBAccessClass::_Deinit()
{
	WWASSERT ((_DynamicDX8IndexBuffer == nullptr) || (_DynamicDX8IndexBuffer->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicDX8IndexBuffer);
	_DynamicDX8IndexBufferInUse=false;
	_DynamicDX8IndexBufferSize=DEFAULT_IB_SIZE;
	_DynamicDX8IndexBufferOffset=0;

	WWASSERT ((_DynamicSortingIndexArray == nullptr) || (_DynamicSortingIndexArray->Num_Refs() == 1));
	REF_PTR_RELEASE(_DynamicSortingIndexArray);
	_DynamicSortingIndexArrayInUse=false;
	_DynamicSortingIndexArraySize=0;
	_DynamicSortingIndexArrayOffset=0;
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

DynamicIBAccessClass::WriteLockClass::WriteLockClass(DynamicIBAccessClass* ib_access_)
	:
	DynamicIBAccess(ib_access_), vk_mirrored(false)
{
	DX8_THREAD_ASSERT();
	DynamicIBAccess->IndexBuffer->Add_Ref();
	switch (DynamicIBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX8:
		WWASSERT(DynamicIBAccess);
//		WWASSERT(!dynamic_dx8_index_buffer->Engine_Refs());
		DX8_Assert();
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.A: the index pool backing mirrors the
		// whole pool for this frame's slot - Get_Index_Count() on the access object is the batch size,
		// while the draw binds the pool and folds IndexBufferOffset into the start index.
		{
			const unsigned geo_size = DynamicIBAccess->IndexBuffer->Get_Index_Count() * sizeof(WORD);
			char* slot = (char*)VKGeo_Back_Lock(DynamicIBAccess->IndexBuffer, geo_size, 0, 1,
				DynamicIBAccess->IndexBufferOffset == 0);
			if (slot) {
				vk_mirrored = true;
				Indices = (unsigned short*)(slot + DynamicIBAccess->IndexBufferOffset * sizeof(WORD));
				break;
			}
		}
#endif
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		VK_NOD3D_Watch_Check("engine dyn IB lock");
#endif
		DX8_ErrorCode(
			static_cast<DX8IndexBufferClass*>(DynamicIBAccess->IndexBuffer)->Get_DX8_Index_Buffer()->Lock(
			DynamicIBAccess->IndexBufferOffset*sizeof(WORD),
			DynamicIBAccess->Get_Index_Count()*sizeof(WORD),
			(void**)&Indices,
			!DynamicIBAccess->IndexBufferOffset ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE));
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		Indices=static_cast<SortingIndexBufferClass*>(DynamicIBAccess->IndexBuffer)->index_buffer;
		Indices+=DynamicIBAccess->IndexBufferOffset;
		break;
	default:
		WWASSERT(0);
		break;
	}
}

DynamicIBAccessClass::WriteLockClass::~WriteLockClass()
{
	DX8_THREAD_ASSERT();
	switch (DynamicIBAccess->Get_Type()) {
	case BUFFER_TYPE_DYNAMIC_DX8:
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		if (vk_mirrored) break;	// wedge 5.A: wrote the backing, the D3D9 pool was never locked
#endif
		DX8_Assert();
#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)
		VK_NOD3D_Watch_Check("engine dyn IB unlock");
#endif
		DX8_ErrorCode(static_cast<DX8IndexBufferClass*>(DynamicIBAccess->IndexBuffer)->Get_DX8_Index_Buffer()->Unlock());
		break;
	case BUFFER_TYPE_DYNAMIC_SORTING:
		break;
	default:
		WWASSERT(0);
		break;
	}
	DynamicIBAccess->IndexBuffer->Release_Ref();
}

// ----------------------------------------------------------------------------
//
//
//
// ----------------------------------------------------------------------------

void DynamicIBAccessClass::Allocate_DX8_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicDX8IndexBufferInUse);
	_DynamicDX8IndexBufferInUse=true;

	// If requesting more indices than dynamic index buffer can fit, delete the ib
	// and adjust the size to the new count.
	// GeneralsX @bugfix Qoder 20/10/2026 R-08: append instead of wrapping while a 16-bit offset can still
	// address the frame's total. Wrapping relies on D3D9 renaming the pool on DISCARD; the Vulkan mirror
	// has to spend an extra generation to reproduce that, so the common case must not wrap at all.
	bool keep_offset=false;
	const unsigned need=(unsigned)IndexCount+_DynamicDX8IndexBufferOffset;
	if (need>_DynamicDX8IndexBufferSize && need<=65535u) {
		REF_PTR_RELEASE(_DynamicDX8IndexBuffer);
		unsigned grow=(unsigned)_DynamicDX8IndexBufferSize*2u;
		if (grow<need) grow=need;
		if (grow>65535u) grow=65535u;
		_DynamicDX8IndexBufferSize=(unsigned short)grow;
		keep_offset=true;
	}
	if (IndexCount>_DynamicDX8IndexBufferSize) {
		REF_PTR_RELEASE(_DynamicDX8IndexBuffer);
		_DynamicDX8IndexBufferSize=IndexCount;
		if (_DynamicDX8IndexBufferSize<DEFAULT_IB_SIZE) _DynamicDX8IndexBufferSize=DEFAULT_IB_SIZE;
		keep_offset=false;
	}

	// Create a new vb if one doesn't exist currently
	if (!_DynamicDX8IndexBuffer) {
		unsigned usage=DX8IndexBufferClass::USAGE_DYNAMIC;
		if (DX8Wrapper::Get_Current_Caps()->Support_NPatches()) {
			usage|=DX8IndexBufferClass::USAGE_NPATCHES;
		}

		_DynamicDX8IndexBuffer=NEW_REF(DX8IndexBufferClass,(
			_DynamicDX8IndexBufferSize,
			(DX8IndexBufferClass::UsageType)usage));
		if (!keep_offset) _DynamicDX8IndexBufferOffset=0;
	}

	// Any room at the end of the buffer?
	if (((unsigned)IndexCount+_DynamicDX8IndexBufferOffset)>_DynamicDX8IndexBufferSize) {
		// GeneralsX @debug Qoder 20/10/2026 R-08: the index pool has the same DISCARD contract as the
		// vertex pool; log its wrap too so a run can rule the two in or out separately.
		static int trace = -1;
		if (trace < 0) { const char* e = getenv("GENERALSX_VK_VBTRACE"); trace = (e && atoi(e)) ? 1 : 0; }
		if (trace) {
			static unsigned long long wraps = 0;
			if (++wraps <= 400) {
				fprintf(stderr, "[VKIBWRAP] req=%u off=%u size=%u n=%llu\n",
					(unsigned)IndexCount, (unsigned)_DynamicDX8IndexBufferOffset,
					(unsigned)_DynamicDX8IndexBufferSize, wraps);
				fflush(stderr);
			}
		}
		_DynamicDX8IndexBufferOffset=0;
	}

	REF_PTR_SET(IndexBuffer,_DynamicDX8IndexBuffer);
	IndexBufferOffset=_DynamicDX8IndexBufferOffset;
}

void DynamicIBAccessClass::Allocate_Sorting_Dynamic_Buffer()
{
	WWMEMLOG(MEM_RENDERER);
	WWASSERT(!_DynamicSortingIndexArrayInUse);
	_DynamicSortingIndexArrayInUse=true;

	unsigned new_index_count=_DynamicSortingIndexArrayOffset+IndexCount;
	WWASSERT(new_index_count<65536);
	if (new_index_count>_DynamicSortingIndexArraySize) {
		REF_PTR_RELEASE(_DynamicSortingIndexArray);
		_DynamicSortingIndexArraySize=new_index_count;
		if (_DynamicSortingIndexArraySize<DEFAULT_IB_SIZE) _DynamicSortingIndexArraySize=DEFAULT_IB_SIZE;
	}

	if (!_DynamicSortingIndexArray) {
		_DynamicSortingIndexArray=NEW_REF(SortingIndexBufferClass,(_DynamicSortingIndexArraySize));
		_DynamicSortingIndexArrayOffset=0;
	}

	REF_PTR_SET(IndexBuffer,_DynamicSortingIndexArray);
	IndexBufferOffset=_DynamicSortingIndexArrayOffset;
}

void DynamicIBAccessClass::_Reset(bool frame_changed)
{
	_DynamicSortingIndexArrayOffset=0;
	if (frame_changed) _DynamicDX8IndexBufferOffset=0;
}

unsigned short DynamicIBAccessClass::Get_Default_Index_Count()
{
	return _DynamicDX8IndexBufferSize;
}
