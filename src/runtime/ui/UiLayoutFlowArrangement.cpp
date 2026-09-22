#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiLayoutArrangementInternal.h"
#include "UiLayoutSemanticsInternal.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace Horo::Runtime::Ui::LayoutInternal {
    namespace {
        using enum UiLayoutDistribution;

        [[nodiscard]] Result<std::int32_t> ResolveDistributionOffset(const UiLayoutDistribution distribution, const std::int32_t freeSpace,
                                                                     const std::uint32_t count) {
            if (freeSpace <= 0 || distribution == Start)
                return Result<std::int32_t>::Success(0);
            if (distribution == Center)
                return RoundDivideEven(freeSpace, 2);
            if (distribution == End)
                return Result<std::int32_t>::Success(freeSpace);
            if (distribution == SpaceAround)
                return DistributedOffset(freeSpace, 1, std::max<std::uint32_t>(1, count * 2));
            if (distribution == SpaceEvenly)
                return DistributedOffset(freeSpace, 1, count + 1);
            return Result<std::int32_t>::Success(0);
        }

        struct FlowDistribution final {
            std::int32_t remaining{};
            std::int32_t leading{};
            std::uint32_t itemCount{};
        };

        struct FlowLayoutContext final {
            std::span<const UiLayoutElementDescriptor *> childDescriptors;
            std::span<const UiLayoutChildMeasurement> children;
            std::span<UiLayoutChildPlacement> placementScratch;
            std::span<std::uint8_t> frozenScratch;
            std::span<UiLogicalRect> output;
            UiLogicalRect parentContent;
            const UiLayoutStyle &parentStyle;
            bool horizontal;
            std::int32_t parentMain;
        };

        [[nodiscard]] Result<void> CompleteFlowLine(const std::span<UiLayoutLine> lineScratch, const std::uint32_t lineIndex,
                                                    const std::int64_t lineMain, const std::int32_t lineCross) {
            const auto mainExtent = CheckedCast(lineMain);
            if (mainExtent.HasError())
                return Result<void>::Failure(mainExtent.ErrorValue());
            lineScratch[lineIndex].mainExtent = mainExtent.Value();
            // cppcheck-suppress unreadVariable
            // The completed line is consumed by the later placement pass.
            lineScratch[lineIndex].crossExtent = lineCross;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> StartWrappedFlowLine(const std::span<UiLayoutLine> lineScratch, std::uint32_t &lineCount,
                                                        std::uint32_t &currentLine, const std::uint32_t firstChild,
                                                        const std::int32_t crossExtent) {
            ++currentLine;
            ++lineCount;
            if (lineCount > lineScratch.size())
                return Failure(UiErrors::CapacityExceeded);
            lineScratch[currentLine] = {firstChild, 0, 0, crossExtent};
            return Result<void>::Success();
        }

        struct FlowItemSize final {
            std::int64_t mainWithMargins{};
            std::int32_t crossWithMargins{};
            std::int32_t main{};
            std::int32_t cross{};
        };

        [[nodiscard]] FlowItemSize MeasureFlowItem(const UiLayoutChildMeasurement &measurement, const UiLayoutEdges margins,
                                                   const bool horizontal) noexcept {
            const auto main = MainValue(measurement.measurement.desired, horizontal);
            const auto cross = CrossValue(measurement.measurement.desired, horizontal);
            const auto mainWithMargins =
                static_cast<std::int64_t>(main) + MainMarginStart(margins, horizontal) + MainMarginEnd(margins, horizontal);
            const auto crossWithMargins = static_cast<std::int32_t>(std::max<std::int64_t>(0, static_cast<std::int64_t>(cross) +
                                                                                                  CrossMarginStart(margins, horizontal) +
                                                                                                  CrossMarginEnd(margins, horizontal)));
            return {mainWithMargins, crossWithMargins, main, cross};
        }

        void SetFlowPlacement(UiLayoutChildPlacement &placement, const std::uint32_t line, const std::uint32_t slot,
                              const FlowItemSize &size) noexcept {
            placement = {line, slot, 1, 1, size.main, size.cross};
        }

        void EnsureInitialFlowLine(const std::span<UiLayoutLine> lineScratch, std::uint32_t &lineCount, const std::uint32_t firstChild,
                                   const std::int32_t crossExtent) noexcept {
            if (lineCount != 0)
                return;
            lineCount = 1;
            lineScratch[0] = {firstChild, 0, 0, crossExtent};
        }

        [[nodiscard]] Result<std::uint32_t> BuildFlowLines(const FlowLineBuildContext &context) {
            std::uint32_t lineCount{};
            std::uint32_t currentLine{};
            std::int64_t lineMain{};
            std::int32_t lineCross{};
            for (std::uint32_t index = 0; index < context.children.size(); ++index) {
                const auto *child = context.childDescriptors[index];
                if (child == nullptr)
                    return Failure<std::uint32_t>(UiErrors::LayoutSourceStale);
                if (IsOutOfFlow(child->style)) {
                    context.placementScratch[index] = {NoPlacementLine, 0, 1, 1, 0, 0};
                    continue;
                }
                const auto size = MeasureFlowItem(context.children[index], child->style.margin, context.horizontal);
                EnsureInitialFlowLine(context.lineScratch, lineCount, index, size.crossWithMargins);
                const auto withGap = context.lineScratch[currentLine].childCount == 0
                                         ? size.mainWithMargins
                                         : lineMain + context.container.gap + size.mainWithMargins;
                if (const bool startsNewLine = context.container.wrap == UiLayoutWrapMode::Wrap &&
                                               context.lineScratch[currentLine].childCount > 0 && context.parentMain > 0 &&
                                               withGap > context.parentMain;
                    startsNewLine) {
                    if (const auto completed = CompleteFlowLine(context.lineScratch, currentLine, lineMain, lineCross);
                        completed.HasError())
                        return Result<std::uint32_t>::Failure(completed.ErrorValue());
                    if (const auto started =
                            StartWrappedFlowLine(context.lineScratch, lineCount, currentLine, index, size.crossWithMargins);
                        started.HasError())
                        return Result<std::uint32_t>::Failure(started.ErrorValue());
                    lineMain = size.mainWithMargins;
                    lineCross = size.crossWithMargins;
                } else {
                    lineMain = withGap;
                    lineCross = std::max(lineCross, size.crossWithMargins);
                }
                SetFlowPlacement(context.placementScratch[index], currentLine, context.lineScratch[currentLine].childCount, size);
                ++context.lineScratch[currentLine].childCount;
            }
            if (lineCount == 0)
                return Result<std::uint32_t>::Success(0);
            if (const auto completed = CompleteFlowLine(context.lineScratch, currentLine, lineMain, lineCross); completed.HasError())
                return Result<std::uint32_t>::Failure(completed.ErrorValue());
            if (context.container.wrap == UiLayoutWrapMode::NoWrap)
                // cppcheck-suppress unreadVariable
                // The no-wrap line cross extent is consumed by the later placement pass.
                context.lineScratch[0].crossExtent = context.parentCross;
            return Result<std::uint32_t>::Success(lineCount);
        }

        struct FlowBounds final {
            std::int32_t minimum{};
            std::int32_t maximum{};
        };

        [[nodiscard]] Result<std::int32_t> ResolveItemBound(const UiLayoutStyle &style, const bool horizontal, const bool minimum,
                                                            const std::int32_t available) {
            const auto &minimumLength = horizontal ? style.minimumWidth : style.minimumHeight;
            const auto &maximumLength = horizontal ? style.maximumWidth : style.maximumHeight;
            const auto &length = minimum ? minimumLength : maximumLength;
            return minimum ? ResolveLength(length, available) : ResolveOptionalMaximum(length, available);
        }

        [[nodiscard]] Result<FlowBounds> ResolveFlowBounds(const FlowLayoutContext &context, const UiLayoutStyle &style) {
            auto minimum = ResolveItemBound(style, context.horizontal, true, context.parentMain);
            auto maximum = ResolveItemBound(style, context.horizontal, false, context.parentMain);
            if (minimum.HasError() || maximum.HasError())
                return Failure<FlowBounds>(UiErrors::LayoutInvalid);
            if (minimum.Value() > maximum.Value())
                maximum = minimum;
            return Result<FlowBounds>::Success({minimum.Value(), maximum.Value()});
        }

        [[nodiscard]] bool IsFlowFrozen(const FlowLayoutContext &context, const std::uint32_t index) noexcept {
            return context.frozenScratch[index] != 0;
        }

        void FreezeFlowItem(const FlowLayoutContext &context, const std::uint32_t index) noexcept {
            context.frozenScratch[index] = 1;
        }

        [[nodiscard]] Result<std::int64_t> UsedFlowMain(const FlowLayoutContext &context, const UiLayoutLine &line,
                                                        const std::uint32_t lineIndex) {
            std::int64_t used{};
            std::uint32_t itemCount{};
            for (std::uint32_t index = line.firstChild; index < context.children.size(); ++index) {
                if (context.placementScratch[index].line != lineIndex)
                    continue;
                const auto *child = context.childDescriptors[index];
                if (child == nullptr)
                    return Result<std::int64_t>::Failure(MakeError(UiErrors::LayoutSourceStale));
                const auto margin = child->style.margin;
                used += context.placementScratch[index].mainExtent + MainMarginStart(margin, context.horizontal) +
                        MainMarginEnd(margin, context.horizontal);
                ++itemCount;
            }
            if (itemCount > 1)
                used += static_cast<std::int64_t>(itemCount - 1) * context.parentStyle.container.gap;
            return Result<std::int64_t>::Success(used);
        }

        [[nodiscard]] std::uint64_t FlowItemWeight(const UiLayoutElementDescriptor &child, const UiLayoutMeasurement &measurement,
                                                   const bool horizontal, const bool growing) noexcept {
            if (growing)
                return child.style.flex.grow;
            const auto base = MainValue(measurement.desired, horizontal);
            return static_cast<std::uint64_t>(child.style.flex.shrink) * static_cast<std::uint64_t>(std::max(0, base));
        }

        [[nodiscard]] Result<void> InitializeFlowItemExtents(const FlowLayoutContext &context, const UiLayoutLine &line,
                                                             const std::uint32_t lineIndex) {
            for (std::uint32_t index = line.firstChild; index < context.children.size(); ++index) {
                if (context.placementScratch[index].line != lineIndex)
                    continue;
                const auto *child = context.childDescriptors[index];
                if (child == nullptr)
                    return Failure(UiErrors::LayoutSourceStale);
                const auto bounds = ResolveFlowBounds(context, child->style);
                if (bounds.HasError())
                    return Result<void>::Failure(bounds.ErrorValue());
                const auto base = MainValue(context.children[index].measurement.desired, context.horizontal);
                context.placementScratch[index].mainExtent = std::clamp(base, bounds.Value().minimum, bounds.Value().maximum);
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::uint64_t> CollectFlowWeights(const FlowLayoutContext &context, const UiLayoutLine &line,
                                                               const std::uint32_t lineIndex, const bool growing) {
            std::uint64_t totalWeight{};
            for (std::uint32_t index = line.firstChild; index < context.children.size(); ++index) {
                if (context.placementScratch[index].line != lineIndex || IsFlowFrozen(context, index))
                    continue;
                const auto *child = context.childDescriptors[index];
                if (child == nullptr)
                    return Failure<std::uint64_t>(UiErrors::LayoutSourceStale);
                const auto bounds = ResolveFlowBounds(context, child->style);
                if (bounds.HasError())
                    return Result<std::uint64_t>::Failure(bounds.ErrorValue());
                if (const bool atBound = growing ? context.placementScratch[index].mainExtent >= bounds.Value().maximum
                                                 : context.placementScratch[index].mainExtent <= bounds.Value().minimum;
                    atBound) {
                    FreezeFlowItem(context, index);
                    continue;
                }
                totalWeight += FlowItemWeight(*child, context.children[index].measurement, context.horizontal, growing);
            }
            return Result<std::uint64_t>::Success(totalWeight);
        }

        struct FlowDistributionStep final {
            std::uint64_t weight{};
            bool froze{};
        };

        [[nodiscard]] Result<FlowDistributionStep> ApplyFlowItemDistribution(const FlowLayoutContext &context, const std::uint32_t index,
                                                                             const bool growing, const std::uint64_t space,
                                                                             const std::uint64_t totalWeight,
                                                                             const std::uint64_t cumulativeWeight) {
            const auto *child = context.childDescriptors[index];
            if (child == nullptr)
                return Failure<FlowDistributionStep>(UiErrors::LayoutSourceStale);
            const auto bounds = ResolveFlowBounds(context, child->style);
            if (bounds.HasError())
                return Result<FlowDistributionStep>::Failure(bounds.ErrorValue());
            const auto weight = FlowItemWeight(*child, context.children[index].measurement, context.horizontal, growing);
            const auto previousShare = SafeWeightedShare(space, cumulativeWeight, totalWeight);
            const auto nextShare = SafeWeightedShare(space, cumulativeWeight + weight, totalWeight);
            const auto signedShare = static_cast<std::int64_t>(nextShare - previousShare);
            const auto proposed = growing ? static_cast<std::int64_t>(context.placementScratch[index].mainExtent) + signedShare
                                          : static_cast<std::int64_t>(context.placementScratch[index].mainExtent) - signedShare;
            const auto target =
                std::clamp(proposed, static_cast<std::int64_t>(bounds.Value().minimum), static_cast<std::int64_t>(bounds.Value().maximum));
            const bool froze = target != proposed;
            if (froze)
                FreezeFlowItem(context, index);
            const auto checkedTarget = CheckedCast(target);
            if (checkedTarget.HasError())
                return Result<FlowDistributionStep>::Failure(checkedTarget.ErrorValue());
            context.placementScratch[index].mainExtent = checkedTarget.Value();
            return Result<FlowDistributionStep>::Success({weight, froze});
        }

        [[nodiscard]] Result<bool> ApplyFlowDistribution(const FlowLayoutContext &context, const UiLayoutLine &line,
                                                         const std::uint32_t lineIndex, const bool growing, const std::int64_t remaining,
                                                         const std::uint64_t totalWeight) {
            bool froze{};
            std::uint64_t cumulativeWeight{};
            const auto space = static_cast<std::uint64_t>(remaining < 0 ? -remaining : remaining);
            for (std::uint32_t index = line.firstChild; index < context.children.size(); ++index) {
                if (context.placementScratch[index].line != lineIndex || IsFlowFrozen(context, index))
                    continue;
                const auto *child = context.childDescriptors[index];
                if (child == nullptr)
                    return Failure<bool>(UiErrors::LayoutSourceStale);
                const auto bounds = ResolveFlowBounds(context, child->style);
                if (bounds.HasError())
                    return Result<bool>::Failure(bounds.ErrorValue());
                if (const bool atBound = growing ? context.placementScratch[index].mainExtent >= bounds.Value().maximum
                                                 : context.placementScratch[index].mainExtent <= bounds.Value().minimum;
                    atBound)
                    continue;
                if (const auto weight = FlowItemWeight(*child, context.children[index].measurement, context.horizontal, growing);
                    weight == 0)
                    continue;
                const auto step = ApplyFlowItemDistribution(context, index, growing, space, totalWeight, cumulativeWeight);
                if (step.HasError())
                    return Result<bool>::Failure(step.ErrorValue());
                cumulativeWeight += step.Value().weight;
                froze = froze || step.Value().froze;
            }
            return Result<bool>::Success(froze);
        }

        [[nodiscard]] Result<void> ResolveFlowItemExtents(const FlowLayoutContext &context, const UiLayoutLine &line,
                                                          const std::uint32_t lineIndex) {
            if (const auto initialized = InitializeFlowItemExtents(context, line, lineIndex); initialized.HasError())
                return initialized;
            if (context.parentStyle.container.kind != UiLayoutContainerKind::Flex)
                return Result<void>::Success();

            bool complete{};
            for (std::uint32_t iteration = 0; iteration <= line.childCount && !complete; ++iteration) {
                const auto used = UsedFlowMain(context, line, lineIndex);
                if (used.HasError())
                    return Result<void>::Failure(used.ErrorValue());
                const auto remaining = static_cast<std::int64_t>(context.parentMain) - used.Value();
                if (remaining == 0) {
                    complete = true;
                    continue;
                }
                const bool growing = remaining > 0;
                const auto totalWeight = CollectFlowWeights(context, line, lineIndex, growing);
                if (totalWeight.HasError())
                    return Result<void>::Failure(totalWeight.ErrorValue());
                if (totalWeight.Value() == 0) {
                    complete = true;
                    continue;
                }
                const auto applied = ApplyFlowDistribution(context, line, lineIndex, growing, remaining, totalWeight.Value());
                if (applied.HasError())
                    return Result<void>::Failure(applied.ErrorValue());
                if (!applied.Value())
                    complete = true;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<FlowDistribution> ResolveFlowDistribution(const FlowLayoutContext &context, const UiLayoutLine &line,
                                                                       const std::uint32_t lineIndex) {
            std::int64_t used{};
            std::uint32_t itemCount{};
            for (std::uint32_t index = line.firstChild; index < context.children.size(); ++index) {
                if (context.placementScratch[index].line != lineIndex)
                    continue;
                const auto *child = context.childDescriptors[index];
                if (child == nullptr)
                    return Failure<FlowDistribution>(UiErrors::LayoutSourceStale);
                const auto margin = child->style.margin;
                used += context.placementScratch[index].mainExtent + MainMarginStart(margin, context.horizontal) +
                        MainMarginEnd(margin, context.horizontal);
                ++itemCount;
            }
            if (itemCount > 1)
                used += static_cast<std::int64_t>(itemCount - 1) * context.parentStyle.container.gap;
            const auto remaining = CheckedCast(std::max<std::int64_t>(0, static_cast<std::int64_t>(context.parentMain) - used));
            if (remaining.HasError())
                return Result<FlowDistribution>::Failure(remaining.ErrorValue());
            const auto leading = ResolveDistributionOffset(context.parentStyle.container.mainAlignment, remaining.Value(), itemCount);
            if (leading.HasError())
                return Result<FlowDistribution>::Failure(leading.ErrorValue());
            return Result<FlowDistribution>::Success({remaining.Value(), leading.Value(), itemCount});
        }

        [[nodiscard]] Result<std::int32_t> ResolveItemSpacing(const UiLayoutContainerStyle &container, const std::int32_t remaining,
                                                              const std::uint32_t itemCount, const std::uint32_t placedCount) {
            if (placedCount >= itemCount)
                return Result<std::int32_t>::Success(0);
            std::int32_t extra{};
            switch (container.mainAlignment) {
                case SpaceBetween:
                    if (const auto delta = DistributedDelta(remaining, placedCount - 1, placedCount, itemCount - 1); delta.HasError())
                        return Result<std::int32_t>::Failure(delta.ErrorValue());
                    else
                        extra = delta.Value();
                    break;
                case SpaceAround:
                    if (const auto delta = DistributedDelta(remaining, placedCount * 2 - 1, placedCount * 2 + 1, itemCount * 2);
                        delta.HasError())
                        return Result<std::int32_t>::Failure(delta.ErrorValue());
                    else
                        extra = delta.Value();
                    break;
                case SpaceEvenly:
                    if (const auto delta = DistributedDelta(remaining, placedCount, placedCount + 1, itemCount + 1); delta.HasError())
                        return Result<std::int32_t>::Failure(delta.ErrorValue());
                    else
                        extra = delta.Value();
                    break;
                default:
                    break;
            }
            return CheckedAdd(container.gap, extra);
        }

        [[nodiscard]] Result<std::int32_t> AdvanceFlowCursor(const std::int32_t cursor, const std::int32_t extent,
                                                             const std::int32_t marginEnd, const UiLayoutContainerStyle &container,
                                                             const std::int32_t remaining, const std::uint32_t itemCount,
                                                             const std::uint32_t placedCount) {
            auto next = CheckedAdd(cursor, extent);
            if (next.HasError())
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            next = CheckedAdd(next.Value(), marginEnd);
            if (next.HasError())
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            const auto spacing = ResolveItemSpacing(container, remaining, itemCount, placedCount);
            if (spacing.HasError())
                return Result<std::int32_t>::Failure(spacing.ErrorValue());
            return CheckedAdd(next.Value(), spacing.Value());
        }

        [[nodiscard]] Result<std::int32_t> ArrangeFlowItem(const FlowLayoutContext &context, const std::int32_t cursor,
                                                           const std::int32_t crossCursor, const std::uint32_t index,
                                                           const UiLayoutLine &line, const FlowDistribution distribution,
                                                           const std::uint32_t placedCount) {
            const auto *child = context.childDescriptors[index];
            if (child == nullptr)
                return Failure<std::int32_t>(UiErrors::LayoutSourceStale);
            const auto margin = child->style.margin;
            const auto marginStart = MainMarginStart(margin, context.horizontal);
            const auto marginEnd = MainMarginEnd(margin, context.horizontal);
            const auto crossAlignment = CrossAlignment(context.parentStyle.container, child->style, context.horizontal);
            const auto margins = CheckedAdd(marginStart, marginEnd);
            if (margins.HasError())
                return Result<std::int32_t>::Failure(margins.ErrorValue());
            const auto mainCellExtent = CheckedAdd(context.placementScratch[index].mainExtent, margins.Value());
            if (mainCellExtent.HasError())
                return Result<std::int32_t>::Failure(mainCellExtent.ErrorValue());
            const UiLogicalRect cell{context.horizontal ? UiLogicalPoint{cursor, crossCursor} : UiLogicalPoint{crossCursor, cursor},
                                     context.horizontal ? UiLogicalExtent{mainCellExtent.Value(), line.crossExtent}
                                                        : UiLogicalExtent{line.crossExtent, mainCellExtent.Value()}};
            const auto placed = PlaceFlowChild(cell, *child, context.children[index], context.horizontal, crossAlignment,
                                               context.placementScratch[index].mainExtent);
            if (placed.HasError())
                return Result<std::int32_t>::Failure(placed.ErrorValue());
            context.output[index] = placed.Value();
            return AdvanceFlowCursor(cursor, context.placementScratch[index].mainExtent, marginEnd, context.parentStyle.container,
                                     distribution.remaining, distribution.itemCount, placedCount);
        }

        [[nodiscard]] Result<std::int32_t> ArrangeFlowLine(const FlowLayoutContext &context, const std::int32_t crossCursor,
                                                           const std::uint32_t lineIndex, const UiLayoutLine &line,
                                                           const FlowDistribution distribution, const bool hasNextLine) {
            const auto origin = context.horizontal ? context.parentContent.origin.x : context.parentContent.origin.y;
            auto cursor = CheckedAdd(origin, distribution.leading);
            if (cursor.HasError())
                return Result<std::int32_t>::Failure(cursor.ErrorValue());
            std::uint32_t placedCount{};
            for (std::uint32_t index = line.firstChild; index < context.children.size(); ++index) {
                if (context.placementScratch[index].line != lineIndex)
                    continue;
                ++placedCount;
                cursor = ArrangeFlowItem(context, cursor.Value(), crossCursor, index, line, distribution, placedCount);
                if (cursor.HasError())
                    return Result<std::int32_t>::Failure(cursor.ErrorValue());
            }
            auto nextCross = CheckedAdd(crossCursor, line.crossExtent);
            if (nextCross.HasError())
                return Result<std::int32_t>::Failure(nextCross.ErrorValue());
            if (hasNextLine) {
                nextCross = CheckedAdd(nextCross.Value(), context.parentStyle.container.gap);
                if (nextCross.HasError())
                    return Result<std::int32_t>::Failure(nextCross.ErrorValue());
            }
            return Result<std::int32_t>::Success(nextCross.Value());
        }

        [[nodiscard]] Result<void> ArrangeOutOfFlowChildren(const FlowLayoutContext &context) {
            for (std::uint32_t index = 0; index < context.children.size(); ++index) {
                if (context.placementScratch[index].line != NoPlacementLine)
                    continue;
                const auto *child = context.childDescriptors[index];
                if (child == nullptr)
                    return Failure(UiErrors::LayoutSourceStale);
                auto placed = PlaceAbsoluteChild(context.parentContent, *child, context.children[index]);
                if (placed.HasError())
                    return Result<void>::Failure(placed.ErrorValue());
                context.output[index] = placed.Value();
            }
            return Result<void>::Success();
        }
    }  // namespace

    [[nodiscard]] Result<void> ArrangeStackOrFlex(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                  const UiLogicalRect parentContent,
                                                  const std::span<const UiLayoutChildMeasurement> children,
                                                  const UiLayoutStyle &parentStyle, const std::span<UiLogicalRect> output,
                                                  const std::span<UiLayoutChildPlacement> placementScratch,
                                                  const std::span<UiLayoutLine> lineScratch) {
        if (placementScratch.size() != children.size() || lineScratch.size() < children.size() || children.size() > MaximumUiTreeElements)
            return Failure(UiErrors::LayoutInvalid);
        std::array<const UiLayoutElementDescriptor *, MaximumUiTreeElements> childDescriptorScratch{};
        for (std::uint32_t index = 0; index < children.size(); ++index) {
            childDescriptorScratch[index] = FindDescriptor(descriptors, children[index].element);
            if (childDescriptorScratch[index] == nullptr)
                return Failure(UiErrors::LayoutSourceStale);
        }
        std::array<std::uint8_t, MaximumUiTreeElements> frozenScratch{};
        const std::span<const UiLayoutElementDescriptor *> childDescriptors{childDescriptorScratch.data(), children.size()};
        const std::span<std::uint8_t> frozen{frozenScratch.data(), children.size()};
        const bool horizontal = IsHorizontal(parentStyle.container);
        const auto parentMain = MainValue(parentContent.extent, horizontal);
        const auto parentCross = CrossValue(parentContent.extent, horizontal);
        const FlowLineBuildContext lineContext{childDescriptors, children,    parentStyle.container, horizontal,
                                               parentMain,       parentCross, placementScratch,      lineScratch};
        const auto lineCountResult = BuildFlowLines(lineContext);
        if (lineCountResult.HasError())
            return Result<void>::Failure(lineCountResult.ErrorValue());
        const auto lineCount = lineCountResult.Value();
        const FlowLayoutContext context{childDescriptors, children,    placementScratch, frozen,    output,
                                        parentContent,    parentStyle, horizontal,       parentMain};
        std::int32_t crossCursor = horizontal ? parentContent.origin.y : parentContent.origin.x;
        for (std::uint32_t lineIndex = 0; lineIndex < lineCount; ++lineIndex) {
            const auto &line = lineScratch[lineIndex];
            if (const auto resolved = ResolveFlowItemExtents(context, line, lineIndex); resolved.HasError())
                return resolved;
            const auto distribution = ResolveFlowDistribution(context, line, lineIndex);
            if (distribution.HasError())
                return Result<void>::Failure(distribution.ErrorValue());
            const auto nextCross = ArrangeFlowLine(context, crossCursor, lineIndex, line, distribution.Value(), lineIndex + 1 < lineCount);
            if (nextCross.HasError())
                return Result<void>::Failure(nextCross.ErrorValue());
            crossCursor = nextCross.Value();
        }
        return ArrangeOutOfFlowChildren(context);
    }
}  // namespace Horo::Runtime::Ui::LayoutInternal
