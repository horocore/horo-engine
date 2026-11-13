add_library(HoroRuntimeConsole STATIC
    CommandRegistry.cpp
    CommandExecutor.cpp
    ConsoleVariable.cpp
    ConsoleVariableSet.cpp
    ConsoleBuiltins.cpp
    ConsoleMetricDescriptor.cpp
    ConsoleTelemetryAudit.cpp
    ConsoleSceneDescriptors.cpp
    ConsoleSceneOverlayVariable.cpp
    ConsoleHistoryProjection.cpp
    ConsoleHistoryVariable.cpp
    ConsoleVisibilityVariable.cpp
    ConsoleStoreOverlayVariables.cpp
    ConsoleConfiguration.cpp
    OverlaySource.cpp)
add_library(HoroEngine::RuntimeConsole ALIAS HoroRuntimeConsole)
target_compile_features(HoroRuntimeConsole PUBLIC cxx_std_20)
target_link_libraries(HoroRuntimeConsole PUBLIC HoroEngine::Foundation)

# One non-installed handler shared by explicit non-shipping product and Editor composition.
set(_horo_console_telemetry_headers "${CMAKE_CURRENT_BINARY_DIR}/telemetry_internal_headers")
file(MAKE_DIRECTORY "${_horo_console_telemetry_headers}")
configure_file(ConsoleMetricCommandInternal.h "${_horo_console_telemetry_headers}/ConsoleMetricCommandInternal.h" COPYONLY)
add_library(HoroRuntimeConsoleTelemetryInternal STATIC EXCLUDE_FROM_ALL ConsoleMetricCommand.cpp)
target_compile_features(HoroRuntimeConsoleTelemetryInternal PUBLIC cxx_std_20)
target_include_directories(HoroRuntimeConsoleTelemetryInternal PUBLIC "${_horo_console_telemetry_headers}")
target_link_libraries(HoroRuntimeConsoleTelemetryInternal PUBLIC HoroEngine::RuntimeConsole)

# Scene inspection implementation is not installed and is pulled only by actual inspection compositions.
set(_horo_console_scene_headers "${CMAKE_CURRENT_BINARY_DIR}/scene_internal_headers")
file(MAKE_DIRECTORY "${_horo_console_scene_headers}")
configure_file(ConsoleSceneCommandInternal.h "${_horo_console_scene_headers}/ConsoleSceneCommandInternal.h" COPYONLY)
configure_file(ConsoleSceneOverlayInternal.h "${_horo_console_scene_headers}/ConsoleSceneOverlayInternal.h" COPYONLY)
add_library(HoroRuntimeConsoleSceneInternal STATIC EXCLUDE_FROM_ALL ConsoleSceneCommand.cpp ConsoleSceneOverlay.cpp)
target_compile_features(HoroRuntimeConsoleSceneInternal PUBLIC cxx_std_20)
target_include_directories(HoroRuntimeConsoleSceneInternal PUBLIC "${_horo_console_scene_headers}")
target_link_libraries(HoroRuntimeConsoleSceneInternal PUBLIC HoroEngine::RuntimeConsole HoroEngine::RuntimeScene)

add_library(HoroRuntimeConsoleStartup STATIC ConsoleStartup.cpp ConsoleStartupExecution.cpp)
add_library(HoroEngine::RuntimeConsoleStartup ALIAS HoroRuntimeConsoleStartup)
target_compile_features(HoroRuntimeConsoleStartup PUBLIC cxx_std_20)
target_link_libraries(HoroRuntimeConsoleStartup PUBLIC HoroEngine::RuntimeConsole HoroEngine::ProjectReadFilesApi)

add_library(HoroRuntimeConsoleUi STATIC ConsolePresentation.cpp ConsoleLogProjection.cpp ConsoleOperationProjection.cpp OverlayLayout.cpp)
add_library(HoroEngine::RuntimeConsoleUi ALIAS HoroRuntimeConsoleUi)
target_compile_features(HoroRuntimeConsoleUi PUBLIC cxx_std_20)
target_link_libraries(HoroRuntimeConsoleUi PUBLIC HoroEngine::RuntimeConsole HoroEngine::Input)

set(HORO_RUNTIME_CONSOLE_PROFILE "Development" CACHE STRING "Separate console product artifact profile")
set_property(CACHE HORO_RUNTIME_CONSOLE_PROFILE PROPERTY STRINGS Editor Development Profile Shipping Diagnostics Server)
set(_horo_console_profiles Editor Development Profile Shipping Diagnostics Server)
if(NOT HORO_RUNTIME_CONSOLE_PROFILE IN_LIST _horo_console_profiles)
    message(FATAL_ERROR "Unsupported HORO_RUNTIME_CONSOLE_PROFILE: ${HORO_RUNTIME_CONSOLE_PROFILE}")
endif()
add_library(HoroRuntimeConsoleDefaults STATIC ConsoleProduct.cpp ConsolePublicDescriptors.cpp)
add_library(HoroEngine::RuntimeConsoleDefaults ALIAS HoroRuntimeConsoleDefaults)
target_compile_features(HoroRuntimeConsoleDefaults PUBLIC cxx_std_20)
target_link_libraries(HoroRuntimeConsoleDefaults PUBLIC HoroEngine::RuntimeConsole)
if(HORO_RUNTIME_CONSOLE_PROFILE STREQUAL "Editor" OR HORO_RUNTIME_CONSOLE_PROFILE STREQUAL "Development")
    target_sources(HoroRuntimeConsoleDefaults PRIVATE ConsoleDevelopmentDescriptors.cpp)
endif()
if(HORO_RUNTIME_CONSOLE_PROFILE STREQUAL "Editor" OR HORO_RUNTIME_CONSOLE_PROFILE STREQUAL "Development" OR
   HORO_RUNTIME_CONSOLE_PROFILE STREQUAL "Diagnostics")
    target_sources(HoroRuntimeConsoleDefaults PRIVATE ConsoleSupportDescriptors.cpp ConsoleSupportBindings.cpp)
else()
    target_sources(HoroRuntimeConsoleDefaults PRIVATE ConsoleSupportBindingsUnavailable.cpp)
endif()
string(TOUPPER "${HORO_RUNTIME_CONSOLE_PROFILE}" _horo_console_profile_macro)
target_compile_definitions(HoroRuntimeConsoleDefaults PUBLIC "HORO_PROFILE_${_horo_console_profile_macro}=1")
if(NOT HORO_RUNTIME_CONSOLE_PROFILE STREQUAL "Shipping")
    target_sources(HoroRuntimeConsoleDefaults PRIVATE ConsoleDiagnosticDescriptors.cpp ConsoleMetricBindings.cpp)
    target_link_libraries(HoroRuntimeConsoleDefaults PRIVATE HoroRuntimeConsoleTelemetryInternal)
    target_compile_definitions(HoroRuntimeConsoleDefaults PUBLIC HORO_DEBUG_COMMANDS_ENABLED=1)
else()
    target_sources(HoroRuntimeConsoleDefaults PRIVATE ConsoleMetricBindingsUnavailable.cpp)
endif()

add_library(HoroRuntimeConsoleScene STATIC)
add_library(HoroEngine::RuntimeConsoleScene ALIAS HoroRuntimeConsoleScene)
target_compile_features(HoroRuntimeConsoleScene PUBLIC cxx_std_20)
target_link_libraries(HoroRuntimeConsoleScene PUBLIC HoroEngine::RuntimeConsole)
target_link_libraries(HoroRuntimeConsoleScene PRIVATE HoroEngine::RuntimeConsoleDefaults)
if(HORO_RUNTIME_CONSOLE_PROFILE STREQUAL "Editor" OR HORO_RUNTIME_CONSOLE_PROFILE STREQUAL "Development")
    target_sources(HoroRuntimeConsoleScene PRIVATE ConsoleSceneBindings.cpp)
    target_link_libraries(HoroRuntimeConsoleScene PRIVATE HoroRuntimeConsoleSceneInternal)
else()
    target_sources(HoroRuntimeConsoleScene PRIVATE ConsoleSceneBindingsUnavailable.cpp)
endif()
