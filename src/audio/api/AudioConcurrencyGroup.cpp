#include "Horo/Audio/AudioConcurrencyGroup.h"

#include "Horo/Audio/AudioErrors.h"

namespace Horo::Audio {
    namespace {
        /** @brief Reject unknown scope values and over-budget authored ceilings. */
        [[nodiscard]] bool ValidGroup(const AudioConcurrencyGroup &group) noexcept {
            using enum AudioConcurrencyScope;
            return group.group.IsValid() && group.maximumInstances <= MaximumAudioVoiceSlots &&
                   (group.scope == Global || group.scope == Emitter || group.scope == Owner);
        }

        /** @brief Map known voice states to the authored counting rule. */
        [[nodiscard]] std::optional<bool> Counts(const AudioVoiceState state, const AudioConcurrencyGroup &group) noexcept {
            using enum AudioVoiceState;
            if (state == Paused)
                return group.countPaused;
            if (state == Virtual)
                return group.countVirtual;
            if (IsTerminalAudioVoiceState(state))
                return false;
            switch (state) {
                case Created:
                case Ready:
                case Scheduled:
                case Playing:
                case Stopping:
                    return true;
                default:
                    return std::nullopt;
            }
        }

        /** @brief Check handle bounds, lifecycle evidence and canonical ordering before counting. */
        [[nodiscard]] bool ValidRecord(const AudioVoiceSnapshot &voice, const AudioVoiceHandle previous) noexcept {
            return voice.voice.IsValid() && voice.voice.slot <= MaximumAudioVoiceSlots &&
                   voice.terminalReason == AudioVoiceTerminalReasonForState(voice.state) &&
                   (!previous.IsValid() || (voice.voice > previous && voice.voice.slot != previous.slot));
        }

        /** @brief Reject stale clock history before applying sample-frame arithmetic. */
        [[nodiscard]] Result<void> ValidateTimeline(const AudioConcurrencySnapshot &snapshot, const AudioConcurrencyTime time) {
            if (snapshot.timelineGeneration == 0 || time.timelineGeneration == 0)
                return Result<void>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
            if (snapshot.timelineGeneration != time.timelineGeneration ||
                (snapshot.lastAdmissionFrame.has_value() && *snapshot.lastAdmissionFrame > time.sampleFrame))
                return Result<void>::Failure(MakeError(AudioErrors::ConcurrencyTimelineStale));
            return Result<void>::Success();
        }

        /** @brief Validate bounded, strictly ordered registry records and count eligible instances. */
        [[nodiscard]] Result<std::uint32_t> CountInstances(const AudioConcurrencySnapshot &snapshot, const AudioConcurrencyGroup &group) {
            if (snapshot.voices.size() > MaximumAudioVoiceSlots)
                return Result<std::uint32_t>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
            AudioVoiceHandle previous;
            std::uint32_t count{};
            for (const auto &voice : snapshot.voices) {
                const auto counts = Counts(voice.state, group);
                if (!counts.has_value() || !ValidRecord(voice, previous))
                    return Result<std::uint32_t>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
                if (voice.voice.owner != snapshot.key.runtime)
                    return Result<std::uint32_t>::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
                previous = voice.voice;
                if (*counts)
                    ++count;
            }
            return Result<std::uint32_t>::Success(count);
        }
    }  // namespace

    /** @copydoc MakeAudioConcurrencyKey */
    Result<AudioConcurrencyKey> MakeAudioConcurrencyKey(const AudioConcurrencyGroup &group, const AudioConcurrencyRequest &request) {
        if (!ValidGroup(group) || !request.runtime.IsValid())
            return Result<AudioConcurrencyKey>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
        AudioConcurrencyKey key{.group = group.group, .runtime = request.runtime, .scope = group.scope};
        if (group.scope == AudioConcurrencyScope::Emitter) {
            if (!request.emitter.IsValid())
                return Result<AudioConcurrencyKey>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
            if (request.emitter.owner != request.runtime)
                return Result<AudioConcurrencyKey>::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
            key.emitter = request.emitter;
        }
        if (group.scope == AudioConcurrencyScope::Owner) {
            if (!request.owner.IsValid())
                return Result<AudioConcurrencyKey>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
            if (request.owner.owner != request.runtime)
                return Result<AudioConcurrencyKey>::Failure(MakeError(AudioErrors::HandleOwnerMismatch));
            key.owner = request.owner;
        }
        return Result<AudioConcurrencyKey>::Success(key);
    }

    /** @copydoc EvaluateAudioConcurrency */
    Result<AudioConcurrencyDecision> EvaluateAudioConcurrency(const AudioConcurrencyGroup &group, const AudioConcurrencyRequest &request,
                                                              const AudioConcurrencySnapshot &snapshot, const AudioConcurrencyTime time) {
        const auto key = MakeAudioConcurrencyKey(group, request);
        if (key.HasError())
            return Result<AudioConcurrencyDecision>::Failure(key.ErrorValue());
        if (snapshot.key != key.Value())
            return Result<AudioConcurrencyDecision>::Failure(MakeError(AudioErrors::ConcurrencyInvalid));
        if (const auto timeline = ValidateTimeline(snapshot, time); timeline.HasError())
            return Result<AudioConcurrencyDecision>::Failure(timeline.ErrorValue());
        const auto count = CountInstances(snapshot, group);
        if (count.HasError())
            return Result<AudioConcurrencyDecision>::Failure(count.ErrorValue());

        AudioConcurrencyDecision decision{.countedInstances = count.Value()};
        if (snapshot.lastAdmissionFrame.has_value()) {
            const auto elapsed = time.sampleFrame - *snapshot.lastAdmissionFrame;
            if (elapsed < group.retriggerFrames)
                decision.remainingRetriggerFrames = group.retriggerFrames - elapsed;
        }
        if (group.maximumInstances != 0 && decision.countedInstances >= group.maximumInstances)
            decision.eligibility = AudioConcurrencyEligibility::InstanceLimit;
        else if (decision.remainingRetriggerFrames != 0)
            decision.eligibility = AudioConcurrencyEligibility::RetriggerWindow;
        return Result<AudioConcurrencyDecision>::Success(decision);
    }
}  // namespace Horo::Audio
