#include "AudioRepeatedPlaybackState.h"

#include <new>
#include <utility>

namespace Horo::Audio {
    /** @copydoc AudioRepeatedPlayback::Create */
    Result<AudioRepeatedPlayback> AudioRepeatedPlayback::Create(const AudioRepeatedPlaybackConfig &config) {
        if (!State::ValidConfig(config))
            return Result<AudioRepeatedPlayback>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        if (auto plan = AudioResamplerPlan::Prepare(config.conversion, config.conversionBudget); plan.HasError())
            return Result<AudioRepeatedPlayback>::Failure(plan.ErrorValue());
        auto registry = AudioVoiceStateMachine::Create({config.runtime, config.maximumVoices});
        if (registry.HasError())
            return Result<AudioRepeatedPlayback>::Failure(registry.ErrorValue());
        try {
            return Result<AudioRepeatedPlayback>::Success(
                AudioRepeatedPlayback{std::make_unique<State>(config, std::move(registry).Value())});
        } catch (const std::bad_alloc &) {
            return Result<AudioRepeatedPlayback>::Failure(MakeError(AudioErrors::MemoryAllocationFailed));
        }
    }

    /** @copydoc AudioRepeatedPlayback::AudioRepeatedPlayback */
    AudioRepeatedPlayback::AudioRepeatedPlayback(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc AudioRepeatedPlayback::~AudioRepeatedPlayback */
    AudioRepeatedPlayback::~AudioRepeatedPlayback() = default;
    /** @copydoc AudioRepeatedPlayback::AudioRepeatedPlayback */
    AudioRepeatedPlayback::AudioRepeatedPlayback(AudioRepeatedPlayback &&other) noexcept = default;
    /** @copydoc AudioRepeatedPlayback::operator= */
    AudioRepeatedPlayback &AudioRepeatedPlayback::operator=(AudioRepeatedPlayback &&other) noexcept = default;

    /** @copydoc AudioRepeatedPlayback::Bind */
    Result<AudioPlaybackLaneHandle> AudioRepeatedPlayback::Bind(const AudioPlaybackBinding &binding,
                                                                const AudioRepeatedPlaybackPolicy &policy) {
        if (!state_)
            return Result<AudioPlaybackLaneHandle>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (const auto valid = state_->ValidateBinding(binding, policy); valid.HasError())
            return Result<AudioPlaybackLaneHandle>::Failure(valid.ErrorValue());
        const auto bucket = state_->SelectBucket(binding, policy);
        if (bucket.HasError())
            return Result<AudioPlaybackLaneHandle>::Failure(bucket.ErrorValue());
        if (state_->DuplicateBinding(binding))
            return Result<AudioPlaybackLaneHandle>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        return state_->StoreLane(binding, policy, bucket.Value());
    }

    /** @copydoc AudioRepeatedPlayback::Submit */
    Result<AudioRepeatedPlaybackReceipt> AudioRepeatedPlayback::Submit(const AudioRepeatedPlaybackRequest &request,
                                                                       const std::span<const AudioResolvedPlaybackClip> clips,
                                                                       const AudioConcurrencyTime time) {
        if (!state_)
            return Result<AudioRepeatedPlaybackReceipt>::Failure(MakeError(AudioErrors::RuntimeInactive));
        auto *lane = state_->Resolve(request.lane);
        if (!lane)
            return Result<AudioRepeatedPlaybackReceipt>::Failure(MakeError(AudioErrors::HandleStale));
        if (!state_->ValidControlTime(time))
            return Result<AudioRepeatedPlaybackReceipt>::Failure(MakeError(AudioErrors::ConcurrencyTimelineStale));
        if (auto replay = state_->Replay(*lane, request)) {
            if (replay->HasValue())
                state_->lastControlFrame = time.sampleFrame;
            return *replay;
        }
        if (const auto valid = state_->ValidateRequest(*lane, request, time); valid.HasError())
            return Result<AudioRepeatedPlaybackReceipt>::Failure(valid.ErrorValue());
        return state_->Admit(*lane, request, clips, time);
    }

    /** @copydoc AudioRepeatedPlayback::Apply */
    const ErrorCodeDescriptor *AudioRepeatedPlayback::Apply(const AudioCommand &command, const AudioConcurrencyTime time) noexcept {
        if (!state_)
            return &AudioErrors::RuntimeInactive;
        const auto *control = std::get_if<AudioVoiceControlRequest>(&command.payload);
        if (!control || !ValidateAudioVoiceControlRequest(*control))
            return &AudioErrors::PlaybackRequestInvalid;
        auto *voice = state_->Find(control->voice);
        if (!voice)
            return &AudioErrors::HandleStale;
        return control->operationSequence == 0 && control->control != AudioVoiceControl::Start &&
                       control->control != AudioVoiceControl::Restart
                   ? state_->ApplyDirect(*voice, command, *control, time)
                   : state_->ApplyPending(*voice, command, *control, time);
    }

    /** @copydoc AudioRepeatedPlayback::Render */
    AudioVoiceRenderResult AudioRepeatedPlayback::Render(const AudioVoiceHandle voice, const AudioResamplerOutput output) noexcept {
        if (!state_)
            return {.error = &AudioErrors::RuntimeInactive};
        auto *entry = state_->Find(voice);
        return entry ? entry->playback->Render(output) : AudioVoiceRenderResult{.error = &AudioErrors::HandleStale};
    }

    /** @copydoc AudioRepeatedPlayback::Snapshot */
    Result<AudioVoiceSnapshot> AudioRepeatedPlayback::Snapshot(const AudioVoiceHandle voice) const {
        if (!state_)
            return Result<AudioVoiceSnapshot>::Failure(MakeError(AudioErrors::RuntimeInactive));
        return state_->registry.Snapshot(voice);
    }

    /** @copydoc AudioRepeatedPlayback::Cancel */
    Result<void> AudioRepeatedPlayback::Cancel(const AudioVoiceHandle voice) {
        if (!state_)
            return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
        auto result = state_->registry.Cancel(voice);
        if (result.HasValue()) {
            if (auto *entry = state_->Find(voice))
                entry->pending = false;
        }
        return result;
    }

    /** @copydoc AudioRepeatedPlayback::Release */
    Result<void> AudioRepeatedPlayback::Release(const AudioVoiceHandle voice) {
        if (!state_)
            return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
        auto snapshot = state_->registry.Snapshot(voice);
        if (snapshot.HasError())
            return Result<void>::Failure(snapshot.ErrorValue());
        if (!IsTerminalAudioVoiceState(snapshot.Value().state))
            return Result<void>::Failure(MakeError(AudioErrors::VoiceInvalidTransition));
        auto *entry = state_->Find(voice);
        if (!entry)
            return Result<void>::Failure(MakeError(AudioErrors::HandleStale));
        *entry = {};
        return Result<void>::Success();
    }

    /** @copydoc AudioRepeatedPlayback::RetireScene */
    Result<void> AudioRepeatedPlayback::RetireScene(const AudioSceneContextHandle scene) {
        if (!state_ || !scene.IsValid() || scene.owner != state_->config.runtime)
            return Result<void>::Failure(MakeError(AudioErrors::PlaybackRequestInvalid));
        for (auto &voice : state_->voices) {
            if (!voice.playback || voice.scene != scene)
                continue;
            state_->CancelVoice(voice);
        }
        for (auto &lane : state_->lanes) {
            if (lane && lane->binding.prototype.sceneContext == scene)
                lane->closed = true;
        }
        return Result<void>::Success();
    }

    /** @copydoc AudioRepeatedPlayback::Reset */
    Result<void> AudioRepeatedPlayback::Reset(const std::uint64_t epoch, const std::uint64_t clockGeneration,
                                              const std::uint64_t discontinuityRevision) {
        if (!state_ || epoch <= state_->config.epoch || clockGeneration == 0 ||
            discontinuityRevision <= state_->config.discontinuityRevision)
            return Result<void>::Failure(MakeError(AudioErrors::ConcurrencyTimelineStale));
        for (auto &voice : state_->voices) {
            if (!voice.playback)
                continue;
            state_->CancelVoice(voice);
            voice.bucket.reset();
        }
        for (auto &lane : state_->lanes)
            lane.reset();
        state_->buckets.clear();
        state_->config.epoch = epoch;
        state_->config.clockGeneration = clockGeneration;
        state_->config.discontinuityRevision = discontinuityRevision;
        state_->lastControlFrame = 0;
        return Result<void>::Success();
    }
}  // namespace Horo::Audio
