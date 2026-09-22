#include "UiGlyphAtlasStorage.h"

namespace Horo::Runtime::Ui {
    Result<void> UiGlyphAtlas::Storage::PublishUpload(const std::size_t uploadIndex, const std::size_t entryIndex,
                                                      const UiGlyphAtlasRasterData &raster, const std::size_t stagingOffset) {
        try {
            const auto position = std::ranges::lower_bound(entriesByKey, raster.key, std::ranges::less{}, &KeyIndex::key);
            entriesByKey.emplace(position, raster.key, entryIndex);
        } catch (const std::bad_alloc &) {
            return UiGlyphAtlasStorageDetail::Failure(UiErrors::GlyphAtlasCapacityExceeded);
        } catch (const std::length_error &) {
            return UiGlyphAtlasStorageDetail::Failure(UiErrors::GlyphAtlasCapacityExceeded);
        }

        auto &entry = entries[entryIndex];
        entry.key = raster.key;
        entry.placement = Placement(entryIndex, raster.width, raster.height);
        entry.state = EntryState::Pending;
        entry.uploadSlot = static_cast<std::uint32_t>(uploadIndex + 1U);
        entry.fallback = raster.key == descriptor.fallback;

        auto &upload = uploads[uploadIndex];
        upload.descriptor = {.upload = upload.id,
                             .entry = entry.id,
                             .key = raster.key,
                             .placement = entry.placement,
                             .revision = revision,
                             .format = raster.format,
                             .rowBytes = raster.rowBytes,
                             .byteCount = raster.bytes.size(),
                             .fallback = entry.fallback};
        upload.state = UiGlyphAtlasUploadState::Pending;
        upload.stagingOffset = stagingOffset;
        upload.stagingBytes = raster.bytes.size();
        upload.entrySlot = static_cast<std::uint32_t>(entryIndex + 1U);
        std::ranges::copy(raster.bytes, staging.begin() + static_cast<std::ptrdiff_t>(stagingOffset));
        pendingBytes += raster.bytes.size();
        nextUpload = (uploadIndex + 1U) % uploads.size();
        return Result<void>::Success();
    }

    Result<UiGlyphAtlasUploadId> UiGlyphAtlas::Storage::AdmitUpload(const UiGlyphAtlasRasterData &raster) {
        if (!raster.IsValid() || raster.format != descriptor.format || raster.width > descriptor.tileExtent.width ||
            raster.height > descriptor.tileExtent.height || raster.bytes.size() > descriptor.limits.maximumPendingUploadBytes)
            return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasUploadId>(UiErrors::GlyphAtlasUploadInvalid);

        if (const auto existing = FindEntry(raster.key); existing.has_value()) {
            if (entries[*existing].state != EntryState::Failed)
                return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasUploadId>(UiErrors::GlyphAtlasUploadInvalidTransition);
            const auto recycled = RecycleFailedEntry(*existing);
            if (recycled.HasError())
                return Result<UiGlyphAtlasUploadId>::Failure(recycled.ErrorValue());
        }

        const auto uploadIndex = AcquireUpload();
        if (uploadIndex.HasError())
            return Result<UiGlyphAtlasUploadId>::Failure(uploadIndex.ErrorValue());

        std::size_t stagingOffset{};
        if (pendingBytes > descriptor.limits.maximumPendingUploadBytes - raster.bytes.size() ||
            !AllocateStaging(raster.bytes.size(), stagingOffset)) {
            static_cast<void>(FreeUpload(uploadIndex.Value()));
            return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasUploadId>(UiErrors::GlyphAtlasUploadCapacityExceeded);
        }

        const auto acquiredEntry = AcquireUploadEntry();
        if (acquiredEntry.HasError()) {
            static_cast<void>(FreeUpload(uploadIndex.Value()));
            return Result<UiGlyphAtlasUploadId>::Failure(acquiredEntry.ErrorValue());
        }

        if (const auto published = PublishUpload(uploadIndex.Value(), acquiredEntry.Value(), raster, stagingOffset); published.HasError()) {
            ReturnAcquiredEntry(acquiredEntry.Value());
            static_cast<void>(FreeUpload(uploadIndex.Value()));
            return Result<UiGlyphAtlasUploadId>::Failure(published.ErrorValue());
        }
        return Result<UiGlyphAtlasUploadId>::Success(uploads[uploadIndex.Value()].id);
    }

    Result<UiGlyphAtlasResetReport> UiGlyphAtlas::Storage::ResetInternal(const UiGlyphAtlasResetReason reason) {
        if (!IsActive())
            return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasLifecycleUnavailable);
        if (!UiGlyphAtlasStorageDetail::IsKnown(reason))
            return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasResetInvalid);
        if (std::ranges::any_of(frames, [](const FrameRecord &frame) {
            return frame.active;
        }))
            return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasResetBusy);
        if (std::ranges::any_of(uploads, [](const UploadRecord &upload) {
            return upload.occupied &&
                   (upload.state == UiGlyphAtlasUploadState::Submitted ||
                    (upload.state == UiGlyphAtlasUploadState::Cancelled && (upload.entrySlot != 0 || upload.stagingBytes != 0)));
        }))
            return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasUploadInFlight);

        const auto nextRevision = revision.Next();
        if (nextRevision.HasError())
            return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasResetInvalid);
        for (const auto page : pages)
            if (page.generation == std::numeric_limits<std::uint32_t>::max())
                return UiGlyphAtlasStorageDetail::Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasResetInvalid);

        UiGlyphAtlasResetReport report{.reason = reason, .previousRevision = revision, .revision = nextRevision.Value()};
        for (const auto &entry : entries)
            if (entry.state != EntryState::Free)
                ++report.invalidatedEntries;
        ResetUploads(report);
        ResetEntries();
        AdvancePageGenerations();
        revision = nextRevision.Value();
        report.requiresFallbackUpload = true;
        return Result<UiGlyphAtlasResetReport>::Success(report);
    }

    bool UiGlyphAtlas::Storage::IsDrained() const noexcept {
        using enum UiGlyphAtlasUploadState;
        return !std::ranges::any_of(frames, [](const FrameRecord &frame) {
            return frame.active;
        }) && !std::ranges::any_of(uploads, [](const UploadRecord &upload) {
            return upload.occupied && (upload.state == Pending || upload.state == Submitted ||
                                       (upload.state == Cancelled && (upload.entrySlot != 0 || upload.stagingBytes != 0)));
        });
    }
}  // namespace Horo::Runtime::Ui
