cmake_minimum_required(VERSION 3.25)
project(MultipleGameplayModules LANGUAGES CXX)

add_library(GameplayApi INTERFACE)
add_library(HoroEngine::GameplayApi ALIAS GameplayApi)
set(HORO_GAMEPLAY_SDK_FINGERPRINT fixture-sdk)
include("${HORO_GAMEPLAY_HELPER}")

horo_add_gameplay_module(Primary MODULE_ID game.primary NO_MANIFEST SOURCES Primary.cpp)
horo_add_gameplay_module(Secondary MODULE_ID game.secondary NO_MANIFEST
    INPUT_MANIFEST_OUTPUT "${CMAKE_BINARY_DIR}/Secondary/inputs.txt" SOURCES Secondary.cpp)

foreach(target Primary Secondary)
    get_property(revision TARGET ${target} PROPERTY HORO_GAMEPLAY_DESCRIPTOR_REVISION_FILE)
    if(NOT revision STREQUAL "${CMAKE_BINARY_DIR}/${target}_generated_behavior_bundle.revision")
        message(FATAL_ERROR "${target} lost its target-owned descriptor revision")
    endif()
    get_target_property(sources ${target} SOURCES)
    if(NOT "${CMAKE_BINARY_DIR}/${target}_generated_behavior_bundle.cpp" IN_LIST sources)
        message(FATAL_ERROR "${target} lost its generated descriptor bundle")
    endif()
endforeach()
