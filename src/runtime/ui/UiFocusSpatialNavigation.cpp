#include "UiFocusGraphInternal.h"

#include <tuple>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Integer axis projection avoids floating-point ties and overflow at signed layout extremes. */
        struct AxisBox final {
            std::int64_t center;
            std::int64_t perpendicularCenter;
            std::int64_t low;
            std::int64_t high;
        };

        /** @brief Projects a box onto the navigation axis using doubled centers in 64-bit logical units. */
        AxisBox Project(const UiLogicalRect &box, const bool horizontal, const std::int64_t sign) noexcept {
            const std::int64_t primary = horizontal ? box.origin.x : box.origin.y;
            const std::int64_t primarySize = horizontal ? box.extent.width : box.extent.height;
            const std::int64_t cross = horizontal ? box.origin.y : box.origin.x;
            const std::int64_t crossSize = horizontal ? box.extent.height : box.extent.width;
            return {sign * (2 * primary + primarySize), 2 * cross + crossSize, cross, cross + crossSize};
        }

        /** @brief Enables wrap only for the explicitly selected cardinal axis. */
        bool CanWrap(const UiFocusWrapPolicy policy, const bool horizontal) noexcept {
            return policy == UiFocusWrapPolicy::Both ||
                   policy == (horizontal ? UiFocusWrapPolicy::Horizontal : UiFocusWrapPolicy::Vertical);
        }
    }  // namespace

    /** @copydoc UiFocusGraph::UpdateLayout */
    Result<void> UiFocusGraph::UpdateLayout(const UiLayoutSnapshot &layout) {
        using FocusGraphDetail::Failure;
        if (!storage_ || storage_->lifecycle != UiFocusGraphState::Active)
            return Failure(UiErrors::FocusLifecycleUnavailable);
        const auto &source = layout.Descriptor();
        const auto &owner = storage_->descriptor.owner;
        if (source.instance != owner.instance || source.canvas != owner.canvas || source.document != owner.document)
            return Failure(UiErrors::FocusScopeMismatch);
        if (!source.sources.IsValid() || !source.interaction.IsValid() || source.sources.document != owner.documentRevision ||
            source.sources.tree != owner.treeRevision || source.interaction.Compare(owner.interaction) == UiRevisionRelation::Older)
            return Failure(UiErrors::FocusSourceStale);
        const auto records = layout.Records();
        if (records.size() > MaximumUiTreeElements)
            return Failure(UiErrors::FocusCapacityExceeded);

        std::fill(storage_->layoutScratch.begin(), storage_->layoutScratch.end(), std::nullopt);
        for (const UiLayoutRecord &record : records) {
            if (!record.element.IsValid() || record.element.ownership != owner.instance.ownership || !record.arrangement.IsValid())
                return Failure(UiErrors::FocusInvalid);
            const auto found = std::lower_bound(storage_->handleOrder.begin(), storage_->handleOrder.end(), record.element,
                                                [this](const std::size_t index, const UiElementHandle handle) {
                return storage_->nodes[index].descriptor.element < handle;
            });
            if (found == storage_->handleOrder.end() || storage_->nodes[*found].descriptor.element != record.element)
                continue;
            auto &bounds = storage_->layoutScratch[*found];
            if (bounds.has_value())
                return Failure(UiErrors::FocusInvalid);
            bounds = record.arrangement.hitTest;
        }
        for (std::size_t index = 0; index < storage_->nodes.size(); ++index)
            storage_->nodes[index].bounds = storage_->layoutScratch[index];
        storage_->descriptor.owner.interaction = source.interaction;
        return Result<void>::Success();
    }

    /** @copydoc UiFocusGraph::Storage::SpatialTarget */
    std::optional<std::size_t> UiFocusGraph::Storage::SpatialTarget(const std::size_t source,
                                                                    const UiNavigationDirection direction) const noexcept {
        using enum UiNavigationDirection;
        if (direction != Left && direction != Right && direction != Up && direction != Down)
            return std::nullopt;
        const auto &sourceBounds = nodes[source].bounds;
        if (!sourceBounds.has_value() || sourceBounds->extent.width == 0 || sourceBounds->extent.height == 0)
            return std::nullopt;
        const bool horizontal = direction == Left || direction == Right;
        const std::int64_t sign = direction == Left || direction == Up ? -1 : 1;
        const AxisBox origin = Project(*sourceBounds, horizontal, sign);
        using Rank = std::tuple<bool, std::int64_t, std::int64_t, std::int64_t, UiElementId>;
        std::optional<Rank> bestRank;
        std::optional<Rank> wrapRank;
        std::optional<std::size_t> best;
        std::optional<std::size_t> wrapped;
        for (std::size_t index = 0; index < nodes.size(); ++index) {
            const Node &node = nodes[index];
            if (index == source || !node.bounds.has_value() || node.bounds->extent.width == 0 || node.bounds->extent.height == 0 ||
                !IsAllowed(index))
                continue;
            const AxisBox candidate = Project(*node.bounds, horizontal, sign);
            const auto gap = std::max<std::int64_t>(0, std::max(origin.low, candidate.low) - std::min(origin.high, candidate.high));
            const bool outsideBeam = std::min(origin.high, candidate.high) <= std::max(origin.low, candidate.low);
            const auto crossDistance = candidate.perpendicularCenter >= origin.perpendicularCenter
                                           ? candidate.perpendicularCenter - origin.perpendicularCenter
                                           : origin.perpendicularCenter - candidate.perpendicularCenter;
            const auto forward = candidate.center - origin.center;
            if (forward > 0) {
                const Rank rank{outsideBeam, forward, gap, crossDistance, node.descriptor.id};
                if (!bestRank.has_value() || rank < *bestRank) {
                    bestRank = rank;
                    best = index;
                }
            } else if (forward < 0 && CanWrap(descriptor.wrap, horizontal)) {
                // Opposite edge precedes perpendicular alignment during wrap.
                const Rank rank{false, candidate.center, gap, crossDistance, node.descriptor.id};
                if (!wrapRank.has_value() || rank < *wrapRank) {
                    wrapRank = rank;
                    wrapped = index;
                }
            }
        }
        return best.has_value() ? best : wrapped;
    }
}  // namespace Horo::Runtime::Ui
