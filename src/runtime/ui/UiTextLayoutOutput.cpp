#include "Horo/Runtime/Ui/UiTextUnicode.h"
#include "UiTextLayoutInternal.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace Horo::Runtime::Ui {
    /** @copydoc UiTextLayoutInternal::PrepareGlyphGeometry */
    Result<UiTextLayoutInternal::GlyphGeometry> UiTextLayoutInternal::PrepareGlyphGeometry(const UiTextShapedGlyph &glyph,
                                                                                           const UiTextScale scale,
                                                                                           const UiLogicalPoint lineOrigin,
                                                                                           const std::int64_t cursor) {
        const auto offsetX = ScaleValue(glyph.offset.x, scale);
        const auto offsetY = ScaleValue(glyph.offset.y, scale);
        const auto advanceX = ScaleValue(glyph.advance.x, scale);
        if (offsetX.HasError() || offsetY.HasError() || advanceX.HasError())
            return Failure<GlyphGeometry>(UiErrors::TextLayoutCapacityExceeded);
        const auto glyphX = AddValue(cursor, offsetX.Value());
        const auto glyphY = AddValue(lineOrigin.y, offsetY.Value());
        if (glyphX.HasError() || glyphY.HasError())
            return Failure<GlyphGeometry>(UiErrors::TextLayoutCapacityExceeded);
        return Result<GlyphGeometry>::Success({{glyphX.Value(), glyphY.Value()}, {advanceX.Value(), 0}});
    }

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
                                                                  const std::uint32_t clusterIndex,
                                                                  UiTextLayoutInternal::SourceAppendState &state,
                                                                  const std::uint8_t lineLevel) {
        const auto &cluster = request.shaped.clusters[clusterIndex];
        const auto clusterOrigin = state.cursor;
        for (std::uint32_t glyphOffset = 0; glyphOffset < cluster.glyphCount; ++glyphOffset) {
            const auto &glyph = request.shaped.glyphs[cluster.firstGlyph + glyphOffset];
            const auto geometry = UiTextLayoutInternal::PrepareGlyphGeometry(glyph, request.options.scale, state.lineOrigin, state.cursor);
            if (geometry.HasError())
                return Result<void>::Failure(geometry.ErrorValue());
            const UiTextLayoutInternal::GlyphAppend placement{glyphFaces[cluster.firstGlyph + glyphOffset],
                                                              glyph.glyph,
                                                              clusterIndex,
                                                              geometry.Value().origin,
                                                              geometry.Value().advance,
                                                              state.lineIndex,
                                                              false};
            if (const auto appended = AppendGlyph(slot, placement); appended.HasError())
                return appended;
            state.cursor += geometry.Value().advance.x;
        }

        state.cursor = clusterOrigin + scaledClusterAdvances[clusterIndex];
        slot.clusters.push_back({clusterIndex,
                                 cluster.byteStart,
                                 cluster.byteEnd,
                                 state.lineIndex,
                                 {static_cast<std::int32_t>(clusterOrigin), state.lineOrigin.y},
                                 scaledClusterAdvances[clusterIndex],
                                 (lineLevel & 1U) != 0});
        if (cluster.breakOpportunity == UiTextBreakOpportunity::Optional && state.gapIndex < state.optionalGaps) {
            const auto addition = state.justifyExtra + (state.justifyRemainder-- > 0 ? 1 : 0);
            state.cursor += addition;
            state.justificationAdded += addition;
            ++state.gapIndex;
        }
        return Result<void>::Success();
    }

    Result<void> UiTextLayoutEngine::Storage::AppendSourceRange(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                                const std::uint32_t end, UiTextLayoutInternal::SourceAppendState &state) {
        if (request.unicode != nullptr && end > state.start) {
            const auto clusters = request.shaped.clusters;
            const auto ordered = request.unicodeAnalyzer->OrderLine(*request.unicode, clusters[state.start].byteStart,
                                                                    clusters[end - 1].byteEnd, visualScalars, visualLevels);
            if (ordered.HasError())
                return Result<void>::Failure(ordered.ErrorValue());
            std::uint32_t previous = NoUiTextLayoutCluster;
            for (std::uint32_t index = 0; index < ordered.Value(); ++index) {
                const auto scalar = request.unicode->Scalars()[visualScalars[index]];
                const auto upper = std::ranges::upper_bound(clusters, scalar.byteStart, {}, &UiTextShapedCluster::byteStart);
                if (upper == clusters.begin())
                    return Failure(UiErrors::TextLayoutInputInvalid);
                const auto clusterIndex = static_cast<std::uint32_t>(std::prev(upper) - clusters.begin());
                if (clusterIndex < state.start || clusterIndex >= end || scalar.byteEnd > clusters[clusterIndex].byteEnd)
                    return Failure(UiErrors::TextLayoutInputInvalid);
                if (clusterIndex != previous) {
                    if (const auto appended = AppendSourceCluster(request, slot, clusterIndex, state, visualLevels[index]);
                        appended.HasError())
                        return appended;
                    previous = clusterIndex;
                }
            }
            return Result<void>::Success();
        }
        for (std::uint32_t clusterIndex = state.start; clusterIndex < end; ++clusterIndex) {
            if (const auto appended =
                    AppendSourceCluster(request, slot, clusterIndex, state, request.shaped.clusters[clusterIndex].bidiLevel);
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
            const bool fits = usedWidth == 0 ? advance.Value() <= available : usedWidth + advance.Value() <= available;
            if (!fits)
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
            const auto geometry = UiTextLayoutInternal::PrepareGlyphGeometry(glyph, request.options.scale, lineOrigin, cursor);
            if (geometry.HasError())
                return Result<void>::Failure(geometry.ErrorValue());
            const UiTextLayoutInternal::GlyphAppend placement{ellipsisFaces[glyphOffset],
                                                              glyph.glyph,
                                                              NoUiTextLayoutCluster,
                                                              geometry.Value().origin,
                                                              geometry.Value().advance,
                                                              lineIndex,
                                                              true};
            if (const auto appended = AppendGlyph(slot, placement); appended.HasError())
                return appended;
            cursor += geometry.Value().advance.x;
        }
        return Result<void>::Success();
    }

    Result<void> UiTextLayoutEngine::Storage::AppendLineRecord(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                               const UiTextLayoutInternal::LineBuildContext &context,
                                                               const UiTextLayoutInternal::LineWindow &window,
                                                               const UiTextLayoutInternal::LinePlacement &placement,
                                                               const std::int32_t renderedWidth) {
        const auto lineExtentWidth = request.options.horizontal == UiTextHorizontalAlignment::Justify &&
                                             context.lineIndex + 1U < context.visibleLines && !window.ellipsis
                                         ? request.assignedContent.extent.width
                                         : renderedWidth;
        const auto firstGlyph = slot.lines.empty() ? 0U : slot.lines.back().firstGlyph + slot.lines.back().glyphCount;
        const auto firstRun = slot.lines.empty() ? 0U : slot.lines.back().firstRun + slot.lines.back().runCount;
        slot.lines.push_back(
            {firstGlyph,
             static_cast<std::uint32_t>(slot.glyphs.size()) - firstGlyph,
             firstRun,
             static_cast<std::uint32_t>(slot.runs.size()) - firstRun,
             {placement.originX, placement.originY},
             {lineExtentWidth, context.lineHeight},
             placement.baseline,
             static_cast<std::int32_t>(std::min<std::int64_t>(placement.lineAdvance, std::numeric_limits<std::int32_t>::max())),
             linePlans[context.lineIndex].hardBreak,
             window.ellipsis});
        return Result<void>::Success();
    }

    Result<void> UiTextLayoutEngine::Storage::BuildLine(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                        const UiTextLayoutInternal::LineBuildContext &context) {
        const auto windowResult =
            PrepareLineWindow(request, context.lineIndex, context.visibleLines, context.verticallyTruncated, context.ellipsisWidth);
        if (windowResult.HasError())
            return Result<void>::Failure(windowResult.ErrorValue());
        const auto &window = windowResult.Value();
        const auto placementResult = PrepareLinePlacement(request, window, context.lineIndex, context.visibleLines, context.lineHeight,
                                                          context.verticalOffset, context.scaledAscent);
        if (placementResult.HasError())
            return Result<void>::Failure(placementResult.ErrorValue());
        const auto &placement = placementResult.Value();
        const auto &clusters = request.shaped.clusters;
        std::uint32_t optionalGaps{};
        if (request.options.horizontal == UiTextHorizontalAlignment::Justify && context.lineIndex + 1U < context.visibleLines &&
            !window.ellipsis)
            for (std::uint32_t cluster = window.start; cluster < window.end; ++cluster)
                optionalGaps += clusters[cluster].breakOpportunity == UiTextBreakOpportunity::Optional ? 1U : 0U;
        const auto justifyExtra = optionalGaps == 0 ? 0 : placement.freeWidth / optionalGaps;
        UiTextLayoutInternal::SourceAppendState state{.start = window.start,
                                                      .lineIndex = context.lineIndex,
                                                      .lineOrigin = {placement.originX, placement.originY},
                                                      .lineOriginX = placement.originX,
                                                      .cursor = placement.originX,
                                                      .optionalGaps = optionalGaps,
                                                      .justifyExtra = justifyExtra,
                                                      .justifyRemainder = optionalGaps == 0 ? 0 : placement.freeWidth % optionalGaps};

        if (const auto appended = AppendLineContent(request, slot, window, state); appended.HasError())
            return appended;
        const auto renderedWidth = UiTextLayoutInternal::AddValue(0, state.cursor - placement.originX);
        if (renderedWidth.HasError())
            return Result<void>::Failure(renderedWidth.ErrorValue());
        return AppendLineRecord(request, slot, context, window, placement, renderedWidth.Value());
    }

    /** @copydoc UiTextLayoutEngine::Storage::AppendLineContent */
    Result<void> UiTextLayoutEngine::Storage::AppendLineContent(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                                                const UiTextLayoutInternal::LineWindow &window,
                                                                UiTextLayoutInternal::SourceAppendState &state) {
        // Truncation retains a logical prefix. Its omission marker occupies the
        // visual trailing edge, which precedes the source glyphs for RTL flow.
        const bool leadingEllipsis = window.ellipsis && request.options.direction == UiTextFlowDirection::RightToLeft;
        if (leadingEllipsis) {
            if (const auto appended =
                    AppendEllipsisGlyphs(request, slot, window.ellipsisGlyphCount, state.lineIndex, state.lineOrigin, state.cursor);
                appended.HasError())
                return appended;
        }
        if (const auto appended = AppendSourceRange(request, slot, window.end, state); appended.HasError())
            return appended;
        const auto sourceEnd = linePlans[state.lineIndex].firstCluster + linePlans[state.lineIndex].clusterCount;
        if (linePlans[state.lineIndex].hardBreak && window.end < sourceEnd) {
            const auto &hardBreak = request.shaped.clusters[sourceEnd - 1];
            slot.clusters.push_back({sourceEnd - 1,
                                     hardBreak.byteStart,
                                     hardBreak.byteEnd,
                                     state.lineIndex,
                                     {static_cast<std::int32_t>(state.cursor), state.lineOrigin.y},
                                     0,
                                     (hardBreak.bidiLevel & 1U) != 0});
        }
        if (window.ellipsis && !leadingEllipsis) {
            if (const auto appended =
                    AppendEllipsisGlyphs(request, slot, window.ellipsisGlyphCount, state.lineIndex, state.lineOrigin, state.cursor);
                appended.HasError())
                return appended;
        }

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
        for (std::uint32_t lineIndex = 0; lineIndex < visibleLines; ++lineIndex) {
            const UiTextLayoutInternal::LineBuildContext context{.lineHeight = lineHeight,
                                                                 .visibleLines = visibleLines,
                                                                 .lineIndex = lineIndex,
                                                                 .verticallyTruncated = metrics.Value().verticallyTruncated,
                                                                 .ellipsisWidth = metrics.Value().ellipsisWidth,
                                                                 .verticalOffset = verticalOffset,
                                                                 .scaledAscent = scaledAscent.Value()};
            if (const auto line = BuildLine(request, slot, context); line.HasError())
                return line;
        }

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
