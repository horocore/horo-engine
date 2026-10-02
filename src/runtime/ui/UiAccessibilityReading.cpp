#include "UiAccessibilityReading.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <limits>

namespace Horo::Runtime::Ui::AccessibilityInternal {
    namespace {
        /** @brief Measures text without overflow, allocation or content scanning. */
        bool MeasureText(const std::string_view text, std::size_t &remaining) noexcept {
            if (text.size() > remaining)
                return false;
            remaining -= text.size();
            return true;
        }

        /** @brief Accounts for every text field in one bounded candidate node. */
        bool MeasureNodeText(const UiAccessibilityNodeInput &node, std::size_t &remaining) noexcept {
            if (!MeasureText(node.name.text, remaining) || !MeasureText(node.description.text, remaining) ||
                !MeasureText(node.error.message.text, remaining) ||
                (node.value.kind == UiAccessibilityValueKind::Text && !MeasureText(node.value.text.text, remaining)))
                return false;
            for (const auto &action : node.actions)
                if (!MeasureText(action.name.text, remaining))
                    return false;
            return true;
        }

        /** @brief Classifies exposure that retains readable semantics while closing interaction. */
        bool IsInactive(const UiAccessibilityExposure exposure) noexcept {
            using enum UiAccessibilityExposure;
            return exposure == Covered || exposure == Suppressed || exposure == Suspended;
        }

        /** @brief Applies ancestor policy over at most the retained tree's fixed depth bound. */
        UiAccessibilityExposure Exposure(const UiElementTree &tree, UiElementHandle parent, UiAccessibilityExposure exposure,
                                         const UiAccessibilityProjection &projection, const ProjectionLookup lookup) {
            using enum UiAccessibilityExposure;
            while (parent.IsValid()) {
                const auto ancestor = tree.Get(parent).Value();
                if (const auto index = FindProjectionIndex(lookup, ancestor.id); index != std::numeric_limits<std::size_t>::max()) {
                    const auto policy = projection.nodes[index].exposure;
                    if (policy == Hidden)
                        return policy;
                    if (IsInactive(policy) || (policy == Offscreen && exposure == Visible))
                        exposure = policy;
                }
                parent = ancestor.parent;
            }
            return exposure;
        }

        /** @brief Rejects focus that contradicts readable but disabled/inactive semantics. */
        bool InvalidFocus(const UiAccessibilityNodeInput &node) noexcept {
            return node.state.Has(UiAccessibilityStateFlag::Focused) &&
                   (node.state.Has(UiAccessibilityStateFlag::Disabled) || IsInactive(node.exposure));
        }
    }  // namespace

    /** @copydoc ValidateInputBounds */
    Result<void> ValidateInputBounds(const UiAccessibilityProjection &projection, const UiAccessibilityLimits &limits) {
        std::size_t relations = limits.relations;
        std::size_t actions = limits.actions;
        std::size_t text = limits.textBytes;
        for (const auto &node : projection.nodes) {
            if (node.relations.size() > relations || node.actions.size() > actions)
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
            relations -= node.relations.size();
            actions -= node.actions.size();
            if (!MeasureNodeText(node, text))
                return Result<void>::Failure(MakeError(UiErrors::CapacityExceeded));
        }
        return Result<void>::Success();
    }

    /** @copydoc BuildReadingProjection */
    Result<void> BuildReadingProjection(const UiElementTree &tree, const UiAccessibilityProjection &projection,
                                        const ProjectionLookup lookup, std::vector<UiElementHandle> &preorder,
                                        std::vector<UiAccessibilityNodeInput> &reading) {
        const auto order = tree.Preorder(preorder);
        if (order.HasError())
            return Result<void>::Failure(order.ErrorValue());
        reading.clear();
        for (const auto handle : std::span{preorder}.first(order.Value())) {
            const auto record = tree.Get(handle).Value();
            const auto index = FindProjectionIndex(lookup, record.id);
            if (index == std::numeric_limits<std::size_t>::max())
                continue;
            auto node = projection.nodes[index];
            if (node.exposure == UiAccessibilityExposure::Hidden)
                continue;
            node.exposure = Exposure(tree, record.parent, node.exposure, projection, lookup);
            if (node.exposure == UiAccessibilityExposure::Hidden)
                continue;
            if (InvalidFocus(node))
                return Result<void>::Failure(MakeError(UiErrors::AccessibilityStateInvalid));
            reading.push_back(node);
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui::AccessibilityInternal
