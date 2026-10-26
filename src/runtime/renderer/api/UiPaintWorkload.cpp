#include "Horo/Runtime/Render/UiPaintWorkload.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "UiPaintParameterValidation.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace Horo::Render {
    namespace {
        Result<void> Invalid() {
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        }

        /** @brief Checks the complete prepared table before looking up any dynamic draw selection. */
        bool ValidSamples(const RenderGraphExecutionRequest &request, const RenderGraphUiPaint &paint) {
            if (paint.sampledResources.size() > UiPaintLimits::MaximumSampledResources)
                return false;
            std::uint32_t previous{};
            for (const auto id : paint.sampledResources) {
                if (id.owner != request.graph.Owner() || id.value <= previous || id.value > request.graph.Resources().size() ||
                    id == paint.color || id == paint.stencil ||
                    request.graph.Resources()[id.value - 1].kind != RenderGraphResourceKind::Texture)
                    return false;
                previous = id.value;
            }
            return true;
        }

        using FixedPaintResources = std::array<RenderGraphResourceId, 5>;
        constexpr std::array FixedPaintKinds{RenderGraphUsageKind::VertexBuffer, RenderGraphUsageKind::IndexBuffer,
                                             RenderGraphUsageKind::UniformBuffer, RenderGraphUsageKind::ColorAttachment,
                                             RenderGraphUsageKind::DepthStencilAttachment};

        /** @brief Rejects foreign, out-of-range or repeated fixed resources before graph-use lookup. */
        bool ValidFixedResources(const RenderGraphExecutionRequest &request, const FixedPaintResources &ids) {
            for (std::size_t index = 0; index < ids.size(); ++index) {
                if (!ids[index].IsValid())
                    continue;
                if (ids[index].owner != request.graph.Owner() || ids[index].value > request.graph.Resources().size())
                    return false;
                if (const auto prior = std::span{ids}.first(index); std::ranges::find(prior, ids[index]) != prior.end())
                    return false;
            }
            return true;
        }

        /** @brief Selects the exact access for a sampled input, buffer, cleared attachment or preserved attachment. */
        RenderGraphAccess PaintAccess(const RenderGraphUiPaint &paint, const std::size_t index, const bool sampled) noexcept {
            using enum RenderGraphAccess;
            if (sampled || index < 3)
                return Read;
            if ((index == 3 && paint.clearColor) || (index == 4 && paint.clearStencil))
                return Write;
            return ReadWrite;
        }

        /** @brief Checks one already ordered graph use against the closed prepared resource categories. */
        bool MatchesUse(const RenderGraphExecutionRequest &request, const RenderGraphResourceUsage &use, const RenderGraphUiPaint &paint,
                        const FixedPaintResources &ids) {
            const auto found = std::ranges::find(ids, use.resource);
            const auto index = static_cast<std::size_t>(found - ids.begin());
            const bool sampled = found == ids.end();
            if (sampled && !std::ranges::binary_search(paint.sampledResources, use.resource))
                return false;
            const auto kind = sampled ? RenderGraphUsageKind::Sampled : FixedPaintKinds[index];
            const auto resourceKind = !sampled && index < 3 ? RenderGraphResourceKind::Buffer : RenderGraphResourceKind::Texture;
            return use.kind == kind && use.access == PaintAccess(paint, index, sampled) &&
                   request.graph.Resources()[use.resource.value - 1].kind == resourceKind;
        }

        /** @brief Exact sorted graph-use admission is linear in pass uses with logarithmic sampled-table lookup. */
        bool MatchesUses(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                         const RenderGraphUiPaint &paint) {
            const FixedPaintResources ids{paint.vertices, paint.indices, paint.parameters, paint.color, paint.stencil};
            if (!ValidFixedResources(request, ids))
                return false;
            const auto fixed = std::ranges::count_if(ids, [](const auto id) {
                return id.IsValid();
            });
            const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
            if (uses.size() != paint.sampledResources.size() + static_cast<std::size_t>(fixed) || !ValidSamples(request, paint))
                return false;
            std::uint32_t previous{};
            for (const auto &use : uses) {
                if (use.resource.owner != request.graph.Owner() || use.resource.value <= previous)
                    return false;
                previous = use.resource.value;
                if (!MatchesUse(request, use, paint, ids))
                    return false;
            }
            return true;
        }

        /** @brief Validates the complete stencil program before encoding; no pass can leak an unmatched mask. */
        bool ValidDraws(const RenderGraphUiPaint &paint) {
            using enum UiPaintSampling;
            using enum UiPaintOperation;
            std::uint8_t depth = 0;
            for (const auto &draw : paint.draws) {
                if (draw.indexCount == 0 || draw.indexCount % 3 != 0 || draw.firstIndex > paint.indexCount ||
                    draw.indexCount > paint.indexCount - draw.firstIndex || !draw.scissor.Fits(paint.extent) ||
                    !std::isfinite(draw.opacity) || draw.opacity < 0.0F || draw.opacity > 1.0F || draw.operation > MultiplyCoverage ||
                    draw.sampling > RoundedRectangle || draw.filter > RenderSamplerFilter::Linear ||
                    draw.magFilter > RenderSamplerFilter::Linear || draw.mipmap > UiPaintMipmap::Linear || draw.stencilDepth != depth ||
                    !Detail::ValidUiPaintMask(draw.maskExtent, draw.cornerRadii) ||
                    ((draw.sampling == Color || draw.sampling == Alpha || draw.sampling == Premultiplied) &&
                     draw.sampled >= paint.sampledResources.size()) ||
                    ((draw.operation == PushStencil || draw.operation == PopStencil) &&
                     (draw.sampling != Solid || draw.coverage != NoUiPaintTexture)) ||
                    (draw.coverage != NoUiPaintTexture && draw.coverage >= paint.sampledResources.size()))
                    return false;
                if (draw.operation == PushStencil) {
                    if (!paint.stencil.IsValid() || depth == UiPaintLimits::MaximumStencilDepth)
                        return false;
                    ++depth;
                } else if (draw.operation == PopStencil) {
                    if (!paint.stencil.IsValid() || depth == 0)
                        return false;
                    --depth;
                }
            }
            return depth == 0;
        }
    }  // namespace

    /** @copydoc UiPaintScissor::Fits */
    bool UiPaintScissor::Fits(const FramebufferExtent target) const noexcept {
        return x <= target.width && y <= target.height && width <= target.width - x && height <= target.height - y;
    }

    /** @copydoc ValidateUiPaintWorkload */
    Result<void> ValidateUiPaintWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                         const RenderGraphUiPaint &paint) {
        if (paint.frameRevision == 0 || paint.firstParameter > UiPaintLimits::MaximumDraws ||
            paint.draws.size() > UiPaintLimits::MaximumDraws - paint.firstParameter || pass.kind != RenderPassKind::Graphics ||
            !pass.usages.IsValidFor(request.graph.Usages().size()) || !paint.kernel || !paint.vertices.IsValid() ||
            !paint.indices.IsValid() || !paint.parameters.IsValid() || paint.extent.width == 0 || paint.extent.height == 0 ||
            paint.vertexCount == 0 || paint.vertexCount > UiPaintLimits::MaximumVertices || paint.indexCount == 0 ||
            paint.indexCount > UiPaintLimits::MaximumIndices || paint.activity > UiPaintPassActivity::Hidden ||
            (paint.draws.empty() != (paint.activity == UiPaintPassActivity::Hidden)) || paint.draws.size() > UiPaintLimits::MaximumDraws ||
            (paint.clearColor && !paint.color.IsValid()) || (paint.clearStencil && !paint.stencil.IsValid()) ||
            !MatchesUses(request, pass, paint) || !ValidDraws(paint))
            return Invalid();
        return Result<void>::Success();
    }
}  // namespace Horo::Render
