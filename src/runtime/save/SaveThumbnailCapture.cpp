#include "Horo/Runtime/Save/SaveThumbnailCapture.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <limits>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Matches source incarnations independently from actual frame provenance. */
        [[nodiscard]] bool SameSource(const SaveThumbnailSource left, const SaveThumbnailSource right) noexcept {
            return left.runtime == right.runtime && left.scene == right.scene && left.view == right.view;
        }

        /** @brief Checks request policy and qualified ceilings before any dispatch. */
        [[nodiscard]] bool ValidRequest(const SaveThumbnailRequest &request, const SaveThumbnailAvailability availability) noexcept {
            const auto &limits = request.limits;
            if (!request.slot.IsValid() || !request.generation.IsValid() || request.source.runtime == 0 || request.source.scene == 0 ||
                request.policy > SaveThumbnailPolicy::Required || request.format != SaveThumbnailFormat::Png ||
                availability > SaveThumbnailAvailability::RendererUnavailable)
                return false;
            if (limits.maximumDimension == 0 || limits.maximumDimension > 4'096 || limits.maximumEncodedBytes == 0 ||
                limits.maximumEncodedBytes > (16U << 20U) || limits.timeout.count() <= 0 || limits.timeout > std::chrono::seconds{60})
                return false;
            if (request.width == 0 || request.height == 0 || request.width > limits.maximumDimension ||
                request.height > limits.maximumDimension)
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
    }  // namespace

    /** @copydoc SaveThumbnailCapture::Request */
    Result<std::uint64_t> SaveThumbnailCapture::Request(SaveThumbnailRequest request, const SaveThumbnailAvailability availability,
                                                        const std::chrono::steady_clock::time_point now) {
        if (closed_)
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ThumbnailCancelled));
        if (snapshot_.state != SaveThumbnailCaptureState::Idle || nextSerial_ == std::numeric_limits<std::uint64_t>::max())
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ThumbnailBusy));
        if (!ValidRequest(request, availability) || now < std::chrono::steady_clock::time_point{})
            return Result<std::uint64_t>::Failure(MakeError(SaveErrors::ThumbnailInvalid));
        snapshot_.requestSerial = nextSerial_++;
        snapshot_.request = std::move(request);
        snapshot_.state = SaveThumbnailCaptureState::Pending;
        started_ = now;
        observed_ = now;
        if (snapshot_.request->policy == SaveThumbnailPolicy::Disabled)
            snapshot_.state = SaveThumbnailCaptureState::Omitted;
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
    Result<void> SaveThumbnailCapture::Advance(const std::chrono::steady_clock::time_point now, const SaveThumbnailSource current) {
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
                                                const SaveThumbnailSource current) {
        if (snapshot_.state != SaveThumbnailCaptureState::Pending || completion.requestSerial != snapshot_.requestSerial ||
            completion.slot != snapshot_.request->slot || completion.generation != snapshot_.request->generation ||
            completion.thumbnail != snapshot_.request->thumbnail)
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

    /** @copydoc MakeSaveCommittedPresentation */
    Result<SaveCommittedPresentation> MakeSaveCommittedPresentation(SaveSlotPublicationMetadata publication,
                                                                    SaveSlotDisplayMetadata display,
                                                                    const SaveThumbnailCaptureSnapshot &capture,
                                                                    const SaveSlotMetadataLimits &limits) {
        if (const auto valid = ValidateSaveSlotPublicationMetadata(publication, limits); valid.HasError())
            return Result<SaveCommittedPresentation>::Failure(valid.ErrorValue());
        if (!capture.request || capture.requestSerial == 0 || capture.state == SaveThumbnailCaptureState::Idle ||
            capture.state == SaveThumbnailCaptureState::Pending)
            return Result<SaveCommittedPresentation>::Failure(MakeError(SaveErrors::ThumbnailInvalid));
        if (capture.request->slot != publication.slot || capture.request->generation != publication.generation)
            return Result<SaveCommittedPresentation>::Failure(MakeError(SaveErrors::ThumbnailStale));
        if (capture.state == SaveThumbnailCaptureState::Failed)
            return Result<SaveCommittedPresentation>::Failure(capture.error.value_or(MakeError(SaveErrors::ThumbnailInvalid)));
        if (capture.state == SaveThumbnailCaptureState::Captured) {
            if (!capture.artifact || !publication.thumbnail || *publication.thumbnail != capture.artifact->Request().thumbnail ||
                capture.artifact->Request().slot != publication.slot || capture.artifact->Request().generation != publication.generation)
                return Result<SaveCommittedPresentation>::Failure(MakeError(SaveErrors::ThumbnailStale));
        } else if (capture.state != SaveThumbnailCaptureState::Omitted || capture.request->policy == SaveThumbnailPolicy::Required ||
                   publication.thumbnail || capture.artifact) {
            return Result<SaveCommittedPresentation>::Failure(MakeError(SaveErrors::ThumbnailInvalid));
        }
        if (ValidateSaveSlotDisplayMetadata(display, limits).HasError())
            display = {};
        return Result<SaveCommittedPresentation>::Success(
            {.catalog = {.publication = std::move(publication), .display = std::move(display)}, .artifact = capture.artifact});
    }
}  // namespace Horo::Runtime
