# Compile the real portable renderer core for the explicitly selected Android development profile.
# The source inventories and dependency pins are shared with src/CMakeLists.txt.
include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/HoroCoreSourceSets.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/HoroRendererCoreDependencies.cmake")

function(horo_add_android_renderer_core)
    horo_populate_renderer_json()
    horo_populate_renderer_vulkan_headers()
    set(source_root "${PROJECT_SOURCE_DIR}/src")

    # The input-only qualification already owns Utf8.cpp; extend the same Foundation target.
    foreach(source IN LISTS HORO_FOUNDATION_SOURCES)
        if(NOT source STREQUAL "foundation/text/Utf8.cpp")
            target_sources(HoroFoundation PRIVATE "${source_root}/${source}")
        endif()
    endforeach()
    add_library(HoroEngine::Foundation ALIAS HoroFoundation)
    target_compile_definitions(HoroFoundation PUBLIC HORO_ENABLE_TELEMETRY=0)
    target_include_directories(HoroFoundation PRIVATE
        "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")

    add_library(HoroFoundationJsonInternal INTERFACE)
    target_include_directories(HoroFoundationJsonInternal INTERFACE "${source_root}/foundation/json")
    target_link_libraries(HoroFoundationJsonInternal INTERFACE nlohmann_json::nlohmann_json)

    add_library(HoroVfxSimulationData STATIC "${source_root}/runtime/vfx/simulation/ParticleSimulationData.cpp")
    add_library(HoroEngine::VfxSimulationData ALIAS HoroVfxSimulationData)
    target_link_libraries(HoroVfxSimulationData PUBLIC HoroEngine::Foundation)

    add_library(HoroRenderApi STATIC)
    add_library(HoroEngine::RenderApi ALIAS HoroRenderApi)
    foreach(source IN LISTS HORO_RENDER_API_SOURCES)
        target_sources(HoroRenderApi PRIVATE "${source_root}/${source}")
    endforeach()
    target_link_libraries(HoroRenderApi PUBLIC HoroEngine::Foundation HoroEngine::VfxSimulationData
        PRIVATE nlohmann_json::nlohmann_json)
    target_include_directories(HoroRenderApi PRIVATE
        "$<TARGET_PROPERTY:HoroFoundationJsonInternal,INTERFACE_INCLUDE_DIRECTORIES>")

    add_library(HoroRenderBackendRegistry STATIC)
    add_library(HoroEngine::RenderBackendRegistry ALIAS HoroRenderBackendRegistry)
    foreach(source IN LISTS HORO_RENDER_REGISTRY_SOURCES)
        target_sources(HoroRenderBackendRegistry PRIVATE "${source_root}/${source}")
    endforeach()
    target_link_libraries(HoroRenderBackendRegistry PUBLIC HoroEngine::RenderApi)

    # Only this existing owner's lifecycle contract is used by the qualification host.
    add_library(HoroPlatform STATIC
        "${source_root}/platform/PlatformErrors.cpp"
        "${source_root}/platform/android/AndroidLifecycle.cpp"
        "${source_root}/platform/android/AndroidLifecycleAdapter.cpp")
    add_library(HoroEngine::Platform ALIAS HoroPlatform)
    target_link_libraries(HoroPlatform PUBLIC HoroEngine::Foundation)

    foreach(target IN ITEMS HoroVfxSimulationData HoroRenderApi HoroRenderBackendRegistry HoroPlatform)
        target_compile_features(${target} PUBLIC cxx_std_20)
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    endforeach()
endfunction()
