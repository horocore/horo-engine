include_guard(GLOBAL)

function(horo_configure_extension_sdk)
    set(package_parent "${CMAKE_BINARY_DIR}/sdk/extensions")
    cmake_path(NORMAL_PATH package_parent)
    set(package_root "${package_parent}/${HORO_EXTENSION_SDK_VERSION}")
    cmake_path(NORMAL_PATH package_root)
    cmake_path(IS_PREFIX package_parent "${package_root}" NORMALIZE
        package_root_is_versioned)
    if(NOT package_root_is_versioned OR package_root STREQUAL package_parent)
        message(FATAL_ERROR
            "Extension SDK package root must be a versioned directory under ${package_parent}")
    endif()

    set(package_cmake_dir
        "${package_root}/lib/cmake/HoroEngineExtensionSdk")

    # Recreate only this version's package so a reused build tree cannot retain
    # files removed or renamed by a newer SDK configuration.
    file(REMOVE_RECURSE "${package_root}")
    file(MAKE_DIRECTORY
        "${package_root}/bin"
        "${package_root}/include/Horo/Extensions"
        "${package_root}/share/horo/extension-sdk"
        "${package_root}/share/horo/extension-sdk/ci"
        "${package_cmake_dir}")

    configure_file(
        "${PROJECT_SOURCE_DIR}/include/Horo/Extensions/ExtensionAbi.h"
        "${package_root}/include/Horo/Extensions/ExtensionAbi.h"
        COPYONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/LICENSE"
        "${package_root}/LICENSE"
        COPYONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/scripts/scaffold_extension.py"
        "${package_root}/bin/horo-scaffold-extension.py"
        COPYONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/scripts/run_extension_author_ci.py"
        "${package_root}/bin/horo-extension-author-ci.py"
        COPYONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/scripts/bootstrap_extension_ci.py"
        "${package_root}/share/horo/extension-sdk/ci/bootstrap.py"
        COPYONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/sdk/ci/extension-author-ci.yml"
        "${package_root}/share/horo/extension-sdk/ci/extension-author-ci.yml"
        COPYONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/sdk/ci/extension-ci.lock.json.in"
        "${package_root}/share/horo/extension-sdk/ci/extension-ci.lock.json"
        @ONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/sdk/schemas/extension-manifest-v1.schema.json"
        "${package_root}/share/horo/extension-sdk/extension-manifest-v1.schema.json"
        COPYONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/sdk/extension-sdk.json.in"
        "${package_root}/share/horo/extension-sdk/extension-sdk.json"
        @ONLY)
    configure_file(
        "${PROJECT_SOURCE_DIR}/sdk/ExtensionSdkREADME.md.in"
        "${package_root}/README.md"
        @ONLY)

    configure_package_config_file(
        "${PROJECT_SOURCE_DIR}/sdk/cmake/HoroEngineExtensionSdkConfig.cmake.in"
        "${package_cmake_dir}/HoroEngineExtensionSdkConfig.cmake"
        INSTALL_DESTINATION "lib/cmake/HoroEngineExtensionSdk"
        NO_SET_AND_CHECK_MACRO)
    write_basic_package_version_file(
        "${package_cmake_dir}/HoroEngineExtensionSdkConfigVersion.cmake"
        VERSION "${HORO_EXTENSION_SDK_VERSION}"
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT)

    set(HORO_EXTENSION_SDK_PACKAGE_DIR "${package_root}" CACHE INTERNAL
        "Self-contained extension C ABI SDK package")

    install(DIRECTORY "${package_root}/"
        DESTINATION
            "${CMAKE_INSTALL_DATADIR}/horo/sdk/extensions/${HORO_EXTENSION_SDK_VERSION}"
        COMPONENT HoroExtensionSdk)
endfunction()
