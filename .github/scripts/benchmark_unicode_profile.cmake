# One-off diagnostic injection; this branch is never merged.
function(horo_benchmark_unicode_profile)
    if(NOT HORO_CI_SONAR_COVERAGE OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "This experiment requires the actual GCC Sonar configuration")
    endif()
    if(HORO_BENCHMARK_UNICODE_EXCLUSION)
        target_compile_options(HoroThirdPartyUnicode PRIVATE "-fprofile-exclude-files=.*")
    endif()
endfunction()

cmake_language(DEFER CALL horo_benchmark_unicode_profile)
