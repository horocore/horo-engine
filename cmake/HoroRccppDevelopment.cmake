# Private pinned RCC++ backend and native platform transports. No game-module or extension SDK dependency.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_CROSSCOMPILING
        OR NOT ((WIN32 AND MSVC) OR (APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")))
    message(FATAL_ERROR "HORO_EDITOR_LIVE_UI requires native Windows x64/MSVC or macOS/AppleClang")
endif()
if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(FATAL_ERROR "HORO_EDITOR_LIVE_UI requires a Debug development build")
endif()

include(FetchContent)
FetchContent_Declare(horo_rccpp
    GIT_REPOSITORY https://github.com/RuntimeCompiledCPlusPlus/RuntimeCompiledCPlusPlus.git
    GIT_TAG 005c05145d98b87d974c36fc885003ea88bf3932
    SOURCE_SUBDIR horo_no_upstream_project
)
FetchContent_MakeAvailable(horo_rccpp)
include("${CMAKE_CURRENT_LIST_DIR}/ApplyRccppDevelopmentPatch.cmake")
file(READ "${horo_rccpp_SOURCE_DIR}/readme.md" horo_rccpp_notice)
string(REPLACE "\r\n" "\n" horo_rccpp_notice "${horo_rccpp_notice}")
string(SHA256 horo_rccpp_notice_digest "${horo_rccpp_notice}")
if(NOT horo_rccpp_notice_digest STREQUAL "566018f77015429efee3e966819319373b15433fd73f017580450b6c1dc66edf")
    message(FATAL_ERROR "Pinned RCC++ license/readme differs from the reviewed upstream notice")
endif()
install(FILES "${horo_rccpp_SOURCE_DIR}/readme.md"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses" RENAME RCCpp.txt)
set(HORO_RCCPP_ROOT "${horo_rccpp_SOURCE_DIR}/Aurora")
file(GLOB horo_rccpp_sources CONFIGURE_DEPENDS
    "${HORO_RCCPP_ROOT}/RuntimeCompiler/*.cpp"
    "${HORO_RCCPP_ROOT}/RuntimeCompiler/SimpleFileWatcher/*.cpp"
    "${HORO_RCCPP_ROOT}/RuntimeObjectSystem/*.cpp"
    "${HORO_RCCPP_ROOT}/RuntimeObjectSystem/ObjectFactorySystem/*.cpp"
    "${HORO_RCCPP_ROOT}/RuntimeObjectSystem/SimpleSerializer/*.cpp"
)
if(APPLE)
    list(FILTER horo_rccpp_sources EXCLUDE REGEX "(PlatformWindows|Compiler_PlatformPosix|FileWatcherLinux|FileWatcherOSX|FileWatcherWin32)\\.cpp$")
    list(APPEND horo_rccpp_sources
        "${PROJECT_SOURCE_DIR}/src/editor/development/live_reload/rccpp/platform/macos/RccppMacCompiler.cpp"
        "${PROJECT_SOURCE_DIR}/src/editor/development/live_reload/rccpp/platform/macos/RccppMacFileWatcher.cpp")
else()
    list(FILTER horo_rccpp_sources EXCLUDE REGEX "(PlatformPosix|FileWatcherLinux|FileWatcherOSX)\\.cpp$")
endif()
add_library(HoroThirdPartyRccpp STATIC ${horo_rccpp_sources})
set_property(TARGET HoroThirdPartyRccpp PROPERTY HORO_RCCPP_ROOT "${HORO_RCCPP_ROOT}")
target_compile_features(HoroThirdPartyRccpp PRIVATE cxx_std_20)
if(MSVC)
    target_compile_options(HoroThirdPartyRccpp PRIVATE /FC /utf-8)
    target_compile_definitions(HoroThirdPartyRccpp PRIVATE NOMINMAX)
    target_link_libraries(HoroThirdPartyRccpp PRIVATE advapi32)
elseif(APPLE)
    target_compile_definitions(HoroThirdPartyRccpp PRIVATE _DEBUG)
endif()
target_include_directories(HoroThirdPartyRccpp SYSTEM PRIVATE
    "${HORO_RCCPP_ROOT}/RuntimeCompiler"
    "${HORO_RCCPP_ROOT}/RuntimeObjectSystem"
)
