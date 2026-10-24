# GCC 15 recognizes the pinned protobuf's clang::musttail attribute, but arc
# instrumentation prevents some required parser tail calls. These two private
# vendor implementations are outside Sonar's source/coverage ownership. Keep
# their parser unchanged and exclude only their own compilations from profiling;
# PRIVATE options must not reach the engine or its tests through protobuf aliases.
if(HORO_CI_SONAR_COVERAGE
   AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 15)
    foreach(target libprotobuf libprotobuf-lite)
        target_compile_options(${target} PRIVATE "-fprofile-exclude-files=.*")
    endforeach()
endif()
