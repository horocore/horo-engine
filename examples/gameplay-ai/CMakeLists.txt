cmake_minimum_required(VERSION 3.25)
project(HoroGuardExample LANGUAGES C CXX)

find_package(HoroEngineGameplay CONFIG REQUIRED)
horo_add_gameplay_module(HoroGameGameplay
    MODULE_ID game.example.guard
    SOURCES source/gameplay/GuardModule.cpp
)

include(CTest)
if(BUILD_TESTING)
    add_executable(HoroGuardExampleCheck tests/HoldWaypointCheck.cpp)
    target_link_libraries(HoroGuardExampleCheck PRIVATE HoroGameGameplay HoroEngine::GameplayApi)
    add_test(NAME HoroGuardWaypointFixedTicks COMMAND HoroGuardExampleCheck)
endif()
