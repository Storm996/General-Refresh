# DirectX 8 headers and rendering backend selection
# GeneralsX @build BenderAI 10/02/2026 - Session 18
# Fighter19's approach: Fetch ONE OR THE OTHER, never both
#
# On Windows: Use min-dx8-sdk (minimal Windows DirectX headers + libs)
# On Linux:   Use DXVK native pre-built tarball (DirectX→Vulkan translation)
# On macOS:   Build DXVK from source using Meson + MoltenVK (DirectX→Metal bridge)
#
# CRITICAL: Mixing headers causes conflicts - dx8-src has incomplete types,
# DXVK has full DirectX8+Wine headers. Compiler picks first path = wrong headers.
#
# macOS DXVK build (Session 61, 24/02/2026):
#   DXVK 2.6 builds natively on macOS arm64 via its "native" build mode.
#   macOS fixes are maintained in the DXVK fork history consumed by this build.
#   This project no longer applies local patch scripts during configure/build.
#
# Reference: docs/WORKDIR/lessons/2026-02-LESSONS.md (historical patch rationale)

set(DXVK_VERSION "v2.6")

if(SAGE_USE_DX8)
  # Windows: Fetch min-dx8-sdk. This branch is LIVE, not dormant: the win64 configure scripts
  # (T0 编译工具 64位/configure_win64*.bat) and .github/workflows/build-windows.yml pass
  # -DSAGE_USE_DX8=ON, while only the Linux/macOS presets leave it OFF and take the DXVK branches.
  # What is still consumed from here on Windows: the DX8-era headers that the game sources include
  # (dinput.h, dxfile.h) and, for 32-bit only, the x86 dinput8/dxguid import libraries.
  FetchContent_Declare(
    dx8
    GIT_REPOSITORY https://github.com/TheSuperHackers/min-dx8-sdk.git
    GIT_TAG        7bddff8c01f5fb931c3cb73d4aa8e66d303d97bc
  )
  FetchContent_MakeAvailable(dx8)
  message(STATUS "Using DirectX 8 SDK (Windows native)")

  # GeneralsX @build win64 11/10/2026 On x64 the min-dx8-sdk import libraries (d3d8/dinput8/dxguid)
  # are x86-only, so the link step picks them up through the SDK's link directory and fails with
  # LNK4272 (wrong machine) plus unresolved DirectInput8Create / GUID_SysKeyboard / c_dfDIKeyboard.
  # Swap in the Windows SDK's own x64 dinput8.lib/dxguid.lib instead.
  # GeneralsX @build d3d9 port 12/10/2026 The renderer now targets the Direct3D 9 interface:
  # DX8Wrapper::Init loads d3d9.dll at runtime and calls Direct3DCreate9 through GetProcAddress,
  # so d3d8.lib has no importer left on ANY Windows architecture - retire it for 32-bit as well
  # (it used to reach the link line through d3d8lib plus the explicit name in Code/Main).
  # Previously generated x64 d3d8.lib (whose only exported symbol, Direct3DCreate8, had become dead
  # weight) is retired as well.
  if(WIN32 AND MSVC)
    get_target_property(_dx8_libs d3d8lib INTERFACE_LINK_LIBRARIES)
    list(REMOVE_ITEM _dx8_libs "d3d8")
    set_target_properties(d3d8lib PROPERTIES INTERFACE_LINK_LIBRARIES "${_dx8_libs}")
  endif()

  if(WIN32 AND MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    # Locate the Windows SDK um/x64 import library directory. The toolchain environment already
    # exports LIB with the default search path of the target architecture, so prefer the entry that
    # actually gets used by link.exe; the glob over the installed Kits is only a fallback.
    # (Note: "$ENV{ProgramFiles(x86)}" is not usable here - CMake rejects '(' in a variable name.)
    set(DX8_X64_UM "")
    file(TO_CMAKE_PATH "$ENV{LIB}" _dx8_lib_path)
    foreach(_dx8_entry ${_dx8_lib_path})
      if(_dx8_entry MATCHES "/um/x64/?$" AND EXISTS "${_dx8_entry}/dinput8.lib")
        set(DX8_X64_UM "${_dx8_entry}")
        break()
      endif()
    endforeach()
    if(NOT DX8_X64_UM)
      file(GLOB DX8_KITS_ROOTS "C:/Program Files (x86)/Windows Kits/10")
      foreach(DX8_KITS_ROOT ${DX8_KITS_ROOTS})
        file(GLOB DX8_WINSDK_VERS "${DX8_KITS_ROOT}/Lib/10.*")
        list(SORT DX8_WINSDK_VERS COMPARE NATURAL ORDER DESCENDING)
        foreach(DX8_VER_DIR ${DX8_WINSDK_VERS})
          if(EXISTS "${DX8_VER_DIR}/um/x64/dinput8.lib")
            set(DX8_X64_UM "${DX8_VER_DIR}/um/x64")
            break()
          endif()
        endforeach()
        if(DX8_X64_UM)
          break()
        endif()
      endforeach()
    endif()
    if(NOT DX8_X64_UM)
      message(FATAL_ERROR "win64: no Windows SDK um/x64 directory with dinput8.lib found")
    endif()

    # Replace the x86 contract coming from min-dx8-sdk: drop its link directory and the two
    # remaining library names, then hand the link step the x64 equivalents.
    # Keep everything the SDK target still provides for both architectures
    # (legacy_stdio_definitions, d3dx8, SAFESEH/NODEFAULTLIB).
    get_target_property(_dx8_libs d3d8lib INTERFACE_LINK_LIBRARIES)
    list(REMOVE_ITEM _dx8_libs "dinput8" "dxguid")
    set_target_properties(d3d8lib PROPERTIES INTERFACE_LINK_LIBRARIES "${_dx8_libs}")
    target_link_libraries(d3d8lib INTERFACE
      "${DX8_X64_UM}/dinput8.lib"
      "${DX8_X64_UM}/dxguid.lib")

    # Strip the x86 link directory min-dx8-sdk injected with BEFORE INTERFACE, otherwise the bare
    # names coming from the game targets still resolve to the x86 import libraries in _deps/dx8-src.
    get_target_property(_dx8_dirs d3d8lib INTERFACE_LINK_DIRECTORIES)
    if(_dx8_dirs)
      list(REMOVE_ITEM _dx8_dirs "${dx8_SOURCE_DIR}")
      list(REMOVE_DUPLICATES _dx8_dirs)
      set_target_properties(d3d8lib PROPERTIES INTERFACE_LINK_DIRECTORIES "${_dx8_dirs}")
    endif()

    message(STATUS "DX9 x64: SDK um dir ${DX8_X64_UM} (dinput8/dxguid only; d3d8 import retired)")
  endif()

elseif(APPLE AND SAGE_USE_MOLTENVK)
  # macOS: Build DXVK 2.6 from source using Meson + MoltenVK
  # GeneralsX @build BenderAI 24/02/2026 - Phase 5 macOS port (Session 61)
  find_program(MESON_EXECUTABLE meson HINTS /opt/homebrew/bin /usr/local/bin)
  find_program(NINJA_EXECUTABLE ninja HINTS /opt/homebrew/bin /usr/local/bin)

  if(NOT MESON_EXECUTABLE)
    message(FATAL_ERROR "DXVK macOS build requires meson: brew install meson")
  endif()
  if(NOT NINJA_EXECUTABLE)
    message(FATAL_ERROR "DXVK macOS build requires ninja: brew install ninja")
  endif()

  # Detect host architecture so Clang targets the correct slice.
  # IMPORTANT: prefer CMAKE_OSX_ARCHITECTURES (set by the preset) over uname -m.
  # On Apple Silicon Macs running CMake / meson via Rosetta, uname -m returns
  # x86_64 even though the native executable arch is arm64. Using CMAKE_OSX_ARCHITECTURES
  # (e.g. "arm64" from the macos-vulkan preset) avoids building an x86_64 dylib that
  # the arm64 game binary cannot dlopen.
  if(CMAKE_OSX_ARCHITECTURES)
    # Use the first entry (handles "arm64;x86_64" fat-binary requests too)
    list(GET CMAKE_OSX_ARCHITECTURES 0 DXVK_HOST_ARCH)
  else()
    execute_process(
      COMMAND uname -m
      OUTPUT_VARIABLE DXVK_HOST_ARCH
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
  endif()
  message(STATUS "Building DXVK ${DXVK_VERSION} for macOS/${DXVK_HOST_ARCH} with Meson (${MESON_EXECUTABLE})")

  include(ExternalProject)
  # GeneralsX @build BenderAI 13/03/2026 Add explicit source mode to keep remote branch updates deterministic by default.
  # GeneralsX @build fbraz 25/09/2026 Default local DXVK fork to workspace generalsx-dxvk repository.
  if(NOT DEFINED DXVK_LOCAL_FORK_DIR)
    if(EXISTS "${CMAKE_SOURCE_DIR}/../generalsx-dxvk/.git")
      set(DXVK_LOCAL_FORK_DIR "${CMAKE_SOURCE_DIR}/../generalsx-dxvk")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/references/fbraz3-dxvk/.git")
      set(DXVK_LOCAL_FORK_DIR "${CMAKE_SOURCE_DIR}/references/fbraz3-dxvk")
    else()
      set(DXVK_LOCAL_FORK_DIR "${CMAKE_SOURCE_DIR}/../generalsx-dxvk")
    endif()
  endif()
  option(SAGE_DXVK_USE_LOCAL_FORK "Build DXVK from local generalsx-dxvk checkout" OFF)

  if(SAGE_DXVK_USE_LOCAL_FORK AND EXISTS "${DXVK_LOCAL_FORK_DIR}/.git")
    set(DXVK_SOURCE_DIR "${DXVK_LOCAL_FORK_DIR}")
    message(STATUS "DXVK macOS build: using local fork source at ${DXVK_SOURCE_DIR}")
  else()
    set(DXVK_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/dxvk-src-generalsx")
    message(STATUS "DXVK macOS build: using GitHub source clone at ${DXVK_SOURCE_DIR}")
  endif()
  set(DXVK_BUILD_DIR  "${CMAKE_BINARY_DIR}/_deps/dxvk-build-macos")
  set(DXVK_D3D8_LIB  "${DXVK_BUILD_DIR}/src/d3d8/libdxvk_d3d8.0.dylib")
  set(DXVK_D3D9_LIB  "${DXVK_BUILD_DIR}/src/d3d9/libdxvk_d3d9.0.dylib")

  # Detect Vulkan SDK location for Meson configuration.
  # VULKAN_SDK must point to the platform subdir (e.g. ~/VulkanSDK/1.4.x/macOS)
  # where lib/libvulkan.dylib and lib/libMoltenVK.dylib live.
  # GeneralsX @build BenderAI 03/03/2026: Normalize env path to macOS platform subdir
  set(VULKAN_SDK_ENV "$ENV{VULKAN_SDK}")

  # If VULKAN_SDK points to the version root (has macOS/ subdir), normalize it
  if(VULKAN_SDK_ENV AND EXISTS "${VULKAN_SDK_ENV}/macOS/lib/libMoltenVK.dylib")
    set(VULKAN_SDK_ENV "${VULKAN_SDK_ENV}/macOS")
    message(STATUS "DXVK macOS build: Normalized VULKAN_SDK to platform subdir: ${VULKAN_SDK_ENV}")
  endif()

  if(NOT VULKAN_SDK_ENV OR NOT EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    # Try home directory: look for ~/VulkanSDK/*/macOS
    file(GLOB VULKAN_HOME_DIRS "$ENV{HOME}/VulkanSDK/*/macOS")
    if(VULKAN_HOME_DIRS)
      list(SORT VULKAN_HOME_DIRS)
      list(REVERSE VULKAN_HOME_DIRS)
      list(GET VULKAN_HOME_DIRS 0 POTENTIAL_SDK)
      if(EXISTS "${POTENTIAL_SDK}/lib/libMoltenVK.dylib")
        set(VULKAN_SDK_ENV "${POTENTIAL_SDK}")
      endif()
    endif()
  endif()

  if(NOT VULKAN_SDK_ENV OR NOT EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    # Try common Homebrew locations
    foreach(BREW_PATH "/usr/local/Caskroom/vulkan-sdk/latest/VulkanSDK/macOS" "/opt/homebrew/Caskroom/vulkan-sdk/latest/VulkanSDK/macOS")
      if(EXISTS "${BREW_PATH}/lib/libMoltenVK.dylib")
        set(VULKAN_SDK_ENV "${BREW_PATH}")
        break()
      endif()
    endforeach()
  endif()

  if(VULKAN_SDK_ENV AND EXISTS "${VULKAN_SDK_ENV}/lib/libMoltenVK.dylib")
    message(STATUS "DXVK macOS build: Using Vulkan SDK at ${VULKAN_SDK_ENV}")
    set(VULKAN_SDK_ENV_VAR "VULKAN_SDK=${VULKAN_SDK_ENV}")
  else()
    message(WARNING "DXVK macOS build: Vulkan SDK / MoltenVK not found; Meson will search system paths")
    if(VULKAN_SDK_ENV)
      message(STATUS "  VULKAN_SDK checked: ${VULKAN_SDK_ENV}")
    endif()
    set(VULKAN_SDK_ENV_VAR "")
  endif()

  if(SAGE_DXVK_USE_LOCAL_FORK AND EXISTS "${DXVK_LOCAL_FORK_DIR}/.git")
    ExternalProject_Add(dxvk_macos_build
      # GeneralsX @build BenderAI 13/03/2026 Build from local fbraz3 fork to avoid stale remote hash pins.
      SOURCE_DIR        ${DXVK_SOURCE_DIR}
      BINARY_DIR        ${DXVK_BUILD_DIR}
      DOWNLOAD_COMMAND  ""
      UPDATE_COMMAND    ""
      PATCH_COMMAND     ""
      CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CC=clang CXX=clang++ "CFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "CXXFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "LDFLAGS=-arch ${DXVK_HOST_ARCH}" ${VULKAN_SDK_ENV_VAR} ${MESON_EXECUTABLE} setup ${DXVK_BUILD_DIR} ${DXVK_SOURCE_DIR} --native-file ${CMAKE_SOURCE_DIR}/cmake/meson-arm64-native.ini -Ddxvk_native_wsi=sdl3 --buildtype=release --reconfigure
      BUILD_COMMAND     ${NINJA_EXECUTABLE} -C ${DXVK_BUILD_DIR} src/d3d9/libdxvk_d3d9.0.dylib src/d3d8/libdxvk_d3d8.0.dylib
      INSTALL_COMMAND   ""
      UPDATE_DISCONNECTED TRUE
    )
  else()
    # GeneralsX @build copilot 01/04/2026 Pin remote DXVK to immutable commit produced by fix/macos-size_t-cstddef.
    # GeneralsX @build 12/07/2026 Bumped to the merged commit that includes the macOS HiDPI WSI fix upstream.
    # GeneralsX @build BenderAI 11/09/2026 Bump DXVK commit to fix libc++ try_key_extraction on modern macOS
    set(DXVK_REMOTE_REF f66da559fd48cd55ddfebca38ed0f708c0f76a24)
    ExternalProject_Add(dxvk_macos_build
      # GeneralsX @build BenderAI 08/04/2026 Consume pre-patched source from pinned fork commit.
      # GeneralsX @build 17/09/2026 Migrate DXVK remote repository to generalsx-project org.
      GIT_REPOSITORY    https://github.com/generalsx-project/dxvk.git
      GIT_TAG           ${DXVK_REMOTE_REF}
      # GeneralsX @build copilot 01/04/2026 Keep pinned commit fetch reliable across clean CI builds.
      GIT_SHALLOW       FALSE
      SOURCE_DIR        ${DXVK_SOURCE_DIR}
      BINARY_DIR        ${DXVK_BUILD_DIR}
      CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env CC=clang CXX=clang++ "CFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "CXXFLAGS=-arch ${DXVK_HOST_ARCH} -mcpu=apple-m1" "LDFLAGS=-arch ${DXVK_HOST_ARCH}" ${VULKAN_SDK_ENV_VAR} ${MESON_EXECUTABLE} setup ${DXVK_BUILD_DIR} ${DXVK_SOURCE_DIR} --native-file ${CMAKE_SOURCE_DIR}/cmake/meson-arm64-native.ini -Ddxvk_native_wsi=sdl3 --buildtype=release --reconfigure
      BUILD_COMMAND     ${NINJA_EXECUTABLE} -C ${DXVK_BUILD_DIR} src/d3d9/libdxvk_d3d9.0.dylib src/d3d8/libdxvk_d3d8.0.dylib
      INSTALL_COMMAND   ""
      UPDATE_DISCONNECTED FALSE
    )
  endif()

  # Copy libdxvk_d3d9 + libdxvk_d3d8 to build dir and create unversioned symlinks.
  # d3d8 links against d3d9 via @rpath, so both must be present at runtime.
  add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
            "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${DXVK_D3D9_LIB} "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
              libdxvk_d3d9.0.dylib "${CMAKE_BINARY_DIR}/libdxvk_d3d9.dylib"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
              ${DXVK_D3D8_LIB} "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
    COMMAND ${CMAKE_COMMAND} -E create_symlink
              libdxvk_d3d8.0.dylib "${CMAKE_BINARY_DIR}/libdxvk_d3d8.dylib"
    DEPENDS dxvk_macos_build
    COMMENT "Installing libdxvk_d3d8 + libdxvk_d3d9 to build directory"
  )
  add_custom_target(dxvk_d3d8_install ALL
    DEPENDS "${CMAKE_BINARY_DIR}/libdxvk_d3d8.0.dylib"
            "${CMAKE_BINARY_DIR}/libdxvk_d3d9.0.dylib"
  )

  # Export path so other cmake files know where the headers are
  set(DXVK_INCLUDE_DIR "${DXVK_SOURCE_DIR}/include/native" CACHE PATH "DXVK native headers")
  # GeneralsX @build felipebraz 10/06/2025 Mirror lowercase dxvk_SOURCE_DIR that FetchContent sets on Linux
  # so CompatLib/CMakeLists.txt check works on macOS as well (CACHE PATH survives auto-regeneration)
  set(dxvk_SOURCE_DIR "${DXVK_SOURCE_DIR}" CACHE PATH "DXVK source directory (macOS)")
  message(STATUS "DXVK source directory: ${DXVK_SOURCE_DIR}")
  message(STATUS "DXVK d3d8 library:     ${DXVK_D3D8_LIB}")

else()
  # Linux: Fetch pre-built DXVK native binary for DirectX→Vulkan translation
  # Native 32-bit and 64-bit Linux binaries (.so)
  FetchContent_Declare(
    dxvk
    URL        https://github.com/doitsujin/dxvk/releases/download/v2.6/dxvk-native-2.6-steamrt-sniper.tar.gz
  )
  FetchContent_MakeAvailable(dxvk)
  message(STATUS "Using DXVK native (Linux DirectX→Vulkan)")
  message(STATUS "DXVK source directory: ${dxvk_SOURCE_DIR}")
endif()
