#include "Horo/Runtime/Save/SaveThumbnailCapture.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Matches source incarnations independently from actual frame provenance. */
        [[nodiscard]] bool SameSource(const SaveThumbnailSource &left, const SaveThumbnailSource &right) noexcept {
            return left.runtime == right.runtime && left.scene == right.scene && left.view == right.view;
        }

        /** @brief Checks stable identity and supported capture policy before validating resource ceilings. */
        [[nodiscard]] bool ValidRequestIdentity(const SaveThumbnailRequest &request,
                                                const SaveThumbnailAvailability availability) noexcept {
            return request.slot.IsValid() && request.generation.IsValid() && request.source.runtime != 0 && request.source.scene != 0 &&
                   request.policy <= SaveThumbnailPolicy::Required && request.format == SaveThumbnailFormat::Png &&
                   availability <= SaveThumbnailAvailability::RendererUnavailable;
        }

        /** @brief Checks qualified finite capture ceilings before dimensions or dispatch. */
        [[nodiscard]] bool ValidLimits(const SaveThumbnailLimits &limits) noexcept {
            return limits.maximumDimension != 0 && limits.maximumDimension <= 4'096 && limits.maximumEncodedBytes != 0 &&
                   limits.maximumEncodedBytes <= (16U << 20U) && limits.timeout.count() > 0 && limits.timeout <= std::chrono::seconds{60};
        }

        /** @brief Checks requested image dimensions against the admitted dimension ceiling. */
        [[nodiscard]] bool ValidDimensions(const SaveThumbnailRequest &request) noexcept {
            return request.width != 0 && request.height != 0 && request.width <= request.limits.maximumDimension &&
                   request.height <= request.limits.maximumDimension;
        }

        /** @brief Checks request policy and qualified ceilings before any dispatch. */
        [[nodiscard]] bool ValidRequest(const SaveThumbnailRequest &request, const SaveThumbnailAvailability availability) noexcept {
            if (!ValidRequestIdentity(request, availability) || !ValidLimits(request.limits) || !ValidDimensions(request))
                return false;
            return request.policy == SaveThumbnailPolicy::Disabled || availability != SaveThumbnailAvailability::Available ||
                   (request.thumbnail.IsValid() && request.source.view != 0 && request.source.frame != 0);
        }

        /** @brief Checks detached completion bounds; PNG decoding and full image integrity remain adapter/decoder duties. */
        [[nodiscard]] bool ValidCompletion(const SaveThumbnailRequest &request, const SaveThumbnailCompletion &completion) noexcept {
            return SameSource(request.source, completion.source) && completion.source.frame >= request.source.frame &&
                   completion.format == request.format && completion.width == request.width && completion.height == request.height &&
                   !completion.encoded.empty() && completion.encoded.size() <= request.limits.maximumEncodedBytes;
        }

        /** @brief Fences a pending completion against the exact requested slot, generation and thumbnail. */
        [[nodiscard]] bool MatchesCompletion(const SaveThumbnailRequest &request, const std::uint64_t serial,
                                             const SaveThumbnailCompletion &completion) noexcept {
            return completion.requestSerial == serial && completion.slot == request.slot && completion.generation == request.generation &&
                   completion.thumbnail == request.thumbnail;
        }

        /** @brief Matches a captured artifact to its published generation without accepting an absent thumbnail. */
        [[nodiscard]] bool MatchesCapturedPublication(const SaveSlotPublicationMetadata &publication,
                                                      const SaveThumbnailCaptureSnapshot &capture) noexcept {
            return capture.artifact && publication.thumbnail && *publication.thumbnail == capture.artifact->Request().thumbnail &&
                   capture.artifact->Request().slot == publication.slot && capture.artifact->Request().generation == publication.generation;
        }

        /** @brief Validates terminal capture outcome after request identity admission, preserving its original failure cause. */
        [[nodiscard]] Result<void> ValidateTerminalCapture(const SaveSlotPublicationMetadata &publication,
                                                           const SaveThumbnailCaptureSnapshot &capture) {
            using enum SaveThumbnailCaptureState;
            if (capture.state == Failed)
                return Result<void>::Failure(capture.error.value_or(MakeError(SaveErrors::ThumbnailInvalid)));
            if (capture.state == Captured) {
                if (!MatchesCapturedPublication(publication, capture))
                    return Result<void>::Failure(MakeError(SaveErrors::ThumbnailStale));
            } else if (capture.state != Omitted || capture.request->policy == SaveThumbnailPolicy::Required || publication.thumbnail ||
                       capture.artifact) {
                return Result<void>::Failure(MakeError(SaveErrors::ThumbnailInvalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc SaveThumbnailCapture::Request */
    Result<std::uint64_t> SaveThumbnailCapture::Request(SaveThumbnailRequest request, const SaveThumbnailAvailability availability,
                                                        const std::chrono::steady_clock::time_point now) {
        using enum SaveThumbnailCaptureState;
        if (closed_)
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ThumbnailCancelled));
        if (snapshot_.state != Idle || nextSerial_ == std::numeric_limits<std::uint64_t>::max())
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ThumbnailBusy));
        if (!ValidRequest(request, availability) || now < std::chrono::steady_clock::time_point{})
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ThumbnailInvalid));
        snapshot_.requestSerial = nextSerial_++;
        snapshot_.request = std::move(request);
        snapshot_.state = Pending;
        started_ = now;
        observed_ = now;
        if (snapshot_.request->policy == SaveThumbnailPolicy::Disabled)
            snapshot_.state = Omitted;
        else if (availability != SaveThumbnailAvailability::Available)
            Finish(MakeError(SaveErrors::ThumbnailUnavailable));
        return Result<std::uint64_t>::Success(snapshot_.requestSerial);
    }

    /** @copydoc SaveThumbnailCapture::Finish */
    void SaveThumbnailCapture::Finish(Error error) {
        snapshot_.state = snapshot_.request->policy == SaveThumbnailPolicy::Required ? SaveThumbnailCaptureState::Failed
                                                                                     : SaveThumbnailCaptureState::Omitted;
        snapshot_.error = std::move(error);
        snapshot_.artifact.reset();
    }

    /** @copydoc SaveThumbnailCapture::Advance */
    Result<void> SaveThumbnailCapture::Advance(const std::chrono::steady_clock::time_point now, const SaveThumbnailSource &current) {
        if (snapshot_.state != SaveThumbnailCaptureState::Pending)
            return Result<void>::Success();
        if (now < observed_)
            return Result<void>::Failure(MakeError(SaveErrors::ThumbnailInvalid));
        observed_ = now;
        if (!SameSource(snapshot_.request->source, current))
            Finish(MakeError(SaveErrors::ThumbnailStale));
        else if (now - started_ >= snapshot_.request->limits.timeout)
            Finish(MakeError(SaveErrors::ThumbnailExpired));
        return Result<void>::Success();
    }

    /** @copydoc SaveThumbnailCapture::Complete */
    Result<bool> SaveThumbnailCapture::Complete(SaveThumbnailCompletion completion, const std::chrono::steady_clock::time_point now,
                                                const SaveThumbnailSource &current) {
        if (snapshot_.state != SaveThumbnailCaptureState::Pending ||
            !MatchesCompletion(*snapshot_.request, snapshot_.requestSerial, completion))
            return Result<bool>::Success(false);
        if (const auto advanced = Advance(now, current); advanced.HasError())
            return Result<bool>::Failure(advanced.ErrorValue());
        if (snapshot_.state != SaveThumbnailCaptureState::Pending)
            return Result<bool>::Success(false);
        if (completion.error) {
            Finish(std::move(*completion.error));
            return Result<bool>::Success(true);
        }
        if (!ValidCompletion(*snapshot_.request, completion)) {
            Finish(MakeError(SaveErrors::ThumbnailInvalid));
            return Result<bool>::Success(true);
        }
        try {
            auto artifact = std::make_shared<SaveThumbnailArtifact>();
            artifact->request_ = *snapshot_.request;
            artifact->source_ = completion.source;
            artifact->bytes_ = std::move(completion.encoded);
            snapshot_.artifact = std::move(artifact);
            snapshot_.state = SaveThumbnailCaptureState::Captured;
        } catch (const std::bad_alloc &) {
            Finish(MakeError(SaveErrors::ThumbnailAllocationFailed));
        }
        return Result<bool>::Success(true);
    }

    /** @copydoc SaveThumbnailCapture::Cancel */
    bool SaveThumbnailCapture::Cancel(const std::uint64_t serial) {
        if (snapshot_.state != SaveThumbnailCaptureState::Pending || serial != snapshot_.requestSerial)
            return false;
        Finish(MakeError(SaveErrors::ThumbnailCancelled));
        return true;
    }

    /** @copydoc SaveThumbnailCapture::BeginShutdown */
    void SaveThumbnailCapture::BeginShutdown() {
        closed_ = true;
        if (snapshot_.state == SaveThumbnailCaptureState::Pending)
            Finish(MakeError(SaveErrors::ThumbnailCancelled));
    }

    /** @copydoc SaveThumbnailCapture::Acknowledge */
    bool SaveThumbnailCapture::Acknowledge(const std::uint64_t serial) noexcept {
        if (serial != snapshot_.requestSerial || snapshot_.state == SaveThumbnailCaptureState::Idle ||
            snapshot_.state == SaveThumbnailCaptureState::Pending)
            return false;
        snapshot_ = {};
        return true;
    }

    /** @copydoc ValidateSaveThumbnailPublication */
    Result<void> ValidateSaveThumbnailPublication(const SaveSlotPublicationMetadata &publication,
                                                  const SaveThumbnailCaptureSnapshot &capture, const SaveSlotMetadataLimits &limits) {
        using enum SaveThumbnailCaptureState;
        if (const auto valid = ValidateSaveSlotPublicationMetadata(publication, limits); valid.HasError())
            return Result<void>::Failure(valid.ErrorValue());
        if (!capture.request || capture.requestSerial == 0 || capture.state == Idle || capture.state == Pending)
            return Result<void>::Failure(MakeError(SaveErrors::ThumbnailInvalid));
        if (capture.request->slot != publication.slot || capture.request->generation != publication.generation)
            return Result<void>::Failure(MakeError(SaveErrors::ThumbnailStale));
        return ValidateTerminalCapture(publication, capture);
    }

    /** @copydoc MakeSaveCommittedPresentation */
    Result<SaveCommittedPresentation> MakeSaveCommittedPresentation(SaveSlotPublicationMetadata publication,
                                                                    SaveSlotDisplayMetadata display,
                                                                    const SaveThumbnailCaptureSnapshot &capture,
                                                                    const SaveSlotMetadataLimits &limits) {
        if (const auto valid = ValidateSaveThumbnailPublication(publication, capture, limits); valid.HasError())
            return Result<SaveCommittedPresentation>::Failure(valid.ErrorValue());
        if (ValidateSaveSlotDisplayMetadata(display, limits).HasError())
            display = {};
        return Result<SaveCommittedPresentation>::Success(
            {.catalog = {.publication = std::move(publication), .display = std::move(display)}, .artifact = capture.artifact});
    }
}  // namespace Horo::Runtime
