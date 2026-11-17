# Non-installed development GUI; public RuntimeConsole remains independent of ImGui/editor.
set(HORO_EDITOR_RUNTIME_CONSOLE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/editor_runtime_console_headers")
file(MAKE_DIRECTORY "${HORO_EDITOR_RUNTIME_CONSOLE_HEADERS}")
configure_file(editor/development/console/RuntimeConsoleView.h
    "${HORO_EDITOR_RUNTIME_CONSOLE_HEADERS}/RuntimeConsoleView.h" COPYONLY)
configure_file(editor/design_system/components/RuntimeConsoleRecordList.h
    "${HORO_EDITOR_RUNTIME_CONSOLE_HEADERS}/RuntimeConsoleRecordList.h" COPYONLY)
add_library(HoroEditorRuntimeConsoleViewInternal STATIC
    editor/development/console/RuntimeConsoleView.cpp
    editor/development/console/RuntimeOverlayPanels.cpp
    editor/design_system/components/RuntimeConsoleRecordList.cpp)
target_compile_features(HoroEditorRuntimeConsoleViewInternal PUBLIC cxx_std_20)
target_include_directories(HoroEditorRuntimeConsoleViewInternal PUBLIC "${HORO_EDITOR_RUNTIME_CONSOLE_HEADERS}"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/editor/design_system/components")
target_link_libraries(HoroEditorRuntimeConsoleViewInternal PUBLIC HoroEngine::Gui HoroEngine::RuntimeConsoleUi)
target_link_libraries(HoroEditorRuntimeConsoleViewInternal PRIVATE HoroRuntimeConsoleSceneInternal HoroNetworkConsoleInspectionInternal)
