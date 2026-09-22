#include "Horo/Runtime/Ui/UiGlyphAtlas.h"

#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiGlyphAtlasStorage.h"

#include <new>
#include <stdexcept>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] constexpr bool IsKnown(const UiGlyphAtlasFrameOutcome value) noexcept {
            return value < UiGlyphAtlasFrameOutcome::Count;
        }

        [[nodiscard]] bool IsTerminal(const UiGlyphAtlasUploadState state) noexcept {
            return state == UiGlyphAtlasUploadState::Ready || state == UiGlyphAtlasUploadState::Failed ||
                   state == UiGlyphAtlasUploadState::Cancelled || state == UiGlyphAtlasUploadState::Retired;
        }

        [[nodiscard]] Result<void> InvalidUploadTransition() {
            return Failure(UiErrors::GlyphAtlasUploadInvalidTransition);
        }

        [[nodiscard]] Result<void> InvalidFrameOutcome(const UiGlyphAtlasFrameOutcome outcome) {
            return IsKnown(outcome) ? Result<void>::Success() : Failure(UiErrors::GlyphAtlasFrameInvalid);
        }
    }  // namespace

    /** @copydoc UiGlyphAtlas::Create */
    Result<UiGlyphAtlas> UiGlyphAtlas::Create(const UiGlyphAtlasDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiGlyphAtlas>(UiErrors::GlyphAtlasInputInvalid);
        try {
            return Result<UiGlyphAtlas>::Success(UiGlyphAtlas{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiGlyphAtlas>(UiErrors::GlyphAtlasCapacityExceeded);
        } catch (const std::length_error &) {
            return Failure<UiGlyphAtlas>(UiErrors::GlyphAtlasCapacityExceeded);
        }
    }

    /** @copydoc UiGlyphAtlas::UiGlyphAtlas(std::unique_ptr<Storage>) */
    UiGlyphAtlas::UiGlyphAtlas(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiGlyphAtlas::~UiGlyphAtlas */
    UiGlyphAtlas::~UiGlyphAtlas() = default;

    /** @copydoc UiGlyphAtlas::UiGlyphAtlas(UiGlyphAtlas &&) */
    UiGlyphAtlas::UiGlyphAtlas(UiGlyphAtlas &&other) noexcept = default;

    /** @copydoc UiGlyphAtlas::operator= */
    UiGlyphAtlas &UiGlyphAtlas::operator=(UiGlyphAtlas &&other) noexcept = default;

    /** @copydoc UiGlyphAtlas::Pages */
    std::span<const UiGlyphAtlasPageId> UiGlyphAtlas::Pages() const noexcept {
        return storage_ ? std::span<const UiGlyphAtlasPageId>{storage_->pages} : std::span<const UiGlyphAtlasPageId>{};
    }

    /** @copydoc UiGlyphAtlas::PageExtent */
    UiGlyphAtlasPageExtent UiGlyphAtlas::PageExtent() const noexcept {
        return storage_ ? storage_->descriptor.pageExtent : UiGlyphAtlasPageExtent{};
    }

    /** @copydoc UiGlyphAtlas::TileExtent */
    UiGlyphAtlasTileExtent UiGlyphAtlas::TileExtent() const noexcept {
        return storage_ ? storage_->descriptor.tileExtent : UiGlyphAtlasTileExtent{};
    }

    /** @copydoc UiGlyphAtlas::Format */
    UiGlyphAtlasRasterFormat UiGlyphAtlas::Format() const noexcept {
        return storage_ ? storage_->descriptor.format : UiGlyphAtlasRasterFormat::Count;
    }

    /** @copydoc UiGlyphAtlas::Revision */
    UiGlyphAtlasRevision UiGlyphAtlas::Revision() const noexcept {
        return storage_ ? storage_->revision : UiGlyphAtlasRevision{};
    }

    /** @copydoc UiGlyphAtlas::RequestUpload */
    Result<UiGlyphAtlasUploadId> UiGlyphAtlas::RequestUpload(const UiGlyphAtlasRasterData &raster) {
        if (!storage_ || !storage_->IsActive())
            return Failure<UiGlyphAtlasUploadId>(UiErrors::GlyphAtlasLifecycleUnavailable);
        return storage_->AdmitUpload(raster);
    }

    /** @copydoc UiGlyphAtlas::DescribeUpload */
    Result<UiGlyphAtlasUploadDescriptor> UiGlyphAtlas::DescribeUpload(const UiGlyphAtlasUploadId upload) const {
        if (!storage_)
            return Failure<UiGlyphAtlasUploadDescriptor>(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<UiGlyphAtlasUploadDescriptor>::Failure(index.ErrorValue());
        return Result<UiGlyphAtlasUploadDescriptor>::Success(storage_->uploads[index.Value()].descriptor);
    }

    /** @copydoc UiGlyphAtlas::Payload */
    Result<std::span<const std::byte>> UiGlyphAtlas::Payload(const UiGlyphAtlasUploadId upload) const {
        if (!storage_)
            return Failure<std::span<const std::byte>>(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<std::span<const std::byte>>::Failure(index.ErrorValue());
        const auto &record = storage_->uploads[index.Value()];
        if (record.state != UiGlyphAtlasUploadState::Pending)
            return Failure<std::span<const std::byte>>(UiErrors::GlyphAtlasUploadInvalidTransition);
        return Result<std::span<const std::byte>>::Success(
            std::span<const std::byte>{storage_->staging}.subspan(record.stagingOffset, record.stagingBytes));
    }

    /** @copydoc UiGlyphAtlas::State */
    Result<UiGlyphAtlasUploadState> UiGlyphAtlas::State(const UiGlyphAtlasUploadId upload) const {
        if (!storage_)
            return Failure<UiGlyphAtlasUploadState>(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<UiGlyphAtlasUploadState>::Failure(index.ErrorValue());
        return Result<UiGlyphAtlasUploadState>::Success(storage_->uploads[index.Value()].state);
    }

    /** @copydoc UiGlyphAtlas::MarkSubmitted */
    Result<void> UiGlyphAtlas::MarkSubmitted(const UiGlyphAtlasUploadId upload, const UiGlyphAtlasCompletionPoint completion) {
        if (!storage_)
            return Failure(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        auto &record = storage_->uploads[index.Value()];
        if (record.state != UiGlyphAtlasUploadState::Pending || !completion.IsValid())
            return InvalidUploadTransition();
        record.completion = completion;
        record.state = UiGlyphAtlasUploadState::Submitted;
        return Result<void>::Success();
    }

    /** @copydoc UiGlyphAtlas::Complete */
    Result<void> UiGlyphAtlas::Complete(const UiGlyphAtlasUploadId upload) {
        if (!storage_)
            return Failure(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        auto &record = storage_->uploads[index.Value()];
        if (record.state == UiGlyphAtlasUploadState::Cancelled) {
            storage_->ReleaseStaging(record);
            if (record.entrySlot != 0)
                storage_->ReleaseEntry(static_cast<std::size_t>(record.entrySlot - 1U));
            record.entrySlot = 0;
            record.state = UiGlyphAtlasUploadState::Retired;
            return Result<void>::Success();
        }
        if (record.state != UiGlyphAtlasUploadState::Submitted || !record.completion.IsValid())
            return InvalidUploadTransition();
        if (record.entrySlot == 0 || record.entrySlot > storage_->entries.size())
            return Failure(UiErrors::GlyphAtlasSourceStale);
        auto &entry = storage_->entries[record.entrySlot - 1U];
        if (entry.state != Storage::EntryState::Pending || entry.uploadSlot != upload.slot)
            return Failure(UiErrors::GlyphAtlasSourceStale);
        storage_->ReleaseStaging(record);
        entry.state = Storage::EntryState::Resident;
        entry.uploadSlot = 0;
        entry.lastUsed = storage_->Touch();
        record.entrySlot = 0;
        record.state = UiGlyphAtlasUploadState::Ready;
        return Result<void>::Success();
    }

    /** @copydoc UiGlyphAtlas::Fail */
    Result<void> UiGlyphAtlas::Fail(const UiGlyphAtlasUploadId upload, const Error &error) {
        if (!storage_)
            return Failure(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        auto &record = storage_->uploads[index.Value()];
        if (record.state == UiGlyphAtlasUploadState::Cancelled)
            return InvalidUploadTransition();
        if (record.state != UiGlyphAtlasUploadState::Pending && record.state != UiGlyphAtlasUploadState::Submitted)
            return InvalidUploadTransition();
        storage_->ReleaseStaging(record);
        if (record.entrySlot != 0 && record.entrySlot <= storage_->entries.size())
            storage_->entries[record.entrySlot - 1U].state = Storage::EntryState::Failed;
        record.failure = error;
        record.state = UiGlyphAtlasUploadState::Failed;
        return Result<void>::Success();
    }

    /** @copydoc UiGlyphAtlas::Cancel */
    Result<void> UiGlyphAtlas::Cancel(const UiGlyphAtlasUploadId upload) {
        if (!storage_)
            return Failure(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        auto &record = storage_->uploads[index.Value()];
        if (record.state == UiGlyphAtlasUploadState::Pending)
            return storage_->CancelPendingUpload(record);
        if (record.state != UiGlyphAtlasUploadState::Submitted)
            return InvalidUploadTransition();
        record.state = UiGlyphAtlasUploadState::Cancelled;
        return Result<void>::Success();
    }

    /** @copydoc UiGlyphAtlas::Retire */
    Result<void> UiGlyphAtlas::Retire(const UiGlyphAtlasUploadId upload) {
        if (!storage_)
            return Failure(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        auto &record = storage_->uploads[index.Value()];
        if (record.state != UiGlyphAtlasUploadState::Cancelled)
            return InvalidUploadTransition();
        storage_->ReleaseStaging(record);
        if (record.entrySlot != 0 && record.entrySlot <= storage_->entries.size())
            storage_->ReleaseEntry(record.entrySlot - 1U);
        record.entrySlot = 0;
        record.state = UiGlyphAtlasUploadState::Retired;
        return Result<void>::Success();
    }

    /** @copydoc UiGlyphAtlas::Discard */
    Result<void> UiGlyphAtlas::Discard(const UiGlyphAtlasUploadId upload) {
        if (!storage_)
            return Failure(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto index = storage_->UploadIndex(upload);
        if (index.HasError())
            return Result<void>::Failure(index.ErrorValue());
        auto &record = storage_->uploads[index.Value()];
        if (!IsTerminal(record.state) || record.stagingBytes != 0)
            return InvalidUploadTransition();
        if (record.state == UiGlyphAtlasUploadState::Failed && record.entrySlot != 0 && record.entrySlot <= storage_->entries.size())
            storage_->ReleaseEntry(record.entrySlot - 1U);
        if (record.state == UiGlyphAtlasUploadState::Cancelled && record.entrySlot != 0)
            return InvalidUploadTransition();
        return storage_->FreeUpload(index.Value());
    }

    /** @copydoc UiGlyphAtlas::BeginFrame */
    Result<UiGlyphAtlasFrameId> UiGlyphAtlas::BeginFrame() {
        if (!storage_ || !storage_->IsActive())
            return Failure<UiGlyphAtlasFrameId>(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto frame = storage_->AcquireFrame();
        if (frame.HasError())
            return Result<UiGlyphAtlasFrameId>::Failure(frame.ErrorValue());
        return Result<UiGlyphAtlasFrameId>::Success(storage_->frames[frame.Value()].id);
    }

    /** @copydoc UiGlyphAtlas::Resolve */
    Result<UiGlyphAtlasLookup> UiGlyphAtlas::Resolve(const UiGlyphAtlasFrameId frame, const UiGlyphAtlasGlyphKey &requested) {
        if (!storage_ || !storage_->IsActive())
            return Failure<UiGlyphAtlasLookup>(UiErrors::GlyphAtlasLifecycleUnavailable);
        const auto frameIndex = storage_->FrameIndex(frame);
        if (frameIndex.HasError())
            return Result<UiGlyphAtlasLookup>::Failure(frameIndex.ErrorValue());
        if (!requested.IsValid())
            return Failure<UiGlyphAtlasLookup>(UiErrors::GlyphAtlasInputInvalid);

        const auto requestedEntry = storage_->FindEntry(requested);
        std::size_t resolvedIndex{};
        UiGlyphAtlasResolutionState resolution = UiGlyphAtlasResolutionState::Fallback;
        if (requestedEntry.has_value() && storage_->entries[*requestedEntry].state == Storage::EntryState::Resident) {
            resolvedIndex = *requestedEntry;
            resolution = UiGlyphAtlasResolutionState::Resident;
        } else {
            const auto fallback = storage_->FindEntry(storage_->descriptor.fallback);
            if (!fallback.has_value() || storage_->entries[*fallback].state != Storage::EntryState::Resident)
                return Failure<UiGlyphAtlasLookup>(UiErrors::GlyphAtlasFallbackUnavailable);
            resolvedIndex = *fallback;
            if (requestedEntry.has_value() && storage_->entries[*requestedEntry].state == Storage::EntryState::Pending)
                resolution = UiGlyphAtlasResolutionState::FallbackPending;
        }

        if (const auto pinned = storage_->Pin(frameIndex.Value(), resolvedIndex); pinned.HasError())
            return Result<UiGlyphAtlasLookup>::Failure(pinned.ErrorValue());
        const auto &entry = storage_->entries[resolvedIndex];
        return Result<UiGlyphAtlasLookup>::Success({requested, entry.key, entry.id, entry.placement, storage_->revision, resolution});
    }

    /** @copydoc UiGlyphAtlas::RetireFrame */
    Result<void> UiGlyphAtlas::RetireFrame(const UiGlyphAtlasFrameId frame, const UiGlyphAtlasFrameOutcome outcome) {
        if (!storage_)
            return Failure(UiErrors::GlyphAtlasLifecycleUnavailable);
        if (const auto validOutcome = InvalidFrameOutcome(outcome); validOutcome.HasError())
            return validOutcome;
        const auto frameIndex = storage_->FrameIndex(frame);
        if (frameIndex.HasError())
            return Result<void>::Failure(frameIndex.ErrorValue());
        auto &record = storage_->frames[frameIndex.Value()];
        for (const auto entryIndex : record.entries) {
            if (entryIndex < storage_->entries.size() && storage_->entries[entryIndex].pinCount > 0)
                --storage_->entries[entryIndex].pinCount;
        }
        record.entries.clear();
        record.active = false;
        return Result<void>::Success();
    }

    /** @copydoc UiGlyphAtlas::Evict */
    Result<UiGlyphAtlasEvictionReport> UiGlyphAtlas::Evict(const UiGlyphAtlasEvictionRequest &request) {
        if (!storage_ || !storage_->IsActive())
            return Failure<UiGlyphAtlasEvictionReport>(UiErrors::GlyphAtlasLifecycleUnavailable);
        if (!request.IsValid())
            return Failure<UiGlyphAtlasEvictionReport>(UiErrors::GlyphAtlasEvictionInvalid);
        UiGlyphAtlasEvictionReport report;
        for (std::uint32_t count = 0; count < request.maximumEntries; ++count) {
            const auto candidate = storage_->FindEvictable();
            if (!candidate.has_value())
                break;
            storage_->EvictEntry(*candidate);
            ++report.evictedEntries;
        }
        for (const auto &entry : storage_->entries) {
            if (entry.state == Storage::EntryState::Resident)
                ++report.residentEntries;
            if (entry.pinCount != 0)
                ++report.pinnedEntries;
        }
        return Result<UiGlyphAtlasEvictionReport>::Success(report);
    }

    /** @copydoc UiGlyphAtlas::Reset */
    Result<UiGlyphAtlasResetReport> UiGlyphAtlas::Reset(const UiGlyphAtlasResetReason reason) {
        if (!storage_)
            return Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasLifecycleUnavailable);
        return storage_->ResetInternal(reason);
    }

    /** @copydoc UiGlyphAtlas::Snapshot */
    UiGlyphAtlasSnapshot UiGlyphAtlas::Snapshot() const noexcept {
        if (!storage_)
            return {};
        UiGlyphAtlasSnapshot snapshot{.revision = storage_->revision,
                                      .pendingUploadBytes = storage_->pendingBytes,
                                      .evictionCount = storage_->evictionCount,
                                      .pressureCount = storage_->pressureCount,
                                      .acceptingRequests = storage_->IsActive()};
        for (const auto &entry : storage_->entries) {
            if (entry.state == Storage::EntryState::Resident)
                ++snapshot.residentEntries;
            else if (entry.state == Storage::EntryState::Pending)
                ++snapshot.pendingEntries;
            if (entry.pinCount != 0)
                ++snapshot.pinnedEntries;
        }
        for (const auto &frame : storage_->frames)
            if (frame.active)
                ++snapshot.activeFrames;
        for (const auto &upload : storage_->uploads)
            if (upload.occupied && (upload.state == UiGlyphAtlasUploadState::Pending || upload.state == UiGlyphAtlasUploadState::Submitted))
                ++snapshot.pendingUploads;
        return snapshot;
    }

    /** @copydoc UiGlyphAtlas::StopAdmission */
    void UiGlyphAtlas::StopAdmission() noexcept {
        if (!storage_)
            return;
        storage_->lifecycle = UiGlyphAtlasState::Closed;
        for (auto &upload : storage_->uploads)
            if (upload.occupied && upload.state == UiGlyphAtlasUploadState::Pending)
                static_cast<void>(storage_->CancelPendingUpload(upload));
    }

    /** @copydoc UiGlyphAtlas::IsDrained */
    bool UiGlyphAtlas::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiGlyphAtlas::State */
    UiGlyphAtlasState UiGlyphAtlas::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiGlyphAtlasState::Closed;
    }
}  // namespace Horo::Runtime::Ui
