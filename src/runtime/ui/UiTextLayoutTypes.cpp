#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiTextLayout.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T = void> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] constexpr bool IsKnown(const Enum value, const Enum last) noexcept {
            return value < last;
        }

        [[nodiscard]] bool SameOwner(const RuntimeUiInstanceId instance, const UiCanvasInstanceId canvas,
                                     const UiElementHandle element) noexcept {
            return instance.IsValid() && canvas.IsValid() && element.IsValid() && instance.ownership == canvas.ownership &&
                   instance.ownership == element.ownership;
        }

        [[nodiscard]] bool FitsRange(const std::uint32_t first, const std::uint32_t count, const std::size_t size) noexcept {
            return first <= size && count <= size - first;
        }
    }  // namespace

    /** @copydoc UiTextScale::Create */
    Result<UiTextScale> UiTextScale::Create(const std::uint32_t value) {
        if (value == 0 || value > MaximumUiTextLayoutScale)
            return Failure<UiTextScale>(UiErrors::TextLayoutInputInvalid);
        return Result<UiTextScale>::Success({value});
    }

    /** @copydoc UiTextScale::IsValid */
    bool UiTextScale::IsValid() const noexcept {
        return value > 0 && value <= MaximumUiTextLayoutScale;
    }

    /** @copydoc UiTextLayoutOptions::IsValid */
    bool UiTextLayoutOptions::IsValid() const noexcept {
        return IsKnown(wrap, UiTextWrapMode::Count) && IsKnown(overflow, UiTextOverflowMode::Count) &&
               IsKnown(horizontal, UiTextHorizontalAlignment::Count) && IsKnown(vertical, UiTextVerticalAlignment::Count) &&
               IsKnown(direction, UiTextFlowDirection::Count) && scale.IsValid() && lineHeight >= 0 && maxLines <= MaximumUiTextLayoutLines;
    }

    /** @copydoc UiTextShapedGlyph::IsValid */
    bool UiTextShapedGlyph::IsValid() const noexcept {
        return advance.x >= 0 && advance.y == 0;
    }

    /** @copydoc UiTextShapedRun::IsValid */
    bool UiTextShapedRun::IsValid(const std::size_t glyphCountLimit) const noexcept {
        return face.IsValid() && IsKnown(direction, UiTextFlowDirection::Count) && glyphCount > 0 &&
               FitsRange(firstGlyph, glyphCount, glyphCountLimit);
    }

    /** @copydoc UiTextShapedCluster::IsValid */
    bool UiTextShapedCluster::IsValid(const std::size_t sourceBytesLimit, const std::size_t glyphCountLimit) const noexcept {
        const bool glyphRange = glyphCount == 0 ? firstGlyph == NoUiTextLayoutCluster : FitsRange(firstGlyph, glyphCount, glyphCountLimit);
        return byteStart < byteEnd && byteEnd <= sourceBytesLimit && glyphRange && advance.x >= 0 && advance.y == 0 &&
               IsKnown(breakOpportunity, UiTextBreakOpportunity::Count);
    }

    /** @copydoc UiTextShapedMetrics::IsValid */
    bool UiTextShapedMetrics::IsValid() const noexcept {
        const auto lineHeight = static_cast<std::int64_t>(ascent) + static_cast<std::int64_t>(descent) + static_cast<std::int64_t>(lineGap);
        return ascent >= 0 && descent >= 0 && lineGap >= 0 && lineHeight <= std::numeric_limits<std::int32_t>::max();
    }

    /** @copydoc UiTextLayoutLimits::IsValid */
    bool UiTextLayoutLimits::IsValid() const noexcept {
        return sourceBytes > 0 && sourceBytes <= MaximumUiTextLayoutSourceBytes && clusters > 0 &&
               clusters <= MaximumUiTextLayoutClusters && glyphs > 0 && glyphs <= MaximumUiTextLayoutGlyphs && lines > 0 &&
               lines <= MaximumUiTextLayoutLines && runs > 0 && runs <= MaximumUiTextLayoutRuns;
    }

    /** @copydoc UiTextShapedTextView::IsValid */
    bool UiTextShapedTextView::IsValid(const UiTextLayoutLimits &limits) const noexcept {
        if (!limits.IsValid() || !content.IsValid() || sourceBytes > limits.sourceBytes || runs.size() > limits.runs ||
            glyphs.size() > limits.glyphs || clusters.size() > limits.clusters || !metrics.IsValid())
            return false;

        std::size_t expectedGlyph = 0;
        for (const auto &run : runs) {
            if (!run.IsValid(glyphs.size()) || run.firstGlyph != expectedGlyph)
                return false;
            expectedGlyph += run.glyphCount;
        }
        if (expectedGlyph != glyphs.size() || !std::ranges::all_of(glyphs, &UiTextShapedGlyph::IsValid))
            return false;

        std::uint32_t previousByteEnd = 0;
        expectedGlyph = 0;
        for (const auto &cluster : clusters) {
            if (!cluster.IsValid(sourceBytes, glyphs.size()) || cluster.byteStart < previousByteEnd)
                return false;
            previousByteEnd = cluster.byteEnd;
            if (cluster.glyphCount == 0) {
                if (cluster.firstGlyph != NoUiTextLayoutCluster)
                    return false;
            } else if (cluster.firstGlyph != expectedGlyph) {
                return false;
            } else {
                expectedGlyph += cluster.glyphCount;
            }
        }
        return expectedGlyph == glyphs.size();
    }

    /** @copydoc UiTextLayoutSource::IsValid */
    bool UiTextLayoutSource::IsValid() const noexcept {
        return SameOwner(instance, canvas, element) && document.IsValid() && revisions.IsValid() && revisions.document.IsValid() &&
               revisions.tree.IsValid();
    }

    /** @copydoc UiTextLayoutRequest::IsValid */
    bool UiTextLayoutRequest::IsValid(const UiTextLayoutLimits &limits) const noexcept {
        if (!source.IsValid() || !assignedContent.IsValid() || !constraints.IsValid() || !options.IsValid() || !shaped.IsValid(limits) ||
            shaped.content != source.revisions.content)
            return false;
        if (assignedContent.extent.width < constraints.minimum.width || assignedContent.extent.width > constraints.maximum.width ||
            assignedContent.extent.height < constraints.minimum.height || assignedContent.extent.height > constraints.maximum.height)
            return false;
        return !ellipsis.has_value() || (ellipsis->IsValid(limits) && ellipsis->content == shaped.content);
    }

    /** @copydoc UiTextLayoutLine::IsValid */
    bool UiTextLayoutLine::IsValid(const std::size_t glyphCountLimit, const std::size_t runCountLimit) const noexcept {
        return FitsRange(firstGlyph, glyphCount, glyphCountLimit) && FitsRange(firstRun, runCount, runCountLimit) && extent.IsValid() &&
               baseline >= NoUiBaseline && advance >= 0;
    }

    /** @copydoc UiTextLayoutGlyph::IsValid */
    bool UiTextLayoutGlyph::IsValid() const noexcept {
        return face.IsValid() && advance.x >= 0 && advance.y == 0;
    }

    /** @copydoc UiTextLayoutRun::IsValid */
    bool UiTextLayoutRun::IsValid(const std::size_t glyphCountLimit, const std::size_t lineCountLimit) const noexcept {
        return face.IsValid() && glyphCount > 0 && FitsRange(firstGlyph, glyphCount, glyphCountLimit) && line < lineCountLimit;
    }

    /** @copydoc UiTextLayoutOverflow::IsValid */
    bool UiTextLayoutOverflow::IsValid() const noexcept {
        return assigned.IsValid() && desired.IsValid() && overflow.IsValid();
    }

    /** @copydoc UiTextLayoutResultDescriptor::IsValid */
    bool UiTextLayoutResultDescriptor::IsValid() const noexcept {
        return source.IsValid() && revision.IsValid() && options.IsValid();
    }

    /** @copydoc UiTextLayoutEngineDescriptor::IsValid */
    bool UiTextLayoutEngineDescriptor::IsValid() const noexcept {
        return SameOwner(instance, canvas, element) && document.IsValid() && limits.IsValid() && concurrentResults >= 2 &&
               concurrentResults <= MaximumUiTextLayoutResultsInFlight && initialRevision.IsValid();
    }
}  // namespace Horo::Runtime::Ui
