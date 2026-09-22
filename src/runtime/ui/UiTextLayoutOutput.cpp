#include "UiTextLayoutInternal.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Horo::Runtime::Ui {
    using UiTextLayoutInternal::Failure;

    Result<void> UiTextLayoutEngine::Storage::PrepareEllipsis(const UiTextLayoutRequest &request, const bool needsEllipsis) {
        if (!needsEllipsis)
            return Result<void>::Success();
        if (!request.ellipsis.has_value() || !request.ellipsis->IsValid(descriptor.limits) || request.ellipsis->clusters.empty() ||
            request.ellipsis->glyphs.empty())
            return Failure(UiErrors::TextLayoutEllipsisInvalid);
        return BuildFaceTable(*request.ellipsis, ellipsisFaces);
    }

    Result<UiTextLayoutInternal::OutputMetrics> UiTextLayoutEngine::Storage::PrepareOutputMetrics(const UiTextLayoutRequest &request,
                                                                                                  const std::int32_t lineHeight,
                                                                                                  const std::uint32_t visibleLines) {
        bool anyHorizontalOverflow{};
        for (const auto &line : linePlans)
            anyHorizontalOverflow = anyHorizontalOverflow || line.advance > request.assignedContent.extent.width;
        const bool verticallyTruncated = visibleLines < linePlans.size();
        const bool needsEllipsis =
            request.options.overflow == UiTextOverflowMode::Ellipsis && (verticallyTruncated || anyHorizontalOverflow);
        if (const auto ellipsis = PrepareEllipsis(request, needsEllipsis); ellipsis.HasError())
            return Result<UiTextLayoutInternal::OutputMetrics>::Failure(ellipsis.ErrorValue());

        std::int64_t ellipsisWidth{};
        if (needsEllipsis) {
            const auto width = ShapedWidth(*request.ellipsis, request.options.scale);
            if (width.HasError())
                return Result<UiTextLayoutInternal::OutputMetrics>::Failure(width.ErrorValue());
            ellipsisWidth = width.Value();
        }
        std::int64_t fullWidth{};
        for (const auto &line : linePlans)
            fullWidth = std::max(fullWidth, line.advance);
        const auto fullHeight = static_cast<std::int64_t>(linePlans.size()) * lineHeight;
        if (fullWidth > std::numeric_limits<std::int32_t>::max() || fullHeight > std::numeric_limits<std::int32_t>::max())
            return Failure<UiTextLayoutInternal::OutputMetrics>(UiErrors::TextLayoutCapacityExceeded);
        return Result<UiTextLayoutInternal::OutputMetrics>::Success(
            {fullWidth, fullHeight, ellipsisWidth, verticallyTruncated, needsEllipsis});
    }

    Result<UiTextLayoutInternal::LineWindow> UiTextLayoutEngine::Storage::PrepareLineWindow(const UiTextLayoutRequest &request,
                                                                                            const std::uint32_t lineIndex,
                                                                                            const std::uint32_t visibleLines,
                                                                                            const bool verticallyTruncated,
                                                                                            const std::int64_t ellipsisWidth) const {
        const auto &clusters = request.shaped.clusters;
        const auto &plan = linePlans[lineIndex];
        auto start = plan.firstCluster;
        auto end = plan.firstCluster + plan.clusterCount;
        if (end > start && clusters[end - 1U].breakOpportunity == UiTextBreakOpportunity::Mandatory && clusters[end - 1U].glyphCount == 0)
            --end;

        auto sourceWidth = clusterPrefix[end] - clusterPrefix[start];
        const bool lineEllipsis =
            request.options.overflow == UiTextOverflowMode::Ellipsis &&
            ((verticallyTruncated && lineIndex + 1U == visibleLines) || sourceWidth > request.assignedContent.extent.width);
        if (lineEllipsis)
            while (end > start && sourceWidth + ellipsisWidth > request.assignedContent.extent.width) {
                --end;
                sourceWidth -= scaledClusterAdvances[end];
            }

        UiTextLayoutInternal::LineWindow window{start, end, sourceWidth, 0, 0, lineEllipsis};
        if (lineEllipsis) {
            const auto available = std::max<std::int64_t>(0, request.assignedContent.extent.width - sourceWidth);
            const auto ellipsisCount = CountEllipsisGlyphs(request, available, window.ellipsisUsedWidth);
            if (ellipsisCount.HasError())
                return Result<UiTextLayoutInternal::LineWindow>::Failure(ellipsisCount.ErrorValue());
            window.ellipsisGlyphCount = ellipsisCount.Value();
        }
        return Result<UiTextLayoutInternal::LineWindow>::Success(window);
    }

    Result<UiTextLayoutInternal::LinePlacement> UiTextLayoutEngine::Storage::PrepareLinePlacement(
        const UiTextLayoutRequest &request, const UiTextLayoutInternal::LineWindow &window, const std::uint32_t lineIndex,
        const std::uint32_t visibleLines, const std::int32_t lineHeight, const std::int64_t verticalOffset,
        const std::int32_t scaledAscent) const {
        const auto lineAdvance = window.sourceWidth + window.ellipsisUsedWidth;
        const auto freeWidth = std::max<std::int64_t>(0, request.assignedContent.extent.width - lineAdvance);
        std::int64_t alignmentOffset{};
        if (request.options.horizontal == UiTextHorizontalAlignment::Center)
            alignmentOffset = freeWidth / 2;
        else if (request.options.horizontal == UiTextHorizontalAlignment::Trailing)
            alignmentOffset = request.options.direction == UiTextFlowDirection::LeftToRight ? freeWidth : 0;
        else if (request.options.horizontal == UiTextHorizontalAlignment::Leading)
            alignmentOffset = request.options.direction == UiTextFlowDirection::LeftToRight ? 0 : freeWidth;
        else if (request.options.horizontal == UiTextHorizontalAlignment::Justify && lineIndex + 1U < visibleLines && !window.ellipsis)
            alignmentOffset = 0;

        const auto originX = UiTextLayoutInternal::AddValue(request.assignedContent.origin.x, alignmentOffset);
        if (originX.HasError())
            return Result<UiTextLayoutInternal::LinePlacement>::Failure(originX.ErrorValue());
        const auto originY = UiTextLayoutInternal::AddValue(request.assignedContent.origin.y,
                                                            verticalOffset + static_cast<std::int64_t>(lineIndex) * lineHeight);
        if (originY.HasError())
            return Result<UiTextLayoutInternal::LinePlacement>::Failure(originY.ErrorValue());
        const auto baseline = UiTextLayoutInternal::AddValue(originY.Value(), scaledAscent);
        if (baseline.HasError())
            return Result<UiTextLayoutInternal::LinePlacement>::Failure(baseline.ErrorValue());
        return Result<UiTextLayoutInternal::LinePlacement>::Success(
            {lineAdvance, freeWidth, originX.Value(), originY.Value(), baseline.Value()});
    }

    Result<void> UiTextLayoutEngine::Storage::AppendSourceCluster(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                                  const std::uint32_t start, const std::uint32_t clusterIndex,
                                                                  const std::uint32_t lineIndex, const UiLogicalPoint lineOrigin,
                                                                  const std::int64_t lineOriginX, std::int64_t &cursor,
                                                                  const std::uint32_t optionalGaps, const std::int64_t justifyExtra,
                                                                  std::int64_t &justifyRemainder, std::uint32_t &gapIndex,
                                                                  std::int64_t &justificationAdded) {
        const auto &cluster = request.shaped.clusters[clusterIndex];
        for (std::uint32_t glyphOffset = 0; glyphOffset < cluster.glyphCount; ++glyphOffset) {
            const auto &glyph = request.shaped.glyphs[cluster.firstGlyph + glyphOffset];
            const auto offsetX = UiTextLayoutInternal::ScaleValue(glyph.offset.x, request.options.scale);
            const auto offsetY = UiTextLayoutInternal::ScaleValue(glyph.offset.y, request.options.scale);
            const auto advanceX = UiTextLayoutInternal::ScaleValue(glyph.advance.x, request.options.scale);
            if (offsetX.HasError() || offsetY.HasError() || advanceX.HasError())
                return Failure(UiErrors::TextLayoutCapacityExceeded);
            const auto glyphX = UiTextLayoutInternal::AddValue(cursor, offsetX.Value());
            const auto glyphY = UiTextLayoutInternal::AddValue(lineOrigin.y, offsetY.Value());
            if (glyphX.HasError() || glyphY.HasError())
                return Failure(UiErrors::TextLayoutCapacityExceeded);
            if (const auto appended = AppendGlyph(slot, glyphFaces[cluster.firstGlyph + glyphOffset], glyph.glyph, clusterIndex,
                                                  {glyphX.Value(), glyphY.Value()}, {advanceX.Value(), 0}, lineIndex, false);
                appended.HasError())
                return appended;
            cursor += advanceX.Value();
        }

        cursor = lineOriginX + (clusterPrefix[clusterIndex + 1U] - clusterPrefix[start]) + justificationAdded;
        if (cluster.breakOpportunity == UiTextBreakOpportunity::Optional && gapIndex < optionalGaps) {
            const auto addition = justifyExtra + (justifyRemainder-- > 0 ? 1 : 0);
            cursor += addition;
            justificationAdded += addition;
            ++gapIndex;
        }
        return Result<void>::Success();
    }

    Result<void> UiTextLayoutEngine::Storage::AppendSourceRange(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                                const std::uint32_t start, const std::uint32_t end,
                                                                const std::uint32_t lineIndex, const UiLogicalPoint lineOrigin,
                                                                const std::int64_t lineOriginX, std::int64_t &cursor,
                                                                const std::uint32_t optionalGaps, const std::int64_t justifyExtra,
                                                                std::int64_t &justifyRemainder, std::uint32_t &gapIndex,
                                                                std::int64_t &justificationAdded) {
        for (std::uint32_t clusterIndex = start; clusterIndex < end; ++clusterIndex) {
            if (const auto appended = AppendSourceCluster(request, slot, start, clusterIndex, lineIndex, lineOrigin, lineOriginX, cursor,
                                                          optionalGaps, justifyExtra, justifyRemainder, gapIndex, justificationAdded);
                appended.HasError())
                return appended;
        }
        return Result<void>::Success();
    }

    Result<std::uint32_t> UiTextLayoutEngine::Storage::CountEllipsisGlyphs(const UiTextLayoutRequest &request, const std::int64_t available,
                                                                           std::int64_t &usedWidth) const {
        const auto &ellipsis = *request.ellipsis;
        std::uint32_t clusterGlyphs{};
        for (const auto &cluster : ellipsis.clusters)
            clusterGlyphs += cluster.glyphCount;

        std::uint32_t glyphCount{};
        for (std::uint32_t glyph = 0; glyph < ellipsis.glyphs.size() && glyph < clusterGlyphs; ++glyph) {
            const auto advance = UiTextLayoutInternal::ScaleValue(ellipsis.glyphs[glyph].advance.x, request.options.scale);
            if (advance.HasError())
                return Result<std::uint32_t>::Failure(advance.ErrorValue());
            if (usedWidth > 0 && usedWidth + advance.Value() > available)
                break;
            if (usedWidth == 0 && advance.Value() > available)
                break;
            usedWidth += advance.Value();
            ++glyphCount;
        }
        return Result<std::uint32_t>::Success(glyphCount);
    }

    Result<void> UiTextLayoutEngine::Storage::AppendEllipsisGlyphs(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                                   const std::uint32_t glyphCount, const std::uint32_t lineIndex,
                                                                   const UiLogicalPoint lineOrigin, std::int64_t &cursor) {
        for (std::uint32_t glyphOffset = 0; glyphOffset < glyphCount; ++glyphOffset) {
            const auto &glyph = request.ellipsis->glyphs[glyphOffset];
            const auto offsetX = UiTextLayoutInternal::ScaleValue(glyph.offset.x, request.options.scale);
            const auto offsetY = UiTextLayoutInternal::ScaleValue(glyph.offset.y, request.options.scale);
            const auto advanceX = UiTextLayoutInternal::ScaleValue(glyph.advance.x, request.options.scale);
            if (offsetX.HasError() || offsetY.HasError() || advanceX.HasError())
                return Failure(UiErrors::TextLayoutCapacityExceeded);
            const auto glyphX = UiTextLayoutInternal::AddValue(cursor, offsetX.Value());
            const auto glyphY = UiTextLayoutInternal::AddValue(lineOrigin.y, offsetY.Value());
            if (glyphX.HasError() || glyphY.HasError())
                return Failure(UiErrors::TextLayoutCapacityExceeded);
            if (const auto appended = AppendGlyph(slot, ellipsisFaces[glyphOffset], glyph.glyph, NoUiTextLayoutCluster,
                                                  {glyphX.Value(), glyphY.Value()}, {advanceX.Value(), 0}, lineIndex, true);
                appended.HasError())
                return appended;
            cursor += advanceX.Value();
        }
        return Result<void>::Success();
    }

    Result<void> UiTextLayoutEngine::Storage::BuildLine(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                        const std::int32_t lineHeight, const std::uint32_t visibleLines,
                                                        const std::uint32_t lineIndex, const bool verticallyTruncated,
                                                        const std::int64_t ellipsisWidth, const std::int64_t verticalOffset,
                                                        const std::int32_t scaledAscent) {
        const auto windowResult = PrepareLineWindow(request, lineIndex, visibleLines, verticallyTruncated, ellipsisWidth);
        if (windowResult.HasError())
            return Result<void>::Failure(windowResult.ErrorValue());
        const auto window = windowResult.Value();
        const auto placementResult =
            PrepareLinePlacement(request, window, lineIndex, visibleLines, lineHeight, verticalOffset, scaledAscent);
        if (placementResult.HasError())
            return Result<void>::Failure(placementResult.ErrorValue());
        const auto placement = placementResult.Value();
        const auto &clusters = request.shaped.clusters;
        const auto firstGlyph = static_cast<std::uint32_t>(slot.glyphs.size());
        const auto firstRun = static_cast<std::uint32_t>(slot.runs.size());
        std::int64_t cursor = placement.originX;
        std::uint32_t optionalGaps{};
        if (request.options.horizontal == UiTextHorizontalAlignment::Justify && lineIndex + 1U < visibleLines && !window.ellipsis)
            for (std::uint32_t cluster = window.start; cluster < window.end; ++cluster)
                optionalGaps += clusters[cluster].breakOpportunity == UiTextBreakOpportunity::Optional ? 1U : 0U;
        const auto justifyExtra = optionalGaps == 0 ? 0 : placement.freeWidth / optionalGaps;
        auto justifyRemainder = optionalGaps == 0 ? 0 : placement.freeWidth % optionalGaps;
        std::uint32_t gapIndex{};
        std::int64_t justificationAdded{};

        if (const auto appended =
                AppendSourceRange(request, slot, window.start, window.end, lineIndex, {placement.originX, placement.originY},
                                  placement.originX, cursor, optionalGaps, justifyExtra, justifyRemainder, gapIndex, justificationAdded);
            appended.HasError())
            return appended;
        if (window.ellipsis) {
            if (const auto appended = AppendEllipsisGlyphs(request, slot, window.ellipsisGlyphCount, lineIndex,
                                                           {placement.originX, placement.originY}, cursor);
                appended.HasError())
                return appended;
        }

        const auto renderedWidth = UiTextLayoutInternal::AddValue(0, cursor - placement.originX);
        if (renderedWidth.HasError())
            return Result<void>::Failure(renderedWidth.ErrorValue());
        const auto lineExtentWidth =
            request.options.horizontal == UiTextHorizontalAlignment::Justify && lineIndex + 1U < visibleLines && !window.ellipsis
                ? request.assignedContent.extent.width
                : renderedWidth.Value();
        slot.lines.push_back(
            {firstGlyph,
             static_cast<std::uint32_t>(slot.glyphs.size()) - firstGlyph,
             firstRun,
             static_cast<std::uint32_t>(slot.runs.size()) - firstRun,
             {placement.originX, placement.originY},
             {lineExtentWidth, lineHeight},
             placement.baseline,
             static_cast<std::int32_t>(std::min<std::int64_t>(placement.lineAdvance, std::numeric_limits<std::int32_t>::max())),
             linePlans[lineIndex].hardBreak,
             window.ellipsis});
        return Result<void>::Success();
    }

    Result<void> UiTextLayoutEngine::Storage::BuildOutput(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                          const std::int32_t lineHeight, const std::uint32_t visibleLines,
                                                          const bool sourceTruncated) {
        const auto metrics = PrepareOutputMetrics(request, lineHeight, visibleLines);
        if (metrics.HasError())
            return Result<void>::Failure(metrics.ErrorValue());

        const auto clamp = [](const std::int64_t value, const std::int32_t minimum, const std::int32_t maximum) {
            return static_cast<std::int32_t>(std::clamp(value, static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum)));
        };
        slot.measurement = {{clamp(metrics.Value().fullWidth, request.constraints.minimum.width, request.constraints.maximum.width),
                             clamp(metrics.Value().fullHeight, request.constraints.minimum.height, request.constraints.maximum.height)},
                            request.options.wrap != UiTextWrapMode::NoWrap,
                            request.options.maxLines != 0 || request.options.overflow == UiTextOverflowMode::Ellipsis};

        const auto blockHeight = static_cast<std::int64_t>(visibleLines) * lineHeight;
        const auto verticalFree = std::max<std::int64_t>(0, request.assignedContent.extent.height - blockHeight);
        std::int64_t verticalOffset{};
        if (request.options.vertical == UiTextVerticalAlignment::Center)
            verticalOffset = verticalFree / 2;
        else if (request.options.vertical == UiTextVerticalAlignment::Bottom)
            verticalOffset = verticalFree;
        const auto scaledAscent = UiTextLayoutInternal::ScaleValue(request.shaped.metrics.ascent, request.options.scale);
        if (scaledAscent.HasError())
            return Result<void>::Failure(scaledAscent.ErrorValue());
        for (std::uint32_t lineIndex = 0; lineIndex < visibleLines; ++lineIndex)
            if (const auto line = BuildLine(request, slot, lineHeight, visibleLines, lineIndex, metrics.Value().verticallyTruncated,
                                            metrics.Value().ellipsisWidth, verticalOffset, scaledAscent.Value());
                line.HasError())
                return line;

        slot.overflow = {request.assignedContent.extent,
                         slot.measurement.desired,
                         {static_cast<std::int32_t>(metrics.Value().fullWidth), static_cast<std::int32_t>(metrics.Value().fullHeight)},
                         request.options.overflow == UiTextOverflowMode::Clip &&
                             (metrics.Value().fullWidth > request.assignedContent.extent.width ||
                              metrics.Value().fullHeight > request.assignedContent.extent.height),
                         sourceTruncated || metrics.Value().verticallyTruncated || metrics.Value().needsEllipsis};
        if (!slot.overflow.IsValid())
            return Failure(UiErrors::TextLayoutInputInvalid);
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::Ui
