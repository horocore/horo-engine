include_guard(GLOBAL)

# Hash actual source contents, not the Git revision or a machine-specific path.
# CONFIGURE_DEPENDS detects additions; explicit content dependencies also detect edits.
function(horo_configure_script_module_identity target archive_definition archive_sha)
    file(GLOB_RECURSE inputs CONFIGURE_DEPENDS RELATIVE "${PROJECT_SOURCE_DIR}"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/*.h"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/*.cpp"
        "${PROJECT_SOURCE_DIR}/include/Horo/Extensions/*.h")
    list(APPEND inputs
        src/extensions/registry/BackendServiceRegistry.cpp
        src/extensions/ExtensionAuthorityIdentityValidation.h
        src/extensions/host/ModuleScriptHostBridge.h
        src/extensions/host/ModuleScriptHostBridge.cpp
        src/extensions/host/ModuleScriptNativePort.cpp
        src/extensions/host/ModuleScriptActiveArtifacts.cpp
        src/extensions/host/NativeExtensionServiceCalls.cpp
        src/extensions/host/NativeExtensionServicePublication.cpp
        src/extensions/host/NativeExtensionScriptSession.cpp)
    list(SORT inputs)
    set(source_identity "horo.module-binding-source.v1\n")
    foreach(input IN LISTS inputs)
        set(path "${PROJECT_SOURCE_DIR}/${input}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(SHA256 "${path}" hash)
        string(APPEND source_identity "${input}:${hash}\n")
    endforeach()
    string(SHA256 binding_sha "${source_identity}")

    set(profile "horo.module-build-profile.v1")
    foreach(variable IN ITEMS CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_SYSTEM_PROCESSOR
            CMAKE_CXX_COMPILER_ID CMAKE_CXX_COMPILER_VERSION CMAKE_CXX_COMPILER_TARGET
            CMAKE_CXX_COMPILER_FRONTEND_VARIANT CMAKE_CXX_FLAGS CMAKE_OSX_ARCHITECTURES
            CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT CMAKE_SYSROOT
            CMAKE_MSVC_RUNTIME_LIBRARY CMAKE_INTERPROCEDURAL_OPTIMIZATION)
        string(APPEND profile "|${variable}=${${variable}}")
    endforeach()
    foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        string(APPEND profile "|${config}=${CMAKE_CXX_FLAGS_${config}}")
    endforeach()
    string(SHA256 profile_sha "${profile}")
    target_compile_definitions(${target} PRIVATE
        "${archive_definition}=\"${archive_sha}\""
        "HORO_SCRIPT_MODULE_BINDING_SHA256=\"${binding_sha}\""
        "HORO_SCRIPT_MODULE_BUILD_PROFILE=\"${profile_sha}:$<CONFIG>\"")
endfunction()
