#include "Horo/Runtime/Ui/UiTextLayout.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <new>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        struct LinePlan final {
            std::uint32_t firstCluster{};
            std::uint32_t clusterCount{};
            std::int64_t advance{};
            bool hardBreak{};
        };

        [[nodiscard]] Result<std::int32_t> ScaleValue(const std::int32_t value, const UiTextScale scale) {
            const std::int64_t product = static_cast<std::int64_t>(value) * static_cast<std::int64_t>(scale.value);
            const std::int64_t denominator = UiTextLayoutScaleUnit;
            const std::int64_t quotient = product / denominator;
            const std::int64_t remainder = product % denominator;
            const std::int64_t absoluteRemainder = remainder < 0 ? -remainder : remainder;
            std::int64_t rounded = quotient;
            if (absoluteRemainder * 2 > denominator || (absoluteRemainder * 2 == denominator && (rounded & 1) != 0))
                rounded += product < 0 ? -1 : 1;
            if (rounded < std::numeric_limits<std::int32_t>::min() || rounded > std::numeric_limits<std::int32_t>::max())
                return Failure<std::int32_t>(UiErrors::TextLayoutCapacityExceeded);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(rounded));
        }

        [[nodiscard]] Result<std::int32_t> AddValue(const std::int64_t left, const std::int64_t right) {
            const auto value = left + right;
            if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max())
                return Failure<std::int32_t>(UiErrors::TextLayoutCapacityExceeded);
            return Result<std::int32_t>::Success(static_cast<std::int32_t>(value));
        }

        [[nodiscard]] bool IsOlder(const UiLayoutSourceRevisions &candidate, const UiLayoutSourceRevisions &current) noexcept {
            return candidate.document < current.document || candidate.tree < current.tree || candidate.content < current.content ||
                   candidate.style < current.style || candidate.intrinsic < current.intrinsic || candidate.canvas < current.canvas ||
                   candidate.policy < current.policy;
        }

    }  // namespace

    struct UiTextLayoutResult::Storage final {
        mutable std::atomic<std::uint64_t> leases{};
        UiTextLayoutResultDescriptor descriptor;
        UiLayoutMeasurement measurement;
        UiTextLayoutOverflow overflow;
        std::vector<UiTextLayoutLine> lines;
        std::vector<UiTextLayoutGlyph> glyphs;
        std::vector<UiTextLayoutRun> runs;

        explicit Storage(const UiTextLayoutLimits &limits) {
            lines.reserve(limits.lines);
            glyphs.reserve(limits.glyphs);
            runs.reserve(limits.runs);
        }

        void Reset() noexcept {
            descriptor = {};
            measurement = {};
            overflow = {};
            lines.clear();
            glyphs.clear();
            runs.clear();
        }
    };

    struct UiTextLayoutEngine::Storage final {
        UiTextLayoutEngineDescriptor descriptor;
        UiTextLayoutEngineState lifecycle{UiTextLayoutEngineState::Active};
        std::vector<std::shared_ptr<UiTextLayoutResult::Storage>> slots;
        std::size_t nextSlot{};
        UiTextLayoutRevision lastRevision;
        UiTextLayoutSource lastSource;
        bool hasSource{};

        std::vector<std::int32_t> scaledClusterAdvances;
        std::vector<std::int64_t> clusterPrefix;
        std::vector<std::uint32_t> softBreaks;
        std::vector<LinePlan> linePlans;
        std::vector<UiTextFaceId> glyphFaces;
        std::vector<UiTextFaceId> ellipsisFaces;

        explicit Storage(const UiTextLayoutEngineDescriptor &source) : descriptor(source) {
            slots.reserve(source.concurrentResults);
            for (std::uint32_t index = 0; index < source.concurrentResults; ++index)
                slots.push_back(std::make_shared<UiTextLayoutResult::Storage>(source.limits));
            scaledClusterAdvances.reserve(source.limits.clusters);
            clusterPrefix.reserve(static_cast<std::size_t>(source.limits.clusters) + 1U);
            softBreaks.reserve(source.limits.clusters);
            linePlans.reserve(source.limits.lines);
            glyphFaces.reserve(source.limits.glyphs);
            ellipsisFaces.reserve(source.limits.glyphs);
        }

        Storage(const Storage &) = delete;
        Storage &operator=(const Storage &) = delete;
        Storage(Storage &&) = delete;
        Storage &operator=(Storage &&) = delete;

        [[nodiscard]] std::shared_ptr<UiTextLayoutResult::Storage> TryAcquire() noexcept {
            for (std::size_t offset = 0; offset < slots.size(); ++offset) {
                const auto index = (nextSlot + offset) % slots.size();
                std::uint64_t expected{};
                if (!slots[index]->leases.compare_exchange_strong(expected, 1))
                    continue;
                nextSlot = (index + 1) % slots.size();
                return slots[index];
            }
            return {};
        }

        void ReleaseSlot(const std::shared_ptr<UiTextLayoutResult::Storage> &slot) noexcept {
            slot->Reset();
            slot->leases.store(0, std::memory_order_release);
        }

        [[nodiscard]] Result<void> BuildFaceTable(const UiTextShapedTextView &view, std::vector<UiTextFaceId> &output) {
            output.resize(view.glyphs.size());
            for (const auto &run : view.runs)
                for (std::uint32_t glyph = run.firstGlyph; glyph < run.firstGlyph + run.glyphCount; ++glyph)
                    output[glyph] = run.face;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::int64_t> ShapedWidth(const UiTextShapedTextView &view, const UiTextScale scale) const {
            std::int64_t width{};
            for (const auto &cluster : view.clusters) {
                const auto advance = ScaleValue(cluster.advance.x, scale);
                if (advance.HasError())
                    return Result<std::int64_t>::Failure(advance.ErrorValue());
                width += advance.Value();
            }
            return Result<std::int64_t>::Success(width);
        }

        [[nodiscard]] Result<void> BuildLinePlans(const UiTextLayoutRequest &request) {
            const auto &clusters = request.shaped.clusters;
            scaledClusterAdvances.resize(clusters.size());
            clusterPrefix.resize(clusters.size() + 1U);
            clusterPrefix[0] = 0;
            softBreaks.clear();
            for (std::uint32_t index = 0; index < clusters.size(); ++index) {
                const auto advance = ScaleValue(clusters[index].advance.x, request.options.scale);
                if (advance.HasError())
                    return Result<void>::Failure(advance.ErrorValue());
                scaledClusterAdvances[index] = advance.Value();
                clusterPrefix[index + 1U] = clusterPrefix[index] + advance.Value();
                if (clusters[index].breakOpportunity == UiTextBreakOpportunity::Optional)
                    softBreaks.push_back(index + 1U);
            }

            linePlans.clear();
            const auto appendLine = [this](const std::uint32_t first, const std::uint32_t end, const bool hardBreak) -> Result<void> {
                if (linePlans.size() >= descriptor.limits.lines)
                    return Failure(UiErrors::TextLayoutCapacityExceeded);
                linePlans.push_back({first, end - first, clusterPrefix[end] - clusterPrefix[first], hardBreak});
                return Result<void>::Success();
            };

            const auto availableWidth = static_cast<std::int64_t>(request.assignedContent.extent.width);
            std::uint32_t lineStart = 0;
            for (std::uint32_t index = 0; index < clusters.size(); ++index) {
                const auto mandatory = clusters[index].breakOpportunity == UiTextBreakOpportunity::Mandatory;
                while (!mandatory && request.options.wrap != UiTextWrapMode::NoWrap && index > lineStart &&
                       clusterPrefix[index + 1U] - clusterPrefix[lineStart] > availableWidth) {
                    std::uint32_t split = index;
                    if (request.options.wrap == UiTextWrapMode::Word) {
                        const auto upper = std::upper_bound(softBreaks.begin(), softBreaks.end(), index);
                        if (upper != softBreaks.begin()) {
                            const auto candidate = *std::prev(upper);
                            if (candidate > lineStart)
                                split = candidate;
                        }
                    }
                    if (split == lineStart)
                        split = index;
                    if (const auto appended = appendLine(lineStart, split, false); appended.HasError())
                        return appended;
                    lineStart = split;
                }
                if (mandatory) {
                    if (const auto appended = appendLine(lineStart, index + 1U, true); appended.HasError())
                        return appended;
                    lineStart = index + 1U;
                }
            }
            if (lineStart <= clusters.size()) {
                if (lineStart == clusters.size() && !linePlans.empty() && linePlans.back().hardBreak) {
                    if (const auto appended = appendLine(lineStart, lineStart, false); appended.HasError())
                        return appended;
                } else if (lineStart < clusters.size() || linePlans.empty()) {
                    if (const auto appended = appendLine(lineStart, static_cast<std::uint32_t>(clusters.size()), false);
                        appended.HasError())
                        return appended;
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::uint32_t> VisibleLineCount(const UiTextLayoutRequest &request, const std::int32_t lineHeight) const {
            std::uint32_t count = static_cast<std::uint32_t>(linePlans.size());
            if (request.options.maxLines > 0)
                count = std::min(count, request.options.maxLines);
            if (request.options.overflow == UiTextOverflowMode::Ellipsis && lineHeight > 0 &&
                request.assignedContent.extent.height < std::numeric_limits<std::int32_t>::max()) {
                const auto byHeight = std::max<std::int32_t>(1, request.assignedContent.extent.height / lineHeight);
                count = std::min(count, static_cast<std::uint32_t>(byHeight));
            }
            return Result<std::uint32_t>::Success(count);
        }

        [[nodiscard]] Result<void> AppendGlyph(UiTextLayoutResult::Storage &slot, const UiTextFaceId face, const std::uint32_t glyph,
                                               const std::uint32_t cluster, const UiLogicalPoint origin, const UiLogicalPoint advance,
                                               const std::uint32_t line, const bool ellipsis) {
            if (slot.glyphs.size() >= descriptor.limits.glyphs)
                return Failure(UiErrors::TextLayoutCapacityExceeded);
            const auto first = static_cast<std::uint32_t>(slot.glyphs.size());
            if (slot.runs.empty() || slot.runs.back().face != face || slot.runs.back().line != line ||
                slot.runs.back().ellipsis != ellipsis || slot.runs.back().firstGlyph + slot.runs.back().glyphCount != first) {
                if (slot.runs.size() >= descriptor.limits.runs)
                    return Failure(UiErrors::TextLayoutCapacityExceeded);
                slot.runs.push_back({face, first, 0, line, ellipsis});
            }
            slot.glyphs.push_back({face, glyph, cluster, origin, advance});
            ++slot.runs.back().glyphCount;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> BuildOutput(const UiTextLayoutRequest &request, UiTextLayoutResult::Storage &slot,
                                               const std::int32_t lineHeight, const std::uint32_t visibleLines,
                                               const bool sourceTruncated) {
            const auto &clusters = request.shaped.clusters;
            const auto &glyphs = request.shaped.glyphs;
            const auto scale = request.options.scale;
            bool anyHorizontalOverflow = false;
            for (const auto &line : linePlans)
                anyHorizontalOverflow = anyHorizontalOverflow || line.advance > request.assignedContent.extent.width;

            const bool verticallyTruncated = visibleLines < linePlans.size();
            bool needsEllipsis = false;
            if (request.options.overflow == UiTextOverflowMode::Ellipsis)
                needsEllipsis = verticallyTruncated || anyHorizontalOverflow;
            if (needsEllipsis) {
                if (!request.ellipsis.has_value() || !request.ellipsis->IsValid(descriptor.limits) || request.ellipsis->clusters.empty() ||
                    request.ellipsis->glyphs.empty())
                    return Failure(UiErrors::TextLayoutEllipsisInvalid);
                if (const auto faces = BuildFaceTable(*request.ellipsis, ellipsisFaces); faces.HasError())
                    return faces;
            }

            std::int64_t ellipsisWidth{};
            if (needsEllipsis) {
                const auto width = ShapedWidth(*request.ellipsis, scale);
                if (width.HasError())
                    return Result<void>::Failure(width.ErrorValue());
                ellipsisWidth = width.Value();
            }
            std::int64_t fullWidth{};
            for (const auto &line : linePlans)
                fullWidth = std::max(fullWidth, line.advance);
            const auto fullHeight = static_cast<std::int64_t>(linePlans.size()) * lineHeight;
            if (fullWidth > std::numeric_limits<std::int32_t>::max() || fullHeight > std::numeric_limits<std::int32_t>::max())
                return Failure(UiErrors::TextLayoutCapacityExceeded);

            const auto clamp = [](const std::int64_t value, const std::int32_t minimum, const std::int32_t maximum) {
                return static_cast<std::int32_t>(std::clamp(value, static_cast<std::int64_t>(minimum), static_cast<std::int64_t>(maximum)));
            };
            slot.measurement = {{clamp(fullWidth, request.constraints.minimum.width, request.constraints.maximum.width),
                                 clamp(fullHeight, request.constraints.minimum.height, request.constraints.maximum.height)},
                                request.options.wrap != UiTextWrapMode::NoWrap,
                                request.options.maxLines != 0 || request.options.overflow == UiTextOverflowMode::Ellipsis};

            const auto blockHeight = static_cast<std::int64_t>(visibleLines) * lineHeight;
            const auto verticalFree = std::max<std::int64_t>(0, request.assignedContent.extent.height - blockHeight);
            std::int64_t verticalOffset{};
            if (request.options.vertical == UiTextVerticalAlignment::Center)
                verticalOffset = verticalFree / 2;
            else if (request.options.vertical == UiTextVerticalAlignment::Bottom)
                verticalOffset = verticalFree;
            const auto scaledAscent = ScaleValue(request.shaped.metrics.ascent, scale);
            if (scaledAscent.HasError())
                return Result<void>::Failure(scaledAscent.ErrorValue());

            for (std::uint32_t lineIndex = 0; lineIndex < visibleLines; ++lineIndex) {
                const auto &plan = linePlans[lineIndex];
                std::uint32_t start = plan.firstCluster;
                std::uint32_t end = plan.firstCluster + plan.clusterCount;
                if (end > start && clusters[end - 1U].breakOpportunity == UiTextBreakOpportunity::Mandatory &&
                    clusters[end - 1U].glyphCount == 0)
                    --end;

                std::int64_t sourceWidth = clusterPrefix[end] - clusterPrefix[start];
                bool lineEllipsis = false;
                if (request.options.overflow == UiTextOverflowMode::Ellipsis &&
                    (verticallyTruncated && lineIndex + 1U == visibleLines || sourceWidth > request.assignedContent.extent.width)) {
                    lineEllipsis = true;
                    while (end > start && sourceWidth + ellipsisWidth > request.assignedContent.extent.width) {
                        --end;
                        sourceWidth -= scaledClusterAdvances[end];
                    }
                }

                std::int64_t ellipsisUsedWidth{};
                std::uint32_t ellipsisGlyphCount{};
                if (lineEllipsis) {
                    const auto available = std::max<std::int64_t>(0, request.assignedContent.extent.width - sourceWidth);
                    std::uint32_t clusterGlyphs{};
                    for (const auto &cluster : request.ellipsis->clusters)
                        clusterGlyphs += cluster.glyphCount;
                    for (std::uint32_t glyph = 0; glyph < request.ellipsis->glyphs.size() && glyph < clusterGlyphs; ++glyph) {
                        const auto advance = ScaleValue(request.ellipsis->glyphs[glyph].advance.x, scale);
                        if (advance.HasError())
                            return Result<void>::Failure(advance.ErrorValue());
                        if (ellipsisGlyphCount > 0 && ellipsisUsedWidth + advance.Value() > available)
                            break;
                        if (ellipsisGlyphCount == 0 && advance.Value() > available)
                            break;
                        ellipsisUsedWidth += advance.Value();
                        ++ellipsisGlyphCount;
                    }
                }

                const auto lineAdvance = sourceWidth + ellipsisUsedWidth;
                const auto freeWidth = std::max<std::int64_t>(0, request.assignedContent.extent.width - lineAdvance);
                std::int64_t alignmentOffset{};
                if (request.options.horizontal == UiTextHorizontalAlignment::Center)
                    alignmentOffset = freeWidth / 2;
                else if (request.options.horizontal == UiTextHorizontalAlignment::Trailing)
                    alignmentOffset = request.options.direction == UiTextFlowDirection::LeftToRight ? freeWidth : 0;
                else if (request.options.horizontal == UiTextHorizontalAlignment::Leading)
                    alignmentOffset = request.options.direction == UiTextFlowDirection::LeftToRight ? 0 : freeWidth;
                else if (request.options.horizontal == UiTextHorizontalAlignment::Justify && lineIndex + 1U < visibleLines && !lineEllipsis)
                    alignmentOffset = 0;

                const auto lineOriginX = AddValue(request.assignedContent.origin.x, alignmentOffset);
                if (lineOriginX.HasError())
                    return Result<void>::Failure(lineOriginX.ErrorValue());
                const auto lineOriginY =
                    AddValue(request.assignedContent.origin.y, verticalOffset + static_cast<std::int64_t>(lineIndex) * lineHeight);
                if (lineOriginY.HasError())
                    return Result<void>::Failure(lineOriginY.ErrorValue());
                const auto baseline = AddValue(lineOriginY.Value(), scaledAscent.Value());
                if (baseline.HasError())
                    return Result<void>::Failure(baseline.ErrorValue());

                const auto firstGlyph = static_cast<std::uint32_t>(slot.glyphs.size());
                const auto firstRun = static_cast<std::uint32_t>(slot.runs.size());
                std::int64_t cursor = lineOriginX.Value();
                std::uint32_t optionalGaps{};
                if (request.options.horizontal == UiTextHorizontalAlignment::Justify && lineIndex + 1U < visibleLines && !lineEllipsis)
                    for (std::uint32_t cluster = start; cluster < end; ++cluster)
                        optionalGaps += clusters[cluster].breakOpportunity == UiTextBreakOpportunity::Optional ? 1U : 0U;
                const auto justifyExtra = optionalGaps == 0 ? 0 : freeWidth / optionalGaps;
                auto justifyRemainder = optionalGaps == 0 ? 0 : freeWidth % optionalGaps;
                std::uint32_t gapIndex{};
                std::int64_t justificationAdded{};

                const auto appendSourceCluster = [&](const std::uint32_t clusterIndex) -> Result<void> {
                    const auto &cluster = clusters[clusterIndex];
                    for (std::uint32_t glyphOffset = 0; glyphOffset < cluster.glyphCount; ++glyphOffset) {
                        const auto &glyph = glyphs[cluster.firstGlyph + glyphOffset];
                        const auto offsetX = ScaleValue(glyph.offset.x, scale);
                        const auto offsetY = ScaleValue(glyph.offset.y, scale);
                        const auto advanceX = ScaleValue(glyph.advance.x, scale);
                        if (offsetX.HasError() || offsetY.HasError() || advanceX.HasError())
                            return Failure(UiErrors::TextLayoutCapacityExceeded);
                        const auto glyphX = AddValue(cursor, offsetX.Value());
                        const auto glyphY = AddValue(lineOriginY.Value(), offsetY.Value());
                        if (glyphX.HasError() || glyphY.HasError())
                            return Failure(UiErrors::TextLayoutCapacityExceeded);
                        if (const auto appended = AppendGlyph(slot, glyphFaces[cluster.firstGlyph + glyphOffset], glyph.glyph, clusterIndex,
                                                              {glyphX.Value(), glyphY.Value()}, {advanceX.Value(), 0}, lineIndex, false);
                            appended.HasError())
                            return appended;
                        cursor += advanceX.Value();
                    }
                    cursor = lineOriginX.Value() + (clusterPrefix[clusterIndex + 1U] - clusterPrefix[start]) + justificationAdded;
                    if (cluster.breakOpportunity == UiTextBreakOpportunity::Optional && gapIndex < optionalGaps) {
                        const auto addition = justifyExtra + (justifyRemainder-- > 0 ? 1 : 0);
                        cursor += addition;
                        justificationAdded += addition;
                        ++gapIndex;
                    }
                    return Result<void>::Success();
                };

                for (std::uint32_t cluster = start; cluster < end; ++cluster)
                    if (const auto appended = appendSourceCluster(cluster); appended.HasError())
                        return appended;

                if (lineEllipsis) {
                    std::uint32_t glyphOffset{};
                    while (glyphOffset < ellipsisGlyphCount) {
                        const auto &glyph = request.ellipsis->glyphs[glyphOffset];
                        const auto offsetX = ScaleValue(glyph.offset.x, scale);
                        const auto offsetY = ScaleValue(glyph.offset.y, scale);
                        const auto advanceX = ScaleValue(glyph.advance.x, scale);
                        if (offsetX.HasError() || offsetY.HasError() || advanceX.HasError())
                            return Failure(UiErrors::TextLayoutCapacityExceeded);
                        const auto glyphX = AddValue(cursor, offsetX.Value());
                        const auto glyphY = AddValue(lineOriginY.Value(), offsetY.Value());
                        if (glyphX.HasError() || glyphY.HasError())
                            return Failure(UiErrors::TextLayoutCapacityExceeded);
                        if (const auto appended = AppendGlyph(slot, ellipsisFaces[glyphOffset], glyph.glyph, NoUiTextLayoutCluster,
                                                              {glyphX.Value(), glyphY.Value()}, {advanceX.Value(), 0}, lineIndex, true);
                            appended.HasError())
                            return appended;
                        cursor += advanceX.Value();
                        ++glyphOffset;
                    }
                }

                const auto renderedWidth = AddValue(0, cursor - lineOriginX.Value());
                if (renderedWidth.HasError())
                    return Result<void>::Failure(renderedWidth.ErrorValue());
                const auto lineExtentWidth =
                    request.options.horizontal == UiTextHorizontalAlignment::Justify && lineIndex + 1U < visibleLines && !lineEllipsis
                        ? request.assignedContent.extent.width
                        : renderedWidth.Value();
                slot.lines.push_back(
                    {firstGlyph,
                     static_cast<std::uint32_t>(slot.glyphs.size()) - firstGlyph,
                     firstRun,
                     static_cast<std::uint32_t>(slot.runs.size()) - firstRun,
                     {lineOriginX.Value(), lineOriginY.Value()},
                     {lineExtentWidth, lineHeight},
                     baseline.Value(),
                     static_cast<std::int32_t>(std::min<std::int64_t>(lineAdvance, std::numeric_limits<std::int32_t>::max())),
                     plan.hardBreak,
                     lineEllipsis});
            }

            const auto overflowWidth = static_cast<std::int32_t>(fullWidth);
            const auto overflowHeight = static_cast<std::int32_t>(fullHeight);
            const bool clipped = request.options.overflow == UiTextOverflowMode::Clip &&
                                 (fullWidth > request.assignedContent.extent.width || fullHeight > request.assignedContent.extent.height);
            slot.overflow = {request.assignedContent.extent,
                             slot.measurement.desired,
                             {overflowWidth, overflowHeight},
                             clipped,
                             sourceTruncated || verticallyTruncated || needsEllipsis};
            if (!slot.overflow.IsValid())
                return Failure(UiErrors::TextLayoutInputInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::shared_ptr<UiTextLayoutResult::Storage>> Layout(const UiTextLayoutRequest &request) {
            if (lifecycle != UiTextLayoutEngineState::Active)
                return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutLifecycleUnavailable);
            if (request.source.instance != descriptor.instance || request.source.canvas != descriptor.canvas ||
                request.source.document != descriptor.document || request.source.element != descriptor.element)
                return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutSourceStale);
            if (!request.IsValid(descriptor.limits))
                return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutInputInvalid);
            if (hasSource && IsOlder(request.source.revisions, lastSource.revisions))
                return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutSourceStale);

            auto slot = TryAcquire();
            if (!slot)
                return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutStorageExhausted);
            try {
                slot->Reset();
                if (const auto faces = BuildFaceTable(request.shaped, glyphFaces); faces.HasError()) {
                    ReleaseSlot(slot);
                    return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Failure(faces.ErrorValue());
                }
                if (const auto plans = BuildLinePlans(request); plans.HasError()) {
                    ReleaseSlot(slot);
                    return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Failure(plans.ErrorValue());
                }
                const auto naturalLineHeight = static_cast<std::int64_t>(request.shaped.metrics.ascent) +
                                               static_cast<std::int64_t>(request.shaped.metrics.descent) +
                                               static_cast<std::int64_t>(request.shaped.metrics.lineGap);
                const auto unscaledLineHeight = request.options.lineHeight == 0 ? naturalLineHeight : request.options.lineHeight;
                const auto scaledLineHeight = ScaleValue(static_cast<std::int32_t>(unscaledLineHeight), request.options.scale);
                if (scaledLineHeight.HasError()) {
                    ReleaseSlot(slot);
                    return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Failure(scaledLineHeight.ErrorValue());
                }
                const auto visible = VisibleLineCount(request, scaledLineHeight.Value());
                if (visible.HasError()) {
                    ReleaseSlot(slot);
                    return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Failure(visible.ErrorValue());
                }
                const bool sourceTruncated = request.options.maxLines > 0 && request.options.maxLines < linePlans.size();
                if (const auto output = BuildOutput(request, *slot, scaledLineHeight.Value(), visible.Value(), sourceTruncated);
                    output.HasError()) {
                    ReleaseSlot(slot);
                    return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Failure(output.ErrorValue());
                }
                UiTextLayoutRevision publication;
                if (lastRevision.IsValid()) {
                    const auto next = lastRevision.Next();
                    if (next.HasError()) {
                        ReleaseSlot(slot);
                        return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Failure(next.ErrorValue());
                    }
                    publication = next.Value();
                } else {
                    publication = descriptor.initialRevision;
                }
                slot->descriptor = {request.source, publication, request.options};
                if (!slot->descriptor.IsValid() ||
                    !std::ranges::all_of(slot->lines,
                                         [&slot](const UiTextLayoutLine &line) {
                    return line.IsValid(slot->glyphs.size(), slot->runs.size());
                }) ||
                    !std::ranges::all_of(slot->glyphs, &UiTextLayoutGlyph::IsValid) ||
                    !std::ranges::all_of(slot->runs, [&slot](const UiTextLayoutRun &run) {
                    return run.IsValid(slot->glyphs.size(), slot->lines.size());
                })) {
                    ReleaseSlot(slot);
                    return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutInputInvalid);
                }
                lastRevision = publication;
                lastSource = request.source;
                hasSource = true;
                return Result<std::shared_ptr<UiTextLayoutResult::Storage>>::Success(std::move(slot));
            } catch (const std::bad_alloc &) {
                ReleaseSlot(slot);
                return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutCapacityExceeded);
            } catch (...) {
                ReleaseSlot(slot);
                return Failure<std::shared_ptr<UiTextLayoutResult::Storage>>(UiErrors::TextLayoutInputInvalid);
            }
        }

        [[nodiscard]] bool IsDrained() const noexcept {
            return std::ranges::all_of(slots, [](const auto &slot) {
                return slot->leases.load(std::memory_order_acquire) == 0;
            });
        }
    };

    /** @copydoc UiTextLayoutResult::UiTextLayoutResult(std::shared_ptr<const Storage>) */
    UiTextLayoutResult::UiTextLayoutResult(std::shared_ptr<const Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiTextLayoutResult::~UiTextLayoutResult */
    UiTextLayoutResult::~UiTextLayoutResult() {
        Release();
    }

    /** @copydoc UiTextLayoutResult::UiTextLayoutResult(const UiTextLayoutResult &) */
    UiTextLayoutResult::UiTextLayoutResult(const UiTextLayoutResult &other) noexcept : storage_(other.storage_) {
        Retain();
    }

    /** @copydoc UiTextLayoutResult::operator= */
    UiTextLayoutResult &UiTextLayoutResult::operator=(const UiTextLayoutResult &other) noexcept {
        if (this != &other) {
            UiTextLayoutResult replacement{other};
            *this = std::move(replacement);
        }
        return *this;
    }

    /** @copydoc UiTextLayoutResult::UiTextLayoutResult(UiTextLayoutResult &&) */
    UiTextLayoutResult::UiTextLayoutResult(UiTextLayoutResult &&other) noexcept : storage_(std::move(other.storage_)) {}

    /** @copydoc UiTextLayoutResult::operator= */
    UiTextLayoutResult &UiTextLayoutResult::operator=(UiTextLayoutResult &&other) noexcept {
        if (this != &other) {
            Release();
            storage_ = std::move(other.storage_);
        }
        return *this;
    }

    /** @copydoc UiTextLayoutResult::Retain */
    void UiTextLayoutResult::Retain() const noexcept {
        if (!storage_)
            return;
        auto current = storage_->leases.load(std::memory_order_acquire);
        while (current != std::numeric_limits<std::uint64_t>::max()) {
            if (storage_->leases.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel))
                return;
        }
        std::terminate();
    }

    /** @copydoc UiTextLayoutResult::Release */
    void UiTextLayoutResult::Release() noexcept {
        if (!storage_)
            return;
        storage_->leases.fetch_sub(1, std::memory_order_acq_rel);
        storage_.reset();
    }

    /** @copydoc UiTextLayoutResult::Descriptor */
    const UiTextLayoutResultDescriptor &UiTextLayoutResult::Descriptor() const noexcept {
        return storage_->descriptor;
    }

    /** @copydoc UiTextLayoutResult::Measurement */
    const UiLayoutMeasurement &UiTextLayoutResult::Measurement() const noexcept {
        return storage_->measurement;
    }

    /** @copydoc UiTextLayoutResult::Overflow */
    const UiTextLayoutOverflow &UiTextLayoutResult::Overflow() const noexcept {
        return storage_->overflow;
    }

    /** @copydoc UiTextLayoutResult::Lines */
    std::span<const UiTextLayoutLine> UiTextLayoutResult::Lines() const noexcept {
        return storage_->lines;
    }

    /** @copydoc UiTextLayoutResult::Glyphs */
    std::span<const UiTextLayoutGlyph> UiTextLayoutResult::Glyphs() const noexcept {
        return storage_->glyphs;
    }

    /** @copydoc UiTextLayoutResult::Runs */
    std::span<const UiTextLayoutRun> UiTextLayoutResult::Runs() const noexcept {
        return storage_->runs;
    }

    /** @copydoc UiTextLayoutResult::IsValid */
    bool UiTextLayoutResult::IsValid() const noexcept {
        return static_cast<bool>(storage_);
    }

    /** @copydoc UiTextLayoutEngine::Create */
    Result<UiTextLayoutEngine> UiTextLayoutEngine::Create(const UiTextLayoutEngineDescriptor &descriptor) {
        if (!descriptor.IsValid())
            return Failure<UiTextLayoutEngine>(UiErrors::TextLayoutInputInvalid);
        try {
            return Result<UiTextLayoutEngine>::Success(UiTextLayoutEngine{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiTextLayoutEngine>(UiErrors::TextLayoutCapacityExceeded);
        }
    }

    /** @copydoc UiTextLayoutEngine::UiTextLayoutEngine(std::unique_ptr<Storage>) */
    UiTextLayoutEngine::UiTextLayoutEngine(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiTextLayoutEngine::~UiTextLayoutEngine */
    UiTextLayoutEngine::~UiTextLayoutEngine() {
        Close();
    }

    /** @copydoc UiTextLayoutEngine::UiTextLayoutEngine(UiTextLayoutEngine &&) */
    UiTextLayoutEngine::UiTextLayoutEngine(UiTextLayoutEngine &&other) noexcept = default;

    /** @copydoc UiTextLayoutEngine::operator= */
    UiTextLayoutEngine &UiTextLayoutEngine::operator=(UiTextLayoutEngine &&other) noexcept = default;

    /** @copydoc UiTextLayoutEngine::Layout */
    Result<UiTextLayoutResult> UiTextLayoutEngine::Layout(const UiTextLayoutRequest &request) {
        if (!storage_)
            return Failure<UiTextLayoutResult>(UiErrors::TextLayoutLifecycleUnavailable);
        const auto published = storage_->Layout(request);
        if (published.HasError())
            return Result<UiTextLayoutResult>::Failure(published.ErrorValue());
        return Result<UiTextLayoutResult>::Success(UiTextLayoutResult{std::move(published).Value()});
    }

    /** @copydoc UiTextLayoutEngine::Close */
    void UiTextLayoutEngine::Close() noexcept {
        if (!storage_)
            return;
        storage_->lifecycle = UiTextLayoutEngineState::Closed;
        storage_->scaledClusterAdvances.clear();
        storage_->clusterPrefix.clear();
        storage_->softBreaks.clear();
        storage_->linePlans.clear();
        storage_->glyphFaces.clear();
        storage_->ellipsisFaces.clear();
    }

    /** @copydoc UiTextLayoutEngine::Shutdown */
    void UiTextLayoutEngine::Shutdown() noexcept {
        Close();
    }

    /** @copydoc UiTextLayoutEngine::IsDrained */
    bool UiTextLayoutEngine::IsDrained() const noexcept {
        return !storage_ || storage_->IsDrained();
    }

    /** @copydoc UiTextLayoutEngine::State */
    UiTextLayoutEngineState UiTextLayoutEngine::State() const noexcept {
        return storage_ ? storage_->lifecycle : UiTextLayoutEngineState::Closed;
    }
}  // namespace Horo::Runtime::Ui
