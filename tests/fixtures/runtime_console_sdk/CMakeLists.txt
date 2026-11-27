cmake_minimum_required(VERSION 3.25)
project(HoroRuntimeConsoleSdkContract LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(HoroEngineGameplay REQUIRED)
if(TARGET HoroEngine::RuntimeConsole)
    message(FATAL_ERROR "Default gameplay SDK imported the optional console")
endif()
find_package(HoroEngineGameplay REQUIRED COMPONENTS RuntimeConsole)
if(TARGET HoroEngine::RuntimeUi OR TARGET HoroEngine::Physics)
    message(FATAL_ERROR "Console SDK imported unrelated runtime capabilities")
endif()
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_LIBRARIES HoroEngine::RuntimeConsole)
unset(HORO_CONSOLE_SDK_HEADER_BOUNDARY CACHE)
check_cxx_source_compiles("
#if __has_include(<Horo/Editor/EditorUiComponents.h>) || __has_include(<editor/input/EditorInputActions.h>) || __has_include(<RuntimeConsoleView.h>) || __has_include(<ConsoleMetricCommandInternal.h>) || __has_include(<ConsoleSceneCommandInternal.h>) || __has_include(<ConsoleSceneOverlayInternal.h>) || __has_include(<NetworkConsoleInspection.h>) || __has_include(<NetworkConsoleOverlay.h>) || __has_include(<Horo/Debug/ConsoleSceneBindings.h>) || __has_include(<Horo/Runtime/Scene/RuntimeScene.h>)
#error The optional console SDK exposes unrelated or private implementation headers
#endif
#include <Horo/Debug/ConsoleConfiguration.h>
#include <Horo/Debug/ConsoleBuiltins.h>
#include <Horo/Debug/OverlaySource.h>
int main() { return Horo::Debug::ConsoleSceneInspectionLimits{}.maximumVisitedSlots == 0; }
" HORO_CONSOLE_SDK_HEADER_BOUNDARY)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT HORO_CONSOLE_SDK_HEADER_BOUNDARY)
    message(FATAL_ERROR "Optional console SDK public-header boundary failed")
endif()
add_executable(HoroRuntimeConsoleSdkConsumer Consumer.cpp)
target_compile_features(HoroRuntimeConsoleSdkConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroRuntimeConsoleSdkConsumer PRIVATE HoroEngine::RuntimeConsole)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/consumer-$<CONFIG>.txt" CONTENT "$<TARGET_FILE:HoroRuntimeConsoleSdkConsumer>")
