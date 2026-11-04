# Included from apps/CMakeLists.txt; source and binary directories remain scoped to apps/.
# The graphical executable is an optional composition root. Foundation and
# headless hosts must remain buildable when the GUI adapter is intentionally off.
if(HORO_BUILD_EDITOR_GUI)
    set(HORO_EDITOR_APP_ID "dev.horo.editor")
    set(HORO_EDITOR_ASSET_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/horo-engine/assets")

    add_executable(HoroEditor
        HoroEditor/main.cpp
        HoroEditor/app/HoroEditorApp.cpp
        HoroEditor/app/EditorWindowChrome.cpp
        HoroEditor/app/EditorNavigationBakeHost.cpp
        HoroEditor/app/EditorWorkerShutdown.cpp
        HoroEditor/app/EditorAudioPreviewHost.cpp
        HoroEditor/app/EditorAudioPreviewDecode.cpp
        HoroEditor/app/EditorAudioPreviewPreparation.cpp
        HoroEditor/app/RenderGraphInspectionPane.cpp
        HoroEditor/app/EditorUserStateMigration.cpp
        HoroEditor/app/ConfiguredEditorUpdateBackend.cpp
        HoroEditor/app/ConfiguredEditorUpdateManifestSource.cpp
    )

    if(APPLE)
        target_sources(HoroEditor PRIVATE HoroEditor/app/EditorWindowChromeMac.mm)
        set_source_files_properties(HoroEditor/app/EditorWindowChromeMac.mm PROPERTIES
            LANGUAGE CXX COMPILE_OPTIONS "-x;objective-c++")
        target_link_libraries(HoroEditor PRIVATE "-framework AppKit")
    elseif(WIN32)
        target_link_libraries(HoroEditor PRIVATE dwmapi)
    endif()

    target_link_libraries(HoroEditor PRIVATE HoroEngine::PlatformProjectReadFiles HoroShaderHotReloadHostInternal)
    target_link_libraries(HoroEditor PRIVATE HoroXRSimulatorPlayHost)
    target_link_libraries(HoroEditor PRIVATE HoroXRSimulatorProjectArtifacts HoroEngine::PlatformProjectIo)
    include(HoroEditorGuiLiveReloadHost)
    if(HORO_EDITOR_LIVE_UI)
        include(HoroEditorLiveUi)
    endif()
    horo_attach_gui_live_host(HoroEditor)

    if(APPLE)
        # SDL_GetBasePath resolves to Contents/Resources for an application bundle.
        set(HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE "assets")
        set(HORO_EDITOR_BUILD_ASSET_ROOT "$<TARGET_BUNDLE_DIR:HoroEditor>/Contents/Resources/assets")
    else()
        file(RELATIVE_PATH HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE
            "${CMAKE_INSTALL_FULL_BINDIR}"
            "${CMAKE_INSTALL_FULL_DATADIR}/horo-engine/assets"
        )
        set(HORO_EDITOR_BUILD_ASSET_ROOT "$<TARGET_FILE_DIR:HoroEditor>/${HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE}")
    endif()

    # Offline qualification stages generated cook output beside the same assets consumed by the real host.
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/HoroEditorAssetRoot-$<CONFIG>.txt"
        CONTENT "${HORO_EDITOR_BUILD_ASSET_ROOT}\n")

    add_custom_command(TARGET HoroEditor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets"
            "${HORO_EDITOR_BUILD_ASSET_ROOT}"
        COMMENT "Staging Horo Editor runtime assets"
        VERBATIM
    )

    if(APPLE)
        set(HORO_EDITOR_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/resources/HoroEditor.icns")
        set_source_files_properties("${HORO_EDITOR_APP_ICON}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
        target_sources(HoroEditor PRIVATE "${HORO_EDITOR_APP_ICON}")
        set_target_properties(HoroEditor PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "Horo Editor"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${HORO_EDITOR_APP_ID}"
            MACOSX_BUNDLE_ICON_FILE "HoroEditor.icns"
            MACOSX_BUNDLE_LONG_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        )
        install(TARGETS HoroEditor
            BUNDLE DESTINATION "."
            COMPONENT Editor
        )
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/"
            DESTINATION "HoroEditor.app/Contents/Resources/assets"
            COMPONENT Editor
        )
    else()
        install(TARGETS HoroEditor
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            COMPONENT Editor
        )
        install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/"
            DESTINATION "${HORO_EDITOR_ASSET_INSTALL_DIR}"
            COMPONENT Editor
        )
    endif()

    if(WIN32)
        set(HORO_EDITOR_WINDOWS_ICON "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/resources/windows/HoroEditor.ico")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/resources/windows/HoroEditor.rc.in"
            "${CMAKE_CURRENT_BINARY_DIR}/HoroEditor.rc"
            @ONLY
        )
        target_sources(HoroEditor PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/HoroEditor.rc")
    endif()

    if(UNIX AND NOT APPLE)
        set(HORO_EDITOR_DESKTOP_FILE "${CMAKE_CURRENT_BINARY_DIR}/${HORO_EDITOR_APP_ID}.desktop")
        configure_file(
            "${CMAKE_CURRENT_SOURCE_DIR}/HoroEditor/resources/linux/HoroEditor.desktop.in"
            "${HORO_EDITOR_DESKTOP_FILE}"
            @ONLY
        )
        install(FILES "${PROJECT_SOURCE_DIR}/assets/launcher/logo.png"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/1024x1024/apps"
            RENAME "${HORO_EDITOR_APP_ID}.png"
            COMPONENT Editor
        )
        install(FILES "${HORO_EDITOR_DESKTOP_FILE}"
            DESTINATION "${CMAKE_INSTALL_DATADIR}/applications"
            COMPONENT Editor
        )
    endif()

    target_compile_features(HoroEditor PRIVATE cxx_std_20)
    set_property(SOURCE HoroEditor/app/HoroEditorApp.cpp
        APPEND PROPERTY COMPILE_DEFINITIONS HORO_SOURCE_REVISION="${HORO_SOURCE_REVISION}")
    target_link_libraries(HoroEditor PRIVATE HoroNativeNavigationBakeHost HoroEngine::PlatformProjectIo)

    if(BUILD_TESTING AND HORO_ENABLE_GPU_SMOKE_TESTS AND HORO_BUILD_RENDER_OPENGL)
        target_sources(HoroEditor PRIVATE HoroEditor/app/EditorSurfaceLifecycleQualification.cpp)
        target_compile_definitions(HoroEditor PRIVATE HORO_EDITOR_SURFACE_QUALIFICATION=1)
    endif()

    target_include_directories(HoroEditor
        PRIVATE
            ${PROJECT_SOURCE_DIR}/apps/HoroEditor/app
            ${PROJECT_SOURCE_DIR}/src
            ${PROJECT_SOURCE_DIR}/vendor/stb
            ${PROJECT_SOURCE_DIR}/vendor/pfd
            ${HORO_GENERATED_DIR}
    )

    target_compile_definitions(HoroEditor
        PRIVATE
            GL_SILENCE_DEPRECATION
            HORO_EDITOR_APP_ID="${HORO_EDITOR_APP_ID}"
            HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE="${HORO_EDITOR_PACKAGED_ASSET_ROOT_RELATIVE}"
            HORO_ENGINE_VERSION_STRING="${PROJECT_VERSION}"
            HORO_BUILD_CONFIGURATION="$<CONFIG>"
            HORO_GAMEPLAY_SDK_PACKAGE_DIR="${HORO_GAMEPLAY_SDK_PACKAGE_DIR}"
            HORO_GAMEPLAY_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
            HORO_GAMEPLAY_CMAKE_EXECUTABLE="${CMAKE_COMMAND}"
    )

    target_link_libraries(HoroEditor
        PRIVATE
            HoroPackageProjectHost
            HoroRuntimeUiPaintHost
            HoroEditorUiStyleHostInternal
            HoroEditorTerrainHostInternal
            HoroEngine::ShaderBuild
            HoroEngine::Gui
            HoroEngine::EditorServices
            HoroEngine::EditorRenderExtraction
            HoroEngine::RenderFrontend
            HoroEditorPresentationSurfaceInternal
            HoroEngine::Runtime
            HoroEngine::RuntimeScene
            HoroEngine::PhysicsSceneIntegration
            HoroEditorSceneProductHost
            HoroEditorPlaySceneHostInternal
            HoroScenePhysicsHost
            HoroEngine::Extensions
            HoroEngine::Platform
            HoroEngine::ProjectMigrations
            HoroEngine::UserStateMigration
            HoroEngine::InputSdl
            HoroEngine::UpdateDiscovery
            HoroEngine::UpdateDownload
            CURL::libcurl
            HoroHostModuleComposition
            HoroThirdParty::ImGui
            nlohmann_json::nlohmann_json
    )

    target_link_libraries(HoroEditor PRIVATE HoroAudioFrontendComposition HoroEngine::AudioImport HoroEngine::AudioNull)
    if(HORO_BUILD_XR_OPENXR)
        set(HORO_EDITOR_XR_BOOTSTRAP_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_xr_bootstrap_headers")
        file(MAKE_DIRECTORY "${HORO_EDITOR_XR_BOOTSTRAP_HEADERS}")
        foreach(header EditorOpenXRBootstrap.h EditorOpenXRHostServices.h)
            configure_file("HoroEditor/app/${header}" "${HORO_EDITOR_XR_BOOTSTRAP_HEADERS}/${header}" COPYONLY)
        endforeach()
        add_library(HoroEditorOpenXRBootstrap STATIC HoroEditor/app/EditorOpenXRBootstrap.cpp)
        target_compile_features(HoroEditorOpenXRBootstrap PUBLIC cxx_std_20)
        target_include_directories(HoroEditorOpenXRBootstrap PUBLIC "${HORO_EDITOR_XR_BOOTSTRAP_HEADERS}")
        target_link_libraries(HoroEditorOpenXRBootstrap
            PUBLIC HoroEngine::Foundation HoroEngine::RuntimeScene HoroOpenXRProductHost
            PRIVATE HoroOpenXRApplicationBindings HoroHostSignatureDocuments HoroEngine::Security HoroEngine::Platform HoroEngine::XRRuntime)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_OPENXR_HOST=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::XRRuntime HoroOpenXRProductHost HoroEditorOpenXRBootstrap)
        if(TARGET HoroOpenXRApplicationHost)
            target_compile_definitions(HoroEditor PRIVATE HORO_HAS_OPENXR_VULKAN_HOST=1)
            target_link_libraries(HoroEditor PRIVATE HoroOpenXRApplicationHost)
        endif()
    endif()
    if(TARGET HoroAudioSdl3)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::AudioSdl3)
        target_compile_definitions(HoroEditor PRIVATE HORO_EDITOR_AUDIO_SDL3=1)
    else()
        target_compile_definitions(HoroEditor PRIVATE HORO_EDITOR_AUDIO_SDL3=0)
    endif()


    # Development composition: concrete backends are linked explicitly at the
    # executable boundary until RenderModuleHost owns installed-product loading.
    if(HORO_BUILD_RENDER_OPENGL)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_RENDER_OPENGL=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::EditorViewportOpenGL)
    endif()

    if(HORO_BUILD_RENDER_VULKAN)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_RENDER_VULKAN=1)
        target_sources(HoroEditor PRIVATE HoroEditor/app/EditorVulkanShaderAssets.cpp)
        target_link_libraries(HoroEditor PRIVATE HoroEditorVulkanGuiInternal)
    endif()

    if(HORO_BUILD_RENDER_METAL)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_RENDER_METAL=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::EditorViewportMetal)
    endif()
endif()
