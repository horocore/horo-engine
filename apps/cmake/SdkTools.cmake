# Included from apps/CMakeLists.txt; source and binary directories remain scoped to apps/.
add_executable(horo-extension-validate
    horo-extension-validate/main.cpp
)
target_compile_features(horo-extension-validate PRIVATE cxx_std_20)
target_link_libraries(horo-extension-validate PRIVATE HoroEngine::Extensions)
set(HORO_EXTENSION_VALIDATOR_SDK_PATH
    "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-validate${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(
    OUTPUT "${HORO_EXTENSION_VALIDATOR_SDK_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:horo-extension-validate>"
        "${HORO_EXTENSION_VALIDATOR_SDK_PATH}"
    DEPENDS horo-extension-validate
    COMMENT "Staging the extension manifest validator in the SDK"
    VERBATIM)
add_custom_target(HoroExtensionSdkValidatorStage ALL
    DEPENDS "${HORO_EXTENSION_VALIDATOR_SDK_PATH}")

add_executable(horo-extension-conformance
    horo-extension-conformance/main.cpp
)
target_compile_features(horo-extension-conformance PRIVATE cxx_std_20)
target_link_libraries(horo-extension-conformance PRIVATE HoroEngine::Extensions)
set(HORO_EXTENSION_CONFORMANCE_SDK_PATH
    "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-extension-conformance${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(
    OUTPUT "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:horo-extension-conformance>"
        "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}"
    DEPENDS horo-extension-conformance
    COMMENT "Staging the extension ABI conformance harness in the SDK"
    VERBATIM)
add_custom_target(HoroExtensionSdkConformanceStage ALL
    DEPENDS "${HORO_EXTENSION_CONFORMANCE_SDK_PATH}")

add_executable(horo-package
    horo-package/main.cpp
    horo-package/PackageIo.cpp
    horo-package/Pack.cpp
    horo-package/SignatureDocument.cpp
    horo-package/Sign.cpp
    horo-package/Verify.cpp
)
target_compile_features(horo-package PRIVATE cxx_std_20)
target_link_libraries(horo-package PRIVATE HoroEngine::PackageSecurity HoroEngine::Packages HoroEngine::Security
    HoroHostSignatureDocuments miniz nlohmann_json::nlohmann_json MbedTLS::mbedcrypto)
set(HORO_PACKAGE_SDK_PATH
    "${HORO_EXTENSION_SDK_PACKAGE_DIR}/bin/horo-package${CMAKE_EXECUTABLE_SUFFIX}")
add_custom_command(
    OUTPUT "${HORO_PACKAGE_SDK_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:horo-package>"
        "${HORO_PACKAGE_SDK_PATH}"
    DEPENDS horo-package
    COMMENT "Staging the package authoring commands in the versioned SDK"
    VERBATIM)
add_custom_target(HoroExtensionSdkPackageStage ALL DEPENDS "${HORO_PACKAGE_SDK_PATH}")

# Focused SDK-author qualification can stage its three source-free tools without
# building unrelated applications.
add_custom_target(HoroExtensionAuthorCiStage DEPENDS
    HoroExtensionSdkValidatorStage
    HoroExtensionSdkConformanceStage
    HoroExtensionSdkPackageStage)
