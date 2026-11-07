include_guard(GLOBAL)

add_executable(HoroPrefabPersistenceTests
    "${PROJECT_SOURCE_DIR}/tests/unit/runtime/prefab/PrefabPersistentCodecTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/runtime/prefab/PrefabRestoreEntitiesTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/support/AllocationProbe.cpp")
target_include_directories(HoroPrefabPersistenceTests PRIVATE "${PROJECT_SOURCE_DIR}/tests/support")
target_link_libraries(HoroPrefabPersistenceTests PRIVATE HoroEngine::PrefabPersistence HoroEngine::PrefabAuthoring)
horo_register_catch_test(HoroPrefabPersistenceTests LABELS "unit;prefab;save;headless")

add_executable(HoroEditorPlaySavePrefabTests
    "${PROJECT_SOURCE_DIR}/tests/unit/editor/EditorPlaySavePrefabTests.cpp")
target_include_directories(HoroEditorPlaySavePrefabTests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/helpers" "${_horo_play_save_test_headers}")
target_link_libraries(HoroEditorPlaySavePrefabTests PRIVATE HoroEditorPlaySaveHostInternal
    HoroEngine::PrefabAuthoring HoroEngine::PrefabPersistence HoroEngine::Platform)
target_compile_definitions(HoroEditorPlaySavePrefabTests PRIVATE
    HORO_TEST_GAME_MODULE_PATH="$<TARGET_FILE:HoroTestGameModule>"
    HORO_TEST_GAME_MODULE_REVISION_PATH="${HORO_TEST_GAME_MODULE_REVISION_FILE}")
add_dependencies(HoroEditorPlaySavePrefabTests HoroTestGameModule)
horo_register_catch_test(HoroEditorPlaySavePrefabTests LABELS "unit;editor;prefab;save;scene;headless")

target_sources(HoroRuntimeSceneTests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/unit/runtime/scene/RuntimeSceneExternalBindingSlotTests.cpp"
    "${PROJECT_SOURCE_DIR}/tests/unit/runtime/scene/RuntimeSceneRestoreSealTests.cpp")
target_sources(HoroFoundationJsonTests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/unit/foundation/OwnedJsonReaderTests.cpp")

add_executable(HoroPrefabPersistencePublicConsumer
    "${PROJECT_SOURCE_DIR}/tests/support/PrefabPersistencePublicHeaderConsumer.cpp")
target_compile_features(HoroPrefabPersistencePublicConsumer PRIVATE cxx_std_20)
target_link_libraries(HoroPrefabPersistencePublicConsumer PRIVATE HoroEngine::PrefabPersistence)
add_test(NAME HoroPrefabPersistencePublicConsumer COMMAND HoroPrefabPersistencePublicConsumer)
set_tests_properties(HoroPrefabPersistencePublicConsumer PROPERTIES LABELS "unit;prefab;save;headless;public_headers;ci-windows")
