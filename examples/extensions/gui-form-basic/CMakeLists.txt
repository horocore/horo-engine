add_executable(HoroEditorUiFormBasicExample
        src/main.cpp
)

target_compile_features(HoroEditorUiFormBasicExample PRIVATE cxx_std_20)
target_link_libraries(HoroEditorUiFormBasicExample PRIVATE HoroEngine::Extensions)

add_custom_command(TARGET HoroEditorUiFormBasicExample POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/extension.json"
                "$<TARGET_FILE_DIR:HoroEditorUiFormBasicExample>/extension.json"
        VERBATIM
)

if(BUILD_TESTING)
    add_test(NAME HoroEditorUiFormBasicExample COMMAND HoroEditorUiFormBasicExample)
    set_tests_properties(HoroEditorUiFormBasicExample PROPERTIES LABELS "extensions;sdk")
endif()
