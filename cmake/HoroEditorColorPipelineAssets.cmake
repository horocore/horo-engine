include_guard(GLOBAL)

set(HORO_EDITOR_COLOR_PIPELINE_ROOT "" CACHE PATH
    "Explicit offline-cooked color/exposure package root to stage with HoroEditor")

# Structural packaging admission only. The runtime data decoder owns complete
# native/analytic generation validation; these checks do not establish cook trust.
function(horo_check_color_package_file root relative)
    set(source "${root}/${relative}")
    if(NOT EXISTS "${source}" OR IS_DIRECTORY "${source}" OR IS_SYMLINK "${source}")
        message(FATAL_ERROR "Color package requires an ordinary file: ${source}")
    endif()
    file(REAL_PATH "${root}" canonical_root)
    file(REAL_PATH "${source}" canonical_source)
    cmake_path(IS_PREFIX canonical_root "${canonical_source}" NORMALIZE within_root)
    if(NOT within_root)
        message(FATAL_ERROR "Color package file escapes its explicit root: ${relative}")
    endif()
    file(SIZE "${source}" bytes)
    if(bytes EQUAL 0 OR bytes GREATER 33554432)
        message(FATAL_ERROR "Color package file has an invalid byte extent: ${relative}")
    endif()
endfunction()

function(horo_stage_color_package_file root relative install_root)
    horo_check_color_package_file("${root}" "${relative}")
    get_filename_component(parent "${relative}" DIRECTORY)
    add_custom_command(TARGET HoroEditor POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${HORO_EDITOR_BUILD_ASSET_ROOT}/color-pipeline/${parent}"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${root}/${relative}" "${HORO_EDITOR_BUILD_ASSET_ROOT}/color-pipeline/${relative}"
        VERBATIM)
    install(FILES "${root}/${relative}"
        DESTINATION "${install_root}/color-pipeline/${parent}" COMPONENT Editor)
endfunction()

function(horo_stage_editor_color_pipeline)
    if(NOT TARGET HoroEditor OR HORO_EDITOR_COLOR_PIPELINE_ROOT STREQUAL "")
        return()
    endif()
    set(root "${HORO_EDITOR_COLOR_PIPELINE_ROOT}")
    if(NOT IS_ABSOLUTE "${root}" OR NOT IS_DIRECTORY "${root}" OR IS_SYMLINK "${root}")
        message(FATAL_ERROR "HORO_EDITOR_COLOR_PIPELINE_ROOT must be an absolute ordinary package directory")
    endif()
    set(files
        color/analytic/manifest.json color/analytic/processor.hlsl color/analytic/reference.json
        color/analytic/textures/000.source.f32le color/analytic/textures/000.upload.f32le
        color/analytic/textures/001.source.f32le color/analytic/textures/001.upload.f32le
        color/color-output-cook-provenance.json color/color-output-generation.json
        color/shader-host-catalog.json color/shader-host-catalog.signature.json
        color/shaders/runtime/color-output-scene-metal.horoshdr
        color/shaders/runtime/color-output-scene-metal.reflection.json
        color/shaders/runtime/color-output-encode-metal.horoshdr
        color/shaders/runtime/color-output-encode-metal.reflection.json
        color/sources/color_output.combined.hlsl color/sources/color_output.wrapper.hlsl
        color/sources/color_encode_sdr.hlsl
        exposure/exposure-cook-provenance.json exposure/shader-host-catalog.json
        exposure/shader-host-catalog.signature.json)
    foreach(stage clear histogram adapt)
        list(APPEND files "exposure/shaders/runtime/exposure-${stage}-metal.horoshdr"
            "exposure/shaders/runtime/exposure-${stage}-metal.reflection.json")
    endforeach()
    horo_check_color_package_file("${root}" color/color-output-generation.json)
    file(READ "${root}/color/color-output-generation.json" generation LIMIT 65537)
    string(JSON schema ERROR_VARIABLE schema_error GET "${generation}" schemaVersion)
    if(schema_error OR NOT schema MATCHES "^[12]$")
        message(FATAL_ERROR "Color package generation must declare supported schemaVersion 1 or 2")
    endif()
    if(schema EQUAL 2)
        list(APPEND files color/shaders/runtime/color-output-look-metal.horoshdr
            color/shaders/runtime/color-output-look-metal.reflection.json
            color/sources/color_output_look.combined.hlsl color/sources/color_output_look.wrapper.hlsl)
    endif()
    if(APPLE)
        set(install_root "HoroEditor.app/Contents/Resources/assets")
    else()
        set(install_root "${HORO_EDITOR_ASSET_INSTALL_DIR}")
    endif()
    foreach(relative IN LISTS files)
        horo_stage_color_package_file("${root}" "${relative}" "${install_root}")
    endforeach()
endfunction()
