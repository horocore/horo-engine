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

        /** @brief Checks complete focus lineage, bound audience and semantic capacity before copying candidates. */
        Result<void> ValidateFocusSource(const UiElementTree &tree, const UiAccessibilitySnapshotDescriptor &semantic,
                                         const UiFocusSnapshot &focus, const std::optional<UiFocusScope> &audience, const std::size_t count,
                                         const std::uint32_t capacity) {
            if (!SameSource(semantic, focus.owner) || (audience && *audience != focus.owner.scope))
                return Result<void>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
            if (focus.modalRoot) {
                if (const auto root = tree.Get(focus.modalRoot->element); root.HasError() || root.Value().id != focus.modalRoot->id)
                    return Result<void>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
            }
            if (count > capacity)
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            return Result<void>::Success();
        }

        /** @brief Validates a graph target against actual tree residency, ancestry and semantic participation. */
        Result<void> ValidateTarget(const UiElementTree &tree, const UiFocusTarget &target, const UiAccessibilityNodeInput &node,
                                    const UiElementHandle modalRoot) {
            if (const auto record = tree.Get(target.element); record.HasError() || record.Value().id != target.id)
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
        void PrepareProjection(const std::span<const UiAccessibilityNodeInput> nodes, const std::optional<UiFocusTarget> &focused,
                               std::vector<UiAccessibilityNodeInput> &candidate,
                               std::vector<AccessibilityInternal::ProjectionLookupEntry> &lookup) {
            candidate.assign(nodes.begin(), nodes.end());
            lookup.clear();
            for (std::size_t index = 0; index < candidate.size(); ++index) {
                using enum UiAccessibilityStateFlag;
                auto &node = candidate[index];
                node.state.flags &= ~(static_cast<std::uint32_t>(Focused) | static_cast<std::uint32_t>(Focusable));
                if (focused && focused->id == node.element)
                    node.state.flags |= static_cast<std::uint32_t>(Focused);
                lookup.push_back({node.element, static_cast<std::uint32_t>(index)});
            }
            std::ranges::sort(lookup, {}, &AccessibilityInternal::ProjectionLookupEntry::element);
        }

        /** @brief Resolves the graph's exact current targets into the borrowed semantic candidate. */
        Result<void> ApplyTargets(const UiElementTree &tree, const std::span<const UiFocusTarget> order, const UiElementHandle modal,
                                  const AccessibilityInternal::ProjectionLookup lookup, std::vector<UiAccessibilityNodeInput> &nodes) {
            for (const auto &target : order) {
                const auto index = AccessibilityInternal::FindProjectionIndex(lookup, target.id);
                if (index == std::numeric_limits<std::size_t>::max())
                    return Result<void>::Failure(MakeError(UiErrors::AccessibilitySnapshotSourceStale));
                auto &node = nodes[index];
                if (const auto valid = ValidateTarget(tree, target, node, modal); valid.HasError())
                    return valid;
                node.state.flags |= static_cast<std::uint32_t>(UiAccessibilityStateFlag::Focusable);
            }
            return Result<void>::Success();
        }

        /** @brief Rejects graph targets excluded by inherited or modal semantic exposure. */
        Result<void> ValidateFocusExposure(const std::span<const UiFocusTarget> order,
                                           const std::span<const UiAccessibilityNodeInput> reading) {
            for (const auto &target : order) {
                const auto found = std::ranges::find(reading, target.id, &UiAccessibilityNodeInput::element);
                if (found == reading.end() ||
                    (found->exposure != UiAccessibilityExposure::Visible && found->exposure != UiAccessibilityExposure::Offscreen))
                    return Result<void>::Failure(MakeError(UiErrors::AccessibilityStateInvalid));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc UiAccessibilityExtractor::Extract */
    Result<UiAccessibilitySnapshot> UiAccessibilityExtractor::Extract(const UiElementTree &tree,
                                                                      const UiAccessibilitySnapshotDescriptor &descriptor,
                                                                      const UiAccessibilityProjection &projection,
                                                                      const UiFocusGraph &focus) {
        if (State() != UiAccessibilityExtractorState::Active)
            return Result<UiAccessibilitySnapshot>::Failure(MakeError(UiErrors::AccessibilityLifecycleUnavailable));
        const auto state = focus.Snapshot();
        if (state.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(state.ErrorValue());
        if (const auto source = ValidateFocusSource(tree, descriptor, state.Value(), storage_->focusScope, projection.nodes.size(),
                                                    storage_->descriptor.limits.nodes);
            source.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(source.ErrorValue());
        const auto order = focus.Order(storage_->focusOrderScratch);
        if (order.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(order.ErrorValue());
        const auto modal = state.Value().modalRoot ? state.Value().modalRoot->element : UiElementHandle{};
        PrepareProjection(projection.nodes, state.Value().focused, storage_->focusProjectionScratch, storage_->lookupScratch);
        const auto targets = std::span{storage_->focusOrderScratch}.first(order.Value());
        if (const auto applied = ApplyTargets(tree, targets, modal, storage_->lookupScratch, storage_->focusProjectionScratch);
            applied.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(applied.ErrorValue());
        // Validate exposure after inherited/modal policy, before acquiring or publishing any immutable slot.
        if (const auto reading =
                AccessibilityInternal::BuildReadingProjection(tree, {storage_->focusProjectionScratch, modal}, storage_->lookupScratch,
                                                              storage_->preorderScratch, storage_->readingScratch);
            reading.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(reading.ErrorValue());
        if (const auto exposure = ValidateFocusExposure(targets, storage_->readingScratch); exposure.HasError())
            return Result<UiAccessibilitySnapshot>::Failure(exposure.ErrorValue());
        auto result = ExtractPrepared(tree, descriptor, {storage_->focusProjectionScratch, modal}, targets, &state.Value());
        if (result.HasError())
            return result;
        storage_->focusScope = state.Value().owner.scope;
        return result;
    }
}  // namespace Horo::Runtime::Ui
