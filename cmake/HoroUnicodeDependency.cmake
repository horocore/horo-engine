include(FetchContent)

# Upstream's common manifest owns bidi, rule/dictionary break iteration and
# normalization. Keep the same source list across MSVC, Clang and GCC; no system
# ICU, host data generator, platform SDK or formatting library is selected.
function(horo_add_unicode_dependency)
    FetchContent_Declare(horo_icu
        URL "https://github.com/unicode-org/icu/releases/download/release-78.2/icu4c-78.2-sources.tgz"
        URL_HASH "SHA256=3e99687b5c435d4b209630e2d2ebb79906c984685e78635078b672e03c89df35"
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR .horo-source-only)
    FetchContent_MakeAvailable(horo_icu)
    file(SHA256 "${horo_icu_SOURCE_DIR}/LICENSE" license_digest)
    file(SHA256 "${horo_icu_SOURCE_DIR}/source/common/sources.txt" sources_digest)
    file(SHA256 "${horo_icu_SOURCE_DIR}/source/data/in/icudt78l.dat" data_digest)
    if(NOT license_digest STREQUAL "e55522d81edc687a341a4411e0776e54ca654e90147f354a90458aaced4116af"
       OR NOT sources_digest STREQUAL "c9b33a6ae1fb56b39eff6d364404582768e21ae2262674a6b65b5a11380e7405"
       OR NOT data_digest STREQUAL "a3d49eacd189624769dc77457c6aa1789a89219fb217a6d87d5b076c269aaf1f")
        message(FATAL_ERROR "Qualified ICU source, license or dictionary data changed")
    endif()
    if(NOT CMAKE_CXX_BYTE_ORDER STREQUAL "LITTLE_ENDIAN")
        message(FATAL_ERROR "Runtime UI Unicode requires the qualified little-endian ICU package")
    endif()
    file(STRINGS "${horo_icu_SOURCE_DIR}/source/common/sources.txt" common_sources)
    list(TRANSFORM common_sources PREPEND "${horo_icu_SOURCE_DIR}/source/common/")
    add_library(HoroThirdPartyUnicode STATIC ${common_sources}
        "${horo_icu_SOURCE_DIR}/source/stubdata/stubdata.cpp")
    add_library(HoroThirdParty::Unicode ALIAS HoroThirdPartyUnicode)
    target_compile_features(HoroThirdPartyUnicode PRIVATE cxx_std_20)
    set_target_properties(HoroThirdPartyUnicode PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(HoroThirdPartyUnicode SYSTEM PUBLIC "${horo_icu_SOURCE_DIR}/source/common")
    target_compile_definitions(HoroThirdPartyUnicode PUBLIC
        U_STATIC_IMPLEMENTATION U_LIB_SUFFIX_C_NAME=_horo
        UCONFIG_NO_FILE_IO=1 U_ENABLE_DYLOAD=0
        UCONFIG_NO_FORMATTING=1 UCONFIG_NO_COLLATION=1
        UCONFIG_NO_TRANSLITERATION=1 UCONFIG_NO_REGULAR_EXPRESSIONS=1
        UCONFIG_USE_ML_PHRASE_BREAKING=0)
    target_compile_definitions(HoroThirdPartyUnicode PRIVATE U_COMMON_IMPLEMENTATION)
    if(WIN32)
        target_compile_definitions(HoroThirdPartyUnicode PRIVATE NOMINMAX)
    else()
        find_package(Threads REQUIRED)
        target_link_libraries(HoroThirdPartyUnicode PRIVATE Threads::Threads)
    endif()
    install(FILES "${horo_icu_SOURCE_DIR}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/licenses" RENAME ICU.txt)
    install(FILES "${horo_icu_SOURCE_DIR}/source/data/in/icudt78l.dat"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/horo-engine/unicode")
    set(HORO_UNICODE_DATA_FILE "${horo_icu_SOURCE_DIR}/source/data/in/icudt78l.dat" PARENT_SCOPE)
endfunction()
