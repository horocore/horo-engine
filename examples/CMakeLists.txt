add_subdirectory(extensions/asset-importer-basic)
add_subdirectory(extensions/gui-form-basic)

add_executable(HoroGuardCoverSample EXCLUDE_FROM_ALL
    gameplay-ai/host/GuardCover.cpp
    gameplay-ai/host/GuardCoverSample.cpp)
target_compile_features(HoroGuardCoverSample PRIVATE cxx_std_20)
target_link_libraries(HoroGuardCoverSample PRIVATE HoroEngine::GameplayAiIntegration)
