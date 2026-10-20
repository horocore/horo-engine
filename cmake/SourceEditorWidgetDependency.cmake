# Zep is private to the GUI source-widget implementation. Do not execute its
# top-level project (which changes global flags, tests, install and toolchain).
set(HORO_ZEP_REVISION "7aa7ed940d5c889dc6b27a2529a740c0bb2ad6af")
FetchContent_Declare(zep
    GIT_REPOSITORY https://github.com/Rezonality/zep.git
    GIT_TAG "${HORO_ZEP_REVISION}"
    SOURCE_SUBDIR horo_no_upstream_project
)
FetchContent_MakeAvailable(zep)
add_library(HoroThirdPartyZep STATIC
    ${zep_SOURCE_DIR}/src/buffer.cpp
    ${zep_SOURCE_DIR}/src/range_markers.cpp
    ${zep_SOURCE_DIR}/src/glyph_iterator.cpp
    ${zep_SOURCE_DIR}/src/commands.cpp
    ${zep_SOURCE_DIR}/src/display.cpp
    ${zep_SOURCE_DIR}/src/editor.cpp
    ${zep_SOURCE_DIR}/src/filesystem.cpp
    ${zep_SOURCE_DIR}/src/indexer.cpp
    ${zep_SOURCE_DIR}/src/keymap.cpp
    ${zep_SOURCE_DIR}/src/line_widgets.cpp
    ${zep_SOURCE_DIR}/src/mcommon/animation/timer.cpp
    ${zep_SOURCE_DIR}/src/mcommon/string/stringutils.cpp
    ${zep_SOURCE_DIR}/src/mcommon/file/path.cpp
    ${zep_SOURCE_DIR}/src/mode.cpp
    ${zep_SOURCE_DIR}/src/mode_search.cpp
    ${zep_SOURCE_DIR}/src/mode_standard.cpp
    ${zep_SOURCE_DIR}/src/mode_tree.cpp
    ${zep_SOURCE_DIR}/src/mode_vim.cpp
    ${zep_SOURCE_DIR}/src/mode_repl.cpp
    ${zep_SOURCE_DIR}/src/regress.cpp
    ${zep_SOURCE_DIR}/src/scroller.cpp
    ${zep_SOURCE_DIR}/src/splits.cpp
    ${zep_SOURCE_DIR}/src/syntax.cpp
    ${zep_SOURCE_DIR}/src/syntax_providers.cpp
    ${zep_SOURCE_DIR}/src/syntax_rainbow_brackets.cpp
    ${zep_SOURCE_DIR}/src/syntax_tree.cpp
    ${zep_SOURCE_DIR}/src/syntax_markdown.cpp
    ${zep_SOURCE_DIR}/src/tab_window.cpp
    ${zep_SOURCE_DIR}/src/theme.cpp
    ${zep_SOURCE_DIR}/src/window.cpp
)
add_library(HoroThirdParty::Zep ALIAS HoroThirdPartyZep)
target_compile_features(HoroThirdPartyZep PRIVATE cxx_std_20)
target_include_directories(HoroThirdPartyZep SYSTEM PUBLIC ${zep_SOURCE_DIR}/include)
target_include_directories(HoroThirdPartyZep PRIVATE ${zep_SOURCE_DIR}/src ${zep_SOURCE_DIR}/src/mcommon)
# No ZEP_FEATURE_CPP_FILE_SYSTEM: only the adapter's denied filesystem is usable.
set_target_properties(HoroThirdPartyZep PROPERTIES POSITION_INDEPENDENT_CODE ON)
find_package(Threads REQUIRED)
target_link_libraries(HoroThirdPartyZep PRIVATE Threads::Threads)

file(READ "${zep_SOURCE_DIR}/LICENSE" horo_zep_notice)
string(REPLACE "\r\n" "\n" horo_zep_notice "${horo_zep_notice}")
string(SHA256 horo_zep_notice_digest "${horo_zep_notice}")
if(NOT horo_zep_notice_digest STREQUAL "7568971a27f1e69deb308e1e31ce3df3953923fb27788893697481c5eca7abe1")
    message(FATAL_ERROR "Pinned Zep notice differs from the reviewed upstream notice")
endif()
install(FILES "${zep_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses" RENAME Zep.txt)
