#include "UiPaintProgramInternal.h"

#include <algorithm>
#include <cmath>

namespace Horo::Render {
    using namespace Runtime::Ui;

    /** @copydoc UiPaintProgram::Storage::Quad */
    Result<std::uint32_t> UiPaintProgram::Storage::Quad(Detail::UiPaintCpuFrame &frame, UiLogicalRect rect,
                                                        const UiLogicalTransform &transform, bool targetUv) {
        using Added = Result<std::uint32_t>;
        if (frame.vertices.size() + 4 > descriptor.capacity.vertices || frame.indices.size() + 6 > descriptor.capacity.indices)
            return Added::Failure(MakeError(UiPaintProgramErrors::Capacity));
        const auto firstVertex = static_cast<std::uint32_t>(frame.vertices.size());
        const auto firstIndex = static_cast<std::uint32_t>(frame.indices.size());
        const auto &m = transform.values;
        const auto safe = descriptor.projection.ContentPixelRect();
        const double scale =
            static_cast<double>(descriptor.projection.pixelsPerDip.pixelUnits) / (64.0 * descriptor.projection.pixelsPerDip.logicalDips);
        constexpr std::array<std::array<float, 2>, 4> corners{{{0, 0}, {1, 0}, {1, 1}, {0, 1}}};
        for (const auto uv : corners) {
            const double x = static_cast<double>(rect.origin.x) + uv[0] * rect.extent.width;
            const double y = static_cast<double>(rect.origin.y) + uv[1] * rect.extent.height;
            const float px = static_cast<float>((m[0] * x + m[1] * y + m[4]) * scale + safe.x);
            const float py = static_cast<float>((m[2] * x + m[3] * y + m[5]) * scale + safe.y);
            if (!std::isfinite(px) || !std::isfinite(py))
                return Added::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
            frame.vertices.push_back(
                {{px, py},
                 targetUv ? std::array{px / descriptor.projection.pixelExtent.width, py / descriptor.projection.pixelExtent.height} : uv,
                 {1, 1, 1, 1}});
        }
        for (const auto index : std::array<std::uint32_t, 6>{0, 1, 2, 0, 2, 3})
            frame.indices.push_back(firstVertex + index);
        return Added::Success(firstIndex);
    }

    /** @copydoc UiPaintProgram::Storage::Scissor */
    Result<UiPaintScissor> UiPaintProgram::Storage::Scissor(const UiRenderSnapshot &snapshot, std::uint32_t clip) const {
        const auto safe = descriptor.projection.ContentPixelRect();
        double left = safe.x, top = safe.y, right = safe.x + safe.width, bottom = safe.y + safe.height;
        const double scale =
            static_cast<double>(descriptor.projection.pixelsPerDip.pixelUnits) / (64.0 * descriptor.projection.pixelsPerDip.logicalDips);
        std::uint32_t steps{};
        while (clip != NoUiRenderIndex) {
            if (clip >= snapshot.Clips().size() || ++steps > MaximumUiRenderClips)
                return Result<UiPaintScissor>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
            const auto &node = snapshot.Clips()[clip];
            left = std::max(left, safe.x + node.rect.origin.x * scale);
            top = std::max(top, safe.y + node.rect.origin.y * scale);
            right = std::min(right, safe.x + (static_cast<double>(node.rect.origin.x) + node.rect.extent.width) * scale);
            bottom = std::min(bottom, safe.y + (static_cast<double>(node.rect.origin.y) + node.rect.extent.height) * scale);
            clip = node.parent;
        }
        const auto x =
            static_cast<std::uint32_t>(std::clamp(std::floor(left), 0.0, static_cast<double>(descriptor.projection.pixelExtent.width)));
        const auto y =
            static_cast<std::uint32_t>(std::clamp(std::floor(top), 0.0, static_cast<double>(descriptor.projection.pixelExtent.height)));
        const auto endX = static_cast<std::uint32_t>(
            std::clamp(std::ceil(right), static_cast<double>(x), static_cast<double>(descriptor.projection.pixelExtent.width)));
        const auto endY = static_cast<std::uint32_t>(
            std::clamp(std::ceil(bottom), static_cast<double>(y), static_cast<double>(descriptor.projection.pixelExtent.height)));
        return Result<UiPaintScissor>::Success({x, y, endX - x, endY - y});
    }

    /** @copydoc UiPaintProgram::Storage::Sample */
    Result<std::uint32_t> UiPaintProgram::Storage::Sample(const Detail::UiPaintSlotProgram &slot, std::uint32_t pass,
                                                          RenderGraphResourceId resource) const {
        const auto &table = slot.sampled[pass];
        const auto found = std::ranges::lower_bound(table, resource.value, {}, &RenderGraphResourceId::value);
        if (found == table.end() || *found != resource)
            return Result<std::uint32_t>::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
        return Result<std::uint32_t>::Success(static_cast<std::uint32_t>(found - table.begin()));
    }

    /** @copydoc UiPaintProgram::Storage::SourceSample */
    Result<std::uint32_t> UiPaintProgram::Storage::SourceSample(const Detail::UiPaintSlotProgram &slot, std::uint32_t pass,
                                                                const UiRenderSubmission &submission, std::uint32_t reference,
                                                                const UiPositionedGlyph *glyph) const {
        const auto resources = submission.geometry->SourceSnapshot().Resources();
        if (reference >= resources.size())
            return Result<std::uint32_t>::Failure(MakeError(UiPaintFrameErrors::InvalidInput));
        const auto &source = resources[reference];
        for (const auto &image : submission.images)
            if (image.realization && image.realization->Source().asset == source.asset &&
                image.realization->Source().revision == source.revision && image.realization->Source().role == source.role)
                return Sample(slot, pass, image.texture);
        for (const auto &font : submission.fonts)
            if (glyph && font.face && font.realization && font.face->SourceAsset() == source.asset &&
                font.face->Revision().Value() == source.revision.Value() &&
                submission.atlas.PinsGlyph(*font.face, glyph->glyph, font.realization->Page(), glyph->uv))
                return Sample(slot, pass, font.texture);
        return Result<std::uint32_t>::Failure(MakeError(UiPaintProgramErrors::PreparationRequired));
    }

    /** @copydoc UiPaintProgram::Storage::Draw */
    Result<void> UiPaintProgram::Storage::Draw(Detail::UiPaintCpuFrame &frame, const UiPaintDraw &draw) {
        if (frame.draws.size() >= descriptor.capacity.parameters || frame.draws.size() >= UiPaintLimits::MaximumDraws)
            return Result<void>::Failure(MakeError(UiPaintProgramErrors::Capacity));
        frame.parameters.push_back({.extent = {static_cast<float>(descriptor.projection.pixelExtent.width),
                                               static_cast<float>(descriptor.projection.pixelExtent.height)},
                                    .opacity = draw.opacity,
                                    .sampling = static_cast<std::uint32_t>(draw.sampling),
                                    .flags = draw.coverage == NoUiPaintTexture ? 0U : 1U,
                                    .maskExtent = draw.maskExtent,
                                    .cornerRadii = draw.cornerRadii});
        frame.draws.push_back(draw);
        return Result<void>::Success();
    }
}  // namespace Horo::Render
