#include "Horo/Cinematic/CinematicErrors.h"
#include "Horo/Editor/CinematicPropertyBindings.h"
#include "Horo/Runtime/Scene/PropertyBindingErrors.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        struct Component final {
            float weight{};
            Math::Vec3 position{};
            bool enabled{};
            bool reject{};
        };

        /** @brief Reads the test component's float through an owner accessor. */
        Result<Runtime::PropertyBindingValue> ReadWeight(const void *component) {
            return Result<Runtime::PropertyBindingValue>::Success(static_cast<const Component *>(component)->weight);
        }

        /** @brief Applies a float only when the test owner admits the write. */
        Result<void> WriteWeight(void *component, const Runtime::PropertyBindingValue &value) {
            auto &target = *static_cast<Component *>(component);
            if (target.reject)
                return Result<void>::Failure(MakeError(Runtime::PropertyBindingErrors::WriteRejected, "Owner denied the write."));
            target.weight = std::get<float>(value);
            return Result<void>::Success();
        }

        /** @brief Reads a vector through the same component registry contract. */
        Result<Runtime::PropertyBindingValue> ReadPosition(const void *component) {
            return Result<Runtime::PropertyBindingValue>::Success(static_cast<const Component *>(component)->position);
        }

        /** @brief Writes a typed vector. */
        Result<void> WritePosition(void *component, const Runtime::PropertyBindingValue &value) {
            static_cast<Component *>(component)->position = std::get<Math::Vec3>(value);
            return Result<void>::Success();
        }

        /** @brief Reads a typed boolean. */
        Result<Runtime::PropertyBindingValue> ReadEnabled(const void *component) {
            return Result<Runtime::PropertyBindingValue>::Success(static_cast<const Component *>(component)->enabled);
        }

        /** @brief Writes a typed boolean. */
        Result<void> WriteEnabled(void *component, const Runtime::PropertyBindingValue &value) {
            static_cast<Component *>(component)->enabled = std::get<bool>(value);
            return Result<void>::Success();
        }

        struct Fixture final : IPropertyDiagnosticSink {
            Runtime::PropertyBindingRegistry registry;
            Component component;
            Gameplay::ComponentTypeId componentType{Gameplay::ComponentTypeId::Parse("game.cinematic.integration").Value()};
            std::array<ScalarCurveKey, 2> floatKeys{ScalarCurveKey{0, 0.0F}, ScalarCurveKey{10, 10.0F}};
            std::array<ScalarCurveKey, 2> boolKeys{ScalarCurveKey{0, 0.0F, CurveInterpolation::Constant},
                                                   ScalarCurveKey{10, 1.0F, CurveInterpolation::Constant}};
            std::array<PropertyTrackDescriptor, 3> tracks;
            std::array<PropertyBindingTargetSnapshot, 3> targets;
            std::vector<PropertyBindingDiagnosticEvent> events;
            static constexpr PropertySceneVersion scene{1, 1};
            static constexpr SequenceId sequence{5, 1};

            explicit Fixture(const Runtime::PropertyWritePolicy policy = Runtime::PropertyWritePolicy::DirectScene) {
                const std::array types{Runtime::PropertyBindingType::Float, Runtime::PropertyBindingType::Vec3,
                                       Runtime::PropertyBindingType::Boolean};
                const std::array names{"weight", "position", "enabled"};
                const std::array<Runtime::PropertyGetterFn, 3> getters{ReadWeight, ReadPosition, ReadEnabled};
                const std::array<Runtime::PropertySetterFn, 3> setters{WriteWeight, WritePosition, WriteEnabled};
                for (std::size_t index = 0; index < tracks.size(); ++index) {
                    const PropertyBindingId binding{index + 10, 1};
                    REQUIRE(registry
                                .Register({.id = binding,
                                           .componentType = componentType,
                                           .property = Gameplay::ComponentPropertyId::Parse(names[index]).Value(),
                                           .type = types[index],
                                           .getter = getters[index],
                                           .setter = setters[index],
                                           .writePolicy = policy})
                                .HasValue());
                    PropertyCurveSet curves{.type = types[index]};
                    for (std::size_t channel = 0; channel < PropertyTrackChannelCount(types[index]); ++channel)
                        curves.channels[channel] = ScalarCurveView::Create(index == 2 ? boolKeys : floatKeys).Value();
                    tracks[index] = {.track = {index + 1, 1}, .targetObject = {index + 1}, .binding = binding, .curves = curves};
                    targets[index] = {binding, {index + 1}, componentType, 1, &component};
                }
                REQUIRE(registry.Freeze().HasValue());
            }

            /** @brief Returns the owner's current borrowed component snapshot. */
            PropertyEvaluationContext Context() const {
                return {scene, targets};
            }

            /** @brief Captures synchronous binding evidence at the explicit host seam. */
            void Publish(const PropertyBindingDiagnosticEvent &event) override {
                events.push_back(event);
            }

            /** @brief Returns a non-optional reporting sink. */
            PropertyDiagnosticSink Sink() {
                return {this};
            }

            /** @brief Creates a controller with the shared registry and current target snapshot. */
            Result<PropertyTrackRuntime> Create() {
                return PropertyTrackRuntime::Create(sequence, Context(), tracks, registry, Sink());
            }
        };
    }  // namespace

    TEST_CASE("Inspector recording and cinematic typed application consume the exact same registry",
              "[unit][cinematic][property-integration][inspector]") {
        Fixture fixture;
        const Editor::InspectorPropertyBindings inspector{fixture.registry};
        std::array<const Runtime::PropertyBindingDescriptor *, 3> properties{};
        REQUIRE(inspector.Enumerate(fixture.componentType, properties).Value() == 3);
        CHECK(properties[0] == fixture.registry.Find(fixture.tracks[0].binding));
        auto created = fixture.Create();
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        CHECK(runtime.IsActive());
        const auto allocations = Tests::AllocationProbe::Count();
        const auto evaluated = runtime.Evaluate(10, fixture.Context());
        REQUIRE(evaluated.HasValue());
        CHECK(evaluated.Value().applied == 3);
        CHECK(Tests::AllocationProbe::Count() == allocations);
        CHECK(std::get<float>(inspector.Read(fixture.targets[0]).Value()) == 10.0F);
        CHECK(std::get<Math::Vec3>(inspector.Read(fixture.targets[1]).Value()) == Math::Vec3{10.0F, 10.0F, 10.0F});
        CHECK(std::get<bool>(inspector.Read(fixture.targets[2]).Value()));
        CHECK(fixture.events.empty());
    }

    TEST_CASE("Required activation surfaces every unresolved typed binding before rejection",
              "[unit][cinematic][property-integration][activation]") {
        Fixture fixture;
        fixture.tracks[0].binding = {999, 1};
        fixture.tracks[1].curves.type = Runtime::PropertyBindingType::Float;
        fixture.tracks[1].curves.channels[1].reset();
        fixture.tracks[1].curves.channels[2].reset();
        fixture.targets[2].component = nullptr;
        const auto runtime = fixture.Create();
        REQUIRE(runtime.HasError());
        REQUIRE(fixture.events.size() == 3);
        CHECK(fixture.events[0].diagnostic.outcome == PropertyBindingEvaluationOutcome::BindingMissing);
        CHECK(fixture.events[1].diagnostic.outcome == PropertyBindingEvaluationOutcome::TypeMismatch);
        CHECK(fixture.events[2].diagnostic.outcome == PropertyBindingEvaluationOutcome::TargetMissing);
        for (const auto &event : fixture.events) {
            CHECK(event.sequence == Fixture::sequence);
            CHECK(event.stage == PropertyDiagnosticStage::Activation);
            CHECK(event.diagnostic.error.has_value());
        }
        CHECK(fixture.component.weight == 0.0F);
    }

    TEST_CASE("Optional unresolved bindings skip only their application and deduplicate diagnostics",
              "[unit][cinematic][property-integration][optional]") {
        Fixture fixture;
        fixture.tracks[0].binding = {999, 1};
        fixture.tracks[0].required = false;
        auto created = fixture.Create();
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        REQUIRE(fixture.events.size() == 1);
        for (const CurveTime time : {1, 4, 10}) {
            auto result = runtime.Evaluate(time, fixture.Context());
            REQUIRE(result.HasValue());
            CHECK(result.Value().applied == 2);
            CHECK(result.Value().diagnostics == 1);
        }
        CHECK(fixture.events.size() == 1);
        CHECK(fixture.component.weight == 0.0F);
        CHECK(fixture.component.enabled);
    }

    TEST_CASE("Scene replacement surfaces stale tracks and invalidates old writes before resolving replacements",
              "[unit][cinematic][property-integration][lifecycle]") {
        Fixture fixture;
        auto created = fixture.Create();
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        const PropertyEvaluationContext missing{{2, 2}, {}};
        CHECK(runtime.Rebind(missing).HasError());
        CHECK_FALSE(runtime.IsActive());
        REQUIRE(fixture.events.size() == 6);
        for (std::size_t index = 0; index < 3; ++index) {
            CHECK(fixture.events[index].stage == PropertyDiagnosticStage::Revalidation);
            CHECK(fixture.events[index].diagnostic.outcome == PropertyBindingEvaluationOutcome::BindingStale);
            CHECK(fixture.events[index + 3].diagnostic.outcome == PropertyBindingEvaluationOutcome::TargetMissing);
        }
        CHECK(runtime.Evaluate(10, fixture.Context()).HasError());
        CHECK(fixture.component.weight == 0.0F);
        Component replacement;
        auto replacementTargets = fixture.targets;
        for (auto &target : replacementTargets) {
            target.component = &replacement;
            target.componentRevision = 2;
        }
        const PropertyEvaluationContext replaced{{2, 3}, replacementTargets};
        REQUIRE(runtime.Rebind(replaced).HasValue());
        REQUIRE(runtime.Evaluate(10, replaced).HasValue());
        CHECK(replacement.weight == 10.0F);
        CHECK(fixture.component.weight == 0.0F);
    }

    TEST_CASE("Missed lifecycle notification still fences stale scene and relocated components",
              "[unit][cinematic][property-integration][lifecycle]") {
        Fixture fixture;
        auto created = fixture.Create();
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        CHECK(runtime.Evaluate(10, {{2, 1}, fixture.targets}).HasError());
        REQUIRE(fixture.events.size() == 3);
        CHECK(runtime.Evaluate(10, {{2, 1}, fixture.targets}).HasError());
        CHECK(fixture.events.size() == 3);
        auto relocated = fixture.targets;
        relocated[0].componentRevision = 2;
        REQUIRE(runtime.Evaluate(10, {Fixture::scene, relocated}).HasValue());
        CHECK(fixture.events.back().diagnostic.outcome == PropertyBindingEvaluationOutcome::BindingStale);
        CHECK(fixture.component.weight == 0.0F);
        CHECK(fixture.component.enabled);
        auto componentMismatch = fixture.targets;
        componentMismatch[0].componentType = Gameplay::ComponentTypeId::Parse("game.cinematic.other").Value();
        REQUIRE(runtime.Rebind({{1, 2}, componentMismatch}).HasError());
        CHECK(fixture.events.back().diagnostic.outcome == PropertyBindingEvaluationOutcome::ComponentMismatch);
    }

    TEST_CASE("Owner setter errors retain exact identity and reappear only after recovery",
              "[unit][cinematic][property-integration][diagnostics]") {
        Fixture fixture;
        auto created = fixture.Create();
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        fixture.component.reject = true;
        REQUIRE(runtime.Evaluate(5, fixture.Context()).HasValue());
        REQUIRE(fixture.events.size() == 1);
        CHECK(fixture.events[0].diagnostic.outcome == PropertyBindingEvaluationOutcome::WriteRejected);
        CHECK(fixture.events[0].diagnostic.error->domain.Value() == Runtime::PropertyBindingErrors::WriteRejected.domain.Value());
        REQUIRE(runtime.Evaluate(6, fixture.Context()).HasValue());
        CHECK(fixture.events.size() == 1);
        fixture.component.reject = false;
        REQUIRE(runtime.Evaluate(7, fixture.Context()).Value().applied == 3);
        fixture.component.reject = true;
        REQUIRE(runtime.Evaluate(8, fixture.Context()).HasValue());
        CHECK(fixture.events.size() == 2);
    }

    TEST_CASE("Property sampling and missing targets surface track-specific failures without blocking healthy properties",
              "[unit][cinematic][property-integration][diagnostics]") {
        Fixture fixture;
        Runtime::PropertyBindingRegistry constrained;
        for (auto descriptor : fixture.registry.Descriptors()) {
            if (descriptor.type == Runtime::PropertyBindingType::Float)
                descriptor.range = {.minimum = 0.0F, .maximum = 1.0F};
            REQUIRE(constrained.Register(std::move(descriptor)).HasValue());
        }
        REQUIRE(constrained.Freeze().HasValue());
        auto created = PropertyTrackRuntime::Create(Fixture::sequence, fixture.Context(), fixture.tracks, constrained, fixture.Sink());
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        auto result = runtime.Evaluate(10, fixture.Context());
        REQUIRE(result.HasValue());
        CHECK(result.Value().sampled == 2);
        CHECK(result.Value().applied == 2);
        CHECK(result.Value().diagnostics == 1);
        REQUIRE(fixture.events.size() == 1);
        CHECK(fixture.events[0].diagnostic.track == fixture.tracks[0].track);
        CHECK(fixture.events[0].diagnostic.outcome == PropertyBindingEvaluationOutcome::ValueOutOfRange);
        CHECK(fixture.component.weight == 0.0F);
        CHECK(fixture.component.enabled);
        result = runtime.Evaluate(10, {Fixture::scene, {}});
        REQUIRE(result.HasValue());
        CHECK(result.Value().applied == 0);
        CHECK(result.Value().diagnostics == 3);
        REQUIRE(fixture.events.size() == 4);
        for (std::size_t index = 1; index < fixture.events.size(); ++index)
            CHECK(fixture.events[index].diagnostic.outcome == PropertyBindingEvaluationOutcome::TargetMissing);
    }

    TEST_CASE("Runtime property failure publishes typed events and process diagnostic history",
              "[unit][cinematic][property-integration][runtime]") {
        Fixture fixture;
        DiagnosticsEngine diagnostics;
        EngineDataBus bus;
        RuntimePropertyDiagnosticSink sink{diagnostics, bus};
        std::vector<PropertyBindingDiagnosticEvent> events;
        auto subscription = bus.Subscribe<PropertyBindingDiagnosticEvent>([&events](const auto &event) {
            events.push_back(event);
        });
        fixture.tracks[0].binding = {999, 1};
        CHECK(PropertyTrackRuntime::Create(Fixture::sequence, fixture.Context(), fixture.tracks, fixture.registry, sink.Sink()).HasError());
        REQUIRE(events.size() == 1);
        CHECK(events[0].diagnostic.track == fixture.tracks[0].track);
        REQUIRE(diagnostics.RecentErrors().size() == 1);
        CHECK(diagnostics.RecentErrors()[0].code.Value() == CinematicErrors::PropertyBindingMissing.code.Value());
        const auto cause = MakeError(Runtime::PropertyBindingErrors::WriteRejected, "Owner denied the write.");
        const auto error = WithCause(MakeError(CinematicErrors::PropertyWriteRejected), cause);
        IPropertyDiagnosticSink &consumer = sink;
        consumer.Publish({Fixture::sequence,
                          Fixture::scene,
                          PropertyDiagnosticStage::Application,
                          {.track = fixture.tracks[0].track, .outcome = PropertyBindingEvaluationOutcome::WriteRejected, .error = error}});
        REQUIRE(events.size() == 2);
        CHECK(events.back().sequence == Fixture::sequence);
        CHECK(events.back().diagnostic.track == fixture.tracks[0].track);
        REQUIRE(events.back().diagnostic.error->cause.Get() != nullptr);
        CHECK(events.back().diagnostic.error->code.Value() == error.code.Value());
        CHECK(events.back().diagnostic.error->cause.Get()->code.Value() == cause.code.Value());
        const auto errors = diagnostics.RecentErrors();
        REQUIRE(errors.size() == 2);
        REQUIRE(errors.back().cause.Get() != nullptr);
        CHECK(errors.back().code.Value() == error.code.Value());
        CHECK(errors.back().cause.Get()->domain.Value() == cause.domain.Value());
        CHECK(errors.back().cause.Get()->code.Value() == cause.code.Value());
    }

    TEST_CASE("Editor property failure reaches Problems and existing source navigation with the same code",
              "[unit][cinematic][property-integration][editor]") {
        Fixture fixture;
        DiagnosticsEngine diagnostics;
        BuildOutputStore output{8};
        const std::array sources{Editor::PropertyTrackSource{fixture.tracks[0].track, 17, 9}};
        Editor::CinematicPropertyProblems problems{diagnostics, output, "/project/non ascii/sequence.json", sources};
        fixture.tracks[0].binding = {999, 1};
        CHECK(PropertyTrackRuntime::Create(Fixture::sequence, fixture.Context(), fixture.tracks, fixture.registry, problems.Sink())
                  .HasError());
        const auto history = diagnostics.RecentDiagnostics();
        REQUIRE(history.size() == 1);
        CHECK(history[0].path == "sequence/5/tracks/1");
        CHECK(history[0].code.Value() == CinematicErrors::PropertyBindingMissing.code.Value());
        REQUIRE(diagnostics.RecentErrors().size() == 1);
        CHECK(diagnostics.RecentErrors()[0].code.Value() == CinematicErrors::PropertyBindingMissing.code.Value());
        CHECK(history[0].location.line == 17);
        CHECK(history[0].location.column == 9);
        const auto snapshot = output.SnapshotIfChanged(0);
        REQUIRE(snapshot.has_value());
        REQUIRE(snapshot->records.size() == 1);
        CHECK(snapshot->records[0].code.Value() == history[0].code.Value());
        REQUIRE(snapshot->records[0].source.has_value());
        CHECK(snapshot->records[0].source->absolutePath == history[0].location.source);
        CHECK(snapshot->records[0].source->line == 17);
        problems.Publish({Fixture::sequence, Fixture::scene, PropertyDiagnosticStage::Application, {}});
        CHECK(diagnostics.RecentDiagnostics().size() == 1);
        const auto cause = MakeError(Runtime::PropertyBindingErrors::WriteRejected, "Owner rejected the request.");
        auto error = WithCause(MakeError(CinematicErrors::PropertyWriteRejected), cause);
        problems.Publish({Fixture::sequence,
                          Fixture::scene,
                          PropertyDiagnosticStage::Application,
                          {.track = fixture.tracks[0].track, .outcome = PropertyBindingEvaluationOutcome::WriteRejected, .error = error}});
        const auto errors = diagnostics.RecentErrors();
        REQUIRE(errors.size() == 2);
        REQUIRE(errors.back().cause.Get() != nullptr);
        CHECK(errors.back().cause.Get()->domain.Value() == cause.domain.Value());
        CHECK(errors.back().cause.Get()->code.Value() == cause.code.Value());
    }

    TEST_CASE("Property inspector rejects invalid snapshots and excludes read-only authoring",
              "[unit][cinematic][property-integration][inspector]") {
        Fixture fixture;
        Editor::InspectorPropertyBindings inspector{fixture.registry};
        std::array<const Runtime::PropertyBindingDescriptor *, 0> empty{};
        CHECK(inspector.Enumerate(fixture.componentType, empty).HasError());
        const auto otherType = Gameplay::ComponentTypeId::Parse("game.cinematic.other").Value();
        CHECK(inspector.Enumerate(otherType, empty).Value() == 0);
        auto target = fixture.targets[0];
        target.binding = {999, 1};
        CHECK(inspector.Read(target).HasError());
        target = fixture.targets[0];
        target.component = nullptr;
        CHECK(inspector.Read(target).HasError());
        target = fixture.targets[0];
        target.componentType = otherType;
        CHECK(inspector.Read(target).HasError());
        Runtime::PropertyBindingRegistry open;
        Editor::InspectorPropertyBindings openInspector{open};
        CHECK(openInspector.Enumerate(fixture.componentType, empty).HasError());
        CHECK(openInspector.Read(fixture.targets[0]).HasError());
        Fixture readOnly{Runtime::PropertyWritePolicy::ReadOnly};
        CHECK(Editor::InspectorPropertyBindings{readOnly.registry}.Enumerate(readOnly.componentType, empty).Value() == 0);
        auto created = readOnly.Create();
        REQUIRE(created.HasValue());
        auto runtime = std::move(created).Value();
        CHECK(runtime.Evaluate(5, readOnly.Context()).Value().applied == 0);
        CHECK(readOnly.events.size() == 3);
    }

    TEST_CASE("Property runtime structural admission failures still surface diagnostics when a sink exists",
              "[unit][cinematic][property-integration][validation]") {
        Fixture fixture;
        CHECK(PropertyTrackRuntime::Create({}, fixture.Context(), fixture.tracks, fixture.registry, fixture.Sink()).HasError());
        CHECK(PropertyTrackRuntime::Create(Fixture::sequence, fixture.Context(), fixture.tracks, fixture.registry, {}).HasError());
        Runtime::PropertyBindingRegistry open;
        CHECK(PropertyTrackRuntime::Create(Fixture::sequence, fixture.Context(), fixture.tracks, open, fixture.Sink()).HasError());
        REQUIRE(fixture.events.size() == 1);
        CHECK(fixture.events[0].diagnostic.error->code.Value() == CinematicErrors::PropertyRegistryUnfrozen.code.Value());
        CHECK(PropertyTrackRuntime::Create(Fixture::sequence, {{0, 0}, fixture.targets}, fixture.tracks, fixture.registry, fixture.Sink())
                  .HasError());
        REQUIRE(fixture.events.size() == 2);
        std::array<PropertyEvaluationDiagnostic, 3> diagnostics{};
        CHECK(PropertyEvaluationPlan::InspectBindings(fixture.tracks, fixture.targets, fixture.registry, {}).HasError());
        auto targets = fixture.targets;
        targets[0].componentRevision = 0;
        CHECK(PropertyEvaluationPlan::InspectBindings(fixture.tracks, targets, fixture.registry, diagnostics).HasError());
        fixture.tracks[0].version.major = 9;
        CHECK(PropertyEvaluationPlan::InspectBindings(fixture.tracks, fixture.targets, fixture.registry, diagnostics).HasError());
        auto plan = PropertyEvaluationPlan::Create(Fixture::scene, std::span{fixture.tracks}.subspan(1), fixture.targets, fixture.registry);
        REQUIRE(plan.HasValue());
        CHECK(plan.Value().Revalidate(fixture.Context(), {}).HasError());
    }
}  // namespace Horo::Cinematic
