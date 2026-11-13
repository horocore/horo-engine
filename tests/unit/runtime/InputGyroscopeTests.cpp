#include "Horo/Runtime/Input.h"
#include "support/AllocationProbe.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <utility>

namespace Horo::Input {
    TEST_CASE("Zero detection thresholds preserve neutral gyro and stick rates and their real edges",
              "[input][accessibility][gyroscope][neutral]") {
        const std::array bindings{InputBinding{.kind = BindingControlKind::GyroscopeAxis},
                                  InputBinding{.kind = BindingControlKind::GyroscopeAxis,
                                               .gyroscope = {SensorAxis::X, 1, GyroscopeSource::Gamepad}},
                                  InputBinding{.kind = BindingControlKind::GamepadAxis}};
        for (auto binding : bindings) {
            binding.digitalThreshold = 0;
            binding.deadzone = 0;
            InputRouter router;
            const ActionId action{"rate"};
            const InputContextId play{"play"};
            REQUIRE(router.SetActionMap({{action, ActionValueType::Axis1D, play, true, {binding}}}).HasValue());
            auto context = router.PushContext(play, InputContextKind::Gameplay);
            RawInputCollector collector;
            collector.BeginFrame(1);
            const auto id = collector.ConnectGamepad("motion");
            router.BeginFrame(collector.Commit());
            const auto neutral = router.ReadAction(context, action);
            CHECK(neutral.x == 0);
            CHECK_FALSE(neutral.down);
            CHECK_FALSE(neutral.pressed);
            collector.BeginFrame(2);
            const SensorSample moving{SensorAvailability::Available, {0.1F, 0, 0}, 0, 1};
            REQUIRE(collector.SetSensorSample(SensorKind::Gyroscope, moving));
            REQUIRE(collector.SetGamepadGyroscopeSample(id, moving));
            REQUIRE(collector.SetGamepadAxis(id, GamepadAxis::LeftX, 0.1F));
            router.BeginFrame(collector.Commit());
            const auto active = router.ReadAction(context, action);
            CHECK(active.down);
            CHECK(active.pressed);
            collector.BeginFrame(3);
            collector.Neutralize();
            router.BeginFrame(collector.Commit());
            const auto released = router.ReadAction(context, action);
            CHECK(released.x == 0);
            CHECK_FALSE(released.down);
            CHECK(released.released);
        }
    }

    TEST_CASE("Assigned controller gyro uses exact player device ownership and independent physical sources",
              "[input][accessibility][gyroscope][assignment]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        VirtualGamepad first{collector}, second{collector};
        const auto firstId = first.Connect("first"), secondId = second.Connect("second");
        REQUIRE(first.SetGyroscopeSample({SensorAvailability::Available, {0, 1, 0}, 0, 100}));
        REQUIRE(second.SetGyroscopeSample({SensorAvailability::Available, {0, 3, 0}, 0, 100}));
        REQUIRE(collector.SetSensorSample(SensorKind::Gyroscope, {SensorAvailability::Available, {0, -4, 0}, 0, 100}));
        InputBinding controller{.kind = BindingControlKind::GyroscopeAxis,
                                .digitalThreshold = 0.1F,
                                .gyroscope = {SensorAxis::Y, 4, GyroscopeSource::Gamepad}};
        auto platform = controller;
        platform.gyroscope.source = GyroscopeSource::Platform;
        const ActionId look{"look"}, phone{"phone"};
        const InputContextId play{"play"};
        InputRouter router;
        REQUIRE(router
                    .SetActionMap({{look, ActionValueType::Axis1D, play, true, {controller}},
                                   {phone, ActionValueType::Axis1D, play, true, {platform}}})
                    .HasValue());
        router.BeginFrame(collector.Commit());
        REQUIRE(router.AssignGamepad(0, firstId));
        REQUIRE(router.AssignGamepad(1, secondId));
        auto context = router.PushContext(play, InputContextKind::Gameplay);
        ActionEvidence a, b, missing;
        Horo::Tests::AllocationProbe::Measurement allocations;
        {
            Horo::Tests::AllocationProbe::ScopedMeasurement probe;
            a = router.ReadActionEvidence(context, look, 0);
            b = router.ReadActionEvidence(context, look, 1);
            missing = router.ReadActionEvidence(context, look, 2);
            allocations = probe.Snapshot();
        }
        CHECK(allocations.requests == 0);
        CHECK(a.value.x == 0.25F);
        CHECK(b.value.x == 0.75F);
        CHECK(missing.value.x == 0);
        REQUIRE(a.source.has_value());
        REQUIRE(b.source.has_value());
        CHECK(a.source->gamepad == firstId);
        CHECK(b.source->gamepad == secondId);
        CHECK(a.source->modality == InputModality::Motion);
        CHECK(CanonicalGlyph(a.source->glyph).label == "Controller gyroscope Y");
        CHECK(a.value.pressed);
        CHECK(b.value.pressed);
        CHECK_FALSE(router.ReadAction(context, look, 0).pressed);
        CHECK(router.ReadAction(context, phone, 0).x == -1);
        collector.BeginFrame(2);
        first.Disconnect();
        router.BeginFrame(collector.Commit());
        CHECK(router.ReadAction(context, look, 0).x == 0);
        CHECK(router.ReadAction(context, look, 1).x == 0.75F);
    }

    TEST_CASE("Controller gyro sample admission burns stale handles and unavailable timing without reviving unfocused input",
              "[input][accessibility][gyroscope][lifecycle]") {
        RawInputCollector collector;
        collector.BeginFrame(1);
        const auto id = collector.ConnectGamepad("gyro");
        const SensorSample available{SensorAvailability::Available, {1, 2, 3}, 0, 10};
        REQUIRE(collector.SetGamepadGyroscopeSample(id, available));
        auto malformed = available;
        malformed.value[2] = std::numeric_limits<float>::infinity();
        CHECK_FALSE(collector.SetGamepadGyroscopeSample(id, malformed));
        malformed = available;
        malformed.timestampNanoseconds = 9;
        CHECK_FALSE(collector.SetGamepadGyroscopeSample(id, malformed));
        CHECK(collector.Commit().FindGamepad(id)->gyroscope.value == available.value);
        collector.BeginFrame(2);
        REQUIRE(collector.SetGamepadGyroscopeSample(id, {SensorAvailability::Disabled, {1, 2, 3}, 99, 900}));
        const auto cleared = collector.Commit().FindGamepad(id)->gyroscope;
        CHECK(cleared.timestampNanoseconds == 0);
        CHECK(cleared.counter == 0);
        CHECK((cleared.value == std::array<float, 3>{}));
        collector.BeginFrame(3);
        REQUIRE(collector.SetGamepadGyroscopeSample(id, {SensorAvailability::Available, {1, 0, 0}, 0, 1}));
        collector.SetWindowState({.focused = false});
        CHECK_FALSE(collector.SetGamepadGyroscopeSample(id, available));
        CHECK(collector.Commit().FindGamepad(id)->gyroscope.availability == SensorAvailability::Suspended);
        collector.BeginFrame(4);
        collector.SetWindowState({.focused = true});
        REQUIRE(collector.DisconnectGamepad(id));
        const auto replacement = collector.ConnectGamepad("replacement");
        CHECK(replacement != id);
        CHECK_FALSE(collector.SetGamepadGyroscopeSample(id, available));
        const auto &current = collector.Commit();
        REQUIRE(current.FindGamepad(replacement) != nullptr);
        CHECK(current.FindGamepad(replacement)->gyroscope.availability == SensorAvailability::Unsupported);
    }

    TEST_CASE("Controller gyro source round trips canonically without changing old platform profile bytes",
              "[input][accessibility][gyroscope][profile]") {
        InputBinding binding{.kind = BindingControlKind::GyroscopeAxis};
        InputBindingProfile profile{.profileId = "gyro", .overrides = {{ActionId{"look"}, {binding}}}};
        const auto original = SerializeBindingProfile(profile);
        REQUIRE(original.HasValue());
        auto document = nlohmann::json::parse(original.Value());
        CHECK_FALSE(document["overrides"][0]["bindings"][0]["gyroscope"].contains("source"));
        profile.overrides[0].bindings[0].gyroscope.source = GyroscopeSource::Gamepad;
        const auto encoded = SerializeBindingProfile(profile);
        REQUIRE(encoded.HasValue());
        const auto decoded = ParseBindingProfile(encoded.Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value().overrides[0].bindings[0] == profile.overrides[0].bindings[0]);
        for (const auto &source :
             {nlohmann::json(-1), nlohmann::json(2), nlohmann::json(256), nlohmann::json(true), nlohmann::json(1.5), nlohmann::json("1"),
              nlohmann::json(nullptr), nlohmann::json(std::numeric_limits<std::uint64_t>::max())}) {
            document["overrides"][0]["bindings"][0]["gyroscope"]["source"] = source;
            CHECK(ParseBindingProfile(document.dump()).HasError());
        }
        profile.overrides[0].bindings[0].gyroscope.source = GyroscopeSource::Count;
        CHECK(SerializeBindingProfile(profile).HasError());
    }

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
        for (auto first : {InputBinding{.kind = BindingControlKind::GamepadAxis}, InputBinding{.kind = BindingControlKind::RawGamepadAxis},
                           InputBinding{.kind = BindingControlKind::PointerWheelX}, InputBinding{.kind = BindingControlKind::PointerWheelY},
                           GyroscopeBinding(SensorAxis::Y)}) {
            auto second = first;
            first.chord[0] = Key::K;
            first.chordSize = 1;
            REQUIRE(router
                        .SetActionMap({{ActionId{"play.first"}, ActionValueType::Axis2D, context, true, {first}},
                                       {ActionId{"play.second"}, ActionValueType::Axis2D, context, true, {second}}})
                        .HasError());
            second.chord[0] = Key::L;
            second.chordSize = 1;
            REQUIRE(router
                        .SetActionMap({{ActionId{"play.first"}, ActionValueType::Axis2D, context, true, {first}},
                                       {ActionId{"play.second"}, ActionValueType::Axis2D, context, true, {second}}})
                        .HasError());
            first.requiredModifiers.control = true;
            REQUIRE(router
                        .SetActionMap({{ActionId{"play.first"}, ActionValueType::Axis2D, context, true, {first}},
                                       {ActionId{"play.second"}, ActionValueType::Axis2D, context, true, {second}}})
                        .HasValue());
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
