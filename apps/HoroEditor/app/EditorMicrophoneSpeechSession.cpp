#include "EditorMicrophoneSpeechSession.h"

#include "EditorMicrophonePermission.h"
#include "Horo/Security/SecureMemory.h"
#include "MacOnDeviceSpeechProvider.h"

#include <new>

namespace Horo::Editor {
    namespace {
        using enum SpeechTranscriptionStatus;
        constexpr unsigned BlocksPerUpdate = 4;

        /** @brief Failures revoke the lease. A finished result retains authority until the caller adopts it. */
        bool IsFailure(const SpeechTranscriptionStatus status) noexcept {
            return status != Accepted && status != Pending && status != Finished;
        }
    }  // namespace

    EditorMicrophoneSpeechSession::EditorMicrophoneSpeechSession(EditorMicrophonePermission &microphone) noexcept
        : microphone_(microphone) {}

    EditorMicrophoneSpeechSession::~EditorMicrophoneSpeechSession() {
        Cancel();
    }

    /** @copydoc EditorMicrophoneSpeechSession::Start */
    SpeechTranscriptionStatus EditorMicrophoneSpeechSession::Start(const std::size_t input, const bool foreground,
                                                                   const ImmersiveAuthoringContext &owner,
                                                                   const ImmersivePlanningContext &context, const std::string_view locale,
                                                                   const float minimumConfidence) {
        if (worker_.Busy())
            return Closed;
        Cancel();
        try {
            auto provider = std::make_shared<MacOnDeviceSpeechProvider>();
            auto begun = microphone_.BeginSpeechCapture(input, foreground, owner, context);
            if (begun.HasError())
                return CaptureInterrupted;
            const auto &lease = begun.Value();
            generation_ = lease.source.generation;
            channels_ = lease.source.format.layout.orderedChannels.size();
            scratch_.resize(lease.source.maximumFrames * channels_);
            const auto status = worker_.Start(owner, context, lease.source, lease.revoked, std::move(provider), locale, minimumConfidence);
            if (status == Accepted)
                spatial_.Begin(context);
            else
                Cancel();
            return status;
        } catch (const std::bad_alloc &) {
            Cancel();
            return LimitExceeded;
        }
    }

    /** @copydoc EditorMicrophoneSpeechSession::RecordPoseEvidence */
    ImmersiveSpatialCaptureStatus EditorMicrophoneSpeechSession::RecordPoseEvidence(const ImmersivePlanningContext &context,
                                                                                    const XR::XRInteractionCandidates &candidates,
                                                                                    const XR::XRInteractionCutoff &measuredAt,
                                                                                    const EditorSpeechClockSample earliest,
                                                                                    const EditorSpeechClockSample latest) noexcept {
        if (generation_ == 0 || sealed_)
            return ImmersiveSpatialCaptureStatus::Closed;
        if (earliest.generation != generation_ || latest.generation != generation_ || candidates.Cutoff() != measuredAt)
            return ImmersiveSpatialCaptureStatus::StaleContext;

        std::array<ImmersiveSpatialObservation, XR::XRInteractionCandidates::MaximumProbes> observations;
        const auto geometry = candidates.Candidates();
        for (std::size_t index = 0; index < geometry.size(); ++index)
            observations[index] = {measuredAt, geometry[index], earliest.timestamp, latest.timestamp};
        return spatial_.Record(context, std::span{observations}.first(geometry.size()));
    }

    /** @copydoc EditorMicrophoneSpeechSession::ReadCaptureClock */
    std::optional<EditorSpeechClockSample> EditorMicrophoneSpeechSession::ReadCaptureClock() const noexcept {
        if (const auto timestamp = microphone_.ReadSpeechClock(generation_))
            return EditorSpeechClockSample{*timestamp, generation_};
        return std::nullopt;
    }

    /** @copydoc EditorMicrophoneSpeechSession::Seal */
    SpeechTranscriptionStatus EditorMicrophoneSpeechSession::Seal(const bool foreground) {
        if (generation_ == 0 || sealed_ || !worker_.Busy())
            return Closed;
        auto tail = microphone_.SealSpeechCapture(generation_, foreground);
        const auto status = tail.HasValue() ? worker_.Seal(std::move(tail).Value()) : CaptureInterrupted;
        if (status == Accepted) {
            sealed_ = true;
            spatial_.Seal();
        } else {
            Cancel();
        }
        return status;
    }

    /** @copydoc EditorMicrophoneSpeechSession::AssembleCommand */
    ImmersiveCommandAssembly EditorMicrophoneSpeechSession::AssembleCommand(const bool foreground, const ImmersiveSpeechEvidence &speech,
                                                                            const XR::XRInteractionCutoff &cutoff) {
        if (generation_ == 0 || microphone_.ReconcileSpeechLease(generation_, foreground).HasError()) {
            Cancel();
            return {ImmersiveCommandStatus::StaleEvidence, {}};
        }
        if (!worker_.OwnsEvidence(speech))
            return {ImmersiveCommandStatus::StaleEvidence, {}};
        return spatial_.Assemble(speech, cutoff);
    }

    /** @copydoc EditorMicrophoneSpeechSession::Advance */
    SpeechTranscriptionUpdate EditorMicrophoneSpeechSession::Advance(const bool foreground) {
        if (generation_ != 0 && microphone_.ReconcileSpeechLease(generation_, foreground).HasError()) {
            Cancel();
            return {CaptureInterrupted, {}};
        }
        auto update = worker_.Poll();
        if (IsFailure(update.status)) {
            Cancel();
            return update;
        }
        if (sealed_ || generation_ == 0)
            return update;
        for (unsigned index = 0; index < BlocksPerUpdate; ++index) {
            const auto status = TransferBlock(foreground);
            if (status == Pending)
                break;
            if (status != Accepted) {
                Cancel();
                return {status, {}};
            }
        }
        return update;
    }

    SpeechTranscriptionStatus EditorMicrophoneSpeechSession::TransferBlock(const bool foreground) {
        Audio::Backend::AudioCapturedBlock block{};
        const auto read = microphone_.ReadSpeechCapture(generation_, foreground, block, scratch_);
        if (read.HasError())
            return CaptureInterrupted;
        using enum Audio::Backend::AudioCaptureRead;
        if (read.Value() == Empty)
            return Pending;
        if (read.Value() != Copied)
            return CaptureInterrupted;
        const auto samples = std::span{scratch_}.first(block.frames * channels_);
        const auto submitted = worker_.Submit(block, samples);
        Security::SecureZero(std::as_writable_bytes(samples));
        return submitted;
    }

    /** @copydoc EditorMicrophoneSpeechSession::Cancel */
    void EditorMicrophoneSpeechSession::Cancel() noexcept {
        worker_.Cancel();
        spatial_.Reset();
        if (generation_ != 0)
            microphone_.CancelSpeechCapture(std::exchange(generation_, 0));
        Security::SecureZero(std::as_writable_bytes(std::span{scratch_}));
        sealed_ = false;
    }
}  // namespace Horo::Editor
