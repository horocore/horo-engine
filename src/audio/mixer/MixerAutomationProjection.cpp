#include "MixerPlanState.h"

#include <cmath>
#include <limits>

namespace Horo::Audio {
    namespace {
        /** @brief Resolve a compiled fader/send cell without changing its authored range contract. */
        bool ResolveGain(float &gain, const AudioAutomationParameter &binding, MixerDetail::ParameterProjection &projection) noexcept {
            if (binding.address.parameter.Value() != 1 || binding.minimum < 0 || binding.maximum > 16)
                return false;
            projection.value = &gain;
            return binding.initialValue == gain;
        }

        /** @brief Resolve one declared DSP parameter into its prepared mutable value and target cells. */
        bool ResolveDSPParameter(MixerDetail::Insert &insert, const AudioAutomationParameter &binding,
                                 MixerDetail::ParameterProjection &projection) noexcept {
            for (std::size_t parameter = 0; parameter < insert.descriptor.parameters.size(); ++parameter) {
                const auto &descriptor = insert.descriptor.parameters[parameter];
                if (descriptor.identity != binding.address.parameter)
                    continue;
                if (binding.minimum < descriptor.minimum || binding.maximum > descriptor.maximum ||
                    binding.initialValue != descriptor.defaultValue)
                    return false;
                projection.value = &insert.parameters[parameter].value;
                projection.target = &insert.parameters[parameter].target;
                return true;
            }
            return false;
        }

        /** @brief Find the exact compiled send belonging to the already resolved source bus. */
        bool ResolveSend(MixerRenderPlan::State &state, const std::size_t bus, const AudioAutomationParameter &binding,
                         MixerDetail::ParameterProjection &projection) noexcept {
            for (std::size_t route = 0; route < state.routes.size(); ++route) {
                if (state.routes[route].id == binding.address.send && state.routes[route].sourceBus == bus &&
                    state.routeState[route].kind == MixerRouteKind::Send)
                    return ResolveGain(state.routeState[route].gain, binding, projection);
            }
            return false;
        }

        /** @brief Locate a unique compiled insert on the resolved bus before examining its parameter descriptor. */
        bool ResolveDSP(MixerDetail::BusState &bus, const AudioAutomationParameter &binding,
                        MixerDetail::ParameterProjection &projection) noexcept {
            for (auto &insert : bus.inserts) {
                if (insert.effect.id == binding.address.effect)
                    return ResolveDSPParameter(insert, binding, projection);
            }
            return false;
        }

        /** @brief Resolve one physical cell from already compiled stable identities on control. */
        bool Resolve(MixerRenderPlan::State &state, const AudioAutomationParameter &binding,
                     MixerDetail::ParameterProjection &projection) noexcept {
            using enum AudioParameterTargetKind;
            for (std::size_t index = 0; index < state.buses.size(); ++index) {
                if (state.buses[index].id != binding.address.bus)
                    continue;
                switch (binding.address.kind) {
                    case Bus:
                        return ResolveGain(state.processing[index].gain, binding, projection);
                    case Send:
                        return ResolveSend(state, index, binding, projection);
                    case DSP:
                        return ResolveDSP(state.processing[index], binding, projection);
                    default:
                        return false;
                }
            }
            return false;
        }

        /** @brief Compare immutable admission bounds before a borrowed engine may drive physical cells. */
        bool SameBinding(const AudioAutomationParameter &expected, const AudioAutomationParameter &actual) noexcept {
            return expected.address == actual.address && expected.minimum == actual.minimum && expected.maximum == actual.maximum &&
                   expected.initialValue == actual.initialValue && expected.maximumSampleDelta == actual.maximumSampleDelta &&
                   expected.minimumSmoothingFrames == actual.minimumSmoothingFrames;
        }
    }  // namespace

    /** @copydoc MixerRenderPlan::BindAutomation */
    Result<void> MixerRenderPlan::BindAutomation(const std::span<const AudioAutomationParameter> bindings) {
        auto failure = [] {
            return Result<void>::Failure(MakeError(AudioErrors::GraphBuildFailed, "Invalid mixer automation binding or budget."));
        };
        auto &state = *state_;
        if (state.automationBound || state.handle.slot != 0 || bindings.empty() || bindings.size() > MaximumAudioAutomationParameters)
            return failure();
        const AudioSampleClock clock{.owner = state.identity.owner,
                                     .epoch = state.identity.epoch,
                                     .generation = 1,
                                     .discontinuityRevision = 1,
                                     .sampleRate = state.profile.sampleRate};
        AudioParameterAutomation admission{clock, {state.identity.owner, 1, 1}};
        try {
            std::vector<MixerDetail::ParameterProjection> candidate;
            candidate.reserve(bindings.size());
            for (const auto &binding : bindings) {
                if (binding.address.owner != state.identity.owner || binding.address.bindingGeneration != state.identity.generation ||
                    admission.Bind(binding) != AudioAutomationStatus::Ok)
                    return failure();
                MixerDetail::ParameterProjection projection{.binding = binding};
                if (!Resolve(state, binding, projection))
                    return failure();
                for (const auto &previous : candidate)
                    if (previous.value == projection.value)
                        return failure();
                candidate.push_back(projection);
            }
            const std::size_t bytes = candidate.capacity() * sizeof(MixerDetail::ParameterProjection);
            if (bytes > state.profile.maximumStorageBytes - StorageBytes())
                return failure();
            // Aligned taps retain prepared capacity: conservatively repeat the complete ordinary block reservation per sample.
            // Additional work covers engine event/sample bounds, constant-time binding reads and DAG/voice dispatch.
            const std::uint64_t perSample = MaximumAudioAutomationRequests * MaximumAudioAutomationRequests +
                                            MaximumAudioAutomationParameters * MaximumAudioAutomationRequests + bindings.size() +
                                            state.buses.size() * state.profile.maximumVoices +
                                            (state.buses.size() + state.routes.size()) * 8;
            const std::uint64_t work = (perSample + state.sampleOperations) * state.profile.maximumFrames;
            if (work > state.profile.maximumSampleOperations || state.sampleOperations > state.profile.maximumSampleOperations - work)
                return failure();
            state.metadataBytes += bytes;
            state.sampleOperations += work;
            state.projections = std::move(candidate);
            state.automationBound = true;
            return Result<void>::Success();
        } catch (const std::bad_alloc &) {
            return failure();
        }
    }

    namespace MixerDetail {
        /** @brief Admit complete clock/scene and sealed physical bindings before any automation advancement or cell mutation. */
        bool ValidateAutomationContext(const MixerRenderPlan::State &state, const MixerAutomationRenderContext &context,
                                       const std::uint32_t frames, const std::span<AudioAutomationValueSelector> selectors) noexcept {
            const auto &clock = context.clock;
            if (const auto &current = context.automation.CurrentClock();
                selectors.size() != state.projections.size() || !state.automationBound || clock.owner != state.identity.owner ||
                clock.epoch != state.identity.epoch || clock.sampleRate != state.profile.sampleRate ||
                clock.state != AudioSampleClockState::Running || clock.generation == 0 || clock.discontinuityRevision == 0 ||
                clock.sampleFrame > std::numeric_limits<std::uint64_t>::max() - (frames - 1U) || current.owner != clock.owner ||
                current.epoch != clock.epoch || current.generation != clock.generation ||
                current.discontinuityRevision != clock.discontinuityRevision || current.sampleRate != clock.sampleRate ||
                current.state != AudioSampleClockState::Running || current.sampleFrame > clock.sampleFrame ||
                (context.snapshot && (!context.transitions || !context.transitions->UsesAutomation(context.automation))))
                return false;
            for (std::size_t index = 0; index < state.projections.size(); ++index) {
                const auto &projection = state.projections[index];
                AudioAutomationParameter binding;
                if (!context.automation.Binding(projection.binding.address, binding) || !SameBinding(projection.binding, binding) ||
                    !context.automation.ResolveValue(projection.binding.address, selectors[index]))
                    return false;
            }
            return true;
        }

        /** @brief Project one coherent sample into all pre-resolved physical cells; no stable-ID discovery occurs. */
        bool ProjectAutomation(MixerRenderPlan::State &state, const AudioParameterAutomation &automation,
                               const std::span<const AudioAutomationValueSelector> selectors) noexcept {
            std::array<float, MaximumAudioAutomationParameters> values{};
            for (std::size_t index = 0; index < state.projections.size(); ++index) {
                const auto &projection = state.projections[index];
                if (automation.Value(selectors[index], values[index]) != AudioAutomationStatus::Ok || !std::isfinite(values[index]) ||
                    values[index] < projection.binding.minimum || values[index] > projection.binding.maximum)
                    return false;
            }
            for (std::size_t index = 0; index < state.projections.size(); ++index) {
                auto &projection = state.projections[index];
                *projection.value = values[index];
                if (projection.target)
                    *projection.target = values[index];
            }
            return true;
        }
    }  // namespace MixerDetail
}  // namespace Horo::Audio
