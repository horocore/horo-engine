#include "Horo/Runtime/Ui/UiTextUnicode.h"
#include "UiTextLayoutInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime::Ui {
    /** @copydoc UiTextLayoutEngine::Storage::AppendPreparedCluster */
    Result<void> UiTextLayoutEngine::Storage::AppendPreparedCluster(const UiTextShape &shape, const UiTextUnicodeAnalysis &unicode,
                                                                    const std::uint32_t clusterIndex) {
        using UiTextLayoutInternal::Failure;
        const auto &cluster = shape.Clusters()[clusterIndex];
        const auto evidence = unicode.Scalars();
        const auto beginning = std::ranges::lower_bound(evidence, cluster.byteStart, {}, &UiTextUnicodeScalar::byteStart);
        const auto ending = std::ranges::lower_bound(evidence, cluster.byteEnd, {}, &UiTextUnicodeScalar::byteEnd);
        if (beginning == evidence.end() || ending == evidence.end() || beginning->byteStart != cluster.byteStart ||
            ending->byteEnd != cluster.byteEnd || beginning->level != cluster.bidiLevel || ending->breakAfter != cluster.breakAfter)
            return Failure(UiErrors::TextLayoutSourceStale);
        const auto first = static_cast<std::uint32_t>(preparedGlyphs.size());
        const auto direction = (cluster.bidiLevel & 1U) != 0 ? UiTextFlowDirection::RightToLeft : UiTextFlowDirection::LeftToRight;
        for (std::uint32_t offset = 0; offset < cluster.glyphCount; ++offset) {
            const auto &glyph = shape.Glyphs()[cluster.firstGlyph + offset];
            if (glyph.advance.x < 0 || glyph.advance.y != 0)
                return Failure(UiErrors::TextLayoutInputInvalid);
            const auto face = UiTextFaceId::Create(glyph.face.Bytes());
            if (face.HasError())
                return Result<void>::Failure(face.ErrorValue());
            if (!preparedRuns.empty() && preparedRuns.back().face == face.Value() && preparedRuns.back().direction == direction)
                ++preparedRuns.back().glyphCount;
            else {
                if (preparedRuns.size() >= descriptor.limits.runs)
                    return Failure(UiErrors::TextLayoutCapacityExceeded);
                preparedRuns.push_back({face.Value(), static_cast<std::uint32_t>(preparedGlyphs.size()), 1, direction});
            }
            preparedGlyphs.push_back({glyph.glyph, glyph.offset, glyph.advance});
        }
        const auto opportunity = cluster.breakAfter == UiTextUnicodeBreak::Mandatory  ? UiTextBreakOpportunity::Mandatory
                                 : cluster.breakAfter == UiTextUnicodeBreak::Optional ? UiTextBreakOpportunity::Optional
                                                                                      : UiTextBreakOpportunity::None;
        preparedClusters.push_back({cluster.byteStart, cluster.byteEnd, cluster.glyphCount == 0 ? NoUiTextLayoutCluster : first,
                                    cluster.glyphCount, cluster.advance, opportunity, cluster.bidiLevel});
        return Result<void>::Success();
    }

    /** @copydoc UiTextLayoutEngine::LayoutShaped */
    Result<UiTextLayoutResult> UiTextLayoutEngine::LayoutShaped(UiTextLayoutRequest request, const UiTextShape &shape,
                                                                const UiTextUnicodeAnalysis &unicode, UiTextUnicodeAnalyzer &analyzer) {
        using UiTextLayoutInternal::Failure;
        if (!storage_ || storage_->lifecycle != UiTextLayoutEngineState::Active)
            return Failure<UiTextLayoutResult>(UiErrors::TextLayoutLifecycleUnavailable);
        if (!shape.IsValid() || !unicode.IsValid() || shape.Text() != unicode.Text() || shape.Descriptor().content != unicode.Content() ||
            shape.Descriptor().language != unicode.Locale() || shape.Descriptor().ownership != request.source.instance.ownership ||
            shape.Descriptor().content.Value() != request.source.revisions.content.Value())
            return Failure<UiTextLayoutResult>(UiErrors::TextLayoutSourceStale);
        const auto &limits = storage_->descriptor.limits;
        if (shape.Text().size() > limits.sourceBytes || shape.Clusters().size() > limits.clusters || shape.Glyphs().size() > limits.glyphs)
            return Failure<UiTextLayoutResult>(UiErrors::TextLayoutCapacityExceeded);
        auto &runs = storage_->preparedRuns;
        auto &glyphs = storage_->preparedGlyphs;
        auto &clusters = storage_->preparedClusters;
        runs.clear();
        glyphs.clear();
        clusters.clear();
        for (std::uint32_t index = 0; index < shape.Clusters().size(); ++index)
            if (const auto appended = storage_->AppendPreparedCluster(shape, unicode, index); appended.HasError())
                return Result<UiTextLayoutResult>::Failure(appended.ErrorValue());
        const auto content = UiLayoutContentRevision::Create(shape.Descriptor().content.Value());
        if (content.HasError())
            return Result<UiTextLayoutResult>::Failure(content.ErrorValue());
        request.shaped = {content.Value(),
                          static_cast<std::uint32_t>(shape.Text().size()),
                          {shape.Metrics().ascent, shape.Metrics().descent, shape.Metrics().lineGap},
                          runs,
                          glyphs,
                          clusters};
        request.unicode = &unicode;
        request.unicodeAnalyzer = &analyzer;
        return Layout(request);
    }
}  // namespace Horo::Runtime::Ui
