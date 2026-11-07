include_guard(GLOBAL)

# Called from src/ after HoroExtensions exists and before public ownership is applied.
# Concrete VM code stays above the language-neutral extension authority.
include(HoroQuickJsDependency)
include(HoroScriptModuleIdentity)

function(horo_stage_module_script_contract target)
    add_library(${target} INTERFACE)
    set(stage "${CMAKE_CURRENT_BINARY_DIR}/${target}-headers")
    foreach(header IN LISTS ARGN)
        get_filename_component(name "${header}" NAME)
        configure_file("${PROJECT_SOURCE_DIR}/${header}" "${stage}/${name}" COPYONLY)
    endforeach()
    target_include_directories(${target} INTERFACE "$<BUILD_INTERFACE:${stage}>")
endfunction()

function(horo_add_script_module_runtime_targets)
    if(NOT TARGET HoroExtensions OR NOT TARGET HoroThirdParty::Lua)
        message(FATAL_ERROR "Script module runtimes require the real extension and Lua owners")
    endif()

    horo_stage_module_script_contract(HoroModuleScriptHostBridgeInternal
        src/extensions/host/ModuleScriptHostBridge.h)
    target_link_libraries(HoroModuleScriptHostBridgeInternal INTERFACE HoroEngine::Extensions)
    horo_stage_module_script_contract(HoroScriptAuthorityIdentityInternal
        src/extensions/ExtensionAuthorityIdentityValidation.h)

    target_sources(HoroExtensions PRIVATE
        "${PROJECT_SOURCE_DIR}/src/extensions/host/ModuleScriptHostBridge.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/host/ModuleScriptNativePort.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/host/ModuleScriptActiveArtifacts.cpp")

    add_library(HoroScriptModuleRuntime STATIC
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptRuntimeBackendBinding.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleImports.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleBindingCache.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleContext.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleCalls.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleReplacement.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleRebind.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleRecovery.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleProjection.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleNativeBridge.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleProgram.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleActiveCapture.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleReferenceDescriptor.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleReferenceService.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleReferenceGraph.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleReferenceInstallation.cpp"
    )
    target_compile_features(HoroScriptModuleRuntime PUBLIC cxx_std_20)
    add_library(HoroEngine::ScriptModuleRuntime ALIAS HoroScriptModuleRuntime)

    add_library(HoroLuaScriptModuleRuntime STATIC
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaScriptModuleRuntime.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaModuleState.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaModuleValueRead.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaModuleValueWrite.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaModuleImports.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaModuleCalls.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaModuleOutcomes.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaModuleExecution.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/LuaModuleRebind.cpp"
    )
    target_compile_features(HoroLuaScriptModuleRuntime PUBLIC cxx_std_20)
    add_library(HoroEngine::LuaScriptModuleRuntime ALIAS HoroLuaScriptModuleRuntime)

    add_library(HoroJavaScriptModuleRuntime STATIC
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleRuntime.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleState.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleValueProperties.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleValueRead.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleValueWrite.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleImports.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleOutcomes.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleExecution.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/JavaScriptModuleRebind.cpp"
    )
    target_compile_features(HoroJavaScriptModuleRuntime PUBLIC cxx_std_20)
    add_library(HoroEngine::JavaScriptModuleRuntime ALIAS HoroJavaScriptModuleRuntime)

    add_library(HoroScriptModuleHost STATIC
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleHostPolicy.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleHostServices.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleOperation.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleHostRebind.cpp"
        "${PROJECT_SOURCE_DIR}/src/extensions/script/ScriptModuleProviderRefresh.cpp"
    )
    target_compile_features(HoroScriptModuleHost PUBLIC cxx_std_20)

    target_link_libraries(HoroScriptModuleRuntime PUBLIC HoroEngine::Extensions
        PRIVATE HoroModuleScriptHostBridgeInternal HoroScriptAuthorityIdentityInternal)

    # Each staged header is an intentional non-installed contract; never export src/.
    horo_stage_module_script_contract(HoroScriptModuleRuntimeInternal
        src/extensions/script/ScriptModuleProgram.h)
    target_link_libraries(HoroScriptModuleRuntimeInternal INTERFACE HoroScriptModuleRuntime)
    horo_stage_module_script_contract(HoroScriptModuleRebindInternal
        src/extensions/script/ScriptModuleRebindInternal.h)
    target_link_libraries(HoroScriptModuleRebindInternal INTERFACE HoroScriptModuleRuntime)
    target_link_libraries(HoroLuaScriptModuleRuntime PUBLIC HoroScriptModuleRuntime
        PRIVATE HoroThirdParty::Lua HoroScriptModuleRebindInternal)
    target_link_libraries(HoroJavaScriptModuleRuntime PUBLIC HoroScriptModuleRuntime
        PRIVATE HoroThirdParty::QuickJS HoroScriptModuleRebindInternal)
    target_link_libraries(HoroScriptModuleHost PRIVATE HoroScriptModuleRuntimeInternal
        HoroModuleScriptHostBridgeInternal HoroLuaScriptModuleRuntime HoroJavaScriptModuleRuntime)

    horo_stage_module_script_contract(HoroScriptModuleHostInternal
        src/extensions/script/ScriptModuleHostServices.h
        src/extensions/script/ScriptModuleProgram.h)
    target_link_libraries(HoroScriptModuleHostInternal INTERFACE
        HoroScriptModuleHost HoroScriptModuleRuntime)

    horo_configure_script_module_identity(HoroLuaScriptModuleRuntime
        HORO_LUA_SCRIPT_SOURCE_SHA256
        4f18ddae154e793e46eeab727c59ef1c0c0c2b744e7b94219710d76f530629ae)
    horo_configure_script_module_identity(HoroJavaScriptModuleRuntime
        HORO_QUICKJS_SCRIPT_SOURCE_SHA256
        a49defe70cbcae9226d9fac025af116e87ebae2440dcff1ca3c80b79e1cdef0b)
endfunction()

