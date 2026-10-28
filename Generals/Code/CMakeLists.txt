# g stands for Generals, i stands for Interface
add_library(gi_gameengine_include INTERFACE)
add_library(gi_libraries_source_wwvegas INTERFACE)
add_library(gi_main INTERFACE)
add_library(gi_always INTERFACE)
add_library(gi_always_no_pch INTERFACE) # Use this for Shared Libs with MFC AFX

target_include_directories(gi_gameengine_include INTERFACE "GameEngine/Include")
target_include_directories(gi_libraries_source_wwvegas INTERFACE "Libraries/Source/WWVegas")
target_include_directories(gi_main INTERFACE "Main")
if(UNIX)
    target_include_directories(gi_always INTERFACE "CompatLib/Include")
endif()

target_compile_definitions(gi_always INTERFACE
    RTS_GENERALS=1
)
target_compile_definitions(gi_always_no_pch INTERFACE
    RTS_GENERALS=1
)
target_link_libraries(gi_gameengine_include INTERFACE
    corei_gameengine_include
)
target_link_libraries(gi_always INTERFACE
    corei_always # Must stay below so headers from game are included first
)
target_link_libraries(gi_always_no_pch INTERFACE
    corei_always_no_pch # Must stay below so headers from game are included first
)

# GeneralsX @build 24/10/2026 F6: the base game had no CompatLib subdirectory at all, so the d3dx8
# target (its Include_win copy of d3dx9math.h / the D3DX math+texture stubs) never existed for a
# Generals-only build and WWMath failed with "cannot open d3dx9math.h". Wire the same compat layer the
# Zero Hour tree uses; the guard keeps a both-games build from adding the target twice.
if(NOT TARGET d3dx8)
    add_subdirectory(CompatLib)
endif()

# Contains internal libraries
add_subdirectory(Libraries)

# Base GameEngine library
add_subdirectory(GameEngine)

# Platform specific GameEngine code
add_subdirectory(GameEngineDevice)

# Contains the main game exe target.
add_subdirectory(Main)

if (RTS_BUILD_GENERALS_TOOLS OR RTS_BUILD_GENERALS_EXTRAS)
    add_subdirectory(Tools)
endif()
