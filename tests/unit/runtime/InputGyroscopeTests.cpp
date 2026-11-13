#include "Horo/Runtime/Input.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace Horo::Input {
    namespace {
        /** @brief Supplies real canonical profiles and collection, without a sensor/backend test authority. */
        struct GyroscopeFixture final {
            InputService input;
            InputContextToken context;
            ActionId action{"play.look"};
            std::uint64_t frame{};

            explicit GyroscopeFixture(std::vector<InputBinding> bindings) {
                REQUIRE(input.Router()
                            .SetActionMap({{action, ActionValueType::Axis2D, InputContextId{"play"}, true, std::move(bindings)}})
                            .HasValue());
                context = input.Router().PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
            }

            void Sample(const SensorAvailability availability, const std::array<float, 3> values, const bool focused = true) {
                input.Router().EndFrame();
                input.BeginFrame(++frame);
                input.Collector().SetWindowState({.focused = focused});
                REQUIRE(
                    input.Collector().SetSensorSample(SensorKind::Gyroscope, {.availability = availability,
                                                                              .value = values,
                                                                              .timestampNanoseconds = static_cast<std::int64_t>(frame)}));
                static_cast<void>(input.CommitFrame());
            }

            ActionEvidence Read() {
                return input.Router().ReadActionEvidence(context, action);
            }
        };

        InputBinding GyroscopeBinding(const SensorAxis axis, const float calibration = 1.0F) {
            InputBinding binding{.kind = BindingControlKind::GyroscopeAxis};
            binding.gyroscope = {axis, calibration};
            return binding;
        }
    }  // namespace

    TEST_CASE("Gyroscope actions normalize SI rate apply sensitivity and retain raw facts without frame allocation",
              "[input][accessibility][gyroscope][allocation]") {
        auto yaw = GyroscopeBinding(SensorAxis::Y, 2);
        yaw.scale = 0.5F;
        auto pitch = GyroscopeBinding(SensorAxis::X, 4);
        pitch.component = 1;
        pitch.scale = -1;
        GyroscopeFixture fixture{{yaw, pitch}};
        fixture.Sample(SensorAvailability::Available, {4, 1, 0});
        ActionEvidence first, repeated;
        Horo::Tests::AllocationProbe::Measurement allocations;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            first = fixture.Read();
            repeated = fixture.Read();
            allocations = probe.Snapshot();
        }
        REQUIRE(allocations.requests == 0);
        REQUIRE(first.value.x == Catch::Approx(0.25F));
        REQUIRE(first.value.y == Catch::Approx(-1));
        REQUIRE(first.value.pressed);
        REQUIRE_FALSE(repeated.value.pressed);
        REQUIRE(repeated.value.x == first.value.x);
        REQUIRE(repeated.value.y == first.value.y);
        REQUIRE(first.source.has_value());
        REQUIRE(first.source->modality == InputModality::Motion);
        REQUIRE_FALSE(first.source->gamepad.has_value());
        const auto &sensor = fixture.input.Router().Snapshot().sensors[static_cast<std::size_t>(SensorKind::Gyroscope)];
        REQUIRE(sensor.value[0] == 4);
        REQUIRE(sensor.value[1] == 1);
        REQUIRE_FALSE(fixture.input.Router().Snapshot().sampleTime.has_value());
    }

    TEST_CASE("Gyroscope availability and focus loss neutralize motion through the normal router",
              "[input][accessibility][gyroscope][lifecycle]") {
        GyroscopeFixture fixture{{GyroscopeBinding(SensorAxis::Z)}};
        for (const auto state : {SensorAvailability::Unsupported, SensorAvailability::Disabled, SensorAvailability::PermissionRequired,
                                 SensorAvailability::Suspended, SensorAvailability::AwaitingSample, SensorAvailability::NativeFailure}) {
            fixture.Sample(SensorAvailability::Available, {0, 0, 1});
            REQUIRE(fixture.Read().value.x == 1);
            fixture.Sample(state, {0, 0, 1});
            const auto unavailable = fixture.Read();
            REQUIRE(unavailable.value.x == 0);
            REQUIRE_FALSE(unavailable.value.down);
            REQUIRE(unavailable.value.released);
        }
        fixture.Sample(SensorAvailability::Available, {0, 0, 1}, false);
        REQUIRE(fixture.Read().value.x == 0);
        auto modal = fixture.input.Router().PushContext(InputContextId{"modal"}, InputContextKind::ModalRoot);
        fixture.Sample(SensorAvailability::Available, {0, 0, 1});
        REQUIRE(fixture.Read().value.x == 0);
    }

    TEST_CASE("Gyroscope saturation and deadzone do not integrate rates or depend on frame numbers",
              "[input][accessibility][gyroscope][numeric]") {
        auto binding = GyroscopeBinding(SensorAxis::X, 2);
        binding.deadzoneKind = DeadzoneKind::Axial;
        binding.deadzone = 0.25F;
        GyroscopeFixture fixture{{binding}};
        fixture.Sample(SensorAvailability::Available, {0.25F, 0, 0});
        REQUIRE(fixture.Read().value.x == 0);
        fixture.Sample(SensorAvailability::Available, {1, 0, 0});
        REQUIRE(fixture.Read().value.x == Catch::Approx(1.0F / 3));
        fixture.frame = 900000;
        fixture.Sample(SensorAvailability::Available, {1, 0, 0});
        REQUIRE(fixture.Read().value.x == Catch::Approx(1.0F / 3));
        fixture.Sample(SensorAvailability::Available, {-std::numeric_limits<float>::max(), 0, 0});
        REQUIRE(fixture.Read().value.x == -1);
    }

    TEST_CASE("Gyroscope bindings use the canonical codec and reject malformed calibration before installation",
              "[input][accessibility][gyroscope][profile]") {
        auto binding = GyroscopeBinding(SensorAxis::Z, 3.25F);
        binding.scale = -0.75F;
        InputBindingProfile profile{.profileId = "gyro", .overrides = {{ActionId{"play.look"}, {binding}}}};
        const auto encoded = SerializeBindingProfile(profile);
        REQUIRE(encoded.HasValue());
        const auto decoded = ParseBindingProfile(encoded.Value());
        REQUIRE(decoded.HasValue());
        REQUIRE(decoded.Value().overrides[0].bindings[0] == binding);
        const auto document = nlohmann::json::parse(encoded.Value());
        for (const auto &axis : {nlohmann::json(-1), nlohmann::json(3), nlohmann::json(256), nlohmann::json(1.5), nlohmann::json(true),
                                 nlohmann::json(std::numeric_limits<std::uint64_t>::max())}) {
            auto invalid = document;
            invalid["overrides"][0]["bindings"][0]["gyroscope"]["axis"] = axis;
            REQUIRE(ParseBindingProfile(invalid.dump()).HasError());
        }
        for (const auto &rate : {nlohmann::json(0), nlohmann::json(0.00001), nlohmann::json(1001), nlohmann::json(1000.00000001),
                                 nlohmann::json("1"), nlohmann::json(true)}) {
            auto invalid = document;
            invalid["overrides"][0]["bindings"][0]["gyroscope"]["radiansPerSecondAtFullScale"] = rate;
            REQUIRE(ParseBindingProfile(invalid.dump()).HasError());
        }
        auto legacy = document;
        legacy["schemaVersion"] = 1;
        legacy["overrides"][0]["bindings"][0].erase("accessibility");
        REQUIRE(ParseBindingProfile(legacy.dump()).HasError());
        for (const auto &calibration : {nlohmann::json(nullptr), nlohmann::json::object(),
                                        nlohmann::json{{"axis", 0}, {"radiansPerSecondAtFullScale", 1}, {"unknown", 1}}}) {
            auto invalid = document;
            invalid["overrides"][0]["bindings"][0]["gyroscope"] = calibration;
            REQUIRE(ParseBindingProfile(invalid.dump()).HasError());
        }
        auto foreign = document;
        foreign["overrides"][0]["bindings"][0]["kind"] = static_cast<unsigned>(BindingControlKind::Key);
        foreign["overrides"][0]["bindings"][0]["key"] = static_cast<unsigned>(Key::K);
        REQUIRE(ParseBindingProfile(foreign.dump()).HasError());
        for (const auto &kind : {nlohmann::json(-248), nlohmann::json(264), nlohmann::json(8.0), nlohmann::json(true)}) {
            auto invalid = document;
            invalid["overrides"][0]["bindings"][0]["kind"] = kind;
            REQUIRE(ParseBindingProfile(invalid.dump()).HasError());
        }
        profile.overrides[0].bindings[0].gyroscope.radiansPerSecondAtFullScale = 0;
        REQUIRE(SerializeBindingProfile(profile).HasError());
    }

    TEST_CASE("Malformed gyro publications cannot leak nonfinite or untimed motion through a direct raw snapshot",
              "[input][accessibility][gyroscope][validation]") {
        RawInputSnapshot raw;
        InputRouter router;
        const ActionId action{"play.look"};
        REQUIRE(router.SetActionMap({{action, ActionValueType::Axis1D, InputContextId{"play"}, true, {GyroscopeBinding(SensorAxis::X)}}})
                    .HasValue());
        auto context = router.PushContext(InputContextId{"play"}, InputContextKind::Gameplay);
        auto &sample = raw.sensors[static_cast<std::size_t>(SensorKind::Gyroscope)];
        sample = {.availability = SensorAvailability::Available, .value = {1, 0, 0}, .timestampNanoseconds = 0};
        router.BeginFrame(raw);
        REQUIRE(router.ReadAction(context, action).x == 0);
        sample.timestampNanoseconds = 1;
        sample.value[2] = std::numeric_limits<float>::infinity();
        router.BeginFrame(raw);
        REQUIRE(router.ReadAction(context, action).x == 0);
    }

    TEST_CASE("Inactive binding fields cannot disguise a duplicate physical control or sticky overlap",
              "[input][accessibility][bindings][conflicts]") {
        const InputContextId context{"play"};
        InputRouter router;
        for (const auto &first :
             {InputBinding{.kind = BindingControlKind::Key, .key = Key::K}, InputBinding{.kind = BindingControlKind::PointerButton},
              InputBinding{.kind = BindingControlKind::PointerWheelX}, InputBinding{.kind = BindingControlKind::PointerWheelY},
              InputBinding{.kind = BindingControlKind::GamepadButton}, InputBinding{.kind = BindingControlKind::GamepadAxis},
              InputBinding{.kind = BindingControlKind::RawGamepadButton}, InputBinding{.kind = BindingControlKind::RawGamepadAxis},
              GyroscopeBinding(SensorAxis::Y)}) {
            INFO(static_cast<unsigned>(first.kind));
            auto alias = first;
            if (first.kind == BindingControlKind::Key)
                alias.pointerButton = PointerButton::Secondary;
            else
                alias.key = Key::K;
            REQUIRE(router.SetActionMap({{ActionId{"play.first"}, ActionValueType::Axis2D, context, true, {first}}}).HasValue());
            REQUIRE(router.SetActionMap({{ActionId{"play.second"}, ActionValueType::Axis2D, context, true, {alias}}}).HasValue());
            const auto revision = router.RoutingState({}).configurationRevision;
            REQUIRE(router.SetActionMap({{ActionId{"play.first"}, ActionValueType::Axis2D, context, true, {first, alias}}}).HasError());
            REQUIRE(router
                        .SetActionMap({{ActionId{"play.first"}, ActionValueType::Axis2D, context, true, {first}},
                                       {ActionId{"play.second"}, ActionValueType::Axis2D, context, true, {alias}}})
                        .HasError());
            REQUIRE(router.RoutingState({}).configurationRevision == revision);
        }
        InputBinding sticky{.kind = BindingControlKind::Key, .key = Key::K, .requiredModifiers = {.control = true}};
        sticky.accessibility.stickyModifiers = true;
        InputBinding plain{.kind = BindingControlKind::Key, .key = Key::K, .pointerButton = PointerButton::Secondary};
        REQUIRE(router
                    .SetActionMap({{ActionId{"play.modified"}, ActionValueType::Digital, context, true, {sticky}},
                                   {ActionId{"play.plain"}, ActionValueType::Digital, context, true, {plain}}})
                    .HasError());
    }

    TEST_CASE("Gyroscope conflicts cannot be hidden by calibration sensitivity or output component changes",
              "[input][accessibility][gyroscope][validation]") {
        auto first = GyroscopeBinding(SensorAxis::Y);
        auto second = GyroscopeBinding(SensorAxis::Y, 2);
        second.scale = -1;
        second.component = 1;
        InputRouter router;
        REQUIRE(router
                    .SetActionMap({{ActionId{"play.look"}, ActionValueType::Axis2D, InputContextId{"play"}, true, {first}},
                                   {ActionId{"play.other"}, ActionValueType::Axis2D, InputContextId{"play"}, true, {second}}})
                    .HasError());
        REQUIRE(router.SetActionMap({{ActionId{"play.look"}, ActionValueType::Axis2D, InputContextId{"play"}, true, {first, second}}})
                    .HasError());
        REQUIRE(router.SetActionMap({{ActionId{"play.fire"}, ActionValueType::Digital, InputContextId{"play"}, true, {first}}}).HasError());
        first.gyroscope.axis = SensorAxis::Count;
        REQUIRE(router.SetActionMap({{ActionId{"play.look"}, ActionValueType::Axis1D, InputContextId{"play"}, true, {first}}}).HasError());
        first.gyroscope.axis = SensorAxis::Y;
        first.accessibility.digital.toggle = true;
        REQUIRE(router.SetActionMap({{ActionId{"play.look"}, ActionValueType::Axis1D, InputContextId{"play"}, true, {first}}}).HasError());
    }
}  // namespace Horo::Input
