cmake_minimum_required(VERSION 3.25)

set(HORO_ENGINE_VERSION "0.1.0")

project(HoroEngine VERSION ${HORO_ENGINE_VERSION} LANGUAGES CXX C)

execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE HORO_GIT_REVISION_RESULT
    OUTPUT_VARIABLE HORO_SOURCE_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT HORO_GIT_REVISION_RESULT EQUAL 0 OR HORO_SOURCE_REVISION STREQUAL "")
    set(HORO_SOURCE_REVISION "unknown")
endif()

set(HORO_GAMEPLAY_SDK_FINGERPRINT
    "horo-${HORO_ENGINE_VERSION}-gameplay-sdk1-${CMAKE_CXX_COMPILER_ID}-${CMAKE_CXX_COMPILER_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)
set(HORO_EXTENSION_SDK_VERSION "${HORO_ENGINE_VERSION}")
set(HORO_EXTENSION_ABI_MAJOR_VERSION 1)
set(HORO_EXTENSION_ABI_PUBLISHED_MINOR 3)
set(HORO_EXTENSION_ABI_MIN_HOST_MINOR 0)
set(HORO_EXTENSION_ABI_MAX_HOST_MAJOR_EXCLUSIVE 2)

# New project layout is being introduced incrementally. Legacy source is parked
# under deprecated/ as migration reference and is intentionally not part of target
# discovery.

include(GNUInstallDirs)
include(CTest)
include(CMakePackageConfigHelpers)

if(MSVC)
    add_compile_options(/FS)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(HORO_BUILD_EDITOR_GUI "Build the graphical HoroEditor bootstrap" ON)
option(HORO_BUILD_RENDER_OPENGL "Build the OpenGL renderer backend" ${HORO_BUILD_EDITOR_GUI})
set(_HORO_BUILD_RENDER_METAL_DEFAULT OFF)
if(APPLE AND HORO_BUILD_EDITOR_GUI)
    set(_HORO_BUILD_RENDER_METAL_DEFAULT ON)
endif()
option(HORO_BUILD_RENDER_METAL "Build the Metal renderer backend" ${_HORO_BUILD_RENDER_METAL_DEFAULT})
option(HORO_BUILD_RENDER_VULKAN "Build the Vulkan renderer backend" OFF)
option(HORO_ENABLE_GPU_SMOKE_TESTS "Register tests that require a display server and hardware graphics context" OFF)
option(HORO_ENABLE_IMGUI_UI_TESTS "Build headless Dear ImGui UI automation tests" OFF)
option(HORO_ENABLE_TELEMETRY "Compile Horo telemetry producer instrumentation" ON)
option(HORO_ENABLE_OPENTELEMETRY "Build the optional OpenTelemetry OTLP export sink" OFF)
option(HORO_BUILD_EXAMPLES "Build SDK and extension examples" ${BUILD_TESTING})
option(HORO_BUILD_PHYSICS_NATIVE "Build the private CanonicalV1 Physics solver" ON)
option(HORO_BUILD_NAVIGATION_RECAST_DETOUR "Build the default grounded navigation query provider" ON)
option(HORO_BUILD_AUDIO_SDL3 "Build the SDL3 portability/reference audio backend" ${HORO_BUILD_EDITOR_GUI})
option(HORO_BUILD_NETWORK_GNS "Build the optional production GameNetworkingSockets transport" OFF)

if(HORO_ENABLE_IMGUI_UI_TESTS AND NOT BUILD_TESTING)
    message(FATAL_ERROR "HORO_ENABLE_IMGUI_UI_TESTS requires BUILD_TESTING=ON")
endif()

if(HORO_ENABLE_IMGUI_UI_TESTS AND NOT HORO_BUILD_EDITOR_GUI)
    message(FATAL_ERROR "HORO_ENABLE_IMGUI_UI_TESTS requires HORO_BUILD_EDITOR_GUI=ON")
endif()

if(HORO_BUILD_RENDER_METAL AND NOT APPLE)
    message(FATAL_ERROR "HORO_BUILD_RENDER_METAL is supported only on Apple platforms")
endif()

if(HORO_BUILD_RENDER_VULKAN AND NOT (WIN32 OR (UNIX AND NOT APPLE)))
    message(FATAL_ERROR "HORO_BUILD_RENDER_VULKAN is supported only on native Windows and Linux platforms")
endif()

if(HORO_BUILD_EDITOR_GUI AND NOT HORO_BUILD_RENDER_OPENGL AND NOT HORO_BUILD_RENDER_METAL)
    message(FATAL_ERROR "The editor GUI composition requires at least one interactive renderer")
endif()

include(Dependencies)
include(GenerateBuildInfo)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include(HoroGameplayModule)
include(HoroExtensionSdk)
include(GenerateProjectMigrations)
include(GenerateProjectCompatibility)
include(HoroTargetBoundaries)
include(HoroDependencyDirection)

horo_configure_extension_sdk()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/CMakeLists.txt")
    add_subdirectory(src)
endif()

set(HORO_GAMEPLAY_SDK_PACKAGE_DIR "${CMAKE_BINARY_DIR}/sdk/gameplay" CACHE INTERNAL
    "Build-tree CMake package used by project gameplay modules")
file(MAKE_DIRECTORY "${HORO_GAMEPLAY_SDK_PACKAGE_DIR}")
set_target_properties(HoroFoundation PROPERTIES EXPORT_NAME Foundation POSITION_INDEPENDENT_CODE ON)
set_target_properties(HoroNetworkApi PROPERTIES EXPORT_NAME NetworkApi POSITION_INDEPENDENT_CODE ON)
set_target_properties(HoroGameplayApi PROPERTIES EXPORT_NAME GameplayApi POSITION_INDEPENDENT_CODE ON)
export(TARGETS HoroFoundation HoroNetworkApi HoroGameplayApi
       NAMESPACE HoroEngine::
       FILE "${HORO_GAMEPLAY_SDK_PACKAGE_DIR}/HoroEngineGameplayTargets.cmake")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroEngineGameplayConfig.cmake.in"
    "${HORO_GAMEPLAY_SDK_PACKAGE_DIR}/HoroEngineGameplayConfig.cmake"
    INSTALL_DESTINATION "."
    NO_SET_AND_CHECK_MACRO
    NO_CHECK_REQUIRED_COMPONENTS_MACRO)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/HoroGameplayModule.cmake"
               "${HORO_GAMEPLAY_SDK_PACKAGE_DIR}/HoroGameplayModule.cmake" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake/WriteGameplayModuleManifest.cmake"
               "${HORO_GAMEPLAY_SDK_PACKAGE_DIR}/WriteGameplayModuleManifest.cmake" COPYONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_gameplay_behavior_bundle.py"
               "${HORO_GAMEPLAY_SDK_PACKAGE_DIR}/generate_gameplay_behavior_bundle.py" COPYONLY)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/apps/CMakeLists.txt")
    add_subdirectory(apps)
endif()

# Validate only production targets. Tests and examples consume production
# contracts but are not layers in the shipped module graph.
include(HoroDependencyPolicy)
horo_collect_directory_targets(HORO_DEPENDENCY_POLICY_TARGETS
    "${CMAKE_CURRENT_SOURCE_DIR}/apps")
get_property(HORO_DEPENDENCY_POLICY_LIBRARY_TARGETS GLOBAL PROPERTY HORO_HEADER_BOUNDARY_TARGETS)
list(APPEND HORO_DEPENDENCY_POLICY_TARGETS ${HORO_DEPENDENCY_POLICY_LIBRARY_TARGETS})
horo_verify_dependency_direction(TARGETS ${HORO_DEPENDENCY_POLICY_TARGETS})

if(HORO_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

if(BUILD_TESTING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
    add_subdirectory(tests)
endif()
