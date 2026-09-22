include_guard(GLOBAL)

include(GNUInstallDirs)

set(HORO_TARGET_INCLUDE_ROOT "${CMAKE_BINARY_DIR}/target-includes" CACHE INTERNAL
    "Per-target build-tree include views")
set(HORO_PUBLIC_HEADER_EXTENSIONS h hh hpp hxx inl ipp tpp)

function(horo_configure_target_header_boundary target)
    cmake_parse_arguments(ARG "" "" "PUBLIC_HEADERS" ${ARGN})
    if(ARG_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "Unknown arguments for ${target}: ${ARG_UNPARSED_ARGUMENTS}")
    endif()

    # Ownership is configuration-independent. Optional targets still own their
    # contracts when a headless or backend-limited profile omits the target.
    foreach(header IN LISTS ARG_PUBLIC_HEADERS)
        if(NOT header MATCHES "^Horo/.+\\.(h|hh|hpp|hxx|inl|ipp|tpp)$")
            message(FATAL_ERROR
                "Public header '${header}' for ${target} must be relative to include/ and use a supported header extension")
        endif()
        if(NOT EXISTS "${PROJECT_SOURCE_DIR}/include/${header}")
            message(FATAL_ERROR "Registered public header does not exist: include/${header}")
        endif()

        string(MAKE_C_IDENTIFIER "${header}" header_key)
        get_property(existing_owner GLOBAL PROPERTY "HORO_PUBLIC_HEADER_OWNER_${header_key}")
        if(existing_owner)
            message(FATAL_ERROR
                "Public header '${header}' is owned by both ${existing_owner} and ${target}")
        endif()
        set_property(GLOBAL PROPERTY "HORO_PUBLIC_HEADER_OWNER_${header_key}" "${target}")
    endforeach()

    if(NOT TARGET "${target}")
        return()
    endif()

    get_target_property(target_type "${target}" TYPE)
    get_target_property(public_includes "${target}" INTERFACE_INCLUDE_DIRECTORIES)
    if(public_includes AND NOT public_includes STREQUAL "public_includes-NOTFOUND")
        list(REMOVE_ITEM public_includes
            "${PROJECT_SOURCE_DIR}/include"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/src/"
            "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        )
        set_property(TARGET "${target}" PROPERTY INTERFACE_INCLUDE_DIRECTORIES "${public_includes}")
    endif()

    if(NOT target_type STREQUAL "INTERFACE_LIBRARY")
        get_target_property(private_includes "${target}" INCLUDE_DIRECTORIES)
        if(private_includes AND NOT private_includes STREQUAL "private_includes-NOTFOUND")
            list(REMOVE_ITEM private_includes
                "${PROJECT_SOURCE_DIR}/include"
                "${PROJECT_SOURCE_DIR}/src"
                "${PROJECT_SOURCE_DIR}/src/"
                "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
            )
            set_property(TARGET "${target}" PROPERTY INCLUDE_DIRECTORIES "${private_includes}")
        endif()
        # Implementation headers remain source-tree-private. They are never a
        # usage requirement and therefore cannot leak to target consumers.
        target_include_directories("${target}" PRIVATE "${PROJECT_SOURCE_DIR}/src")
    endif()

    set(stage_root "${HORO_TARGET_INCLUDE_ROOT}/${target}/public")
    # Remove only headers that belonged to this target in an earlier
    # configuration. Ownership can move between targets while an existing
    # build directory is reused; leaving the old staged file would let
    # include-order shadow the current owning target's header. Preserve current
    # staged files so a reconfigure does not force an unnecessary rebuild.
    set(staged_header_patterns)
    foreach(extension IN LISTS HORO_PUBLIC_HEADER_EXTENSIONS)
        list(APPEND staged_header_patterns "${stage_root}/*.${extension}")
    endforeach()
    file(GLOB_RECURSE staged_headers RELATIVE "${stage_root}" ${staged_header_patterns})
    foreach(staged_header IN LISTS staged_headers)
        list(FIND ARG_PUBLIC_HEADERS "${staged_header}" owned_header_index)
        if(owned_header_index EQUAL -1)
            file(REMOVE "${stage_root}/${staged_header}")
        endif()
    endforeach()

    foreach(header IN LISTS ARG_PUBLIC_HEADERS)
        get_filename_component(header_directory "${stage_root}/${header}" DIRECTORY)
        file(MAKE_DIRECTORY "${header_directory}")
        configure_file(
            "${PROJECT_SOURCE_DIR}/include/${header}"
            "${stage_root}/${header}"
            COPYONLY
        )
    endforeach()

    if(target_type STREQUAL "INTERFACE_LIBRARY")
        target_include_directories("${target}" INTERFACE
            "$<BUILD_INTERFACE:${stage_root}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    else()
        target_include_directories("${target}" PUBLIC
            "$<BUILD_INTERFACE:${stage_root}>"
            "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
    endif()

    set_property(TARGET "${target}" PROPERTY HORO_PUBLIC_HEADERS "${ARG_PUBLIC_HEADERS}")
    set_property(GLOBAL APPEND PROPERTY HORO_HEADER_BOUNDARY_TARGETS "${target}")
endfunction()

function(horo_verify_public_header_inventory)
    set(repository_header_globs)
    foreach(extension IN LISTS HORO_PUBLIC_HEADER_EXTENSIONS)
        list(APPEND repository_header_globs
            "${PROJECT_SOURCE_DIR}/include/Horo/*.${extension}")
    endforeach()
    file(GLOB_RECURSE repository_headers
        CONFIGURE_DEPENDS
        RELATIVE "${PROJECT_SOURCE_DIR}/include"
        ${repository_header_globs})

    set(unowned_headers)
    foreach(header IN LISTS repository_headers)
        string(MAKE_C_IDENTIFIER "${header}" header_key)
        get_property(owner GLOBAL PROPERTY "HORO_PUBLIC_HEADER_OWNER_${header_key}")
        if(NOT owner)
            list(APPEND unowned_headers "${header}")
        endif()
    endforeach()

    if(unowned_headers)
        list(JOIN unowned_headers "\n  " formatted_headers)
        message(FATAL_ERROR
            "Public headers must have exactly one owning target. Unowned headers:\n  ${formatted_headers}")
    endif()

    get_property(boundary_targets GLOBAL PROPERTY HORO_HEADER_BOUNDARY_TARGETS)
    foreach(target IN LISTS boundary_targets)
        get_target_property(public_includes "${target}" INTERFACE_INCLUDE_DIRECTORIES)
        foreach(include_directory IN LISTS public_includes)
            if(include_directory STREQUAL "${PROJECT_SOURCE_DIR}/include"
                    OR include_directory STREQUAL "${PROJECT_SOURCE_DIR}/src"
                    OR include_directory STREQUAL "${PROJECT_SOURCE_DIR}/src/"
                    OR include_directory STREQUAL "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
                    OR include_directory STREQUAL "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>")
                message(FATAL_ERROR
                    "${target} publishes repository-wide include path '${include_directory}'. "
                    "Register its owned headers instead.")
            endif()
        endforeach()
    endforeach()
endfunction()

function(horo_add_public_header_consumer_targets)
    get_property(boundary_targets GLOBAL PROPERTY HORO_HEADER_BOUNDARY_TARGETS)
    foreach(target IN LISTS boundary_targets)
        get_target_property(headers "${target}" HORO_PUBLIC_HEADERS)
        if(NOT headers OR headers STREQUAL "headers-NOTFOUND")
            continue()
        endif()

        set(generated_sources)
        foreach(header IN LISTS headers)
            string(MAKE_C_IDENTIFIER "${header}" source_name)
            set(source "${CMAKE_CURRENT_BINARY_DIR}/public_header_consumers/${target}/${source_name}.cpp")
            file(GENERATE OUTPUT "${source}" CONTENT "#include <${header}>\n")
            list(APPEND generated_sources "${source}")
        endforeach()

        set(consumer_target "${target}PublicHeaderConsumer")
        add_library("${consumer_target}" OBJECT ${generated_sources})
        target_compile_features("${consumer_target}" PRIVATE cxx_std_20)
        if(MSVC)
            # Header consumers do not link and need no shared compiler PDB.
            # Embedded debug records also keep independent translation units
            # deterministic when several consumer targets build concurrently.
            target_compile_options("${consumer_target}" PRIVATE "$<$<CONFIG:Debug>:/Z7>")
        endif()
        target_link_libraries("${consumer_target}" PRIVATE "${target}")
        set_target_properties("${consumer_target}" PROPERTIES FOLDER "Tests/Header Boundaries")
    endforeach()
endfunction()
