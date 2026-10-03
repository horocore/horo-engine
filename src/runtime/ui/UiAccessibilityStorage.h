#pragma once

#include "Horo/Runtime/Ui/UiAccessibility.h"
#include "Horo/Runtime/Ui/UiFocusGraph.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Runtime::Ui::AccessibilityInternal {
    struct ProjectionLookupEntry final {
        UiElementId element;
        std::uint32_t index{};
    };

    using ProjectionLookup = std::span<const ProjectionLookupEntry>;

    [[nodiscard]] std::size_t FindProjectionIndex(ProjectionLookup lookup, UiElementId element) noexcept;
}  // namespace Horo::Runtime::Ui::AccessibilityInternal

namespace Horo::Runtime::Ui {
    struct UiAccessibilitySnapshot::Storage final {
        mutable std::atomic<std::uint64_t> leases{};
        UiAccessibilitySnapshotDescriptor descriptor;
        std::vector<UiAccessibilityNode> nodes;
        std::vector<UiAccessibilityRelation> relations;
        std::vector<UiAccessibilityAction> actions;
        std::vector<char> text;
        std::vector<UiAccessibilityNodeId> readingOrder;
        std::vector<UiAccessibilityNodeId> focusOrder;
        std::optional<UiFocusSnapshot> focusState;

        explicit Storage(const UiAccessibilityLimits &limits);

        void ResolveParents(const UiElementTree &tree, AccessibilityInternal::ProjectionLookup lookup);
        void Publish(const UiElementTree &tree, const UiAccessibilitySnapshotDescriptor &sourceDescriptor,
                     const UiAccessibilityProjection &projection, AccessibilityInternal::ProjectionLookup lookup);
        /** @brief Copies bounded authoritative focus evidence into the unpublished snapshot slot. */
        void PublishFocus(std::span<const UiFocusTarget> order, const UiFocusSnapshot *state);
    };

    struct UiAccessibilityExtractor::Storage final {
        UiAccessibilityExtractorDescriptor descriptor;
        UiAccessibilityExtractorState lifecycle{UiAccessibilityExtractorState::Active};
        std::vector<std::shared_ptr<UiAccessibilitySnapshot::Storage>> slots;
        std::size_t nextSlot{};
        UiAccessibilitySemanticRevision lastRevision;
        std::vector<std::uint8_t> cycleScratch;
        std::vector<AccessibilityInternal::ProjectionLookupEntry> lookupScratch;
        std::vector<UiElementHandle> preorderScratch;
        std::vector<UiAccessibilityNodeInput> readingScratch;
        std::vector<UiAccessibilityNodeInput> focusProjectionScratch;
        std::vector<UiFocusTarget> focusOrderScratch;
        std::optional<UiFocusScope> focusScope;
        /** @brief Validates a complete candidate and prepares retained reading/relationship evidence before leasing a slot. */
        [[nodiscard]] Result<void> PrepareReading(const UiElementTree &tree, const UiAccessibilitySnapshotDescriptor &source,
                                                  const UiAccessibilityProjection &projection);

        explicit Storage(const UiAccessibilityExtractorDescriptor &source);

        std::shared_ptr<UiAccessibilitySnapshot::Storage> TryAcquire() noexcept;
        [[nodiscard]] bool IsDrained() const noexcept;
    };
}  // namespace Horo::Runtime::Ui
