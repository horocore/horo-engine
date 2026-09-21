#include "Horo/Cinematic/PropertyTrack.h"

#include "Horo/Cinematic/CinematicErrors.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        template <typename T> [[nodiscard]] Result<T> RejectProperty(const ErrorCodeDescriptor &code, std::string message = {}) {
            return Result<T>::Failure(MakeError(code, std::move(message)));
        }

        [[nodiscard]] std::optional<std::size_t> FindTargetIndex(const std::span<const PropertyBindingTargetSnapshot> targets,
                                                                 const PropertyBindingId binding,
                                                                 const Runtime::SceneObjectId targetObject) noexcept {
            const auto found = std::ranges::find_if(targets, [binding, targetObject](const PropertyBindingTargetSnapshot &target) {
                return target.binding == binding && target.targetObject == targetObject;
            });
            return found == targets.end() ? std::nullopt : std::optional<std::size_t>(static_cast<std::size_t>(found - targets.begin()));
        }

        [[nodiscard]] bool SameTarget(const PropertyBindingTargetSnapshot &left, const PropertyBindingTargetSnapshot &right) noexcept {
            return left.binding == right.binding && left.targetObject == right.targetObject && left.componentType == right.componentType &&
                   left.componentRevision == right.componentRevision && left.component == right.component;
        }

        [[nodiscard]] bool IsFiniteRange(const Runtime::PropertyRangeConstraint &range) noexcept {
            if (range.minimum && !std::isfinite(*range.minimum))
                return false;
            if (range.maximum && !std::isfinite(*range.maximum))
                return false;
            return !range.minimum || !range.maximum || *range.minimum <= *range.maximum;
        }

        [[nodiscard]] bool IsWithinRange(const Runtime::PropertyRangeConstraint &range, const float value) noexcept {
            return (!range.minimum || value >= *range.minimum) && (!range.maximum || value <= *range.maximum);
        }

        [[nodiscard]] bool IsWithinRange(const Runtime::PropertyRangeConstraint &range,
                                         const Runtime::PropertyBindingValue &value) noexcept {
            const auto check = [&range](const float component) {
                return std::isfinite(component) && IsWithinRange(range, component);
            };
            return std::visit([&check](const auto &typed) {
                using Value = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Value, float>) {
                    return check(typed);
                } else if constexpr (std::is_same_v<Value, bool>) {
                    return true;
                } else if constexpr (std::is_same_v<Value, Math::Vec2>) {
                    return check(typed.x) && check(typed.y);
                } else if constexpr (std::is_same_v<Value, Math::Vec3>) {
                    return check(typed.x) && check(typed.y) && check(typed.z);
                } else {
                    return check(typed.x) && check(typed.y) && check(typed.z) && check(typed.w);
                }
            }, value);
        }

        [[nodiscard]] bool MatchesType(const Runtime::PropertyBindingType type, const Runtime::PropertyBindingValue &value) noexcept {
            switch (type) {
                case Runtime::PropertyBindingType::Float:
                    return std::holds_alternative<float>(value);
                case Runtime::PropertyBindingType::Vec2:
                    return std::holds_alternative<Math::Vec2>(value);
                case Runtime::PropertyBindingType::Vec3:
                    return std::holds_alternative<Math::Vec3>(value);
                case Runtime::PropertyBindingType::Vec4:
                    return std::holds_alternative<Math::Vec4>(value);
                case Runtime::PropertyBindingType::Boolean:
                    return std::holds_alternative<bool>(value);
                case Runtime::PropertyBindingType::Count:
                    return false;
            }
            return false;
        }

        [[nodiscard]] Result<void> ValidateCurveSet(const PropertyCurveSet &curves) {
            const std::size_t channelCount = PropertyTrackChannelCount(curves.type);
            if (channelCount == 0)
                return RejectProperty<void>(CinematicErrors::PropertyMalformed, "The property curve type is invalid.");
            for (std::size_t channel = 0; channel < curves.channels.size(); ++channel) {
                if (channel < channelCount) {
                    if (!curves.channels[channel].has_value() || curves.channels[channel]->Keys().empty())
                        return RejectProperty<void>(CinematicErrors::PropertyMalformed,
                                                    "The property curve is missing a required scalar channel.");
                    if (curves.type == Runtime::PropertyBindingType::Boolean) {
                        for (const ScalarCurveKey &key : curves.channels[channel]->Keys()) {
                            if (key.interpolation != CurveInterpolation::Constant || (key.value != 0.0F && key.value != 1.0F))
                                return RejectProperty<void>(CinematicErrors::PropertyMalformed,
                                                            "Boolean property curves must contain constant zero/one keys.");
                        }
                    }
                } else if (curves.channels[channel].has_value()) {
                    return RejectProperty<void>(CinematicErrors::PropertyMalformed,
                                                "The property curve contains an unused scalar channel.");
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<PropertyBindingValue> SampleValue(const PropertyTrackDescriptor &track, const CurveTime time) {
            const std::size_t channelCount = PropertyTrackChannelCount(track.curves.type);
            std::array<float, 4> samples{};
            for (std::size_t channel = 0; channel < channelCount; ++channel) {
                auto sampled = track.curves.channels[channel]->Sample(time);
                if (sampled.HasError())
                    return Result<PropertyBindingValue>::Failure(WrapError(CinematicErrors::PropertySampleInvalid,
                                                                           std::move(sampled).ErrorValue(),
                                                                           "A property curve could not be sampled."));
                samples[channel] = sampled.Value().value;
                if (!std::isfinite(samples[channel]))
                    return RejectProperty<PropertyBindingValue>(CinematicErrors::PropertySampleInvalid,
                                                                "A property curve produced a non-finite sample.");
            }

            switch (track.curves.type) {
                case Runtime::PropertyBindingType::Float:
                    return Result<PropertyBindingValue>::Success(samples[0]);
                case Runtime::PropertyBindingType::Vec2:
                    return Result<PropertyBindingValue>::Success(Math::Vec2{samples[0], samples[1]});
                case Runtime::PropertyBindingType::Vec3:
                    return Result<PropertyBindingValue>::Success(Math::Vec3{samples[0], samples[1], samples[2]});
                case Runtime::PropertyBindingType::Vec4:
                    return Result<PropertyBindingValue>::Success(Math::Vec4{samples[0], samples[1], samples[2], samples[3]});
                case Runtime::PropertyBindingType::Boolean:
                    return Result<PropertyBindingValue>::Success(samples[0] >= 0.5F);
                case Runtime::PropertyBindingType::Count:
                    break;
            }
            return RejectProperty<PropertyBindingValue>(CinematicErrors::PropertyMalformed, "The property curve type is not supported.");
        }

        [[nodiscard]] PropertyEvaluationDiagnostic Diagnostic(const PropertyTrackDescriptor &track,
                                                              const PropertyBindingEvaluationOutcome outcome,
                                                              const ErrorCodeDescriptor &descriptor) {
            return {.track = track.track,
                    .binding = track.binding,
                    .targetObject = track.targetObject,
                    .outcome = outcome,
                    .error = MakeError(descriptor)};
        }

        [[nodiscard]] const ErrorCodeDescriptor &DiagnosticError(const PropertyBindingEvaluationOutcome outcome) noexcept {
            switch (outcome) {
                case PropertyBindingEvaluationOutcome::BindingMissing:
                    return CinematicErrors::PropertyBindingMissing;
                case PropertyBindingEvaluationOutcome::BindingStale:
                    return CinematicErrors::PropertyBindingStale;
                case PropertyBindingEvaluationOutcome::TargetMissing:
                    return CinematicErrors::PropertyBindingTargetMissing;
                case PropertyBindingEvaluationOutcome::ComponentMismatch:
                    return CinematicErrors::PropertyComponentMismatch;
                case PropertyBindingEvaluationOutcome::TypeMismatch:
                    return CinematicErrors::PropertyTypeMismatch;
                case PropertyBindingEvaluationOutcome::ValueOutOfRange:
                    return CinematicErrors::PropertySampleInvalid;
                case PropertyBindingEvaluationOutcome::ReadOnly:
                case PropertyBindingEvaluationOutcome::WriteRejected:
                    return CinematicErrors::PropertyWriteRejected;
                case PropertyBindingEvaluationOutcome::Applied:
                case PropertyBindingEvaluationOutcome::Count:
                    return CinematicErrors::PropertyMalformed;
            }
            return CinematicErrors::PropertyMalformed;
        }

        [[nodiscard]] bool MatchesTrack(const PropertyEvaluationValue &value, const PropertyTrackDescriptor &track) noexcept {
            return value.track == track.track && value.binding == track.binding && value.targetObject == track.targetObject;
        }

        [[nodiscard]] Result<void> ValidateTrackDescriptor(const PropertyTrackDescriptor &track,
                                                           const std::span<const PropertyTrackDescriptor> priorTracks) {
            if (track.version != CurrentPropertyTrackVersion)
                return RejectProperty<void>(CinematicErrors::PropertyVersionUnsupported,
                                            "The property track requires an explicit compatible-version migration.");
            if (!track.track.IsValid() || !track.targetObject.IsValid() || !track.binding.IsValid())
                return RejectProperty<void>(CinematicErrors::PropertyMalformed, "A property track identity or target is invalid.");
            if (auto curves = ValidateCurveSet(track.curves); curves.HasError())
                return Result<void>::Failure(std::move(curves).ErrorValue());
            if (std::ranges::any_of(priorTracks, [&track](const PropertyTrackDescriptor &prior) {
                return prior.track == track.track;
            }))
                return RejectProperty<void>(CinematicErrors::PropertyMalformed, "Property tracks contain a duplicate track identity.");
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> PropertyEvaluationPlan::ValidateTargetSnapshots(const std::span<const PropertyBindingTargetSnapshot> targets) {
        for (std::size_t index = 0; index < targets.size(); ++index) {
            const PropertyBindingTargetSnapshot &target = targets[index];
            if (!target.binding.IsValid() || !target.targetObject.IsValid() || !target.componentType.IsValid() ||
                target.componentRevision == 0)
                return RejectProperty<void>(CinematicErrors::PropertyMalformed,
                                            "A property target snapshot contains an invalid identity or revision.");
            if (target.component == nullptr)
                return RejectProperty<void>(CinematicErrors::PropertyBindingTargetMissing,
                                            "A property target has no live component instance.");
            if (std::ranges::find_if(targets.first(index), [&target](const PropertyBindingTargetSnapshot &entry) {
                return entry.binding == target.binding && entry.targetObject == target.targetObject;
            }) != targets.first(index).end())
                return RejectProperty<void>(CinematicErrors::PropertyMalformed,
                                            "Property target snapshots contain a duplicate binding target.");
        }
        return Result<void>::Success();
    }

    Result<PropertyEvaluationPlan::CompiledTrack> PropertyEvaluationPlan::CompileTrack(
        const PropertyTrackDescriptor &track, const std::span<const PropertyTrackDescriptor> priorTracks,
        const std::span<const PropertyBindingTargetSnapshot> targets, const Runtime::PropertyBindingRegistry &registry) {
        if (auto validated = ValidateTrackDescriptor(track, priorTracks); validated.HasError())
            return Result<CompiledTrack>::Failure(std::move(validated).ErrorValue());

        const Runtime::PropertyBindingDescriptor *binding = registry.Find(track.binding);
        if (binding == nullptr) {
            if (track.required)
                return RejectProperty<CompiledTrack>(CinematicErrors::PropertyBindingMissing,
                                                     "The required property binding is absent from the registry.");
            return Result<CompiledTrack>::Success(
                {track, nullptr, std::nullopt, MaximumPropertyTracks, PropertyBindingEvaluationOutcome::BindingMissing});
        }
        if (!IsFiniteRange(binding->range))
            return RejectProperty<CompiledTrack>(CinematicErrors::PropertyMalformed,
                                                 "The property binding range is not finite or ordered.");
        if (binding->type != track.curves.type) {
            if (track.required)
                return RejectProperty<CompiledTrack>(CinematicErrors::PropertyTypeMismatch,
                                                     "The property curve type does not match its binding descriptor.");
            return Result<CompiledTrack>::Success(
                {track, binding, std::nullopt, MaximumPropertyTracks, PropertyBindingEvaluationOutcome::TypeMismatch});
        }

        const auto targetIndex = FindTargetIndex(targets, track.binding, track.targetObject);
        if (!targetIndex.has_value()) {
            if (track.required)
                return RejectProperty<CompiledTrack>(CinematicErrors::PropertyBindingTargetMissing,
                                                     "The required property target is absent from the scene snapshot.");
            return Result<CompiledTrack>::Success(
                {track, binding, std::nullopt, MaximumPropertyTracks, PropertyBindingEvaluationOutcome::TargetMissing});
        }
        const PropertyBindingTargetSnapshot &target = targets[*targetIndex];
        if (target.componentType != binding->componentType) {
            if (track.required)
                return RejectProperty<CompiledTrack>(CinematicErrors::PropertyComponentMismatch,
                                                     "The property target component type does not match its binding.");
            return Result<CompiledTrack>::Success(
                {track, binding, std::nullopt, MaximumPropertyTracks, PropertyBindingEvaluationOutcome::ComponentMismatch});
        }
        return Result<CompiledTrack>::Success({track, binding, target, *targetIndex, PropertyBindingEvaluationOutcome::Applied});
    }

    /** @copydoc PropertyEvaluationPlan::Create */
    Result<PropertyEvaluationPlan> PropertyEvaluationPlan::Create(const PropertySceneVersion scene,
                                                                  const std::span<const PropertyTrackDescriptor> tracks,
                                                                  const std::span<const PropertyBindingTargetSnapshot> targets,
                                                                  const Runtime::PropertyBindingRegistry &registry) {
        if (scene.sceneGeneration == 0 || scene.bindingRevision == 0)
            return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyMalformed,
                                                          "Property scene and binding generations must be non-zero.");
        if (!registry.IsFrozen())
            return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyRegistryUnfrozen,
                                                          "Property activation requires a frozen binding registry snapshot.");
        if (tracks.empty() || tracks.size() > MaximumPropertyTracks || targets.size() > MaximumPropertyTracks)
            return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyLimitExceeded,
                                                          "Property activation exceeds the compiled track or target limit.");
        if (auto validated = ValidateTargetSnapshots(targets); validated.HasError())
            return Result<PropertyEvaluationPlan>::Failure(std::move(validated).ErrorValue());

        std::vector<CompiledTrack> compiled;
        compiled.reserve(tracks.size());
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            auto compiledTrack = CompileTrack(tracks[index], tracks.first(index), targets, registry);
            if (compiledTrack.HasError())
                return Result<PropertyEvaluationPlan>::Failure(std::move(compiledTrack).ErrorValue());
            compiled.push_back(std::move(compiledTrack).Value());
        }

        std::ranges::sort(compiled, {}, [](const CompiledTrack &track) {
            return std::pair{track.track.track.stableValue, track.track.track.generation};
        });
        return Result<PropertyEvaluationPlan>::Success(PropertyEvaluationPlan{scene, std::move(compiled)});
    }

    /** @copydoc PropertyEvaluationPlan::Evaluate */
    Result<std::size_t> PropertyEvaluationPlan::Evaluate(const CurveTime time, const PropertyEvaluationContext &context,
                                                         const std::span<PropertyEvaluationValue> output) const {
        if (context.scene != scene_)
            return RejectProperty<std::size_t>(CinematicErrors::PropertyBindingStale,
                                               "The property plan does not belong to the active scene binding generation.");
        if (output.size() < orderedTracks_.size())
            return RejectProperty<std::size_t>(CinematicErrors::PropertyLimitExceeded,
                                               "Caller output storage is smaller than the admitted property-track count.");

        std::size_t outputCount = 0;
        for (const CompiledTrack &compiled : orderedTracks_) {
            if (compiled.binding == nullptr || !compiled.activationTarget.has_value())
                continue;
            if (compiled.activationTargetIndex >= context.targets.size())
                return RejectProperty<std::size_t>(CinematicErrors::PropertyBindingTargetMissing,
                                                   "A required property target disappeared from the scene snapshot.");
            const PropertyBindingTargetSnapshot &target = context.targets[compiled.activationTargetIndex];
            if (!SameTarget(target, *compiled.activationTarget))
                return RejectProperty<std::size_t>(CinematicErrors::PropertyBindingStale,
                                                   "A property component or binding generation changed after activation.");

            auto sampled = SampleValue(compiled.track, time);
            if (sampled.HasError())
                return Result<std::size_t>::Failure(std::move(sampled).ErrorValue());
            if (!IsWithinRange(compiled.binding->range, sampled.Value()))
                return RejectProperty<std::size_t>(CinematicErrors::PropertySampleInvalid,
                                                   "A property sample violates its binding range constraint.");
            output[outputCount++] = {.track = compiled.track.track,
                                     .binding = compiled.track.binding,
                                     .targetObject = compiled.track.targetObject,
                                     .value = sampled.Value()};
        }
        return Result<std::size_t>::Success(outputCount);
    }

    bool PropertyEvaluationPlan::ApplyTrack(const CompiledTrack &compiled, const PropertyEvaluationContext &context,
                                            const PropertyEvaluationValue &value, PropertyEvaluationDiagnostic &diagnostic) const {
        const auto reject = [&diagnostic, &compiled](const PropertyBindingEvaluationOutcome outcome,
                                                     const ErrorCodeDescriptor &descriptor) {
            diagnostic = Diagnostic(compiled.track, outcome, descriptor);
            return false;
        };
        if (compiled.activationTargetIndex >= context.targets.size())
            return reject(PropertyBindingEvaluationOutcome::TargetMissing, CinematicErrors::PropertyBindingTargetMissing);

        const PropertyBindingTargetSnapshot &target = context.targets[compiled.activationTargetIndex];
        if (!compiled.activationTarget.has_value() || !SameTarget(target, *compiled.activationTarget))
            return reject(PropertyBindingEvaluationOutcome::BindingStale, CinematicErrors::PropertyBindingStale);
        if (target.componentType != compiled.binding->componentType)
            return reject(PropertyBindingEvaluationOutcome::ComponentMismatch, CinematicErrors::PropertyComponentMismatch);
        if (compiled.binding->writePolicy == Runtime::PropertyWritePolicy::ReadOnly)
            return reject(PropertyBindingEvaluationOutcome::ReadOnly, CinematicErrors::PropertyWriteRejected);
        if (!MatchesType(compiled.binding->type, value.value))
            return reject(PropertyBindingEvaluationOutcome::TypeMismatch, CinematicErrors::PropertyTypeMismatch);
        if (!IsWithinRange(compiled.binding->range, value.value))
            return reject(PropertyBindingEvaluationOutcome::ValueOutOfRange, CinematicErrors::PropertySampleInvalid);

        auto written = compiled.binding->setter(target.component, value.value);
        if (written.HasError()) {
            diagnostic =
                Diagnostic(compiled.track, PropertyBindingEvaluationOutcome::WriteRejected, CinematicErrors::PropertyWriteRejected);
            diagnostic.error = std::move(written).ErrorValue();
            return false;
        }
        return true;
    }

    /** @copydoc PropertyEvaluationPlan::Apply */
    Result<PropertyEvaluationResult> PropertyEvaluationPlan::Apply(const PropertyEvaluationContext &context,
                                                                   const std::span<const PropertyEvaluationValue> values,
                                                                   const std::span<PropertyEvaluationDiagnostic> diagnostics) const {
        if (context.scene != scene_)
            return RejectProperty<PropertyEvaluationResult>(CinematicErrors::PropertyBindingStale,
                                                            "The property plan does not belong to the active scene binding generation.");
        if (values.size() > orderedTracks_.size() || diagnostics.size() < orderedTracks_.size())
            return RejectProperty<PropertyEvaluationResult>(CinematicErrors::PropertyLimitExceeded,
                                                            "Property application scratch storage is outside the admitted bound.");

        PropertyEvaluationResult result{};
        std::size_t valueIndex = 0;
        for (const CompiledTrack &compiled : orderedTracks_) {
            if (compiled.activationOutcome != PropertyBindingEvaluationOutcome::Applied) {
                diagnostics[result.diagnostics++] =
                    Diagnostic(compiled.track, compiled.activationOutcome, DiagnosticError(compiled.activationOutcome));
                continue;
            }
            if (valueIndex >= values.size())
                return RejectProperty<PropertyEvaluationResult>(CinematicErrors::PropertyLimitExceeded,
                                                                "A resolved property track has no sampled value to apply.");
            const PropertyEvaluationValue &value = values[valueIndex++];
            if (!MatchesTrack(value, compiled.track))
                return RejectProperty<PropertyEvaluationResult>(CinematicErrors::PropertyMalformed,
                                                                "Sampled property values are not ordered by the active plan.");
            ++result.sampled;

            PropertyEvaluationDiagnostic diagnostic{};
            if (ApplyTrack(compiled, context, value, diagnostic))
                ++result.applied;
            else
                diagnostics[result.diagnostics++] = std::move(diagnostic);
        }
        return Result<PropertyEvaluationResult>::Success(result);
    }

    /** @copydoc PropertyEvaluationPlan::EvaluateAndApply */
    Result<PropertyEvaluationResult> PropertyEvaluationPlan::EvaluateAndApply(
        const CurveTime time, const PropertyEvaluationContext &context, const std::span<PropertyEvaluationValue> values,
        const std::span<PropertyEvaluationDiagnostic> diagnostics) const {
        auto sampled = Evaluate(time, context, values);
        if (sampled.HasError())
            return Result<PropertyEvaluationResult>::Failure(std::move(sampled).ErrorValue());
        return Apply(context, values.first(sampled.Value()), diagnostics);
    }

    /** @copydoc PropertyEvaluationPlan::SceneVersion */
    PropertySceneVersion PropertyEvaluationPlan::SceneVersion() const noexcept {
        return scene_;
    }

    /** @copydoc PropertyEvaluationPlan::TrackCount */
    std::size_t PropertyEvaluationPlan::TrackCount() const noexcept {
        return orderedTracks_.size();
    }

    PropertyEvaluationPlan::PropertyEvaluationPlan(const PropertySceneVersion scene, std::vector<CompiledTrack> orderedTracks) noexcept
        : scene_(scene), orderedTracks_(std::move(orderedTracks)) {}
}  // namespace Horo::Cinematic
