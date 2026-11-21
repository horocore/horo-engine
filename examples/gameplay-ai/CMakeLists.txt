cmake_minimum_required(VERSION 3.25)
project(HoroGuardExample LANGUAGES C CXX)

find_package(HoroEngineGameplay CONFIG REQUIRED)
horo_add_gameplay_module(HoroGameGameplay
    MODULE_ID game.example.guard
    SOURCES source/gameplay/GuardModule.cpp
            source/gameplay/GuardProximitySense.cpp
            source/gameplay/GuardTargetBeacon.cpp
)

include(CTest)
if(BUILD_TESTING)
    add_executable(HoroGuardExampleCheck tests/HoldWaypointCheck.cpp)
    target_link_libraries(HoroGuardExampleCheck PRIVATE HoroGameGameplay HoroEngine::GameplayApi)
    add_test(NAME HoroGuardWaypointFixedTicks COMMAND HoroGuardExampleCheck "${CMAKE_CURRENT_SOURCE_DIR}/Assets")
    add_executable(HoroGuardSenseCheck tests/GuardSenseCheck.cpp)
    target_link_libraries(HoroGuardSenseCheck PRIVATE HoroGameGameplay HoroEngine::GameplayApi)
    add_test(NAME HoroGuardSensesNearbyTargets COMMAND HoroGuardSenseCheck)
endif()
