include_guard(GLOBAL)

# Include from tests/ after native Provider/Client fixtures and Catch registration.
horo_stage_module_script_contract(HoroScriptModuleTestReferenceInternal
    src/extensions/script/ScriptModuleReferenceService.h)
target_link_libraries(HoroScriptModuleTestReferenceInternal INTERFACE HoroScriptModuleRuntime)

add_executable(HoroScriptModuleRuntimeAdapterTests
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptRuntimeBackendBindingTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptModuleRuntimeParityTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptModuleReplacementTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptModuleBindingCacheTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptModuleImportDiagnosticsTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptModuleProjectionTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/support/AllocationProbe.cpp")
target_include_directories(HoroScriptModuleRuntimeAdapterTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
target_link_libraries(HoroScriptModuleRuntimeAdapterTests PRIVATE HoroScriptModuleTestReferenceInternal
    HoroScriptModuleRuntimeInternal HoroLuaScriptModuleRuntime HoroJavaScriptModuleRuntime)
horo_register_catch_test(HoroScriptModuleRuntimeAdapterTests LABELS "unit;extensions;script;headless")

add_library(HoroModuleScriptSourceFixture MODULE
    "${PROJECT_SOURCE_DIR}/tests/fixtures/extensions/ModuleScriptSource.c")
set_target_properties(HoroModuleScriptSourceFixture PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF)
target_link_libraries(HoroModuleScriptSourceFixture PRIVATE HoroEngine::Extensions)

add_executable(HoroModuleScriptProductHostTests
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptModuleHeadlessTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptModuleSourceOwnerTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/extensions/ScriptModuleCliTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/editor/EditorPlayModuleScriptTests.cpp")
# Explicit test-only forwarders expose just the controller's three qualified contracts.
set(controller_headers "${CMAKE_CURRENT_BINARY_DIR}/module-script-controller-headers")
foreach(header IN ITEMS editor/gameplay/EditorPlaySessionController.h
        editor/document/RuntimeSceneConversion.h editor/document/SceneDocument.h)
    file(CONFIGURE OUTPUT "${controller_headers}/${header}"
        CONTENT "#include \"${PROJECT_SOURCE_DIR}/src/${header}\"\n" @ONLY)
endforeach()
target_include_directories(HoroModuleScriptProductHostTests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/support" "${controller_headers}")
target_link_libraries(HoroModuleScriptProductHostTests PRIVATE HoroScriptModuleHostInternal
    HoroModuleScriptPackageInternal HoroModuleScriptHostBridgeInternal
    HoroScriptModuleTestReferenceInternal HoroScriptModuleRuntimeInternal
    HoroLuaScriptModuleRuntime HoroJavaScriptModuleRuntime HoroEngine::EditorServices
    HoroHostSignatureDocuments MbedTLS::mbedcrypto)
target_compile_definitions(HoroModuleScriptProductHostTests PRIVATE
    HORO_NATIVE_SERVICE_PROVIDER_FIXTURE="$<TARGET_FILE:HoroNativeServiceProviderFixture>"
    HORO_NATIVE_SERVICE_CLIENT_FIXTURE="$<TARGET_FILE:HoroNativeServiceClientFixture>"
    HORO_MODULE_SCRIPT_SOURCE_FIXTURE="$<TARGET_FILE:HoroModuleScriptSourceFixture>"
    HORO_MODULE_SCRIPT_CLI="$<TARGET_FILE:horo-engine>")
add_dependencies(HoroModuleScriptProductHostTests HoroNativeServiceProviderFixture
    HoroNativeServiceClientFixture HoroModuleScriptSourceFixture horo-engine)
horo_register_catch_test(HoroModuleScriptProductHostTests LABELS "unit;extensions;script;editor;headless")

# Existing scalar CLI regression sources share the same authenticated package contract.
target_link_libraries(HoroReferenceExtensionCliTests PRIVATE HoroModuleScriptPackageInternal)
