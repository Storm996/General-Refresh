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
#define VKDRAW_MAX_TEX_SLOTS 4
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
#define VKFF_STATE_BYTES 1792u
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
    VKDraw_FF_TSS tss[VKFF_MAX_STAGES];   // +1408  8 * 48 = 384, ends +1792 = VKFF_STATE_BYTES
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

// ---- counters and the wedge-5 self test -------------------------------------------
struct VKDraw_Stats {
    unsigned long long draws, drawsSkipped, drawsBad, psoCreated, psoReused, psoFailed;
    unsigned long long passes, framebuffers, buffersCreated, buffersDropped, ringRotations;
    unsigned long long sceneCopies, clears, setsBuilt, setsReused;
    unsigned long long fanRewrites, topologyRefusals;
    unsigned long long framesBegun, framesPresented, framesFailed;
    // GeneralsX @feature Qoder 18/10/2026 pure-Vulkan wedge 5 forensics: a backing that was retired while
    // a frame that binds it was still in flight loses the device, and a batch that reads past its mirrored
    // region does the same. Both are counted here so the run log can prove which one happened.
    unsigned long long backingsRetired, backingsReaped, boundsRefusals, indexValueRefusals;
};
void VKDraw_Get_Stats(VKDraw_Stats* out);
void VKDraw_Log_Stats();          // periodic one-liner from the draw path

#endif // _WIN32 && SAGE_USE_VULKAN_CORE
#endif // GENERALSX_VULKANDRAW_H
