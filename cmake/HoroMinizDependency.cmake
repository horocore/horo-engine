include_guard(GLOBAL)
include(FetchContent)

function(horo_add_miniz_dependency)
    if(TARGET miniz)
        return()
    endif()
    set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(miniz
        GIT_REPOSITORY https://github.com/richgel999/miniz.git
        GIT_TAG 174573d60290f447c13a2b1b3405de2b96e27d6c
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(miniz)
    # miniz hard-codes /Zi, which requires a separate PDB when using cached MSVC compilation.
    if(MSVC AND CMAKE_C_COMPILER_LAUNCHER MATCHES "sccache")
        set_property(TARGET miniz PROPERTY C_COMPILER_LAUNCHER "")
    endif()
endfunction()
