#include "Horo/Platform/AndroidInput.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>
using namespace Horo;
using namespace Horo::Platform::Android;

TEST_CASE("Android input projects physical controls through the canonical collector and neutralizes focus loss") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.Key(Input::Key::A, true) == InputAdmission::Unavailable);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    REQUIRE(session.Key(Input::Key::A, true) == InputAdmission::Accepted);
    REQUIRE(session.Touch({1, 1}, 3, 4, true) == InputAdmission::Accepted);
    REQUIRE(session.ConnectGamepad(9) == InputAdmission::Accepted);
    REQUIRE(session.Button(9, Input::GamepadButton::South, true) == InputAdmission::Accepted);
    REQUIRE(session.Axis(9, Input::GamepadAxis::LeftX, 0.5F) == InputAdmission::Accepted);
    const auto &first = collector.Commit();
    CHECK(first.State(Input::Key::A).pressed);
    CHECK(first.touches[0].contact.down);
    REQUIRE(first.gamepads.size() == 1);
    const auto pad = first.gamepads[0].id;
    CHECK(first.gamepads[0].axes[0] == 0.5F);
    collector.BeginFrame(2);
    REQUIRE(session.SetFocus(false, true) == InputAdmission::Accepted);
    REQUIRE(session.Button(9, Input::GamepadButton::South, true) == InputAdmission::Unavailable);
    const auto &neutral = collector.Commit();
    CHECK_FALSE(neutral.State(Input::Key::A).down);
    CHECK(neutral.State(Input::Key::A).released);
    CHECK(neutral.touches[0].cancelled);
    CHECK_FALSE(neutral.gamepads[0].buttons[0].down);
    CHECK(neutral.gamepads[0].axes[0] == 0);
    collector.BeginFrame(3);
    REQUIRE(session.DisconnectGamepad(9) == InputAdmission::Accepted);
    REQUIRE(session.ConnectGamepad(9) == InputAdmission::Accepted);
    const auto &reconnected = collector.Commit();
    REQUIRE(reconnected.gamepads.size() == 1);
    CHECK(reconnected.gamepads[0].id != pad);
}

TEST_CASE("Android sensor grants and settings-return revocation publish only fresh admitted canonical samples") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    REQUIRE(session.ConfigureSensor(SensorKind::StepCounter, true, true) == InputAdmission::Accepted);
    REQUIRE(session.Sample(SensorKind::StepCounter, {}, 1, 20) == InputAdmission::Unavailable);
    REQUIRE(session.SetPermission(PermissionState::Requesting) == InputAdmission::Accepted);
    REQUIRE(session.SetPermission(PermissionState::DeniedPermanently) == InputAdmission::Accepted);
    REQUIRE(session.Sensor(SensorKind::StepCounter).availability == SensorAvailability::PermissionRequired);
    REQUIRE(session.SetPermission(PermissionState::Granted) == InputAdmission::Accepted);
    REQUIRE(session.Sample(SensorKind::StepCounter, {}, 100, 9'007'199'254'740'993ULL) == InputAdmission::Accepted);
    const auto &first = collector.Commit();
    CHECK(first.sensors[2].counter == 9'007'199'254'740'993ULL);
    collector.BeginFrame(2);
    REQUIRE(session.SetPermission(PermissionState::Revoked) == InputAdmission::Accepted);
    const auto &revoked = collector.Commit();
    CHECK(revoked.sensors[2].availability == SensorAvailability::PermissionRequired);
    CHECK(revoked.sensors[2].counter == 0);
    CHECK(revoked.sensors[2].timestampNanoseconds == 0);
    collector.BeginFrame(3);
    REQUIRE(session.ConfigureSensor(SensorKind::Accelerometer, true, true) == InputAdmission::Accepted);
    REQUIRE(session.Sample(SensorKind::Accelerometer, {1, 2, 3}, 101) == InputAdmission::Accepted);
    REQUIRE(session.Sample(SensorKind::Accelerometer, {1, 2, 3}, 100) == InputAdmission::Invalid);
    REQUIRE(session.Sample(SensorKind::Accelerometer, {std::numeric_limits<float>::infinity(), 0, 0}, 102) == InputAdmission::Invalid);
    REQUIRE(session.SetFocus(true, false) == InputAdmission::Accepted);
    const auto &lost = collector.Commit();
    CHECK(lost.sensors[0].availability == SensorAvailability::Suspended);
    CHECK(lost.sensors[0].timestampNanoseconds == 0);
    CHECK((lost.sensors[0].value == std::array<float, 3>{}));
}

TEST_CASE("Android input bounds device admission and rejects stale malformed foreign-thread and retired publication") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    for (std::int32_t i = 0; i < 4; ++i)
        REQUIRE(session.ConnectGamepad(i) == InputAdmission::Accepted);
    CHECK(session.ConnectGamepad(4) == InputAdmission::Capacity);
    CHECK(session.Axis(99, Input::GamepadAxis::LeftX, 1) == InputAdmission::Stale);
    CHECK(session.Key(Input::Key::Count, true) == InputAdmission::Invalid);
    CHECK(session.ConfigureSensor(SensorKind::Count, true, true) == InputAdmission::Invalid);
    CHECK(session.ConfigureSensor(SensorKind::Gyroscope, false, true) == InputAdmission::Accepted);
    CHECK(session.Sensor(SensorKind::Gyroscope).availability == SensorAvailability::Unsupported);
    InputAdmission crossThread{};
    std::thread worker([&] {
        crossThread = session.Key(Input::Key::B, true);
    });
    worker.join();
    CHECK(crossThread == InputAdmission::WrongThread);
    REQUIRE(session.ConfigureSensor(SensorKind::Gyroscope, true, true) == InputAdmission::Accepted);
    REQUIRE(session.FailSensor(SensorKind::Gyroscope) == InputAdmission::Accepted);
    CHECK(session.Sensor(SensorKind::Gyroscope).availability == SensorAvailability::NativeFailure);
    session.Shutdown();
    CHECK(session.Key(Input::Key::B, true) == InputAdmission::Stopped);
    CHECK(collector.Commit().gamepads.empty());
}

TEST_CASE("Android gamepad hats and physical direction keys retain independent held evidence") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    REQUIRE(session.ConnectGamepad(9) == InputAdmission::Accepted);
    const auto up = static_cast<std::size_t>(Input::GamepadButton::DPadUp);
    REQUIRE(session.Button(9, Input::GamepadButton::DPadUp, true) == InputAdmission::Accepted);
    REQUIRE(session.Hat(9, 0, 0) == InputAdmission::Accepted);
    CHECK(collector.Commit().gamepads[0].buttons[up].down);
    collector.BeginFrame(2);
    REQUIRE(session.Hat(9, 0, -1) == InputAdmission::Accepted);
    REQUIRE(session.Button(9, Input::GamepadButton::DPadUp, false) == InputAdmission::Accepted);
    CHECK(collector.Commit().gamepads[0].buttons[up].down);
    collector.BeginFrame(3);
    REQUIRE(session.Hat(9, 0, 0) == InputAdmission::Accepted);
    CHECK_FALSE(collector.Commit().gamepads[0].buttons[up].down);
    collector.BeginFrame(4);
    REQUIRE(session.Button(9, Input::GamepadButton::DPadUp, true) == InputAdmission::Accepted);
    REQUIRE(session.SetFocus(false, false) == InputAdmission::Accepted);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    REQUIRE(session.Hat(9, 0, 0) == InputAdmission::Accepted);
    CHECK_FALSE(collector.Commit().gamepads[0].buttons[up].down);
    CHECK(session.Hat(9, std::numeric_limits<float>::infinity(), 0) == InputAdmission::Invalid);
    CHECK(session.Hat(99, 0, 0) == InputAdmission::Stale);
}

TEST_CASE("Android permission loss neutralizes held controls and all prior samples without requiring a focus callback") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    REQUIRE(session.SetPermission(PermissionState::Granted) == InputAdmission::Accepted);
    REQUIRE(session.ConfigureSensor(SensorKind::Accelerometer, true, true) == InputAdmission::Accepted);
    REQUIRE(session.Sample(SensorKind::Accelerometer, {1, 2, 3}, 100) == InputAdmission::Accepted);
    REQUIRE(session.Key(Input::Key::A, true) == InputAdmission::Accepted);
    REQUIRE(session.Touch({1, 1}, 3, 4, true) == InputAdmission::Accepted);
    REQUIRE(session.ConnectGamepad(9) == InputAdmission::Accepted);
    REQUIRE(session.Button(9, Input::GamepadButton::DPadUp, true) == InputAdmission::Accepted);
    static_cast<void>(collector.Commit());
    collector.BeginFrame(2);
    REQUIRE(session.SetPermission(PermissionState::Revoked) == InputAdmission::Accepted);
    const auto &revoked = collector.Commit();
    CHECK_FALSE(revoked.State(Input::Key::A).down);
    CHECK(revoked.State(Input::Key::A).released);
    CHECK(revoked.touches[0].cancelled);
    CHECK_FALSE(revoked.gamepads[0].buttons[static_cast<std::size_t>(Input::GamepadButton::DPadUp)].down);
    CHECK(revoked.sensors[0].availability == SensorAvailability::AwaitingSample);
    CHECK(revoked.sensors[0].timestampNanoseconds == 0);
    CHECK((revoked.sensors[0].value == std::array<float, 3>{}));
    collector.BeginFrame(3);
    REQUIRE(session.Hat(9, 0, 0) == InputAdmission::Accepted);
    REQUIRE(session.Key(Input::Key::B, true) == InputAdmission::Accepted);
    const auto &fresh = collector.Commit();
    CHECK_FALSE(fresh.gamepads[0].buttons[static_cast<std::size_t>(Input::GamepadButton::DPadUp)].down);
    CHECK(fresh.State(Input::Key::B).down);
}

TEST_CASE("Android sensor recovery requires fresh evidence after disabled failed and suspended states") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    CHECK(session.Permission() == PermissionState::NotRequested);
    CHECK(session.SetPermission(static_cast<PermissionState>(255)) == InputAdmission::Invalid);
    CHECK(session.Permission() == PermissionState::NotRequested);
    CHECK(session.FailSensor(SensorKind::Count) == InputAdmission::Invalid);
    CHECK(session.Sample(SensorKind::Count, {}, 1) == InputAdmission::Invalid);
    CHECK(session.Sensor(SensorKind::Count).availability == SensorAvailability::Unsupported);
    REQUIRE(session.ConfigureSensor(SensorKind::Gyroscope, true, false) == InputAdmission::Accepted);
    CHECK(session.Sensor(SensorKind::Gyroscope).availability == SensorAvailability::Disabled);
    CHECK(session.Sample(SensorKind::Gyroscope, {1, 2, 3}, 1) == InputAdmission::Unavailable);
    REQUIRE(session.ConfigureSensor(SensorKind::Gyroscope, true, true) == InputAdmission::Accepted);
    CHECK(session.Sensor(SensorKind::Gyroscope).availability == SensorAvailability::AwaitingSample);
    REQUIRE(session.Sample(SensorKind::Gyroscope, {1, 2, 3}, 10) == InputAdmission::Accepted);
    REQUIRE(session.FailSensor(SensorKind::Gyroscope) == InputAdmission::Accepted);
    CHECK(session.Sensor(SensorKind::Gyroscope).availability == SensorAvailability::NativeFailure);
    CHECK(session.Sensor(SensorKind::Gyroscope).timestampNanoseconds == 0);
    CHECK(session.Sample(SensorKind::Gyroscope, {1, 2, 3}, 11) == InputAdmission::Unavailable);
    REQUIRE(session.ConfigureSensor(SensorKind::Gyroscope, true, true) == InputAdmission::Accepted);
    CHECK(session.Sensor(SensorKind::Gyroscope).availability == SensorAvailability::AwaitingSample);
    REQUIRE(session.SetFocus(false, false) == InputAdmission::Accepted);
    CHECK(session.Sample(SensorKind::Gyroscope, {}, 12) == InputAdmission::Unavailable);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    CHECK(session.Sensor(SensorKind::Gyroscope).availability == SensorAvailability::AwaitingSample);
    REQUIRE(session.Sample(SensorKind::Gyroscope, {4, 5, 6}, 13) == InputAdmission::Accepted);
    const auto &frame = collector.Commit();
    CHECK(frame.sensors[1].timestampNanoseconds == 13);
    CHECK((frame.sensors[1].value == std::array<float, 3>{4, 5, 6}));
}

TEST_CASE("Android malformed devices and controls cannot alter admitted canonical state") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    CHECK(session.ConnectGamepad(-1) == InputAdmission::Invalid);
    CHECK(session.DisconnectGamepad(-1) == InputAdmission::Stale);
    CHECK(session.DisconnectGamepad(99) == InputAdmission::Stale);
    CHECK(session.Button(99, Input::GamepadButton::South, true) == InputAdmission::Stale);
    REQUIRE(session.ConnectGamepad(7) == InputAdmission::Accepted);
    REQUIRE(session.ConnectGamepad(7) == InputAdmission::Accepted);
    CHECK(session.Button(7, Input::GamepadButton::Count, true) == InputAdmission::Invalid);
    CHECK(session.Axis(7, Input::GamepadAxis::Count, 1) == InputAdmission::Invalid);
    CHECK(session.Axis(7, Input::GamepadAxis::LeftX, std::numeric_limits<float>::quiet_NaN()) == InputAdmission::Invalid);
    CHECK(session.Key(Input::Key::Unknown, true) == InputAdmission::Invalid);
    REQUIRE(session.Modifiers({.shift = true}) == InputAdmission::Accepted);
    const auto &frame = collector.Commit();
    REQUIRE(frame.gamepads.size() == 1);
    CHECK(frame.gamepads[0].axes[0] == 0);
    CHECK_FALSE(frame.gamepads[0].buttons[0].down);
    CHECK(frame.modifiers.shift);
}

TEST_CASE("Android touch admission preserves stale cancellation and capacity outcomes") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    CHECK(session.Touch({}, 0, 0, true) == InputAdmission::Invalid);
    CHECK(session.Touch({1, 1}, 0, 0, false) == InputAdmission::Stale);
    REQUIRE(session.Touch({1, 1}, 2, 3, true) == InputAdmission::Accepted);
    session.CancelTouches();
    CHECK(session.Touch({1, 1}, 2, 3, true) == InputAdmission::Stale);
    CHECK(collector.Commit().touches[0].cancelled);
    collector.BeginFrame(2);
    for (std::uint64_t i = 1; i <= Input::MaximumTouchContacts; ++i)
        REQUIRE(session.Touch({i, 2}, 2, 3, true) == InputAdmission::Accepted);
    CHECK(session.Touch({Input::MaximumTouchContacts + 1, 2}, 2, 3, true) == InputAdmission::Capacity);
    const auto &frame = collector.Commit();
    CHECK(frame.touchOverflow);
    for (const auto &touch : frame.touches) {
        CHECK(touch.cancelled);
        CHECK_FALSE(touch.contact.down);
    }
}

TEST_CASE("Android foreign thread callbacks and retired session paths leave borrowed Input untouched") {
    Input::RawInputCollector collector;
    AndroidInputSession session{collector};
    collector.BeginFrame(1);
    REQUIRE(session.SetFocus(true, true) == InputAdmission::Accepted);
    REQUIRE(session.SetPermission(PermissionState::Granted) == InputAdmission::Accepted);
    std::array<InputAdmission, 11> outcomes{};
    SensorSample foreignSample;
    PermissionState foreignPermission{};
    std::thread worker([&session, &outcomes, &foreignSample, &foreignPermission] {
        outcomes = {session.SetFocus(false, false),
                    session.SetPermission(PermissionState::Revoked),
                    session.Modifiers({.shift = true}),
                    session.Touch({1, 1}, 1, 1, true),
                    session.ConnectGamepad(1),
                    session.DisconnectGamepad(1),
                    session.Button(1, Input::GamepadButton::South, true),
                    session.Hat(1, 0, 0),
                    session.Axis(1, Input::GamepadAxis::LeftX, 1),
                    session.ConfigureSensor(SensorKind::Gyroscope, true, true),
                    session.FailSensor(SensorKind::Gyroscope)};
        foreignSample = session.Sensor(SensorKind::Gyroscope);
        foreignPermission = session.Permission();
        session.CancelTouches();
        session.Shutdown();
    });
    worker.join();
    for (const auto outcome : outcomes)
        CHECK(outcome == InputAdmission::WrongThread);
    CHECK(foreignSample.availability == SensorAvailability::Unsupported);
    CHECK(foreignPermission == PermissionState::NotRequested);
    CHECK(session.Permission() == PermissionState::Granted);
    const auto &frame = collector.Commit();
    CHECK(frame.window.focused);
    CHECK_FALSE(frame.modifiers.shift);
    CHECK(frame.gamepads.empty());
    session.Shutdown();
    CHECK(session.SetFocus(true, true) == InputAdmission::Stopped);
    CHECK(session.Sensor(SensorKind::Gyroscope).availability == SensorAvailability::Unsupported);
    session.Shutdown();
}
