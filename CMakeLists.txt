cmake_minimum_required(VERSION 3.24)
project(MineCraftCPP LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put the executable in build/bin (or build/bin/<Config> for multi-config generators).
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---------------------------------------------------------------------------
# Vulkan SDK (provides headers, loader, validation layers, and glslc)
# ---------------------------------------------------------------------------
find_package(Vulkan REQUIRED)
if(NOT Vulkan_GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found. Install the LunarG Vulkan SDK and re-open your shell.")
endif()

# ---------------------------------------------------------------------------
# Third-party dependencies via FetchContent
# ---------------------------------------------------------------------------
include(FetchContent)

# GLFW: window + input + Vulkan surface creation
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4)
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

# GLM: math (header-only)
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG        1.0.1)
FetchContent_MakeAvailable(glm)

# VMA: Vulkan Memory Allocator (provides target GPUOpen::VulkanMemoryAllocator)
FetchContent_Declare(vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG        v3.1.0)
FetchContent_MakeAvailable(vma)

# stb: single-header image loading (no CMakeLists -> MakeAvailable just downloads it)
FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG        master)
FetchContent_MakeAvailable(stb)

# ---------------------------------------------------------------------------
# Shader compilation: shaders/*.{vert,frag,comp} -> build/shaders/*.spv
# ---------------------------------------------------------------------------
set(SHADER_SRC_DIR ${CMAKE_SOURCE_DIR}/shaders)
set(SHADER_OUT_DIR ${CMAKE_BINARY_DIR}/shaders)
file(GLOB SHADER_SOURCES CONFIGURE_DEPENDS
    ${SHADER_SRC_DIR}/*.vert
    ${SHADER_SRC_DIR}/*.frag
    ${SHADER_SRC_DIR}/*.comp)

set(SPV_FILES "")
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(FNAME ${SHADER} NAME)
    set(SPV ${SHADER_OUT_DIR}/${FNAME}.spv)
    add_custom_command(
        OUTPUT  ${SPV}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUT_DIR}
        COMMAND ${Vulkan_GLSLC_EXECUTABLE} ${SHADER} -o ${SPV}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${FNAME}"
        VERBATIM)
    list(APPEND SPV_FILES ${SPV})
endforeach()
add_custom_target(shaders ALL DEPENDS ${SPV_FILES})

# ---------------------------------------------------------------------------
# Engine library — everything except main.cpp, shared by the game executable
# and the developer tools in tools/ (anim_studio etc., see docs/animation.md).
# ---------------------------------------------------------------------------
set(ENGINE_SOURCES
    src/anim/AnimChannel.cpp
    src/anim/AnimClip.cpp
    src/anim/AnimGraph.cpp
    src/anim/AnimStateMachine.cpp
    src/anim/Blend1D.cpp
    src/anim/FootIK.cpp
    src/anim/Json.cpp
    src/anim/Molang.cpp
    src/anim/PlayerAnimator.cpp
    src/anim/Pose.cpp
    src/anim/Rig.cpp
    src/core/Paths.cpp
    src/core/Window.cpp
    src/core/Camera.cpp
    src/core/FileDialog.cpp
    src/core/KeyBinds.cpp
    src/core/Lang.cpp
    src/core/Settings.cpp
    src/core/ThreadPool.cpp
    src/core/SystemStats.cpp
    src/game/Drops.cpp
    src/game/Mining.cpp
    src/game/Player.cpp
    src/game/PlayerModel.cpp
    src/game/Recipes.cpp
    src/game/Session.cpp
    src/game/PlayerSkin.cpp
    src/gfx/VkContext.cpp
    src/gfx/Swapchain.cpp
    src/gfx/Renderer.cpp
    src/gfx/SkyRenderer.cpp
    src/gfx/PanoramaRenderer.cpp
    src/gfx/PlayerPreview.cpp
    src/gfx/WeatherRenderer.cpp
    src/gfx/ModelRenderer.cpp
    src/gfx/HeldItemRenderer.cpp
    src/gfx/GpuBuffer.cpp
    src/gfx/TextureArray.cpp
    src/gfx/UIRenderer.cpp
    src/gfx/vma_impl.cpp
    src/gfx/stb_impl.cpp
    src/ui/DebugOverlay.cpp
    src/ui/Font.cpp
    src/ui/Hud.cpp
    src/ui/InventoryScreen.cpp
    src/ui/ItemIconRenderer.cpp
    src/ui/MenuScreen.cpp
    src/world/Block.cpp
    src/world/BlockMesh.cpp
    src/world/Blocks.cpp
    src/world/BlockRegistry.cpp
    src/world/LodMesh.cpp
    src/world/Mesher.cpp
    src/world/ModelDisplay.cpp
    src/world/Noise.cpp
    src/world/Raycast.cpp
    src/world/WorldGen.cpp
    src/world/World.cpp
    src/world/WorldSave.cpp)

add_library(mc_engine STATIC ${ENGINE_SOURCES})

# VMA is header-only and we compile its implementation in vma_impl.cpp, so we only need
# its include directory (avoids depending on VMA's CMake target name).
# PUBLIC: the game exe and every tools/ exe compile against the same headers.
target_include_directories(mc_engine PUBLIC
    ${CMAKE_SOURCE_DIR}/src
    ${stb_SOURCE_DIR}
    ${vma_SOURCE_DIR}/include)
target_link_libraries(mc_engine PUBLIC
    Vulkan::Vulkan glfw glm::glm)

# The VMA and stb implementation TUs generate many third-party warnings; silence them.
set_source_files_properties(src/gfx/vma_impl.cpp src/gfx/stb_impl.cpp
    PROPERTIES COMPILE_FLAGS "/w")

target_compile_definitions(mc_engine PUBLIC
    GLFW_INCLUDE_VULKAN
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    $<$<CONFIG:Debug>:MC_DEBUG>)

if(MSVC)
    # No global /W warning level so the per-file /w on vendored TUs doesn't conflict (D9025).
    target_compile_options(mc_engine PRIVATE /permissive-)
endif()

# ---------------------------------------------------------------------------
# Game executable — thin main.cpp over the engine library
# ---------------------------------------------------------------------------
add_executable(MineCraftCPP src/main.cpp)
add_dependencies(MineCraftCPP shaders)
target_link_libraries(MineCraftCPP PRIVATE mc_engine)

if(MSVC)
    target_compile_options(MineCraftCPP PRIVATE /permissive-)
endif()

# ---------------------------------------------------------------------------
# Developer tools — standalone executables (NEVER part of the game exe).
# tools/CMakeLists.txt hosts anim_studio and future debug apps (docs/animation.md §10).
# ---------------------------------------------------------------------------
option(MC_BUILD_TOOLS "Build the developer tools in tools/" ON)
if(MC_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

# Copy shaders + assets next to the executable (works for single- and multi-config).
add_custom_command(TARGET MineCraftCPP POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${SHADER_OUT_DIR} $<TARGET_FILE_DIR:MineCraftCPP>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:MineCraftCPP>/assets
    COMMENT "Copying shaders and assets next to executable"
    VERBATIM)
