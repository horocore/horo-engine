include_guard(GLOBAL)

# Application composition only: include after HoroHostSignatureDocuments is declared.
function(horo_add_module_script_package_target)
    add_library(HoroModuleScriptPackage STATIC
        "${PROJECT_SOURCE_DIR}/apps/common/module_scripts/ModuleScriptAuthentication.cpp"
        "${PROJECT_SOURCE_DIR}/apps/common/module_scripts/ModuleScriptSnapshot.cpp"
        "${PROJECT_SOURCE_DIR}/apps/common/module_scripts/ModuleScriptProgramCapture.cpp")
    target_compile_features(HoroModuleScriptPackage PUBLIC cxx_std_20)
    target_link_libraries(HoroModuleScriptPackage PRIVATE HoroScriptModuleRuntimeInternal
        HoroHostSignatureDocuments HoroEngine::Platform HoroEngine::Security)
    horo_stage_module_script_contract(HoroModuleScriptPackageInternal
        apps/common/module_scripts/ModuleScriptPackage.h)
    target_link_libraries(HoroModuleScriptPackageInternal INTERFACE
        HoroModuleScriptPackage HoroScriptModuleRuntimeInternal)
endfunction()
