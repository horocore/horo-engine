#include "Horo/Editor/AudioEditorDocument.h"

#include <algorithm>
#include <utility>

namespace Horo::Editor {
    namespace AudioEditorDocumentErrors {
        const ErrorCodeDescriptor InvalidIdentity{ErrorDomainId{"editor.audio"}, ErrorCode{"editor.audio.identity.invalid"},
                                                  ErrorSeverity::Error, "Audio document identity or revision is invalid.",
                                                  "Capture an open audio Asset document."};
        const ErrorCodeDescriptor StaleRevision{ErrorDomainId{"editor.audio"}, ErrorCode{"editor.audio.revision.stale"},
                                                ErrorSeverity::Warning, "Audio preview request belongs to an older source revision.",
                                                "Capture the current authoring revision."};
        const ErrorCodeDescriptor Closed{ErrorDomainId{"editor.audio"}, ErrorCode{"editor.audio.document.closed"}, ErrorSeverity::Warning,
                                         "Audio document is closing or closed.", "Open a new document session."};
        const ErrorCodeDescriptor PreviewUnavailable{ErrorDomainId{"editor.audio"}, ErrorCode{"editor.audio.preview.unavailable"},
                                                     ErrorSeverity::Warning, "No current prepared audio preview is available.",
                                                     "Prepare the current source through AudioFrontend."};
        const ErrorCodeDescriptor PreviewBusy{ErrorDomainId{"editor.audio"}, ErrorCode{"editor.audio.preview.busy"}, ErrorSeverity::Warning,
                                              "The previous preview still owns runtime resources.",
                                              "Continue control pumping until its close completes."};
        const ErrorCodeDescriptor Unfocused{ErrorDomainId{"editor.audio"}, ErrorCode{"editor.audio.document.unfocused"},
                                            ErrorSeverity::Warning, "Audio transport intent requires the focused document.",
                                            "Focus the audio document before starting playback."};
    }  // namespace AudioEditorDocumentErrors

    namespace {
        /** @brief Preserves document boundary failures without translating runtime causes. */
        Result<void> Failure(const ErrorCodeDescriptor &error) {
            return Result<void>::Failure(MakeError(error));
        }
    }  // namespace

    /** @copydoc AudioEditorDocument::AudioEditorDocument */
    AudioEditorDocument::AudioEditorDocument(DocumentIdentity identity, const std::uint64_t revision) noexcept
        : identity_(std::move(identity)), revision_(revision) {}

    /** @copydoc AudioEditorDocument::Open */
    Result<AudioEditorDocument> AudioEditorDocument::Open(DocumentIdentity identity, const std::uint64_t sourceRevision) {
        if (!identity.IsValid() || identity.key.kind != DocumentKind::Asset || sourceRevision == 0)
            return Result<AudioEditorDocument>::Failure(MakeError(AudioEditorDocumentErrors::InvalidIdentity));
        return Result<AudioEditorDocument>::Success(AudioEditorDocument(std::move(identity), sourceRevision));
    }

    /** @copydoc AudioEditorDocument::AudioEditorDocument */
    AudioEditorDocument::AudioEditorDocument(AudioEditorDocument &&other) noexcept
        : identity_(std::move(other.identity_)), revision_(std::exchange(other.revision_, 0)),
          previewRevision_(std::exchange(other.previewRevision_, 0)), focused_(std::exchange(other.focused_, false)),
          phase_(std::exchange(other.phase_, AudioEditorDocumentPhase::Closed)), preview_(std::move(other.preview_)),
          retiredResults_(other.retiredResults_), retiredResultCount_(std::exchange(other.retiredResultCount_, 0)) {}

    /** @copydoc AudioEditorDocument::~AudioEditorDocument */
    AudioEditorDocument::~AudioEditorDocument() {
        Close();
        // The host pumps close before destruction. AudioFrontend's fatal lifetime guard prevents
        // premature tab destruction from freeing native callback or stream memory.
    }

    /** @copydoc AudioEditorDocument::CheckRevision */
    Result<void> AudioEditorDocument::CheckRevision(const std::uint64_t expectedRevision) const {
        if (phase_ != AudioEditorDocumentPhase::Open)
            return Failure(AudioEditorDocumentErrors::Closed);
        if (expectedRevision != revision_)
            return Failure(AudioEditorDocumentErrors::StaleRevision);
        return Result<void>::Success();
    }

    /** @copydoc AudioEditorDocument::AttachPreview */
    Result<void> AudioEditorDocument::AttachPreview(const std::uint64_t expectedRevision, std::unique_ptr<Audio::AudioFrontend> &preview) {
        if (const auto valid = CheckRevision(expectedRevision); valid.HasError())
            return valid;
        if (!preview || preview->Snapshot().phase != Audio::AudioFrontendPhase::Prepared)
            return Failure(AudioEditorDocumentErrors::PreviewUnavailable);
        if (preview_ || retiredResultCount_ != 0)
            return Failure(AudioEditorDocumentErrors::PreviewBusy);
        preview_ = std::move(preview);
        previewRevision_ = expectedRevision;
        return Result<void>::Success();
    }

    /** @copydoc AudioEditorDocument::StartPreview */
    Result<void> AudioEditorDocument::StartPreview(const std::uint64_t expectedRevision, const Audio::AudioMonotonicTimestamp deadline) {
        if (const auto valid = CheckRevision(expectedRevision); valid.HasError())
            return valid;
        if (!focused_)
            return Failure(AudioEditorDocumentErrors::Unfocused);
        if (!preview_ || previewRevision_ != revision_)
            return Failure(AudioEditorDocumentErrors::PreviewUnavailable);
        return preview_->Start(deadline);
    }

    /** @copydoc AudioEditorDocument::Transport */
    Result<Audio::AudioCommandAdmission> AudioEditorDocument::Transport(const std::uint64_t expectedRevision,
                                                                        const Audio::AudioVoiceControlRequest &control) {
        using enum Audio::AudioVoiceControl;

        if (const auto valid = CheckRevision(expectedRevision); valid.HasError())
            return Result<Audio::AudioCommandAdmission>::Failure(valid.ErrorValue());
        if (!focused_ && control.control != Stop && control.control != Pause && control.control != Cancel)
            return Result<Audio::AudioCommandAdmission>::Failure(MakeError(AudioEditorDocumentErrors::Unfocused));
        if (!preview_ || previewRevision_ != revision_)
            return Result<Audio::AudioCommandAdmission>::Failure(MakeError(AudioEditorDocumentErrors::PreviewUnavailable));
        return preview_->Transport(control);
    }

    /** @copydoc AudioEditorDocument::DrainTransportResults */
    std::size_t AudioEditorDocument::DrainTransportResults(const std::span<Audio::AudioFrontendOperationResult> output) noexcept {
        const auto count = std::min(output.size(), retiredResultCount_);
        for (std::size_t index = 0; index < count; ++index)
            output[index] = retiredResults_[index];
        for (std::size_t index = count; index < retiredResultCount_; ++index)
            retiredResults_[index - count] = retiredResults_[index];
        retiredResultCount_ -= count;
        return preview_ ? count + preview_->DrainTransportResults(output.subspan(count)) : count;
    }

    /** @copydoc AudioEditorDocument::SetFocused */
    Result<void> AudioEditorDocument::SetFocused(const std::uint64_t expectedRevision, const bool focused) {
        if (const auto valid = CheckRevision(expectedRevision); valid.HasError())
            return valid;
        focused_ = focused;
        return Result<void>::Success();
    }

    /** @copydoc AudioEditorDocument::Reload */
    Result<void> AudioEditorDocument::Reload(const std::uint64_t expectedRevision, const std::uint64_t nextRevision) {
        if (const auto valid = CheckRevision(expectedRevision); valid.HasError())
            return valid;
        if (nextRevision <= revision_)
            return Failure(AudioEditorDocumentErrors::InvalidIdentity);
        if (preview_)
            preview_->Close();
        revision_ = nextRevision;
        previewRevision_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc AudioEditorDocument::Close */
    void AudioEditorDocument::Close() noexcept {
        if (phase_ == AudioEditorDocumentPhase::Closed)
            return;
        focused_ = false;
        phase_ = preview_ ? AudioEditorDocumentPhase::Closing : AudioEditorDocumentPhase::Closed;
        if (preview_)
            preview_->Close();
    }

    /** @copydoc AudioEditorDocument::Pump */
    Result<void> AudioEditorDocument::Pump(const Audio::AudioMonotonicTimestamp deadline) {
        if (!preview_)
            return Result<void>::Success();
        const auto pumped = preview_->Pump(deadline);
        if (preview_->Snapshot().phase == Audio::AudioFrontendPhase::Closed) {
            // Attachment excludes prior undrained captures; one frontend admits at most this fixed capacity.
            retiredResultCount_ = preview_->DrainTransportResults(retiredResults_);
            preview_.reset();
            previewRevision_ = 0;
            if (phase_ == AudioEditorDocumentPhase::Closing)
                phase_ = AudioEditorDocumentPhase::Closed;
        }
        return pumped;
    }

    /** @copydoc AudioEditorDocument::Snapshot */
    AudioEditorDocumentSnapshot AudioEditorDocument::Snapshot() const {
        return {identity_,          revision_, previewRevision_,
                focused_,           phase_,    preview_ ? std::optional{preview_->Snapshot()} : std::nullopt,
                retiredResultCount_};
    }
}  // namespace Horo::Editor
