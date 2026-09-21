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

        [[nodiscard]] const PropertyBindingTargetSnapshot *FindTarget(
            const std::span<const PropertyBindingTargetSnapshot> targets, const PropertyBindingId binding,
            const Runtime::SceneObjectId targetObject) noexcept {
            const auto found = std::ranges::find_if(targets, [binding, targetObject](const PropertyBindingTargetSnapshot &target) {
                return target.binding == binding && target.targetObject == targetObject;
            });
            return found == targets.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] bool SameTarget(const PropertyBindingTargetSnapshot &left,
                                      const PropertyBindingTargetSnapshot &right) noexcept {
            return left.binding == right.binding && left.targetObject == right.targetObject &&
                   left.componentType == right.componentType && left.componentRevision == right.componentRevision &&
                   left.component == right.component;
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

        [[nodiscard]] bool IsWithinRange(const Runtime::PropertyRangeConstraint &range, const Runtime::PropertyBindingValue &value) noexcept {
            const auto check = [&range](const float component) {
                return std::isfinite(component) && IsWithinRange(range, component);
            };
            return std::visit(
                [&check](const auto &typed) {
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
                },
                value);
        }

        [[nodiscard]] bool MatchesType(const Runtime::PropertyBindingType type,
                                       const Runtime::PropertyBindingValue &value) noexcept {
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
                    return Result<PropertyBindingValue>::Failure(
                        WrapError(CinematicErrors::PropertySampleInvalid, std::move(sampled).ErrorValue(),
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
            return RejectProperty<PropertyBindingValue>(CinematicErrors::PropertyMalformed,
                                                        "The property curve type is not supported.");
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

        [[nodiscard]] const PropertyEvaluationValue *FindValue(const std::span<const PropertyEvaluationValue> values,
                                                               const PropertyTrackDescriptor &track) noexcept {
            const auto found = std::ranges::find_if(values, [&track](const PropertyEvaluationValue &value) {
                return value.track == track.track && value.binding == track.binding && value.targetObject == track.targetObject;
            });
            return found == values.end() ? nullptr : std::to_address(found);
        }
    }  // namespace

    /** @copydoc PropertyEvaluationPlan::Create */
    Result<PropertyEvaluationPlan> PropertyEvaluationPlan::Create(
        const PropertySceneVersion scene, const std::span<const PropertyTrackDescriptor> tracks,
        const std::span<const PropertyBindingTargetSnapshot> targets, const Runtime::PropertyBindingRegistry &registry) {
        if (scene.sceneGeneration == 0 || scene.bindingRevision == 0)
            return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyMalformed,
                                                           "Property scene and binding generations must be non-zero.");
        if (!registry.IsFrozen())
            return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyRegistryUnfrozen,
                                                           "Property activation requires a frozen binding registry snapshot.");
        if (tracks.empty() || tracks.size() > MaximumPropertyTracks || targets.size() > MaximumPropertyTracks)
            return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyLimitExceeded,
                                                           "Property activation exceeds the compiled track or target limit.");

        for (std::size_t index = 0; index < targets.size(); ++index) {
            const PropertyBindingTargetSnapshot &target = targets[index];
            if (!target.binding.IsValid() || !target.targetObject.IsValid() || !target.componentType.IsValid() ||
                target.componentRevision == 0)
                return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyMalformed,
                                                               "A property target snapshot contains an invalid identity or revision.");
            if (target.component == nullptr)
                return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyBindingTargetMissing,
                                                               "A property target has no live component instance.");
            if (std::ranges::find_if(targets.first(index), [&target](const PropertyBindingTargetSnapshot &entry) {
                    return entry.binding == target.binding && entry.targetObject == target.targetObject;
                }) != targets.first(index).end())
                return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyMalformed,
                                                               "Property target snapshots contain a duplicate binding target.");
        }

        std::vector<CompiledTrack> compiled;
        compiled.reserve(tracks.size());
        for (std::size_t index = 0; index < tracks.size(); ++index) {
            const PropertyTrackDescriptor &track = tracks[index];
            if (track.version != CurrentPropertyTrackVersion)
                return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyVersionUnsupported,
                                                               "The property track requires an explicit compatible-version migration.");
            if (!track.track.IsValid() || !track.targetObject.IsValid() || !track.binding.IsValid())
                return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyMalformed,
                                                               "A property track identity or target is invalid.");
            if (auto curves = ValidateCurveSet(track.curves); curves.HasError())
                return Result<PropertyEvaluationPlan>::Failure(curves.ErrorValue());
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (tracks[prior].track == track.track)
                    return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyMalformed,
                                                                   "Property tracks contain a duplicate track identity.");
            }

            const Runtime::PropertyBindingDescriptor *binding = registry.Find(track.binding);
            if (binding == nullptr) {
                if (track.required)
                    return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyBindingMissing,
                                                                   "The required property binding is absent from the registry.");
                compiled.push_back({track, nullptr, std::nullopt, PropertyBindingEvaluationOutcome::BindingMissing});
                continue;
            }
            if (!IsFiniteRange(binding->range))
                return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyMalformed,
                                                               "The property binding range is not finite or ordered.");
            if (binding->type != track.curves.type) {
                if (track.required)
                    return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyTypeMismatch,
                                                                   "The property curve type does not match its binding descriptor.");
                compiled.push_back({track, binding, std::nullopt, PropertyBindingEvaluationOutcome::TypeMismatch});
                continue;
            }
            const PropertyBindingTargetSnapshot *target = FindTarget(targets, track.binding, track.targetObject);
            if (target == nullptr) {
                if (track.required)
                    return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyBindingTargetMissing,
                                                                   "The required property target is absent from the scene snapshot.");
                compiled.push_back({track, binding, std::nullopt, PropertyBindingEvaluationOutcome::TargetMissing});
                continue;
            }
            if (target->componentType != binding->componentType) {
                if (track.required)
                    return RejectProperty<PropertyEvaluationPlan>(CinematicErrors::PropertyComponentMismatch,
                                                                   "The property target component type does not match its binding.");
                compiled.push_back({track, binding, std::nullopt, PropertyBindingEvaluationOutcome::ComponentMismatch});
                continue;
            }
            compiled.push_back({track, binding, *target, PropertyBindingEvaluationOutcome::Applied});
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

        std::array<PropertyEvaluationValue, MaximumPropertyTracks> staged{};
        std::size_t stagedCount = 0;
        for (const CompiledTrack &compiled : orderedTracks_) {
            if (compiled.binding == nullptr || !compiled.activationTarget.has_value())
                continue;
            const PropertyBindingTargetSnapshot *target =
                FindTarget(context.targets, compiled.track.binding, compiled.track.targetObject);
            if (target == nullptr)
                return RejectProperty<std::size_t>(CinematicErrors::PropertyBindingTargetMissing,
                                                   "A required property target disappeared from the scene snapshot.");
            if (!SameTarget(*target, *compiled.activationTarget))
                return RejectProperty<std::size_t>(CinematicErrors::PropertyBindingStale,
                                                   "A property component or binding generation changed after activation.");

            auto sampled = SampleValue(compiled.track, time);
            if (sampled.HasError())
                return Result<std::size_t>::Failure(std::move(sampled).ErrorValue());
            if (!IsWithinRange(compiled.binding->range, sampled.Value()))
                return RejectProperty<std::size_t>(CinematicErrors::PropertySampleInvalid,
                                                   "A property sample violates its binding range constraint.");
            staged[stagedCount++] = {.track = compiled.track.track,
                                     .binding = compiled.track.binding,
                                     .targetObject = compiled.track.targetObject,
                                     .value = sampled.Value()};
        }
        for (std::size_t index = 0; index < stagedCount; ++index)
            output[index] = staged[index];
        return Result<std::size_t>::Success(stagedCount);
    }

    /** @copydoc PropertyEvaluationPlan::Apply */
    Result<PropertyEvaluationResult> PropertyEvaluationPlan::Apply(
        const PropertyEvaluationContext &context, const std::span<const PropertyEvaluationValue> values,
        const std::span<PropertyEvaluationDiagnostic> diagnostics) const {
        if (context.scene != scene_)
            return RejectProperty<PropertyEvaluationResult>(CinematicErrors::PropertyBindingStale,
                                                             "The property plan does not belong to the active scene binding generation.");
        if (values.size() > orderedTracks_.size() || diagnostics.size() < orderedTracks_.size())
            return RejectProperty<PropertyEvaluationResult>(CinematicErrors::PropertyLimitExceeded,
                                                             "Property application scratch storage is outside the admitted bound.");

        PropertyEvaluationResult result{};
        for (const CompiledTrack &compiled : orderedTracks_) {
            const PropertyEvaluationValue *value = FindValue(values, compiled.track);
            if (compiled.activationOutcome != PropertyBindingEvaluationOutcome::Applied) {
                diagnostics[result.diagnostics++] =
                    Diagnostic(compiled.track, compiled.activationOutcome, DiagnosticError(compiled.activationOutcome));
                continue;
            }
            if (value == nullptr)
                return RejectProperty<PropertyEvaluationResult>(CinematicErrors::PropertyLimitExceeded,
                                                                 "A resolved property track has no sampled value to apply.");
            ++result.sampled;

            const PropertyBindingTargetSnapshot *target =
                FindTarget(context.targets, compiled.track.binding, compiled.track.targetObject);
            if (target == nullptr) {
                diagnostics[result.diagnostics++] =
                    Diagnostic(compiled.track, PropertyBindingEvaluationOutcome::TargetMissing,
                               CinematicErrors::PropertyBindingTargetMissing);
                continue;
            }
            if (!compiled.activationTarget.has_value() || !SameTarget(*target, *compiled.activationTarget)) {
                diagnostics[result.diagnostics++] =
                    Diagnostic(compiled.track, PropertyBindingEvaluationOutcome::BindingStale, CinematicErrors::PropertyBindingStale);
                continue;
            }
            if (target->componentType != compiled.binding->componentType) {
                diagnostics[result.diagnostics++] =
                    Diagnostic(compiled.track, PropertyBindingEvaluationOutcome::ComponentMismatch,
                               CinematicErrors::PropertyComponentMismatch);
                continue;
            }
            if (compiled.binding->writePolicy == Runtime::PropertyWritePolicy::ReadOnly) {
                diagnostics[result.diagnostics++] =
                    Diagnostic(compiled.track, PropertyBindingEvaluationOutcome::ReadOnly,
                               CinematicErrors::PropertyWriteRejected);
                continue;
            }
            if (!MatchesType(compiled.binding->type, value->value)) {
                diagnostics[result.diagnostics++] =
                    Diagnostic(compiled.track, PropertyBindingEvaluationOutcome::TypeMismatch,
                               CinematicErrors::PropertyTypeMismatch);
                continue;
            }
            if (!IsWithinRange(compiled.binding->range, value->value)) {
                diagnostics[result.diagnostics++] =
                    Diagnostic(compiled.track, PropertyBindingEvaluationOutcome::ValueOutOfRange,
                               CinematicErrors::PropertySampleInvalid);
                continue;
            }

            auto written = compiled.binding->setter(target->component, value->value);
            if (written.HasError()) {
                PropertyEvaluationDiagnostic diagnostic =
                    Diagnostic(compiled.track, PropertyBindingEvaluationOutcome::WriteRejected,
                               CinematicErrors::PropertyWriteRejected);
                diagnostic.error = std::move(written).ErrorValue();
                diagnostics[result.diagnostics++] = std::move(diagnostic);
                continue;
            }
            ++result.applied;
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

    PropertyEvaluationPlan::PropertyEvaluationPlan(const PropertySceneVersion scene,
                                                   std::vector<CompiledTrack> orderedTracks) noexcept
        : scene_(scene), orderedTracks_(std::move(orderedTracks)) {}
}  // namespace Horo::Cinematic
