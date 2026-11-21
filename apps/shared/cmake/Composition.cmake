# Included from apps/CMakeLists.txt; source and binary directories remain scoped to apps/.
# Explicit non-installed package composition contract; stage only its owning host header.
set(HORO_PACKAGE_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/package_host_headers")
file(MAKE_DIRECTORY "${HORO_PACKAGE_HOST_HEADERS}")
configure_file(shared/packages/PackageProjectSession.h "${HORO_PACKAGE_HOST_HEADERS}/PackageProjectSession.h" COPYONLY)
configure_file(shared/packages/PackageProjectHost.h "${HORO_PACKAGE_HOST_HEADERS}/PackageProjectHost.h" COPYONLY)
configure_file(shared/packages/PackageHostPolicy.h "${HORO_PACKAGE_HOST_HEADERS}/PackageHostPolicy.h" COPYONLY)
configure_file(shared/packages/ApprovedGameplayPackageHost.h "${HORO_PACKAGE_HOST_HEADERS}/ApprovedGameplayPackageHost.h" COPYONLY)
add_library(HoroPackageProjectHost STATIC shared/packages/PackageProjectSession.cpp shared/packages/PackageProjectPreparation.cpp
    shared/packages/PackageProjectHost.cpp shared/packages/PackageProjectMetadata.cpp shared/packages/PackageHostPolicy.cpp
    shared/packages/ApprovedGameplayPackageHost.cpp)
target_compile_features(HoroPackageProjectHost PUBLIC cxx_std_20)
target_include_directories(HoroPackageProjectHost PUBLIC "${HORO_PACKAGE_HOST_HEADERS}")
target_link_libraries(HoroPackageProjectHost PUBLIC HoroEngine::PackageApplication
    PRIVATE HoroEngine::PackageGameplayHost HoroHostSignatureDocuments HoroEngine::Security)

# One application-owned signature document codec, shared by the package CLI and
# explicit installed-host bootstrap. The staged interface is never installed.
set(HORO_HOST_SIGNATURE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/host_signature_headers")
file(MAKE_DIRECTORY "${HORO_HOST_SIGNATURE_HEADERS}")
configure_file(shared/security/HostSignatureDocument.h "${HORO_HOST_SIGNATURE_HEADERS}/HostSignatureDocument.h" COPYONLY)
add_library(HoroHostSignatureDocuments STATIC shared/security/HostSignatureDocument.cpp)
target_compile_features(HoroHostSignatureDocuments PUBLIC cxx_std_20)
target_include_directories(HoroHostSignatureDocuments PUBLIC "${HORO_HOST_SIGNATURE_HEADERS}")
target_link_libraries(HoroHostSignatureDocuments PUBLIC HoroEngine::PackageSecurity PRIVATE HoroFoundationJsonInternal)

# The cold release trust policy bridges the Application and host crypto boundaries.
set(HORO_RELEASE_MANIFEST_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/release_manifest_host_headers")
file(MAKE_DIRECTORY "${HORO_RELEASE_MANIFEST_HOST_HEADERS}")
configure_file(shared/security/HostReleaseManifestSignatureVerifier.h
    "${HORO_RELEASE_MANIFEST_HOST_HEADERS}/HostReleaseManifestSignatureVerifier.h" COPYONLY)
add_library(HoroReleaseManifestSignatureHostInternal STATIC shared/security/HostReleaseManifestSignatureVerifier.cpp)
target_compile_features(HoroReleaseManifestSignatureHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroReleaseManifestSignatureHostInternal PUBLIC "${HORO_RELEASE_MANIFEST_HOST_HEADERS}")
target_link_libraries(HoroReleaseManifestSignatureHostInternal PUBLIC HoroEngine::Application
    PRIVATE HoroHostSignatureDocuments HoroEngine::Security)

add_library(HoroHostModuleComposition STATIC
    shared/modules/HostModuleComposition.cpp
)
target_compile_features(HoroHostModuleComposition PUBLIC cxx_std_20)
target_include_directories(HoroHostModuleComposition PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/shared/modules
)
target_link_libraries(HoroHostModuleComposition PUBLIC HoroEngine::Foundation HoroEngine::PlatformServices)

# Editor and headless samples share one explicit simulator Scene/Input adapter.
# Stage only this host contract; implementation state remains application-private.
set(HORO_XR_SIMULATOR_PLAY_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/xr_simulator_play_headers")
file(MAKE_DIRECTORY "${HORO_XR_SIMULATOR_PLAY_HEADERS}")
configure_file(shared/xr/SimulatorPlayHost.h "${HORO_XR_SIMULATOR_PLAY_HEADERS}/SimulatorPlayHost.h" COPYONLY)
add_library(HoroXRSimulatorPlayHost STATIC
    shared/xr/SimulatorPlayHost.cpp
    shared/xr/SimulatorPlayInput.cpp
    shared/xr/SimulatorPlayScene.cpp)
target_compile_features(HoroXRSimulatorPlayHost PUBLIC cxx_std_20)
target_include_directories(HoroXRSimulatorPlayHost PUBLIC "${HORO_XR_SIMULATOR_PLAY_HEADERS}")
target_link_libraries(HoroXRSimulatorPlayHost
    PUBLIC HoroEngine::EditorXRSimulatorModel HoroEditorPlayXrHostInterface HoroEngine::RuntimeScene HoroEngine::Runtime
    PRIVATE HoroXRSimulatorHostInterface HoroEditorXrSceneInternal)

set(HORO_XR_SIMULATOR_ARTIFACT_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/xr_simulator_artifact_headers")
file(MAKE_DIRECTORY "${HORO_XR_SIMULATOR_ARTIFACT_HEADERS}")
configure_file(shared/xr/SimulatorProjectArtifacts.h
    "${HORO_XR_SIMULATOR_ARTIFACT_HEADERS}/SimulatorProjectArtifacts.h" COPYONLY)
add_library(HoroXRSimulatorProjectArtifacts STATIC shared/xr/SimulatorProjectArtifacts.cpp)
target_compile_features(HoroXRSimulatorProjectArtifacts PUBLIC cxx_std_20)
target_include_directories(HoroXRSimulatorProjectArtifacts PUBLIC "${HORO_XR_SIMULATOR_ARTIFACT_HEADERS}")
target_link_libraries(HoroXRSimulatorProjectArtifacts
    PUBLIC HoroEngine::EditorXRSimulatorModel HoroEngine::ProjectReadFilesApi
    PRIVATE HoroEngine::Platform)

# Application-only observer; Scene remains the sole native candidate owner. Only
# this header and its declared non-installed Physics boundary reach consumers.
set(HORO_SCENE_PHYSICS_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/scene_physics_host_headers")
file(MAKE_DIRECTORY "${HORO_SCENE_PHYSICS_HOST_HEADERS}")
foreach(_horo_scene_host_header IN ITEMS ScenePhysicsHost.h CommittedSceneAssets.h)
    file(GENERATE OUTPUT "${HORO_SCENE_PHYSICS_HOST_HEADERS}/${_horo_scene_host_header}"
        CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/shared/scene/${_horo_scene_host_header}\"\n")
endforeach()
add_library(HoroScenePhysicsHost STATIC shared/scene/ScenePhysicsHost.cpp shared/scene/CommittedSceneAssets.cpp)
target_compile_features(HoroScenePhysicsHost PUBLIC cxx_std_20)
target_include_directories(HoroScenePhysicsHost PUBLIC "${HORO_SCENE_PHYSICS_HOST_HEADERS}")
target_link_libraries(HoroScenePhysicsHost PUBLIC HoroPhysicsSceneHostInternal HoroEngine::SceneCook HoroEngine::Assets
        HoroEngine::ProjectPhysicsCapture PRIVATE HoroEngine::AnimationSceneIntegration)

set(HORO_SCENE_DECISION_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/scene_decision_headers")
file(GENERATE OUTPUT "${HORO_SCENE_DECISION_HEADERS}/CookedDecisionSceneAdmission.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/shared/scene/CookedDecisionSceneAdmission.h\"\n")
add_library(HoroCookedDecisionSceneAdmissionInternal STATIC shared/scene/CookedDecisionSceneAdmission.cpp)
target_compile_features(HoroCookedDecisionSceneAdmissionInternal PUBLIC cxx_std_20)
target_include_directories(HoroCookedDecisionSceneAdmissionInternal PUBLIC "${HORO_SCENE_DECISION_HEADERS}")
target_link_libraries(HoroCookedDecisionSceneAdmissionInternal PUBLIC HoroEngine::AISceneIntegration HoroEngine::PrefabRuntime
    PRIVATE HoroScenePhysicsHost HoroEngine::SceneRuntimeConversion)

# Project Perception admission and position capture are shared by both application hosts.
set(HORO_PROJECT_SCENE_PERCEPTION_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/project_scene_perception_headers")
file(GENERATE OUTPUT "${HORO_PROJECT_SCENE_PERCEPTION_HEADERS}/ProjectScenePerception.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/shared/scene/ProjectScenePerception.h\"\n")
add_library(HoroProjectScenePerceptionInternal STATIC shared/scene/ProjectScenePerception.cpp)
target_compile_features(HoroProjectScenePerceptionInternal PUBLIC cxx_std_20)
target_include_directories(HoroProjectScenePerceptionInternal PUBLIC "${HORO_PROJECT_SCENE_PERCEPTION_HEADERS}")
target_link_libraries(HoroProjectScenePerceptionInternal PUBLIC HoroEngine::AISceneIntegration HoroEngine::PlatformProjectIo
    PRIVATE nlohmann_json::nlohmann_json)

# The installed standalone and Editor Play hosts share a canonical mounted-World adapter.
set(HORO_MOUNTED_WORLD_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/mounted_world_host_headers")
file(MAKE_DIRECTORY "${HORO_MOUNTED_WORLD_HOST_HEADERS}")
configure_file(shared/world/MountedWorldHost.h "${HORO_MOUNTED_WORLD_HOST_HEADERS}/MountedWorldHost.h" COPYONLY)
add_library(HoroMountedWorldHostInternal STATIC shared/world/MountedWorldHost.cpp)
target_compile_features(HoroMountedWorldHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroMountedWorldHostInternal PUBLIC "${HORO_MOUNTED_WORLD_HOST_HEADERS}")
target_link_libraries(HoroMountedWorldHostInternal
    PUBLIC HoroEngine::SceneRenderExtraction HoroEngine::WorldStreaming HoroEngine::RuntimeScene HoroEngine::Assets
    PRIVATE HoroEngine::SceneCellPayload HoroScenePhysicsHost)

set(HORO_PROJECT_NAVIGATION_SCENE_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/project_navigation_scene_headers")
file(GENERATE OUTPUT "${HORO_PROJECT_NAVIGATION_SCENE_HEADERS}/ProjectSceneNavigationComposition.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/shared/navigation/ProjectSceneNavigationComposition.h\"\n")
add_library(HoroProjectSceneNavigationHostInternal STATIC shared/navigation/ProjectSceneNavigationComposition.cpp)
target_compile_features(HoroProjectSceneNavigationHostInternal PUBLIC cxx_std_20)
target_include_directories(HoroProjectSceneNavigationHostInternal PUBLIC "${HORO_PROJECT_NAVIGATION_SCENE_HEADERS}")
target_link_libraries(HoroProjectSceneNavigationHostInternal
    PUBLIC HoroEngine::NavigationAssetSceneIntegration HoroEngine::PlatformProjectReadFiles
    PRIVATE HoroEngine::NavigationRecastDetour HoroEngine::ProjectSettings)

set(HORO_SCENE_AI_NAVIGATION_MOTION_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/scene_ai_navigation_motion_headers")
file(GENERATE OUTPUT "${HORO_SCENE_AI_NAVIGATION_MOTION_HEADERS}/SceneAiNavigationMotion.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/shared/navigation/SceneAiNavigationMotion.h\"\n")
add_library(HoroSceneAiNavigationMotionInternal STATIC shared/navigation/SceneAiNavigationMotion.cpp)
target_compile_features(HoroSceneAiNavigationMotionInternal PUBLIC cxx_std_20)
target_include_directories(HoroSceneAiNavigationMotionInternal PUBLIC "${HORO_SCENE_AI_NAVIGATION_MOTION_HEADERS}")
target_link_libraries(HoroSceneAiNavigationMotionInternal PUBLIC HoroEngine::AINavigationIntegration HoroEngine::NavigationCharacterIntegration)
target_link_libraries(HoroSceneAiNavigationMotionInternal PRIVATE HoroScenePhysicsHost)

# Non-installed identity authority shared by application Scene factories, never by copied counters.
set(HORO_ANIMATION_HOST_IDENTITY_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/animation_host_identity_headers")
file(MAKE_DIRECTORY "${HORO_ANIMATION_HOST_IDENTITY_HEADERS}")
file(GENERATE OUTPUT "${HORO_ANIMATION_HOST_IDENTITY_HEADERS}/AnimationRuntimeIdentitySource.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/shared/animation/AnimationRuntimeIdentitySource.h\"\n")
add_library(HoroAnimationHostIdentityInternal INTERFACE)
target_compile_features(HoroAnimationHostIdentityInternal INTERFACE cxx_std_20)
target_include_directories(HoroAnimationHostIdentityInternal INTERFACE "${HORO_ANIMATION_HOST_IDENTITY_HEADERS}")
target_link_libraries(HoroAnimationHostIdentityInternal INTERFACE HoroEngine::AnimationApi)

# Conservative provider closure: any first-party navigation implementation or
# public contract change invalidates native bake compatibility. Configure again
# when contents change, including uncommitted edits; a Git revision is insufficient.
file(GLOB_RECURSE horo_navigation_identity_inputs CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/Horo/*.h"
    "${PROJECT_SOURCE_DIR}/src/runtime/navigation/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/navigation/*.h"
    "${PROJECT_SOURCE_DIR}/src/application/navigation/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/application/navigation/*.h"
    "${PROJECT_SOURCE_DIR}/src/foundation/math/*.cpp"
    "${PROJECT_SOURCE_DIR}/src/foundation/math/*.h")
list(SORT horo_navigation_identity_inputs)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${horo_navigation_identity_inputs})
set(horo_navigation_identity_material "recast=${HORO_RECAST_NAVIGATION_REVISION};enabled=${HORO_BUILD_NAVIGATION_RECAST_DETOUR};compiler=${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_VERSION};target=${CMAKE_CXX_COMPILER_TARGET};system=${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR};architectures=${CMAKE_OSX_ARCHITECTURES};pointer=${CMAKE_SIZEOF_VOID_P};flags=${CMAKE_CXX_FLAGS};debug=${CMAKE_CXX_FLAGS_DEBUG};release=${CMAKE_CXX_FLAGS_RELEASE};relwithdebinfo=${CMAKE_CXX_FLAGS_RELWITHDEBINFO};minsize=${CMAKE_CXX_FLAGS_MINSIZEREL};definitions=DT_POLYREF64,RC_DISABLE_ASSERTS")
foreach(horo_navigation_identity_input IN LISTS horo_navigation_identity_inputs)
    file(SHA256 "${horo_navigation_identity_input}" horo_navigation_identity_digest)
    file(RELATIVE_PATH horo_navigation_identity_path "${PROJECT_SOURCE_DIR}" "${horo_navigation_identity_input}")
    string(APPEND horo_navigation_identity_material ";${horo_navigation_identity_path}=${horo_navigation_identity_digest}")
endforeach()
string(SHA256 HORO_NAVIGATION_BUILD_FINGERPRINT "${horo_navigation_identity_material}")

# One non-installed native composition authority for editor, CLI and MCP bake adapters.
set(HORO_NAVIGATION_HOST_HEADERS "${CMAKE_CURRENT_BINARY_DIR}/navigation_host_headers")
file(MAKE_DIRECTORY "${HORO_NAVIGATION_HOST_HEADERS}")
configure_file(shared/navigation/NativeNavigationBakeComposition.h "${HORO_NAVIGATION_HOST_HEADERS}/NativeNavigationBakeComposition.h" COPYONLY)
add_library(HoroNativeNavigationBakeHost STATIC shared/navigation/NativeNavigationBakeComposition.cpp)
target_compile_features(HoroNativeNavigationBakeHost PUBLIC cxx_std_20)
target_include_directories(HoroNativeNavigationBakeHost PUBLIC "${HORO_NAVIGATION_HOST_HEADERS}")
target_compile_definitions(HoroNativeNavigationBakeHost PRIVATE
        HORO_NAVIGATION_BUILD_FINGERPRINT="${HORO_NAVIGATION_BUILD_FINGERPRINT}-$<CONFIG>")
target_link_libraries(HoroNativeNavigationBakeHost PUBLIC HoroEngine::NavigationBakeAutomation
        PRIVATE HoroEngine::NavigationRecastDetour HoroEngine::PlatformProjectIo HoroEngine::Platform)

# The application joins actual Platform image evidence with the selected native Physics owner.
add_library(HoroPhysicsStateProcessBindingInternal STATIC)
target_compile_features(HoroPhysicsStateProcessBindingInternal PUBLIC cxx_std_20)
set(_horo_state_binding_headers "${CMAKE_CURRENT_BINARY_DIR}/internal/physics-state-binding")
file(MAKE_DIRECTORY "${_horo_state_binding_headers}")
configure_file(shared/physics/PhysicsStateProcessBinding.h "${_horo_state_binding_headers}/PhysicsStateProcessBinding.h" COPYONLY)
target_include_directories(HoroPhysicsStateProcessBindingInternal PUBLIC "$<BUILD_INTERFACE:${_horo_state_binding_headers}>")
target_link_libraries(HoroPhysicsStateProcessBindingInternal PUBLIC HoroEngine::Physics
    PRIVATE HoroPhysicsStateIdentityInternal HoroPlatformProcessImageInternal)
if(HORO_BUILD_PHYSICS_NATIVE)
    target_sources(HoroPhysicsStateProcessBindingInternal PRIVATE shared/physics/PhysicsStateProcessBinding.cpp)
else()
    target_sources(HoroPhysicsStateProcessBindingInternal PRIVATE shared/physics/PhysicsStateProcessBindingUnavailable.cpp)
endif()

# Canonical Physics state joins the existing application-owned Save transaction.
target_sources(HoroScenePhysicsHost PRIVATE shared/scene/ScenePhysicsHostState.cpp)

target_sources(HoroPhysicsStateProcessBindingInternal PRIVATE shared/physics/PhysicsStateHostLifecycle.cpp)
file(GENERATE OUTPUT "${_horo_state_binding_headers}/PhysicsStateHostLifecycle.h"
    CONTENT "#pragma once\n#include \"${CMAKE_CURRENT_SOURCE_DIR}/shared/physics/PhysicsStateHostLifecycle.h\"\n")
