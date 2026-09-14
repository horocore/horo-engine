include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED ON CACHE BOOL "" FORCE)

if(HORO_BUILD_PHYSICS_NATIVE)
    include(HoroPhysicsDependency)
    horo_add_canonical_physics_dependency()
endif()

if(HORO_BUILD_NAVIGATION_RECAST_DETOUR)
    include(HoroNavigationDependency)
    horo_add_navigation_runtime_dependency()
endif()

set(HORO_NLOHMANN_JSON_REVISION "9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03")
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG "${HORO_NLOHMANN_JSON_REVISION}"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

set(HORO_UFBX_REVISION "83bc7cf44f76bc8622de63b809a42b5d557cd733")
FetchContent_Declare(
    ufbx
    GIT_REPOSITORY https://github.com/ufbx/ufbx.git
    GIT_TAG "${HORO_UFBX_REVISION}"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ufbx)

set(HORO_LUA_VERSION "5.4.8")
FetchContent_Declare(
    lua
    URL "https://www.lua.org/ftp/lua-${HORO_LUA_VERSION}.tar.gz"
    URL_HASH "SHA256=4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(lua)

add_library(HoroThirdPartyLua STATIC
    ${lua_SOURCE_DIR}/src/lapi.c
    ${lua_SOURCE_DIR}/src/lauxlib.c
    ${lua_SOURCE_DIR}/src/lbaselib.c
    ${lua_SOURCE_DIR}/src/lcode.c
    ${lua_SOURCE_DIR}/src/lcorolib.c
    ${lua_SOURCE_DIR}/src/lctype.c
    ${lua_SOURCE_DIR}/src/ldblib.c
    ${lua_SOURCE_DIR}/src/ldebug.c
    ${lua_SOURCE_DIR}/src/ldo.c
    ${lua_SOURCE_DIR}/src/ldump.c
    ${lua_SOURCE_DIR}/src/lfunc.c
    ${lua_SOURCE_DIR}/src/lgc.c
    ${lua_SOURCE_DIR}/src/linit.c
    ${lua_SOURCE_DIR}/src/liolib.c
    ${lua_SOURCE_DIR}/src/llex.c
    ${lua_SOURCE_DIR}/src/lmathlib.c
    ${lua_SOURCE_DIR}/src/lmem.c
    ${lua_SOURCE_DIR}/src/loadlib.c
    ${lua_SOURCE_DIR}/src/lobject.c
    ${lua_SOURCE_DIR}/src/lopcodes.c
    ${lua_SOURCE_DIR}/src/loslib.c
    ${lua_SOURCE_DIR}/src/lparser.c
    ${lua_SOURCE_DIR}/src/lstate.c
    ${lua_SOURCE_DIR}/src/lstring.c
    ${lua_SOURCE_DIR}/src/lstrlib.c
    ${lua_SOURCE_DIR}/src/ltable.c
    ${lua_SOURCE_DIR}/src/ltablib.c
    ${lua_SOURCE_DIR}/src/ltm.c
    ${lua_SOURCE_DIR}/src/lundump.c
    ${lua_SOURCE_DIR}/src/lutf8lib.c
    ${lua_SOURCE_DIR}/src/lvm.c
    ${lua_SOURCE_DIR}/src/lzio.c
)
add_library(HoroThirdParty::Lua ALIAS HoroThirdPartyLua)
set_target_properties(HoroThirdPartyLua PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(HoroThirdPartyLua PUBLIC ${lua_SOURCE_DIR}/src)
if(APPLE)
    target_compile_definitions(HoroThirdPartyLua PRIVATE LUA_USE_MACOSX)
elseif(UNIX)
    target_compile_definitions(HoroThirdPartyLua PRIVATE LUA_USE_LINUX)
    target_link_libraries(HoroThirdPartyLua PRIVATE dl m)
elseif(WIN32)
    target_compile_definitions(HoroThirdPartyLua PRIVATE LUA_USE_WINDOWS)
endif()

set(_HORO_BUILD_TESTING "${BUILD_TESTING}")
set(HORO_MINIZ_REVISION "174573d60290f447c13a2b1b3405de2b96e27d6c")
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG "${HORO_MINIZ_REVISION}"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(miniz)

# miniz hard-codes /Zi in its MSVC flags. With /Z7 selected for cached builds,
# sccache otherwise expects a PDB that the compiler intentionally does not emit.
if(MSVC AND CMAKE_C_COMPILER_LAUNCHER MATCHES "sccache")
    set_property(TARGET miniz PROPERTY C_COMPILER_LAUNCHER "")
endif()

# Portable package paths need Unicode normalization and full case folding.
set(HORO_UTF8PROC_REVISION "d7bf128df773c2a1a7242eb80e51e91a769fc985")
set(UTF8PROC_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(UTF8PROC_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    utf8proc
    GIT_REPOSITORY https://github.com/JuliaStrings/utf8proc.git
    GIT_TAG "${HORO_UTF8PROC_REVISION}"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(utf8proc)

# The extension marketplace is part of every editor distribution. Build its
# HTTPS stack from pinned sources so users do not need a separately installed
# libcurl SDK. Prefer the native Windows trust store through Schannel; use the
# portable Mbed TLS backend on other targets. The security module also uses the
# pinned crypto target on every platform; this is independent of curl's TLS
# provider selection.
set(HORO_MBEDTLS_REVISION "5b64a9fdb979c8971561ec78221b528e3cc4e00a")
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    MbedTLS
    GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
    GIT_TAG "${HORO_MBEDTLS_REVISION}"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(MbedTLS)

if(NOT WIN32)
    # curl discovers TLS providers through find_package(), while Mbed TLS is
    # already part of this build through FetchContent. Give curl's find module
    # the populated targets instead of requiring a second system SDK.
    set(MBEDTLS_INCLUDE_DIR "${mbedtls_SOURCE_DIR}/include" CACHE PATH "" FORCE)
    set(MBEDTLS_LIBRARY "MbedTLS::mbedtls" CACHE STRING "" FORCE)
    set(MBEDX509_LIBRARY "MbedTLS::mbedx509" CACHE STRING "" FORCE)
    set(MBEDCRYPTO_LIBRARY "MbedTLS::mbedcrypto" CACHE STRING "" FORCE)
    # The pinned revision provides this API. Avoid curl's isolated
    # try_compile() losing visibility of the parent FetchContent targets.
    set(HAVE_MBEDTLS_DES_CRYPT_ECB ON CACHE BOOL "" FORCE)
    set(CURL_USE_CMAKECONFIG OFF CACHE BOOL "" FORCE)
endif()

set(HORO_CURL_REVISION "6e3f8dc1f173b47de9a68516ce4b95bf25598c2f")
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE STRING "" FORCE)
set(CURL_BROTLI OFF CACHE STRING "" FORCE)
set(CURL_ZSTD OFF CACHE STRING "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
else()
    set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
endif()
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    CURL
    GIT_REPOSITORY https://github.com/curl/curl.git
    GIT_TAG "${HORO_CURL_REVISION}"
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(CURL)
set(BUILD_TESTING "${_HORO_BUILD_TESTING}" CACHE BOOL "" FORCE)
unset(_HORO_BUILD_TESTING)

add_library(HoroThirdPartyUfbx STATIC
    ${ufbx_SOURCE_DIR}/ufbx.c
)
add_library(HoroThirdParty::Ufbx ALIAS HoroThirdPartyUfbx)
target_include_directories(HoroThirdPartyUfbx
    PUBLIC
        ${ufbx_SOURCE_DIR}
)

if(BUILD_TESTING)
    set(HORO_CATCH2_REVISION "6ee0826dcae55ed1e06b2c5701981221e979e1e6")
    set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
    set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG "${HORO_CATCH2_REVISION}"
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    # Ensure Catch2 sees the same C++ standard as the rest of the project.
    # Without this the library defaults to C++14 on AppleClang/MSVC and
    # misses StringMaker<std::string_view>, while tests built with cxx_std_20
    # expect it — linker error on macOS/Windows.
    if(TARGET Catch2)
        target_compile_features(Catch2 PRIVATE cxx_std_20)
    endif()
    if(TARGET Catch2WithMain)
        target_compile_features(Catch2WithMain PRIVATE cxx_std_20)
    endif()
endif()

if(HORO_BUILD_RENDER_OPENGL)
    find_package(OpenGL REQUIRED)
endif()

if(HORO_BUILD_RENDER_VULKAN)
    # Vulkan headers are a pinned private build dependency. Runtime loading stays
    # behind the host-owned loader lease and never requires an installed SDK.
    set(HORO_VULKAN_HEADERS_REVISION "29f979ee5aa58b7b005f805ea8df7a855c39ff37")
    set(VULKAN_HEADERS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        VulkanHeaders
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG "${HORO_VULKAN_HEADERS_REVISION}"
        GIT_SHALLOW FALSE
    )
    FetchContent_MakeAvailable(VulkanHeaders)
endif()


if(HORO_BUILD_EDITOR_GUI OR HORO_BUILD_AUDIO_SDL3)
    set(HORO_SDL3_REVISION
        "f87239e71e42da91ca317a12eefb82cfbf3393eb"
    )
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG "${HORO_SDL3_REVISION}"
    )
    FetchContent_MakeAvailable(SDL3)

    if(TARGET SDL3::SDL3-static)
        set(HORO_SDL3_TARGET SDL3::SDL3-static)
    elseif(TARGET SDL3::SDL3)
        set(HORO_SDL3_TARGET SDL3::SDL3)
    elseif(TARGET SDL3-static)
        set(HORO_SDL3_TARGET SDL3-static)
    elseif(TARGET SDL3)
        set(HORO_SDL3_TARGET SDL3)
    else()
        message(FATAL_ERROR "SDL3 dependency did not provide a usable CMake target")
    endif()
endif()

if(HORO_BUILD_EDITOR_GUI)
    set(HORO_IMGUI_REVISION
        "993fa347495860ed44b83574254ef2a317d0c14f"
    )

    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG "${HORO_IMGUI_REVISION}"
    )
    FetchContent_MakeAvailable(imgui)

    if(HORO_ENABLE_IMGUI_UI_TESTS)
        set(HORO_IMGUI_TEST_ENGINE_REVISION
            "4018a79b61da483544ccbfbc2f6e8e85a35c2cbc"
        )
        FetchContent_Declare(
            imgui_test_engine
            GIT_REPOSITORY https://github.com/ocornut/imgui_test_engine.git
            GIT_TAG "${HORO_IMGUI_TEST_ENGINE_REVISION}"
            GIT_SHALLOW TRUE
        )
        FetchContent_MakeAvailable(imgui_test_engine)
    endif()

    if(HORO_BUILD_RENDER_OPENGL)
        add_library(HoroThirdPartyGlad STATIC
            ${CMAKE_CURRENT_LIST_DIR}/../vendor/glad/src/gl.c
        )
        add_library(HoroThirdParty::Glad ALIAS HoroThirdPartyGlad)
        target_include_directories(HoroThirdPartyGlad
            PUBLIC
                ${CMAKE_CURRENT_LIST_DIR}/../vendor/glad/include
        )
    endif()

    add_library(HoroThirdPartyImGui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
    )

    if(HORO_ENABLE_IMGUI_UI_TESTS)
        target_sources(HoroThirdPartyImGui PRIVATE
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_capture_tool.cpp
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_context.cpp
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_coroutine.cpp
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_engine.cpp
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_exporters.cpp
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_perftool.cpp
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_ui.cpp
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine/imgui_te_utils.cpp
        )
        target_include_directories(HoroThirdPartyImGui PRIVATE
            ${imgui_test_engine_SOURCE_DIR}
            ${imgui_test_engine_SOURCE_DIR}/imgui_test_engine
        )
    endif()
    add_library(HoroThirdParty::ImGui ALIAS HoroThirdPartyImGui)

    target_compile_features(HoroThirdPartyImGui PUBLIC cxx_std_20)
    target_include_directories(HoroThirdPartyImGui
        PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
    )
    target_compile_definitions(HoroThirdPartyImGui
        PUBLIC
            GL_SILENCE_DEPRECATION
    )
    if(HORO_ENABLE_IMGUI_UI_TESTS)
        target_compile_definitions(HoroThirdPartyImGui
            PUBLIC
                IMGUI_ENABLE_TEST_ENGINE
                IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL=1
                IMGUI_TEST_ENGINE_ENABLE_STD_FUNCTION=1
        )
    endif()
    target_link_libraries(HoroThirdPartyImGui
        PUBLIC
            ${HORO_SDL3_TARGET}
    )

    if(HORO_BUILD_RENDER_OPENGL)
        add_library(HoroThirdPartyImGuiOpenGL STATIC
            ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        )
        add_library(HoroThirdParty::ImGuiOpenGL ALIAS HoroThirdPartyImGuiOpenGL)
        target_compile_features(HoroThirdPartyImGuiOpenGL PUBLIC cxx_std_20)
        target_include_directories(HoroThirdPartyImGuiOpenGL PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
        )
        target_compile_definitions(HoroThirdPartyImGuiOpenGL PUBLIC GL_SILENCE_DEPRECATION)
        target_link_libraries(HoroThirdPartyImGuiOpenGL PUBLIC HoroThirdPartyImGui OpenGL::GL)
    endif()

    if(HORO_BUILD_RENDER_METAL)
        add_library(HoroThirdPartyImGuiMetal STATIC
            ${imgui_SOURCE_DIR}/backends/imgui_impl_metal.mm
        )
        add_library(HoroThirdParty::ImGuiMetal ALIAS HoroThirdPartyImGuiMetal)
        target_compile_features(HoroThirdPartyImGuiMetal PUBLIC cxx_std_20)
        target_compile_options(HoroThirdPartyImGuiMetal PRIVATE -fobjc-arc)
        target_include_directories(HoroThirdPartyImGuiMetal PUBLIC
            ${imgui_SOURCE_DIR}
            ${imgui_SOURCE_DIR}/backends
        )
        target_link_libraries(HoroThirdPartyImGuiMetal PUBLIC
            HoroThirdPartyImGui
            "-framework Metal"
            "-framework QuartzCore"
        )
    endif()
endif()
