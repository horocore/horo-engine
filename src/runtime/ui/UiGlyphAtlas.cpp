#include "Horo/Runtime/Ui/UiGlyphAtlas.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] constexpr bool IsKnown(const UiGlyphAtlasFrameOutcome value) noexcept {
            return value < UiGlyphAtlasFrameOutcome::Count;
        }

        [[nodiscard]] constexpr bool IsKnown(const UiGlyphAtlasResetReason value) noexcept {
            return value < UiGlyphAtlasResetReason::Count;
        }

        [[nodiscard]] bool SameOwner(const UiGlyphAtlasPageId id, const UiOwnershipGeneration owner) noexcept {
            return id.IsValid() && id.ownership == owner;
        }

        [[nodiscard]] bool SameOwner(const UiGlyphAtlasEntryId id, const UiOwnershipGeneration owner) noexcept {
            return id.IsValid() && id.ownership == owner;
        }

        [[nodiscard]] bool SameOwner(const UiGlyphAtlasUploadId id, const UiOwnershipGeneration owner) noexcept {
            return id.IsValid() && id.ownership == owner;
        }

        [[nodiscard]] bool SameOwner(const UiGlyphAtlasFrameId id, const UiOwnershipGeneration owner) noexcept {
            return id.IsValid() && id.ownership == owner;
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

    struct UiGlyphAtlas::Storage final {
        enum class EntryState : std::uint8_t {
            Free,
            Pending,
            Resident,
            Failed,
        };

        struct EntryRecord final {
            UiGlyphAtlasEntryId id;
            UiGlyphAtlasGlyphKey key;
            UiGlyphAtlasPlacement placement;
            EntryState state{EntryState::Free};
            std::uint32_t pinCount{};
            std::uint32_t uploadSlot{};
            std::uint64_t lastUsed{};
            std::uint32_t generation{};
            bool everUsed{};
            bool fallback{};
        };

        struct UploadRecord final {
            UiGlyphAtlasUploadId id;
            UiGlyphAtlasUploadDescriptor descriptor;
            UiGlyphAtlasUploadState state{UiGlyphAtlasUploadState::Retired};
            UiGlyphAtlasCompletionPoint completion;
            std::optional<Error> failure;
            std::size_t stagingOffset{};
            std::size_t stagingBytes{};
            std::uint32_t entrySlot{};
            std::uint32_t generation{};
            bool occupied{};
        };

        struct FrameRecord final {
            UiGlyphAtlasFrameId id;
            std::vector<std::uint32_t> entries;
            std::uint32_t generation{};
            bool everUsed{};
            bool active{};
        };

        UiGlyphAtlasDescriptor descriptor;
        UiGlyphAtlasState lifecycle{UiGlyphAtlasState::Active};
        UiGlyphAtlasRevision revision;
        std::uint32_t tilesPerPage{};
        std::uint32_t columnsPerPage{};
        std::vector<UiGlyphAtlasPageId> pages;
        std::vector<EntryRecord> entries;
        std::vector<UploadRecord> uploads;
        std::vector<FrameRecord> frames;
        std::vector<std::byte> staging;
        std::size_t pendingBytes{};
        std::size_t nextUpload{};
        std::size_t nextFrame{};
        std::uint64_t accessSequence{};
        std::uint64_t evictionCount{};
        std::uint64_t pressureCount{};

        explicit Storage(const UiGlyphAtlasDescriptor &source) : descriptor(source), revision(source.initialRevision) {
            columnsPerPage = source.pageExtent.width / source.tileExtent.width;
            tilesPerPage = columnsPerPage * (source.pageExtent.height / source.tileExtent.height);
            pages.reserve(source.limits.pages);
            for (std::uint32_t page = 0; page < source.limits.pages; ++page)
                pages.push_back({source.ownership, page + 1U, 1U});

            const auto entryCount = static_cast<std::size_t>(source.limits.pages) * tilesPerPage;
            entries.resize(entryCount);
            uploads.resize(source.limits.maximumUploads);
            frames.resize(source.limits.maximumFramesInFlight);
            staging.resize(source.limits.maximumPendingUploadBytes);
            for (auto &frame : frames)
                frame.entries.reserve(source.limits.maximumUsesPerFrame);
        }

        [[nodiscard]] bool IsActive() const noexcept {
            return lifecycle == UiGlyphAtlasState::Active;
        }

        [[nodiscard]] Result<std::size_t> UploadIndex(const UiGlyphAtlasUploadId id) const {
            if (!SameOwner(id, descriptor.ownership) || id.slot > uploads.size())
                return Failure<std::size_t>(UiErrors::GlyphAtlasUploadStale);
            const auto index = static_cast<std::size_t>(id.slot - 1U);
            const auto &upload = uploads[index];
            if (!upload.occupied || upload.id != id)
                return Failure<std::size_t>(UiErrors::GlyphAtlasUploadStale);
            return Result<std::size_t>::Success(index);
        }

        [[nodiscard]] Result<std::size_t> FrameIndex(const UiGlyphAtlasFrameId id) const {
            if (!SameOwner(id, descriptor.ownership) || id.slot > frames.size())
                return Failure<std::size_t>(UiErrors::GlyphAtlasFrameInvalid);
            const auto index = static_cast<std::size_t>(id.slot - 1U);
            const auto &frame = frames[index];
            if (!frame.active || frame.id != id)
                return Failure<std::size_t>(UiErrors::GlyphAtlasFrameInvalid);
            return Result<std::size_t>::Success(index);
        }

        [[nodiscard]] std::optional<std::size_t> FindEntry(const UiGlyphAtlasGlyphKey &key) const {
            for (std::size_t index = 0; index < entries.size(); ++index)
                if (entries[index].state != EntryState::Free && entries[index].key == key)
                    return index;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::size_t> FindFreeEntry() const {
            for (std::size_t index = 0; index < entries.size(); ++index)
                if (entries[index].state == EntryState::Free &&
                    (!entries[index].everUsed || entries[index].generation != std::numeric_limits<std::uint32_t>::max()))
                    return index;
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::size_t> FindFreeUpload() const {
            for (std::size_t offset = 0; offset < uploads.size(); ++offset) {
                const auto index = (nextUpload + offset) % uploads.size();
                if (!uploads[index].occupied)
                    return index;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::size_t> FindFreeFrame() const {
            for (std::size_t offset = 0; offset < frames.size(); ++offset) {
                const auto index = (nextFrame + offset) % frames.size();
                if (!frames[index].active)
                    return index;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<std::size_t> FindEvictable() const {
            std::optional<std::size_t> selected;
            for (std::size_t index = 0; index < entries.size(); ++index) {
                const auto &entry = entries[index];
                if (entry.state != EntryState::Resident || entry.fallback || entry.pinCount != 0)
                    continue;
                if (!selected.has_value() || entry.lastUsed < entries[*selected].lastUsed ||
                    (entry.lastUsed == entries[*selected].lastUsed && index < *selected))
                    selected = index;
            }
            return selected;
        }

        void ReleaseEntry(const std::size_t index) noexcept {
            auto &entry = entries[index];
            entry.key = {};
            entry.placement = {};
            entry.state = EntryState::Free;
            entry.pinCount = 0;
            entry.uploadSlot = 0;
            entry.lastUsed = 0;
            entry.fallback = false;
        }

        void EvictEntry(const std::size_t index) noexcept {
            ReleaseEntry(index);
            ++evictionCount;
        }

        [[nodiscard]] bool AllocateStaging(const std::size_t bytes, std::size_t &offset) const noexcept {
            if (bytes == 0 || bytes > staging.size())
                return false;
            std::size_t candidate = 0;
            while (candidate <= staging.size() - bytes) {
                std::size_t nextCandidate = candidate;
                bool overlaps = false;
                for (const auto &upload : uploads) {
                    if (!upload.occupied || upload.stagingBytes == 0)
                        continue;
                    if (candidate < upload.stagingOffset + upload.stagingBytes && upload.stagingOffset < candidate + bytes) {
                        overlaps = true;
                        nextCandidate = std::max(nextCandidate, upload.stagingOffset + upload.stagingBytes);
                    }
                }
                if (!overlaps) {
                    offset = candidate;
                    return true;
                }
                if (nextCandidate <= candidate)
                    return false;
                candidate = nextCandidate;
            }
            return false;
        }

        void ReleaseStaging(UploadRecord &upload) noexcept {
            if (upload.stagingBytes == 0)
                return;
            pendingBytes -= upload.stagingBytes;
            upload.stagingBytes = 0;
            upload.stagingOffset = 0;
        }

        [[nodiscard]] std::uint64_t Touch() noexcept {
            if (accessSequence == std::numeric_limits<std::uint64_t>::max())
                return accessSequence;
            return ++accessSequence;
        }

        [[nodiscard]] UiGlyphAtlasPlacement Placement(const std::size_t entryIndex, const std::uint32_t width,
                                                      const std::uint32_t height) const noexcept {
            const auto pageIndex = static_cast<std::uint32_t>(entryIndex / tilesPerPage);
            const auto tileIndex = static_cast<std::uint32_t>(entryIndex % tilesPerPage);
            const auto x = (tileIndex % columnsPerPage) * descriptor.tileExtent.width;
            const auto y = (tileIndex / columnsPerPage) * descriptor.tileExtent.height;
            const float pageWidth = static_cast<float>(descriptor.pageExtent.width);
            const float pageHeight = static_cast<float>(descriptor.pageExtent.height);
            return {pages[pageIndex],
                    {x, y, width, height},
                    {static_cast<float>(x) / pageWidth, static_cast<float>(y) / pageHeight, static_cast<float>(x + width) / pageWidth,
                     static_cast<float>(y + height) / pageHeight}};
        }

        [[nodiscard]] Result<std::size_t> AcquireEntry() {
            const auto free = FindFreeEntry();
            if (!free.has_value())
                return Failure<std::size_t>(UiErrors::GlyphAtlasCapacityExceeded);
            auto &entry = entries[*free];
            if (!entry.everUsed) {
                entry.everUsed = true;
                entry.generation = 1;
            } else {
                if (entry.generation == std::numeric_limits<std::uint32_t>::max())
                    return Failure<std::size_t>(UiErrors::GlyphAtlasCapacityExceeded);
                ++entry.generation;
            }
            entry.id = {descriptor.ownership, static_cast<std::uint32_t>(*free + 1U), entry.generation};
            return Result<std::size_t>::Success(*free);
        }

        [[nodiscard]] Result<std::size_t> AcquireUploadEntry() {
            auto entryIndex = FindFreeEntry();
            std::uint32_t evictions{};
            while (!entryIndex.has_value() && evictions < descriptor.limits.maximumEvictionsPerRequest) {
                const auto candidate = FindEvictable();
                if (!candidate.has_value())
                    break;
                EvictEntry(*candidate);
                ++evictions;
                entryIndex = FindFreeEntry();
            }
            if (!entryIndex.has_value()) {
                ++pressureCount;
                return Failure<std::size_t>(UiErrors::GlyphAtlasPressure);
            }
            return AcquireEntry();
        }

        void PublishUpload(const std::size_t uploadIndex, const std::size_t entryIndex, const UiGlyphAtlasRasterData &raster,
                           const std::size_t stagingOffset) noexcept {
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
        }

        [[nodiscard]] Result<std::size_t> AcquireUpload() {
            const auto free = FindFreeUpload();
            if (!free.has_value())
                return Failure<std::size_t>(UiErrors::GlyphAtlasUploadCapacityExceeded);
            auto &upload = uploads[*free];
            if (!upload.occupied) {
                if (!upload.generation) {
                    upload.generation = 1;
                } else if (upload.generation == std::numeric_limits<std::uint32_t>::max()) {
                    return Failure<std::size_t>(UiErrors::GlyphAtlasUploadCapacityExceeded);
                } else {
                    ++upload.generation;
                }
            }
            upload.id = {descriptor.ownership, static_cast<std::uint32_t>(*free + 1U), upload.generation};
            upload.occupied = true;
            return Result<std::size_t>::Success(*free);
        }

        [[nodiscard]] Result<std::size_t> AcquireFrame() {
            const auto free = FindFreeFrame();
            if (!free.has_value())
                return Failure<std::size_t>(UiErrors::GlyphAtlasFrameInFlight);
            auto &frame = frames[*free];
            if (!frame.everUsed) {
                frame.everUsed = true;
                frame.generation = 1;
            } else {
                if (frame.generation == std::numeric_limits<std::uint32_t>::max())
                    return Failure<std::size_t>(UiErrors::GlyphAtlasFrameInFlight);
                ++frame.generation;
            }
            frame.id = {descriptor.ownership, static_cast<std::uint32_t>(*free + 1U), frame.generation};
            frame.entries.clear();
            frame.active = true;
            nextFrame = (*free + 1U) % frames.size();
            return Result<std::size_t>::Success(*free);
        }

        [[nodiscard]] Result<void> Pin(const std::size_t frameIndex, const std::size_t entryIndex) {
            auto &frame = frames[frameIndex];
            if (std::ranges::find(frame.entries, static_cast<std::uint32_t>(entryIndex)) != frame.entries.end())
                return Result<void>::Success();
            if (frame.entries.size() >= descriptor.limits.maximumUsesPerFrame)
                return Failure(UiErrors::GlyphAtlasFrameCapacityExceeded);
            frame.entries.push_back(static_cast<std::uint32_t>(entryIndex));
            ++entries[entryIndex].pinCount;
            entries[entryIndex].lastUsed = Touch();
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CancelPendingUpload(UploadRecord &upload) {
            if (upload.entrySlot != 0) {
                const auto entryIndex = static_cast<std::size_t>(upload.entrySlot - 1U);
                if (entryIndex < entries.size() && entries[entryIndex].uploadSlot == upload.id.slot)
                    ReleaseEntry(entryIndex);
                upload.entrySlot = 0;
            }
            ReleaseStaging(upload);
            upload.state = UiGlyphAtlasUploadState::Cancelled;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> FreeUpload(const std::size_t index) {
            auto &upload = uploads[index];
            if (!upload.occupied)
                return Failure(UiErrors::GlyphAtlasUploadStale);
            upload.failure.reset();
            upload.descriptor = {};
            upload.completion = {};
            upload.stagingOffset = 0;
            upload.stagingBytes = 0;
            upload.entrySlot = 0;
            upload.state = UiGlyphAtlasUploadState::Retired;
            upload.occupied = false;
            return Result<void>::Success();
        }

        void ResetUploads(UiGlyphAtlasResetReport &report) noexcept {
            for (std::size_t index = 0; index < uploads.size(); ++index) {
                auto &upload = uploads[index];
                if (!upload.occupied)
                    continue;
                if (upload.state == UiGlyphAtlasUploadState::Pending)
                    ++report.cancelledUploads;
                if (upload.entrySlot != 0) {
                    const auto entryIndex = static_cast<std::size_t>(upload.entrySlot - 1U);
                    if (entryIndex < entries.size())
                        ReleaseEntry(entryIndex);
                }
                ReleaseStaging(upload);
                static_cast<void>(FreeUpload(index));
            }
        }

        void ResetEntries() noexcept {
            for (std::size_t index = 0; index < entries.size(); ++index)
                ReleaseEntry(index);
        }

        void AdvancePageGenerations() noexcept {
            for (std::size_t index = 0; index < pages.size(); ++index) {
                ++pages[index].generation;
                pages[index] = {descriptor.ownership, static_cast<std::uint32_t>(index + 1U), pages[index].generation};
            }
        }

        [[nodiscard]] Result<UiGlyphAtlasResetReport> ResetInternal(const UiGlyphAtlasResetReason reason) {
            if (!IsActive())
                return Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasLifecycleUnavailable);
            if (!IsKnown(reason))
                return Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasResetInvalid);
            if (std::ranges::any_of(frames, [](const FrameRecord &frame) {
                return frame.active;
            }))
                return Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasResetBusy);
            if (std::ranges::any_of(uploads, [](const UploadRecord &upload) {
                return upload.occupied &&
                       (upload.state == UiGlyphAtlasUploadState::Submitted ||
                        (upload.state == UiGlyphAtlasUploadState::Cancelled && (upload.entrySlot != 0 || upload.stagingBytes != 0)));
            }))
                return Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasUploadInFlight);

            const auto nextRevision = revision.Next();
            if (nextRevision.HasError())
                return Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasResetInvalid);
            for (const auto page : pages)
                if (page.generation == std::numeric_limits<std::uint32_t>::max())
                    return Failure<UiGlyphAtlasResetReport>(UiErrors::GlyphAtlasResetInvalid);

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

        [[nodiscard]] bool IsDrained() const noexcept {
            return !std::ranges::any_of(frames, [](const FrameRecord &frame) {
                return frame.active;
            }) && !std::ranges::any_of(uploads, [](const UploadRecord &upload) {
                return upload.occupied &&
                       (upload.state == UiGlyphAtlasUploadState::Pending || upload.state == UiGlyphAtlasUploadState::Submitted ||
                        (upload.state == UiGlyphAtlasUploadState::Cancelled && (upload.entrySlot != 0 || upload.stagingBytes != 0)));
            });
        }
    };

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
        if (!raster.IsValid() || raster.format != storage_->descriptor.format || raster.width > storage_->descriptor.tileExtent.width ||
            raster.height > storage_->descriptor.tileExtent.height ||
            raster.bytes.size() > storage_->descriptor.limits.maximumPendingUploadBytes)
            return Failure<UiGlyphAtlasUploadId>(UiErrors::GlyphAtlasUploadInvalid);
        if (const auto existing = storage_->FindEntry(raster.key); existing.has_value())
            return Failure<UiGlyphAtlasUploadId>(UiErrors::GlyphAtlasUploadInvalidTransition);

        const auto uploadIndex = storage_->AcquireUpload();
        if (uploadIndex.HasError())
            return Result<UiGlyphAtlasUploadId>::Failure(uploadIndex.ErrorValue());

        std::size_t stagingOffset{};
        if (storage_->pendingBytes > storage_->descriptor.limits.maximumPendingUploadBytes - raster.bytes.size() ||
            !storage_->AllocateStaging(raster.bytes.size(), stagingOffset)) {
            storage_->uploads[uploadIndex.Value()].occupied = false;
            return Failure<UiGlyphAtlasUploadId>(UiErrors::GlyphAtlasUploadCapacityExceeded);
        }

        const auto acquiredEntry = storage_->AcquireUploadEntry();
        if (acquiredEntry.HasError()) {
            storage_->uploads[uploadIndex.Value()].occupied = false;
            return Result<UiGlyphAtlasUploadId>::Failure(acquiredEntry.ErrorValue());
        }
        storage_->PublishUpload(uploadIndex.Value(), acquiredEntry.Value(), raster, stagingOffset);
        return Result<UiGlyphAtlasUploadId>::Success(storage_->uploads[uploadIndex.Value()].id);
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
