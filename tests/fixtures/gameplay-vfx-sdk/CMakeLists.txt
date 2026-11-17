cmake_minimum_required(VERSION 3.25)
project(HoroGameplayVfxSdkConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(HoroEngineGameplay CONFIG REQUIRED COMPONENTS Vfx)
foreach(forbidden RuntimeScene GameplayVfxSceneIntegration GameplayVfxHostInternal CinematicVfxBridge FoundationJsonInternal)
    if(TARGET HoroEngine::${forbidden})
        message(FATAL_ERROR "Host/private target leaked into VFX SDK: ${forbidden}")
    endif()
endforeach()

include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES HoroEngine::GameplayVfxIntegration)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
unset(HORO_VFX_SDK_EXPOSES_HOST_BROKER CACHE)
check_cxx_source_compiles("#include \"GameplayVfxHostAccess.h\"\nint main() { return 0; }"
    HORO_VFX_SDK_EXPOSES_HOST_BROKER)
if(HORO_VFX_SDK_EXPOSES_HOST_BROKER)
    message(FATAL_ERROR "Trusted VFX host broker is visible to ordinary SDK clients")
endif()

add_executable(HoroGameplayVfxSdkConsumer main.cpp)
target_compile_features(HoroGameplayVfxSdkConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroGameplayVfxSdkConsumer PRIVATE HoroEngine::GameplayVfxIntegration)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/artifact-$<CONFIG>.txt"
    CONTENT "$<TARGET_FILE:HoroGameplayVfxSdkConsumer>")
