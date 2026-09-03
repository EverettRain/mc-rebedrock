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

# --- constexpr evaluation budget --------------------------------------------
# rebedrock bakes content into constexpr tables at compile time (DOD): the block
# registry, the interned block-state metadata table (kBlockStateMetadata), and
# the O(N^2) blockRegistryIsWellFormed() uniqueness check all evaluate at compile
# time. Their cost grows with the block/state roster, and the compilers' default
# constexpr step budgets are not uniform: GCC allows ~33M ops, but Clang defaults
# to ~1M steps and MSVC lower still — so once the roster grew (STRUCT block
# registration, ~2500 states) a Clang/MSVC build began failing with
# "static assertion expression is not an integral constant expression" while GCC
# still passed. Raise the ceiling far past current need on every compiler. This
# only raises the *limit*; it does not change the actual evaluation cost, so it
# has no effect on build time until the tables genuinely grow into it. Scoped to
# C++ so the vendored C sources (miniz, glfw) do not get a C++-only flag.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fconstexpr-steps=1000000000>)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-fconstexpr-ops-limit=1000000000>)
elseif(MSVC)
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:/constexpr:steps1000000000>)
endif()

# --- Build metadata for the single version manifest (META-0) -----------------
# core/VersionManifest.hpp's kBuildTime / kBuildRef are baked in from the git
# commit, NOT the wall clock: the commit's author date and short hash. Two
# configures of the same commit therefore produce byte-identical metadata — a
# build's identity is a function of its source, not of when it was built (see
# content-dev/META-metadata/REGULAR.md §3 determinism). Outside a checkout both
# fall back to "unknown"; nothing ever reads the current system time.
set(MC_REBEDROCK_BUILD_TIME "unknown")
set(MC_REBEDROCK_BUILD_REF "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                show -s --format=%cd --date=format:%Y-%m-%dT%H:%M:%SZ HEAD
        OUTPUT_VARIABLE _MC_REBEDROCK_GIT_TIME
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _MC_REBEDROCK_GIT_TIME_RC)
    if(_MC_REBEDROCK_GIT_TIME_RC EQUAL 0 AND _MC_REBEDROCK_GIT_TIME)
        set(MC_REBEDROCK_BUILD_TIME "${_MC_REBEDROCK_GIT_TIME}")
    endif()
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                rev-parse --short HEAD
        OUTPUT_VARIABLE _MC_REBEDROCK_GIT_REF
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _MC_REBEDROCK_GIT_REF_RC)
    if(_MC_REBEDROCK_GIT_REF_RC EQUAL 0 AND _MC_REBEDROCK_GIT_REF)
        set(MC_REBEDROCK_BUILD_REF "${_MC_REBEDROCK_GIT_REF}")
    endif()
endif()
# The generated header lands under the build tree; its directory is added to the
# runtime library's PUBLIC include path so every consumer sees core/BuildInfo.hpp.
set(MC_REBEDROCK_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/BuildInfo.hpp.in"
    "${MC_REBEDROCK_GENERATED_INCLUDE_DIR}/core/BuildInfo.hpp"
    @ONLY)

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
    # A COFF object file can hold at most 32767 sections, and VulkanRenderer.cpp
    # went past it: "too many sections (33362) ... file too big". It is a ~7k-line
    # TU that also pulls in the header-only HudRenderer/WorldRenderer split, and
    # every inline member, template instantiation and lambda in that pile lands in
    # its own COMDAT section (plus .pdata/.xdata/.debug_* companions in a Debug
    # build) — so the count tracks how much header-only code the renderer
    # includes, and it had been sitting just under the cap.
    #
    # -mbig-obj switches the assembler to the extended COFF variant, raising the
    # limit to 2^31; mingw-w64's ld, ar and nm read it natively, and it is a no-op
    # for objects small enough not to need it. Applied to the whole cross build
    # rather than the one file that overflowed first, so the next TU to cross the
    # line does not repeat this failure. ELF (Linux) and Mach-O (macOS) have no
    # such cap, which is why only the Windows companion build ever hit it.
    #
    # Here rather than in cmake/toolchains/mingw-x86_64.cmake because
    # CMAKE_CXX_FLAGS_INIT is only consulted on a *fresh* configure, while editing
    # this file re-runs cmake in the existing windows-x64 tree on the next build.
    add_compile_options(-Wa,-mbig-obj)

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
    src/audio/AmbientMusicScheduler.cpp
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
    src/animation/PartDefinition.cpp
    src/animation/AnimationClip.cpp
    src/animation/Animator.cpp
    src/animation/BoneMask.cpp
    src/animation/AnimationController.cpp
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
    src/gameplay/CustomNames.cpp
    src/gameplay/CraftingSystem.cpp
    src/gameplay/RecipeTable.cpp
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
    src/gameplay/LootTable.cpp
    src/gameplay/ChestLootTable.cpp
    src/gameplay/BlockTags.cpp
    src/gameplay/DataPackStack.cpp
    src/gameplay/FunctionManager.cpp
    src/assets/ResourceProvider.cpp
    src/assets/PackMetadata.cpp
    src/assets/PackManager.cpp
    src/core/Json.cpp
    src/gameplay/NaturalSpawner.cpp
    src/gameplay/MobSpawnSettings.cpp
    src/gameplay/PlayerController.cpp
    src/gameplay/PlayerVitals.cpp
    src/input/InputSystem.cpp
    src/gameplay/StatusEffect.cpp
    src/gameplay/WeatherSystem.cpp
    src/gameplay/WorldSimulation.cpp
    src/gameplay/ChunkTickScheduler.cpp
    src/world/WorldMutationService.cpp
    src/gameplay/entities/EntityType.cpp
    src/gameplay/entities/EntityRegistry.cpp
    src/gameplay/entities/EntityAttributeOverlay.cpp
    src/gameplay/entities/BuiltinSpecies.cpp
    src/gameplay/entities/UnknownEntity.cpp
    src/gameplay/entities/NeutralMob.cpp
    src/gameplay/entities/MobAi.cpp
    src/gameplay/entities/MobBrain.cpp
    src/gameplay/entities/ExperienceOrb.cpp
    src/gameplay/entities/ProjectileSystem.cpp
    src/world/BlockPlacement.cpp
    src/world/Chunk.cpp
    src/world/ChunkSection.cpp
    src/world/ChunkStreamer.cpp
    src/world/ChunkMesher.cpp
    src/world/WorldLighting.cpp
    src/world/WorldLightEngine.cpp
    src/world/SurfaceGenerator.cpp
    src/world/NetherGenerator.cpp
    src/world/EndGenerator.cpp
    src/world/StructureTemplate.cpp
    src/world/StructurePlacer.cpp
    src/world/StructureManager.cpp
    src/world/gen/Biome.cpp
    src/world/gen/BiomeSource.cpp
    src/world/gen/LayeredBiomeSource.cpp
    src/world/gen/MultiNoiseBiomeSource.cpp
    src/world/gen/TheEndBiomeSource.cpp
    src/world/gen/Carver.cpp
    src/world/gen/Features.cpp
    src/world/gen/StructureGenerator.cpp
    src/world/gen/JigsawExpansion.cpp
    src/world/gen/NoiseChunkGenerator.cpp
    src/world/gen/NoiseSampler.cpp
    src/world/gen/TreeGrower.cpp
    src/world/World.cpp
    src/world/DayNightCycle.cpp
    src/world/WorldClock.cpp
    # RW-0: the world-ray/block-hit test a flying projectile needs every tick.
    # Previously only linked into the GL executable (its sole caller was the
    # renderer's crosshair pick); ProjectileSystem needs it headless too, and
    # the header/impl pull in nothing but World/BlockShape, so moving it here
    # costs the GL build nothing (mc_rebedrock already links mc_rebedrock_runtime).
    src/world/VoxelRaycast.cpp
    # RN-9: the particle library and the client-side animateTick dispatcher moved
    # here from the GL executable list. Neither touches Vulkan (BlockAtlasLayout
    # is a pure constexpr layout contract), and putting them in the runtime lib is
    # what lets their tests link the library instead of re-listing sources — the
    # same trade VoxelRaycast.cpp took just above.
    src/render/ParticleSystem.cpp
    src/render/BlockAnimateTick.cpp
    # STRUCT-0: the vendored miniz decompressor moved here from the GL executable
    # so StructureTemplate's gzip reader has it headless (the GL build still gets
    # it by linking mc_rebedrock_runtime, and keeps its own vendor/miniz include).
    vendor/miniz/miniz.c
)
# The runtime spawns threads (ChunkStreamer worker, the TCP IO thread). This is
# looked up without REQUIRED because the MinGW cross build reconfigures this same
# file with a toolchain that supplies them itself; where the package exists the
# library passes it on to everything that links it.
find_package(Threads)
add_library(mc_rebedrock_runtime STATIC ${MC_REBEDROCK_RUNTIME_SOURCES})
target_include_directories(mc_rebedrock_runtime PUBLIC
    src "${MC_REBEDROCK_GENERATED_INCLUDE_DIR}" "${glm_SOURCE_DIR}")
# STRUCT-0: StructureTemplate.cpp (in the runtime lib) includes <miniz.h>.
target_include_directories(mc_rebedrock_runtime PRIVATE vendor/miniz)
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
    src/config/GameOptions.cpp
    src/core/Application.cpp
    src/gameplay/entities/SpeciesRenderData.cpp
    src/render/Frustum.cpp
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
    src/ui/TextWrap.cpp
    src/ui/TextField.cpp
    src/ui/ChatHistory.cpp
    src/ui/ItemTooltip.cpp
    src/ui/PageStack.cpp
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
    src/world/gen/MultiNoiseBiomeSource.cpp
    src/world/gen/TheEndBiomeSource.cpp
    src/world/gen/TreeGrower.cpp
    src/world/Chunk.cpp
    src/world/ChunkSection.cpp
    src/world/ChunkStreamer.cpp
    src/world/ChunkMesher.cpp
    src/world/SurfaceGenerator.cpp
    src/world/NetherGenerator.cpp
    src/world/EndGenerator.cpp
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
    # Shared GLSL, the way vanilla lays its own out (assets/minecraft/shaders/include).
    # The terrain lighting used to be hand-copied into grass_block.frag and
    # block_cutout.frag; the two copies drifting is what shipped a MoltenVK
    # pipeline-creation crash once already. Every one of these MUST be listed in
    # DEPENDS below — glslc will not tell CMake what it included, so an unlisted
    # header edits without rebuilding anything.
    set(MC_REBEDROCK_SHADER_INCLUDES
        "${MC_REBEDROCK_SHADER_SOURCE_DIR}/include/lightmap.glsl"
    )

    foreach(shader IN LISTS MC_REBEDROCK_SHADER_SOURCES)
        set(shader_source "${MC_REBEDROCK_SHADER_SOURCE_DIR}/${shader}")
        set(shader_output "${MC_REBEDROCK_SHADER_OUTPUT_DIR}/${shader}.spv")

        add_custom_command(
            OUTPUT "${shader_output}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${MC_REBEDROCK_SHADER_OUTPUT_DIR}"
            COMMAND "${GLSLC_EXECUTABLE}" -I "${MC_REBEDROCK_SHADER_SOURCE_DIR}"
                    "${shader_source}" -o "${shader_output}"
            DEPENDS "${shader_source}" ${MC_REBEDROCK_SHADER_INCLUDES}
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

    add_executable(mc_rebedrock_version_manifest_test
        tests/version_manifest_test.cpp
    )
    target_link_libraries(mc_rebedrock_version_manifest_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_version_manifest_test PRIVATE src)
    add_test(NAME version_manifest COMMAND mc_rebedrock_version_manifest_test)

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

    add_executable(mc_rebedrock_face_bakery_test
        tests/face_bakery_test.cpp
    )
    target_link_libraries(mc_rebedrock_face_bakery_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_face_bakery_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME face_bakery COMMAND mc_rebedrock_face_bakery_test)

    add_executable(mc_rebedrock_element_model_baker_test
        tests/element_model_baker_test.cpp
    )
    target_link_libraries(mc_rebedrock_element_model_baker_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_element_model_baker_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME element_model_baker COMMAND mc_rebedrock_element_model_baker_test)

    add_executable(mc_rebedrock_chunk_streamer_test
        tests/chunk_streamer_test.cpp
    )
    target_link_libraries(mc_rebedrock_chunk_streamer_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_chunk_streamer_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME chunk_streamer COMMAND mc_rebedrock_chunk_streamer_test)

    add_executable(mc_rebedrock_chunk_streaming_trace_test
        tests/chunk_streaming_trace_test.cpp
    )
    target_include_directories(mc_rebedrock_chunk_streaming_trace_test PRIVATE
        src
    )
    add_test(NAME chunk_streaming_trace COMMAND mc_rebedrock_chunk_streaming_trace_test)

    add_executable(mc_rebedrock_section_delivery_queue_test
        tests/section_delivery_queue_test.cpp
    )
    target_include_directories(mc_rebedrock_section_delivery_queue_test PRIVATE
        src
    )
    add_test(NAME section_delivery_queue COMMAND mc_rebedrock_section_delivery_queue_test)

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

    add_executable(mc_rebedrock_smoke_script_test
        tests/smoke_script_test.cpp
    )
    target_include_directories(mc_rebedrock_smoke_script_test PRIVATE
        src
    )
    add_test(NAME smoke_script COMMAND mc_rebedrock_smoke_script_test)

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

    add_executable(mc_rebedrock_command_source_test
        tests/command_source_test.cpp
    )
    target_link_libraries(mc_rebedrock_command_source_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_command_source_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME command_source COMMAND mc_rebedrock_command_source_test)

    add_executable(mc_rebedrock_input_system_test
        tests/input_system_test.cpp
    )
    target_link_libraries(mc_rebedrock_input_system_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_input_system_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME input_system COMMAND mc_rebedrock_input_system_test)

    add_executable(mc_rebedrock_dimension_test
        tests/dimension_test.cpp
    )
    target_link_libraries(mc_rebedrock_dimension_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_dimension_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME dimension COMMAND mc_rebedrock_dimension_test)

    add_executable(mc_rebedrock_command_selector_test
        tests/command_selector_test.cpp
    )
    target_link_libraries(mc_rebedrock_command_selector_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_command_selector_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME command_selector COMMAND mc_rebedrock_command_selector_test)

    add_executable(mc_rebedrock_command_help_test
        tests/command_help_test.cpp
    )
    target_link_libraries(mc_rebedrock_command_help_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_command_help_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME command_help COMMAND mc_rebedrock_command_help_test)

    add_executable(mc_rebedrock_chat_history_test
        tests/chat_history_test.cpp
        src/ui/ChatHistory.cpp
    )
    target_include_directories(mc_rebedrock_chat_history_test PRIVATE src)
    add_test(NAME chat_history COMMAND mc_rebedrock_chat_history_test)

    add_executable(mc_rebedrock_text_wrap_test
        tests/text_wrap_test.cpp
        src/ui/TextWrap.cpp
    )
    target_include_directories(mc_rebedrock_text_wrap_test PRIVATE src)
    add_test(NAME text_wrap COMMAND mc_rebedrock_text_wrap_test)

    # UI-1: the text input widget's editing semantics are pure values with an
    # injected width function, so the whole surface is asserted with no font and
    # no window. TextFont/BitmapFontMetrics come along only for decodeUtf8 and
    # appendUtf8 — the project's one UTF-8 codec, which the layer refuses to
    # hand-copy.
    add_executable(mc_rebedrock_text_field_test
        tests/text_field_test.cpp
        src/ui/TextField.cpp
        src/ui/TextFont.cpp
        src/ui/BitmapFontMetrics.cpp
    )
    target_include_directories(mc_rebedrock_text_field_test PRIVATE src)
    add_test(NAME text_field COMMAND mc_rebedrock_text_field_test)

    # I-2: the item presentation layer is pure value assembly (no Vulkan, no
    # font), so its whole surface is asserted headless.
    add_executable(mc_rebedrock_item_tooltip_test
        tests/item_tooltip_test.cpp
        # The layer resolves its text through ui::Language, which lives outside
        # the runtime library (it reaches TextFont's UTF-8 decoder) — same three
        # sources option_cycle_test lists for the same reason.
        src/ui/ItemTooltip.cpp
        src/ui/Language.cpp
        src/ui/TextFont.cpp
        src/ui/BitmapFontMetrics.cpp
    )
    target_link_libraries(mc_rebedrock_item_tooltip_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_item_tooltip_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME item_tooltip COMMAND mc_rebedrock_item_tooltip_test)

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

    add_executable(mc_rebedrock_entity_type_identity_test
        tests/entity_type_identity_test.cpp
    )
    target_link_libraries(mc_rebedrock_entity_type_identity_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_entity_type_identity_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME entity_type_identity COMMAND mc_rebedrock_entity_type_identity_test)

    add_executable(mc_rebedrock_entity_attributes_test
        tests/entity_attributes_test.cpp
    )
    target_link_libraries(mc_rebedrock_entity_attributes_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_entity_attributes_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME entity_attributes COMMAND mc_rebedrock_entity_attributes_test)

    add_executable(mc_rebedrock_entity_species_test
        tests/entity_species_test.cpp
    )
    target_link_libraries(mc_rebedrock_entity_species_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_entity_species_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME entity_species COMMAND mc_rebedrock_entity_species_test)

    add_executable(mc_rebedrock_mob_brain_test
        tests/mob_brain_test.cpp
    )
    target_link_libraries(mc_rebedrock_mob_brain_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_mob_brain_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME mob_brain COMMAND mc_rebedrock_mob_brain_test)

    add_executable(mc_rebedrock_entity_fire_test
        tests/entity_fire_test.cpp
    )
    target_link_libraries(mc_rebedrock_entity_fire_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_entity_fire_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME entity_fire COMMAND mc_rebedrock_entity_fire_test)

    add_executable(mc_rebedrock_monster_completion_test
        tests/monster_completion_test.cpp
    )
    target_link_libraries(mc_rebedrock_monster_completion_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_monster_completion_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME monster_completion COMMAND mc_rebedrock_monster_completion_test)

    add_executable(mc_rebedrock_status_effect_test
        tests/status_effect_test.cpp
    )
    target_link_libraries(mc_rebedrock_status_effect_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_status_effect_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME status_effect COMMAND mc_rebedrock_status_effect_test)

    add_executable(mc_rebedrock_aging_breeding_test
        tests/aging_breeding_test.cpp
    )
    target_link_libraries(mc_rebedrock_aging_breeding_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_aging_breeding_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME aging_breeding COMMAND mc_rebedrock_aging_breeding_test)

    add_executable(mc_rebedrock_sheep_content_test
        tests/sheep_content_test.cpp
    )
    target_link_libraries(mc_rebedrock_sheep_content_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_sheep_content_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME sheep_content COMMAND mc_rebedrock_sheep_content_test)

    add_executable(mc_rebedrock_cow_content_test
        tests/cow_content_test.cpp
    )
    target_link_libraries(mc_rebedrock_cow_content_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_cow_content_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME cow_content COMMAND mc_rebedrock_cow_content_test)

    add_executable(mc_rebedrock_chicken_content_test
        tests/chicken_content_test.cpp
    )
    target_link_libraries(mc_rebedrock_chicken_content_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_chicken_content_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME chicken_content COMMAND mc_rebedrock_chicken_content_test)

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

        # Informational: XP-1's per-tick orb pool cost (physics + magnet + merge
        # scan + pickup) at 10/50/200 orbs.
        add_executable(mc_rebedrock_experience_orb_benchmark
            tests/experience_orb_benchmark.cpp
        )
        target_link_libraries(mc_rebedrock_experience_orb_benchmark PRIVATE mc_rebedrock_runtime)
        target_include_directories(mc_rebedrock_experience_orb_benchmark PRIVATE
            src
            "${glm_SOURCE_DIR}"
        )

        # Informational: the W-5 AC wire wavefront vs the naive serial relaxation.
        add_executable(mc_rebedrock_redstone_wire_benchmark
            tests/redstone_wire_benchmark.cpp
        )
        target_link_libraries(mc_rebedrock_redstone_wire_benchmark PRIVATE mc_rebedrock_runtime)
        target_include_directories(mc_rebedrock_redstone_wire_benchmark PRIVATE
            src
            "${glm_SOURCE_DIR}"
        )

        # D-1's baking step: reads the demo JSON dataset through the data codec and
        # emits the constexpr floor (tests/data/DemoBakedData.inc). Its output is
        # committed, so ordinary builds compile the baked table with no codegen
        # dependency; run mc_rebedrock_bake_demo_data to regenerate it from source.
        add_executable(mc_rebedrock_bake_demo_data
            tools/bake/bake_demo_data.cpp
        )
        target_link_libraries(mc_rebedrock_bake_demo_data PRIVATE mc_rebedrock_runtime)
        target_include_directories(mc_rebedrock_bake_demo_data PRIVATE src tests)
        add_custom_target(mc_rebedrock_regenerate_demo_bake
            COMMAND mc_rebedrock_bake_demo_data
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/bake/demo-source"
                "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/DemoBakedData.inc"
            COMMENT "Regenerating the baked demo data floor from source JSON"
            VERBATIM
        )
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
        # I-3: the stack codec resolves a custom name through the session table.
        src/gameplay/CustomNames.cpp
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

    add_executable(mc_rebedrock_level_test
        tests/level_test.cpp
    )
    target_link_libraries(mc_rebedrock_level_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_level_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME level COMMAND mc_rebedrock_level_test)

    add_executable(mc_rebedrock_dimension_tick_test
        tests/dimension_tick_test.cpp
    )
    target_link_libraries(mc_rebedrock_dimension_tick_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_dimension_tick_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME dimension_tick COMMAND mc_rebedrock_dimension_tick_test)

    add_executable(mc_rebedrock_dimension_gen_test
        tests/dimension_gen_test.cpp
    )
    target_link_libraries(mc_rebedrock_dimension_gen_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_dimension_gen_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME dimension_gen COMMAND mc_rebedrock_dimension_gen_test)

    add_executable(mc_rebedrock_dimension_save_test
        tests/dimension_save_test.cpp
    )
    target_link_libraries(mc_rebedrock_dimension_save_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_dimension_save_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME dimension_save COMMAND mc_rebedrock_dimension_save_test)

    add_executable(mc_rebedrock_dimension_transfer_test
        tests/dimension_transfer_test.cpp
    )
    target_link_libraries(mc_rebedrock_dimension_transfer_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_dimension_transfer_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME dimension_transfer COMMAND mc_rebedrock_dimension_transfer_test)

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
    target_link_libraries(mc_rebedrock_player_render_state_test PRIVATE
        mc_rebedrock_runtime
        mc_rebedrock_animation
    )
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
    )
    target_link_libraries(mc_rebedrock_damage_pipeline_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_damage_pipeline_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME damage_pipeline COMMAND mc_rebedrock_damage_pipeline_test)

    add_executable(mc_rebedrock_armor_mitigation_test
        tests/armor_mitigation_test.cpp
    )
    target_link_libraries(mc_rebedrock_armor_mitigation_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_armor_mitigation_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME armor_mitigation COMMAND mc_rebedrock_armor_mitigation_test)

    add_executable(mc_rebedrock_resistance_absorption_test
        tests/resistance_absorption_test.cpp
    )
    target_link_libraries(mc_rebedrock_resistance_absorption_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_resistance_absorption_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME resistance_absorption COMMAND mc_rebedrock_resistance_absorption_test)

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

    add_executable(mc_rebedrock_block_entity_ticker_test
        tests/block_entity_ticker_test.cpp
    )
    target_link_libraries(mc_rebedrock_block_entity_ticker_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_block_entity_ticker_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME block_entity_ticker COMMAND mc_rebedrock_block_entity_ticker_test)

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

    add_executable(mc_rebedrock_pack_manager_test
        tests/pack_manager_test.cpp
    )
    target_link_libraries(mc_rebedrock_pack_manager_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_pack_manager_test PRIVATE src)
    add_test(NAME pack_manager COMMAND mc_rebedrock_pack_manager_test)

    add_executable(mc_rebedrock_data_pack_stack_test
        tests/data_pack_stack_test.cpp
    )
    target_link_libraries(mc_rebedrock_data_pack_stack_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_data_pack_stack_test PRIVATE src)
    add_test(NAME data_pack_stack COMMAND mc_rebedrock_data_pack_stack_test)

    add_executable(mc_rebedrock_function_manager_test
        tests/function_manager_test.cpp
    )
    target_link_libraries(mc_rebedrock_function_manager_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_function_manager_test PRIVATE src)
    add_test(NAME function_manager COMMAND mc_rebedrock_function_manager_test)

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

    add_executable(mc_rebedrock_tooltip_layout_test
        tests/tooltip_layout_test.cpp
        src/ui/HudLayout.cpp
    )
    target_include_directories(mc_rebedrock_tooltip_layout_test PRIVATE src)
    target_link_libraries(mc_rebedrock_tooltip_layout_test PRIVATE glm::glm)
    add_test(NAME tooltip_layout COMMAND mc_rebedrock_tooltip_layout_test)

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

    add_executable(mc_rebedrock_atlas_layer_fit_test
        tests/atlas_layer_fit_test.cpp
        src/assets/ImageData.cpp
        src/assets/StbImageImplementation.cpp
    )
    target_include_directories(mc_rebedrock_atlas_layer_fit_test PRIVATE src "${stb_SOURCE_DIR}")
    add_test(NAME atlas_layer_fit COMMAND mc_rebedrock_atlas_layer_fit_test)

    add_executable(mc_rebedrock_item_cube_render_test
        tests/item_cube_render_test.cpp
    )
    target_link_libraries(mc_rebedrock_item_cube_render_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_item_cube_render_test PRIVATE src)
    add_test(NAME item_cube_render COMMAND mc_rebedrock_item_cube_render_test)

    add_executable(mc_rebedrock_widget_labels_test
        tests/widget_labels_test.cpp
    )
    target_link_libraries(mc_rebedrock_widget_labels_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_widget_labels_test PRIVATE src)
    add_test(NAME widget_labels COMMAND mc_rebedrock_widget_labels_test)

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

    add_executable(mc_rebedrock_rng_golden_test
        tests/rng_golden_test.cpp
    )
    target_link_libraries(mc_rebedrock_rng_golden_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_rng_golden_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME rng_golden COMMAND mc_rebedrock_rng_golden_test)

    add_executable(mc_rebedrock_rng_scope_guard_test
        tests/rng_scope_guard_test.cpp
    )
    target_compile_definitions(mc_rebedrock_rng_scope_guard_test PRIVATE
        "MC_REBEDROCK_SRC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/src\""
    )
    add_test(NAME rng_scope_guard COMMAND mc_rebedrock_rng_scope_guard_test)

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

    add_executable(mc_rebedrock_face_occlusion_test
        tests/face_occlusion_test.cpp
    )
    target_link_libraries(mc_rebedrock_face_occlusion_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_face_occlusion_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME face_occlusion COMMAND mc_rebedrock_face_occlusion_test)

    add_executable(mc_rebedrock_shader_descriptor_bindings_test
        tests/shader_descriptor_bindings_test.cpp
    )
    target_include_directories(mc_rebedrock_shader_descriptor_bindings_test PRIVATE src)
    target_compile_definitions(mc_rebedrock_shader_descriptor_bindings_test PRIVATE
        "MC_REBEDROCK_SHADER_SRC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/src\""
        "MC_REBEDROCK_RENDERER_SRC=\"${CMAKE_CURRENT_SOURCE_DIR}/src/render/vulkan/VulkanRenderer.cpp\"")
    add_test(NAME shader_descriptor_bindings COMMAND mc_rebedrock_shader_descriptor_bindings_test)

    add_executable(mc_rebedrock_environment_attribute_test
        tests/environment_attribute_test.cpp
    )
    target_link_libraries(mc_rebedrock_environment_attribute_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_environment_attribute_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME environment_attribute COMMAND mc_rebedrock_environment_attribute_test)

    add_executable(mc_rebedrock_grass_overlay_depth_test
        tests/grass_overlay_depth_test.cpp
    )
    target_include_directories(mc_rebedrock_grass_overlay_depth_test PRIVATE src)
    target_compile_definitions(mc_rebedrock_grass_overlay_depth_test PRIVATE
        "MC_REBEDROCK_SHADER_SRC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/src\""
        "MC_REBEDROCK_RENDERER_SRC=\"${CMAKE_CURRENT_SOURCE_DIR}/src/render/vulkan/VulkanRenderer.cpp\"")
    add_test(NAME grass_overlay_depth COMMAND mc_rebedrock_grass_overlay_depth_test)

    # Joins the atlas bake's pre-tinted layer set against the mesher's per-face
    # tint kinds, so no surface can end up multiplied by a biome colour twice.
    # Needs the baker itself (it lives in the executable target, not the runtime
    # library, so name its translation unit here) and stb for PNG encoding.
    add_executable(mc_rebedrock_biome_tint_layers_test
        tests/biome_tint_layers_test.cpp
        src/render/vulkan/BlockAtlasBaker.cpp
        src/assets/ImageData.cpp
        src/assets/TextureAnimation.cpp
        src/assets/StbImageImplementation.cpp
    )
    target_link_libraries(mc_rebedrock_biome_tint_layers_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_biome_tint_layers_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
        "${stb_SOURCE_DIR}"
    )
    add_test(NAME biome_tint_layers COMMAND mc_rebedrock_biome_tint_layers_test)

    add_executable(mc_rebedrock_sky_light_test
        tests/sky_light_test.cpp
    )
    target_include_directories(mc_rebedrock_sky_light_test PRIVATE src)
    add_test(NAME sky_light COMMAND mc_rebedrock_sky_light_test)

    add_executable(mc_rebedrock_terrain_lightmap_test
        tests/terrain_lightmap_test.cpp
    )
    target_include_directories(mc_rebedrock_terrain_lightmap_test PRIVATE src)
    target_compile_definitions(mc_rebedrock_terrain_lightmap_test PRIVATE
        "MC_REBEDROCK_SHADER_SRC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/src\"")
    add_test(NAME terrain_lightmap COMMAND mc_rebedrock_terrain_lightmap_test)

    add_executable(mc_rebedrock_biome_climate_test
        tests/biome_climate_test.cpp
    )
    target_link_libraries(mc_rebedrock_biome_climate_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_biome_climate_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME biome_climate COMMAND mc_rebedrock_biome_climate_test)

    add_executable(mc_rebedrock_biome_colors_test
        tests/biome_colors_test.cpp
    )
    target_link_libraries(mc_rebedrock_biome_colors_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_biome_colors_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME biome_colors COMMAND mc_rebedrock_biome_colors_test)

    add_executable(mc_rebedrock_biome_identity_test
        tests/biome_identity_test.cpp
    )
    target_link_libraries(mc_rebedrock_biome_identity_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_biome_identity_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME biome_identity COMMAND mc_rebedrock_biome_identity_test)

    add_executable(mc_rebedrock_item_cube_uv_test
        tests/item_cube_uv_test.cpp
    )
    target_link_libraries(mc_rebedrock_item_cube_uv_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_item_cube_uv_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    target_compile_definitions(mc_rebedrock_item_cube_uv_test PRIVATE
        "MC_REBEDROCK_SHADER_SRC_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/resources/shaders/src\"")
    add_test(NAME item_cube_uv COMMAND mc_rebedrock_item_cube_uv_test)

    if(MC_REBEDROCK_BUILD_TOOLS)
        # Informational: us-per-step of the shape-derivation hot path
        # (collisionShape -> shapeOverlaps) the physics collision walk hits every
        # tick, the A/B baseline for AR-B1 Slice D (BlockShape unification).
        add_executable(mc_rebedrock_block_shape_benchmark
            tests/block_shape_benchmark.cpp
        )
        target_link_libraries(mc_rebedrock_block_shape_benchmark PRIVATE mc_rebedrock_runtime)
        target_include_directories(mc_rebedrock_block_shape_benchmark PRIVATE
            src
            "${glm_SOURCE_DIR}"
        )

        # Informational: us-per-probe of the two collision-probe loops AR-B4-0
        # touches (PlayerController::collidesAtHeight and
        # EntitySystem::boxIntersectsWorld), the A/B baseline for the
        # row-below tall-collision scan.
        add_executable(mc_rebedrock_collision_probe_benchmark
            tests/collision_probe_benchmark.cpp
        )
        target_link_libraries(mc_rebedrock_collision_probe_benchmark PRIVATE mc_rebedrock_runtime)
        target_include_directories(mc_rebedrock_collision_probe_benchmark PRIVATE
            src
            "${glm_SOURCE_DIR}"
        )
    endif()

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

    add_executable(mc_rebedrock_stair_door_gate_shape_test
        tests/stair_door_gate_shape_test.cpp
        src/world/World.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
        src/world/BlockPlacement.cpp
    )
    target_include_directories(mc_rebedrock_stair_door_gate_shape_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME stair_door_gate_shape COMMAND mc_rebedrock_stair_door_gate_shape_test)

    add_executable(mc_rebedrock_trapdoor_button_plate_wall_shape_test
        tests/trapdoor_button_plate_wall_shape_test.cpp
        src/world/World.cpp
        src/world/Chunk.cpp
        src/world/ChunkSection.cpp
        src/world/BlockPlacement.cpp
    )
    target_include_directories(mc_rebedrock_trapdoor_button_plate_wall_shape_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME trapdoor_button_plate_wall_shape
             COMMAND mc_rebedrock_trapdoor_button_plate_wall_shape_test)

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

    add_executable(mc_rebedrock_redstone_wire_ac_test
        tests/redstone_wire_ac_test.cpp
    )
    target_link_libraries(mc_rebedrock_redstone_wire_ac_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_redstone_wire_ac_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME redstone_wire_ac COMMAND mc_rebedrock_redstone_wire_ac_test)

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

    add_executable(mc_rebedrock_pressure_plate_fixture_test
        tests/redstone/pressure_plate_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_pressure_plate_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_pressure_plate_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME pressure_plate_fixture COMMAND mc_rebedrock_pressure_plate_fixture_test)

    add_executable(mc_rebedrock_trapdoor_fixture_test
        tests/redstone/trapdoor_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_trapdoor_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_trapdoor_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME trapdoor_fixture COMMAND mc_rebedrock_trapdoor_fixture_test)

    add_executable(mc_rebedrock_door_fixture_test
        tests/redstone/door_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_door_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_door_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME door_fixture COMMAND mc_rebedrock_door_fixture_test)

    add_executable(mc_rebedrock_fence_gate_fixture_test
        tests/redstone/fence_gate_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_fence_gate_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_fence_gate_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME fence_gate_fixture COMMAND mc_rebedrock_fence_gate_fixture_test)

    add_executable(mc_rebedrock_repeater_locked_fixture_test
        tests/redstone/repeater_locked_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_repeater_locked_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_repeater_locked_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME repeater_locked_fixture COMMAND mc_rebedrock_repeater_locked_fixture_test)

    add_executable(mc_rebedrock_diode_output_wake_fixture_test
        tests/redstone/diode_output_wake_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_diode_output_wake_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_diode_output_wake_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME diode_output_wake_fixture COMMAND mc_rebedrock_diode_output_wake_fixture_test)

    add_executable(mc_rebedrock_diode_self_start_fixture_test
        tests/redstone/diode_self_start_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_diode_self_start_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_diode_self_start_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME diode_self_start_fixture COMMAND mc_rebedrock_diode_self_start_fixture_test)

    add_executable(mc_rebedrock_fence_gate_in_wall_test
        tests/fence_gate_in_wall_test.cpp
    )
    target_link_libraries(mc_rebedrock_fence_gate_in_wall_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_fence_gate_in_wall_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME fence_gate_in_wall COMMAND mc_rebedrock_fence_gate_in_wall_test)

    add_executable(mc_rebedrock_redstone_item_placement_test
        tests/redstone_item_placement_test.cpp
    )
    target_link_libraries(mc_rebedrock_redstone_item_placement_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_redstone_item_placement_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME redstone_item_placement COMMAND mc_rebedrock_redstone_item_placement_test)

    add_executable(mc_rebedrock_block_set_type_test
        tests/block_set_type_test.cpp
    )
    target_link_libraries(mc_rebedrock_block_set_type_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_block_set_type_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME block_set_type COMMAND mc_rebedrock_block_set_type_test)

    add_executable(mc_rebedrock_comparator_container_test
        tests/comparator_container_test.cpp
    )
    target_link_libraries(mc_rebedrock_comparator_container_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_comparator_container_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME comparator_container COMMAND mc_rebedrock_comparator_container_test)

    add_executable(mc_rebedrock_redstone_island_partition_test
        tests/redstone_island_partition_test.cpp
    )
    target_link_libraries(mc_rebedrock_redstone_island_partition_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_redstone_island_partition_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME redstone_island_partition COMMAND mc_rebedrock_redstone_island_partition_test)

    add_executable(mc_rebedrock_lockstep_fixture_test
        tests/redstone/lockstep_fixture_test.cpp
    )
    target_link_libraries(mc_rebedrock_lockstep_fixture_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_lockstep_fixture_test PRIVATE
        src
        tests
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME lockstep_fixture COMMAND mc_rebedrock_lockstep_fixture_test)

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
        # isRainingAt asks the biome whether rain falls there at all (BM-2).
        src/world/gen/Biome.cpp
        src/world/gen/NoiseSampler.cpp
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

    add_executable(mc_rebedrock_collision_walk_test
        tests/collision_walk_test.cpp
    )
    target_link_libraries(mc_rebedrock_collision_walk_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_collision_walk_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME collision_walk COMMAND mc_rebedrock_collision_walk_test)

    add_executable(mc_rebedrock_player_pose_test
        tests/player_pose_test.cpp
    )
    target_link_libraries(mc_rebedrock_player_pose_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_player_pose_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME player_pose COMMAND mc_rebedrock_player_pose_test)

    add_executable(mc_rebedrock_screen_mode_test
        tests/screen_mode_test.cpp
    )
    target_include_directories(mc_rebedrock_screen_mode_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME screen_mode COMMAND mc_rebedrock_screen_mode_test)

    add_executable(mc_rebedrock_option_cycle_test
        tests/option_cycle_test.cpp
        # The value labels go through ui::formatTranslation, which lives here
        # rather than in the runtime library (and Language.cpp reaches
        # TextFont's UTF-8 decoder).
        src/ui/Language.cpp
        src/ui/TextFont.cpp
        src/ui/BitmapFontMetrics.cpp
    )
    target_link_libraries(mc_rebedrock_option_cycle_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_option_cycle_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME option_cycle COMMAND mc_rebedrock_option_cycle_test)

    add_executable(mc_rebedrock_menu_widget_test
        tests/menu_widget_test.cpp
    )
    target_link_libraries(mc_rebedrock_menu_widget_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_menu_widget_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME menu_widget COMMAND mc_rebedrock_menu_widget_test)

    add_executable(mc_rebedrock_key_binding_test
        tests/key_binding_test.cpp
    )
    target_link_libraries(mc_rebedrock_key_binding_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_key_binding_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME key_binding COMMAND mc_rebedrock_key_binding_test)

    add_executable(mc_rebedrock_toast_overlay_test
        tests/toast_overlay_test.cpp
    )
    target_link_libraries(mc_rebedrock_toast_overlay_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_toast_overlay_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME toast_overlay COMMAND mc_rebedrock_toast_overlay_test)

    add_executable(mc_rebedrock_menu_layout_test
        tests/menu_layout_test.cpp
        src/ui/MenuGeometry.cpp
        src/ui/HudLayout.cpp
    )
    target_include_directories(mc_rebedrock_menu_layout_test PRIVATE src)
    target_link_libraries(mc_rebedrock_menu_layout_test PRIVATE glm::glm)
    add_test(NAME menu_layout COMMAND mc_rebedrock_menu_layout_test)

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

    # AU-2: the device-free ambient/music scheduler (MusicScheduler +
    # MoodAccumulator). Links only the audio library, which carries the
    # scheduler's translation unit; no audio device is touched.
    add_executable(mc_rebedrock_ambient_music_scheduler_test
        tests/ambient_music_scheduler_test.cpp
    )
    target_link_libraries(mc_rebedrock_ambient_music_scheduler_test PRIVATE mc_rebedrock_audio)
    add_test(NAME ambient_music_scheduler COMMAND mc_rebedrock_ambient_music_scheduler_test)

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

    add_executable(mc_rebedrock_part_definition_test
        tests/part_definition_test.cpp)
    target_link_libraries(mc_rebedrock_part_definition_test PRIVATE mc_rebedrock_animation)
    add_test(NAME part_definition COMMAND mc_rebedrock_part_definition_test)

    add_executable(mc_rebedrock_json_test tests/json_test.cpp)
    target_link_libraries(mc_rebedrock_json_test PRIVATE mc_rebedrock_animation)
    add_test(NAME json COMMAND mc_rebedrock_json_test)

    add_executable(mc_rebedrock_molang_test tests/molang_test.cpp)
    target_link_libraries(mc_rebedrock_molang_test PRIVATE mc_rebedrock_animation)
    add_test(NAME molang COMMAND mc_rebedrock_molang_test)

    add_executable(mc_rebedrock_skeletal_animation_test tests/skeletal_animation_test.cpp)
    target_link_libraries(mc_rebedrock_skeletal_animation_test PRIVATE mc_rebedrock_animation)
    add_test(NAME skeletal_animation COMMAND mc_rebedrock_skeletal_animation_test)

    add_executable(mc_rebedrock_bone_mask_test tests/bone_mask_test.cpp)
    target_link_libraries(mc_rebedrock_bone_mask_test PRIVATE mc_rebedrock_animation)
    add_test(NAME bone_mask COMMAND mc_rebedrock_bone_mask_test)

    add_executable(mc_rebedrock_blend_mode_test tests/blend_mode_test.cpp)
    target_link_libraries(mc_rebedrock_blend_mode_test PRIVATE mc_rebedrock_animation)
    add_test(NAME blend_mode COMMAND mc_rebedrock_blend_mode_test)

    add_executable(mc_rebedrock_animation_controller_test tests/animation_controller_test.cpp)
    target_link_libraries(mc_rebedrock_animation_controller_test PRIVATE mc_rebedrock_animation)
    add_test(NAME animation_controller COMMAND mc_rebedrock_animation_controller_test)

    add_executable(mc_rebedrock_player_layered_animation_test
        tests/player_layered_animation_test.cpp)
    target_link_libraries(mc_rebedrock_player_layered_animation_test PRIVATE mc_rebedrock_animation)
    add_test(NAME player_layered_animation COMMAND mc_rebedrock_player_layered_animation_test)

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
    )
    target_link_libraries(mc_rebedrock_particle_system_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_particle_system_test PRIVATE
        src "${glm_SOURCE_DIR}")
    add_test(NAME particle_system COMMAND mc_rebedrock_particle_system_test)

    # RN-9d: the client-side animateTick dispatcher and its first consumer.
    add_executable(mc_rebedrock_block_animate_tick_test
        tests/block_animate_tick_test.cpp
    )
    target_link_libraries(mc_rebedrock_block_animate_tick_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_block_animate_tick_test PRIVATE
        src "${glm_SOURCE_DIR}")
    add_test(NAME block_animate_tick COMMAND mc_rebedrock_block_animate_tick_test)

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

        # DDC-3: the built-in enchantment datapack generator. Projects the
        # constexpr kEnchantmentTable into resources/data/rebedrock/enchantment/
        # *.json (the internal datapack). Run once by hand after editing the
        # table; the golden test re-derives and byte-compares, so the files can
        # never drift. Writes files, so it is never a ctest entry.
        add_executable(mc_rebedrock_enchantment_datapack_gen
            tests/enchantment_datapack_gen.cpp
        )
        target_link_libraries(mc_rebedrock_enchantment_datapack_gen PRIVATE mc_rebedrock_runtime)
        target_include_directories(mc_rebedrock_enchantment_datapack_gen PRIVATE src "${glm_SOURCE_DIR}")
    endif()

    add_executable(mc_rebedrock_game_rules_registry_test
        tests/game_rules_registry_test.cpp
        src/gameplay/GameRules.cpp
    )
    target_include_directories(mc_rebedrock_game_rules_registry_test PRIVATE src)
    add_test(NAME game_rules_registry COMMAND mc_rebedrock_game_rules_registry_test)

    # The behaviour behind the rules the registry table declares: each gate is
    # checked on and off, so a rule that reaches nothing fails here rather than
    # silently reporting success to the player. Needs the shared runtime for the
    # entity registry and the world the mob-drop half kills a cow in.
    add_executable(mc_rebedrock_game_rule_effects_test
        tests/game_rule_effects_test.cpp
    )
    target_link_libraries(mc_rebedrock_game_rule_effects_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_game_rule_effects_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME game_rule_effects COMMAND mc_rebedrock_game_rule_effects_test)

    # AR-A1: now exercises spawn-egg creative-catalog reachability too, which
    # needs the entity registry/species manifest — linking the shared runtime
    # rather than re-listing just the two Content sources.
    add_executable(mc_rebedrock_content_registry_test
        tests/content_registry_test.cpp
    )
    target_link_libraries(mc_rebedrock_content_registry_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_content_registry_test PRIVATE src)
    add_test(NAME content_registry COMMAND mc_rebedrock_content_registry_test)

    # AR-CI: the creative-catalog reachability guard (every non-Hidden block in
    # exactly one tab, no technical leakage, no regression on the old tabs).
    add_executable(mc_rebedrock_creative_catalog_test
        tests/creative_catalog_test.cpp
    )
    target_link_libraries(mc_rebedrock_creative_catalog_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_creative_catalog_test PRIVATE src)
    add_test(NAME creative_catalog COMMAND mc_rebedrock_creative_catalog_test)

    add_executable(mc_rebedrock_registry_test
        tests/registry_test.cpp
    )
    target_include_directories(mc_rebedrock_registry_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME registry COMMAND mc_rebedrock_registry_test)

    # D-1: the data codec, the two-layer DataStore, and the baked floor.
    add_executable(mc_rebedrock_data_codec_test
        tests/data_codec_test.cpp
    )
    target_link_libraries(mc_rebedrock_data_codec_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_data_codec_test PRIVATE src tests "${glm_SOURCE_DIR}")
    add_test(NAME data_codec COMMAND mc_rebedrock_data_codec_test)

    # D-3: recipes resolved from the baked floor + datapack overlay.
    add_executable(mc_rebedrock_recipe_data_test
        tests/recipe_data_test.cpp
    )
    target_link_libraries(mc_rebedrock_recipe_data_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_recipe_data_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME recipe_data COMMAND mc_rebedrock_recipe_data_test)

    # D-4: block loot resolved from the baked floor + datapack overlay.
    add_executable(mc_rebedrock_loot_data_test
        tests/loot_data_test.cpp
    )
    target_link_libraries(mc_rebedrock_loot_data_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_loot_data_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME loot_data COMMAND mc_rebedrock_loot_data_test)

    # STRUCT-0: the structure `.nbt` reader (schema-directed NBT cursor + gzip +
    # palette resolution). Links the runtime (which now carries miniz) and pulls
    # vendor/miniz so the test can gzip its own fixtures.
    add_executable(mc_rebedrock_structure_template_test
        tests/structure_template_test.cpp
    )
    target_link_libraries(mc_rebedrock_structure_template_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_structure_template_test
        PRIVATE src "${glm_SOURCE_DIR}" vendor/miniz)
    add_test(NAME structure_template COMMAND mc_rebedrock_structure_template_test)

    # STRUCT-1: the chest loot engine (pools/weighted entries/set_count) reduced
    # from vanilla JE chest JSON, and its deterministic evaluator.
    add_executable(mc_rebedrock_chest_loot_test
        tests/chest_loot_test.cpp
    )
    target_link_libraries(mc_rebedrock_chest_loot_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_chest_loot_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME chest_loot COMMAND mc_rebedrock_chest_loot_test)

    # STRUCT-2 (engine): random_spread placement math, the rotation transform, and
    # the template placer, ending in the STORAGE->PLACEMENT->LOOT slice.
    add_executable(mc_rebedrock_structure_placement_test
        tests/structure_placement_test.cpp
    )
    target_link_libraries(mc_rebedrock_structure_placement_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_structure_placement_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME structure_placement COMMAND mc_rebedrock_structure_placement_test)

    # STRUCT-2 (wiring, first half): the template registry + non-streamer flow.
    add_executable(mc_rebedrock_structure_manager_test
        tests/structure_manager_test.cpp
    )
    target_link_libraries(mc_rebedrock_structure_manager_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_structure_manager_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME structure_manager COMMAND mc_rebedrock_structure_manager_test)

    # STRUCT-3a: the jigsaw data layer — template pools + jigsaw blocks lifted from
    # a template.
    add_executable(mc_rebedrock_structure_pool_test
        tests/structure_pool_test.cpp
    )
    target_link_libraries(mc_rebedrock_structure_pool_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_structure_pool_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME structure_pool COMMAND mc_rebedrock_structure_pool_test)

    # STRUCT-3b: the jigsaw expansion algorithm (connect / size / collision).
    add_executable(mc_rebedrock_jigsaw_expansion_test
        tests/jigsaw_expansion_test.cpp
    )
    target_link_libraries(mc_rebedrock_jigsaw_expansion_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_jigsaw_expansion_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME jigsaw_expansion COMMAND mc_rebedrock_jigsaw_expansion_test)

    # STRUCT-2 (wiring): the generation step placeStructures (no-op when empty,
    # stamps + biome-gates + placement-selects when loaded).
    add_executable(mc_rebedrock_structure_generation_test
        tests/structure_generation_test.cpp
    )
    target_link_libraries(mc_rebedrock_structure_generation_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_structure_generation_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME structure_generation COMMAND mc_rebedrock_structure_generation_test)

    # PACK: a combined pack (assets/ + data/) feeds both stacks — one user-supplied
    # pack yields textures + structures/loot, with the dedicated-server base isolated.
    add_executable(mc_rebedrock_pack_combined_test
        tests/pack_combined_test.cpp
    )
    target_link_libraries(mc_rebedrock_pack_combined_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_pack_combined_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME pack_combined COMMAND mc_rebedrock_pack_combined_test)

    # DDC-1: JE-compatible enchantment content JSON schema + loader.
    add_executable(mc_rebedrock_enchantment_content_test
        tests/enchantment_content_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchantment_content_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchantment_content_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME enchantment_content COMMAND mc_rebedrock_enchantment_content_test)

    # DDC-2: effect-component engine — load-time compiler + run-time DOD executor.
    add_executable(mc_rebedrock_effect_engine_test
        tests/effect_engine_test.cpp
    )
    target_link_libraries(mc_rebedrock_effect_engine_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_effect_engine_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME effect_engine COMMAND mc_rebedrock_effect_engine_test)

    # EQ-4: armor enchantment effects — Protection family EPF + Thorns, driven
    # through the DDC-2 effect engine and folded into the Damage pipeline.
    add_executable(mc_rebedrock_armor_enchantment_test
        tests/armor_enchantment_test.cpp
    )
    target_link_libraries(mc_rebedrock_armor_enchantment_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_armor_enchantment_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME armor_enchantment COMMAND mc_rebedrock_armor_enchantment_test)

    # ENCH-1b: mining-tool enchantment effects — Efficiency / Unbreaking /
    # Fortune / Silk Touch, driven through the DDC-2 effect engine and consumed by
    # the mining speed / durability spend / drop roll call sites.
    add_executable(mc_rebedrock_enchantment_mining_test
        tests/enchantment_mining_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchantment_mining_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchantment_mining_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME enchantment_mining COMMAND mc_rebedrock_enchantment_mining_test)

    # DDC-3: legacy enchantment content migration — golden compare (constexpr
    # kEnchantmentTable <-> JE-schema datapack JSON, zero drift) + the shipped
    # internal datapack (resources/data/rebedrock/enchantment/*.json) loaded
    # through the real StandardPackResourceProvider path.
    add_executable(mc_rebedrock_enchantment_migration_test
        tests/enchantment_migration_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchantment_migration_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchantment_migration_test PRIVATE src "${glm_SOURCE_DIR}")
    target_compile_definitions(mc_rebedrock_enchantment_migration_test PRIVATE
        MC_REBEDROCK_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
    add_test(NAME enchantment_migration COMMAND mc_rebedrock_enchantment_migration_test)

    # D-5: Java-Edition data-side mapping — vanilla-name authority + JE ingestion.
    add_executable(mc_rebedrock_je_mapping_test
        tests/je_mapping_test.cpp
    )
    target_link_libraries(mc_rebedrock_je_mapping_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_je_mapping_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME je_mapping COMMAND mc_rebedrock_je_mapping_test)

    # JC1: vanilla <-> rebedrock mapping framework (identity/state/deviation
    # override registry), the JE-save-compat axis's greenfield framework node.
    add_executable(mc_rebedrock_vanilla_mapping_test
        tests/vanilla_mapping_test.cpp
    )
    target_link_libraries(mc_rebedrock_vanilla_mapping_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_vanilla_mapping_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME vanilla_mapping COMMAND mc_rebedrock_vanilla_mapping_test)

    # WG-0: nether/end base-block and biome identity registration.
    add_executable(mc_rebedrock_nether_end_identity_test
        tests/nether_end_identity_test.cpp
    )
    target_link_libraries(mc_rebedrock_nether_end_identity_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_nether_end_identity_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME nether_end_identity COMMAND mc_rebedrock_nether_end_identity_test)

    # WG-1: NoiseChunkGenerator generalisation — per-dimension NoiseGeneratorSettings.
    add_executable(mc_rebedrock_noise_generator_settings_test
        tests/noise_generator_settings_test.cpp
    )
    target_link_libraries(mc_rebedrock_noise_generator_settings_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_noise_generator_settings_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME noise_generator_settings COMMAND mc_rebedrock_noise_generator_settings_test)

    # WG-2: nether terrain (settings + MultiNoiseBiomeSource + surface/features).
    add_executable(mc_rebedrock_nether_terrain_test
        tests/nether_terrain_test.cpp
    )
    target_link_libraries(mc_rebedrock_nether_terrain_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_nether_terrain_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME nether_terrain COMMAND mc_rebedrock_nether_terrain_test)

    # WG-3: end terrain (settings + TheEndBiomeSource + island generator).
    add_executable(mc_rebedrock_end_terrain_test
        tests/end_terrain_test.cpp
    )
    target_link_libraries(mc_rebedrock_end_terrain_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_end_terrain_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME end_terrain COMMAND mc_rebedrock_end_terrain_test)

    # WG-4: DimensionGenerator seam wiring + per-dimension world height.
    add_executable(mc_rebedrock_dimension_seam_wiring_test
        tests/dimension_seam_wiring_test.cpp
    )
    target_link_libraries(mc_rebedrock_dimension_seam_wiring_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_dimension_seam_wiring_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME dimension_seam_wiring COMMAND mc_rebedrock_dimension_seam_wiring_test)

    add_executable(mc_rebedrock_block_registry_lifecycle_test
        tests/block_registry_lifecycle_test.cpp
    )
    target_include_directories(mc_rebedrock_block_registry_lifecycle_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME block_registry_lifecycle COMMAND mc_rebedrock_block_registry_lifecycle_test)

    add_executable(mc_rebedrock_block_entity_type_registry_test
        tests/block_entity_type_registry_test.cpp
    )
    target_include_directories(mc_rebedrock_block_entity_type_registry_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME block_entity_type_registry COMMAND mc_rebedrock_block_entity_type_registry_test)

    add_executable(mc_rebedrock_block_entity_store_test
        tests/block_entity_store_test.cpp
    )
    target_include_directories(mc_rebedrock_block_entity_store_test PRIVATE src "${glm_SOURCE_DIR}")
    add_test(NAME block_entity_store COMMAND mc_rebedrock_block_entity_store_test)

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

    add_executable(mc_rebedrock_item_registry_test
        tests/item_registry_test.cpp
    )
    target_link_libraries(mc_rebedrock_item_registry_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_item_registry_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME item_registry COMMAND mc_rebedrock_item_registry_test)

    add_executable(mc_rebedrock_player_experience_test
        tests/player_experience_test.cpp
    )
    target_link_libraries(mc_rebedrock_player_experience_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_player_experience_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME player_experience COMMAND mc_rebedrock_player_experience_test)

    add_executable(mc_rebedrock_experience_orb_test
        tests/experience_orb_test.cpp
    )
    target_link_libraries(mc_rebedrock_experience_orb_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_experience_orb_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME experience_orb COMMAND mc_rebedrock_experience_orb_test)

    add_executable(mc_rebedrock_projectile_system_test
        tests/projectile_system_test.cpp
    )
    target_link_libraries(mc_rebedrock_projectile_system_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_projectile_system_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME projectile_system COMMAND mc_rebedrock_projectile_system_test)

    add_executable(mc_rebedrock_bow_arrow_test
        tests/bow_arrow_test.cpp
    )
    target_link_libraries(mc_rebedrock_bow_arrow_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_bow_arrow_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME bow_arrow COMMAND mc_rebedrock_bow_arrow_test)

    # RW-4: ranged (bow) enchantment effects — Power / Punch / Flame / Infinity,
    # driven through the DDC-2 effect engine and consumed by the bow release site
    # (Power damage / Infinity consume) and the projectile hit (Punch knockback /
    # Flame ignite). Backfills the ENCH.gate remote half (bow shard).
    add_executable(mc_rebedrock_ranged_enchantment_test
        tests/ranged_enchantment_test.cpp
    )
    target_link_libraries(mc_rebedrock_ranged_enchantment_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_ranged_enchantment_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME ranged_enchantment COMMAND mc_rebedrock_ranged_enchantment_test)

    add_executable(mc_rebedrock_experience_sources_test
        tests/experience_sources_test.cpp
    )
    target_link_libraries(mc_rebedrock_experience_sources_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_experience_sources_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME experience_sources COMMAND mc_rebedrock_experience_sources_test)

    add_executable(mc_rebedrock_experience_sinks_test
        tests/experience_sinks_test.cpp
    )
    target_link_libraries(mc_rebedrock_experience_sinks_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_experience_sinks_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME experience_sinks COMMAND mc_rebedrock_experience_sinks_test)

    add_executable(mc_rebedrock_enchantment_registry_test
        tests/enchantment_registry_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchantment_registry_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchantment_registry_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME enchantment_registry COMMAND mc_rebedrock_enchantment_registry_test)

    add_executable(mc_rebedrock_enchantment_runtime_registry_test
        tests/enchantment_runtime_registry_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchantment_runtime_registry_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchantment_runtime_registry_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME enchantment_runtime_registry COMMAND mc_rebedrock_enchantment_runtime_registry_test)

    add_executable(mc_rebedrock_enchantment_offers_test
        tests/enchantment_offers_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchantment_offers_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchantment_offers_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME enchantment_offers COMMAND mc_rebedrock_enchantment_offers_test)

    add_executable(mc_rebedrock_enchantment_storage_test
        tests/enchantment_storage_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchantment_storage_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchantment_storage_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME enchantment_storage COMMAND mc_rebedrock_enchantment_storage_test)

    add_executable(mc_rebedrock_equipment_slots_test
        tests/equipment_slots_test.cpp
    )
    target_link_libraries(mc_rebedrock_equipment_slots_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_equipment_slots_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME equipment_slots COMMAND mc_rebedrock_equipment_slots_test)

    add_executable(mc_rebedrock_armor_content_test
        tests/armor_content_test.cpp
    )
    target_link_libraries(mc_rebedrock_armor_content_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_armor_content_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME armor_content COMMAND mc_rebedrock_armor_content_test)

    add_executable(mc_rebedrock_equipment_wear_test
        tests/equipment_wear_test.cpp
    )
    target_link_libraries(mc_rebedrock_equipment_wear_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_equipment_wear_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME equipment_wear COMMAND mc_rebedrock_equipment_wear_test)

    add_executable(mc_rebedrock_enchantment_combat_test
        tests/enchantment_combat_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchantment_combat_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchantment_combat_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME enchantment_combat COMMAND mc_rebedrock_enchantment_combat_test)

    add_executable(mc_rebedrock_enchanting_table_test
        tests/enchanting_table_test.cpp
    )
    target_link_libraries(mc_rebedrock_enchanting_table_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_enchanting_table_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME enchanting_table COMMAND mc_rebedrock_enchanting_table_test)

    add_executable(mc_rebedrock_anvil_test
        tests/anvil_test.cpp
    )
    target_link_libraries(mc_rebedrock_anvil_test PRIVATE mc_rebedrock_runtime)
    target_include_directories(mc_rebedrock_anvil_test PRIVATE
        src
        "${glm_SOURCE_DIR}"
    )
    add_test(NAME anvil COMMAND mc_rebedrock_anvil_test)

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
