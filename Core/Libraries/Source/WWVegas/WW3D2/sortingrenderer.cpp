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
 *                     $Archive:: /Commando/Code/ww3d2/sortingrenderer.cpp                    $*
 *                                                                                             *
 *              Original Author:: Greg Hjelstrom                                               *
 *                                                                                             *
 *                       Author : Kenny Mitchell                                               *
 *                                                                                             *
 *                     $Modtime:: 06/27/02 1:27p                                              $*
 *                                                                                             *
 *                    $Revision:: 2                                                           $*
 *                                                                                             *
 * 06/26/02 KM Matrix name change to avoid MAX conflicts                                       *
 * 06/27/02 KM Changes to max texture stage caps																*
 *---------------------------------------------------------------------------------------------*
 * Functions:                                                                                  *
 * - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#include "sortingrenderer.h"
#include "dx8vertexbuffer.h"
#include "dx8indexbuffer.h"
#include "dx8wrapper.h"
#include "vertmaterial.h"
#include "texture.h"
#include "d3d9.h"
#include "d3dx9math.h"
#include "statistics.h"
#include <WWDebug/wwprofile.h>
#include <algorithm>
#include <list>

#include <chrono>
#include <cstdio>
#include <cstdlib>

// GeneralsX @feature Qoder 19/10/2026 L12 probe V14B (task R-05): splits SortingRendererClass::Flush and
// Flush_Sorting_Pool into segments to locate the ~0.95 ms CPU cost. Gated by GENERALSX_VKFPS_PHASES=1,
// appends [V14B] lines to vkfps_phase14.log every 300 Flushes. Zero cost when the gate is off.
namespace vkf14b {
enum { SEG_SPLICE = 0, SEG_DIRECT, SEG_POOLINS, SEG_VCOPY, SEG_SORT, SEG_CHUNK, SEG_POOL, SEG_RESET, SEG_COUNT };
struct Acc { long double us; int cnt; };
static Acc g_seg[SEG_COUNT];
static int g_ticks = 0;
static const char* g_names[SEG_COUNT] = { "splice", "direct", "poolins", "vcopy", "sort", "chunk", "pool", "reset" };
static inline long double now_us() {
    return (long double)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count() / 1000.0L;
}
static inline bool on() { static int v = -1; if (v < 0) { const char* e = getenv("GENERALSX_VKFPS_PHASES"); v = (e && e[0] == '1') ? 1 : 0; } return v == 1; }
struct Scope { int id; long double t0; bool act; Scope(bool on, int i) : id(i), t0(on ? now_us() : 0.0L), act(on) {} ~Scope() { if (act) { g_seg[id].us += now_us() - t0; g_seg[id].cnt++; } } };
static void dump() {
    FILE* f = fopen("vkfps_phase14.log", "at");
    if (!f) return;
    fprintf(f, "[V14B] win=%d", g_ticks);
    for (int i = 0; i < SEG_COUNT; i++) if (g_seg[i].cnt) fprintf(f, " %s=%.1fus/n=%.2f", g_names[i], (double)(g_seg[i].us / g_ticks), (double)((long double)g_seg[i].cnt / (long double)g_ticks));
    fprintf(f, "\n"); fflush(f); fclose(f);
    for (int i = 0; i < SEG_COUNT; i++) { g_seg[i].us = 0; g_seg[i].cnt = 0; }
    g_ticks = 0;
}
} // namespace vkf14b



bool SortingRendererClass::_EnableTriangleDraw=true;
static unsigned DEFAULT_SORTING_POLY_COUNT = 16384;	// (count * 3) must be less than 65536
static unsigned DEFAULT_SORTING_VERTEX_COUNT = 32768;	// count must be less than 65536

void SortingRendererClass::SetMinVertexBufferSize( unsigned val )
{
	DEFAULT_SORTING_VERTEX_COUNT = val;
	DEFAULT_SORTING_POLY_COUNT = val/2;	//typically have 2:1 vertex:triangle ratio.
}

struct ShortVectorIStruct
{
	unsigned short i;
	unsigned short j;
	unsigned short k;
};

struct TempIndexStruct
{
	ShortVectorIStruct tri;
	unsigned short idx;
	float z;
};

bool operator <(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z < r.z; }
bool operator <=(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z <= r.z; }
bool operator >(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z > r.z; }
bool operator >=(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z >= r.z; }
bool operator ==(const TempIndexStruct &l, const TempIndexStruct &r) { return l.z == r.z; }
// ----------------------------------------------------------------------------
static
void InsertionSort(TempIndexStruct *begin, TempIndexStruct *end)
{
	for (TempIndexStruct *iter = begin + 1; iter < end; ++iter) {
		TempIndexStruct val = iter[0];
		TempIndexStruct *insert = iter;
		while (insert != begin && insert[-1] > val) {
			insert[0] = insert[-1];
			insert -= 1;
		}
		insert[0] = val;
	}
}

// ----------------------------------------------------------------------------
static
void Sort(TempIndexStruct *begin, TempIndexStruct *end)
{
	const int diff = end - begin;
	if (diff <= 16) {
		// Insertion sort has less overhead for small arrays
		InsertionSort(begin, end);
	} else {
		// Choose the median of begin, mid, and (end - 1) as the partitioning element.
		// Rearrange so that *(begin + 1) <= *begin <= *(end - 1).  These will be guard
		// elements.
		TempIndexStruct *mid = begin + diff/2;
		std::swap(mid[0], begin[1]);
		if (begin[1] > end[-1]) {
			std::swap(begin[1], end[-1]);
		}
		if (begin[0] > end[-1]) {
			std::swap(begin[0], end[-1]);
		}
		if (begin[1] > begin[0]) {
			std::swap(begin[1], begin[0]);
		}

		// *begin is now the partitioning element
		TempIndexStruct *begin1 = begin + 1;	// TODO: Temp fix until I find out who is passing me NaN
		TempIndexStruct *end1 = end - 1;			// TODO: Temp fix until I find out who is passing me NaN
		TempIndexStruct *left = begin + 1;
		TempIndexStruct *right = end - 1;
		for (;;) {
#if 0		// TODO: Temp fix until I find out who is passing me NaN.
			do ++left; while (left[0] < begin[0]);		// Scan up to find element >= than partition
			do --right; while (right[0] > begin[0]);	// Scan down to find element <= than partition
#else
			do ++left; while (left < end1 && left[0] < begin[0]);		// Scan up to find element >= than partition
			do --right; while (right > begin1 && right[0] > begin[0]);	// Scan down to find element <= than partition
#endif
			if (right < left) break;									// Pointers crossed.  Partitioning completed.
			std::swap(left[0], right[0]);							// Exchange elements.
		}
		std::swap(begin[0], right[0]);							// Insert partition element

		// Sort the smaller subarray first then the larger
		if (right - begin > end - (right + 1)) {
			Sort(right + 1, end);
			Sort(begin, right);
		} else {
			Sort(begin, right);
			Sort(right + 1, end);
		}
	}
}

// ----------------------------------------------------------------------------

class SortingNodeStruct
{
	W3DMPO_CODE(SortingNodeStruct)

public:
	RenderStateStruct sorting_state;

	Vector3 transformed_center;
	unsigned short start_index;			// First index used in the ib
	unsigned short polygon_count;			// Polygon count to process (3 indices = one polygon)
	unsigned short min_vertex_index;		// First index used in the vb
	unsigned short vertex_count;			// Number of vertices used in vb
};

typedef std::list<SortingNodeStruct*> SortingNodeStructList;
static SortingNodeStructList sorted_list;
static SortingNodeStructList unsorted_list;
static SortingNodeStructList clean_list;
static unsigned total_sorting_vertices;

static SortingNodeStruct* Get_Sorting_Struct()
{
	if (!clean_list.empty()) {
		SortingNodeStruct* state = clean_list.front();
		clean_list.pop_front();
		return state;
	}
	return W3DNEW SortingNodeStruct();
}

// ----------------------------------------------------------------------------
//
// Temporary arrays for the sorting system
//
// ----------------------------------------------------------------------------

static TempIndexStruct* temp_index_array;
static unsigned temp_index_array_count;

static TempIndexStruct* Get_Temp_Index_Array(unsigned count)
{
	if (count < DEFAULT_SORTING_POLY_COUNT)
		count = DEFAULT_SORTING_POLY_COUNT;
	if (count>temp_index_array_count) {
		delete[] temp_index_array;
		temp_index_array=W3DNEWARRAY TempIndexStruct[count];
		temp_index_array_count=count;
	}
	return temp_index_array;
}

// ----------------------------------------------------------------------------
//
// Insert triangles to the sorting system.
//
// ----------------------------------------------------------------------------

void SortingRendererClass::Insert_Triangles(
	const SphereClass& bounding_sphere,
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	if (!WW3D::Is_Sorting_Enabled()) {
		DX8Wrapper::Draw_Triangles(start_index,polygon_count,min_vertex_index,vertex_count);
		return;
	}

	SNAPSHOT_SAY(("SortingRenderer::Insert(start_i: %d, polygons : %d, min_vi: %d, vertex_count: %d)",
		start_index,polygon_count,min_vertex_index,vertex_count));


	DX8_RECORD_SORTING_RENDER(polygon_count,vertex_count);

	SortingNodeStruct* state=Get_Sorting_Struct();

	DX8Wrapper::Get_Render_State(state->sorting_state);

 	WWASSERT(
		((state->sorting_state.index_buffer_type==BUFFER_TYPE_SORTING || state->sorting_state.index_buffer_type==BUFFER_TYPE_DYNAMIC_SORTING) &&
		(state->sorting_state.vertex_buffer_types[0]==BUFFER_TYPE_SORTING || state->sorting_state.vertex_buffer_types[0]==BUFFER_TYPE_DYNAMIC_SORTING)));


	state->start_index=start_index;
	state->polygon_count=polygon_count;
	state->min_vertex_index=min_vertex_index;
	state->vertex_count=vertex_count;

	if (bounding_sphere.Is_Valid())
	{
		D3DXMATRIX mtx=(D3DXMATRIX&)state->sorting_state.world*(D3DXMATRIX&)state->sorting_state.view;
		D3DXVECTOR3 vec=(D3DXVECTOR3&)bounding_sphere.Center;
		D3DXVECTOR4 transformed_vec;
		D3DXVec3Transform(
			&transformed_vec,
			&vec,
			&mtx);
		state->transformed_center=Vector3(transformed_vec[0],transformed_vec[1],transformed_vec[2]);

		Insert_To_Sorted_List(state);
	}
	else
	{
		// TheSuperHackers @perf stephanmeesters 04/07/2026 Nodes without bounding information do not require sorting.
		state->transformed_center = Vector3(0.0f, 0.0f, 0.0f);
		unsorted_list.push_back(state);
	}

#ifdef DEBUG_CRASHING
	SortingVertexBufferClass* vertex_buffer=static_cast<SortingVertexBufferClass*>(state->sorting_state.vertex_buffers[0]);
	WWASSERT(vertex_buffer);
	WWASSERT(state->vertex_count<=vertex_buffer->Get_Vertex_Count());

	unsigned short* indices=nullptr;
	SortingIndexBufferClass* index_buffer=static_cast<SortingIndexBufferClass*>(state->sorting_state.index_buffer);
	WWASSERT(index_buffer);
	indices=index_buffer->index_buffer;
	WWASSERT(indices);
	indices+=state->start_index;
	indices+=state->sorting_state.iba_offset;

	for (int i=0;i<state->polygon_count;++i) {
		unsigned short idx1=indices[i*3]-state->min_vertex_index;
		unsigned short idx2=indices[i*3+1]-state->min_vertex_index;
		unsigned short idx3=indices[i*3+2]-state->min_vertex_index;
		WWASSERT(idx1<state->vertex_count);
		WWASSERT(idx2<state->vertex_count);
		WWASSERT(idx3<state->vertex_count);
	}
#endif // DEBUG_CRASHING
}

// ----------------------------------------------------------------------------
//
// Insert triangles to the sorting system, with no bounding information.
//
// ----------------------------------------------------------------------------

void SortingRendererClass::Insert_Triangles(
	unsigned short start_index,
	unsigned short polygon_count,
	unsigned short min_vertex_index,
	unsigned short vertex_count)
{
	Insert_Triangles(SphereClass(),start_index,polygon_count,min_vertex_index,vertex_count);
}

// ----------------------------------------------------------------------------
//
// Flush all sorting polygons.
//
// ----------------------------------------------------------------------------

void Release_Refs(SortingNodeStruct* state)
{
	int i;
	for (i=0;i<MAX_VERTEX_STREAMS;++i) {
		REF_PTR_RELEASE(state->sorting_state.vertex_buffers[i]);
	}
	REF_PTR_RELEASE(state->sorting_state.index_buffer);
	REF_PTR_RELEASE(state->sorting_state.material);
	for (i=0;i<DX8Wrapper::Get_Current_Caps()->Get_Max_Textures_Per_Pass();++i)
	{
		REF_PTR_RELEASE(state->sorting_state.Textures[i]);
	}
}

static unsigned overlapping_node_count;
static unsigned overlapping_polygon_count;
static unsigned overlapping_vertex_count;
static const unsigned MAX_OVERLAPPING_NODES=4096;
static SortingNodeStruct* overlapping_nodes[MAX_OVERLAPPING_NODES];

// ----------------------------------------------------------------------------

void SortingRendererClass::Insert_To_Sorted_List(SortingNodeStruct *state)
{
	/// @todo lorenzen sez use a bucket sort here... and stop copying so much data so many times

	for (SortingNodeStructList::iterator node = sorted_list.begin(); node != sorted_list.end(); ++node)
	{
		if (state->transformed_center.Z > (*node)->transformed_center.Z) {
			sorted_list.insert(node, state);
			return;
		}
	}

	sorted_list.push_back(state);
}

// ----------------------------------------------------------------------------

void SortingRendererClass::Insert_To_Sorting_Pool(SortingNodeStruct* state)
{
	if (overlapping_node_count>=MAX_OVERLAPPING_NODES) {
		// GeneralsX @debug Qoder 21/10/2026 PROPDROP: confirm whether the rotor blur nodes are
		// the ones falling off the fixed-size sorted pool (4096 nodes/frame).
		static int s_propDrop = 0;
		const char* tn = (state->sorting_state.Textures[0])
			? (const char*)state->sorting_state.Textures[0]->Get_Texture_Name() : nullptr;
		if (s_propDrop < 8 && tn && strstr(tn, "avcomanche_p")) {
			++s_propDrop;
			fprintf(stderr, "[PROPDROP] tex='%s' at node=%u polys=%u verts=%u\n",
				tn, overlapping_node_count, state->polygon_count, state->vertex_count);
			fflush(stderr);
		}
		Release_Refs(state);
		delete state;
		WWASSERT(0);
		return;
	}

	overlapping_nodes[overlapping_node_count]=state;
	overlapping_vertex_count+=state->vertex_count;
	overlapping_polygon_count+=state->polygon_count;
	overlapping_node_count++;
}

// ----------------------------------------------------------------------------
//static unsigned prevLight = 0xffffffff;

static void Apply_Render_State(RenderStateStruct& render_state)
{
	DX8Wrapper::Set_Shader(render_state.shader);

	DX8Wrapper::Set_Material(render_state.material);

	for (int i=0;i<DX8Wrapper::Get_Current_Caps()->Get_Max_Textures_Per_Pass();++i)
	{
		DX8Wrapper::Set_Texture(i,render_state.Textures[i]);
	}

	DX8Wrapper::_Set_DX8_Transform(D3DTS_WORLD,render_state.world);
	DX8Wrapper::_Set_DX8_Transform(D3DTS_VIEW,render_state.view);


	if (!render_state.material->Get_Lighting())
		return;	//no point changing lights if they are ignored.
  //prevLight = render_state.lightsHash;

	if (render_state.LightEnable[0]) {
		DX8Wrapper::Set_DX8_Light(0,&render_state.Lights[0]);
		if (render_state.LightEnable[1]) {
			DX8Wrapper::Set_DX8_Light(1,&render_state.Lights[1]);
			if (render_state.LightEnable[2]) {
				DX8Wrapper::Set_DX8_Light(2,&render_state.Lights[2]);
				if (render_state.LightEnable[3]) {
					DX8Wrapper::Set_DX8_Light(3,&render_state.Lights[3]);
				}
				else {
					DX8Wrapper::Set_DX8_Light(3,nullptr);
				}
			}
			else {
				DX8Wrapper::Set_DX8_Light(2,nullptr);
			}
		}
		else {
			DX8Wrapper::Set_DX8_Light(1,nullptr);
		}
	}
	else {
		DX8Wrapper::Set_DX8_Light(0,nullptr);
	}


}

// ----------------------------------------------------------------------------

void SortingRendererClass::Flush_Sorting_Pool()
{
	{
		static int s_poolEnter = 0;
		if (s_poolEnter < 12) {
			++s_poolEnter;
			fprintf(stderr, "[PROPPOOLIN] nodes=%u polys=%u verts=%u\n",
				overlapping_node_count, overlapping_polygon_count, overlapping_vertex_count);
			fflush(stderr);
		}
	}
	if (!overlapping_node_count) return;
	const bool vk14p = vkf14b::on();
	if (vk14p) vkf14b::g_seg[vkf14b::SEG_POOLINS].cnt += overlapping_node_count;

	SNAPSHOT_SAY(("SortingSystem - Flush"));

	// Fill dynamic index buffer with sorting index buffer vertices
	TempIndexStruct* tis=Get_Temp_Index_Array(overlapping_polygon_count);

	const long double t_vcopy = vk14p ? vkf14b::now_us() : 0.0L;
	unsigned vertexAllocCount = overlapping_vertex_count;
	if (DynamicVBAccessClass::Get_Default_Vertex_Count() < DEFAULT_SORTING_VERTEX_COUNT)
		vertexAllocCount = DEFAULT_SORTING_VERTEX_COUNT;	//make sure that we force the DX8 dynamic vertex buffer to maximum size
	if (overlapping_vertex_count > vertexAllocCount)
		vertexAllocCount = overlapping_vertex_count;
	WWASSERT(DEFAULT_SORTING_VERTEX_COUNT == 1 || vertexAllocCount <= DEFAULT_SORTING_VERTEX_COUNT);
	DynamicVBAccessClass dyn_vb_access(BUFFER_TYPE_DYNAMIC_DX8,dynamic_fvf_type,vertexAllocCount/*overlapping_vertex_count*/);
	{
		DynamicVBAccessClass::WriteLockClass lock(&dyn_vb_access);
		VertexFormatXYZNDUV2* dest_verts=(VertexFormatXYZNDUV2 *)lock.Get_Formatted_Vertex_Array();

		unsigned polygon_array_offset=0;
		unsigned vertex_array_offset=0;
		for (unsigned node_id=0;node_id<overlapping_node_count;++node_id) {
			SortingNodeStruct* state=overlapping_nodes[node_id];
			VertexFormatXYZNDUV2* src_verts=nullptr;
			SortingVertexBufferClass* vertex_buffer=static_cast<SortingVertexBufferClass*>(state->sorting_state.vertex_buffers[0]);
			WWASSERT(vertex_buffer);
			src_verts=vertex_buffer->VertexBuffer;
			WWASSERT(src_verts);
			src_verts+=state->sorting_state.vba_offset;
			src_verts+=state->sorting_state.index_base_offset;
			src_verts+=state->min_vertex_index;

			// If you have a crash in here and "dest_verts" points to illegal memory area,
			// it is because D3D is in illegal state, and the only known cure is rebooting.
			// This illegal state is usually caused by Quake3-engine powered games such as MOHAA.
			memcpy(dest_verts, src_verts, sizeof(VertexFormatXYZNDUV2)*state->vertex_count);

			// GeneralsX @debug 24/10/2026 ROTORPROBE: the rotor blur discs reach the pool and are flushed
			// but still do not show on screen, so dump the geometry the batch will draw and the state it
			// will be drawn with (cull / depth / alpha test / vertex diffuse).
			{
				static int s_rotorProbe = 0;
				const char *rn = state->sorting_state.Textures[0]
					? (const char *)state->sorting_state.Textures[0]->Get_Texture_Name() : nullptr;
				if (s_rotorProbe < 6 && rn && strstr(rn, "avcomanche_p")) {
					++s_rotorProbe;
					const D3DXMATRIX &w = (const D3DXMATRIX &)state->sorting_state.world;
					fprintf(stderr,
						"[ROTORPROBE] #%d polys=%u verts=%u start=%u minv=%u vba=%u iba=%u "
						"cull=%u zcmp=%u zwrite=%u atest=%u | "
						"d0=%08x d1=%08x d2=%08x | v0=(%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f) v2=(%.2f,%.2f,%.2f) "
						"| world=(%.1f,%.1f,%.1f) view=(%.1f,%.1f,%.1f)\n",
						s_rotorProbe, (unsigned)state->polygon_count, (unsigned)state->vertex_count,
						(unsigned)state->start_index, (unsigned)state->min_vertex_index,
						(unsigned)state->sorting_state.vba_offset, (unsigned)state->sorting_state.iba_offset,
						(unsigned)state->sorting_state.shader.Get_Cull_Mode(),
						(unsigned)state->sorting_state.shader.Get_Depth_Compare(),
						(unsigned)state->sorting_state.shader.Get_Depth_Mask(),
						(unsigned)state->sorting_state.shader.Get_Alpha_Test(),
						(unsigned)src_verts[0].diffuse, (unsigned)src_verts[1].diffuse, (unsigned)src_verts[2].diffuse,
						(double)src_verts[0].x, (double)src_verts[0].y, (double)src_verts[0].z,
						(double)src_verts[1].x, (double)src_verts[1].y, (double)src_verts[1].z,
						(double)src_verts[2].x, (double)src_verts[2].y, (double)src_verts[2].z,
						(double)w._41, (double)w._42, (double)w._43,
						(double)state->sorting_state.view[3][0], (double)state->sorting_state.view[3][1],
						(double)state->sorting_state.view[3][2]);
					fflush(stderr);

					// GeneralsX @debug 25/10/2026 ROTORUV: the last unknown of the invisible rotor disc is
					// the alpha the fixed-function sampler will read. Print the stage-0 coordinate source and
					// decode the BC3 alpha texel under the first three vertices straight from the managed D3D
					// texture, so "wrong UV set" and "alpha is really zero" stop looking alike.
					{
						TextureBaseClass* rtb = state->sorting_state.Textures[0];
						TextureClass* rtc = rtb ? rtb->As_TextureClass() : nullptr;
						const unsigned tci0 = DX8Wrapper::VK_Read_TSS(0, D3DTSS_TEXCOORDINDEX, 0);
						// GeneralsX @debug 25/10/2026 ROTORW: the address and first vertices the pool just
						// wrote. Compared with ROTORBUF's read address at draw time this separates "the
						// draw binds another region" from "the region was overwritten in between".
						fprintf(stderr, "[ROTORW] #%d dest=%p v0=(%.2f,%.2f,%.2f) v1=(%.2f,%.2f,%.2f) uv1=(%.3f,%.3f)\n",
							s_rotorProbe, (const void*)dest_verts,
							(double)dest_verts[0].x, (double)dest_verts[0].y, (double)dest_verts[0].z,
							(double)dest_verts[1].x, (double)dest_verts[1].y, (double)dest_verts[1].z,
							(double)dest_verts[0].u1, (double)dest_verts[0].v1);
						fflush(stderr);

						fprintf(stderr, "[ROTORUV] #%d tci0=%u fmt=%d %dx%d uv0=(%.3f,%.3f) uv1=(%.3f,%.3f) uv2=(%.3f,%.3f)\n",
							s_rotorProbe, tci0, rtc ? (int)rtc->Get_Texture_Format() : -1,
							rtc ? rtc->Get_Width() : 0, rtc ? rtc->Get_Height() : 0,
							(double)src_verts[0].u1, (double)src_verts[0].v1,
							(double)src_verts[1].u1, (double)src_verts[1].v1,
							(double)src_verts[2].u1, (double)src_verts[2].v1);
						IDirect3DTexture9* rd3dt = rtc ? rtc->Peek_D3D_Texture() : nullptr;
						D3DLOCKED_RECT rlr;
						if (rd3dt && SUCCEEDED(rd3dt->LockRect(0, &rlr, nullptr, D3DLOCK_READONLY))) {
							const unsigned char* rbase = (const unsigned char*)rlr.pBits;
							const int rw = rtc->Get_Width(), rh = rtc->Get_Height();
							for (int vi = 0; vi < 3; ++vi) {
								const float uu = src_verts[vi].u1, vv = src_verts[vi].v1;
								if (uu < 0.0f || uu > 1.0f || vv < 0.0f || vv > 1.0f) {
									fprintf(stderr, "[ROTORUV]   v%d uv outside [0,1]\n", vi);
									continue;
								}
								const int px = (int)(uu * (float)(rw - 1) + 0.5f);
								const int py = (int)(vv * (float)(rh - 1) + 0.5f);
								const unsigned char* blk = rbase + (py / 4) * rlr.Pitch + (px / 4) * 16;
								const unsigned a0 = blk[0], a1 = blk[1];
								unsigned long long abits = 0ull;
								for (int b = 0; b < 6; ++b) abits |= (unsigned long long)blk[2 + b] << (8 * b);
								const unsigned aidx = (unsigned)((abits >> (3 * ((px & 3) + (py & 3) * 4))) & 7u);
								int aval;
								if (a0 > a1) {
									if (aidx == 0) aval = (int)a0;
									else if (aidx == 1) aval = (int)a1;
									else aval = (int)(((8u - aidx) * a0 + (aidx - 1u) * a1) / 7u);
								} else {
									if (aidx == 0) aval = (int)a0;
									else if (aidx == 1) aval = (int)a1;
									else if (aidx == 6) aval = 0;
									else if (aidx == 7) aval = 255;
									else aval = (int)(((6u - aidx) * a0 + (aidx - 1u) * a1) / 5u);
								}
								fprintf(stderr, "[ROTORUV]   v%d px=(%d,%d) a0=%u a1=%u idx=%u alpha=%d\n",
									vi, px, py, a0, a1, aidx, aval);
							}
							rd3dt->UnlockRect(0);
						} else {
							fprintf(stderr, "[ROTORUV]   texture lock failed\n");
						}
						fflush(stderr);
					}

					// Where is that disc on screen? Camera-space centre through world*view, then NDC
					// through the projection diagonal (a transpose does not touch [0][0]/[1][1]).
					{
						D3DXMATRIX wm = (const D3DXMATRIX &)state->sorting_state.world;
						D3DXMATRIX vm = (const D3DXMATRIX &)state->sorting_state.view;
						D3DXMATRIX wv = wm * vm;
						D3DXVECTOR4 c;
						D3DXVECTOR3 origin(0.0f, 0.0f, 0.0f);
						D3DXVec3Transform(&c, &origin, &wv);
						Matrix4x4 pm4;
						DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, pm4);
						const float p00 = pm4[0][0], p11 = pm4[1][1];
						const float ndcx = (c.w > 1e-4f) ? (c.x * p00 / c.w) : 999.0f;
						const float ndcy = (c.w > 1e-4f) ? (c.y * p11 / c.w) : 999.0f;
						fprintf(stderr, "[ROTORSCREEN] cam=(%.1f,%.1f,%.1f) p00=%.4f p11=%.4f ndc=(%.3f,%.3f)\n",
							(double)c.x, (double)c.y, (double)c.w, (double)p00, (double)p11,
							(double)ndcx, (double)ndcy);
						fflush(stderr);
						fprintf(stderr, "[ROTORWORLD] r0=(%.3f,%.3f,%.3f) r1=(%.3f,%.3f,%.3f) r2=(%.3f,%.3f,%.3f) r3=(%.3f,%.3f,%.3f)\n",
							(double)w._11, (double)w._12, (double)w._13,
							(double)w._21, (double)w._22, (double)w._23,
							(double)w._31, (double)w._32, (double)w._33,
							(double)w._41, (double)w._42, (double)w._43);
						fflush(stderr);
					}
				}
			}
			// GeneralsX @debug 24/10/2026 ROTORPROBE: solid magenta vertices for the debug draw above.
			{
				static int s_rotorTint = -1;
				if (s_rotorTint < 0) s_rotorTint = getenv("GENERALSX_ROTOR_DEBUG") ? 1 : 0;
				const char *tn = state->sorting_state.Textures[0]
					? (const char *)state->sorting_state.Textures[0]->Get_Texture_Name() : nullptr;
				if (s_rotorTint && tn && strstr(tn, "avcomanche_p")) {
					for (unsigned v = 0; v < state->vertex_count; ++v)
						dest_verts[v].diffuse = 0xFFFF00FFu;
				}
			}
			dest_verts += state->vertex_count;

			D3DXMATRIX d3d_mtx=(D3DXMATRIX&)state->sorting_state.world*(D3DXMATRIX&)state->sorting_state.view;
			const Matrix4x4& mtx=(const Matrix4x4&)d3d_mtx;

			unsigned short* indices=nullptr;
			SortingIndexBufferClass* index_buffer=static_cast<SortingIndexBufferClass*>(state->sorting_state.index_buffer);
			WWASSERT(index_buffer);
			indices=index_buffer->index_buffer;
			WWASSERT(indices);
			indices+=state->start_index;
			indices+=state->sorting_state.iba_offset;

			if (mtx[0][2] == 0.0f && mtx[1][2] == 0.0f && mtx[3][2] == 0.0f && mtx[2][2] == 1.0f) {
				// The common case for particle systems.
				for (int i=0;i<state->polygon_count;++i) {
					unsigned short idx1=indices[i*3]-state->min_vertex_index;
					unsigned short idx2=indices[i*3+1]-state->min_vertex_index;
					unsigned short idx3=indices[i*3+2]-state->min_vertex_index;
					WWASSERT(idx1<state->vertex_count);
					WWASSERT(idx2<state->vertex_count);
					WWASSERT(idx3<state->vertex_count);
					const VertexFormatXYZNDUV2 *v1 = src_verts + idx1;
					const VertexFormatXYZNDUV2 *v2 = src_verts + idx2;
					const VertexFormatXYZNDUV2 *v3 = src_verts + idx3;
					unsigned array_index=i+polygon_array_offset;
					WWASSERT(array_index<overlapping_polygon_count);
					TempIndexStruct *tis_ptr = tis + array_index;
					tis_ptr->tri.i = idx1 + vertex_array_offset;
					tis_ptr->tri.j = idx2 + vertex_array_offset;
					tis_ptr->tri.k = idx3 + vertex_array_offset;
					tis_ptr->idx = node_id;
					tis_ptr->z = (v1->z + v2->z + v3->z)/3.0f;
					DEBUG_ASSERTCRASH((! _isnan(tis_ptr->z) && _finite(tis_ptr->z)), ("Triangle has invalid center"));
				}
			} else {
				for (int i=0;i<state->polygon_count;++i) {
					unsigned short idx1=indices[i*3]-state->min_vertex_index;
					unsigned short idx2=indices[i*3+1]-state->min_vertex_index;
					unsigned short idx3=indices[i*3+2]-state->min_vertex_index;
					WWASSERT(idx1<state->vertex_count);
					WWASSERT(idx2<state->vertex_count);
					WWASSERT(idx3<state->vertex_count);
					const VertexFormatXYZNDUV2 *v1 = src_verts + idx1;
					const VertexFormatXYZNDUV2 *v2 = src_verts + idx2;
					const VertexFormatXYZNDUV2 *v3 = src_verts + idx3;
					unsigned array_index=i+polygon_array_offset;
					WWASSERT(array_index<overlapping_polygon_count);
					TempIndexStruct *tis_ptr = tis + array_index;
					tis_ptr->tri.i = idx1 + vertex_array_offset;
					tis_ptr->tri.j = idx2 + vertex_array_offset;
					tis_ptr->tri.k = idx3 + vertex_array_offset;
					tis_ptr->idx = node_id;
					tis_ptr->z = (mtx[0][2]*(v1->x + v2->x + v3->x) +
												mtx[1][2]*(v1->y + v2->y + v3->y) +
												mtx[2][2]*(v1->z + v2->z + v3->z))/3.0f + mtx[3][2];
					DEBUG_ASSERTCRASH((! _isnan(tis_ptr->z) && _finite(tis_ptr->z)), ("Triangle has invalid center"));
				}
			}

			state->min_vertex_index=vertex_array_offset;

			polygon_array_offset+=state->polygon_count;
			vertex_array_offset+=state->vertex_count;
		}
	}
	if (vk14p) { vkf14b::g_seg[vkf14b::SEG_VCOPY].us += vkf14b::now_us() - t_vcopy; vkf14b::g_seg[vkf14b::SEG_VCOPY].cnt++; }

	// GeneralsX @debug 24/10/2026 ROTORPROBE: A/B only the rotor discs' cull bit, so one run says
	// whether the invisible propeller is a winding/cull decision or something downstream of it.
	{
		static int s_rotorNoCull = -1;
		if (s_rotorNoCull < 0) s_rotorNoCull = getenv("GENERALSX_ROTOR_NOCULL") ? 1 : 0;
		if (s_rotorNoCull) {
			for (unsigned n = 0; n < overlapping_node_count; ++n) {
				SortingNodeStruct *st = overlapping_nodes[n];
				const char *tn = st->sorting_state.Textures[0]
					? (const char *)st->sorting_state.Textures[0]->Get_Texture_Name() : nullptr;
				if (tn && strstr(tn, "avcomanche_p"))
					st->sorting_state.shader.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);
			}
		}
	}

	if (vk14p) { vkf14b::Scope s(vk14p, vkf14b::SEG_SORT); (void)s;
	Sort(tis, tis + overlapping_polygon_count); }

	// GeneralsX @debug 24/10/2026 ROTORPROBE: untextured magenta rotor discs, opaque and unculled.
	// If nothing magenta appears the batch never reaches the raster at all; if it does, the invisible
	// propeller is a texture/alpha/lighting problem, not geometry or culling.
	{
		static int s_rotorDebug = -1;
		if (s_rotorDebug < 0) s_rotorDebug = getenv("GENERALSX_ROTOR_DEBUG") ? 1 : 0;
		if (s_rotorDebug) {
			for (unsigned n = 0; n < overlapping_node_count; ++n) {
				SortingNodeStruct *st = overlapping_nodes[n];
				const char *tn = st->sorting_state.Textures[0]
					? (const char *)st->sorting_state.Textures[0]->Get_Texture_Name() : nullptr;
				if (!tn || !strstr(tn, "avcomanche_p")) continue;
				st->sorting_state.shader = ShaderClass::_PresetOpaqueSolidShader;
				st->sorting_state.shader.Set_Cull_Mode(ShaderClass::CULL_MODE_DISABLE);
				st->sorting_state.shader.Set_Depth_Mask(ShaderClass::DEPTH_WRITE_DISABLE);
				st->sorting_state.shader.Set_Depth_Compare(ShaderClass::PASS_ALWAYS);
				// GeneralsX @debug 25/10/2026 ROTORFLAT: vertex tint alone was not visible because the
				// solid preset routes the diffuse material source, not the vertex colour. Paint the
				// material itself so a magenta disc proves the batch reaches the rasteriser.
				if (st->sorting_state.material) {
					st->sorting_state.material->Set_Diffuse(1.0f, 0.0f, 1.0f);
					st->sorting_state.material->Set_Ambient(1.0f, 0.0f, 1.0f);
				}
				// Where does the disc land? Two-step world->view so no matrix-product convention is
				// guessed at, then NDC through the projection diagonal (transpose-invariant).
				{
					static unsigned s_ndcTick = 0, s_ndcLog = 0;
					if ((++s_ndcTick % 45u) == 0u && s_ndcLog < 60u) {
						++s_ndcLog;
						D3DXMATRIX wm = (const D3DXMATRIX &)st->sorting_state.world;
						D3DXMATRIX vm = (const D3DXMATRIX &)st->sorting_state.view;
						D3DXVECTOR3 o(0.0f, 0.0f, 0.0f);
						D3DXVECTOR4 wp;
						D3DXVec3Transform(&wp, &o, &wm);
						D3DXVECTOR3 wp3(wp.x, wp.y, wp.z);
						D3DXVECTOR4 cp;
						D3DXVec3Transform(&cp, &wp3, &vm);
						Matrix4x4 pm4;
						DX8Wrapper::_Get_DX8_Transform(D3DTS_PROJECTION, pm4);
						const float p00 = pm4[0][0], p11 = pm4[1][1];
						const float ndcx = (cp.z > 1e-4f) ? (cp.x * p00 / cp.z) : 999.0f;
						const float ndcy = (cp.z > 1e-4f) ? (cp.y * p11 / cp.z) : 999.0f;
						fprintf(stderr, "[ROTORNDC] cam=(%.1f,%.1f,%.1f) p00=%.3f p11=%.3f ndc=(%.2f,%.2f)\n",
							(double)cp.x, (double)cp.y, (double)cp.z,
							(double)p00, (double)p11, (double)ndcx, (double)ndcy);
						fflush(stderr);
					}
				}
				// White stage-0 tap so MODULATE(texture, vertexColour) resolves to the tinted vertices.
				REF_PTR_RELEASE(st->sorting_state.Textures[0]);
			}
		}
	}

	// TheSuperHackers @fix stephanmeesters 10/06/2026
	// Split rendering into chunks to prevent a crash when exceeding the 16-bit index buffer limit.
	constexpr const unsigned MAX_INDEX_CHUNK = 65535;
	unsigned chunkOffset = 0;
	const long double t_chunk = vk14p ? vkf14b::now_us() : 0.0L;
	while (chunkOffset < overlapping_polygon_count)
	{
		unsigned chunkCount = overlapping_polygon_count - chunkOffset;
		if (chunkCount * 3 > MAX_INDEX_CHUNK) {
			chunkCount = MAX_INDEX_CHUNK / 3;
		}
		const unsigned chunkEnd = chunkOffset + chunkCount;

		DynamicIBAccessClass dyn_ib_access(BUFFER_TYPE_DYNAMIC_DX8,chunkCount*3);
		{
			DynamicIBAccessClass::WriteLockClass lock(&dyn_ib_access);
			ShortVectorIStruct* sorted_polygon_index_array=(ShortVectorIStruct*)lock.Get_Index_Array();

			for (unsigned a=0;a<chunkCount;++a) {
				sorted_polygon_index_array[a]=tis[chunkOffset + a].tri;
			}
		}

		// GeneralsX @debug Qoder 21/10/2026 PROPFLUSH: the Chinook rotor blur (PROPS01/02) is a
		// sorted SRC_ALPHA/INV_SRC_ALPHA mesh that never reaches the screen. Confirm whether its
		// nodes are actually flushed and drawn, and with which state, before touching the blend path.
		{
			static int s_propFlush = 0;
			for (unsigned a = chunkOffset; a < chunkEnd && s_propFlush < 8; ++a) {
				SortingNodeStruct* st = overlapping_nodes[tis[a].idx];
				const char* tn = st->sorting_state.Textures[0]
					? (const char*)st->sorting_state.Textures[0]->Get_Texture_Name() : nullptr;
				if (tn && strstr(tn, "avcomanche_p")) {
					++s_propFlush;
					fprintf(stderr, "[PROPFLUSH] tex='%s' polys=%u verts=%u start=%u vtxRange=%u shader=%08x depthMask=%u src=%u dst=%u\n",
						tn, st->polygon_count, st->vertex_count, st->start_index, st->min_vertex_index,
						st->sorting_state.shader.Get_Bits(),
						(unsigned)st->sorting_state.shader.Get_Depth_Mask(),
						(unsigned)st->sorting_state.shader.Get_Src_Blend_Func(),
						(unsigned)st->sorting_state.shader.Get_Dst_Blend_Func());
					fflush(stderr);
					break;
				}
			}
		}

		// Set index buffer and render!

		DX8Wrapper::Set_Index_Buffer(dyn_ib_access,0); // Override with this buffer (do something to prevent need for this!)
		DX8Wrapper::Set_Vertex_Buffer(dyn_vb_access); // Override with this buffer (do something to prevent need for this!)

		DX8Wrapper::Apply_Render_State_Changes();

		unsigned count_to_render=1;
		unsigned start_index=0;
		unsigned node_id=tis[chunkOffset].idx;
		for (unsigned i=chunkOffset + 1;i<chunkEnd;++i) {
			if (node_id!=tis[i].idx) {
				SortingNodeStruct* state=overlapping_nodes[node_id];
				Apply_Render_State(state->sorting_state);

				DX8Wrapper::Draw_Triangles(
					start_index*3,
					count_to_render,
					state->min_vertex_index,
					state->vertex_count);

				count_to_render=0;
				start_index=i - chunkOffset;
				node_id=tis[i].idx;
			}
			count_to_render++;	//keep track of number of polygons of same kind
		}

		// Render any remaining polygons...
		if (count_to_render) {
			SortingNodeStruct* state=overlapping_nodes[node_id];
			Apply_Render_State(state->sorting_state);

			DX8Wrapper::Draw_Triangles(
				start_index*3,
				count_to_render,
				state->min_vertex_index,
				state->vertex_count);
		}

		chunkOffset += chunkCount;
	}
	if (vk14p) { vkf14b::g_seg[vkf14b::SEG_CHUNK].us += vkf14b::now_us() - t_chunk; vkf14b::g_seg[vkf14b::SEG_CHUNK].cnt++; }

	// GeneralsX @debug Qoder 21/10/2026 PROPPOOL: one bounded line per heavy frame, so the rotor
	// node can be matched against the pool ceiling.
	{
		static int s_propPool = 0;
		if (s_propPool < 12 && overlapping_node_count > 0) {
			++s_propPool;
			fprintf(stderr, "[PROPPOOL] nodes=%u polygons=%u vertices=%u\n",
				overlapping_node_count, overlapping_polygon_count, overlapping_vertex_count);
			fflush(stderr);
		}
	}

	// Release all references and return nodes back to the clean list for the frame...
	for (unsigned node_id=0;node_id<overlapping_node_count;++node_id) {
		SortingNodeStruct* state=overlapping_nodes[node_id];
		Release_Refs(state);
		clean_list.push_front(state);
	}
	overlapping_node_count=0;
	overlapping_polygon_count=0;
	overlapping_vertex_count=0;

	SNAPSHOT_SAY(("SortingSystem - Done flushing"));

}

// ----------------------------------------------------------------------------

void SortingRendererClass::Flush()
{
	WWPROFILE("SortingRenderer::Flush");
	static int s_sortFlushLog = 0;
	if (s_sortFlushLog < 8) {
		++s_sortFlushLog;
		fprintf(stderr, "[SORTFLUSH] sorted=%u unsorted=%u\n",
			(unsigned)sorted_list.size(), (unsigned)unsorted_list.size());
		fflush(stderr);
	}
	const bool vk14 = vkf14b::on();
	if (vk14) { vkf14b::g_ticks++; if (vkf14b::g_ticks >= 300) vkf14b::dump(); }
	Matrix4x4 old_view;
	Matrix4x4 old_world;
	DX8Wrapper::Get_Transform(D3DTS_VIEW,old_view);
	DX8Wrapper::Get_Transform(D3DTS_WORLD,old_world);

	// TheSuperHackers @perf stephanmeesters 04/07/2026
	// Splice nodes that have no bounding information (Z=0.0) at the correct location into the sorted list.
	SortingNodeStructList::iterator node = sorted_list.begin();
	while (node != sorted_list.end() && (*node)->transformed_center.Z > 0.0f) {
		++node;
	}
	if (vk14) { vkf14b::Scope s(true, vkf14b::SEG_SPLICE); (void)s;
	sorted_list.splice(node, unsorted_list); }

	while (!sorted_list.empty()) {
		SortingNodeStruct* state = sorted_list.front();
		sorted_list.pop_front();

		{
			static int s_nodeLog = 0;
			if (s_nodeLog < 12) {
				++s_nodeLog;
				const char* tn = state->sorting_state.Textures[0]
					? (const char*)state->sorting_state.Textures[0]->Get_Texture_Name() : "(null)";
				fprintf(stderr, "[SORTNODE] tex='%s' polys=%u verts=%u vbType=%u ibType=%u\n",
					tn, state->polygon_count, state->vertex_count,
					(unsigned)state->sorting_state.vertex_buffer_types[0],
					(unsigned)state->sorting_state.index_buffer_type);
				fflush(stderr);
			}
		}

		if ((state->sorting_state.index_buffer_type==BUFFER_TYPE_SORTING || state->sorting_state.index_buffer_type==BUFFER_TYPE_DYNAMIC_SORTING) &&
			(state->sorting_state.vertex_buffer_types[0]==BUFFER_TYPE_SORTING || state->sorting_state.vertex_buffer_types[0]==BUFFER_TYPE_DYNAMIC_SORTING)) {
			// GeneralsX @bugfix 24/10/2026: the R-05 segment timer used to wrap the call itself, so a
			// normal run (GENERALSX_VKFPS_PHASES unset) never inserted the node into the pool and every
			// sorted mesh batch was silently dropped - the rotor blur discs (PROPS01/02) among them.
			// Only the measurement is gated now; the insert always runs.
			vkf14b::Scope s(vk14, vkf14b::SEG_POOLINS);
			Insert_To_Sorting_Pool(state);
		}
		else {
			{
			static int s_propDirect = 0;
			const char* tn = state->sorting_state.Textures[0]
				? (const char*)state->sorting_state.Textures[0]->Get_Texture_Name() : nullptr;
			if (s_propDirect < 8 && tn && strstr(tn, "avcomanche_p")) {
				++s_propDirect;
				fprintf(stderr, "[PROPDIRECT] tex='%s' polys=%u verts=%u vbType=%u ibType=%u\n",
					tn, state->polygon_count, state->vertex_count,
					(unsigned)state->sorting_state.vertex_buffer_types[0],
					(unsigned)state->sorting_state.index_buffer_type);
				fflush(stderr);
			}
			vkf14b::Scope s(vk14, vkf14b::SEG_DIRECT);
			DX8Wrapper::Set_Render_State(state->sorting_state);
			DX8Wrapper::Draw_Triangles(state->start_index,state->polygon_count,state->min_vertex_index,state->vertex_count);
			DX8Wrapper::Release_Render_State();
			}
			Release_Refs(state);
			clean_list.push_front(state);
		}
	}

	bool old_enable=DX8Wrapper::_Is_Triangle_Draw_Enabled();
	DX8Wrapper::_Enable_Triangle_Draw(_EnableTriangleDraw);
	{
		// GeneralsX @bugfix 24/10/2026: Flush_Sorting_Pool is the draw of every pooled batch, and it
		// used to live inside the GENERALSX_VKFPS_PHASES gate; with the probe off the pool was never
		// flushed, so sorted geometry never reached the GPU. The timer is what is gated, not the draw.
		vkf14b::Scope s(vk14, vkf14b::SEG_POOL);
		Flush_Sorting_Pool();
	}
	DX8Wrapper::_Enable_Triangle_Draw(old_enable);

	{
		// GeneralsX @bugfix 24/10/2026: same gate bug - the per-frame reset of the dynamic buffer
		// rings and the VIEW/WORLD restore must happen on every run, not only under the FPS probe.
		vkf14b::Scope s(vk14, vkf14b::SEG_RESET);
		DX8Wrapper::Set_Index_Buffer(nullptr,0);
		DX8Wrapper::Set_Vertex_Buffer(nullptr);
		total_sorting_vertices=0;

		DynamicIBAccessClass::_Reset(false);
		DynamicVBAccessClass::_Reset(false);


		DX8Wrapper::Set_Transform(D3DTS_VIEW,old_view);
		DX8Wrapper::Set_Transform(D3DTS_WORLD,old_world);
	}

}

// ----------------------------------------------------------------------------

void SortingRendererClass::Deinit()
{
	//
	//	Flush the sorted list
	//
	while (!sorted_list.empty()) {
		delete sorted_list.front();
		sorted_list.pop_front();
	}

	//
	//	Flush the unsorted list
	//
	while (!unsorted_list.empty()) {
		delete unsorted_list.front();
		unsorted_list.pop_front();
	}

	//
	//	Flush the clean list
	//
	while (!clean_list.empty()) {
		delete clean_list.front();
		clean_list.pop_front();
	}

	delete[] temp_index_array;
	temp_index_array=nullptr;
	temp_index_array_count=0;
}
