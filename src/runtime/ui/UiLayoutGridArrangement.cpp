#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiLayoutArrangementInternal.h"
#include "UiLayoutSemanticsInternal.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <utility>

namespace Horo::Runtime::Ui::LayoutInternal {
    namespace {
        using enum UiGridTrackKind;

        [[nodiscard]] Result<std::int32_t> ResolveTrackValue(const UiGridTrack &track, const std::int32_t available) {
            switch (track.kind) {
                case Auto:
                case Fraction:
                    return Result<std::int32_t>::Success(0);
                case Dip:
                    return Result<std::int32_t>::Success(track.value);
                case Percent:
                    return MultiplyRatio(available, static_cast<std::uint32_t>(track.value), UiScalarUnitsPerDip);
            }
            return Failure<std::int32_t>(UiErrors::LayoutInvalid);
        }

        [[nodiscard]] Result<std::int32_t> ResolveTrackBound(const UiGridTrack &track, const bool maximum, const std::int32_t available) {
            return maximum ? ResolveOptionalMaximum(track.maximum, available) : ResolveLength(track.minimum, available);
        }

        [[nodiscard]] Result<std::uint16_t> ResolveGridDimension(const std::uint16_t authored, const std::uint16_t implicit,
                                                                 const bool allowImplicit) {
            const auto result = authored != 0 ? authored : implicit;
            if (result == 0 || result > MaximumUiGridTracks || (!allowImplicit && authored == 0))
                return Failure<std::uint16_t>(UiErrors::LayoutConstraintConflict);
            return Result<std::uint16_t>::Success(result);
        }

        struct GridCell final {
            std::uint16_t row{};
            std::uint16_t column{};
        };

        using GridOccupancy = std::array<bool, MaximumUiGridTracks * MaximumUiGridTracks>;

        struct GridPlacementState final {
            std::uint16_t columnCount{};
            std::uint16_t rowCount{};
        };

        [[nodiscard]] bool IsGridCellFree(const GridOccupancy &occupied, const GridCell cell,
                                          const UiLayoutGridPlacement placement) noexcept {
            if (cell.row + placement.rowSpan > MaximumUiGridTracks)
                return false;
            for (std::uint16_t y = 0; y < placement.rowSpan; ++y)
                for (std::uint16_t x = 0; x < placement.columnSpan; ++x)
                    if (occupied[(cell.row + y) * MaximumUiGridTracks + cell.column + x])
                        return false;
            return true;
        }

        [[nodiscard]] Result<GridCell> FindGridCell(const GridOccupancy &occupied, const UiLayoutGridPlacement placement,
                                                    const std::uint16_t columnCount, const std::uint16_t cursorRow,
                                                    const std::uint16_t cursorColumn) {
            const auto requestedColumn = placement.column == 0 ? cursorColumn : static_cast<std::uint16_t>(placement.column - 1);
            const auto requestedRow = placement.row == 0 ? cursorRow : static_cast<std::uint16_t>(placement.row - 1);
            for (std::uint16_t row = requestedRow; row < MaximumUiGridTracks; ++row) {
                const auto firstColumn = (placement.column == 0 && row != cursorRow) ? std::uint16_t{0} : requestedColumn;
                for (auto column = firstColumn; column + placement.columnSpan <= columnCount; ++column) {
                    if (placement.row != 0 && row != placement.row - 1)
                        break;
                    const GridCell cell{row, column};
                    if (IsGridCellFree(occupied, cell, placement))
                        return Result<GridCell>::Success(cell);
                }
            }
            return Failure<GridCell>(UiErrors::LayoutConstraintConflict);
        }

        void MarkGridCell(GridOccupancy &occupied, const GridCell cell, const UiLayoutGridPlacement placement) noexcept {
            for (std::uint16_t y = 0; y < placement.rowSpan; ++y)
                for (std::uint16_t x = 0; x < placement.columnSpan; ++x)
                    occupied[(cell.row + y) * MaximumUiGridTracks + cell.column + x] = true;
        }

        [[nodiscard]] Result<GridPlacementState> PlaceGridItems(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                                const std::span<const UiLayoutChildMeasurement> children,
                                                                const UiLayoutContainerStyle &container,
                                                                const std::span<UiLayoutChildPlacement> placementScratch) {
            const auto columnCountResult = ResolveGridDimension(container.columnCount, 1, true);
            if (columnCountResult.HasError())
                return Result<GridPlacementState>::Failure(columnCountResult.ErrorValue());
            const auto columnCount = columnCountResult.Value();
            GridOccupancy occupied{};
            std::uint16_t maximumRow{};
            std::uint16_t cursorRow{};
            std::uint16_t cursorColumn{};
            for (std::uint32_t index = 0; index < children.size(); ++index) {
                const auto *child = FindDescriptor(descriptors, children[index].element);
                if (child == nullptr)
                    return Failure<GridPlacementState>(UiErrors::LayoutSourceStale);
                if (IsOutOfFlow(child->style)) {
                    placementScratch[index] = {NoPlacementLine, 0, 1, 1, 0, 0};
                    continue;
                }
                const auto placement = child->style.grid;
                const auto cell = FindGridCell(occupied, placement, columnCount, cursorRow, cursorColumn);
                if (cell.HasError())
                    return Result<GridPlacementState>::Failure(cell.ErrorValue());
                MarkGridCell(occupied, cell.Value(), placement);
                placementScratch[index] = {cell.Value().row,
                                           cell.Value().column,
                                           placement.rowSpan,
                                           placement.columnSpan,
                                           children[index].measurement.desired.width,
                                           children[index].measurement.desired.height};
                maximumRow = std::max<std::uint16_t>(maximumRow, static_cast<std::uint16_t>(cell.Value().row + placement.rowSpan));
                cursorRow = cell.Value().row;
                cursorColumn = static_cast<std::uint16_t>(cell.Value().column + placement.columnSpan);
                if (cursorColumn >= columnCount) {
                    cursorColumn = 0;
                    cursorRow = static_cast<std::uint16_t>(cell.Value().row + 1);
                }
            }
            const auto rowCount = ResolveGridDimension(container.rowCount, std::max<std::uint16_t>(1, maximumRow), true);
            if (rowCount.HasError() || rowCount.Value() < maximumRow)
                return Failure<GridPlacementState>(UiErrors::LayoutConstraintConflict);
            return Result<GridPlacementState>::Success({columnCount, rowCount.Value()});
        }

        struct GridTrackState final {
            std::uint16_t columnCount{};
            std::uint16_t rowCount{};
            std::array<std::int32_t, MaximumUiGridTracks> columnSizes{};
            std::array<std::int32_t, MaximumUiGridTracks> rowSizes{};
            std::array<std::int32_t, MaximumUiGridTracks> columnWeights{};
            std::array<std::int32_t, MaximumUiGridTracks> rowWeights{};
            std::array<std::int32_t, MaximumUiGridTracks + 1> columnOffsets{};
            std::array<std::int32_t, MaximumUiGridTracks + 1> rowOffsets{};
        };

        [[nodiscard]] Result<GridTrackState> InitializeGridTracks(const UiLayoutContainerStyle &container,
                                                                  const UiLogicalRect parentContent, const GridPlacementState placement) {
            GridTrackState state{.columnCount = placement.columnCount, .rowCount = placement.rowCount};
            for (std::uint16_t index = 0; index < state.columnCount; ++index) {
                const auto value = ResolveTrackValue(container.columns[index], parentContent.extent.width);
                if (value.HasError())
                    return Result<GridTrackState>::Failure(value.ErrorValue());
                state.columnSizes[index] = value.Value();
                state.columnWeights[index] = container.columns[index].kind == Fraction ? container.columns[index].value : 0;
            }
            for (std::uint16_t index = 0; index < state.rowCount; ++index) {
                const auto value = ResolveTrackValue(container.rows[index], parentContent.extent.height);
                if (value.HasError())
                    return Result<GridTrackState>::Failure(value.ErrorValue());
                state.rowSizes[index] = value.Value();
                state.rowWeights[index] = container.rows[index].kind == Fraction ? container.rows[index].value : 0;
            }
            return Result<GridTrackState>::Success(state);
        }

        [[nodiscard]] Result<std::int32_t> OuterGridExtent(const std::int32_t desired, const std::int32_t start, const std::int32_t end) {
            const auto extent = CheckedCast(static_cast<std::int64_t>(desired) + start + end);
            if (extent.HasError())
                return Failure<std::int32_t>(UiErrors::LayoutInvalid);
            return Result<std::int32_t>::Success(std::max(0, extent.Value()));
        }

        [[nodiscard]] Result<void> ApplyGridIntrinsicTracks(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                            const std::span<const UiLayoutChildMeasurement> children,
                                                            const UiLayoutContainerStyle &container,
                                                            const std::span<const UiLayoutChildPlacement> placementScratch,
                                                            GridTrackState &state) {
            for (std::uint32_t index = 0; index < children.size(); ++index) {
                if (placementScratch[index].line == NoPlacementLine)
                    continue;
                const auto *child = FindDescriptor(descriptors, children[index].element);
                if (child == nullptr)
                    return Failure(UiErrors::LayoutSourceStale);
                const auto column = placementScratch[index].slot;
                const auto row = placementScratch[index].line;
                const auto width =
                    OuterGridExtent(children[index].measurement.desired.width, child->style.margin.left, child->style.margin.right);
                const auto height =
                    OuterGridExtent(children[index].measurement.desired.height, child->style.margin.top, child->style.margin.bottom);
                if (width.HasError() || height.HasError())
                    return Failure(UiErrors::LayoutInvalid);
                if (placementScratch[index].columnSpan == 1 && container.columns[column].kind == Auto)
                    state.columnSizes[column] = std::max(state.columnSizes[column], width.Value());
                if (placementScratch[index].rowSpan == 1 && container.rows[row].kind == Auto)
                    state.rowSizes[row] = std::max(state.rowSizes[row], height.Value());
            }
            return Result<void>::Success();
        }

        template <std::size_t Size>
        [[nodiscard]] std::int64_t GridSpanExtent(const std::array<std::int32_t, Size> &sizes, const std::uint32_t start,
                                                  const std::uint32_t span, const std::int32_t gap) {
            std::int64_t result{};
            for (std::uint32_t offset = 0; offset < span; ++offset)
                result += sizes[start + offset];
            if (span > 1)
                result += static_cast<std::int64_t>(span - 1) * gap;
            return result;
        }

        template <std::size_t Size>
        [[nodiscard]] Result<void> AddGridSpanDeficit(std::array<std::int32_t, Size> &sizes,
                                                      const std::array<UiGridTrack, MaximumUiGridTracks> &tracks, const std::uint32_t start,
                                                      const std::uint32_t span, const std::int64_t deficit) {
            if (deficit <= 0)
                return Result<void>::Success();
            std::uint32_t flexible{};
            for (std::uint32_t offset = 0; offset < span; ++offset)
                if (tracks[start + offset].kind == Auto || tracks[start + offset].kind == Fraction)
                    ++flexible;
            if (flexible == 0)
                flexible = span;
            std::int64_t remaining = deficit;
            for (std::uint32_t offset = 0; offset < span; ++offset) {
                if (flexible != span && tracks[start + offset].kind != Auto && tracks[start + offset].kind != Fraction)
                    continue;
                const auto part = remaining / flexible + (remaining % flexible != 0 ? 1 : 0);
                const auto size = CheckedCast(static_cast<std::int64_t>(sizes[start + offset]) + part);
                if (size.HasError())
                    return Result<void>::Failure(size.ErrorValue());
                sizes[start + offset] = size.Value();
                remaining -= part;
                --flexible;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyGridSpanDeficits(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                         const std::span<const UiLayoutChildMeasurement> children,
                                                         const UiLayoutContainerStyle &container,
                                                         const std::span<const UiLayoutChildPlacement> placementScratch,
                                                         GridTrackState &state) {
            for (std::uint32_t index = 0; index < children.size(); ++index) {
                if (placementScratch[index].line == NoPlacementLine)
                    continue;
                const auto *child = FindDescriptor(descriptors, children[index].element);
                if (child == nullptr)
                    return Failure(UiErrors::LayoutSourceStale);
                const auto column = placementScratch[index].slot;
                const auto row = placementScratch[index].line;
                const auto requiredWidth = static_cast<std::int64_t>(children[index].measurement.desired.width) + child->style.margin.left +
                                           child->style.margin.right;
                const auto requiredHeight = static_cast<std::int64_t>(children[index].measurement.desired.height) +
                                            child->style.margin.top + child->style.margin.bottom;
                const auto currentWidth = GridSpanExtent(state.columnSizes, column, placementScratch[index].columnSpan, container.gap);
                const auto currentHeight = GridSpanExtent(state.rowSizes, row, placementScratch[index].rowSpan, container.gap);
                auto result = AddGridSpanDeficit(state.columnSizes, container.columns, column, placementScratch[index].columnSpan,
                                                 requiredWidth - currentWidth);
                if (result.HasError())
                    return result;
                result = AddGridSpanDeficit(state.rowSizes, container.rows, row, placementScratch[index].rowSpan,
                                            requiredHeight - currentHeight);
                if (result.HasError())
                    return result;
            }
            return Result<void>::Success();
        }

        template <std::size_t Size>
        [[nodiscard]] Result<void> ClampGridTracks(std::array<std::int32_t, Size> &sizes,
                                                   const std::array<UiGridTrack, MaximumUiGridTracks> &tracks, const std::uint16_t count,
                                                   const std::int32_t available) {
            for (std::uint16_t index = 0; index < count; ++index) {
                auto minimum = ResolveTrackBound(tracks[index], false, available);
                auto maximum = ResolveTrackBound(tracks[index], true, available);
                if (minimum.HasError() || maximum.HasError())
                    return Failure(UiErrors::LayoutInvalid);
                if (minimum.Value() > maximum.Value())
                    maximum = minimum;
                sizes[index] = std::clamp(sizes[index], minimum.Value(), maximum.Value());
            }
            return Result<void>::Success();
        }

        template <std::size_t Size>
        [[nodiscard]] Result<void> DistributeGridFractions(std::array<std::int32_t, Size> &sizes,
                                                           const std::array<std::int32_t, Size> &weights, const std::uint16_t count,
                                                           const std::int32_t available, const std::int32_t gap) {
            std::int64_t used{};
            std::int64_t totalWeight{};
            for (std::uint16_t index = 0; index < count; ++index) {
                used += sizes[index];
                totalWeight += weights[index];
            }
            if (count > 1)
                used += static_cast<std::int64_t>(count - 1) * gap;
            const auto remaining = std::max<std::int64_t>(0, static_cast<std::int64_t>(available) - used);
            if (totalWeight == 0)
                return Result<void>::Success();
            for (std::uint16_t index = 0; index < count; ++index) {
                if (weights[index] == 0)
                    continue;
                const auto size = CheckedCast(static_cast<std::int64_t>(sizes[index]) + remaining * weights[index] / totalWeight);
                if (size.HasError())
                    return Result<void>::Failure(size.ErrorValue());
                sizes[index] = size.Value();
            }
            return Result<void>::Success();
        }

        template <std::size_t Size>
        [[nodiscard]] Result<std::array<std::int32_t, Size + 1>> BuildGridOffsets(const std::array<std::int32_t, Size> &sizes,
                                                                                  const std::uint16_t count, const std::int32_t origin) {
            std::array<std::int32_t, Size + 1> offsets{};
            offsets[0] = origin;
            for (std::uint16_t index = 0; index < count; ++index) {
                const auto next = CheckedAdd(offsets[index], sizes[index]);
                if (next.HasError())
                    return Result<std::array<std::int32_t, Size + 1>>::Failure(next.ErrorValue());
                offsets[index + 1] = next.Value();
            }
            return Result<std::array<std::int32_t, Size + 1>>::Success(offsets);
        }

        [[nodiscard]] Result<GridTrackState> ResolveGridTracks(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                               const std::span<const UiLayoutChildMeasurement> children,
                                                               const UiLayoutContainerStyle &container, const UiLogicalRect parentContent,
                                                               const GridPlacementState placement,
                                                               const std::span<const UiLayoutChildPlacement> placementScratch) {
            auto stateResult = InitializeGridTracks(container, parentContent, placement);
            if (stateResult.HasError())
                return stateResult;
            auto state = std::move(stateResult).Value();
            if (const auto intrinsic = ApplyGridIntrinsicTracks(descriptors, children, container, placementScratch, state);
                intrinsic.HasError())
                return Result<GridTrackState>::Failure(intrinsic.ErrorValue());
            if (const auto deficits = ApplyGridSpanDeficits(descriptors, children, container, placementScratch, state); deficits.HasError())
                return Result<GridTrackState>::Failure(deficits.ErrorValue());
            auto clamped = ClampGridTracks(state.columnSizes, container.columns, state.columnCount, parentContent.extent.width);
            if (clamped.HasError())
                return Result<GridTrackState>::Failure(clamped.ErrorValue());
            clamped = ClampGridTracks(state.rowSizes, container.rows, state.rowCount, parentContent.extent.height);
            if (clamped.HasError())
                return Result<GridTrackState>::Failure(clamped.ErrorValue());
            auto distributed = DistributeGridFractions(state.columnSizes, state.columnWeights, state.columnCount,
                                                       parentContent.extent.width, container.gap);
            if (distributed.HasError())
                return Result<GridTrackState>::Failure(distributed.ErrorValue());
            distributed =
                DistributeGridFractions(state.rowSizes, state.rowWeights, state.rowCount, parentContent.extent.height, container.gap);
            if (distributed.HasError())
                return Result<GridTrackState>::Failure(distributed.ErrorValue());
            clamped = ClampGridTracks(state.columnSizes, container.columns, state.columnCount, parentContent.extent.width);
            if (clamped.HasError())
                return Result<GridTrackState>::Failure(clamped.ErrorValue());
            clamped = ClampGridTracks(state.rowSizes, container.rows, state.rowCount, parentContent.extent.height);
            if (clamped.HasError())
                return Result<GridTrackState>::Failure(clamped.ErrorValue());
            const auto columnOffsets = BuildGridOffsets(state.columnSizes, state.columnCount, parentContent.origin.x);
            const auto rowOffsets = BuildGridOffsets(state.rowSizes, state.rowCount, parentContent.origin.y);
            if (columnOffsets.HasError() || rowOffsets.HasError())
                return Failure<GridTrackState>(UiErrors::LayoutInvalid);
            state.columnOffsets = columnOffsets.Value();
            state.rowOffsets = rowOffsets.Value();
            return Result<GridTrackState>::Success(state);
        }

        [[nodiscard]] Result<void> ArrangeGridFlowChildren(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                           const std::span<const UiLayoutChildMeasurement> children,
                                                           const UiLayoutStyle &parentStyle, const GridTrackState &tracks,
                                                           const std::span<const UiLayoutChildPlacement> placementScratch,
                                                           const std::span<UiLogicalRect> output) {
            for (std::uint32_t index = 0; index < children.size(); ++index) {
                if (placementScratch[index].line == NoPlacementLine)
                    continue;
                const auto *child = FindDescriptor(descriptors, children[index].element);
                if (child == nullptr)
                    return Failure(UiErrors::LayoutSourceStale);
                const auto column = placementScratch[index].slot;
                const auto row = placementScratch[index].line;
                auto width = CheckedSub(tracks.columnOffsets[column + placementScratch[index].columnSpan], tracks.columnOffsets[column]);
                auto height = CheckedSub(tracks.rowOffsets[row + placementScratch[index].rowSpan], tracks.rowOffsets[row]);
                const auto columnGap =
                    CheckedCast(static_cast<std::int64_t>(placementScratch[index].columnSpan - 1) * parentStyle.container.gap);
                const auto rowGap = CheckedCast(static_cast<std::int64_t>(placementScratch[index].rowSpan - 1) * parentStyle.container.gap);
                if (width.HasError() || height.HasError() || columnGap.HasError() || rowGap.HasError())
                    return Failure(UiErrors::LayoutInvalid);
                width = CheckedAdd(width.Value(), columnGap.Value());
                height = CheckedAdd(height.Value(), rowGap.Value());
                const auto originX = CheckedCast(static_cast<std::int64_t>(tracks.columnOffsets[column]) +
                                                 static_cast<std::int64_t>(column) * parentStyle.container.gap);
                const auto originY = CheckedCast(static_cast<std::int64_t>(tracks.rowOffsets[row]) +
                                                 static_cast<std::int64_t>(row) * parentStyle.container.gap);
                if (width.HasError() || height.HasError() || originX.HasError() || originY.HasError())
                    return Failure(UiErrors::LayoutInvalid);
                const UiLogicalRect cell{{originX.Value(), originY.Value()}, {width.Value(), height.Value()}};
                const auto placed =
                    PlaceFlowChild(cell, *child, children[index], false, CrossAlignment(parentStyle.container, child->style, false));
                if (placed.HasError())
                    return Result<void>::Failure(placed.ErrorValue());
                output[index] = placed.Value();
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ArrangeOutOfFlowChildren(const std::span<const UiLayoutElementDescriptor> descriptors,
                                                            const UiLogicalRect parentContent,
                                                            const std::span<const UiLayoutChildMeasurement> children,
                                                            const std::span<const UiLayoutChildPlacement> placementScratch,
                                                            const std::span<UiLogicalRect> output) {
            for (std::uint32_t index = 0; index < children.size(); ++index) {
                if (placementScratch[index].line != NoPlacementLine)
                    continue;
                const auto *child = FindDescriptor(descriptors, children[index].element);
                if (child == nullptr)
                    return Failure(UiErrors::LayoutSourceStale);
                const auto placed = PlaceAbsoluteChild(parentContent, *child, children[index]);
                if (placed.HasError())
                    return Result<void>::Failure(placed.ErrorValue());
                output[index] = placed.Value();
            }
            return Result<void>::Success();
        }
    }  // namespace

    [[nodiscard]] Result<void> ArrangeGrid(const std::span<const UiLayoutElementDescriptor> descriptors, const UiLogicalRect parentContent,
                                           const std::span<const UiLayoutChildMeasurement> children, const UiLayoutStyle &parentStyle,
                                           const std::span<UiLogicalRect> output,
                                           const std::span<UiLayoutChildPlacement> placementScratch) {
        if (placementScratch.size() != children.size())
            return Failure(UiErrors::LayoutInvalid);
        const auto placement = PlaceGridItems(descriptors, children, parentStyle.container, placementScratch);
        if (placement.HasError())
            return Result<void>::Failure(placement.ErrorValue());
        const auto tracks =
            ResolveGridTracks(descriptors, children, parentStyle.container, parentContent, placement.Value(), placementScratch);
        if (tracks.HasError())
            return Result<void>::Failure(tracks.ErrorValue());
        if (const auto arranged = ArrangeGridFlowChildren(descriptors, children, parentStyle, tracks.Value(), placementScratch, output);
            arranged.HasError())
            return arranged;
        return ArrangeOutOfFlowChildren(descriptors, parentContent, children, placementScratch, output);
    }
}  // namespace Horo::Runtime::Ui::LayoutInternal
