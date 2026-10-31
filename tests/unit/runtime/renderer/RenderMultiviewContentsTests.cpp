#include "RenderMultiviewAttachmentContents.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::Detail {
    TEST_CASE("Multiview contents require complete independent attachment coverage", "[renderer][multiview][contents]") {
        const RenderGraphResourceId color{{1}, 1}, depth{{1}, 2};
        RenderTextureDescriptor descriptor{.extent = {8, 8}, .layerCount = 2, .usage = RenderTextureUsage::RenderAttachment};
        const RenderTextureDescriptor depthDescriptor{.extent = {8, 8},
                                                      .format = RenderTextureFormat::Depth32Float,
                                                      .layerCount = 2,
                                                      .usage = RenderTextureUsage::RenderAttachment};
        std::array<MultiviewGraphTarget, 2> targets{
            {{color, depth, {}, {}, {0, 0, 8, 8}, 0, 1}, {color, depth, {}, {}, {0, 0, 8, 8}, 1, 0}}};
        const RenderGraphMultiview workload{.targets = targets};
        using enum MultiviewAttachmentAspect;
        REQUIRE(MultiviewAttachmentContentsDefined(false, workload, color, descriptor, Color));
        REQUIRE(MultiviewAttachmentContentsDefined(false, workload, depth, depthDescriptor, Depth));
        SECTION("partial rectangle cannot define either fresh allocation") {
            targets.back().rectangle.width = 4;
            CHECK_FALSE(MultiviewAttachmentContentsDefined(false, workload, color, descriptor, Color));
            CHECK_FALSE(MultiviewAttachmentContentsDefined(false, workload, depth, depthDescriptor, Depth));
        }
        SECTION("duplicate color layer leaves depth coverage independent") {
            targets.back().arrayLayer = 0;
            CHECK_FALSE(MultiviewAttachmentContentsDefined(false, workload, color, descriptor, Color));
            CHECK(MultiviewAttachmentContentsDefined(false, workload, depth, depthDescriptor, Depth));
        }
        SECTION("duplicate depth layer leaves color coverage independent") {
            targets.back().depthArrayLayer = 1;
            CHECK(MultiviewAttachmentContentsDefined(false, workload, color, descriptor, Color));
            CHECK_FALSE(MultiviewAttachmentContentsDefined(false, workload, depth, depthDescriptor, Depth));
        }
        SECTION("unwritten extra mip remains undefined") {
            descriptor.mipCount = 2;
            CHECK_FALSE(MultiviewAttachmentContentsDefined(false, workload, color, descriptor, Color));
        }
        SECTION("unwritten allocation layer remains undefined") {
            descriptor.layerCount = 3;
            CHECK_FALSE(MultiviewAttachmentContentsDefined(false, workload, color, descriptor, Color));
        }
        SECTION("partial writes preserve previously defined contents") {
            targets.back().rectangle = {2, 2, 2, 2};
            CHECK(MultiviewAttachmentContentsDefined(true, workload, color, descriptor, Color));
            CHECK(MultiviewAttachmentContentsDefined(true, workload, depth, depthDescriptor, Depth));
        }
    }
}  // namespace Horo::Render::Detail
