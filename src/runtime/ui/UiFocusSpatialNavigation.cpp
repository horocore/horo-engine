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
            using enum UiFocusWrapPolicy;
            return policy == Both || policy == (horizontal ? Horizontal : Vertical);
        }

        using SpatialRank = std::tuple<bool, std::int64_t, std::int64_t, std::int64_t, UiElementId>;

        /** @brief Keeps the best deterministic candidate in fixed storage, shared by forward and wrap selection. */
        struct RankedTarget final {
            std::optional<SpatialRank> rank;
            std::optional<std::size_t> target;

            /** @brief Adopts a strictly better candidate without allocating. */
            void Consider(const std::size_t index, const SpatialRank &candidate) noexcept {
                if (!rank.has_value() || candidate < *rank) {
                    rank = candidate;
                    target = index;
                }
            }
        };
    }  // namespace

    /** @copydoc UiFocusGraph::UpdateLayout */
    Result<void> UiFocusGraph::UpdateLayout(const UiLayoutSnapshot &layout) {
        if (auto prepared = PrepareLayout(layout); prepared.HasError())
            return prepared;
        PublishPreparedLayout(layout);
        return Result<void>::Success();
    }

    /** @copydoc UiFocusGraph::PrepareLayout */
    Result<void> UiFocusGraph::PrepareLayout(const UiLayoutSnapshot &layout, const std::span<const UiLogicalRect> projected,
                                             const std::span<const std::uint8_t> eligibility) {
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
        if (records.size() > MaximumUiTreeElements || (!projected.empty() && projected.size() != records.size()) ||
            (!eligibility.empty() && (eligibility.size() != records.size() || std::ranges::any_of(eligibility, [](const auto value) {
            return value > 1;
        }))))
            return Failure(UiErrors::FocusCapacityExceeded);

        std::ranges::fill(storage_->layoutScratch, std::nullopt);
        std::ranges::fill(storage_->eligibilityScratch, eligibility.empty());
        for (std::size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
            const auto &record = records[recordIndex];
            if (!record.element.IsValid() || record.element.ownership != owner.instance.ownership || !record.arrangement.IsValid())
                return Failure(UiErrors::FocusInvalid);
            const auto found = std::ranges::lower_bound(storage_->handleOrder, record.element, {}, [this](const std::size_t index) {
                return storage_->nodes[index].descriptor.element;
            });
            if (found == storage_->handleOrder.end() || storage_->nodes[*found].descriptor.element != record.element)
                continue;
            auto &bounds = storage_->layoutScratch[*found];
            if (bounds.has_value())
                return Failure(UiErrors::FocusInvalid);
            const auto box = projected.empty() ? record.arrangement.hitTest : projected[recordIndex];
            if (!box.IsValid())
                return Failure(UiErrors::FocusInvalid);
            bounds = box;
            storage_->eligibilityScratch[*found] = eligibility.empty() || eligibility[recordIndex] != 0;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiFocusGraph::PublishPreparedLayout */
    void UiFocusGraph::PublishPreparedLayout(const UiLayoutSnapshot &layout) noexcept {
        for (std::size_t index = 0; index < storage_->nodes.size(); ++index) {
            storage_->nodes[index].bounds = storage_->layoutScratch[index];
            storage_->nodes[index].presentationEligible = storage_->eligibilityScratch[index];
        }
        if (storage_->focusedIndex.has_value() && !storage_->IsAllowed(*storage_->focusedIndex))
            storage_->focusedIndex = storage_->ResolveInitial();
        storage_->descriptor.owner.interaction = layout.Descriptor().interaction;
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
        RankedTarget best;
        RankedTarget wrapped;
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
                best.Consider(index, {outsideBeam, forward, gap, crossDistance, node.descriptor.id});
            } else if (forward < 0 && CanWrap(descriptor.wrap, horizontal)) {
                // Opposite edge precedes perpendicular alignment during wrap.
                wrapped.Consider(index, {false, candidate.center, gap, crossDistance, node.descriptor.id});
            }
        }
        return best.target.has_value() ? best.target : wrapped.target;
    }
}  // namespace Horo::Runtime::Ui
