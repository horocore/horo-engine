# The production graph canvas uses the private HoroGui adapter. Do not
# configure upstream examples, which fetch a different Dear ImGui package.
set(HORO_IMGUI_NODE_EDITOR_REVISION "1e0e494f1a86885eb332ca518543108ccd88d13e")
FetchContent_Declare(imgui_node_editor
    URL https://codeload.github.com/thedmd/imgui-node-editor/tar.gz/${HORO_IMGUI_NODE_EDITOR_REVISION}
    URL_HASH SHA256=104f8863ad817029fd41042b3958d9fc5241a7bff6ecb6a30089d1d71944ed72
    SOURCE_SUBDIR horo_no_upstream_project
)
FetchContent_MakeAvailable(imgui_node_editor)
add_library(HoroThirdPartyImGuiNodeEditor STATIC
    ${imgui_node_editor_SOURCE_DIR}/crude_json.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_canvas.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor.cpp
    ${imgui_node_editor_SOURCE_DIR}/imgui_node_editor_api.cpp
)
add_library(HoroThirdParty::ImGuiNodeEditor ALIAS HoroThirdPartyImGuiNodeEditor)
target_compile_features(HoroThirdPartyImGuiNodeEditor PRIVATE cxx_std_20)
target_include_directories(HoroThirdPartyImGuiNodeEditor SYSTEM PUBLIC ${imgui_node_editor_SOURCE_DIR})
target_link_libraries(HoroThirdPartyImGuiNodeEditor PRIVATE HoroThirdParty::ImGui)
set_target_properties(HoroThirdPartyImGuiNodeEditor PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(READ "${imgui_node_editor_SOURCE_DIR}/LICENSE" horo_imgui_node_editor_notice)
string(REPLACE "\r\n" "\n" horo_imgui_node_editor_notice "${horo_imgui_node_editor_notice}")
string(SHA256 horo_imgui_node_editor_notice_digest "${horo_imgui_node_editor_notice}")
if(NOT horo_imgui_node_editor_notice_digest STREQUAL "2d176cd30f8dc07a5b8e922fb0de308ffea7b8f2d7ff5a5f2ee99b562ed05b0c")
    message(FATAL_ERROR "Pinned imgui-node-editor notice differs from the reviewed upstream notice")
endif()
install(FILES "${imgui_node_editor_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses" RENAME ImGuiNodeEditor.txt)
