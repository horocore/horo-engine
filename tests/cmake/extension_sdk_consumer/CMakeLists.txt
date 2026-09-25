cmake_minimum_required(VERSION 3.25)
project(HoroExtensionSdkConsumer LANGUAGES C)

find_package(HoroEngineExtensionSdk CONFIG REQUIRED)

if(NOT TARGET HoroEngine::ExtensionSdk)
    message(FATAL_ERROR "The extension SDK did not publish its author target")
endif()
if(NOT HORO_EXTENSION_SDK_VERSION STREQUAL HORO_EXPECTED_EXTENSION_SDK_VERSION)
    message(FATAL_ERROR "Unexpected extension SDK version")
endif()
if(NOT HORO_EXTENSION_ABI_MAJOR_VERSION EQUAL 1 OR
   NOT HORO_EXTENSION_ABI_PUBLISHED_MINOR EQUAL 2 OR
   NOT HORO_EXTENSION_ABI_MIN_HOST_MINOR EQUAL 0 OR
   NOT HORO_EXTENSION_ABI_MAX_HOST_MAJOR_EXCLUSIVE EQUAL 2)
    message(FATAL_ERROR "Unexpected SDK-to-host ABI compatibility range")
endif()
if(NOT EXISTS "${HORO_EXTENSION_SDK_METADATA}")
    message(FATAL_ERROR "The extension SDK metadata is missing")
endif()
file(READ "${HORO_EXTENSION_SDK_METADATA}" extension_sdk_metadata)
string(JSON metadata_version GET "${extension_sdk_metadata}" sdk version)
string(JSON metadata_abi_major GET "${extension_sdk_metadata}" hostAbi minimum major)
string(JSON metadata_abi_minimum GET "${extension_sdk_metadata}" hostAbi minimum minor)
string(JSON metadata_abi_maximum GET "${extension_sdk_metadata}" hostAbi maximumExclusive major)
string(JSON metadata_abi_published GET "${extension_sdk_metadata}" hostAbi published minor)
string(JSON metadata_dependency_count LENGTH "${extension_sdk_metadata}" compileDependencies)
if(NOT metadata_version STREQUAL HORO_EXTENSION_SDK_VERSION OR
   NOT metadata_abi_major EQUAL HORO_EXTENSION_ABI_MAJOR_VERSION OR
   NOT metadata_abi_minimum EQUAL HORO_EXTENSION_ABI_MIN_HOST_MINOR OR
   NOT metadata_abi_maximum EQUAL HORO_EXTENSION_ABI_MAX_HOST_MAJOR_EXCLUSIVE OR
   NOT metadata_abi_published EQUAL HORO_EXTENSION_ABI_PUBLISHED_MINOR OR
   NOT metadata_dependency_count EQUAL 0)
    message(FATAL_ERROR "Extension SDK metadata disagrees with its CMake package")
endif()

get_target_property(extension_sdk_dependencies
    HoroEngine::ExtensionSdk INTERFACE_LINK_LIBRARIES)
if(extension_sdk_dependencies)
    message(FATAL_ERROR
        "The extension SDK must not expose engine or third-party dependencies")
endif()

add_library(HoroExternalExtension MODULE ExternalExtension.c)
target_compile_features(HoroExternalExtension PRIVATE c_std_11)
set_target_properties(HoroExternalExtension PROPERTIES
    C_EXTENSIONS OFF
    C_VISIBILITY_PRESET hidden)
target_link_libraries(HoroExternalExtension PRIVATE HoroEngine::ExtensionSdk)
