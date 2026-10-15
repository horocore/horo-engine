include(CMakeParseArguments)

if(NOT HORO_GAMEPLAY_MODULE_CMAKE_DIR)
    set(HORO_GAMEPLAY_MODULE_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}")
endif()
if(NOT HORO_GAMEPLAY_CODEGEN_SCRIPT)
    get_filename_component(HORO_GAMEPLAY_ENGINE_ROOT "${HORO_GAMEPLAY_MODULE_CMAKE_DIR}" DIRECTORY)
    set(HORO_GAMEPLAY_CODEGEN_SCRIPT "${HORO_GAMEPLAY_ENGINE_ROOT}/scripts/generate_gameplay_behavior_bundle.py")
endif()
set_property(GLOBAL PROPERTY HORO_GAMEPLAY_MODULE_CMAKE_DIR "${HORO_GAMEPLAY_MODULE_CMAKE_DIR}")
set_property(GLOBAL PROPERTY HORO_GAMEPLAY_CODEGEN_SCRIPT "${HORO_GAMEPLAY_CODEGEN_SCRIPT}")
set_property(GLOBAL PROPERTY HORO_GAMEPLAY_PYTHON_EXECUTABLE "${Python3_EXECUTABLE}")
set_property(GLOBAL PROPERTY HORO_GAMEPLAY_SDK_FINGERPRINT "${HORO_GAMEPLAY_SDK_FINGERPRINT}")

# Adds one primary project gameplay dynamic library with a generated complete
# HORO_BEHAVIOR descriptor bundle. INPUT_MANIFEST_OUTPUT optionally gives an
# additional target its own freshness metadata path; the primary project default
# remains .horo/local/gameplay_build_inputs.txt. NO_MANIFEST only suppresses the
# runtime artifact JSON, never descriptor/fingerprint or input evidence.
function(horo_add_gameplay_module target)
    get_property(gameplay_module_cmake_dir GLOBAL PROPERTY HORO_GAMEPLAY_MODULE_CMAKE_DIR)
    get_property(gameplay_codegen_script GLOBAL PROPERTY HORO_GAMEPLAY_CODEGEN_SCRIPT)
    get_property(gameplay_python_executable GLOBAL PROPERTY HORO_GAMEPLAY_PYTHON_EXECUTABLE)
    get_property(gameplay_sdk_fingerprint GLOBAL PROPERTY HORO_GAMEPLAY_SDK_FINGERPRINT)
    cmake_parse_arguments(ARG "NO_MANIFEST" "MODULE_ID;BUILD_FINGERPRINT;MANIFEST_OUTPUT;INPUT_MANIFEST_OUTPUT" "SOURCES;INPUTS" ${ARGN})
    if(NOT ARG_MODULE_ID OR NOT ARG_SOURCES)
        message(FATAL_ERROR "horo_add_gameplay_module requires MODULE_ID and SOURCES")
    endif()
    if(NOT ARG_BUILD_FINGERPRINT)
        set(ARG_BUILD_FINGERPRINT "${gameplay_sdk_fingerprint}")
    endif()

    set(gameplay_sources)
    foreach(source IN LISTS ARG_SOURCES)
        if(IS_ABSOLUTE "${source}")
            list(APPEND gameplay_sources "${source}")
        else()
            list(APPEND gameplay_sources "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
        endif()
    endforeach()

    set(gameplay_inputs)
    foreach(input IN LISTS ARG_INPUTS)
        if(IS_ABSOLUTE "${input}")
            set(absolute_input "${input}")
        else()
            set(absolute_input "${CMAKE_CURRENT_SOURCE_DIR}/${input}")
        endif()
        cmake_path(NORMAL_PATH absolute_input)
        file(REAL_PATH "${absolute_input}" canonical_input BASE_DIRECTORY "${CMAKE_SOURCE_DIR}")
        file(REAL_PATH "${CMAKE_SOURCE_DIR}" canonical_project_root)
        cmake_path(IS_PREFIX canonical_project_root "${canonical_input}" NORMALIZE input_is_project_owned)
        if(NOT input_is_project_owned)
            message(FATAL_ERROR "Gameplay module INPUTS must remain inside the project root: ${input}")
        endif()
        list(APPEND gameplay_inputs "${canonical_input}")
    endforeach()

    set(generated "${CMAKE_CURRENT_BINARY_DIR}/${target}_generated_behavior_bundle.cpp")
    set(generated_revision "${CMAKE_CURRENT_BINARY_DIR}/${target}_generated_behavior_bundle.revision")
    add_custom_command(
        OUTPUT "${generated}" "${generated_revision}"
        COMMAND "${gameplay_python_executable}" "${gameplay_codegen_script}"
                --output "${generated}"
                --revision-output "${generated_revision}"
                --output-root "${CMAKE_CURRENT_BINARY_DIR}"
                --module-id "${ARG_MODULE_ID}"
                --fingerprint "${ARG_BUILD_FINGERPRINT}"
                ${gameplay_sources}
        DEPENDS ${gameplay_sources} ${gameplay_inputs} "${gameplay_codegen_script}"
        VERBATIM
        COMMENT "Generating complete gameplay behavior bundle for ${target}"
    )
    add_library(${target} SHARED ${gameplay_sources} "${generated}")
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_link_libraries(${target} PRIVATE HoroEngine::GameplayApi)
    if(HORO_GAMEPLAY_SDK_USES_COVERAGE)
        target_compile_options(${target} PRIVATE --coverage)
        target_link_options(${target} PRIVATE --coverage)
    endif()

    if(ARG_INPUT_MANIFEST_OUTPUT)
        set(input_manifest "${ARG_INPUT_MANIFEST_OUTPUT}")
    else()
        set(input_manifest "${CMAKE_SOURCE_DIR}/.horo/local/gameplay_build_inputs.txt")
    endif()
    set(input_manifest_contents "")
    foreach(input IN LISTS gameplay_sources gameplay_inputs)
        if(IS_ABSOLUTE "${input}")
            file(RELATIVE_PATH relative_input "${CMAKE_SOURCE_DIR}" "${input}")
        else()
            set(relative_input "${input}")
        endif()
        string(APPEND input_manifest_contents "${relative_input}\n")
    endforeach()
    file(GENERATE OUTPUT "${input_manifest}" CONTENT "${input_manifest_contents}")

    if(NOT ARG_NO_MANIFEST)
        if(NOT ARG_MANIFEST_OUTPUT)
            if(HORO_GAMEPLAY_MANIFEST_OUTPUT)
                set(ARG_MANIFEST_OUTPUT "${HORO_GAMEPLAY_MANIFEST_OUTPUT}")
            else()
                set(ARG_MANIFEST_OUTPUT "${CMAKE_SOURCE_DIR}/.horo/local/gameplay_module.json")
            endif()
        endif()
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}"
                    "-DMANIFEST_OUTPUT=${ARG_MANIFEST_OUTPUT}"
                    "-DMODULE_ID=${ARG_MODULE_ID}"
                    "-DBUILD_FINGERPRINT=${ARG_BUILD_FINGERPRINT}"
                    "-DMODULE_PATH=$<TARGET_FILE:${target}>"
                    "-DDESCRIPTOR_REVISION_FILE=${generated_revision}"
                    -P "${gameplay_module_cmake_dir}/WriteGameplayModuleManifest.cmake"
            VERBATIM
            COMMENT "Publishing gameplay module artifact manifest for ${target}"
        )
    endif()
    set_property(TARGET ${target} PROPERTY HORO_GAMEPLAY_DESCRIPTOR_REVISION_FILE "${generated_revision}")
endfunction()
