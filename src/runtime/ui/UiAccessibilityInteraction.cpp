#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiAccessibilityReading.h"
#include "UiAccessibilityStorage.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Requires exact published interaction lineage, rather than independently supplied focus flags. */
        bool SameSource(const UiAccessibilitySnapshotDescriptor &semantic, const UiFocusOwnerContext &focus) noexcept {
            return semantic.instance == focus.instance && semantic.canvas == focus.canvas && semantic.document == focus.document &&
                   semantic.documentRevision == focus.documentRevision && semantic.treeRevision == focus.treeRevision &&
                   semantic.interactionRevision == focus.interaction;
        }

        /** @brief Validates a graph target against actual tree residency, ancestry and semantic participation. */
        Result<void> ValidateTarget(const UiElementTree &tree, const UiFocusTarget &target, const UiAccessibilityNodeInput &node,
                                    const UiElementHandle modalRoot) {
            auto record = tree.Get(target.element);
            if (record.HasError() || record.Value().id != target.id)
                return Result<void>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
            if (node.state.Has(UiAccessibilityStateFlag::Disabled))
                return Result<void>::Failure(MakeError(UiErrors::AccessibilityStateInvalid));
            bool within = !modalRoot.IsValid();
            auto handle = target.element;
            while (handle.IsValid()) {
                within = within || handle == modalRoot;
                handle = tree.Get(handle).Value().parent;
            }
            return within ? Result<void>::Success() : Result<void>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
        }

        /** @brief Prepares a sorted semantic index and replaces caller focus without allocating. */
        void PrepareProjection(const std::span<const UiAccessibilityNodeInput> nodes, const std::optional<UiFocusTarget> focused,
                               std::vector<UiAccessibilityNodeInput> &candidate,
                               std::vector<AccessibilityInternal::ProjectionLookupEntry> &lookup) {
            candidate.assign(nodes.begin(), nodes.end());
            lookup.clear();
            for (std::size_t index = 0; index < candidate.size(); ++index) {
                auto &node = candidate[index];
                node.state.flags &= ~(static_cast<std::uint32_t>(UiAccessibilityStateFlag::Focused) |
                                      static_cast<std::uint32_t>(UiAccessibilityStateFlag::Focusable));
                if (focused && focused->id == node.element)
                    node.state.flags |= static_cast<std::uint32_t>(UiAccessibilityStateFlag::Focused);
                lookup.push_back({node.element, static_cast<std::uint32_t>(index)});
            }
            std::ranges::sort(lookup, {}, &AccessibilityInternal::ProjectionLookupEntry::element);
        }
    }  // namespace

    /** @copydoc UiAccessibilityExtractor::Extract */
    Result<UiAccessibilitySnapshot> UiAccessibilityExtractor::Extract(const UiElementTree &tree,
                                                                      const UiAccessibilitySnapshotDescriptor &descriptor,
                                                                      const UiAccessibilityProjection &projection,
                                                                      const UiFocusGraph &focus) {
        if (!storage_ || storage_->lifecycle != UiAccessibilityExtractorState::Active)
            return Result<UiAccessibilitySnapshot>::Failure(MakeError(UiErrors::AccessibilityLifecycleUnavailable));
        const auto state = focus.Snapshot();
        if (state.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(state.ErrorValue());
        if (!SameSource(descriptor, state.Value().owner) || (storage_->focusScope && *storage_->focusScope != state.Value().owner.scope))
            return Result<UiAccessibilitySnapshot>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
        if (projection.nodes.size() > storage_->descriptor.limits.nodes)
            return Result<UiAccessibilitySnapshot>::Failure(MakeError(UiErrors::CapacityExceeded));
        const auto order = focus.Order(storage_->focusOrderScratch);
        if (order.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(order.ErrorValue());
        const auto modal = state.Value().modalRoot ? state.Value().modalRoot->element : UiElementHandle{};
        PrepareProjection(projection.nodes, state.Value().focused, storage_->focusProjectionScratch, storage_->lookupScratch);
        for (const auto &target : std::span{storage_->focusOrderScratch}.first(order.Value())) {
            const auto index = AccessibilityInternal::FindProjectionIndex(storage_->lookupScratch, target.id);
            if (index == std::numeric_limits<std::size_t>::max())
                return Result<UiAccessibilitySnapshot>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
            auto &node = storage_->focusProjectionScratch[index];
            if (const auto valid = ValidateTarget(tree, target, node, modal); valid.HasError())
                return Result<UiAccessibilitySnapshot>::Failure(valid.ErrorValue());
            node.state.flags |= static_cast<std::uint32_t>(UiAccessibilityStateFlag::Focusable);
        }
        // Validate exposure after inherited/modal policy, before acquiring or publishing any immutable slot.
        if (const auto reading =
                AccessibilityInternal::BuildReadingProjection(tree, {storage_->focusProjectionScratch, modal}, storage_->lookupScratch,
                                                              storage_->preorderScratch, storage_->readingScratch);
            reading.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(reading.ErrorValue());
        for (const auto &target : std::span{storage_->focusOrderScratch}.first(order.Value())) {
            const auto found = std::ranges::find(storage_->readingScratch, target.id, &UiAccessibilityNodeInput::element);
            if (found == storage_->readingScratch.end() ||
                (found->exposure != UiAccessibilityExposure::Visible && found->exposure != UiAccessibilityExposure::Offscreen))
                return Result<UiAccessibilitySnapshot>::Failure(MakeError(UiErrors::AccessibilityStateInvalid));
        }
        auto result = ExtractPrepared(tree, descriptor, {storage_->focusProjectionScratch, modal},
                                      std::span{storage_->focusOrderScratch}.first(order.Value()), &state.Value());
        if (result.HasError())
            return result;
        storage_->focusScope = state.Value().owner.scope;
        return result;
    }
}  // namespace Horo::Runtime::Ui
