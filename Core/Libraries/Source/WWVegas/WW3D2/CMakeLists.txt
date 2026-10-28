# GeneralsX @feature Qoder 17/10/2026 Pure-Vulkan port line: WP1 opt-in Vulkan device core.
# OFF in every shipping tree (Linux/macOS/win64 stay bit-identical); the win64vk port tree flips it.
option(SAGE_USE_VULKAN_CORE "WP1: compile Vulkan device core behind DX8Wrapper facade (Windows only)" OFF)
set(SAGE_VULKAN_HEADERS_DIR "" CACHE PATH "Vulkan-Headers include dir for SAGE_USE_VULKAN_CORE")

set(WW3D2_SRC
    aabtree.cpp
    aabtree.h
    #aabtreebuilder.cpp
    #aabtreebuilder.h
    agg_def.cpp
    agg_def.h
    animatedsoundmgr.cpp
    animatedsoundmgr.h
    #animobj.cpp
    #animobj.h
    #assetmgr.cpp
    #assetmgr.h
    assetstatus.cpp
    assetstatus.h
    Backend/DX8Backend.cpp
    Backend/DX8Backend.h
    Backend/RenderBackend.h
    bitmaphandler.cpp
    bitmaphandler.h
    bmp2d.cpp
    bmp2d.h
    #boxrobj.cpp
    #boxrobj.h
    bwrender.cpp
    bwrender.h
    #camera.cpp
    #camera.h
    #classid.h # unused
    collect.cpp
    collect.h
    colorspace.h
    coltest.cpp
    coltest.h
    coltype.h
    composite.cpp
    composite.h
    #dazzle.cpp
    #dazzle.h
    #ddsfile.cpp
    #ddsfile.h
    #decalmsh.cpp
    #decalmsh.h
    decalsys.cpp
    decalsys.h
    distlod.cpp
    distlod.h
    dllist.h
    dx8caps.cpp
    dx8caps.h
    dx8fvf.cpp
    dx8fvf.h
    dx8indexbuffer.cpp
    dx8indexbuffer.h
    dx8list.h
    # GeneralsX @feature T3-0 M4 (段E): dot3 网格着色器挂载模块
    dx8normalmap.cpp
    dx8normalmap.h
    dx8polygonrenderer.cpp
    dx8polygonrenderer.h
    dx8renderer.cpp
    dx8renderer.h
    dx8rendererdebugger.cpp
    dx8rendererdebugger.h
    dx8texman.cpp
    dx8texman.h
    dx8vertexbuffer.cpp
    dx8vertexbuffer.h
    dx8webbrowser.cpp
    dx8webbrowser.h
    dx8wrapper.cpp
    dx8wrapper.h
    dynamesh.cpp
    dynamesh.h
    font3d.cpp
    font3d.h
    formconv.cpp
    formconv.h
    # GeneralsX @build fbraz 10/02/2026 BenderAI - FramGrab uses Windows-only VFW/AVI APIs (Phase 3 out-of-scope)
    #FramGrab.cpp
    #framgrab.h
    hanim.cpp
    hanim.h
    #hanimmgr.cpp
    #hanimmgr.h
    hcanim.cpp
    hcanim.h
    #hlod.cpp
    #hlod.h
    hmdldef.cpp
    hmdldef.h
    #hmorphanim.cpp
    #hmorphanim.h
    #hrawanim.cpp
    #hrawanim.h
    htree.cpp
    htree.h
    #htreemgr.cpp
    #htreemgr.h
    IRenderBackend.h
    intersec.cpp
    intersec.h
    intersec.inl
    inttest.h
    layer.cpp
    layer.h
    #light.cpp
    #light.h
    #lightenvironment.cpp
    #lightenvironment.h
    line3d.cpp
    line3d.h
    #linegrp.cpp
    #linegrp.h
    #mapper.cpp
    #mapper.h
    matinfo.cpp
    matinfo.h
    matpass.cpp
    matpass.h
    #matrixmapper.cpp
    #matrixmapper.h
    #mesh.cpp
    #mesh.h
    #meshbuild.cpp
    #meshbuild.h
    meshdam.cpp
    meshdam.h
    #meshgeometry.cpp
    #meshgeometry.h
    #meshmatdesc.cpp
    #meshmatdesc.h
    #meshmdl.cpp
    #meshmdl.h
    #meshmdlio.cpp
    metalmap.cpp
    metalmap.h
    missingtexture.cpp
    missingtexture.h
    #motchan.cpp
    #motchan.h
    #nullrobj.cpp
    #nullrobj.h
    #part_buf.cpp
    #part_buf.h
    #part_emt.cpp
    #part_emt.h
    #part_ldr.cpp
    #part_ldr.h
    pivot.cpp
    pivot.h
    pointgr.cpp
    pointgr.h
    polyinfo.cpp
    polyinfo.h
    predlod.cpp
    predlod.h
    prim_anim.cpp
    prim_anim.h
    projector.cpp
    projector.h
    proto.cpp
    proto.h
    proxy.h
    rddesc.h
    #render2d.cpp
    #render2d.h
    render2dsentence.cpp
    render2dsentence.h
    renderobjectrecycler.cpp
    renderobjectrecycler.h
    rendobj.cpp
    rendobj.h
    #rinfo.cpp
    #rinfo.h
    ringobj.cpp
    ringobj.h
    robjlist.h
    #scene.cpp
    #scene.h
    segline.cpp
    segline.h
    seglinerenderer.cpp
    seglinerenderer.h
    #shader.cpp
    #shader.h
    shattersystem.cpp
    shattersystem.h
    shdlib.h
    snappts.cpp
    snapPts.h
    sortingrenderer.cpp
    sortingrenderer.h
    soundlibrarybridge.h
    soundrobj.cpp
    soundrobj.h
    sphereobj.cpp
    sphereobj.h
    static_sort_list.cpp
    static_sort_list.h
    statistics.cpp
    statistics.h
    streak.cpp
    streak.h
    streakRender.cpp
    streakRender.h
    stripoptimizer.cpp
    stripoptimizer.h
    surfaceclass.cpp
    surfaceclass.h
    texproject.cpp
    texproject.h
    #textdraw.cpp # unused
    textdraw.h
    texture.cpp
    texture.h
    texturefilter.cpp
    texturefilter.h
    textureloader.cpp
    textureloader.h
    texturethumbnail.cpp
    texturethumbnail.h
    #vertmaterial.cpp
    #vertmaterial.h
    visrasterizer.cpp
    visrasterizer.h
    w3d_dep.cpp
    w3d_dep.h
    #w3d_file.h
    w3d_obsolete.h
    w3d_util.cpp
    w3d_util.h
    w3derr.h
    w3dexclusionlist.cpp
    w3dexclusionlist.h
    ww3d.cpp
    ww3d.h
    ww3dformat.cpp
    ww3dformat.h
    ww3dids.h
    ww3dtrig.h
)

if(WIN32)
    list(APPEND WW3D2_SRC
        FramGrab.cpp
        framgrab.h
    )
endif()

# GeneralsX @feature Qoder 18/10/2026 wedge 8: the program registry is compiled on every platform. The
# shared facade TU (GameEngineDevice) does not carry SAGE_USE_VULKAN_CORE, so the symbol has to come from
# this library unconditionally; the .cpp holds the real table on Vulkan-Windows and no-ops elsewhere.
list(APPEND WW3D2_SRC
    Backend/VulkanPgm.cpp
    Backend/VulkanPgm.h
)

# GeneralsX @feature Qoder 17/10/2026 WP1 wedge 1: gated sources + headers (Windows only).
if(SAGE_USE_VULKAN_CORE)
    if(NOT WIN32)
        message(FATAL_ERROR "SAGE_USE_VULKAN_CORE is Windows-only for now")
    endif()
    list(APPEND WW3D2_SRC
        Backend/VulkanCore.cpp
        Backend/VulkanCore.h
        Backend/VulkanFns.h
        Backend/VulkanGeo.h
        Backend/VulkanTexMirror.h
        Backend/VulkanDesc.h
        Backend/VulkanDraw.h
        Backend/VulkanDraw.cpp
        Backend/VulkanFF.h
        Backend/VulkanFF.cpp
        Backend/VkShadersTri.h
        Backend/VkShadersDesc.h
        Backend/VkShadersDraw.h
        Backend/VkShadersFF.h
        Backend/VkShadersPgm.h
        Backend/VkLightingLayout.h
        # GeneralsX @feature Qoder 18/10/2026 wedge 9.J: inert D3D9 COM stubs plus their hit census. The
        # header is generated by EngineRebuild/Vulkan/scripts/gen_d3d_stubs.py; nothing reaches these
        # classes until GENERALSX_VK_NOD3D is set, so landing them changes no behaviour.
        Backend/VKD3DStub.h
        Backend/VKD3DStubNotes.cpp
        Backend/VKD3DStubImpl.h
        Backend/VKD3DStubImpl.cpp
    )
endif()

add_library(corei_ww3d2 INTERFACE)

target_sources(corei_ww3d2 INTERFACE ${WW3D2_SRC})

if(SAGE_USE_VULKAN_CORE)
    target_include_directories(corei_ww3d2 INTERFACE "${SAGE_VULKAN_HEADERS_DIR}")
    target_compile_definitions(corei_ww3d2 INTERFACE SAGE_USE_VULKAN_CORE)
endif()

# GeneralsX @build fbraz 11/02/2026 BenderAI - Enable FreeType text rendering on Linux/macOS
target_compile_definitions(corei_ww3d2 INTERFACE
    $<$<PLATFORM_ID:Linux,Darwin>:SAGE_USE_FREETYPE>
)

target_include_directories(corei_ww3d2 INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

if (MSVC AND NOT IS_VS6_BUILD)
    target_link_libraries(corei_ww3d2 INTERFACE
        comsuppw
    )
endif()

# GeneralsX @build fbraz 11/02/2026 BenderAI - FreeType2 for Unix text rendering (Linux + macOS Phase 5)
if(UNIX)
    find_package(Freetype REQUIRED)
    # GeneralsX @build felipebraz 20/06/2025 fontconfig needed on macOS too (render2dsentence.cpp uses Locate_Font_FontConfig)
    find_package(Fontconfig REQUIRED)
    # GeneralsX @build felipebraz 24/02/2026 fontconfig static lib (vcpkg) links iconv; must be explicit on macOS
    find_package(Iconv REQUIRED)
endif()

target_link_libraries(corei_ww3d2 INTERFACE
    core_browserengine
    core_wwlib
    core_wwmath
    corei_platform
    $<$<PLATFORM_ID:Linux>:Freetype::Freetype>
    $<$<PLATFORM_ID:Darwin>:Freetype::Freetype>
    $<$<PLATFORM_ID:Linux>:Fontconfig::Fontconfig>
    $<$<PLATFORM_ID:Darwin>:Fontconfig::Fontconfig>
    $<$<PLATFORM_ID:Darwin>:Iconv::Iconv>
)
