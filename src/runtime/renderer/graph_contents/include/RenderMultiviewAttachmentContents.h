#pragma once
/** @file RenderMultiviewAttachmentContents.h
 * @brief Non-installed whole-resource contents proof for an already admitted multiview attachment write.
 */
#include "Horo/Runtime/Render/MultiviewGraphWorkload.h"

namespace Horo::Render::Detail {
    /** @brief Chooses the existing independent color or depth resource/layer mapping. */
    enum class MultiviewAttachmentAspect {
        Color,
        Depth
    };

    /** @brief Preserves defined contents or proves complete CLEAR/STORE coverage without native state mutation.
     * @param previouslyDefined Existing whole-resource contents evidence.
     * @param workload Already validated bounded multiview workload whose recorder clears/stores each target rectangle.
     * @param resource Exact attachment resource being evaluated.
     * @param descriptor Exact resident allocation, including all layers and mips.
     * @param aspect Color or depth mapping; their layer coverage is independent.
     * @return Whole-resource evidence after this planned write. Partial rectangles/layers never define fresh storage.
     * @pre Native admission has validated target identities and rectangles. The caller publishes only after successful submission.
     */
    [[nodiscard]] inline bool MultiviewAttachmentContentsDefined(const bool previouslyDefined, const RenderGraphMultiview &workload,
                                                                 const RenderGraphResourceId resource,
                                                                 const RenderTextureDescriptor &descriptor,
                                                                 const MultiviewAttachmentAspect aspect) noexcept {
        if (previouslyDefined)
            return true;
        if (!resource.IsValid() || !descriptor.IsValid() || descriptor.mipCount != 1 || descriptor.depth != 1 ||
            descriptor.layerCount > MultiviewExtractionLimits::HardMaximumViews ||
            workload.targets.size() > MultiviewExtractionLimits::HardMaximumViews)
            return false;
        for (std::uint32_t layer = 0; layer < descriptor.layerCount; ++layer) {
            bool covered = false;
            for (const auto &target : workload.targets) {
                const auto attachment = aspect == MultiviewAttachmentAspect::Color ? target.color : target.depth;
                const auto targetLayer = aspect == MultiviewAttachmentAspect::Color ? target.arrayLayer : target.depthArrayLayer;
                const auto &rectangle = target.rectangle;
                if (attachment == resource && targetLayer == layer && rectangle.x == 0 && rectangle.y == 0 &&
                    rectangle.width == descriptor.extent.width && rectangle.height == descriptor.extent.height) {
                    covered = true;
                    break;
                }
            }
            if (!covered)
                return false;
        }
        return true;
    }
}  // namespace Horo::Render::Detail
