# Shared by the native suite and the compiler-free tooling gate.
function(horo_add_protobuf_coverage_model name compiler version coverage excluded)
    add_test(NAME HoroProtobufCoverage${name}
        COMMAND "${CMAKE_COMMAND}"
            -S "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/protobuf_coverage/model"
            -B "${CMAKE_CURRENT_BINARY_DIR}/protobuf-coverage/${name}"
            -G "${CMAKE_GENERATOR}"
            "-DHORO_ENGINE_SOURCE_DIR=${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.."
            "-DHORO_MODEL_COMPILER=${compiler}"
            "-DHORO_MODEL_VERSION=${version}"
            "-DHORO_MODEL_COVERAGE=${coverage}"
            "-DHORO_MODEL_EXCLUDED=${excluded}")
    set_tests_properties(HoroProtobufCoverage${name} PROPERTIES
        LABELS "cmake;ci-windows" TIMEOUT 30)
endfunction()

horo_add_protobuf_coverage_model(GNU15Coverage GNU 15.2.0 ON ON)
horo_add_protobuf_coverage_model(GNU14Coverage GNU 14.3.0 ON OFF)
horo_add_protobuf_coverage_model(GNU15NoCoverage GNU 15.2.0 OFF OFF)
horo_add_protobuf_coverage_model(ClangCoverage Clang 20.1.0 ON OFF)
horo_add_protobuf_coverage_model(AppleClangCoverage AppleClang 17.0.0 ON OFF)
horo_add_protobuf_coverage_model(MsvcCoverage MSVC 19.44 ON OFF)
