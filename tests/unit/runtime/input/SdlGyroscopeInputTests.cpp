#include "SdlInputBackend.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>

namespace {
    using namespace Horo::Input;

    /** @brief Process-local headless SDL video ownership; no native display or graphics context is required. */
    struct HiddenVideo final {
        HiddenVideo() {
            REQUIRE(SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE));
            REQUIRE(SDL_InitSubSystem(SDL_INIT_VIDEO));
        }

        ~HiddenVideo() {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
        }

        HiddenVideo(const HiddenVideo &) = delete;
        HiddenVideo &operator=(const HiddenVideo &) = delete;
    };

    /** @brief Real SDL virtual device; backend native enable calls and queued samples use SDL's ordinary driver path. */
    struct MotionDevice final {
        SDL_JoystickID native{};
        SDL_Joystick *joystick{};
        unsigned enables{}, disables{};
        bool permitEnable{};
        FrameNumber frame{};

        explicit MotionDevice(const bool sensor = true, const bool permit = true) : permitEnable(permit) {
            REQUIRE(SDL_InitSubSystem(SDL_INIT_GAMEPAD));
            const SDL_VirtualJoystickSensorDesc gyro{SDL_SENSOR_GYRO, 100.0F};
            SDL_VirtualJoystickDesc description{};
            SDL_INIT_INTERFACE(&description);
            description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
            description.naxes = SDL_GAMEPAD_AXIS_COUNT;
            description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
            description.axis_mask = (1U << SDL_GAMEPAD_AXIS_COUNT) - 1;
            description.button_mask = (1U << SDL_GAMEPAD_BUTTON_COUNT) - 1;
            description.name = "ACC virtual gyro";
            description.nsensors = sensor ? 1 : 0;
            description.sensors = sensor ? &gyro : nullptr;
            description.userdata = this;
            description.SetSensorsEnabled = [](void *data, const bool enabled) {
                auto &owner = *static_cast<MotionDevice *>(data);
                if (enabled) {
                    ++owner.enables;
                    return owner.permitEnable;
                }
                ++owner.disables;
                return true;
            };
            native = SDL_AttachVirtualJoystick(&description);
            REQUIRE(native != 0);
            REQUIRE(SDL_IsGamepad(native));
            joystick = SDL_OpenJoystick(native);
            REQUIRE(joystick != nullptr);
        }

        ~MotionDevice() {
            if (joystick)
                SDL_CloseJoystick(joystick);
            if (native)
                (void)SDL_DetachVirtualJoystick(native);
            SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
        }

        MotionDevice(const MotionDevice &) = delete;
        MotionDevice &operator=(const MotionDevice &) = delete;

        void Begin(SdlInputBackend &backend) {
            backend.BeginFrame(++frame);
        }

        const RawInputSnapshot &Pump(SdlInputBackend &backend) {
            Begin(backend);
            backend.PollEvents();
            return backend.Commit();
        }

        GamepadDeviceId Id(const RawInputSnapshot &raw) const {
            const auto found = std::ranges::find(raw.gamepads, "ACC virtual gyro", &GamepadState::name);
            REQUIRE(found != raw.gamepads.end());
            return found->id;
        }

        void Send(const std::uint64_t timestamp, const std::array<float, 3> values = {1, 2, 3}) const {
            REQUIRE(SDL_SendJoystickVirtualSensorData(joystick, SDL_SENSOR_GYRO, timestamp, values.data(), 3));
        }

        SDL_Event Event(const std::uint64_t timestamp) const {
            SDL_Event event{};
            event.type = SDL_EVENT_GAMEPAD_SENSOR_UPDATE;
            event.gsensor.which = native;
            event.gsensor.sensor = SDL_SENSOR_GYRO;
            event.gsensor.timestamp = SDL_GetTicksNS();
            event.gsensor.sensor_timestamp = timestamp;
            event.gsensor.data[1] = 2;
            return event;
        }
    };

    TEST_CASE("SDL controller gyro is opt in and preserves native SI units independent nanoseconds and player evidence",
              "[unit][input][sdl][gyroscope]") {
        MotionDevice device;
        SdlInputBackend backend;
        backend.BindWindow(7);
        const auto id = device.Id(device.Pump(backend));
        CHECK(device.enables == 0);
        backend.SetGamepadGyroscopeEnabled(true);
        CHECK(device.enables == 0);
        CHECK(device.Pump(backend).FindGamepad(id)->gyroscope.availability == SensorAvailability::AwaitingSample);
        REQUIRE(device.enables == 1);
        SDL_Delay(1);
        device.Send(1'234'567'890);
        const auto &raw = device.Pump(backend);
        const auto sample = raw.FindGamepad(id)->gyroscope;
        REQUIRE(sample.availability == SensorAvailability::Available);
        CHECK((sample.value == std::array<float, 3>{1, 2, 3}));
        CHECK(sample.timestampNanoseconds == 1'234'567'890);
        CHECK(raw.sensors[static_cast<std::size_t>(SensorKind::Gyroscope)].availability == SensorAvailability::Unsupported);
        InputRouter router;
        const ActionId look{"look"};
        const InputContextId play{"play"};
        REQUIRE(
            router
                .SetActionMap({{look,
                                ActionValueType::Axis1D,
                                play,
                                true,
                                {{.kind = BindingControlKind::GyroscopeAxis, .gyroscope = {SensorAxis::Y, 4, GyroscopeSource::Gamepad}}}}})
                .HasValue());
        router.BeginFrame(raw);
        REQUIRE(router.AssignGamepad(0, id));
        auto context = router.PushContext(play, InputContextKind::Gameplay);
        const auto evidence = router.ReadActionEvidence(context, look, 0);
        CHECK(evidence.value.x == 0.5F);
        REQUIRE(evidence.source.has_value());
        CHECK(evidence.source->gamepad == id);
        CHECK(evidence.source->modality == InputModality::Motion);
    }

    TEST_CASE("SDL gyro disable and reenable reject queued old epoch samples before accepting a fresh clock domain",
              "[unit][input][sdl][gyroscope][lifecycle]") {
        MotionDevice device;
        SdlInputBackend backend;
        backend.BindWindow(7);
        backend.SetGamepadGyroscopeEnabled(true);
        const auto id = device.Id(device.Pump(backend));
        SDL_Delay(1);
        device.Send(9000);
        REQUIRE(device.Pump(backend).FindGamepad(id)->gyroscope.availability == SensorAvailability::Available);
        const auto queued = device.Event(10000);
        backend.SetGamepadGyroscopeEnabled(false);
        CHECK(device.Pump(backend).FindGamepad(id)->gyroscope.availability == SensorAvailability::Disabled);
        CHECK(device.disables == 1);
        backend.SetGamepadGyroscopeEnabled(true);
        device.Begin(backend);
        backend.ProcessEvent(queued);
        CHECK(backend.Commit().FindGamepad(id)->gyroscope.availability == SensorAvailability::AwaitingSample);
        SDL_Delay(1);
        device.Send(1);
        const auto fresh = device.Pump(backend).FindGamepad(id)->gyroscope;
        CHECK(fresh.availability == SensorAvailability::Available);
        CHECK(fresh.timestampNanoseconds == 1);
        CHECK(device.enables == 2);
    }

    TEST_CASE("SDL gyro unsupported and native enable failure remain unavailable without falling back",
              "[unit][input][sdl][gyroscope][availability]") {
        for (const bool supported : {false, true}) {
            MotionDevice device{supported, false};
            SdlInputBackend backend;
            backend.BindWindow(7);
            backend.SetGamepadGyroscopeEnabled(true);
            const auto &raw = device.Pump(backend);
            const auto id = device.Id(raw);
            const auto expected = supported ? SensorAvailability::NativeFailure : SensorAvailability::Unsupported;
            CHECK(raw.FindGamepad(id)->gyroscope.availability == expected);
            SDL_Delay(1);
            device.Begin(backend);
            backend.ProcessEvent(device.Event(1));
            CHECK(backend.Commit().FindGamepad(id)->gyroscope.availability == expected);
            CHECK(device.enables == (supported ? 1 : 0));
        }
    }

    TEST_CASE("SDL gyro focus loss and window replacement fence queued samples and retire native collection",
              "[unit][input][sdl][gyroscope][focus]") {
        MotionDevice device;
        SdlInputBackend backend;
        backend.BindWindow(7);
        backend.SetGamepadGyroscopeEnabled(true);
        const auto id = device.Id(device.Pump(backend));
        SDL_Event focus{};
        focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        focus.window.windowID = 7;
        device.Begin(backend);
        backend.ProcessEvent(focus);
        CHECK(backend.Commit().FindGamepad(id)->gyroscope.availability == SensorAvailability::Suspended);
        CHECK(device.disables == 1);
        focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        device.Begin(backend);
        backend.ProcessEvent(focus);
        CHECK(backend.Commit().FindGamepad(id)->gyroscope.availability == SensorAvailability::AwaitingSample);
        SDL_Delay(1);
        const auto queued = device.Event(5);
        backend.BindWindow(9);
        device.Begin(backend);
        backend.ProcessEvent(queued);
        CHECK(backend.Commit().FindGamepad(id)->gyroscope.availability == SensorAvailability::AwaitingSample);
        SDL_Delay(1);
        device.Send(6);
        CHECK(device.Pump(backend).FindGamepad(id)->gyroscope.availability == SensorAvailability::Available);
        backend.SetGamepadGyroscopeEnabled(false);
        CHECK(device.Pump(backend).FindGamepad(id)->gyroscope.availability == SensorAvailability::Disabled);
    }

    TEST_CASE("SDL malformed gyro samples stay neutral and fresh valid evidence can recover", "[unit][input][sdl][gyroscope][validation]") {
        MotionDevice device;
        SdlInputBackend backend;
        backend.BindWindow(7);
        backend.SetGamepadGyroscopeEnabled(true);
        const auto id = device.Id(device.Pump(backend));
        SDL_Delay(1);
        for (const auto stamp : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
            device.Begin(backend);
            backend.ProcessEvent(device.Event(stamp));
            const auto sample = backend.Commit().FindGamepad(id)->gyroscope;
            CHECK(sample.availability == SensorAvailability::NativeFailure);
            CHECK(sample.timestampNanoseconds == 0);
        }
        device.Begin(backend);
        auto malformed = device.Event(1);
        malformed.gsensor.data[0] = std::numeric_limits<float>::quiet_NaN();
        backend.ProcessEvent(malformed);
        CHECK(backend.Commit().FindGamepad(id)->gyroscope.availability == SensorAvailability::NativeFailure);
        SDL_Delay(1);
        device.Send(1);
        CHECK(device.Pump(backend).FindGamepad(id)->gyroscope.availability == SensorAvailability::Available);
    }

    TEST_CASE("SDL backend retirement disables controller sensors it activated", "[unit][input][sdl][gyroscope][retirement]") {
        MotionDevice device;
        {
            SdlInputBackend backend;
            backend.BindWindow(7);
            backend.SetGamepadGyroscopeEnabled(true);
            (void)device.Pump(backend);
            REQUIRE(device.enables == 1);
            REQUIRE(device.disables == 0);
        }
        CHECK(device.disables == 1);
    }

    TEST_CASE("SDL initially unfocused windows cannot enable requested controller motion before a focus event",
              "[unit][input][sdl][gyroscope][focus]") {
        HiddenVideo video;
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window{SDL_CreateWindow("ACC hidden sensor surface", 64, 64,
                                                                                          SDL_WINDOW_HIDDEN),
                                                                         SDL_DestroyWindow};
        REQUIRE(window != nullptr);
        REQUIRE((SDL_GetWindowFlags(window.get()) & SDL_WINDOW_INPUT_FOCUS) == 0);
        MotionDevice device;
        SdlInputBackend backend;
        backend.BindWindow(SDL_GetWindowID(window.get()));
        backend.SetGamepadGyroscopeEnabled(true);
        const auto &raw = device.Pump(backend);
        const auto id = device.Id(raw);
        CHECK_FALSE(raw.window.focused);
        CHECK(raw.FindGamepad(id)->gyroscope.availability == SensorAvailability::Suspended);
        CHECK(device.enables == 0);
    }
}  // namespace
