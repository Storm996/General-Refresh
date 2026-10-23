// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5: pipeline state objects and the draw path.
//
// What this layer is. Wedges 1-4 built a device, a present chain, mirrored resources and a descriptor
// layer - everything a draw needs except the draw itself. This file is where a DX8 batch stops being a
// D3D9 call and becomes vkCmdDrawIndexed: it owns the geometry backings the batches bind, the render
// passes and framebuffers behind the render targets, the pipeline cache that turns the facade's shadow
// state into a VkPipeline, and the command buffer that records a whole engine frame.
//
// Design decisions (all pre-resolved in the approved plan, recorded here so the code is self-describing):
//  - the pipeline key is an EXPLICIT tuple, not a hash. Wedge 0's census proves the surface is small
//    (37 distinct pso keys in the main menu, collapse 1.135 against 42 full keys), so a vector scan over
//    a 20-field key is cheap and, unlike the census hash, cannot silently alias two different states onto
//    one pipeline.
//  - texture stage state is NOT part of the key. Wedge 7 replaces the D3D combiner with one interpreted
//    pixel shader that reads stage state from a uniform block, so stage combinations must not multiply
//    pipelines. That is the entire reason the census measured the pso/full gap.
//  - geometry gets a per-object backing rather than living in wedge 3.1's shared bump ring: a draw needs
//    the (VkBuffer, offset) that a specific lock wrote to, and that pair has to survive until the GPU is
//    done with it. Dynamic buffers rotate over VKGEO_SLOT_COUNT regions fenced by the frame index.
//  - the scene is rendered to our own colour + depth pair and the present chain samples that, so a
//    render-target switch inside a frame does not have to touch the swapchain.
//
// Vulkan-handle-free by contract, like VulkanGeo.h and VulkanDesc.h: dx8wrapper.cpp includes this and
// must not see <vulkan/vulkan.h>. Every handle crosses as void*, every Vulkan enum as unsigned carrying
// the *D3D9* ordinal (documented per field below), which the implementation maps locally.
#ifndef GENERALSX_VULKANDRAW_H
#define GENERALSX_VULKANDRAW_H

#if defined(_WIN32) && defined(SAGE_USE_VULKAN_CORE)

#define VKDRAW_MAX_STREAMS 8
// GeneralsX @feature Qoder 18/10/2026 Wedge 8: the descriptor array grew from four to eight slots
// so the terrain NM programs (which sample register s4 for the tangent-space normal atlas) can be
// represented at all. The old four was a mirror-phase convenience, not a hardware limit.
#define VKDRAW_MAX_TEX_SLOTS 8
// vkff.frag still declares four texture samples; the FF interpreter therefore cannot use slots
// 4..7 even though the descriptor array now carries them. Keep the accounting honest.
#define VKFF_SAMPLED_SLOTS 4
// Which fixed-function stage a cached pipeline was built with. Two bits, not a bool: wedge 6 stands up the
// vertex side and wedge 7 the pixel side, and a pipeline object has to record both decisions separately or
// a half-gated run could reuse a full-FF pipeline for a batch that asked for only one of them (or the
// reverse). The pipeline cache's ff field is one of these combinations.
#define VKDRAW_FF_VERT_BIT 1u
#define VKDRAW_FF_PIX_BIT  2u
// Wedge 6 widened this from 12: the fixed-function vertex stage reads position, normal, two vertex colours
// and up to four texture-coordinate sets (8 attributes) plus PSIZE, and a batch with FVF_XYZ|NORMAL|DIFFUSE
// |SPECULAR|TEX4 already needs 8 - so the table has to hold the widest layout the engine can hand it
// without an attribute being silently dropped.
#define VKDRAW_MAX_ATTRS 16

// The vertex input locations the wedge-6 fixed-function stage (Vulkan/shaders/vkff.vert) declares. The
// facade maps FVF onto these; a location the FVF does not provide is bound to a 16-byte all-zero stream
// by this layer, because a shader that reads an input must have a buffer behind it on Vulkan, while D3D9
// simply supplied its own default. Kept as macros so the two sides name the same slot in the same words.
#define VKDRAW_LOC_POSITION 0u     // vec4: D3DFVF_XYZ (w ignored) or D3DFVF_XYZRHW (screen x,y,z,1/rhw)
#define VKDRAW_LOC_NORMAL   1u     // vec3: D3DFVF_NORMAL
#define VKDRAW_LOC_DIFFUSE  2u     // vec4: D3DFVF_DIFFUSE, unorm colour
#define VKDRAW_LOC_SPECULAR 3u     // vec4: D3DFVF_SPECULAR, unorm colour
#define VKDRAW_LOC_TEX0     4u     // .. VKDRAW_LOC_TEX0+3 = the four FVF texture-coordinate sets
#define VKDRAW_LOC_PSIZE   12u     // float: D3DFVF_PSIZE, declared for completeness - POINTSCALE is dropped
#define VKDRAW_LOC_FIRST   0u
#define VKDRAW_LOC_LAST    7u      // the highest location the stage actually reads
#define VKDRAW_DUMMY_BYTES 16u

// One vertex attribute as the facade describes it. The facade already owns the FVF layout maths
// (FVFInfoClass knows each field's byte offset), so it decodes FVF here and this layer never has to
// duplicate that table. 'type'/'components' are translated to a VkFormat locally: keeping the D3D-side
// description semantic (floats, colour bytes) means no Vulkan enum value crosses the seam.
struct VKDraw_Attr {
    unsigned location;    // matches the fixed-function attribute slots wedge 6 declares in GLSL
    unsigned offset;      // bytes into the vertex
    unsigned components;  // 1..4
    unsigned type;        // 0 = 32-bit float, 1 = 4-byte unorm colour (BGRA storage order), 2 = short4 normalized,
                          // 3 = 32-bit uint (blend weights), 4 = short4 unnormalized
};

// One vertex stream as the draw needs it. Filled by the facade from its render_state.
struct VKDraw_Stream {
    void*             vk_buffer;   // VkBuffer from VKGeo_Back_Bind, nullptr = not mirrored
    unsigned long long offset;
    unsigned          stride;
    unsigned          vertex_count;
};

// The explicit pipeline key. Every field is a plain value the facade already keeps a shadow of.
struct VKDraw_PSO_Key {
    unsigned long long shader_bits;    // DX8Wrapper::ShaderBits() style vertex+pixel shader flags
    unsigned  fvf;                     // D3D FVF dword, verbatim
    unsigned  light_mask;              // bit i set when light i is enabled
    unsigned  vs_tag;                  // wedge 8 shader registry index; 0 = fixed function
    unsigned  ps_tag;
    unsigned  topology;                // real D3DPRIMITIVETYPE ordinal: 1 points, 2/3 lines, 4 list, 5 strip, 6 fan
    unsigned  rt_format;               // VkFormat of the colour attachment
    unsigned  depth_format;            // VkFormat of the depth attachment, 0 when there is none
    unsigned  sample_count;            // rasterization samples
    unsigned  cull_mode;               // D3DCULL: 1 none, 2 cw, 3 ccw
    unsigned  front_ccw;               // D3DRS_FRONTFACE semantics: 1 = counter-clockwise is front
    unsigned  fill_mode;               // D3DFILLMODE: 3 solid expected
    unsigned  depth_test;              // 0/1  (D3DRS_ZENABLE)
    unsigned  depth_write;             // 0/1  (D3DRS_ZWRITEENABLE)
    unsigned  depth_cmp;               // D3DCMPFUNC: 1 never .. 8 always
    unsigned  stencil_test;            // 0/1
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: the rest of the D3D9 stencil block. The
    // single test bit could describe the player-colour pass in name only - the pass that matters writes
    // the player index on ZFAIL, zeroes MISS and compares EQUAL later, so a bool would paint the whole
    // screen. Every field is the raw D3D ordinal; the layer only maps it.
    unsigned  stencil_func;            // D3DCMPFUNC
    unsigned  stencil_ref;             // 0..255 (D3D9 truncates the DWORD reference to 8 bits)
    unsigned  stencil_mask;            // D3DRS_STENCILMASK, compare mask
    unsigned  stencil_write_mask;      // D3DRS_STENCILWRITEMASK
    unsigned  stencil_fail;            // D3DSTENCILOP, stencil test failed
    unsigned  stencil_zfail;           // D3DSTENCILOP, stencil passed / depth failed
    unsigned  stencil_pass;            // D3DSTENCILOP
    unsigned  blend_enable;            // 0/1  (D3DRS_ALPHABLENDENABLE)
    unsigned  src_blend;               // D3DBLEND ordinal
    unsigned  dst_blend;               // D3DBLEND ordinal
    unsigned  src_blend_alpha;         // D3DRS_SRCBLENDALPHA
    unsigned  dst_blend_alpha;         // D3DRS_DESTBLENDALPHA
    unsigned  write_mask;              // D3DRS_COLORWRITEENABLE: bit0 R, bit1 G, bit2 B, bit3 A
    unsigned  alpha_test_enable;       // 0/1
    unsigned  alpha_ref;               // D3DRS_ALPHAREF, 0..255
    unsigned  alpha_cmp;               // D3DCMPFUNC
    unsigned  scissor_test;            // 0/1 (D3DRS_SCISSORTESTENABLE)
};

// ---- fixed-function vertex state (wedge 6) -----------------------------------------------------------
//
// Everything the D3D8 fixed-function VERTEX stage consumes, in one block that is memcpy'd verbatim into
// the binding-3 dynamic uniform buffer. The layout is the contract between three places - this struct,
// the packer in dx8wrapper.cpp, and the uniform block in shaders/vkff.vert - so it is spelled out in
// vec4-sized groups with no bare floats between them: that makes the C++ offsets equal the std140 offsets
// by construction, which is the only way the three can be kept honest without a shader compiler in the
// loop. Every group is documented with the byte offset the GLSL block expects.
//
// Scalars that are not colours are carried as raw D3D enum ordinals and decoded in the shader, the same
// way the pipeline key carries them: the layer stays a translator, not a second opinion on the state.
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan step 9.I: 1792 -> 1824. The point registers live in the
// two vec4 slots appended after the combiner block (see VKDraw_FF_State::point / point_scale).
// GeneralsX @bugfix Qoder 19/10/2026 BUG-03: 1824 -> 2336. The eight D3DTS_TEXTUREn matrices are appended
// after them; the terrain passes project camera-space positions with TEXTURE2/3 and nothing rendered right
// without them (see VKDraw_FF_State::tex_mat).
#define VKFF_STATE_BYTES 2336u
#define VKFF_LIGHT_VEC4  7u
#define VKFF_MAX_STAGES  8u

// flags.x - the boolean half of D3DRS. Each bit is one render state the fixed-function vertex stage
// reads; a bit is set when that state is on in the facade's shadow (or is on by device default, see
// VK_Read_RS). The shader branches on these, not on specialised variants, because wedge 0 measured the
// whole fixed-function surface at a few dozen keys and uniform branches are free next to a PSO explosion.
#define VKFF_FLAG_LIGHTING        (1u << 0)   // D3DRS_LIGHTING: when off, no light contributes at all
#define VKFF_FLAG_COLORVERTEX     (1u << 1)   // D3DRS_COLORVERTEX: when off, vertex diffuse is ignored
#define VKFF_FLAG_SPECULAR_ENABLE (1u << 2)   // D3DRS_SPECULARENABLE
#define VKFF_FLAG_NORMALIZE       (1u << 3)   // D3DRS_NORMALIZENORMALS
#define VKFF_FLAG_FOG_ENABLE      (1u << 4)   // D3DRS_FOGENABLE
#define VKFF_FLAG_HAS_NORMAL      (1u << 5)   // FVF: D3DFVF_NORMAL present
#define VKFF_FLAG_HAS_DIFFUSE     (1u << 6)   // FVF: D3DFVF_DIFFUSE present
#define VKFF_FLAG_HAS_SPECULAR    (1u << 7)   // FVF: D3DFVF_SPECULAR present
#define VKFF_FLAG_RHW             (1u << 8)   // FVF: D3DFVF_XYZRHW - already transformed, skip the matrices
#define VKFF_FLAG_POSITION_ONLY   (1u << 9)   // FVF: D3DFVF_XYZ, nothing else lights or warps
// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan step 9.I: D3DRS_POINTSPRITEENABLE. D3D9 generated the
// point's texture coordinate (0..1 across the sprite) itself; Vulkan exposes the same value as
// gl_PointCoord, so the pixel stage substitutes it for the stage's coordinate set 0. Point SIZE is a
// separate state (D3DRS_POINTSIZE and the scale registers), so this bit only decides the UV source.
#define VKFF_FLAG_POINT_SPRITE    (1u << 10)

// flags.y packs the small enums: light mask in bits 0..7, texture-coordinate set count in 8..10,
// vertex fog mode in 11..13, table fog mode in 14..16, and the four material colour sources two bits
// each from 17. Sources are D3DMCS ordinals (0 material, 1 vertex diffuse, 2 vertex specular); D3D9
// numbers them MATERIAL=0, COLOR1=1, COLOR2=2 and the facade shadow carries that ordinal verbatim.
#define VKFF_LIGHT_MASK(f)    ((f) & 0xFFu)
#define VKFF_TEXCOUNT(f)      (((f) >> 8) & 7u)
#define VKFF_FOG_VERTEX(f)    (((f) >> 11) & 7u)
#define VKFF_FOG_TABLE(f)     (((f) >> 14) & 7u)
#define VKFF_AMBIENT_SRC(f)   (((f) >> 17) & 3u)
#define VKFF_DIFFUSE_SRC(f)   (((f) >> 19) & 3u)
#define VKFF_SPECULAR_SRC(f)  (((f) >> 21) & 3u)
#define VKFF_EMISSIVE_SRC(f)  (((f) >> 23) & 3u)

// flags[2] is wedge 7's, and it carries the one part of the fixed-function pixel pipeline Vulkan has no
// hardware counterpart for: D3DRS_ALPHATESTENABLE / ALPHAFUNC / ALPHAREF. D3D9 tests the alpha the combiner
// stages produced against an 8-bit reference, which a modern API can only do inside the pixel stage - so the
// interpreter reads these three from one word instead of the pipeline state, and the pipeline key keeps its
// own copies only as a description of what the wedge-5 mechanism path was told. The reference is the raw
// 0..255 integer the shadow holds (VK_Read_RS's documented default is 0), the compare is the D3DCOMPAREFUNC
// ordinal converted to the shader's 0-based table (D3DCMP_NEVER=1 .. D3DCMP_ALWAYS=8), and bit 0 says
// whether either is consulted at all. The packed field MUST stay 0-based because vkff.frag decodes it
// with CMP_NEVER=0..CMP_ALWAYS=7; packing the raw enum shifted every compare by one and turned the
// vegetation pass' GREATEREQUAL(7) into ALWAYS(7), which is why alpha-cut tree cards stayed opaque.
// flags[3] stays reserved.
#define VKFF_ALPHA_TEST_ENABLE(f) ((f) & 1u)
#define VKFF_ALPHA_FUNC(f)        (((f) >> 1) & 7u)
#define VKFF_ALPHA_REF(f)         (((f) >> 8) & 0xFFu)
#define VKFF_ALPHA_CMP_INDEX(f)   (((f) >= 1u && (f) <= 8u) ? ((f) - 1u) : 7u)
#define VKFF_PACK_ALPHA_TEST(en, func, ref) \
    (((en) ? 1u : 0u) | (VKFF_ALPHA_CMP_INDEX(func) << 1) | (((ref) & 0xFFu) << 8))

// One light: 7 vec4. D3DLIGHT9 fields land in these slots (see DX8Wrapper::Set_Light for what the engine
// actually fills in - it puts the spot EXPONENT in Falloff and both cone angles in Phi/Theta).
struct VKDraw_FF_Light {
    float type_att[4];    // +0: x = D3DLIGHTTYPE verbatim (1 point, 2 spot, 3 directional), yzw = att0,1,2
    float pos_range[4];   // +16: xyz = position, w = range
    float dir_theta[4];   // +32: xyz = direction (the light TRAVEL vector), w = theta in radians
    float phi_falloff[4]; // +48: x = phi in radians, y = falloff (the spot exponent), zw = 0
    float ambient[4];     // +64
    float diffuse[4];     // +80
    float specular[4];    // +96
};

struct VKDraw_FF_Stage {
    unsigned tci;         // D3DTSS_TEXCOORDINDEX, raw: high word selects the camera-space generator
    unsigned ttff;        // D3DTSS_TEXTURETRANSFORMFLAGS, raw
    unsigned set;         // which FVF texture-coordinate set the low word names
    unsigned components;  // how many floats that set actually holds (1..4)
};

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7: one texture-combiner stage, three vec4 slots.
// Every word is the raw D3DTSS ordinal as the facade's shadow holds it (D3DTEXTUREOP for the ops, D3DTA_*
// with its COMPLEMENT/ALPHAREPLICATE modifier bits for the args); ARG0 exists for the triadic ops
// (MULTIPLYADD) and reads zero for a two-arg stage. The interpreter in shaders/vkff.frag decodes them.
struct VKDraw_FF_TSS {
    unsigned col[4];      // colorOp, arg0, arg1, arg2
    unsigned alp[4];      // alphaOp, arg0, arg1, arg2
    unsigned res[4];      // resultArg (D3DTA_CURRENT or D3DTA_TEMP), rest 0
};

struct VKDraw_FF_State {
    float   world[16];              // +0     mat4, D3D row-major memory as the device would have received it
    float   view[16];               // +64
    float   proj[16];               // +128   already carries the wedge-5 ZBias emulation (see the packer)
    float   mat_diffuse[4];         // +192
    float   mat_ambient[4];         // +208
    float   mat_specular[4];        // +224
    float   mat_emissive[4];        // +240
    float   mat_power[4];           // +256   x = D3DMATERIAL9::Power, rest 0
    float   scene_ambient[4];       // +272   D3DRS_AMBIENT unpacked to r,g,b
    float   fog_color[4];           // +288   D3DRS_FOGCOLOR unpacked
    float   fog_params[4];          // +304   x = FOGSTART, y = FOGEND, z = FOGDENSITY
    float   tex_factor[4];          // +320   x = D3DRS_TEXTUREFACTOR as a float colour
    unsigned flags[4];              // +336   see VKFF_FLAG_* / the flags.y bit fields
    VKDraw_FF_Light lights[8];      // +352   8 * 112 = 896, ends +1248
    VKDraw_FF_Stage stage[VKFF_MAX_STAGES];   // +1248  8 * 16 = 128, ends +1376
    float   viewport[4];            // +1376  x, y, w, h in pixels: the D3DFVF_XYZRHW path arrives in this
                                    //        space and the vertex stage has to turn it back into clip
                                    //        coordinates, which needs the rectangle D3D was about to apply
    // +1392. The rest of the block is padding for the vertex stage's convenience, and the first group of
    // it is not: D3DRS_LOCALVIEWER puts the eye at the world origin, which on this engine is the camera
    // position, and a per-vertex specular half-vector needs exactly that. The packer solves it out of the
    // view matrix once per draw (eye = -R^T * t of the camera-space matrix) instead of making the shader
    // invert a matrix, and hands it over here.
    float   eye_position[4];        // +1392  xyz = world-space eye, w = 1 when it could be computed
    // +1408. The combiner registers the wedge-7 pixel interpreter consumes, one VKDraw_FF_TSS per stage.
    VKDraw_FF_TSS tss[VKFF_MAX_STAGES];   // +1408  8 * 48 = 384, ends +1792
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan step 9.I: the point family. D3D9 sized a point from
    // D3DRS_POINTSIZE scaled by the distance curve in the three POINTSCALE coefficients; the vertex stage
    // reproduces that formula and writes gl_PointSize. The registers are carried verbatim (floats, not
    // ordinals) because every one of them is a float in D3D9.
    float   point[4];               // +1792  x = POINTSIZE, y = POINTSIZE_MIN, z = POINTSIZE_MAX,
                                    //        w = 1 when POINTSCALEENABLE is on
    float   point_scale[4];         // +1808  x = POINTSCALE_A, y = _B, z = _C, w unused
    // GeneralsX @bugfix Qoder 19/10/2026 BUG-03: the texture matrix stack. D3D8 multiplies the TEXCOORD
    // generator output by D3DTS_TEXTUREn whenever D3DTSS_TEXTURETRANSFORMFLAGS names a coordinate count
    // (D3DTTFF_COUNT1..4), which is how the terrain passes turn camera-space positions into the cloud and
    // lightmap uv the noise shaders sample - without the matrices the multiply lands on raw camPos and the
    // ground renders black. The bytes are the same ones DX8CALL(SetTransform) would have handed the device,
    // taken from DX8Wrapper::DX8Transforms with the device-default identity standing in for never-written
    // slots (the shadow is memset to zero by Set_Transform_Dirty).
    float   tex_mat[VKFF_MAX_STAGES][16];   // +1824  8 * 64 = 512, ends +2336
};

// One indexed draw, fully described: no hidden device state is consulted at submit time, because on
// Vulkan there is no device state to consult until a pipeline has been chosen.
struct VKDraw_Submit {
    VKDraw_PSO_Key     key;

    VKDraw_Stream      streams[VKDRAW_MAX_STREAMS];
    unsigned           stream_count;

    VKDraw_Attr        attrs[VKDRAW_MAX_ATTRS];
    unsigned           attr_count;

    void*              vk_index_buffer;   // VkBuffer, nullptr for a non-indexed draw
    unsigned long long index_offset;      // bytes into that buffer
    unsigned           index_count;       // indices available from index_offset
    unsigned           start_index;       // first index to read
    unsigned           polygon_count;
    unsigned           base_vertex;       // D3D9 BaseVertexIndex; same meaning as vertexOffset

    // 1 while the engine is replaying CPU-skinned geometry (DX8SkinFVFCategoryContainer): its vertices are
    // already in world space, so the recorder must not apply the world transform a second time.
    unsigned           skin_batch;

    void*              texture_views[VKDRAW_MAX_TEX_SLOTS];    // VkImageView, nullptr = white fallback
    void*              texture_samplers[VKDRAW_MAX_TEX_SLOTS]; // VkSampler,   nullptr = default sampler

    // Constant shadow arrays, uploaded verbatim into the wedge-4 rings for this draw. Sizes are the
    // engine's own register budgets (256 vertex / 224 pixel vec4). The third binding carries the
    // fixed-function state block (wedge 6) - matrices, material, lights, fog and the stage selectors -
    // and is null for the mechanism probes, which then bind the all-zero buffer like the other two.
    const float*       vertex_constants;
    const float*       pixel_constants;
    const VKDraw_FF_State* ff;
    unsigned           vertex_constant_vec4s;
    unsigned           pixel_constant_vec4s;

    unsigned           viewport[4];       // x, y, width, height in pixels
    unsigned           scissor[4];        // left, top, right, bottom
};

// ---- lifecycle --------------------------------------------------------------------
// True when the core is up, GENERALSX_VKDRAW=1 and the pipeline layer is built. Every other entry point
// in this header is a no-op returning false when this is false, so the facade can call unconditionally.
bool VKDraw_Ready();

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 7: is the interpreted fixed-function PIXEL stage in
// force (GENERALSX_VKFFPIX, which also needs the vertex stage it reads its varyings from)? The facade
// consults it before counting a combiner construct the interpreter has no row for, so a run with the gate
// off reports nothing about a path it never took.
bool VKDraw_FF_Pixel_On();

// Build the pipeline layer (pipeline layout, shader modules, the scene target, the pass/framebuffer and
// pipeline caches). Called from VulkanCore::Init behind the gate; idempotent.
bool VKDraw_Create();
void VKDraw_Destroy();

// ---- target and frame management --------------------------------------------------
// Point the recorder at a colour/depth mirror pair (keys as used by VKRT_Alloc_Color / VKRT_Alloc_Depth;
// a null depth key draws without depth testing). The scene target uses the addresses of two statics
// inside VulkanDraw.cpp, which the facade reaches through VKDraw_Scene_Targets().
void VKDraw_Set_Targets(void* color_key, void* depth_key, unsigned width, unsigned height);
void VKDraw_Get_Targets(void** color_key, void** depth_key, unsigned* width, unsigned* height);
void VKDraw_Scene_Targets(void** color_key, void** depth_key, unsigned* width, unsigned* height);

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5.B: the scene mirror is what the engine draws
// into, and it has to be sized to the *render* resolution, not the window. The two differ whenever the
// game runs at a resolution the display does not (windowed, or the pillarbox letterbox), and a mirror at
// the wrong size turns every viewport into a crop. 0 keeps the swapchain extent, which is the right answer
// for a renderer that has no notion of a render resolution at all.
void VKDraw_Set_Scene_Extent(unsigned width, unsigned height);

// Where the scene lands on the swapchain image, in swapchain pixels: the pillarbox fit rect. The bars are
// not drawn, they are the area the blit does not touch, so End_Frame clears the whole image first. A zero
// width means "the full extent", i.e. no letterboxing.
void VKDraw_Set_Present_Rect(unsigned x, unsigned y, unsigned width, unsigned height);

// DX8Wrapper::Clear equivalent, as vkCmdClearAttachments: the recorder opens the target's pass if the
// clear arrives before the frame's first draw, which is exactly how a D3D clear that precedes every batch
// behaves. 'rect' is x, y, width, height in
// target pixels and mirrors D3D's rule that Clear touches the current viewport only - a shadow pass that
// clears one tile of a big render target must leave the other tiles alone. Null rect = the whole target.
void VKDraw_Clear(int clear_color, int clear_z_stencil, const float color[4], float z, unsigned stencil,
                  const unsigned* rect);

// GeneralsX @feature Qoder 19/10/2026 wedge 11 forensics: copy a colour mirror to host staging at the
// next frame boundary and log its R-channel census (see VulkanDraw.cpp). No-op unless GENERALSX_VKRTPROBE=1.
void VKDraw_RTProbe(void* key);

// Frame boundaries, hooked to DX8Wrapper::Begin_Scene / the present call. Begin waits the slot's fence
// and opens the frame's command buffer; End closes the still-open render pass and the command buffer.
bool VKDraw_Begin_Frame();
void VKDraw_End_Frame();

// Submit the recorded frame and present it. With wedge 5 in force the present chain samples the scene
// target instead of clearing a solid colour. Returns true only when a frame actually reached the screen.
bool VKDraw_Present();

// True when the last acquire or present reported a swapchain that no longer matches the surface. The
// present dispatch recreates the swapchain on this; Begin_Frame clears it.
bool VKDraw_Resize_Needed();

// ---- drawing ----------------------------------------------------------------------
// The one draw entry point. It refuses a batch it cannot describe or cannot bind inside the mirrored
// buffers (unmirrored vertex/index backing, an index that reads past the vertex region, a topology
// Vulkan has no equivalent for) and returns false in that case.
//
// GeneralsX @refactor Qoder 18/10/2026 pure-Vulkan wedge 5: while the port is being built the facade's
// contract is the opposite of "never fall back": a false return means the batch was not recorded, so the
// caller still issues the D3D9 call and the game stays playable. That is what makes every intermediate
// state of this line launchable. Once wedge 9 retires d3d9 the fallthrough disappears with it and a false
// return becomes a dropped batch, which is why the refusal counters are logged from here on.
bool VK_Draw_Submit(const VKDraw_Submit* submit);

// Non-indexed (DrawPrimitiveUP) path: the caller hands CPU vertex data, which is copied into a fresh
// region of a dedicated host-visible buffer together with the index sequence D3D9 would have generated
// for that primitive type (a triangle fan is expanded while generating, since Vulkan has no fan
// topology). 'attrs' may be null for a position-only layout.
bool VK_Draw_User_Primitive(unsigned primitive_type, const void* vertex_data, unsigned vertex_count,
                           unsigned stride, unsigned polygon_count, const VKDraw_Attr* attrs,
                           unsigned attr_count, const VKDraw_PSO_Key* key);

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: the same user-memory path with the facade's
// complete submit. 'state' carries the pipeline key, textures, samplers, constants and fixed-function
// block the shadow held when the game issued the DrawPrimitiveUP; this entry point only supplies the
// geometry (copied into the scratch ring) and the generated index sequence.
bool VK_Draw_User_Primitive_State(unsigned primitive_type, const void* vertex_data, unsigned vertex_count,
                                  unsigned stride, unsigned polygon_count, const VKDraw_Submit* state);

// GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 9: indexed twin of the user-memory path for the
// families that used to drive a raw IDirect3DVertexBuffer9/IDirect3DIndexBuffer9 pair (water, shadow
// volumes). Both arrays are copied into the scratch ring, so the caller may free or reuse its CPU mirror
// as soon as the call returns. 'vertex_count' is D3D9's NumVertices and is read starting at
// 'base_vertex' (D3D9 BaseVertexIndex); indices are 16-bit, the only format those paths ever built.
bool VK_Draw_Indexed_User_Primitive_State(unsigned primitive_type,
                                          const void* vertex_data, unsigned vertex_count, unsigned stride,
                                          const void* index_data, unsigned index_count, unsigned base_vertex,
                                          const VKDraw_Submit* state);

// GeneralsX @feature Qoder 20/10/2026 native sun shadow (ported from ModernGeneralsZH): after the scene
// batches are recorded, replay every captured opaque caster into a sun-space depth map and multiply the
// scene by a screen-space PCSS visibility pass. 'sun_dir' is the map's sun position in world space
// (W3DShadowManager::getLightPosWorld(0), i.e. a far point along the direction toward the sun); the
// backend normalizes it. Call once per frame between the 3D views and the UI. Returns false when the
// gate is off, nothing was captured, or the pass could not be recorded.
bool VKDraw_Shadow_Apply(const float sun_dir[3]);

// GeneralsX @bugfix Qoder 21/10/2026: drop the per-world sun-fit state when a new map/height map
// takes over the scene. The ground-height estimate and the caster list describe the world that was
// being drawn; keeping them across a map change makes the fit start at the old terrain height and
// converge only as new casters stream in, which reads as "shadows appear a few seconds late".
// Shadow resources stay alive - only the frame-window state is cleared.
void VKDraw_Shadow_Reset();

// GeneralsX @feature Qoder 21/10/2026 in-game GPU vegetation: draw instanced grass blades and stones
// into the scene pass that is open right now (called from W3DGrassBuffer::render, after the terrain).
// Instances are 12 floats: (x,y,z,halfWidth), (height,leanAngle,leanAmount,phase), (r,g,b,seed) for
// grass; (x,y,z,scale), (rotX,rotY,rotZ,variant), (r,g,b,0) for stones. 'view_proj' is WW3D's column-vector
// projection*view in row-major memory and is transposed here for GLSL. Returns false when the Vulkan core is off, no pass
// is open, or the instance payload is empty - the caller then keeps the retail billboard path.
bool VKDraw_Vegetation_Ready();
// GeneralsX @feature Qoder 22/10/2026 light shadows: the strongest dynamic light's own shadow map, as
// this frame built it. False when the feature is off or has no map/light; the caller then leaves the
// receiver constants disabled. The VP is row-vector (D3D) and projects world -> light clip (0..1 z).
bool VKDraw_LightShadow_Info(float vp[16], float pos_range[4]);
bool VKDraw_Vegetation_Draw(const void* grass, unsigned grass_count,
                            const void* rocks, unsigned rock_count,
                            const float view_proj[16], const float camera[3], float time_seconds,
                            const float sun_dir[3], const float fog_color[3],
                            float fog_start, float fog_end, float cull_distance);

// ---- counters and the wedge-5 self test -------------------------------------------
struct VKDraw_Stats {
    unsigned long long draws, drawsSkipped, drawsBad, psoCreated, psoReused, psoFailed;
    unsigned long long passes, framebuffers, buffersCreated, buffersDropped, ringRotations;
    unsigned long long sceneCopies, clears, setsBuilt, setsReused;
    // GeneralsX @feature Qoder 19/10/2026 BUG-04 forensics: WW3D::Begin_Render clears before Begin_Scene, so
    // those requests arrive with no Vulkan frame open yet. Counted here (and parked in the recorder) to keep
    // "the engine never asked" distinguishable from "the ask arrived outside the frame".
    unsigned long long clearsDeferred;
    unsigned long long fanRewrites, topologyRefusals;
    unsigned long long framesBegun, framesPresented, framesFailed;
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5 forensics: a backing that was retired while
    // a frame that binds it was still in flight loses the device, and a batch that reads past its mirrored
    // region does the same. Both are counted here so the run log can prove which one happened.
    unsigned long long backingsRetired, backingsReaped, boundsRefusals, indexValueRefusals;
};
void VKDraw_Get_Stats(VKDraw_Stats* out);
void VKDraw_Log_Stats();          // periodic one-liner from the draw path

// GeneralsX @bugfix Qoder 19/10/2026 R-02 forensics: the constant rings' in-frame overwrite history. A
// wrap is the ring reusing its own frame window while that window still holds the payloads the frame's
// EARLIEST recorded batches point at - the GPU replays later data for them. Ring order: 0 vertex,
// 1 pixel, 2 fixed-function. 'peak' is the current frame's window high-water in bytes.
void VKDraw_Get_Const_Wraps(unsigned long long wraps[3]);
void VKDraw_Get_Const_Spills(unsigned long long spills[3]);
void VKDraw_Get_Const_Peaks(unsigned long long peaks[3]);
unsigned long long VKDraw_Get_Const_WindowCap(int ring);   // the window half, spill excluded

#endif // _WIN32 && SAGE_USE_VULKAN_CORE
#endif // GENERALSX_VULKANDRAW_H
