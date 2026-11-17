if(NOT HORO_ENGINE_SOURCE_DIR OR NOT HORO_TEST_BINARY_DIR)
    message(FATAL_ERROR "HORO_ENGINE_SOURCE_DIR and HORO_TEST_BINARY_DIR are required")
endif()

function(cli_policy_case root forbidden)
    set(target "${forbidden}")
    if(forbidden STREQUAL "none")
        set(target "")
    endif()
    string(REPLACE ":" "_" case_name "${root}-${forbidden}")
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${HORO_ENGINE_SOURCE_DIR}/tests/cmake/cli_headless_policy"
        -B "${HORO_TEST_BINARY_DIR}/${case_name}"
        "-DHORO_ENGINE_SOURCE_DIR=${HORO_ENGINE_SOURCE_DIR}" "-DHORO_TEST_ROOT=${root}"
        "-DHORO_FORBIDDEN_TARGET=${target}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(forbidden STREQUAL "none")
        if(NOT result EQUAL 0)
            message(FATAL_ERROR "Neutral ${root} dependencies were rejected: ${output}\n${error}")
        endif()
    elseif(result EQUAL 0 OR NOT "${output}\n${error}" MATCHES "CLI headless dependency violation:")
        message(FATAL_ERROR "Forbidden ${root} dependency ${forbidden} was not rejected: ${output}\n${error}")
    endif()
endfunction()

foreach(forbidden none HoroGui HoroEditor HoroRenderOpenGL HoroRenderMetal HoroRenderVulkan HoroRenderD3D12 HoroThirdPartyImGui imgui glfw OpenGL::GL Vulkan::Vulkan)
    cli_policy_case(host "${forbidden}")
endforeach()

foreach(forbidden none HoroRenderApi HoroRenderFrontend HoroRenderNull HoroRuntimeScene HoroGameplayRuntime HoroPhysics HoroAudio HoroNetworking HoroMcpController)
    cli_policy_case(horopak "${forbidden}")
endforeach()
