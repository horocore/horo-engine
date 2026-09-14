add_library(HoroHostModuleComposition STATIC
    common/HostModuleComposition.cpp
)
target_compile_features(HoroHostModuleComposition PUBLIC cxx_std_20)
target_include_directories(HoroHostModuleComposition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/common
)
target_link_libraries(HoroHostModuleComposition PUBLIC HoroEngine::Foundation)

# The terminal/headless composition remains available without GUI or GPU targets.
add_executable(horo-engine
    horo-engine/main.cpp
)
target_compile_features(horo-engine PRIVATE cxx_std_20)
target_compile_definitions(horo-engine PRIVATE
    HORO_ENGINE_VERSION_STRING="${PROJECT_VERSION}"
    HORO_BUILD_CONFIGURATION="$<CONFIG>"
    HORO_SOURCE_REVISION="${HORO_SOURCE_REVISION}"
)
target_link_libraries(horo-engine PRIVATE HoroEngine::Application HoroHostModuleComposition)

add_executable(horo-extension-validate
    horo-extension-validate/main.cpp
)
target_compile_features(horo-extension-validate PRIVATE cxx_std_20)
target_link_libraries(horo-extension-validate PRIVATE HoroEngine::Extensions)
set(HORO_EXTENSION_VALIDATOR_SDK_PATH
    "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-validate${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(
    OUTPUT "${HORO_EXTENSION_VALIDATOR_SDK_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:horo-extension-validate>"
        "${HORO_EXTENSION_VALIDATOR_SDK_PATH}"
    DEPENDS horo-extension-validate
    COMMENT "Staging the extension manifest validator in the SDK"
    VERBATIM)
add_custom_target(HoroExtensionSdkValidatorStage ALL
    DEPENDS "${HORO_EXTENSION_VALIDATOR_SDK_PATH}")

add_executable(horo-extension-conformance
    horo-extension-conformance/main.cpp
)
target_compile_features(horo-extension-conformance PRIVATE cxx_std_20)
target_link_libraries(horo-extension-conformance PRIVATE HoroEngine::Extensions)
set(HORO_EXTENSION_CONFORMANCE_SDK_PATH
    "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-conformance${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(
    OUTPUT "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:horo-extension-conformance>"
        "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}"
    DEPENDS horo-extension-conformance
    COMMENT "Staging the extension ABI conformance harness in the SDK"
    VERBATIM)
add_custom_target(HoroExtensionSdkConformanceStage ALL
    DEPENDS "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}")

# The graphical executable is an optional composition root. Foundation and
# headless hosts must remain buildable when the GUI adapter is intentionally off.
if(HORO_BUILD_EDITOR_GUI)
    set(HORO_EDITOR_APP_ID "dev.horo.editor")
    set(HORO_EDITOR_ASSET_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/horo-engine/assets")

    add_executable(HoroEditor
        HoroEditor/main.cpp
        HoroEditor/app/HoroEditorApp.cpp
    )

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
            HORO_SOURCE_REVISION="${HORO_SOURCE_REVISION}"
            HORO_GAMEPLAY_SDK_PACKAGE_DIR="${HORO_GAMEPLAY_SDK_PACKAGE_DIR}"
            HORO_GAMEPLAY_CXX_COMPILER="${CMAKE_CXX_COMPILER}"
    )

    target_link_libraries(HoroEditor
        PRIVATE
            HoroEngine::Gui
            HoroEngine::EditorServices
            HoroEngine::EditorRenderExtraction
            HoroEngine::RenderFrontend
            HoroEngine::Runtime
            HoroEngine::RuntimeScene
            HoroEngine::PhysicsSceneIntegration
            HoroEngine::Extensions
            HoroEngine::Platform
            HoroEngine::ProjectMigrations
            HoroEngine::InputSdl
            HoroHostModuleComposition
            HoroThirdParty::ImGui
    )

    if(HORO_ENABLE_OPENTELEMETRY)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_OPENTELEMETRY=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::OpenTelemetry)
    endif()

    # Development composition: concrete backends are linked explicitly at the
    # executable boundary until RenderModuleHost owns installed-product loading.
    if(HORO_BUILD_RENDER_OPENGL)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_RENDER_OPENGL=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::EditorViewportOpenGL)
    endif()

    if(HORO_BUILD_RENDER_METAL)
        target_compile_definitions(HoroEditor PRIVATE HORO_HAS_RENDER_METAL=1)
        target_link_libraries(HoroEditor PRIVATE HoroEngine::EditorViewportMetal)
    endif()
endif()
