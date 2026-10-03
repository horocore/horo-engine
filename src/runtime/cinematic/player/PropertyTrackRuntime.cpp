#include "Horo/Cinematic/PropertyTrackRuntime.h"

#include "Horo/Cinematic/CinematicErrors.h"
#include "Horo/Foundation/Logging/Logger.h"

#include <algorithm>
#include <array>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        /** @brief Compares typed failure identity for bounded per-track deduplication. */
        bool SameFailure(const PropertyEvaluationDiagnostic &left, const PropertyEvaluationDiagnostic &right) {
            return left.outcome == right.outcome && left.error.has_value() == right.error.has_value() &&
                   (!left.error.has_value() ||
                    (left.error->domain.Value() == right.error->domain.Value() && left.error->code.Value() == right.error->code.Value()));
        }
    }  // namespace

    /** @copydoc RuntimePropertyDiagnosticSink::RuntimePropertyDiagnosticSink */
    RuntimePropertyDiagnosticSink::RuntimePropertyDiagnosticSink(DiagnosticsEngine &diagnostics, EngineDataBus &bus) noexcept
        : diagnostics_(diagnostics), bus_(bus) {}

    /** @copydoc RuntimePropertyDiagnosticSink::Sink */
    PropertyDiagnosticSink RuntimePropertyDiagnosticSink::Sink() noexcept {
        return {this};
    }

    /** @copydoc RuntimePropertyDiagnosticSink::Publish */
    void RuntimePropertyDiagnosticSink::Publish(const PropertyBindingDiagnosticEvent &event) {
        if (event.diagnostic.error.has_value()) {
            diagnostics_.Report(*event.diagnostic.error);
            const auto &error = *event.diagnostic.error;
            const std::array fields{Telemetry::Field{"sequence", event.sequence.stableValue},
                                    Telemetry::Field{"sequence_generation", std::uint64_t{event.sequence.generation}},
                                    Telemetry::Field{"track", event.diagnostic.track.stableValue},
                                    Telemetry::Field{"track_generation", std::uint64_t{event.diagnostic.track.generation}},
                                    Telemetry::Field{"binding", event.diagnostic.binding.stableValue},
                                    Telemetry::Field{"binding_generation", std::uint64_t{event.diagnostic.binding.generation}},
                                    Telemetry::Field{"target", event.diagnostic.targetObject.value},
                                    Telemetry::Field{"scene_generation", event.scene.sceneGeneration},
                                    Telemetry::Field{"binding_revision", event.scene.bindingRevision},
                                    Telemetry::Field{"stage", static_cast<std::uint64_t>(event.stage)},
                                    Telemetry::Field{"outcome", static_cast<std::uint64_t>(event.diagnostic.outcome)},
                                    Telemetry::Field{"error_domain", error.domain.Value()},
                                    Telemetry::Field{"error_code", error.code.Value()}};
            Log::Logger::Write("cinematic.property_binding", Log::Level::Error, error.message, fields);
        }
        bus_.Publish(event);
    }

    PropertyTrackRuntime::PropertyTrackRuntime(const SequenceId sequence, std::vector<PropertyTrackDescriptor> tracks,
                                               const Runtime::PropertyBindingRegistry &registry, const PropertyDiagnosticSink sink)
        : sequence_(sequence), tracks_(std::move(tracks)), registry_(&registry), sink_(sink), values_(tracks_.size()),
          diagnostics_(tracks_.size()), reported_(tracks_.size()) {
        std::ranges::sort(tracks_, {}, &PropertyTrackDescriptor::track);
    }

    /** @copydoc PropertyTrackRuntime::Create */
    Result<PropertyTrackRuntime> PropertyTrackRuntime::Create(const SequenceId sequence, const PropertyEvaluationContext &context,
                                                              const std::span<const PropertyTrackDescriptor> tracks,
                                                              const Runtime::PropertyBindingRegistry &registry,
                                                              const PropertyDiagnosticSink sink) {
        if (!sequence.IsValid() || sink.consumer == nullptr || tracks.empty() || tracks.size() > MaximumPropertyTracks)
            return Result<PropertyTrackRuntime>::Failure(MakeError(CinematicErrors::PropertyMalformed));
        PropertyTrackRuntime runtime{sequence, {tracks.begin(), tracks.end()}, registry, sink};
        if (auto activated = runtime.Activate(context); activated.HasError())
            return Result<PropertyTrackRuntime>::Failure(std::move(activated).ErrorValue());
        return Result<PropertyTrackRuntime>::Success(std::move(runtime));
    }

    Result<void> PropertyTrackRuntime::Activate(const PropertyEvaluationContext &context) {
        auto inspected = PropertyEvaluationPlan::InspectBindings(tracks_, context.targets, *registry_, diagnostics_);
        if (inspected.HasError()) {
            PublishFailure(PropertyDiagnosticStage::Activation, context.scene, inspected.ErrorValue());
            return Result<void>::Failure(std::move(inspected).ErrorValue());
        }
        const auto failures = std::span{diagnostics_}.first(inspected.Value());
        Publish(PropertyDiagnosticStage::Activation, context.scene, failures);
        for (const auto &failure : failures) {
            const auto track = std::ranges::find(tracks_, failure.track, &PropertyTrackDescriptor::track);
            if (track != tracks_.end() && track->required)
                return Result<void>::Failure(*failure.error);
        }
        auto plan = PropertyEvaluationPlan::Create(context.scene, tracks_, context.targets, *registry_);
        if (plan.HasError()) {
            PublishFailure(PropertyDiagnosticStage::Activation, context.scene, plan.ErrorValue());
            return Result<void>::Failure(std::move(plan).ErrorValue());
        }
        plan_ = std::move(plan).Value();
        return Result<void>::Success();
    }

    void PropertyTrackRuntime::Publish(const PropertyDiagnosticStage stage, const PropertySceneVersion scene,
                                       const std::span<const PropertyEvaluationDiagnostic> diagnostics) {
        std::size_t failureIndex{};
        for (std::size_t index = 0; index < tracks_.size(); ++index) {
            if (failureIndex == diagnostics.size() || diagnostics[failureIndex].track != tracks_[index].track) {
                reported_[index].reset();
                continue;
            }
            const auto &failure = diagnostics[failureIndex++];
            if (!reported_[index].has_value() || !SameFailure(*reported_[index], failure))
                sink_.consumer->Publish({sequence_, scene, stage, failure});
            reported_[index] = failure;
        }
    }

    void PropertyTrackRuntime::PublishFailure(const PropertyDiagnosticStage stage, const PropertySceneVersion scene, const Error &error) {
        sink_.consumer->Publish({sequence_, scene, stage, {.error = error}});
    }

    /** @copydoc PropertyTrackRuntime::Rebind */
    Result<void> PropertyTrackRuntime::Rebind(const PropertyEvaluationContext &context) {
        if (plan_.has_value()) {
            auto revalidated = plan_->Revalidate(context, diagnostics_);
            if (revalidated.HasError())
                return Result<void>::Failure(std::move(revalidated).ErrorValue());
            Publish(PropertyDiagnosticStage::Revalidation, context.scene, std::span{diagnostics_}.first(revalidated.Value()));
        }
        plan_.reset();
        for (auto &reported : reported_)
            reported.reset();
        return Activate(context);
    }

    /** @copydoc PropertyTrackRuntime::Evaluate */
    Result<PropertyEvaluationResult> PropertyTrackRuntime::Evaluate(const CurveTime time, const PropertyEvaluationContext &context) {
        using enum PropertyDiagnosticStage;
        if (!plan_.has_value()) {
            auto error = MakeError(CinematicErrors::PropertyBindingStale, "Property playback requires a successful rebind.");
            PublishFailure(Application, context.scene, error);
            return Result<PropertyEvaluationResult>::Failure(std::move(error));
        }
        auto revalidated = plan_->Revalidate(context, diagnostics_);
        if (revalidated.HasError())
            return Result<PropertyEvaluationResult>::Failure(std::move(revalidated).ErrorValue());
        if (context.scene != plan_->SceneVersion()) {
            Publish(Revalidation, context.scene, std::span{diagnostics_}.first(revalidated.Value()));
            return Result<PropertyEvaluationResult>::Failure(MakeError(CinematicErrors::PropertyBindingStale));
        }
        auto evaluated = plan_->EvaluateAndApply(time, context, values_, diagnostics_);
        if (evaluated.HasError()) {
            // Preserve per-track stale-target evidence even when batch sampling refuses to proceed.
            if (revalidated.Value() != 0)
                Publish(Revalidation, context.scene, std::span{diagnostics_}.first(revalidated.Value()));
            else
                PublishFailure(Application, context.scene, evaluated.ErrorValue());
            return evaluated;
        }
        Publish(Application, context.scene, std::span{diagnostics_}.first(evaluated.Value().diagnostics));
        return evaluated;
    }

    /** @copydoc PropertyTrackRuntime::IsActive */
    bool PropertyTrackRuntime::IsActive() const noexcept {
        return plan_.has_value();
    }
}  // namespace Horo::Cinematic
