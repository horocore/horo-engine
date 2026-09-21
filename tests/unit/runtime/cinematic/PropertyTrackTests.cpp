#include "Horo/Cinematic/CinematicErrors.h"
#include "Horo/Cinematic/PropertyTrack.h"
#include "Horo/Runtime/Scene/PropertyBindingErrors.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace Horo::Cinematic {
    namespace {
        struct Component final {
            float weight{};
            Math::Vec3 position{};
            bool enabled{};
        };

        [[nodiscard]] constexpr PropertyBindingId Binding(const std::uint64_t value, const std::uint32_t generation = 1) noexcept {
            return {value, generation};
        }

        [[nodiscard]] constexpr TrackId Track(const std::uint64_t value) noexcept {
            return {value, 1};
        }

        [[nodiscard]] constexpr Runtime::SceneObjectId Object(const std::uint64_t value) noexcept {
            return {value};
        }

        [[nodiscard]] Gameplay::ComponentTypeId ComponentType(const std::string_view value = "game.cinematic.test") {
            auto result = Gameplay::ComponentTypeId::Parse(value);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] Gameplay::ComponentPropertyId Property(const std::string_view value) {
            auto result = Gameplay::ComponentPropertyId::Parse(value);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        [[nodiscard]] Result<Runtime::PropertyBindingValue> ReadWeight(const void *component) {
            return Result<Runtime::PropertyBindingValue>::Success(static_cast<const Component *>(component)->weight);
        }

        [[nodiscard]] Result<void> WriteWeight(void *component, const Runtime::PropertyBindingValue &value) {
            if (!std::holds_alternative<float>(value))
                return Result<void>::Failure(MakeError(Runtime::PropertyBindingErrors::ValueTypeMismatch));
            static_cast<Component *>(component)->weight = std::get<float>(value);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<Runtime::PropertyBindingValue> ReadPosition(const void *component) {
            return Result<Runtime::PropertyBindingValue>::Success(static_cast<const Component *>(component)->position);
        }

        [[nodiscard]] Result<void> WritePosition(void *component, const Runtime::PropertyBindingValue &value) {
            if (!std::holds_alternative<Math::Vec3>(value))
                return Result<void>::Failure(MakeError(Runtime::PropertyBindingErrors::ValueTypeMismatch));
            static_cast<Component *>(component)->position = std::get<Math::Vec3>(value);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<Runtime::PropertyBindingValue> ReadEnabled(const void *component) {
            return Result<Runtime::PropertyBindingValue>::Success(static_cast<const Component *>(component)->enabled);
        }

        [[nodiscard]] Result<void> WriteEnabled(void *component, const Runtime::PropertyBindingValue &value) {
            if (!std::holds_alternative<bool>(value))
                return Result<void>::Failure(MakeError(Runtime::PropertyBindingErrors::ValueTypeMismatch));
            static_cast<Component *>(component)->enabled = std::get<bool>(value);
            return Result<void>::Success();
        }

        [[nodiscard]] ScalarCurveView Curve(const std::array<ScalarCurveKey, 2> &keys) {
            auto result = ScalarCurveView::Create(keys);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        struct CurveFixture final {
            std::array<ScalarCurveKey, 2> floatKeys;
            std::array<std::array<ScalarCurveKey, 2>, 3> vec3Keys;
            std::array<ScalarCurveKey, 2> boolKeys;

            CurveFixture(const float floatFrom, const float floatTo, const Math::Vec3 vecFrom = {}, const Math::Vec3 vecTo = {},
                         const bool boolFrom = false, const bool boolTo = true)
                : floatKeys{ScalarCurveKey{0, floatFrom}, ScalarCurveKey{10, floatTo}},
                  vec3Keys{std::array{ScalarCurveKey{0, vecFrom.x}, ScalarCurveKey{10, vecTo.x}},
                           std::array{ScalarCurveKey{0, vecFrom.y}, ScalarCurveKey{10, vecTo.y}},
                           std::array{ScalarCurveKey{0, vecFrom.z}, ScalarCurveKey{10, vecTo.z}}},
                  boolKeys{ScalarCurveKey{0, boolFrom ? 1.0F : 0.0F, CurveInterpolation::Constant},
                           ScalarCurveKey{10, boolTo ? 1.0F : 0.0F, CurveInterpolation::Constant}} {}

            [[nodiscard]] PropertyCurveSet FloatCurves() const {
                PropertyCurveSet curves{.type = Runtime::PropertyBindingType::Float};
                curves.channels[0] = Curve(floatKeys);
                return curves;
            }

            [[nodiscard]] PropertyCurveSet Vec3Curves() const {
                PropertyCurveSet curves{.type = Runtime::PropertyBindingType::Vec3};
                for (std::size_t channel = 0; channel < vec3Keys.size(); ++channel)
                    curves.channels[channel] = Curve(vec3Keys[channel]);
                return curves;
            }

            [[nodiscard]] PropertyCurveSet BoolCurves() const {
                PropertyCurveSet curves{.type = Runtime::PropertyBindingType::Boolean};
                curves.channels[0] = Curve(boolKeys);
                return curves;
            }
        };

        struct BindingFixture final {
            Runtime::PropertyBindingRegistry registry;
            Gameplay::ComponentTypeId componentType{ComponentType()};
            PropertyBindingId weight{Binding(10)};
            PropertyBindingId position{Binding(11)};
            PropertyBindingId enabled{Binding(12)};

            BindingFixture() {
                REQUIRE(registry
                            .Register({.id = weight,
                                       .componentType = componentType,
                                       .property = Property("weight"),
                                       .type = Runtime::PropertyBindingType::Float,
                                       .getter = ReadWeight,
                                       .setter = WriteWeight})
                            .HasValue());
                REQUIRE(registry
                            .Register({.id = position,
                                       .componentType = componentType,
                                       .property = Property("position"),
                                       .type = Runtime::PropertyBindingType::Vec3,
                                       .getter = ReadPosition,
                                       .setter = WritePosition})
                            .HasValue());
                REQUIRE(registry
                            .Register({.id = enabled,
                                       .componentType = componentType,
                                       .property = Property("enabled"),
                                       .type = Runtime::PropertyBindingType::Boolean,
                                       .getter = ReadEnabled,
                                       .setter = WriteEnabled})
                            .HasValue());
                REQUIRE(registry.Freeze().HasValue());
            }
        };

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == descriptor.domain.Value());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }
    }  // namespace

    TEST_CASE("Property binding registry is typed, frozen, and authoring-name discoverable",
              "[unit][cinematic][property-binding][registry]") {
        BindingFixture fixture;
        CHECK(fixture.registry.IsFrozen());
        CHECK(fixture.registry.Find(fixture.weight) != nullptr);
        CHECK(fixture.registry.FindByName(fixture.componentType, "position")->id == fixture.position);
        CHECK(fixture.registry.FindByName(fixture.componentType, "missing") == nullptr);

        auto duplicate = fixture.registry.Register({.id = Binding(99),
                                                    .componentType = fixture.componentType,
                                                    .property = Property("weight"),
                                                    .type = Runtime::PropertyBindingType::Float,
                                                    .getter = ReadWeight,
                                                    .setter = WriteWeight});
        RequireError(duplicate, Runtime::PropertyBindingErrors::RegistryFrozen);
    }

    TEST_CASE("Property tracks sample float vectors and bools through one shared plan", "[unit][cinematic][property-track][typed]") {
        BindingFixture fixture;
        Component component{};
        const CurveFixture curves{0.0F, 10.0F, {0.0F, 2.0F, 4.0F}, {10.0F, 12.0F, 14.0F}, true, false};
        const std::array targets{PropertyBindingTargetSnapshot{fixture.weight, Object(1), fixture.componentType, 7, &component},
                                 PropertyBindingTargetSnapshot{fixture.position, Object(2), fixture.componentType, 7, &component},
                                 PropertyBindingTargetSnapshot{fixture.enabled, Object(3), fixture.componentType, 7, &component}};
        const std::array tracks{PropertyTrackDescriptor{.track = Track(3),
                                                        .targetObject = Object(3),
                                                        .binding = fixture.enabled,
                                                        .curves = curves.BoolCurves()},
                                PropertyTrackDescriptor{.track = Track(1),
                                                        .targetObject = Object(1),
                                                        .binding = fixture.weight,
                                                        .curves = curves.FloatCurves()},
                                PropertyTrackDescriptor{.track = Track(2),
                                                        .targetObject = Object(2),
                                                        .binding = fixture.position,
                                                        .curves = curves.Vec3Curves()}};
        constexpr PropertySceneVersion scene{4, 9};
        auto plan = PropertyEvaluationPlan::Create(scene, tracks, targets, fixture.registry);
        REQUIRE(plan.HasValue());
        CHECK(plan.Value().TrackCount() == 3);

        std::array<PropertyEvaluationValue, 3> values{};
        const PropertyEvaluationContext context{scene, targets};
        REQUIRE(plan.Value().Evaluate(5, context, values).Value() == 3);
        CHECK(values[0].track == Track(1));
        CHECK(std::get<float>(values[0].value) == Catch::Approx(5.0F));
        CHECK(values[1].track == Track(2));
        CHECK(std::get<Math::Vec3>(values[1].value) == Math::Vec3{5.0F, 7.0F, 9.0F});
        CHECK(values[2].track == Track(3));
        CHECK(std::get<bool>(values[2].value));

        std::array<PropertyEvaluationDiagnostic, 3> diagnostics{};
        auto applied = plan.Value().Apply(context, values, diagnostics);
        REQUIRE(applied.HasValue());
        CHECK(applied.Value().sampled == 3);
        CHECK(applied.Value().applied == 3);
        CHECK(applied.Value().diagnostics == 0);
        CHECK(component.weight == Catch::Approx(5.0F));
        CHECK(component.position == Math::Vec3{5.0F, 7.0F, 9.0F});
        CHECK(component.enabled);
    }

    TEST_CASE("Property sampling is history-independent and allocation-free after activation",
              "[unit][cinematic][property-track][determinism][allocation]") {
        BindingFixture fixture;
        Component component{};
        const CurveFixture curves{0.0F, 10.0F};
        const std::array targets{PropertyBindingTargetSnapshot{fixture.weight, Object(1), fixture.componentType, 1, &component}};
        const std::array tracks{PropertyTrackDescriptor{.track = Track(1),
                                                        .targetObject = Object(1),
                                                        .binding = fixture.weight,
                                                        .curves = curves.FloatCurves()}};
        constexpr PropertySceneVersion scene{1, 1};
        auto plan = PropertyEvaluationPlan::Create(scene, tracks, targets, fixture.registry);
        REQUIRE(plan.HasValue());
        const PropertyEvaluationContext context{scene, targets};
        std::array<PropertyEvaluationValue, 1> values{};
        REQUIRE(plan.Value().Evaluate(10, context, values).Value() == 1);
        CHECK(std::get<float>(values[0].value) == Catch::Approx(10.0F));
        REQUIRE(plan.Value().Evaluate(2, context, values).Value() == 1);
        CHECK(std::get<float>(values[0].value) == Catch::Approx(2.0F));

        const std::size_t before = Tests::AllocationProbe::Count();
        for (std::size_t sample = 0; sample < 10'000; ++sample)
            REQUIRE(plan.Value().Evaluate(static_cast<CurveTime>(sample % 11), context, values).HasValue());
        CHECK(Tests::AllocationProbe::Count() == before);
    }

    TEST_CASE("Property plans fence scene replacement and surface optional binding skips", "[unit][cinematic][property-track][lifecycle]") {
        BindingFixture fixture;
        Component component{};
        const CurveFixture curves{0.0F, 1.0F};
        const std::array targets{PropertyBindingTargetSnapshot{fixture.weight, Object(1), fixture.componentType, 2, &component}};
        const std::array tracks{PropertyTrackDescriptor{.track = Track(1),
                                                        .targetObject = Object(1),
                                                        .binding = fixture.weight,
                                                        .curves = curves.FloatCurves()}};
        constexpr PropertySceneVersion scene{3, 2};
        auto plan = PropertyEvaluationPlan::Create(scene, tracks, targets, fixture.registry);
        REQUIRE(plan.HasValue());
        std::array<PropertyEvaluationValue, 1> values{};
        RequireError(plan.Value().Evaluate(1, PropertyEvaluationContext{{4, 2}, targets}, values), CinematicErrors::PropertyBindingStale);

        const std::array missingTracks{PropertyTrackDescriptor{.track = Track(2),
                                                               .targetObject = Object(9),
                                                               .binding = Binding(999),
                                                               .curves = curves.FloatCurves(),
                                                               .required = false}};
        auto optional = PropertyEvaluationPlan::Create(scene, missingTracks, {}, fixture.registry);
        REQUIRE(optional.HasValue());
        std::array<PropertyEvaluationDiagnostic, 1> diagnostics{};
        auto result = optional.Value().EvaluateAndApply(1, PropertyEvaluationContext{scene, {}}, values, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(result.Value().applied == 0);
        REQUIRE(result.Value().diagnostics == 1);
        CHECK(result.Value().diagnostics == 1);
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::BindingMissing);
        CHECK(diagnostics[0].error->code.Value() == CinematicErrors::PropertyBindingMissing.code.Value());
    }

    TEST_CASE("Property activation rejects typed mismatch, stale target, and malformed bool curves",
              "[unit][cinematic][property-track][validation]") {
        BindingFixture fixture;
        Component component{};
        const CurveFixture curves{0.0F, 1.0F};
        const std::array targets{PropertyBindingTargetSnapshot{fixture.weight, Object(1), fixture.componentType, 1, &component}};
        auto mismatch =
            PropertyTrackDescriptor{.track = Track(1), .targetObject = Object(1), .binding = fixture.weight, .curves = curves.Vec3Curves()};
        RequireError(PropertyEvaluationPlan::Create({1, 1}, std::span{&mismatch, 1}, targets, fixture.registry),
                     CinematicErrors::PropertyTypeMismatch);

        auto staleTargets = targets;
        staleTargets[0].componentRevision = 2;
        auto valid = PropertyTrackDescriptor{.track = Track(2),
                                             .targetObject = Object(1),
                                             .binding = fixture.weight,
                                             .curves = curves.FloatCurves()};
        auto plan = PropertyEvaluationPlan::Create({1, 1}, std::span{&valid, 1}, targets, fixture.registry);
        REQUIRE(plan.HasValue());
        std::array<PropertyEvaluationValue, 1> values{};
        const PropertyEvaluationContext staleContext{{1, 1}, staleTargets};
        RequireError(plan.Value().Evaluate(1, staleContext, values), CinematicErrors::PropertyBindingStale);

        const std::array boolTargets{PropertyBindingTargetSnapshot{fixture.enabled, Object(3), fixture.componentType, 1, &component}};
        const std::array badKeys{ScalarCurveKey{0, 0.0F}, ScalarCurveKey{10, 1.0F}};
        PropertyCurveSet badBoolCurves{.type = Runtime::PropertyBindingType::Boolean};
        badBoolCurves.channels[0] = Curve(badKeys);
        auto badBool =
            PropertyTrackDescriptor{.track = Track(3), .targetObject = Object(3), .binding = fixture.enabled, .curves = badBoolCurves};
        RequireError(PropertyEvaluationPlan::Create({1, 1}, std::span{&badBool, 1}, boolTargets, fixture.registry),
                     CinematicErrors::PropertyMalformed);
    }

    TEST_CASE("Property activation requires a frozen binding registry", "[unit][cinematic][property-track][diagnostics]") {
        BindingFixture fixture;
        Component component{};
        const CurveFixture curves{0.0F, 1.0F};
        constexpr PropertySceneVersion scene{1, 1};
        const std::array validTarget{PropertyBindingTargetSnapshot{fixture.weight, Object(1), fixture.componentType, 1, &component}};

        Runtime::PropertyBindingRegistry openRegistry;
        REQUIRE(openRegistry
                    .Register({.id = fixture.weight,
                               .componentType = fixture.componentType,
                               .property = Property("weight"),
                               .type = Runtime::PropertyBindingType::Float,
                               .getter = ReadWeight,
                               .setter = WriteWeight})
                    .HasValue());
        auto validTrack = PropertyTrackDescriptor{.track = Track(1),
                                                  .targetObject = Object(1),
                                                  .binding = fixture.weight,
                                                  .curves = curves.FloatCurves()};
        RequireError(PropertyEvaluationPlan::Create(scene, std::span{&validTrack, 1}, validTarget, openRegistry),
                     CinematicErrors::PropertyRegistryUnfrozen);
    }

    TEST_CASE("Optional property tracks preserve typed diagnostics", "[unit][cinematic][property-track][diagnostics]") {
        BindingFixture fixture;
        Component component{};
        const CurveFixture curves{0.0F, 1.0F};
        constexpr PropertySceneVersion scene{1, 1};
        const std::array validTarget{PropertyBindingTargetSnapshot{fixture.weight, Object(1), fixture.componentType, 1, &component}};
        auto mismatch = PropertyTrackDescriptor{.track = Track(2),
                                                .targetObject = Object(1),
                                                .binding = fixture.weight,
                                                .curves = curves.Vec3Curves(),
                                                .required = false};
        auto mismatchPlan = PropertyEvaluationPlan::Create(scene, std::span{&mismatch, 1}, validTarget, fixture.registry);
        REQUIRE(mismatchPlan.HasValue());
        std::array<PropertyEvaluationValue, 1> values{};
        std::array<PropertyEvaluationDiagnostic, 1> diagnostics{};
        auto mismatchResult = mismatchPlan.Value().EvaluateAndApply(5, PropertyEvaluationContext{scene, validTarget}, values, diagnostics);
        REQUIRE(mismatchResult.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::TypeMismatch);
        CHECK(diagnostics[0].error->code.Value() == CinematicErrors::PropertyTypeMismatch.code.Value());
    }

    TEST_CASE("Optional property tracks preserve target diagnostics", "[unit][cinematic][property-track][diagnostics]") {
        BindingFixture fixture;
        Component component{};
        const CurveFixture curves{0.0F, 1.0F};
        constexpr PropertySceneVersion scene{1, 1};
        std::array<PropertyEvaluationValue, 1> values{};
        std::array<PropertyEvaluationDiagnostic, 1> diagnostics{};
        auto targetMissing = PropertyTrackDescriptor{.track = Track(3),
                                                     .targetObject = Object(9),
                                                     .binding = fixture.weight,
                                                     .curves = curves.FloatCurves(),
                                                     .required = false};
        auto missingPlan = PropertyEvaluationPlan::Create(scene, std::span{&targetMissing, 1}, {}, fixture.registry);
        REQUIRE(missingPlan.HasValue());
        auto missingResult = missingPlan.Value().EvaluateAndApply(5, PropertyEvaluationContext{scene, {}}, values, diagnostics);
        REQUIRE(missingResult.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::TargetMissing);

        const std::array wrongTypeTarget{
            PropertyBindingTargetSnapshot{fixture.weight, Object(10), ComponentType("game.cinematic.other"), 1, &component}};
        auto componentMismatch = PropertyTrackDescriptor{.track = Track(4),
                                                         .targetObject = Object(10),
                                                         .binding = fixture.weight,
                                                         .curves = curves.FloatCurves(),
                                                         .required = false};
        auto componentPlan = PropertyEvaluationPlan::Create(scene, std::span{&componentMismatch, 1}, wrongTypeTarget, fixture.registry);
        REQUIRE(componentPlan.HasValue());
        diagnostics = {};
        auto componentResult =
            componentPlan.Value().EvaluateAndApply(5, PropertyEvaluationContext{scene, wrongTypeTarget}, values, diagnostics);
        REQUIRE(componentResult.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::ComponentMismatch);
    }
}  // namespace Horo::Cinematic
