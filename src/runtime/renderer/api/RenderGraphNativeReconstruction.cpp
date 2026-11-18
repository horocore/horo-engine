#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Render {
    namespace {
        using Image = NativeReconstructionImage;

        /** @brief Bounds every integer sampled coordinate and checks float constants before any native recording. */
        bool ValidDispatch(const NativeReconstructionDispatchPlan &plan) noexcept {
            const auto &dispatch = plan.constants;
            constexpr auto texelLimit = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max() / 2);
            if (plan.entry > NativeReconstructionEntry::TemporalHistory || !std::ranges::all_of(dispatch.extents, [](auto extent) {
                return extent != 0 && extent <= texelLimit;
            }))
                return false;
            if (!std::ranges::all_of(dispatch.sample,
                                     [](float value) {
                return std::isfinite(value);
            }) ||
                dispatch.sample[2] <= 0.0F || dispatch.sample[3] < 0.0F || dispatch.sample[3] >= 1.0F)
                return false;
            if (!std::isfinite(dispatch.depth[0]) || dispatch.depth[0] < 0.0F || !std::isfinite(dispatch.depth[1]) ||
                dispatch.depth[1] < 0.0F || dispatch.depth[1] > 1.0F || dispatch.depth[2] != 0.0F || dispatch.depth[3] != 0.0F)
                return false;
            const bool readsHistory = plan.entry == NativeReconstructionEntry::TemporalHistory;
            return readsHistory
                       ? dispatch.sample[3] > 0.0F
                       : dispatch.sample[2] == 1.0F && dispatch.sample[3] == 0.0F && dispatch.depth[0] == 0.0F && dispatch.depth[1] == 0.0F;
        }

        /** @brief Verifies one exact whole texture and graph-local identity before accessing its authored uses. */
        bool MatchesImageUse(const RenderGraphExecutionRequest &request, std::span<const RenderGraphResourceUsage> uses, Image role,
                             const RenderGraphReconstructionImage &image) {
            const auto id = image.resource;
            if (!image.view.IsValid() || id.owner != request.graph.Owner() || !id.IsValid() || id.value > request.graph.Resources().size())
                return false;
            if (const auto &texture = request.graph.Resources()[id.value - 1];
                texture.id != id || texture.kind != RenderGraphResourceKind::Texture)
                return false;
            const bool write = NativeReconstructionWritesImage(role);
            const auto usage = std::ranges::find(uses, id, &RenderGraphResourceUsage::resource);
            return usage != uses.end() && usage->kind == (write ? RenderGraphUsageKind::Storage : RenderGraphUsageKind::Sampled) &&
                   usage->access == (write ? RenderGraphAccess::Write : RenderGraphAccess::Read);
        }

        /** @brief Separates source guide precision from the exact resolved history/write formats. */
        bool MatchesFormat(Image role, RenderTextureFormat format) noexcept {
            using enum RenderTextureFormat;
            switch (role) {
                case Image::CurrentColor:
                    return format == Rgba16Float || format == Rgba32Float;
                case Image::OutputColor:
                case Image::PreviousColor:
                    return format == Rgba32Float;
                case Image::CurrentDepth:
                    return format == R16Float || format == R32Float;
                case Image::PreviousDepth:
                case Image::OutputDepth:
                    return format == R32Float;
                case Image::Motion:
                    return format == Rg16Float || format == Rg32Float;
                case Image::Count:
                    return false;
            }
            return false;
        }
    }  // namespace

    /** @copydoc ValidateNativeReconstructionGraphWorkload */
    Result<void> ValidateNativeReconstructionGraphWorkload(const RenderGraphExecutionRequest &request, const RenderGraphExecutionPass &pass,
                                                           const RenderGraphNativeReconstruction &workload) {
        const auto invalid = [] {
            return Result<void>::Failure(MakeError(RenderGraphExecutionErrors::InvalidGraph));
        };
        if (!workload.kernel || pass.kind != RenderPassKind::Compute || !ValidDispatch(workload.dispatch) ||
            !pass.usages.IsValidFor(request.graph.Usages().size()))
            return invalid();
        const auto uses = request.graph.Usages().subspan(pass.usages.offset, pass.usages.count);
        std::array<RenderGraphResourceId, RenderGraphNativeReconstruction::ImageCount> used;
        std::size_t count{};
        for (std::size_t i = 0; i < workload.images.size(); ++i) {
            const auto role = static_cast<Image>(i);
            const auto &binding = workload.images[i];
            if (!NativeReconstructionUsesImage(workload.dispatch.entry, role)) {
                if (binding.resource != RenderGraphResourceId{} || binding.view != RenderTextureViewHandle{})
                    return invalid();
                continue;
            }
            const auto prior = std::span{used}.first(count);
            if (std::ranges::find(prior, binding.resource) != prior.end() || !MatchesImageUse(request, uses, role, binding))
                return invalid();
            used[count++] = binding.resource;
        }
        return count == uses.size() ? Result<void>::Success() : invalid();
    }

    /** @copydoc MatchesNativeReconstructionImage */
    bool MatchesNativeReconstructionImage(const NativeReconstructionDispatchPlan &dispatch, Image role,
                                          const RenderTextureDescriptor &descriptor) noexcept {
        if (!NativeReconstructionUsesImage(dispatch.entry, role) || !descriptor.IsValid() || !MatchesFormat(role, descriptor.format))
            return false;
        const bool source = role == Image::CurrentColor || role == Image::CurrentDepth || role == Image::Motion;
        const auto &sizes = dispatch.constants.extents;
        const auto extent = source ? FramebufferExtent{sizes[0], sizes[1]} : FramebufferExtent{sizes[2], sizes[3]};
        const auto usage =
            NativeReconstructionWritesImage(role) ? RenderTextureUsage::Storage | RenderTextureUsage::Sampled : RenderTextureUsage::Sampled;
        return descriptor.extent == extent && descriptor.dimension == RenderTextureDimension::TwoD && descriptor.depth == 1 &&
               descriptor.layerCount == 1 && descriptor.mipCount == 1 && descriptor.sampleCount == 1 &&
               HasTextureUsage(descriptor.usage, usage);
    }
}  // namespace Horo::Render
