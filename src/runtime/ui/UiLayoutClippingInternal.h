#pragma once

#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiLayoutClipping.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace UiLayoutClippingInternal {
        inline constexpr std::uint32_t NoIndex = std::numeric_limits<std::uint32_t>::max();
        inline constexpr std::int64_t MinimumScalar = std::numeric_limits<std::int32_t>::min();
        inline constexpr std::int64_t MaximumScalar = std::numeric_limits<std::int32_t>::max();

        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnown(UiLayoutOverflowPolicy policy) noexcept;
        [[nodiscard]] bool SameOwner(RuntimeUiInstanceId instance, UiCanvasInstanceId canvas) noexcept;
        [[nodiscard]] Result<std::int32_t> CheckedCast(std::int64_t value);
        [[nodiscard]] Result<UiLogicalPoint> Add(UiLogicalPoint first, UiLogicalPoint second);
        [[nodiscard]] Result<UiLogicalPoint> Negate(UiLogicalPoint value);
        [[nodiscard]] std::int64_t Right(const UiLogicalRect &rect) noexcept;
        [[nodiscard]] std::int64_t Bottom(const UiLogicalRect &rect) noexcept;
        [[nodiscard]] Result<UiLogicalRect> Translate(UiLogicalRect rect, UiLogicalPoint translation);
        [[nodiscard]] Result<UiLogicalRect> Union(UiLogicalRect first, UiLogicalRect second);
        [[nodiscard]] Result<std::int32_t> ClampOffset(std::int64_t value, std::int32_t minimum, std::int32_t maximum);
        [[nodiscard]] Result<std::int64_t> DesiredOffset(UiFocusBringIntoViewPolicy policy, UiLogicalRect target, UiLogicalRect viewport,
                                                         bool horizontal);
        [[nodiscard]] Result<UiLogicalPoint> OffsetDelta(UiLogicalPoint value);
    }  // namespace UiLayoutClippingInternal

    struct UiLayoutClipSnapshot::Storage final {
        mutable std::atomic<std::uint64_t> leases{};
        UiLayoutClipSnapshotDescriptor descriptor;
        std::vector<UiLayoutClipRecord> records;
        std::vector<UiLayoutClipNode> clips;
        std::vector<UiLayoutScrollRecord> scrolls;
        std::vector<std::uint32_t> recordLookup;

        explicit Storage(const UiLayoutClipEngineDescriptor &source);
    };

    struct UiLayoutClipEngine::Storage final {
        UiLayoutClipEngineDescriptor descriptor;
        UiLayoutClipEngineState lifecycle{UiLayoutClipEngineState::Active};
        std::vector<std::shared_ptr<UiLayoutClipSnapshot::Storage>> slots;
        std::shared_ptr<UiLayoutClipSnapshot::Storage> current;
        std::size_t nextSlot{};

        std::vector<std::uint32_t> recordLookup;
        std::vector<std::uint32_t> parents;
        std::vector<std::uint32_t> clipIndexes;
        std::vector<std::uint32_t> ownClipIndexes;
        std::vector<std::uint32_t> scrollIndexes;
        std::vector<std::uint32_t> path;
        std::vector<UiLogicalPoint> translations;
        std::vector<UiLogicalPoint> offsets;
        std::vector<UiLogicalPoint> minimumOffsets;
        std::vector<UiLogicalPoint> maximumOffsets;
        std::vector<UiLogicalRect> viewports;
        std::vector<UiLogicalRect> contents;
        std::vector<UiLayoutClipRecord> candidateRecords;
        std::vector<UiLayoutClipNode> candidateClips;
        std::vector<UiLayoutScrollRecord> candidateScrolls;

        explicit Storage(const UiLayoutClipEngineDescriptor &source);
        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;
        ~Storage();

        void ReleaseCurrent() noexcept;
        [[nodiscard]] std::shared_ptr<UiLayoutClipSnapshot::Storage> TryAcquire() noexcept;
        [[nodiscard]] std::uint32_t FindRecord(std::span<const UiLayoutRecord> records, UiElementHandle element) const noexcept;
        [[nodiscard]] Result<void> BuildParentIndex(const UiElementTree &tree, std::span<const UiLayoutRecord> records);
        [[nodiscard]] Result<void> BuildScrollBounds(std::span<const UiLayoutRecord> records,
                                                     std::span<const UiLayoutClipDescriptor> descriptors);
        [[nodiscard]] Result<void> ValidateUpdate(const UiElementTree &tree, const UiLayoutSnapshotDescriptor &source,
                                                  std::span<const UiLayoutRecord> records, const UiLayoutClipUpdateRequest &request) const;
        [[nodiscard]] Result<void> ValidateSource(const UiElementTree &tree, const UiLayoutSnapshotDescriptor &source,
                                                  std::span<const UiLayoutRecord> records, const UiLayoutClipUpdateRequest &request) const;
        [[nodiscard]] Result<void> ValidateElements(std::span<const UiLayoutRecord> records,
                                                    const UiLayoutClipUpdateRequest &request) const;
        [[nodiscard]] Result<void> ValidateBringIntoView(const UiElementTree &tree, const UiLayoutSnapshotDescriptor &source,
                                                         const std::optional<UiFocusBringIntoViewRequest> &request) const;
        [[nodiscard]] Result<void> BuildRevealPath(std::uint32_t target);
        [[nodiscard]] Result<void> SetRevealOffset(std::uint32_t scrollElement, UiFocusBringIntoViewPolicy policy, UiLogicalRect target);
        [[nodiscard]] Result<void> RevealScrollContainer(std::span<const UiLayoutRecord> records, std::size_t pathIndex,
                                                         std::uint32_t target, UiFocusBringIntoViewPolicy policy,
                                                         UiLogicalPoint innerTranslation);
        [[nodiscard]] Result<void> ApplyBringIntoView(std::span<const UiLayoutRecord> records, const UiFocusBringIntoViewRequest &request);
        [[nodiscard]] Result<void> BuildParentProjection(std::uint32_t index);
        [[nodiscard]] Result<void> BuildClipProjection(std::span<const UiLayoutRecord> records,
                                                       std::span<const UiLayoutClipDescriptor> descriptors, std::uint32_t index);
        [[nodiscard]] Result<void> BuildScrollProjection(std::uint32_t index);
        [[nodiscard]] Result<void> BuildProjectionRecord(std::span<const UiLayoutRecord> records,
                                                         std::span<const UiLayoutClipDescriptor> descriptors, std::uint32_t index);
        [[nodiscard]] Result<void> BuildProjection(std::span<const UiLayoutRecord> records,
                                                   std::span<const UiLayoutClipDescriptor> descriptors);
        [[nodiscard]] Result<void> ValidateProjection(std::size_t recordCount) const;
        [[nodiscard]] Result<std::shared_ptr<UiLayoutClipSnapshot::Storage>> Publish(const UiLayoutSnapshotDescriptor &source);
    };
}  // namespace Horo::Runtime::Ui
