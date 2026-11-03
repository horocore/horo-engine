include_guard(GLOBAL)

# Pure version/hash semantics live below source authoring and runtime prefab models.
function(horo_add_project_version_target)
    add_library(HoroProjectVersion STATIC
        "${PROJECT_SOURCE_DIR}/src/application/project/ProjectVersion.cpp"
        "${PROJECT_SOURCE_DIR}/src/application/project/ProjectVersionErrors.cpp")
    add_library(HoroEngine::ProjectVersion ALIAS HoroProjectVersion)
    set_target_properties(HoroProjectVersion PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(HoroProjectVersion PUBLIC cxx_std_20)
    target_link_libraries(HoroProjectVersion PUBLIC HoroEngine::Foundation)
endfunction()

function(horo_stage_prefab_internal target)
    add_library(${target} INTERFACE)
    set(stage "${CMAKE_CURRENT_BINARY_DIR}/${target}-headers")
    foreach(header IN LISTS ARGN)
        get_filename_component(name "${header}" NAME)
        configure_file("${PROJECT_SOURCE_DIR}/${header}" "${stage}/${name}" COPYONLY)
    endforeach()
    target_include_directories(${target} INTERFACE "$<BUILD_INTERFACE:${stage}>")
endfunction()

# Called after the existing Prefab/Runtime/Authoring owners have been declared.
function(horo_add_prefab_persistence_targets)
    add_library(HoroPrefabOverrides STATIC
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabComponentOverrideResolution.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabComponentEdits.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabComponentOverrideCodec.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabComponentOverrideValidation.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabOverrideRecordOrdering.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabComponentSchemas.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabValueOverrideAddresses.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabValueOverrideBindings.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabValueOverrideResolution.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabValueOverrideSerialization.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabValueOverrideValues.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabValueSchemas.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabValueJson.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabOverridePlacement.cpp"
    )
    add_library(HoroEngine::PrefabOverrides ALIAS HoroPrefabOverrides)
    set_target_properties(HoroPrefabOverrides PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(HoroPrefabOverrides PUBLIC cxx_std_20)
    target_link_libraries(HoroPrefabOverrides PUBLIC HoroEngine::Prefab HoroEngine::PrefabOverrideModel
        PRIVATE HoroFoundationJsonInternal)
    horo_stage_prefab_internal(HoroPrefabOverrideInternal
        src/runtime/prefab/PrefabValueOverridesInternal.h
        src/runtime/prefab/PrefabValueResolutionInternal.h
        src/runtime/prefab/PrefabValueJsonInternal.h)
    target_link_libraries(HoroPrefabOverrideInternal INTERFACE HoroPrefabOverrides)
    target_link_libraries(HoroPrefabAuthoring PUBLIC HoroEngine::PrefabOverrides
        PRIVATE HoroPrefabOverrideInternal)
    target_link_libraries(HoroPrefabRuntime PUBLIC HoroEngine::PrefabOverrides HoroEngine::SaveApi)
    target_link_libraries(HoroRuntimeScene PUBLIC HoroEngine::SaveApi)
    horo_stage_prefab_internal(HoroPrefabRuntimeProjectionInternal
        src/runtime/prefab/PrefabRuntimeProjection.h
        src/runtime/prefab/PrefabPreparedGroup.h)
    target_link_libraries(HoroPrefabRuntimeProjectionInternal INTERFACE HoroEngine::PrefabRuntime)
    add_library(HoroPrefabPersistence STATIC
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabPersistentState.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabPersistentEncoding.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabPersistentDecoding.cpp"
        "${PROJECT_SOURCE_DIR}/src/runtime/prefab/PrefabRestoreEntities.cpp"
    )
    add_library(HoroEngine::PrefabPersistence ALIAS HoroPrefabPersistence)
    set_target_properties(HoroPrefabPersistence PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_features(HoroPrefabPersistence PUBLIC cxx_std_20)
    target_link_libraries(HoroPrefabPersistence PUBLIC HoroEngine::PrefabRuntime
        HoroEngine::RuntimeScene HoroEngine::SaveApi PRIVATE HoroPrefabRuntimeProjectionInternal)
endfunction()
