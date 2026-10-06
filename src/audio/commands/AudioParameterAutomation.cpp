#include "Horo/Audio/AudioParameterAutomation.h"

#include "Horo/Audio/AudioCommands.h"
#include "Horo/Audio/ScheduledAudioCommandBatch.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Audio {
    namespace {
        /** @brief Validate the authority tuple independently of its cursor observation. */
        bool ValidClock(const AudioSampleClock &clock) noexcept {
            return clock.owner.IsValid() && clock.epoch != 0 && clock.generation != 0 && clock.discontinuityRevision != 0 &&
                   clock.sampleRate >= MinimumAudioSampleRate && clock.sampleRate <= MaximumAudioSampleRate &&
                   (clock.state == AudioSampleClockState::Running || clock.state == AudioSampleClockState::Paused);
        }

        /** @brief Compare every timeline authority field, including negotiated sample rate. */
        bool SameClock(const AudioSampleClock &left, const AudioSampleClock &right) noexcept {
            return left.owner == right.owner && left.epoch == right.epoch && left.generation == right.generation &&
                   left.discontinuityRevision == right.discontinuityRevision && left.sampleRate == right.sampleRate;
        }

        /** @brief Voice addresses cannot carry mixer identities or a foreign handle owner. */
        bool ValidVoiceAddress(const AudioParameterAddress &address) noexcept {
            return address.voice.IsValid() && address.voice.owner == address.owner && !address.bus.IsValid() && !address.send.IsValid() &&
                   !address.effect.IsValid();
        }

        /** @brief Mixer targets each admit precisely their owning stable identity fields. */
        bool ValidMixerAddress(const AudioParameterAddress &address) noexcept {
            using enum AudioParameterTargetKind;
            if (address.voice != AudioVoiceHandle{} || !address.bus.IsValid())
                return false;
            switch (address.kind) {
                case Bus:
                    return !address.send.IsValid() && !address.effect.IsValid();
                case Send:
                    return address.send.IsValid() && !address.effect.IsValid();
                case DSP:
                    return address.effect.IsValid() && !address.send.IsValid();
                default:
                    return false;
            }
        }

        /** @brief Range validation and worst-case slope admission happen before activation. */
        bool ValidParameter(const AudioAutomationParameter &parameter) noexcept {
            return IsValidAudioParameterAddress(parameter.address) && std::isfinite(parameter.minimum) &&
                   std::isfinite(parameter.maximum) && std::isfinite(parameter.initialValue) &&
                   std::isfinite(parameter.maximumSampleDelta) && parameter.minimum <= parameter.maximum &&
                   parameter.initialValue >= parameter.minimum && parameter.initialValue <= parameter.maximum &&
                   parameter.maximumSampleDelta > 0.0F && parameter.minimumSmoothingFrames != 0;
        }

        /** @brief Bound continuous derivative over the entire admitted range, including unknown future overlap anchors. */
        double RequiredFrames(const AudioAutomationParameter &parameter, const AudioAutomationCurve curve) noexcept {
            const double slope = curve == AudioAutomationCurve::Smoothstep ? 1.5 : 1.0;
            const double range = static_cast<double>(parameter.maximum) - parameter.minimum;
            return std::max(static_cast<double>(parameter.minimumSmoothingFrames), std::ceil(slope * range / parameter.maximumSampleDelta));
        }

        /** @brief Reject stale generation, reused admission identity and past sample time before queue mutation. */
        AudioAutomationStatus ValidateTiming(const AudioParameterAutomationRequest &request, const AudioSampleClock &clock,
                                             const std::uint64_t lastRequestId) noexcept {
            using enum AudioAutomationStatus;
            if (request.clockGeneration != clock.generation || request.discontinuityRevision != clock.discontinuityRevision)
                return StaleClock;
            if (request.requestId <= lastRequestId)
                return Duplicate;
            if (request.startFrame < clock.sampleFrame)
                return Late;
            return Ok;
        }

        /** @brief Resolve mandatory smoothing and prove finite endpoint time before accepting a trajectory. */
        AudioAutomationStatus PrepareDuration(const AudioAutomationParameter &descriptor, const AudioParameterAutomationRequest &request,
                                              std::uint32_t &duration) noexcept {
            using enum AudioAutomationStatus;
            if (request.targetValue < descriptor.minimum || request.targetValue > descriptor.maximum)
                return OutOfRange;
            const auto required = static_cast<std::uint32_t>(RequiredFrames(descriptor, request.curve));
            const auto candidate = request.curve == AudioAutomationCurve::Immediate ? required : request.durationFrames;
            if (candidate < required)
                return ContinuityLimit;
            if (request.startFrame > std::numeric_limits<std::uint64_t>::max() - candidate)
                return InvalidInput;
            duration = candidate;
            return Ok;
        }

        /** @brief Structural command validation precedes exact epoch admission, without target resolution. */
        AudioAutomationStatus ValidateCommand(const AudioCommand &command, const AudioSampleClock &clock) noexcept {
            if (AudioCommand normalized; NormalizeAudioCommand(command, normalized) != AudioCommandStatus::Ok)
                return AudioAutomationStatus::InvalidInput;
            if (command.scope.owner != clock.owner || command.scope.epoch != clock.epoch)
                return AudioAutomationStatus::StaleClock;
            return AudioAutomationStatus::Ok;
        }
    }  // namespace

    /** @copydoc IsValidAudioParameterAddress */
    bool IsValidAudioParameterAddress(const AudioParameterAddress &address) noexcept {
        if (!address.owner.IsValid() || !address.parameter.IsValid() || address.bindingGeneration == 0) {
            return false;
        }
        return address.kind == AudioParameterTargetKind::Voice ? ValidVoiceAddress(address) : ValidMixerAddress(address);
    }

    /** @copydoc IsValidAudioAutomationRequest */
    bool IsValidAudioAutomationRequest(const AudioParameterAutomationRequest &request) noexcept {
        if (!IsValidAudioParameterAddress(request.address) || request.requestId == 0 || request.clockGeneration == 0 ||
            request.discontinuityRevision == 0 || !std::isfinite(request.targetValue)) {
            return false;
        }
        using enum AudioAutomationCurve;
        return (request.curve == Immediate && request.durationFrames == 0) ||
               ((request.curve == Linear || request.curve == Smoothstep) && request.durationFrames != 0);
    }

    /** @copydoc AudioParameterAutomation::AudioParameterAutomation */
    AudioParameterAutomation::AudioParameterAutomation(const AudioSampleClock &clock, const AudioSceneContextHandle scene) noexcept
        : clock_(clock), scene_(scene) {
        closed_ = !ValidClock(clock) || clock.state != AudioSampleClockState::Running || !scene.IsValid() || scene.owner != clock.owner;
    }

    /** @copydoc AudioParameterAutomation::Bind */
    AudioAutomationStatus AudioParameterAutomation::Bind(const AudioAutomationParameter &parameter) noexcept {
        using enum AudioAutomationStatus;
        if (closed_ || sealed_)
            return Closed;
        if (!ValidParameter(parameter) || parameter.address.owner != clock_.owner ||
            RequiredFrames(parameter, AudioAutomationCurve::Smoothstep) > std::numeric_limits<std::uint32_t>::max())
            return InvalidInput;
        if (Find(parameter.address) != parameterCount_)
            return Duplicate;
        if (parameterCount_ == parameters_.size())
            return Capacity;
        parameters_[parameterCount_++] = {.descriptor = parameter,
                                          .value = parameter.initialValue,
                                          .from = parameter.initialValue,
                                          .target = parameter.initialValue};
        return Ok;
    }

    /** @copydoc AudioParameterAutomation::Seal */
    AudioAutomationStatus AudioParameterAutomation::Seal() noexcept {
        if (closed_)
            return AudioAutomationStatus::Closed;
        sealed_ = true;
        return AudioAutomationStatus::Ok;
    }

    /** @copydoc AudioParameterAutomation::Find */
    std::size_t AudioParameterAutomation::Find(const AudioParameterAddress &address) const noexcept {
        for (std::size_t index = 0; index < parameterCount_; ++index) {
            if (parameters_[index].descriptor.address == address)
                return index;
        }
        return parameterCount_;
    }

    /** @copydoc AudioParameterAutomation::Schedule */
    AudioAutomationStatus AudioParameterAutomation::Schedule(const AudioParameterAutomationRequest &request) noexcept {
        using enum AudioAutomationStatus;
        if (closed_)
            return Closed;
        if (!sealed_ || !IsValidAudioAutomationRequest(request))
            return InvalidInput;
        if (const auto status = ValidateTiming(request, clock_, lastRequestId_); status != Ok)
            return status;
        const auto index = Find(request.address);
        if (index == parameterCount_)
            return MissingParameter;
        std::uint32_t duration{};
        if (const auto status = PrepareDuration(parameters_[index].descriptor, request, duration); status != Ok)
            return status;
        if (pendingCount_ == pending_.size())
            return Capacity;
        std::size_t position = pendingCount_;
        while (position > 0 && pending_[position - 1].request.startFrame > request.startFrame) {
            pending_[position] = pending_[position - 1];
            --position;
        }
        pending_[position] = {.request = request, .parameterIndex = index, .duration = duration};
        ++pendingCount_;
        lastRequestId_ = request.requestId;
        return Ok;
    }

    /** @copydoc AudioParameterAutomation::Sample */
    void AudioParameterAutomation::Sample(const std::uint64_t frame) noexcept {
        for (std::size_t index = 0; index < parameterCount_; ++index) {
            auto &parameter = parameters_[index];
            if (parameter.activeId == 0)
                continue;
            const auto elapsed = frame - parameter.start;
            if (elapsed >= parameter.duration) {
                parameter.value = parameter.target;
                parameter.activeId = 0;
                continue;
            }
            double fraction = static_cast<double>(elapsed) / parameter.duration;
            if (parameter.curve == AudioAutomationCurve::Smoothstep)
                fraction = fraction * fraction * (3.0 - 2.0 * fraction);
            parameter.value =
                static_cast<float>(std::lerp(static_cast<double>(parameter.from), static_cast<double>(parameter.target), fraction));
        }
    }

    /** @copydoc AudioParameterAutomation::Advance */
    AudioAutomationStatus AudioParameterAutomation::Advance(const AudioSampleClock &clock) noexcept {
        using enum AudioAutomationStatus;
        if (closed_)
            return Closed;
        if (!sealed_ || !ValidClock(clock))
            return InvalidInput;
        if (!SameClock(clock, clock_))
            return StaleClock;
        if (clock.state == AudioSampleClockState::Paused)
            return Paused;
        if (clock.sampleFrame < clock_.sampleFrame)
            return Late;
        std::size_t consumed = 0;
        while (consumed < pendingCount_ && pending_[consumed].request.startFrame <= clock.sampleFrame) {
            const auto &pending = pending_[consumed++];
            Sample(pending.request.startFrame);
            auto &parameter = parameters_[pending.parameterIndex];
            parameter.from = parameter.value;
            parameter.target = pending.request.targetValue;
            parameter.start = pending.request.startFrame;
            parameter.duration = pending.duration;
            parameter.curve = pending.request.curve;
            parameter.activeId = pending.request.requestId;
        }
        if (consumed != 0) {
            std::move(pending_.begin() + static_cast<std::ptrdiff_t>(consumed),
                      pending_.begin() + static_cast<std::ptrdiff_t>(pendingCount_), pending_.begin());
            pendingCount_ -= consumed;
        }
        Sample(clock.sampleFrame);
        clock_ = clock;
        return Ok;
    }

    /** @copydoc AudioParameterAutomation::Apply */
    AudioAutomationStatus AudioParameterAutomation::Apply(const AudioCommand &command) noexcept {
        using enum AudioAutomationStatus;
        if (closed_)
            return Closed;
        if (const auto status = ValidateCommand(command, clock_); status != Ok)
            return status;
        if (std::holds_alternative<AudioResetCommand>(command.payload)) {
            Close();
            return Ok;
        }
        if (command.scope.scene != scene_)
            return StaleClock;
        if (std::holds_alternative<AudioSceneUnloadCommand>(command.payload)) {
            Close();
            return Ok;
        }
        if (const auto *automation = std::get_if<AudioAutomateParameterCommand>(&command.payload))
            return Schedule(automation->request);
        if (const auto *cancel = std::get_if<AudioCancelAutomationCommand>(&command.payload)) {
            if (cancel->clockGeneration != clock_.generation || cancel->discontinuityRevision != clock_.discontinuityRevision)
                return StaleClock;
            return Cancel(cancel->requestId);
        }
        return InvalidInput;
    }

    /** @copydoc AudioParameterAutomation::ApplyBatch */
    AudioAutomationStatus AudioParameterAutomation::ApplyBatch(const ScheduledAudioCommandBatch &batch) noexcept {
        using enum AudioAutomationStatus;
        if (closed_)
            return Closed;
        ScheduledAudioCommandBatch normalized;
        if (NormalizeScheduledAudioCommandBatch(batch, normalized) != ScheduledAudioCommandBatchStatus::Ok)
            return InvalidInput;
        if (batch.target.kind == AudioCommandTargetKind::ExactSampleFrame &&
            (batch.target.clockGeneration != clock_.generation || batch.target.discontinuityRevision != clock_.discontinuityRevision ||
             batch.target.sampleFrame != clock_.sampleFrame))
            return StaleClock;
        auto candidate = *this;
        for (std::uint32_t index = 0; index < normalized.commandCount; ++index) {
            if (const auto status = candidate.Apply(normalized.commands[index]); status != Ok)
                return status;
        }
        *this = candidate;
        return Ok;
    }

    /** @copydoc AudioParameterAutomation::Cancel */
    AudioAutomationStatus AudioParameterAutomation::Cancel(const std::uint64_t requestId) noexcept {
        using enum AudioAutomationStatus;
        if (closed_)
            return Closed;
        for (std::size_t index = 0; index < pendingCount_; ++index) {
            if (pending_[index].request.requestId != requestId)
                continue;
            std::move(pending_.begin() + static_cast<std::ptrdiff_t>(index + 1),
                      pending_.begin() + static_cast<std::ptrdiff_t>(pendingCount_), pending_.begin() + static_cast<std::ptrdiff_t>(index));
            --pendingCount_;
            return Ok;
        }
        for (std::size_t index = 0; index < parameterCount_; ++index) {
            auto &parameter = parameters_[index];
            if (parameter.activeId != requestId || requestId == 0)
                continue;
            parameter.activeId = 0;
            parameter.target = parameter.value;
            return Ok;
        }
        return NotFound;
    }

    /** @copydoc AudioParameterAutomation::Value */
    AudioAutomationStatus AudioParameterAutomation::Value(const AudioParameterAddress &address, float &value) const noexcept {
        using enum AudioAutomationStatus;
        if (closed_)
            return Closed;
        const auto index = Find(address);
        if (index == parameterCount_)
            return MissingParameter;
        value = parameters_[index].value;
        return Ok;
    }

    /** @copydoc AudioParameterAutomation::ResolveValue */
    bool AudioParameterAutomation::ResolveValue(const AudioParameterAddress &address,
                                                AudioAutomationValueSelector &selector) const noexcept {
        if (closed_ || !sealed_)
            return false;
        const auto index = Find(address);
        if (index == parameterCount_)
            return false;
        selector.owner_ = this;
        selector.index_ = index;
        return true;
    }

    /** @copydoc AudioParameterAutomation::Value */
    AudioAutomationStatus AudioParameterAutomation::Value(const AudioAutomationValueSelector &selector, float &value) const noexcept {
        using enum AudioAutomationStatus;
        if (closed_)
            return Closed;
        if (!sealed_ || selector.owner_ != this || selector.index_ >= parameterCount_)
            return MissingParameter;
        value = parameters_[selector.index_].value;
        return Ok;
    }

    /** @copydoc AudioParameterAutomation::HasRequest */
    bool AudioParameterAutomation::HasRequest(const std::uint64_t requestId) const noexcept {
        if (closed_ || requestId == 0)
            return false;
        for (std::size_t index = 0; index < pendingCount_; ++index)
            if (pending_[index].request.requestId == requestId)
                return true;
        for (std::size_t index = 0; index < parameterCount_; ++index)
            if (parameters_[index].activeId == requestId)
                return true;
        return false;
    }

    /** @copydoc AudioParameterAutomation::Binding */
    bool AudioParameterAutomation::Binding(const AudioParameterAddress &address, AudioAutomationParameter &binding) const noexcept {
        if (closed_ || !sealed_)
            return false;
        const auto index = Find(address);
        if (index == parameterCount_)
            return false;
        binding = parameters_[index].descriptor;
        return true;
    }

    /** @copydoc AudioParameterAutomation::Close */
    void AudioParameterAutomation::Close() noexcept {
        closed_ = true;
        pendingCount_ = 0;
        parameterCount_ = 0;
    }
}  // namespace Horo::Audio
