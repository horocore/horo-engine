# Private Android presentation owners, also compiled with injected protocol tests on hosts.
include_guard(GLOBAL)

function(horo_add_android_presentation_targets)
    set(source_root "${PROJECT_SOURCE_DIR}/src")
    set(window_headers "${CMAKE_CURRENT_BINARY_DIR}/internal/android-window")
    file(MAKE_DIRECTORY "${window_headers}/Horo/Platform/Internal")
    foreach(header IN ITEMS AndroidWindowHost.h AndroidLifecycleAdapter.h)
        configure_file("${source_root}/platform/android/${header}"
            "${window_headers}/Horo/Platform/Internal/${header}" COPYONLY)
    endforeach()
    add_library(HoroPlatformAndroidWindowInternal INTERFACE)
    target_include_directories(HoroPlatformAndroidWindowInternal INTERFACE "${window_headers}")
    target_link_libraries(HoroPlatformAndroidWindowInternal INTERFACE HoroEngine::Platform)

    add_library(HoroPlatformAndroidWindow STATIC
        "${source_root}/platform/android/AndroidWindowHost.cpp"
        "${source_root}/platform/android/AndroidWindowNative.cpp")
    add_library(HoroEngine::PlatformAndroidWindow ALIAS HoroPlatformAndroidWindow)
    target_compile_features(HoroPlatformAndroidWindow PUBLIC cxx_std_20)
    target_link_libraries(HoroPlatformAndroidWindow PUBLIC HoroPlatformAndroidWindowInternal)
    if(ANDROID)
        target_link_libraries(HoroPlatformAndroidWindow PRIVATE android)
    endif()

    if(NOT HORO_BUILD_RENDER_VULKAN_ANDROID)
        return()
    endif()
    if(NOT TARGET Vulkan::Headers)
        message(FATAL_ERROR "Android Vulkan requires the pinned Vulkan header dependency")
    endif()

    set(renderer_root "${source_root}/runtime/renderer/modules/vulkan_android")
    set(host_headers "${CMAKE_CURRENT_BINARY_DIR}/internal/vulkan-android-host")
    file(MAKE_DIRECTORY "${host_headers}/Horo/Runtime/Render/Internal")
    configure_file("${renderer_root}/VulkanAndroidBackendModule.h"
        "${host_headers}/Horo/Runtime/Render/Internal/VulkanAndroidBackendModule.h" COPYONLY)
    add_library(HoroRenderVulkanAndroidHostInterface INTERFACE)
    target_include_directories(HoroRenderVulkanAndroidHostInterface INTERFACE "${host_headers}")
    target_link_libraries(HoroRenderVulkanAndroidHostInterface INTERFACE
        HoroEngine::RenderBackendRegistry HoroPlatformAndroidWindowInternal)

    add_library(HoroRenderVulkanAndroid STATIC
        "${renderer_root}/VulkanAndroidBackend.cpp"
        "${renderer_root}/VulkanAndroidRuntime.cpp"
        "${renderer_root}/VulkanAndroidDispatch.cpp"
        "${renderer_root}/VulkanAndroidSurface.cpp"
        "${renderer_root}/VulkanAndroidFrame.cpp"
        "${renderer_root}/VulkanAndroidPresentation.cpp"
        "${renderer_root}/VulkanAndroidErrors.cpp")
    add_library(HoroEngine::RenderVulkanAndroid ALIAS HoroRenderVulkanAndroid)
    target_compile_features(HoroRenderVulkanAndroid PUBLIC cxx_std_20)
    target_link_libraries(HoroRenderVulkanAndroid PUBLIC HoroRenderVulkanAndroidHostInterface
        PRIVATE HoroPlatformAndroidWindow Vulkan::Headers ${CMAKE_DL_LIBS})

    if(BUILD_TESTING)
        set(test_headers "${CMAKE_CURRENT_BINARY_DIR}/internal/vulkan-android-testing")
        file(MAKE_DIRECTORY "${test_headers}")
        foreach(header IN ITEMS VulkanAndroidRuntime.h VulkanAndroidErrors.h)
            configure_file("${renderer_root}/${header}" "${test_headers}/${header}" COPYONLY)
        endforeach()
        add_library(HoroRenderVulkanAndroidTestingInternal INTERFACE)
        target_include_directories(HoroRenderVulkanAndroidTestingInternal INTERFACE "${test_headers}")
        target_link_libraries(HoroRenderVulkanAndroidTestingInternal INTERFACE
            HoroEngine::RenderVulkanAndroid Vulkan::Headers)
    endif()
endfunction()
