#pragma once

/**
 * @file UiGlyphAtlasStorage.h
 * @brief Target-private bounded storage for the Runtime UI glyph atlas.
 */

#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiGlyphAtlas.h"

#include <algorithm>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace Horo::Runtime::Ui {
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
            UiGlyphAtlasFrameId lastPinnedFrame;
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

        struct KeyIndex final {
            UiGlyphAtlasGlyphKey key;
            std::size_t entry{};
        };

        UiGlyphAtlasDescriptor descriptor;
        UiGlyphAtlasState lifecycle{UiGlyphAtlasState::Active};
        UiGlyphAtlasRevision revision;
        std::uint32_t tilesPerPage{};
        std::uint32_t columnsPerPage{};
        std::vector<UiGlyphAtlasPageId> pages;
        std::vector<EntryRecord> entries;
        std::vector<KeyIndex> entriesByKey;
        std::vector<std::size_t> freeEntries;
        std::vector<UploadRecord> uploads;
        std::vector<FrameRecord> frames;
        std::vector<std::byte> staging;
        std::size_t pendingBytes{};
        std::size_t nextUpload{};
        std::size_t nextFrame{};
        std::uint64_t accessSequence{};
        std::uint64_t evictionCount{};
        std::uint64_t pressureCount{};

        template <typename T = void> [[nodiscard]] static Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] static bool IsKnown(const UiGlyphAtlasResetReason value) noexcept {
            return value < UiGlyphAtlasResetReason::Count;
        }

        [[nodiscard]] static bool SameOwner(const UiGlyphAtlasUploadId id, const UiOwnershipGeneration owner) noexcept {
            return id.IsValid() && id.ownership == owner;
        }

        [[nodiscard]] static bool SameOwner(const UiGlyphAtlasFrameId id, const UiOwnershipGeneration owner) noexcept {
            return id.IsValid() && id.ownership == owner;
        }

        explicit Storage(const UiGlyphAtlasDescriptor &source) : descriptor(source), revision(source.initialRevision) {
            columnsPerPage = source.pageExtent.width / source.tileExtent.width;
            tilesPerPage = columnsPerPage * (source.pageExtent.height / source.tileExtent.height);
            pages.reserve(source.limits.pages);
            for (std::uint32_t page = 0; page < source.limits.pages; ++page)
                pages.push_back({source.ownership, page + 1U, 1U});

            const auto entryCount = static_cast<std::size_t>(source.limits.pages) * tilesPerPage;
            entries.resize(entryCount);
            entriesByKey.reserve(entryCount);
            freeEntries.reserve(entryCount);
            for (std::size_t index = entryCount; index > 0; --index)
                freeEntries.push_back(index - 1U);
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
            const auto position = std::lower_bound(entriesByKey.begin(), entriesByKey.end(), key,
                                                   [](const KeyIndex &record, const UiGlyphAtlasGlyphKey &requested) {
                return record.key < requested;
            });
            if (position == entriesByKey.end() || position->key != key)
                return std::nullopt;
            return position->entry;
        }

        [[nodiscard]] std::optional<std::size_t> FindFreeEntry() const {
            for (auto position = freeEntries.rbegin(); position != freeEntries.rend(); ++position) {
                const auto &entry = entries[*position];
                if (!entry.everUsed || entry.generation != std::numeric_limits<std::uint32_t>::max())
                    return *position;
            }
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
                if ((entry.state != EntryState::Resident && entry.state != EntryState::Failed) || entry.fallback || entry.pinCount != 0)
                    continue;
                if (!selected.has_value() || entry.lastUsed < entries[*selected].lastUsed ||
                    (entry.lastUsed == entries[*selected].lastUsed && index < *selected))
                    selected = index;
            }
            return selected;
        }

        void EraseEntryKey(const UiGlyphAtlasGlyphKey &key) noexcept {
            const auto position = std::lower_bound(entriesByKey.begin(), entriesByKey.end(), key,
                                                   [](const KeyIndex &record, const UiGlyphAtlasGlyphKey &requested) {
                return record.key < requested;
            });
            if (position != entriesByKey.end() && position->key == key)
                entriesByKey.erase(position);
        }

        void ReleaseEntry(const std::size_t index) noexcept {
            auto &entry = entries[index];
            if (entry.state == EntryState::Free)
                return;
            EraseEntryKey(entry.key);
            entry.key = {};
            entry.placement = {};
            entry.lastPinnedFrame = {};
            entry.state = EntryState::Free;
            entry.pinCount = 0;
            entry.uploadSlot = 0;
            entry.lastUsed = 0;
            entry.fallback = false;
            freeEntries.push_back(index);
        }

        void ReleaseFailedUpload(const std::size_t entryIndex) noexcept {
            const auto &entry = entries[entryIndex];
            if (entry.state != EntryState::Failed || entry.uploadSlot == 0)
                return;
            const auto uploadIndex = static_cast<std::size_t>(entry.uploadSlot - 1U);
            if (uploadIndex >= uploads.size())
                return;
            auto &upload = uploads[uploadIndex];
            if (upload.occupied && upload.state == UiGlyphAtlasUploadState::Failed && upload.entrySlot == entry.uploadSlot)
                static_cast<void>(FreeUpload(uploadIndex));
        }

        void EvictEntry(const std::size_t index) noexcept {
            ReleaseFailedUpload(index);
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
            while (!freeEntries.empty()) {
                const auto index = freeEntries.back();
                freeEntries.pop_back();
                auto &entry = entries[index];
                if (entry.everUsed && entry.generation == std::numeric_limits<std::uint32_t>::max())
                    continue;
                if (!entry.everUsed) {
                    entry.everUsed = true;
                    entry.generation = 1;
                } else {
                    ++entry.generation;
                }
                entry.id = {descriptor.ownership, static_cast<std::uint32_t>(index + 1U), entry.generation};
                return Result<std::size_t>::Success(index);
            }
            return Failure<std::size_t>(UiErrors::GlyphAtlasCapacityExceeded);
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

        [[nodiscard]] Result<void> PublishUpload(const std::size_t uploadIndex, const std::size_t entryIndex,
                                                 const UiGlyphAtlasRasterData &raster, std::size_t stagingOffset);

        [[nodiscard]] Result<std::size_t> AcquireUpload() {
            const auto free = FindFreeUpload();
            if (!free.has_value())
                return Failure<std::size_t>(UiErrors::GlyphAtlasUploadCapacityExceeded);
            auto &upload = uploads[*free];
            if (!upload.generation) {
                upload.generation = 1;
            } else if (upload.generation == std::numeric_limits<std::uint32_t>::max()) {
                return Failure<std::size_t>(UiErrors::GlyphAtlasUploadCapacityExceeded);
            } else {
                ++upload.generation;
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
            auto &entry = entries[entryIndex];
            if (entry.lastPinnedFrame == frame.id)
                return Result<void>::Success();
            if (frame.entries.size() >= descriptor.limits.maximumUsesPerFrame)
                return Failure(UiErrors::GlyphAtlasFrameCapacityExceeded);
            frame.entries.push_back(static_cast<std::uint32_t>(entryIndex));
            entry.lastPinnedFrame = frame.id;
            ++entry.pinCount;
            entry.lastUsed = Touch();
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

        [[nodiscard]] Result<void> RecycleFailedEntry(const std::size_t index) {
            auto &entry = entries[index];
            if (entry.state != EntryState::Failed)
                return Failure(UiErrors::GlyphAtlasUploadInvalidTransition);
            ReleaseFailedUpload(index);
            ReleaseEntry(index);
            return Result<void>::Success();
        }

        void ReturnAcquiredEntry(const std::size_t index) noexcept {
            if (entries[index].state == EntryState::Free)
                freeEntries.push_back(index);
            else
                ReleaseEntry(index);
        }

        [[nodiscard]] Result<UiGlyphAtlasUploadId> AdmitUpload(const UiGlyphAtlasRasterData &raster);

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

        [[nodiscard]] Result<UiGlyphAtlasResetReport> ResetInternal(UiGlyphAtlasResetReason reason);

        [[nodiscard]] bool IsDrained() const noexcept;
    };
}  // namespace Horo::Runtime::Ui
