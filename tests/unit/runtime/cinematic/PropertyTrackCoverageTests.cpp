#include "Horo/Cinematic/CinematicErrors.h"
#include "Horo/Cinematic/PropertyTrack.h"
#include "Horo/Runtime/Scene/PropertyBindingErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <variant>

namespace Horo::Cinematic {
    namespace {
        struct Component final {
            float weight{};
            Math::Vec2 scale{};
            Math::Vec4 color{};
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

        [[nodiscard]] Gameplay::ComponentTypeId ComponentType(const std::string_view value = "game.cinematic.coverage") {
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

        [[nodiscard]] Result<Runtime::PropertyBindingValue> ReadScale(const void *component) {
            return Result<Runtime::PropertyBindingValue>::Success(static_cast<const Component *>(component)->scale);
        }

        [[nodiscard]] Result<void> WriteScale(void *component, const Runtime::PropertyBindingValue &value) {
            if (!std::holds_alternative<Math::Vec2>(value))
                return Result<void>::Failure(MakeError(Runtime::PropertyBindingErrors::ValueTypeMismatch));
            static_cast<Component *>(component)->scale = std::get<Math::Vec2>(value);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<Runtime::PropertyBindingValue> ReadColor(const void *component) {
            return Result<Runtime::PropertyBindingValue>::Success(static_cast<const Component *>(component)->color);
        }

        [[nodiscard]] Result<void> WriteColor(void *component, const Runtime::PropertyBindingValue &value) {
            if (!std::holds_alternative<Math::Vec4>(value))
                return Result<void>::Failure(MakeError(Runtime::PropertyBindingErrors::ValueTypeMismatch));
            static_cast<Component *>(component)->color = std::get<Math::Vec4>(value);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> RejectWrite(void *, const Runtime::PropertyBindingValue &) {
            return Result<void>::Failure(MakeError(Runtime::PropertyBindingErrors::WriteRejected));
        }

        [[nodiscard]] ScalarCurveView Curve(const std::array<ScalarCurveKey, 2> &keys) {
            auto result = ScalarCurveView::Create(keys);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        struct BindingFixture final {
            Runtime::PropertyBindingRegistry registry;
            Gameplay::ComponentTypeId componentType{ComponentType()};
            PropertyBindingId weight{Binding(10)};

            BindingFixture() {
                REQUIRE(registry
                            .Register({.id = weight,
                                       .componentType = componentType,
                                       .property = Property("weight"),
                                       .type = Runtime::PropertyBindingType::Float,
                                       .getter = ReadWeight,
                                       .setter = WriteWeight})
                            .HasValue());
                REQUIRE(registry.Freeze().HasValue());
            }
        };

        struct CurveFixture final {
            std::array<ScalarCurveKey, 2> keys{ScalarCurveKey{0, 0.0F}, ScalarCurveKey{10, 1.0F}};

            [[nodiscard]] PropertyCurveSet FloatCurves() const {
                PropertyCurveSet curves{.type = Runtime::PropertyBindingType::Float};
                curves.channels[0] = Curve(keys);
                return curves;
            }
        };

        struct MalformedFixture final {
            BindingFixture bindings;
            Component component{};
            CurveFixture curves;
            constexpr static PropertySceneVersion scene{1, 1};
            std::array<PropertyBindingTargetSnapshot, 1> targets{
                PropertyBindingTargetSnapshot{bindings.weight, Object(1), bindings.componentType, 1, &component}};

            [[nodiscard]] PropertyTrackDescriptor MakeTrack(const PropertyBindingId binding = Binding(10), const TrackId track = Track(1),
                                                            const Runtime::SceneObjectId object = Object(1)) const {
                return {.track = track, .targetObject = object, .binding = binding, .curves = curves.FloatCurves()};
            }
        };

        struct VectorFixture final {
            Runtime::PropertyBindingRegistry registry;
            Component component{};
            Gameplay::ComponentTypeId componentType{ComponentType()};
            PropertyBindingId scaleBinding{Binding(20)};
            PropertyBindingId colorBinding{Binding(21)};
            std::array<PropertyBindingTargetSnapshot, 2> targets{};
            std::array<PropertyTrackDescriptor, 2> tracks{};

            VectorFixture() {
                REQUIRE(registry
                            .Register({.id = scaleBinding,
                                       .componentType = componentType,
                                       .property = Property("scale"),
                                       .type = Runtime::PropertyBindingType::Vec2,
                                       .getter = ReadScale,
                                       .setter = WriteScale,
                                       .range = {.minimum = 0.0F, .maximum = 10.0F}})
                            .HasValue());
                REQUIRE(registry
                            .Register({.id = colorBinding,
                                       .componentType = componentType,
                                       .property = Property("color"),
                                       .type = Runtime::PropertyBindingType::Vec4,
                                       .getter = ReadColor,
                                       .setter = WriteColor,
                                       .range = {.minimum = 0.0F, .maximum = 10.0F}})
                            .HasValue());
                REQUIRE(registry.Freeze().HasValue());
                const std::array x{ScalarCurveKey{0, 0.0F}, ScalarCurveKey{10, 10.0F}};
                const std::array y{ScalarCurveKey{0, 2.0F}, ScalarCurveKey{10, 8.0F}};
                const std::array r{ScalarCurveKey{0, 1.0F}, ScalarCurveKey{10, 3.0F}};
                const std::array g{ScalarCurveKey{0, 2.0F}, ScalarCurveKey{10, 4.0F}};
                const std::array b{ScalarCurveKey{0, 3.0F}, ScalarCurveKey{10, 5.0F}};
                const std::array a{ScalarCurveKey{0, 4.0F}, ScalarCurveKey{10, 6.0F}};
                PropertyCurveSet scaleCurves{.type = Runtime::PropertyBindingType::Vec2};
                scaleCurves.channels[0] = Curve(x);
                scaleCurves.channels[1] = Curve(y);
                PropertyCurveSet colorCurves{.type = Runtime::PropertyBindingType::Vec4};
                colorCurves.channels[0] = Curve(r);
                colorCurves.channels[1] = Curve(g);
                colorCurves.channels[2] = Curve(b);
                colorCurves.channels[3] = Curve(a);
                targets = {PropertyBindingTargetSnapshot{scaleBinding, Object(20), componentType, 1, &component},
                           PropertyBindingTargetSnapshot{colorBinding, Object(21), componentType, 1, &component}};
                tracks =
                    {PropertyTrackDescriptor{.track = Track(2), .targetObject = Object(21), .binding = colorBinding, .curves = colorCurves},
                     PropertyTrackDescriptor{.track = Track(1),
                                             .targetObject = Object(20),
                                             .binding = scaleBinding,
                                             .curves = scaleCurves}};
            }
        };

        [[nodiscard]] Runtime::PropertyBindingDescriptor MakeDescriptor() {
            return {.id = Binding(20),
                    .componentType = ComponentType(),
                    .property = Property("weight"),
                    .type = Runtime::PropertyBindingType::Float,
                    .getter = ReadWeight,
                    .setter = WriteWeight};
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == descriptor.domain.Value());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
        }

        template <typename Mutate> void RequireInvalidDescriptor(Mutate mutate) {
            Runtime::PropertyBindingRegistry registry;
            auto descriptor = MakeDescriptor();
            mutate(descriptor);
            RequireError(registry.Register(std::move(descriptor)), Runtime::PropertyBindingErrors::InvalidDescriptor);
        }
    }  // namespace

    TEST_CASE("Property registry rejects invalid identities and names", "[unit][cinematic][property-binding][validation]") {
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.id = {};
        });
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.property = {};
            descriptor.propertyName.clear();
        });
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.property = {};
            descriptor.propertyName = "MoveWeight";
        });
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.propertyName = "position";
        });
    }

    TEST_CASE("Property registry rejects invalid types accessors and ranges", "[unit][cinematic][property-binding][validation]") {
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.type = static_cast<Runtime::PropertyBindingType>(99);
        });
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.getter = nullptr;
        });
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.setter = nullptr;
        });
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.writePolicy = static_cast<Runtime::PropertyWritePolicy>(99);
        });
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.range = {.minimum = 2.0F, .maximum = 1.0F};
        });
        RequireInvalidDescriptor([](auto &descriptor) {
            descriptor.range = {.minimum = std::numeric_limits<float>::quiet_NaN(), .maximum = 1.0F};
        });
    }

    TEST_CASE("Property registry rejects duplicates and exposes frozen snapshots", "[unit][cinematic][property-binding][registry]") {
        Runtime::PropertyBindingRegistry registry;
        auto first = MakeDescriptor();
        REQUIRE(registry.Register(first).HasValue());
        auto duplicateProperty = MakeDescriptor();
        duplicateProperty.id = Binding(21);
        RequireError(registry.Register(std::move(duplicateProperty)), Runtime::PropertyBindingErrors::DuplicateBinding);
        auto duplicateId = MakeDescriptor();
        duplicateId.property = Property("position");
        RequireError(registry.Register(std::move(duplicateId)), Runtime::PropertyBindingErrors::DuplicateBinding);
        REQUIRE(registry.Freeze().HasValue());
        REQUIRE(registry.Freeze().HasValue());
        CHECK(registry.Descriptors().size() == 1);
        CHECK(registry.Find({}) == nullptr);
        CHECK(registry.FindByName({}, "weight") == nullptr);
        CHECK(registry.FindByName(ComponentType(), "") == nullptr);
    }

    TEST_CASE("Property tracks sample vec2 and vec4 channels", "[unit][cinematic][property-track][typed]") {
        VectorFixture fixture;
        constexpr PropertySceneVersion scene{1, 1};
        auto plan = PropertyEvaluationPlan::Create(scene, fixture.tracks, fixture.targets, fixture.registry);
        REQUIRE(plan.HasValue());
        CHECK(plan.Value().SceneVersion() == scene);
        std::array<PropertyEvaluationValue, 2> values{};
        REQUIRE(plan.Value().Evaluate(5, PropertyEvaluationContext{scene, fixture.targets}, values).Value() == 2);
        CHECK(std::get<Math::Vec2>(values[0].value) == Math::Vec2{5.0F, 5.0F});
        CHECK(std::get<Math::Vec4>(values[1].value) == Math::Vec4{2.0F, 3.0F, 4.0F, 5.0F});
        std::array<PropertyEvaluationDiagnostic, 2> diagnostics{};
        auto applied = plan.Value().Apply(PropertyEvaluationContext{scene, fixture.targets}, values, diagnostics);
        REQUIRE(applied.HasValue());
        CHECK(applied.Value().applied == 2);
        CHECK(fixture.component.scale == Math::Vec2{5.0F, 5.0F});
        CHECK(fixture.component.color == Math::Vec4{2.0F, 3.0F, 4.0F, 5.0F});
    }

    TEST_CASE("Property plans reject malformed metadata", "[unit][cinematic][property-track][validation]") {
        MalformedFixture fixture;
        auto validTrack = fixture.MakeTrack();
        RequireError(PropertyEvaluationPlan::Create({}, std::span{&validTrack, 1}, fixture.targets, fixture.bindings.registry),
                     CinematicErrors::PropertyMalformed);
        auto unsupportedVersion = validTrack;
        unsupportedVersion.version = {2, 0};
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&unsupportedVersion, 1}, fixture.targets,
                                                    fixture.bindings.registry),
                     CinematicErrors::PropertyVersionUnsupported);
        auto invalidIdentity = validTrack;
        invalidIdentity.track = {};
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&invalidIdentity, 1}, fixture.targets,
                                                    fixture.bindings.registry),
                     CinematicErrors::PropertyMalformed);
        const std::array duplicateTracks{validTrack, validTrack};
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, duplicateTracks, fixture.targets, fixture.bindings.registry),
                     CinematicErrors::PropertyMalformed);
    }

    TEST_CASE("Property plans reject malformed targets and bindings", "[unit][cinematic][property-track][validation]") {
        MalformedFixture fixture;
        auto validTrack = fixture.MakeTrack();
        auto invalidRevision = fixture.targets;
        invalidRevision[0].componentRevision = 0;
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&validTrack, 1}, invalidRevision,
                                                    fixture.bindings.registry),
                     CinematicErrors::PropertyMalformed);
        auto missingComponent = fixture.targets;
        missingComponent[0].component = nullptr;
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&validTrack, 1}, missingComponent,
                                                    fixture.bindings.registry),
                     CinematicErrors::PropertyBindingTargetMissing);
        const std::array duplicateTargets{fixture.targets[0], fixture.targets[0]};
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&validTrack, 1}, duplicateTargets,
                                                    fixture.bindings.registry),
                     CinematicErrors::PropertyMalformed);
        auto missingBinding = fixture.MakeTrack(Binding(999));
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&missingBinding, 1}, fixture.targets,
                                                    fixture.bindings.registry),
                     CinematicErrors::PropertyBindingMissing);
        auto missingTarget = fixture.MakeTrack(fixture.bindings.weight, Track(2), Object(99));
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&missingTarget, 1}, fixture.targets,
                                                    fixture.bindings.registry),
                     CinematicErrors::PropertyBindingTargetMissing);
        auto wrongComponent = fixture.targets;
        wrongComponent[0].componentType = ComponentType("game.cinematic.other");
        RequireError(PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&validTrack, 1}, wrongComponent,
                                                    fixture.bindings.registry),
                     CinematicErrors::PropertyComponentMismatch);
    }

    TEST_CASE("Property plans reject invalid ranges and report apply shape errors", "[unit][cinematic][property-track][diagnostics]") {
        MalformedFixture fixture;
        Runtime::PropertyBindingRegistry invalidRangeRegistry;
        RequireError(invalidRangeRegistry.Register({.id = fixture.bindings.weight,
                                                    .componentType = fixture.bindings.componentType,
                                                    .property = Property("weight"),
                                                    .type = Runtime::PropertyBindingType::Float,
                                                    .getter = ReadWeight,
                                                    .setter = WriteWeight,
                                                    .range = {.minimum = 2.0F, .maximum = 1.0F}}),
                     Runtime::PropertyBindingErrors::InvalidDescriptor);
        auto track = fixture.MakeTrack();
        auto plan =
            PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&track, 1}, fixture.targets, fixture.bindings.registry);
        REQUIRE(plan.HasValue());
        std::array<PropertyEvaluationValue, 1> values{};
        REQUIRE(plan.Value().Evaluate(1, PropertyEvaluationContext{MalformedFixture::scene, fixture.targets}, values).Value() == 1);
        std::array<PropertyEvaluationDiagnostic, 1> diagnostics{};
        CHECK(plan.Value()
                  .Evaluate(1, PropertyEvaluationContext{MalformedFixture::scene, fixture.targets}, std::span<PropertyEvaluationValue>{})
                  .HasError());
        CHECK(plan.Value().Evaluate(1, PropertyEvaluationContext{{2, 1}, fixture.targets}, values).ErrorValue().code.Value() ==
              CinematicErrors::PropertyBindingStale.code.Value());
        CHECK(plan.Value().Evaluate(1, PropertyEvaluationContext{MalformedFixture::scene, {}}, values).ErrorValue().code.Value() ==
              CinematicErrors::PropertyBindingTargetMissing.code.Value());
        auto staleTargets = fixture.targets;
        staleTargets[0].componentRevision = 2;
        auto staleApply = plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, staleTargets}, values, diagnostics);
        REQUIRE(staleApply.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::BindingStale);
        auto missingApply = plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, {}}, values, diagnostics);
        REQUIRE(missingApply.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::TargetMissing);
    }

    TEST_CASE("Property plans report typed apply diagnostics", "[unit][cinematic][property-track][diagnostics]") {
        MalformedFixture fixture;
        auto track = fixture.MakeTrack();
        auto plan =
            PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&track, 1}, fixture.targets, fixture.bindings.registry);
        REQUIRE(plan.HasValue());
        std::array<PropertyEvaluationValue, 1> values{};
        REQUIRE(plan.Value().Evaluate(1, PropertyEvaluationContext{MalformedFixture::scene, fixture.targets}, values).HasValue());
        std::array<PropertyEvaluationDiagnostic, 1> diagnostics{};
        auto wrongTypeTargets = fixture.targets;
        wrongTypeTargets[0].componentType = ComponentType("game.cinematic.other");
        auto wrongTypeApply = plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, wrongTypeTargets}, values, diagnostics);
        REQUIRE(wrongTypeApply.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::BindingStale);
        auto wrongValue = values;
        wrongValue[0].value = true;
        auto mismatchApply =
            plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, fixture.targets}, wrongValue, diagnostics);
        REQUIRE(mismatchApply.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::TypeMismatch);
        wrongValue[0].track = Track(99);
        RequireError(plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, fixture.targets}, wrongValue, diagnostics),
                     CinematicErrors::PropertyMalformed);
        std::array<PropertyEvaluationValue, 2> tooManyValues{};
        RequireError(plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, fixture.targets}, tooManyValues, diagnostics),
                     CinematicErrors::PropertyLimitExceeded);
        std::array<PropertyEvaluationDiagnostic, 0> noDiagnostics{};
        RequireError(plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, fixture.targets}, values, noDiagnostics),
                     CinematicErrors::PropertyLimitExceeded);
        std::array<PropertyEvaluationValue, 0> noValues{};
        RequireError(plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, fixture.targets}, noValues, diagnostics),
                     CinematicErrors::PropertyLimitExceeded);
        RequireError(plan.Value().Apply(PropertyEvaluationContext{{2, 1}, fixture.targets}, values, diagnostics),
                     CinematicErrors::PropertyBindingStale);
    }

    TEST_CASE("Property plans report read-only diagnostics", "[unit][cinematic][property-track][diagnostics]") {
        MalformedFixture fixture;
        Runtime::PropertyBindingRegistry registry;
        const PropertyBindingId binding = Binding(30);
        REQUIRE(registry
                    .Register({.id = binding,
                               .componentType = fixture.bindings.componentType,
                               .property = Property("weight"),
                               .type = Runtime::PropertyBindingType::Float,
                               .getter = ReadWeight,
                               .setter = WriteWeight,
                               .writePolicy = Runtime::PropertyWritePolicy::ReadOnly})
                    .HasValue());
        REQUIRE(registry.Freeze().HasValue());
        auto track = fixture.MakeTrack(binding);
        const std::array targets{PropertyBindingTargetSnapshot{binding, Object(1), fixture.bindings.componentType, 1, &fixture.component}};
        auto plan = PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&track, 1}, targets, registry);
        REQUIRE(plan.HasValue());
        std::array<PropertyEvaluationValue, 1> values{};
        REQUIRE(plan.Value().Evaluate(1, PropertyEvaluationContext{MalformedFixture::scene, targets}, values).HasValue());
        std::array<PropertyEvaluationDiagnostic, 1> diagnostics{};
        auto result = plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, targets}, values, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::ReadOnly);
    }

    TEST_CASE("Property plans report range diagnostics", "[unit][cinematic][property-track][diagnostics]") {
        MalformedFixture fixture;
        Runtime::PropertyBindingRegistry registry;
        const PropertyBindingId binding = Binding(31);
        REQUIRE(registry
                    .Register({.id = binding,
                               .componentType = fixture.bindings.componentType,
                               .property = Property("weight"),
                               .type = Runtime::PropertyBindingType::Float,
                               .getter = ReadWeight,
                               .setter = WriteWeight,
                               .range = {.minimum = 0.0F, .maximum = 1.0F}})
                    .HasValue());
        REQUIRE(registry.Freeze().HasValue());
        auto track = fixture.MakeTrack(binding);
        const std::array targets{PropertyBindingTargetSnapshot{binding, Object(1), fixture.bindings.componentType, 1, &fixture.component}};
        auto plan = PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&track, 1}, targets, registry);
        REQUIRE(plan.HasValue());
        std::array<PropertyEvaluationValue, 1> values{};
        values[0].binding = binding;
        values[0].track = Track(1);
        values[0].targetObject = Object(1);
        values[0].value = 2.0F;
        std::array<PropertyEvaluationDiagnostic, 1> diagnostics{};
        auto result = plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, targets}, values, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::ValueOutOfRange);
    }

    TEST_CASE("Property plans report setter diagnostics", "[unit][cinematic][property-track][diagnostics]") {
        MalformedFixture fixture;
        Runtime::PropertyBindingRegistry registry;
        const PropertyBindingId binding = Binding(32);
        REQUIRE(registry
                    .Register({.id = binding,
                               .componentType = fixture.bindings.componentType,
                               .property = Property("weight"),
                               .type = Runtime::PropertyBindingType::Float,
                               .getter = ReadWeight,
                               .setter = RejectWrite})
                    .HasValue());
        REQUIRE(registry.Freeze().HasValue());
        auto track = fixture.MakeTrack(binding);
        const std::array targets{PropertyBindingTargetSnapshot{binding, Object(1), fixture.bindings.componentType, 1, &fixture.component}};
        auto plan = PropertyEvaluationPlan::Create(MalformedFixture::scene, std::span{&track, 1}, targets, registry);
        REQUIRE(plan.HasValue());
        std::array<PropertyEvaluationValue, 1> values{};
        REQUIRE(plan.Value().Evaluate(1, PropertyEvaluationContext{MalformedFixture::scene, targets}, values).HasValue());
        std::array<PropertyEvaluationDiagnostic, 1> diagnostics{};
        auto result = plan.Value().Apply(PropertyEvaluationContext{MalformedFixture::scene, targets}, values, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(diagnostics[0].outcome == PropertyBindingEvaluationOutcome::WriteRejected);
    }
}  // namespace Horo::Cinematic
