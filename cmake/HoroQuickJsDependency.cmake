include_guard(GLOBAL)
include(FetchContent)
include(CheckCCompilerFlag)
include(GNUInstallDirs)

# Source-only population keeps upstream CLI, install and global options out of Horo.
set(HORO_QUICKJS_REVISION "d7215b0ad3fc2c562e8317fd9bca058fd4863804")
set(HORO_QUICKJS_ARCHIVE_SHA256 "a49defe70cbcae9226d9fac025af116e87ebae2440dcff1ca3c80b79e1cdef0b")
FetchContent_Declare(horo_quickjs
    URL "https://codeload.github.com/quickjs-ng/quickjs/tar.gz/${HORO_QUICKJS_REVISION}"
    URL_HASH "SHA256=${HORO_QUICKJS_ARCHIVE_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR .horo-source-only)
FetchContent_MakeAvailable(horo_quickjs)

file(SHA256 "${horo_quickjs_SOURCE_DIR}/LICENSE" _horo_quickjs_license_sha256)
if(NOT _horo_quickjs_license_sha256 STREQUAL "96f73f9d2a16c21a36b418f06073be26e7d6d5e7c1bc99756b21a4f2c74ef171")
    message(FATAL_ERROR "Pinned QuickJS-NG license differs from the reviewed MIT notice")
endif()

add_library(HoroThirdPartyQuickJS STATIC
    "${horo_quickjs_SOURCE_DIR}/dtoa.c"
    "${horo_quickjs_SOURCE_DIR}/libregexp.c"
    "${horo_quickjs_SOURCE_DIR}/libunicode.c"
    "${horo_quickjs_SOURCE_DIR}/quickjs.c")
add_library(HoroThirdParty::QuickJS ALIAS HoroThirdPartyQuickJS)
set_target_properties(HoroThirdPartyQuickJS PROPERTIES
    POSITION_INDEPENDENT_CODE ON C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)
target_include_directories(HoroThirdPartyQuickJS PUBLIC "${horo_quickjs_SOURCE_DIR}")
target_compile_definitions(HoroThirdPartyQuickJS PRIVATE QUICKJS_NG_BUILD _GNU_SOURCE)
if(MSVC)
    target_compile_options(HoroThirdPartyQuickJS PRIVATE /J)
    check_c_compiler_flag("/experimental:c11atomics" HORO_QUICKJS_HAS_C11_ATOMICS_FLAG)
    if(HORO_QUICKJS_HAS_C11_ATOMICS_FLAG)
        target_compile_options(HoroThirdPartyQuickJS PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(HoroThirdPartyQuickJS PRIVATE -funsigned-char)
endif()
if(WIN32)
    target_compile_definitions(HoroThirdPartyQuickJS PRIVATE WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0601)
else()
    target_link_libraries(HoroThirdPartyQuickJS PRIVATE m)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "tvOS")
    target_compile_definitions(HoroThirdPartyQuickJS PRIVATE _TVOS)
elseif(CMAKE_SYSTEM_NAME STREQUAL "watchOS")
    target_compile_definitions(HoroThirdPartyQuickJS PRIVATE _WATCHOS)
endif()
find_package(Threads REQUIRED)
target_link_libraries(HoroThirdPartyQuickJS PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
install(FILES "${horo_quickjs_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses" RENAME "QuickJS-NG-LICENSE" COMPONENT Runtime)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/third-party/QuickJS-NG.txt" CONTENT
    "QuickJS-NG v0.18.0\nrevision=${HORO_QUICKJS_REVISION}\narchive-sha256=${HORO_QUICKJS_ARCHIVE_SHA256}\nlicense=MIT\nprofile=static-core-no-libc\n")
install(FILES "${CMAKE_BINARY_DIR}/third-party/QuickJS-NG.txt"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses" COMPONENT Runtime)
