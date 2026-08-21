cmake_minimum_required(VERSION 3.25)

project(
    mc_rebedrock
    VERSION 0.1.0
    DESCRIPTION "A Vulkan-based voxel sandbox"
    # C is needed for the vendored miniz (zip reading); everything else is C++.
    LANGUAGES CXX C
)

include(CTest)
include(FetchContent)
include(ExternalProject)

# Dependency-source reuse for the two canonical build branches
# (build/rebedrock-debug and build/rebedrock-release). GLFW/GLM/VMA sources
# downloaded by a previous configure are kept in .cache/fetchcontent
# (git-ignored) and shared by both branches; stb is vendored under vendor/.
# Only those two build directories are supported — see README "macOS 构建与运行".
set(_MC_REBEDROCK_FETCH_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cache/fetchcontent")
foreach(_MC_REBEDROCK_FETCH_DEP GLFW GLM VMA)
    if(EXISTS "${_MC_REBEDROCK_FETCH_CACHE}/${_MC_REBEDROCK_FETCH_DEP}-src")
        set(FETCHCONTENT_SOURCE_DIR_${_MC_REBEDROCK_FETCH_DEP}
            "${_MC_REBEDROCK_FETCH_CACHE}/${_MC_REBEDROCK_FETCH_DEP}-src")
    endif()
endforeach()
unset(_MC_REBEDROCK_FETCH_CACHE)
unset(_MC_REBEDROCK_FETCH_DEP)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb-src")
    set(FETCHCONTENT_SOURCE_DIR_STB "${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb-src")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MC_REBEDROCK_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(
    MC_REBEDROCK_BUILD_WINDOWS_ON_MAC
    "Build a Windows x64 executable alongside the macOS executable"
    ${APPLE}
)
option(MC_REBEDROCK_STAGE_RUNTIME "Stage the shared game runtime directory" ON)
option(MC_REBEDROCK_MINGW_CROSS "Use the bundled MinGW Vulkan import target" OFF)
option(MC_REBEDROCK_BUILD_TOOLS
    "Build the manual benchmarks and diagnostics (not run by ctest)" OFF)

# Keep the build root tidy: test executables and static libraries land in a
# tests/ subdirectory. The game binary overrides its own output directory to
# ${MC_REBEDROCK_GAME_ROOT}/bin below.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tests")

if(MC_REBEDROCK_MINGW_CROSS)
    if(NOT Vulkan_INCLUDE_DIR OR NOT IS_DIRECTORY "${Vulkan_INCLUDE_DIR}/vulkan")
        message(FATAL_ERROR
            "A Vulkan SDK include directory is required for the Windows cross-build")
    endif()
    if(NOT CMAKE_DLLTOOL)
        find_program(CMAKE_DLLTOOL NAMES x86_64-w64-mingw32-dlltool REQUIRED)
    endif()
    set(MC_REBEDROCK_VULKAN_IMPORT_LIBRARY
        "${CMAKE_CURRENT_BINARY_DIR}/libvulkan-1.a")
    add_custom_command(
        OUTPUT "${MC_REBEDROCK_VULKAN_IMPORT_LIBRARY}"
        COMMAND "${CMAKE_DLLTOOL}"
            --input-def "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vulkan-1.def"
            --output-lib "${MC_REBEDROCK_VULKAN_IMPORT_LIBRARY}"
            --machine i386:x86-64
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/vulkan-1.def"
        COMMENT "Generating the Windows Vulkan loader import library"
        VERBATIM
    )
    add_custom_target(mc_rebedrock_vulkan_import
        DEPENDS "${MC_REBEDROCK_VULKAN_IMPORT_LIBRARY}")
    add_library(Vulkan::Vulkan STATIC IMPORTED GLOBAL)
    set_target_properties(Vulkan::Vulkan PROPERTIES
        IMPORTED_LOCATION "${MC_REBEDROCK_VULKAN_IMPORT_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIR}"
    )
else()
    find_package(Vulkan REQUIRED)
endif()

set(GLM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLM_BUILD_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
    GIT_TAG v3.4.0
    GIT_SHALLOW TRUE
)
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG 31c1ad37456438565541f4919958214b6e762fb4
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(glm vma stb)

find_package(glfw3 3.4 CONFIG QUIET)
if(NOT TARGET glfw)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.4
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(glfw)
endif()

add_library(mc_rebedrock_third_party STATIC
    src/assets/StbImageImplementation.cpp
    src/render/vulkan/VmaImplementation.cpp
)
target_include_directories(mc_rebedrock_third_party PRIVATE
    "${stb_SOURCE_DIR}"
    "${vma_SOURCE_DIR}/include"
)
target_link_libraries(mc_rebedrock_third_party PRIVATE Vulkan::Vulkan)
if(MC_REBEDROCK_MINGW_CROSS)
    add_dependencies(mc_rebedrock_third_party mc_rebedrock_vulkan_import)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mc_rebedrock_third_party PRIVATE
        -Wno-nullability-completeness
        -Wno-nullability-extension
    )
endif()

add_library(mc_rebedrock_audio STATIC
    src/audio/AudioSystem.cpp
    src/audio/MiniaudioImplementation.cpp
)
target_include_directories(mc_rebedrock_audio
    PUBLIC src "${glm_SOURCE_DIR}"
    PRIVATE third_party/miniaudio "${stb_SOURCE_DIR}"
)
if(APPLE)
    target_link_libraries(mc_rebedrock_audio PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreFoundation"
    )
elseif(WIN32)
    target_link_libraries(mc_rebedrock_audio PRIVATE ole32)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(mc_rebedrock_audio PRIVATE
        -Wno-unused-function
        -Wno-unused-parameter
    )
endif()

# Standalone data-driven animation library. It depends only on glm and the
# standard library (its JSON reader lives in src/core), with no gameplay or
# rendering dependencies, so it can be reused and unit-tested on its own.
add_library(mc_rebedrock_animation STATIC
    src/core/Json.cpp
    src/animation/Molang.cpp
    src/animation/SkeletalModel.cpp
    src/animation/AnimationClip.cpp
    src/animation/Animator.cpp
    src/animation/AnimationAssets.cpp
    src/animation/ModelAnimationSystem.cpp
    src/animation/PlayerModelAnimator.cpp
    src/animation/HingeAnimation.cpp
    src/animation/DisplayEntityAnimation.cpp
)
target_include_directories(mc_rebedrock_animation
    PUBLIC src "${glm_SOURCE_DIR}"
)
if(MSVC)
    target_compile_options(mc_rebedrock_animation PRIVATE /W4 /permissive-)
    if(MC_REBEDROCK_WARNINGS_AS_ERRORS)
        target_compile_options(mc_rebedrock_animation PRIVATE /WX)
    endif()
else()
    target_compile_options(mc_rebedrock_animation PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(MC_REBEDROCK_WARNINGS_AS_ERRORS)
        target_compile_options(mc_rebedrock_animation PRIVATE -Werror)
    endif()
endif()

# The authoritative runtime source chain — everything the simulation needs and
# nothing from render/vulkan/glfw. It is built exactly once, here, and linked by
# everyone who needs it: the game, the dedicated server, and every headless test.
# These files used to be listed again inside each test target, which compiled the
# same ~50 sources once per test binary (~1100 redundant object files in a full
# build); a change to one runtime header rebuilt all of them.
set(MC_REBEDROCK_RUNTIME_SOURCES
    src/runtime/GameRuntime.cpp
    src/gameplay/GameCommandCodec.cpp
    src/gameplay/GameSnapshotCodec.cpp
    src/gameplay/GameEventCodec.cpp
    src/persistence/SaveRepository.cpp
    src/gameplay/ChestSystem.cpp
    src/gameplay/FurnaceSystem.cpp
    src/gameplay/ContentRegistry.cpp
    src/gameplay/CraftingSystem.cpp
    src/gameplay/EntitySystem.cpp
    src/gameplay/EntityRenderSnapshot.cpp
    src/gameplay/GameRules.cpp
    src/gameplay/GameSession.cpp
    src/gameplay/PlayerInteraction.cpp
    src/gameplay/PlayerActionState.cpp
    src/gameplay/ScreenHandler.cpp
    src/ui/HudLayout.cpp
    src/gameplay/GameplayMutationSink.cpp
    src/gameplay/SimulationHostBridge.cpp
    src/gameplay/Inventory.cpp
    src/gameplay/ItemEntitySystem.cpp
    src/gameplay/ItemPlacement.cpp
    src/gameplay/MiningSystem.cpp
    src/gameplay/BlockTags.cpp
    src/assets/ResourceProvider.cpp
    src/assets/PackMetadata.cpp
    src/core/Json.cpp
    src/gameplay/NaturalSpawner.cpp
    src/gameplay/MobSpawnSettings.cpp
    src/gameplay/PlayerController.cpp
    src/gameplay/PlayerVitals.cpp
    src/gameplay/WeatherSystem.cpp
    src/gameplay/WorldSimulation.cpp
    src/gameplay/ChunkTickScheduler.cpp
    src/world/WorldMutationService.cpp
    src/gameplay/entities/EntityType.cpp
    src/gameplay/entities/EntityRegistry.cpp
    src/gameplay/entities/NeutralMob.cpp
    src/gameplay/entities/MobAi.cpp
    src/gameplay/entities/MobBrain.cpp
    src/gameplay/entities/PigEntity.cpp
    src/gameplay/entities/CowEntity.cpp
    src/gameplay/entities/ZombieEntity.cpp
    src/world/BlockPlacement.cpp
    src/world/Chunk.cpp
    src/world/ChunkSection.cpp
    src/world/ChunkStreamer.cpp
    src/world/ChunkMesher.cpp
    src/world/WorldLighting.cpp
    src/world/WorldLightEngine.cpp
    src/world/SurfaceGenerator.cpp
    src/world/gen/Biome.cpp
    src/world/gen/BiomeSource.cpp
    src/world/gen/LayeredBiomeSource.cpp
    src/world/gen/Carver.cpp
    src/world/gen/Features.cpp
    src/world/gen/NoiseChunkGenerator.cpp
    src/world/gen/NoiseSampler.cpp
    src/world/gen/TreeGrower.cpp
    src/world/World.cpp
    src/world/DayNightCycle.cpp
    src/world/WorldClock.cpp
)
# The runtime spawns threads (ChunkStreamer worker, the TCP IO thread). This is
# looked up without REQUIRED because the MinGW cross build reconfigures this same
# file with a toolchain that supplies them itself; where the package exists the
# library passes it on to everything that links it.
find_package(Threads)
add_library(mc_rebedrock_runtime STATIC ${MC_REBEDROCK_RUNTIME_SOURCES})
target_include_directories(mc_rebedrock_runtime PUBLIC src "${glm_SOURCE_DIR}")
if(TARGET Threads::Threads)
    target_link_libraries(mc_rebedrock_runtime PUBLIC Threads::Threads)
endif()
if(MSVC)
    target_compile_options(mc_rebedrock_runtime PRIVATE /W4 /permissive-)
    if(MC_REBEDROCK_WARNINGS_AS_ERRORS)
        target_compile_options(mc_rebedrock_runtime PRIVATE /WX)
    endif()
else()
    target_compile_options(mc_rebedrock_runtime PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
    if(MC_REBEDROCK_WARNINGS_AS_ERRORS)
        target_compile_options(mc_rebedrock_runtime PRIVATE -Werror)
    endif()
endif()

add_executable(mc_rebedrock
    src/main.cpp
    src/assets/ImageData.cpp
    src/assets/FontProviders.cpp
    src/assets/GuiSpriteScaling.cpp
    src/assets/SoundRegistry.cpp
    src/assets/TextureAnimation.cpp
    src/assets/ZipResourcePack.cpp
    vendor/miniz/miniz.c
    src/config/GameOptions.cpp
    src/core/Application.cpp
    src/gameplay/entities/SpeciesRenderData.cpp
    src/render/Frustum.cpp
    src/render/ParticleSystem.cpp
    src/render/RainSystem.cpp
    src/render/PerspectiveCamera.cpp
    src/render/TestScene.cpp
    src/render/vulkan/VulkanRenderer.cpp
    src/render/vulkan/VulkanResources.cpp
    src/render/vulkan/TextureManager.cpp
    src/render/vulkan/BlockAtlasBaker.cpp
    src/render/vulkan/GpuSceneBuffer.cpp
    src/render/vulkan/OffscreenTarget.cpp
    src/ui/MenuGeometry.cpp
    src/ui/BitmapFontMetrics.cpp
    src/ui/Language.cpp
    src/ui/TextFont.cpp
    src/ui/ChatHistory.cpp
    src/ui/PageStack.cpp
    src/world/VoxelRaycast.cpp
)

set(
    MC_REBEDROCK_GAME_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/game"
    CACHE PATH "Output directory containing bin, config, and resources"
)
set_target_properties(mc_rebedrock PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${MC_REBEDROCK_GAME_ROOT}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${MC_REBEDROCK_GAME_ROOT}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${MC_REBEDROCK_GAME_ROOT}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${MC_REBEDROCK_GAME_ROOT}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${MC_REBEDROCK_GAME_ROOT}/bin"
)

if(APPLE)
    set_target_properties(mc_rebedrock PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "/opt/homebrew/lib"
    )
endif()

target_include_directories(mc_rebedrock PRIVATE
    src
    "${glm_SOURCE_DIR}"
    "${stb_SOURCE_DIR}"
    "${vma_SOURCE_DIR}/include"
    vendor/miniz
)
target_link_libraries(mc_rebedrock PRIVATE
    mc_rebedrock_runtime
    Vulkan::Vulkan
    glfw
    mc_rebedrock_animation
    mc_rebedrock_audio
    mc_rebedrock_third_party
)
if(MINGW)
    # Keep the Windows entry point self-contained. Only system DLLs and the
    # Vulkan loader supplied by the GPU driver remain runtime dependencies.
    target_link_options(mc_rebedrock PRIVATE
        -static
        -static-libgcc
        -static-libstdc++
    )
endif()
if(MC_REBEDROCK_MINGW_CROSS)
    add_dependencies(mc_rebedrock mc_rebedrock_vulkan_import)
endif()

target_compile_definitions(mc_rebedrock PRIVATE
    MC_REBEDROCK_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
    MC_REBEDROCK_SHADER_ROOT="${MC_REBEDROCK_GAME_ROOT}/resources/shaders"
    MC_REBEDROCK_CONFIG_ROOT="${MC_REBEDROCK_GAME_ROOT}/config"
)

if(MC_REBEDROCK_STAGE_RUNTIME)
    set(MC_REBEDROCK_RUNTIME_STAMP
        "${MC_REBEDROCK_GAME_ROOT}/resources/.runtime-assets-ready")
    # CONFIGURE_DEPENDS so newly added animation JSONs are re-globbed (and thus
    # re-staged) on an incremental build without needing a manual reconfigure.
    file(GLOB MC_REBEDROCK_ANIMATION_ASSETS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/animation/*.json")
    file(GLOB MC_REBEDROCK_LANGUAGE_ASSETS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/lang/rebedrock/*.json")
    add_custom_command(
        OUTPUT "${MC_REBEDROCK_RUNTIME_STAMP}"
        COMMAND "${CMAKE_COMMAND}"
            "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/resources"
            "-DGAME_ROOT=${MC_REBEDROCK_GAME_ROOT}"
            "-DDEFAULT_OPTIONS=${CMAKE_CURRENT_SOURCE_DIR}/resources/default-options.properties"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntime.cmake"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntime.cmake"
            "${CMAKE_CURRENT_SOURCE_DIR}/resources/default-options.properties"
            "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG_EN.md"
            "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG_CH.md"
            ${MC_REBEDROCK_ANIMATION_ASSETS}
            ${MC_REBEDROCK_LANGUAGE_ASSETS}
        COMMENT "Staging MC Rebedrock runtime resources"
        VERBATIM
    )
    add_custom_target(mc_rebedrock_runtime_assets ALL
        DEPENDS "${MC_REBEDROCK_RUNTIME_STAMP}")
    add_dependencies(mc_rebedrock mc_rebedrock_runtime_assets)
endif()

if(MSVC)
    target_compile_options(mc_rebedrock PRIVATE /W4 /permissive-)
    if(MC_REBEDROCK_WARNINGS_AS_ERRORS)
        target_compile_options(mc_rebedrock PRIVATE /WX)
    endif()
else()
    target_compile_options(mc_rebedrock PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
    )
    if(MC_REBEDROCK_WARNINGS_AS_ERRORS)
        target_compile_options(mc_rebedrock PRIVATE -Werror)
    endif()
endif()

if(APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # VMA 3.4 uses Clang nullability annotations that trigger -Wpedantic on macOS.
    target_compile_options(mc_rebedrock PRIVATE
        -Wno-nullability-completeness
        -Wno-nullability-extension
    )
endif()

# The world generation and meshing form the chunk-loading hot path. A debug
# build without optimization drops their throughput by an order of magnitude,
# so a fast-flying player outruns the streaming worker (vanilla's world
# generation runs optimized even in dev). Compile these translation units
# optimized in every build type; the noise math is IEEE-exact at -O2, so a
# given seed still produces the same world.
# Every translation unit on the chunk pipeline: the noise stack that lays the
# terrain down, the mesher that bakes sections, and the world/light accessors
# they call per voxel. WorldLighting's ChunkLightSampler and World::block sit
# behind every meshed vertex, so leaving them unoptimized stalls the whole
# load.
set(MC_REBEDROCK_HOT_PATH_SOURCES
    src/world/gen/NoiseSampler.cpp
    src/world/gen/NoiseChunkGenerator.cpp
    src/world/gen/Carver.cpp
    src/world/gen/Features.cpp
    src/world/gen/Biome.cpp
    src/world/gen/BiomeSource.cpp
    src/world/gen/LayeredBiomeSource.cpp
    src/world/gen/TreeGrower.cpp
    src/world/Chunk.cpp
    src/world/ChunkSection.cpp
    src/world/ChunkStreamer.cpp
    src/world/ChunkMesher.cpp
    src/world/SurfaceGenerator.cpp
    src/world/World.cpp
    src/world/WorldLightEngine.cpp
    src/world/WorldLighting.cpp
)
if(MSVC)
    set_source_files_properties(${MC_REBEDROCK_HOT_PATH_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/O2")
else()
    set_source_files_properties(${MC_REBEDROCK_HOT_PATH_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-O2")
endif()

if(MC_REBEDROCK_STAGE_RUNTIME)
    find_program(
        GLSLC_EXECUTABLE
        NAMES glslc
        HINTS "$ENV{VULKAN_SDK}/bin"
    )
endif()

if(MC_REBEDROCK_STAGE_RUNTIME AND GLSLC_EXECUTABLE)
    set(MC_REBEDROCK_SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/src")
    set(MC_REBEDROCK_SHADER_OUTPUT_DIR
        "${MC_REBEDROCK_GAME_ROOT}/resources/shaders")
    set(MC_REBEDROCK_SHADER_SOURCES
        grass_block.vert
        grass_block.frag
        block_cutout.frag
        block_outline.vert
        block_outline.frag
        sky.vert
        sky.frag
        hud.vert
        hud.frag
        panorama.vert
        panorama.frag
        item_entity.vert
        item_entity.frag
        occlusion_query.vert
        occlusion_query.frag
        particle_instanced.vert
        particle_instanced.frag
        shadow.vert
        shadow.frag
        shadow_debug.vert
        shadow_debug.frag
        rain_sheet.vert
        rain_sheet.frag
    )
    set(MC_REBEDROCK_SHADER_OUTPUTS)

    foreach(shader IN LISTS MC_REBEDROCK_SHADER_SOURCES)
        set(shader_source "${MC_REBEDROCK_SHADER_SOURCE_DIR}/${shader}")
        set(shader_output "${MC_REBEDROCK_SHADER_OUTPUT_DIR}/${shader}.spv")

        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${MC_REBEDROCK_SHADER_OUTPUT_DIR}"
            COMMAND "${GLSLC_EXECUTABLE}" "${shader_source}" -o "${shader_output}"
            DEPENDS "${shader_source}"
            COMMENT "Compiling shader ${shader}"
            VERBATIM
        )

        list(APPEND MC_REBEDROCK_SHADER_OUTPUTS "${shader_output}")
    endforeach()

    add_custom_target(mc_rebedrock_shaders ALL DEPENDS ${MC_REBEDROCK_SHADER_OUTPUTS})
    add_dependencies(mc_rebedrock mc_rebedrock_shaders)
elseif(MC_REBEDROCK_STAGE_RUNTIME)
    message(FATAL_ERROR "glslc was not found; install the Vulkan SDK before configuring")
endif()

if(APPLE AND MC_REBEDROCK_BUILD_WINDOWS_ON_MAC)
    find_program(MC_REBEDROCK_MINGW_C_COMPILER NAMES x86_64-w64-mingw32-gcc)
    find_program(MC_REBEDROCK_MINGW_CXX_COMPILER NAMES x86_64-w64-mingw32-g++)
    find_program(MC_REBEDROCK_MINGW_DLLTOOL NAMES x86_64-w64-mingw32-dlltool)
    if(NOT MC_REBEDROCK_MINGW_C_COMPILER OR
       NOT MC_REBEDROCK_MINGW_CXX_COMPILER OR
       NOT MC_REBEDROCK_MINGW_DLLTOOL)
        message(FATAL_ERROR
            "The Windows companion build requires mingw-w64. "
            "Install it with: brew install mingw-w64")
    endif()
    set(MC_REBEDROCK_WINDOWS_BUILD_DIR
        "${CMAKE_CURRENT_BINARY_DIR}/windows-x64")
    ExternalProject_Add(mc_rebedrock_windows
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        BINARY_DIR "${MC_REBEDROCK_WINDOWS_BUILD_DIR}"
        CMAKE_GENERATOR "Unix Makefiles"
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/toolchains/mingw-x86_64.cmake"
            "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DBUILD_TESTING=OFF"
            "-DMC_REBEDROCK_BUILD_WINDOWS_ON_MAC=OFF"
            "-DMC_REBEDROCK_STAGE_RUNTIME=OFF"
            "-DMC_REBEDROCK_MINGW_CROSS=ON"
            "-DMC_REBEDROCK_GAME_ROOT=${MC_REBEDROCK_GAME_ROOT}"
            "-DVulkan_INCLUDE_DIR=${Vulkan_INCLUDE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_GLM=${glm_SOURCE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_VMA=${vma_SOURCE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_STB=${stb_SOURCE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_GLFW=${glfw_SOURCE_DIR}"
        BUILD_COMMAND
            "${CMAKE_COMMAND}" --build "${MC_REBEDROCK_WINDOWS_BUILD_DIR}"
                --target mc_rebedrock --parallel 8
        INSTALL_COMMAND ""
        TEST_COMMAND ""
        BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
    )
    add_dependencies(mc_rebedrock mc_rebedrock_windows)
endif()

if(BUILD_TESTING)
    add_executable(mc_rebedrock_nibble_array_test
        tests/nibble_array_test.cpp
        src/world/ChunkSection.cpp
    )
    target_include_directories(mc_rebedrock_nibble_array_test PRIVATE src)
    add_test(NAME nibble_array COMMAND mc_rebedrock_nibble_array_test)

    add_executable(mc_rebedrock_world_light_engine_test
        tests/world_light_engine_test.cpp
    )
    target_link_libraries(mc_rebedrock_world_light_engine_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_world_light_engine_test PRIVATE src)
    add_test(NAME world_light_engine COMMAND mc_rebedrock_world_light_engine_test)

    add_executable(mc_rebedrock_world_constants_test
        tests/world_constants_test.cpp
    )
    target_include_directories(mc_rebedrock_world_constants_test PRIVATE src)
    add_test(NAME world_constants COMMAND mc_rebedrock_world_constants_test)

    add_executable(mc_rebedrock_camera_test
        tests/perspective_camera_test.cpp
        src/render/PerspectiveCamera.cpp
    )
    target_include_directories(mc_rebedrock_camera_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME perspective_camera COMMAND mc_rebedrock_camera_test)

    add_executable(mc_rebedrock_chunk_mesher_test
        tests/chunk_mesher_test.cpp
    )
    target_link_libraries(mc_rebedrock_chunk_mesher_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_chunk_mesher_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME chunk_mesher COMMAND mc_rebedrock_chunk_mesher_test)

    add_executable(mc_rebedrock_world_generation_test
        tests/world_generation_test.cpp
    )
    target_link_libraries(mc_rebedrock_world_generation_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_world_generation_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME world_generation COMMAND mc_rebedrock_world_generation_test)

    add_executable(mc_rebedrock_vertex_lighting_test
        tests/vertex_lighting_test.cpp
    )
    target_link_libraries(mc_rebedrock_vertex_lighting_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_vertex_lighting_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME vertex_lighting COMMAND mc_rebedrock_vertex_lighting_test)

    add_executable(mc_rebedrock_chunk_streamer_test
        tests/chunk_streamer_test.cpp
    )
    target_link_libraries(mc_rebedrock_chunk_streamer_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_chunk_streamer_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME chunk_streamer COMMAND mc_rebedrock_chunk_streamer_test)

    add_executable(mc_rebedrock_voxel_raycast_test
        tests/voxel_raycast_test.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
        src/world/VoxelRaycast.cpp
        src/world/World.cpp
    )
    target_include_directories(mc_rebedrock_voxel_raycast_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME voxel_raycast COMMAND mc_rebedrock_voxel_raycast_test)

    add_executable(mc_rebedrock_frustum_test
        tests/frustum_test.cpp
        src/render/Frustum.cpp
    )
    target_include_directories(mc_rebedrock_frustum_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME frustum COMMAND mc_rebedrock_frustum_test)

    add_executable(mc_rebedrock_streaming_budget_test
        tests/streaming_budget_test.cpp
    )
    target_include_directories(mc_rebedrock_streaming_budget_test PRIVATE
        src
    )
    add_test(NAME streaming_budget COMMAND mc_rebedrock_streaming_budget_test)

    add_executable(mc_rebedrock_string_reader_test
        tests/string_reader_test.cpp
    )
    target_include_directories(mc_rebedrock_string_reader_test PRIVATE src)
    add_test(NAME string_reader COMMAND mc_rebedrock_string_reader_test)

    add_executable(mc_rebedrock_command_dispatcher_test
        tests/command_dispatcher_test.cpp
    )
    target_link_libraries(mc_rebedrock_command_dispatcher_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_command_dispatcher_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME command_dispatcher COMMAND mc_rebedrock_command_dispatcher_test)

    add_executable(mc_rebedrock_command_suggestions_test
        tests/command_suggestions_test.cpp
    )
    target_link_libraries(mc_rebedrock_command_suggestions_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_command_suggestions_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME command_suggestions COMMAND mc_rebedrock_command_suggestions_test)

    add_executable(mc_rebedrock_entity_registry_test
        tests/entity_registry_test.cpp
    )
    target_link_libraries(mc_rebedrock_entity_registry_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_entity_registry_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME entity_registry COMMAND mc_rebedrock_entity_registry_test)

    add_executable(mc_rebedrock_mob_brain_test
        tests/mob_brain_test.cpp
    )
    target_link_libraries(mc_rebedrock_mob_brain_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_mob_brain_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME mob_brain COMMAND mc_rebedrock_mob_brain_test)

    # Manual tools, not part of the test suite: they take arguments or print
    # numbers instead of asserting, so ctest cannot judge them and a normal build
    # has no reason to compile them. Configure with -DMC_REBEDROCK_BUILD_TOOLS=ON
    # when you need one (hot-path work, a migration check).
    if(MC_REBEDROCK_BUILD_TOOLS)
        # Informational: compares an empty Brain against 70 pursuing zombies.
        add_executable(mc_rebedrock_mob_ai_benchmark
            tests/mob_ai_benchmark.cpp
        )
        target_link_libraries(mc_rebedrock_mob_ai_benchmark PRIVATE mc_rebedrock_runtime)
    endif()

    add_executable(mc_rebedrock_game_session_test
        tests/game_session_test.cpp
    )
    target_link_libraries(mc_rebedrock_game_session_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_game_session_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME game_session COMMAND mc_rebedrock_game_session_test)
    add_executable(mc_rebedrock_snapshot_budget_test
        tests/snapshot_budget_test.cpp
    )
    target_link_libraries(mc_rebedrock_snapshot_budget_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_snapshot_budget_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME snapshot_budget COMMAND mc_rebedrock_snapshot_budget_test)
    add_executable(mc_rebedrock_game_command_codec_test
        tests/game_command_codec_test.cpp
        src/gameplay/GameCommandCodec.cpp
    )
    target_include_directories(mc_rebedrock_game_command_codec_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME game_command_codec COMMAND mc_rebedrock_game_command_codec_test)
    add_executable(mc_rebedrock_game_snapshot_codec_test
        tests/game_snapshot_codec_test.cpp
    )
    target_link_libraries(mc_rebedrock_game_snapshot_codec_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_game_snapshot_codec_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME game_snapshot_codec COMMAND mc_rebedrock_game_snapshot_codec_test)
    add_executable(mc_rebedrock_game_event_codec_test
        tests/game_event_codec_test.cpp
    )
    target_link_libraries(mc_rebedrock_game_event_codec_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_game_event_codec_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME game_event_codec COMMAND mc_rebedrock_game_event_codec_test)
    add_executable(mc_rebedrock_loopback_transport_test
        tests/loopback_transport_test.cpp
    )
    target_link_libraries(mc_rebedrock_loopback_transport_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_loopback_transport_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    find_package(Threads REQUIRED)
    target_link_libraries(mc_rebedrock_loopback_transport_test PRIVATE Threads::Threads)
    add_test(NAME loopback_transport COMMAND mc_rebedrock_loopback_transport_test)
    add_executable(mc_rebedrock_tcp_transport_test
        tests/tcp_transport_test.cpp
    )
    target_link_libraries(mc_rebedrock_tcp_transport_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_tcp_transport_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    target_link_libraries(mc_rebedrock_tcp_transport_test PRIVATE Threads::Threads)
    add_test(NAME tcp_transport COMMAND mc_rebedrock_tcp_transport_test)
    add_executable(mc_rebedrock_handshake_test
        tests/handshake_test.cpp
    )
    target_link_libraries(mc_rebedrock_handshake_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_handshake_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    target_link_libraries(mc_rebedrock_handshake_test PRIVATE Threads::Threads)
    add_test(NAME handshake COMMAND mc_rebedrock_handshake_test)
    add_executable(mc_rebedrock_player_interaction_test
        tests/player_interaction_test.cpp
    )
    target_link_libraries(mc_rebedrock_player_interaction_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_player_interaction_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME player_interaction COMMAND mc_rebedrock_player_interaction_test)
    add_executable(mc_rebedrock_multi_player_test
        tests/multi_player_test.cpp
    )
    target_link_libraries(mc_rebedrock_multi_player_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_multi_player_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME multi_player COMMAND mc_rebedrock_multi_player_test)

    add_executable(mc_rebedrock_game_runtime_test
        tests/game_runtime_test.cpp
    )
    target_link_libraries(mc_rebedrock_game_runtime_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_game_runtime_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME game_runtime COMMAND mc_rebedrock_game_runtime_test)

    # D7: the dedicated server executable — links the runtime, not render/vulkan.
    add_executable(mc_rebedrock_dedicated_server
        src/server/dedicated_server_main.cpp
    )
    target_include_directories(mc_rebedrock_dedicated_server PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    target_link_libraries(mc_rebedrock_dedicated_server PRIVATE mc_rebedrock_runtime)

    add_executable(mc_rebedrock_dedicated_server_test
        tests/dedicated_server_test.cpp
    )
    target_include_directories(mc_rebedrock_dedicated_server_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    target_link_libraries(mc_rebedrock_dedicated_server_test PRIVATE mc_rebedrock_runtime)
    add_test(NAME dedicated_server COMMAND mc_rebedrock_dedicated_server_test)

    add_executable(mc_rebedrock_player_action_state_test
        tests/player_action_state_test.cpp
        src/gameplay/PlayerActionState.cpp
    )
    target_include_directories(mc_rebedrock_player_action_state_test PRIVATE src)
    add_test(NAME player_action_state COMMAND mc_rebedrock_player_action_state_test)
    add_executable(mc_rebedrock_player_render_state_test
        tests/player_render_state_test.cpp
    )
    target_link_libraries(mc_rebedrock_player_render_state_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_player_render_state_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME player_render_state COMMAND mc_rebedrock_player_render_state_test)

    add_executable(mc_rebedrock_mob_spawn_settings_test
        tests/mob_spawn_settings_test.cpp
    )
    target_link_libraries(mc_rebedrock_mob_spawn_settings_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_mob_spawn_settings_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME mob_spawn_settings COMMAND mc_rebedrock_mob_spawn_settings_test)

    add_executable(mc_rebedrock_damage_pipeline_test
        tests/damage_pipeline_test.cpp
        src/gameplay/PlayerVitals.cpp
    )
    target_include_directories(mc_rebedrock_damage_pipeline_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME damage_pipeline COMMAND mc_rebedrock_damage_pipeline_test)

    add_executable(mc_rebedrock_screen_handler_test
        tests/screen_handler_test.cpp
    )
    target_link_libraries(mc_rebedrock_screen_handler_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_screen_handler_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME screen_handler COMMAND mc_rebedrock_screen_handler_test)

    add_executable(mc_rebedrock_mutation_flow_test
        tests/mutation_flow_test.cpp
    )
    target_link_libraries(mc_rebedrock_mutation_flow_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_mutation_flow_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME mutation_flow COMMAND mc_rebedrock_mutation_flow_test)

    add_executable(mc_rebedrock_game_events_test
        tests/game_events_test.cpp
    )
    target_link_libraries(mc_rebedrock_game_events_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_game_events_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME game_events COMMAND mc_rebedrock_game_events_test)

    add_executable(mc_rebedrock_entity_render_snapshot_test
        tests/entity_render_snapshot_test.cpp
    )
    target_link_libraries(mc_rebedrock_entity_render_snapshot_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_entity_render_snapshot_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME entity_render_snapshot COMMAND mc_rebedrock_entity_render_snapshot_test)

    add_executable(mc_rebedrock_natural_spawn_test
        tests/natural_spawn_test.cpp
    )
    target_link_libraries(mc_rebedrock_natural_spawn_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_natural_spawn_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME natural_spawn COMMAND mc_rebedrock_natural_spawn_test)

    add_executable(mc_rebedrock_spawn_placement_test
        tests/spawn_placement_test.cpp
    )
    target_link_libraries(mc_rebedrock_spawn_placement_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_spawn_placement_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME spawn_placement COMMAND mc_rebedrock_spawn_placement_test)

    add_executable(mc_rebedrock_weather_system_test
        tests/weather_system_test.cpp
        src/gameplay/WeatherSystem.cpp
    )
    target_include_directories(mc_rebedrock_weather_system_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME weather_system COMMAND mc_rebedrock_weather_system_test)

    add_executable(mc_rebedrock_crafting_system_test
        tests/crafting_system_test.cpp
    )
    target_link_libraries(mc_rebedrock_crafting_system_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_crafting_system_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME crafting_system COMMAND mc_rebedrock_crafting_system_test)

    add_executable(mc_rebedrock_furnace_system_test
        tests/furnace_system_test.cpp
    )
    target_link_libraries(mc_rebedrock_furnace_system_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_furnace_system_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME furnace_system COMMAND mc_rebedrock_furnace_system_test)

    add_executable(mc_rebedrock_resource_provider_test
        tests/resource_provider_test.cpp
        src/assets/PackMetadata.cpp
        src/assets/ResourceProvider.cpp
        src/core/Json.cpp
    )
    target_include_directories(mc_rebedrock_resource_provider_test PRIVATE src)
    add_test(NAME resource_provider COMMAND mc_rebedrock_resource_provider_test)

    add_executable(mc_rebedrock_pack_metadata_test
        tests/pack_metadata_test.cpp
        src/assets/PackMetadata.cpp
        src/core/Json.cpp
    )
    target_include_directories(mc_rebedrock_pack_metadata_test PRIVATE src)
    add_test(NAME pack_metadata COMMAND mc_rebedrock_pack_metadata_test)

    add_executable(mc_rebedrock_gui_nine_slice_test
        tests/gui_nine_slice_test.cpp
        src/assets/GuiSpriteScaling.cpp
    )
    target_link_libraries(mc_rebedrock_gui_nine_slice_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_gui_nine_slice_test PRIVATE src)
    target_link_libraries(mc_rebedrock_gui_nine_slice_test PRIVATE glm::glm)
    add_test(NAME gui_nine_slice COMMAND mc_rebedrock_gui_nine_slice_test)

    add_executable(mc_rebedrock_simulation_driver_test
        tests/simulation_driver_test.cpp
    )
    target_link_libraries(mc_rebedrock_simulation_driver_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_simulation_driver_test PRIVATE src "${glm_SOURCE_DIR}")
    target_link_libraries(mc_rebedrock_simulation_driver_test PRIVATE Threads::Threads)
    add_test(NAME simulation_driver COMMAND mc_rebedrock_simulation_driver_test)

    add_executable(mc_rebedrock_world_lock_test
        tests/world_lock_test.cpp
        src/world/World.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
    )
    target_include_directories(mc_rebedrock_world_lock_test PRIVATE src "${glm_SOURCE_DIR}")
    find_package(Threads REQUIRED)
    target_link_libraries(mc_rebedrock_world_lock_test PRIVATE Threads::Threads)
    add_test(NAME world_lock COMMAND mc_rebedrock_world_lock_test)

    add_executable(mc_rebedrock_chunk_tick_scheduler_test
        tests/chunk_tick_scheduler_test.cpp
        src/gameplay/ChunkTickScheduler.cpp
    )
    target_include_directories(mc_rebedrock_chunk_tick_scheduler_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME chunk_tick_scheduler COMMAND mc_rebedrock_chunk_tick_scheduler_test)

    add_executable(mc_rebedrock_block_tags_test
        tests/block_tags_test.cpp
    )
    target_link_libraries(mc_rebedrock_block_tags_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_block_tags_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME block_tags COMMAND mc_rebedrock_block_tags_test)

    add_executable(mc_rebedrock_gui_sprite_scaling_test
        tests/gui_sprite_scaling_test.cpp
        src/assets/GuiSpriteScaling.cpp
        src/assets/ResourceProvider.cpp
        src/assets/PackMetadata.cpp
        src/core/Json.cpp
    )
    target_include_directories(mc_rebedrock_gui_sprite_scaling_test PRIVATE src)
    add_test(NAME gui_sprite_scaling COMMAND mc_rebedrock_gui_sprite_scaling_test)

    add_executable(mc_rebedrock_menu_geometry_test
        tests/menu_geometry_test.cpp
        src/ui/MenuGeometry.cpp
        src/ui/HudLayout.cpp
    )
    target_include_directories(mc_rebedrock_menu_geometry_test PRIVATE src)
    target_link_libraries(mc_rebedrock_menu_geometry_test PRIVATE glm::glm)
    add_test(NAME menu_geometry COMMAND mc_rebedrock_menu_geometry_test)

    add_executable(mc_rebedrock_texture_animation_test
        tests/texture_animation_test.cpp
        src/assets/TextureAnimation.cpp
        src/assets/ResourceProvider.cpp
        src/assets/PackMetadata.cpp
        src/core/Json.cpp
    )
    target_include_directories(mc_rebedrock_texture_animation_test PRIVATE src)
    add_test(NAME texture_animation COMMAND mc_rebedrock_texture_animation_test)

    add_executable(mc_rebedrock_species_skin_test
        tests/species_skin_test.cpp
        src/gameplay/entities/SpeciesRenderData.cpp
        src/assets/ImageData.cpp
        src/assets/StbImageImplementation.cpp
    )
    target_link_libraries(mc_rebedrock_species_skin_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_species_skin_test PRIVATE
        src
        "${stb_SOURCE_DIR}"
        "${glm_SOURCE_DIR}"
    )
    target_link_libraries(mc_rebedrock_species_skin_test PRIVATE mc_rebedrock_animation)
    add_test(NAME species_skin COMMAND mc_rebedrock_species_skin_test)

    add_executable(mc_rebedrock_image_data_test
        tests/image_data_test.cpp
        src/assets/ImageData.cpp
        src/assets/StbImageImplementation.cpp
    )
    target_include_directories(mc_rebedrock_image_data_test PRIVATE src "${stb_SOURCE_DIR}")
    add_test(NAME image_data COMMAND mc_rebedrock_image_data_test)

    add_executable(mc_rebedrock_sound_registry_test
        tests/sound_registry_test.cpp
        src/assets/PackMetadata.cpp
        src/assets/SoundRegistry.cpp
        src/assets/ResourceProvider.cpp
        src/core/Json.cpp
    )
    target_include_directories(mc_rebedrock_sound_registry_test PRIVATE src)
    add_test(NAME sound_registry COMMAND mc_rebedrock_sound_registry_test)

    add_executable(mc_rebedrock_language_provider_test
        tests/language_provider_test.cpp
        src/ui/Language.cpp
        src/ui/TextFont.cpp
        src/ui/BitmapFontMetrics.cpp
        src/assets/PackMetadata.cpp
        src/assets/ResourceProvider.cpp
        src/core/Json.cpp
    )
    target_include_directories(mc_rebedrock_language_provider_test PRIVATE src)
    add_test(NAME language_provider COMMAND mc_rebedrock_language_provider_test)

    add_executable(mc_rebedrock_font_providers_test
        tests/font_providers_test.cpp
        src/assets/FontProviders.cpp
        src/assets/PackMetadata.cpp
        src/assets/ResourceProvider.cpp
        src/core/Json.cpp
        src/ui/BitmapFontMetrics.cpp
        src/ui/TextFont.cpp
    )
    target_include_directories(mc_rebedrock_font_providers_test PRIVATE src)
    add_test(NAME font_providers COMMAND mc_rebedrock_font_providers_test)

    add_executable(mc_rebedrock_zip_resource_pack_test
        tests/zip_resource_pack_test.cpp
        src/assets/PackMetadata.cpp
        src/assets/ZipResourcePack.cpp
        src/assets/ResourceProvider.cpp
        src/core/Json.cpp
        vendor/miniz/miniz.c
    )
    target_include_directories(mc_rebedrock_zip_resource_pack_test PRIVATE src vendor/miniz)
    add_test(NAME zip_resource_pack COMMAND mc_rebedrock_zip_resource_pack_test)

    add_executable(mc_rebedrock_mining_system_test
        tests/mining_system_test.cpp
    )
    target_link_libraries(mc_rebedrock_mining_system_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_mining_system_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME mining_system COMMAND mc_rebedrock_mining_system_test)

    add_executable(mc_rebedrock_block_state_test
        tests/block_state_test.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
        src/world/World.cpp
    )
    target_include_directories(mc_rebedrock_block_state_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME block_state COMMAND mc_rebedrock_block_state_test)

    add_executable(mc_rebedrock_block_shape_test
        tests/block_shape_test.cpp
    )
    target_include_directories(mc_rebedrock_block_shape_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME block_shape COMMAND mc_rebedrock_block_shape_test)

    add_executable(mc_rebedrock_world_mutation_service_test
        tests/world_mutation_service_test.cpp
    )
    target_link_libraries(mc_rebedrock_world_mutation_service_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_world_mutation_service_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME world_mutation_service COMMAND mc_rebedrock_world_mutation_service_test)

    add_executable(mc_rebedrock_neighbor_updater_test
        tests/neighbor_updater_test.cpp
    )
    target_include_directories(mc_rebedrock_neighbor_updater_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME neighbor_updater COMMAND mc_rebedrock_neighbor_updater_test)

    add_executable(mc_rebedrock_tick_priority_and_events_test
        tests/tick_priority_and_events_test.cpp
        src/gameplay/ChunkTickScheduler.cpp
    )
    target_include_directories(mc_rebedrock_tick_priority_and_events_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME tick_priority_and_events COMMAND mc_rebedrock_tick_priority_and_events_test)

    add_executable(mc_rebedrock_update_shape_test
        tests/update_shape_test.cpp
        src/world/World.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
    )
    target_include_directories(mc_rebedrock_update_shape_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME update_shape COMMAND mc_rebedrock_update_shape_test)

    add_executable(mc_rebedrock_redstone_signal_test
        tests/redstone_signal_test.cpp
        src/world/World.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
    )
    target_include_directories(mc_rebedrock_redstone_signal_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME redstone_signal COMMAND mc_rebedrock_redstone_signal_test)

    add_executable(mc_rebedrock_redstone_torch_test
        tests/redstone_torch_test.cpp
    )
    target_include_directories(mc_rebedrock_redstone_torch_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME redstone_torch COMMAND mc_rebedrock_redstone_torch_test)

    add_executable(mc_rebedrock_redstone_diode_test
        tests/redstone_diode_test.cpp
    )
    target_include_directories(mc_rebedrock_redstone_diode_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME redstone_diode COMMAND mc_rebedrock_redstone_diode_test)

    add_executable(mc_rebedrock_redstone_drive_test
        tests/redstone_drive_test.cpp
    )
    target_link_libraries(mc_rebedrock_redstone_drive_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_redstone_drive_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME redstone_drive COMMAND mc_rebedrock_redstone_drive_test)

    add_executable(mc_rebedrock_torch_inverter_fixture_test
        tests/redstone/torch_inverter_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_torch_inverter_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_torch_inverter_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME torch_inverter_fixture COMMAND mc_rebedrock_torch_inverter_fixture_test)

    add_executable(mc_rebedrock_repeater_delay_fixture_test
        tests/redstone/repeater_delay_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_repeater_delay_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_repeater_delay_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME repeater_delay_fixture COMMAND mc_rebedrock_repeater_delay_fixture_test)

    add_executable(mc_rebedrock_comparator_subtract_fixture_test
        tests/redstone/comparator_subtract_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_comparator_subtract_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_comparator_subtract_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME comparator_subtract_fixture COMMAND mc_rebedrock_comparator_subtract_fixture_test)

    add_executable(mc_rebedrock_wire_heavy_fixture_test
        tests/redstone/wire_heavy_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_wire_heavy_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_wire_heavy_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME wire_heavy_fixture COMMAND mc_rebedrock_wire_heavy_fixture_test)

    add_executable(mc_rebedrock_wire_drives_repeater_fixture_test
        tests/redstone/wire_drives_repeater_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_wire_drives_repeater_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_wire_drives_repeater_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME wire_drives_repeater_fixture COMMAND mc_rebedrock_wire_drives_repeater_fixture_test)

    add_executable(mc_rebedrock_observer_pulse_fixture_test
        tests/redstone/observer_pulse_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_observer_pulse_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_observer_pulse_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME observer_pulse_fixture COMMAND mc_rebedrock_observer_pulse_fixture_test)

    add_executable(mc_rebedrock_button_pulse_fixture_test
        tests/redstone/button_pulse_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_button_pulse_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_button_pulse_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME button_pulse_fixture COMMAND mc_rebedrock_button_pulse_fixture_test)

    add_executable(mc_rebedrock_piston_event_fixture_test
        tests/redstone/piston_event_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_piston_event_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_piston_event_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME piston_event_fixture COMMAND mc_rebedrock_piston_event_fixture_test)

    add_executable(mc_rebedrock_chunk_section_test
        tests/chunk_section_test.cpp
        src/world/ChunkSection.cpp
    )
    target_include_directories(mc_rebedrock_chunk_section_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME chunk_section COMMAND mc_rebedrock_chunk_section_test)

    add_executable(mc_rebedrock_item_entity_test
        tests/item_entity_test.cpp
    )
    target_link_libraries(mc_rebedrock_item_entity_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_item_entity_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME item_entity COMMAND mc_rebedrock_item_entity_test)

    add_executable(mc_rebedrock_environment_snapshot_test
        tests/environment_snapshot_test.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
        src/world/World.cpp
    )
    target_include_directories(mc_rebedrock_environment_snapshot_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME environment_snapshot COMMAND mc_rebedrock_environment_snapshot_test)

    add_executable(mc_rebedrock_world_simulation_test
        tests/world_simulation_test.cpp
    )
    target_link_libraries(mc_rebedrock_world_simulation_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_world_simulation_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME world_simulation COMMAND mc_rebedrock_world_simulation_test)

    add_executable(mc_rebedrock_player_controller_test
        tests/player_controller_test.cpp
    )
    target_link_libraries(mc_rebedrock_player_controller_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_player_controller_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME player_controller COMMAND mc_rebedrock_player_controller_test)

    add_executable(mc_rebedrock_tree_grower_test
        tests/tree_grower_test.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
        src/world/gen/TreeGrower.cpp
    )
    target_include_directories(mc_rebedrock_tree_grower_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME tree_grower COMMAND mc_rebedrock_tree_grower_test)

    if(MC_REBEDROCK_BUILD_TOOLS)
        # Informational: ms-per-tick of the random-tick pass, the A/B baseline for
        # hot-path changes (see docs/content-dev/R0-identity/R0-2-block-deenum.md).
        add_executable(mc_rebedrock_random_tick_benchmark
            tests/random_tick_benchmark.cpp
        )
        target_link_libraries(mc_rebedrock_random_tick_benchmark PRIVATE mc_rebedrock_runtime)
    endif()

    add_executable(mc_rebedrock_game_options_test
        tests/game_options_test.cpp
        src/config/GameOptions.cpp
    )
    target_include_directories(mc_rebedrock_game_options_test PRIVATE src)
    add_test(NAME game_options COMMAND mc_rebedrock_game_options_test)

    add_executable(mc_rebedrock_test_scene_test
        tests/test_scene_test.cpp
        src/render/TestScene.cpp
    )
    target_include_directories(mc_rebedrock_test_scene_test PRIVATE src)
    add_test(NAME test_scene COMMAND mc_rebedrock_test_scene_test)

    add_executable(mc_rebedrock_audio_system_test
        tests/audio_system_test.cpp
        src/assets/PackMetadata.cpp
        src/assets/SoundRegistry.cpp
        src/assets/ResourceProvider.cpp
        src/core/Json.cpp
    )
    target_link_libraries(mc_rebedrock_audio_system_test PRIVATE mc_rebedrock_audio)
    add_test(NAME audio_system COMMAND mc_rebedrock_audio_system_test)

    # Every animation test links the standalone animation library, proving it
    # stands on its own without gameplay or rendering sources.
    add_executable(mc_rebedrock_model_animation_test tests/model_animation_test.cpp)
    target_link_libraries(mc_rebedrock_model_animation_test PRIVATE mc_rebedrock_animation)
    add_test(NAME model_animation COMMAND mc_rebedrock_model_animation_test)

    add_executable(mc_rebedrock_player_model_animation_test
        tests/player_model_animation_test.cpp)
    target_link_libraries(mc_rebedrock_player_model_animation_test PRIVATE mc_rebedrock_animation)
    add_test(NAME player_model_animation COMMAND mc_rebedrock_player_model_animation_test)

    add_executable(mc_rebedrock_box_uv_test
        tests/box_uv_test.cpp)
    target_link_libraries(mc_rebedrock_box_uv_test PRIVATE mc_rebedrock_animation)
    add_test(NAME box_uv COMMAND mc_rebedrock_box_uv_test)

    add_executable(mc_rebedrock_json_test tests/json_test.cpp)
    target_link_libraries(mc_rebedrock_json_test PRIVATE mc_rebedrock_animation)
    add_test(NAME json COMMAND mc_rebedrock_json_test)

    add_executable(mc_rebedrock_molang_test tests/molang_test.cpp)
    target_link_libraries(mc_rebedrock_molang_test PRIVATE mc_rebedrock_animation)
    add_test(NAME molang COMMAND mc_rebedrock_molang_test)

    add_executable(mc_rebedrock_skeletal_animation_test tests/skeletal_animation_test.cpp)
    target_link_libraries(mc_rebedrock_skeletal_animation_test PRIVATE mc_rebedrock_animation)
    add_test(NAME skeletal_animation COMMAND mc_rebedrock_skeletal_animation_test)

    add_executable(mc_rebedrock_keyframe_bezier_test tests/keyframe_bezier_test.cpp)
    target_link_libraries(mc_rebedrock_keyframe_bezier_test PRIVATE mc_rebedrock_animation)
    add_test(NAME keyframe_bezier COMMAND mc_rebedrock_keyframe_bezier_test)

    add_executable(mc_rebedrock_animation_presets_test tests/animation_presets_test.cpp)
    target_link_libraries(mc_rebedrock_animation_presets_test PRIVATE mc_rebedrock_animation)
    add_test(NAME animation_presets COMMAND mc_rebedrock_animation_presets_test)

    add_executable(mc_rebedrock_animation_assets_test tests/animation_assets_test.cpp)
    target_link_libraries(mc_rebedrock_animation_assets_test PRIVATE mc_rebedrock_animation)
    target_compile_definitions(mc_rebedrock_animation_assets_test PRIVATE
        "MC_REBEDROCK_ANIMATION_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/resources/animation\"")
    add_test(NAME animation_assets COMMAND mc_rebedrock_animation_assets_test)

    add_executable(mc_rebedrock_particle_system_test
        tests/particle_system_test.cpp
        src/render/ParticleSystem.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
        src/world/World.cpp
    )
    target_include_directories(mc_rebedrock_particle_system_test PRIVATE
        src "${glm_SOURCE_DIR}")
    add_test(NAME particle_system COMMAND mc_rebedrock_particle_system_test)

    add_executable(mc_rebedrock_rain_system_test
        tests/rain_system_test.cpp
        src/render/RainSystem.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
        src/world/World.cpp
    )
    target_include_directories(mc_rebedrock_rain_system_test PRIVATE
        src "${glm_SOURCE_DIR}")
    add_test(NAME rain_system COMMAND mc_rebedrock_rain_system_test)

    add_executable(mc_rebedrock_save_repository_test
        tests/save_repository_test.cpp
    )
    target_link_libraries(mc_rebedrock_save_repository_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_save_repository_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME save_repository COMMAND mc_rebedrock_save_repository_test)

    if(MC_REBEDROCK_BUILD_TOOLS)
        # migration_diag <world-dir> loads a real old-format world, saves it
        # (rewriting to the current format) and reloads to prove the migration lost
        # nothing. It needs a world path, so it can never be a ctest entry; it is
        # kept so any real save can be re-checked after a persistence change.
        add_executable(mc_rebedrock_migration_diag
            tests/migration_diag.cpp
        )
        target_link_libraries(mc_rebedrock_migration_diag PRIVATE mc_rebedrock_runtime)
    endif()

    add_executable(mc_rebedrock_game_rules_registry_test
        tests/game_rules_registry_test.cpp
        src/gameplay/GameRules.cpp
    )
    target_include_directories(mc_rebedrock_game_rules_registry_test PRIVATE src)
    add_test(NAME game_rules_registry COMMAND mc_rebedrock_game_rules_registry_test)

    add_executable(mc_rebedrock_content_registry_test
        tests/content_registry_test.cpp
        src/gameplay/ContentRegistry.cpp
        src/gameplay/Inventory.cpp
    )
    target_include_directories(mc_rebedrock_content_registry_test PRIVATE src)
    add_test(NAME content_registry COMMAND mc_rebedrock_content_registry_test)

    add_executable(mc_rebedrock_registry_test
        tests/registry_test.cpp
    )
    target_include_directories(mc_rebedrock_registry_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME registry COMMAND mc_rebedrock_registry_test)

    add_executable(mc_rebedrock_block_registry_lifecycle_test
        tests/block_registry_lifecycle_test.cpp
    )
    target_include_directories(mc_rebedrock_block_registry_lifecycle_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME block_registry_lifecycle COMMAND mc_rebedrock_block_registry_lifecycle_test)

    add_executable(mc_rebedrock_block_deenum_test
        tests/block_deenum_test.cpp
    )
    target_link_libraries(mc_rebedrock_block_deenum_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_block_deenum_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME block_deenum COMMAND mc_rebedrock_block_deenum_test)

    add_executable(mc_rebedrock_block_behavior_test
        tests/block_behavior_test.cpp
    )
    target_link_libraries(mc_rebedrock_block_behavior_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_block_behavior_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME block_behavior COMMAND mc_rebedrock_block_behavior_test)

    endif()

install(TARGETS mc_rebedrock RUNTIME DESTINATION bin)
# No Mojang assets are installed: textures, sounds, fonts and translations come
# from a standard resource pack the user supplies in resourcepacks/. Only
# ReBedrock's own authored assets ship.
install(DIRECTORY "${MC_REBEDROCK_SHADER_OUTPUT_DIR}/"
    DESTINATION resources/shaders)
install(DIRECTORY resources/animation
    DESTINATION resources)
install(FILES resources/default-options.properties
    DESTINATION config
    RENAME options.properties)
install(DIRECTORY DESTINATION saves)
