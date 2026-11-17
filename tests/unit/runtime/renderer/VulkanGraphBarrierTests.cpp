#include "runtime/renderer/modules/vulkan/VulkanGraphRuntimeInternal.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>

namespace {
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;
    using Access = RenderGraphSynchronizationAccess;
    using Operation = RenderGraphSynchronizationOperation;
    using Scope = RenderGraphPipelineScope;
    using Layout = RenderGraphTextureLayout;

    /** @brief Supplies an inert native identity and immutable whole-buffer descriptor; no GPU is invoked. */
    VulkanGraphResource Buffer() {
        VulkanGraphResource resource;
        resource.buffer = std::bit_cast<VkBuffer>(std::uintptr_t{0x1234});
        resource.bufferDescriptor = {256, RenderBufferUsage::Storage | RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination};
        return resource;
    }

    /** @brief Creates one exact buffer storage state on a selected effective queue. */
    RenderGraphLogicalState Storage(const Scope scope, const Access access, const RenderQueueId queue = {1}) {
        return {access, Operation::Storage, scope, Layout::NotApplicable, queue};
    }

    /** @brief Checks stable typed identity rather than parsing diagnostic text. */
    void CheckError(const Horo::Result<VulkanGraphBarrier> &result, const Horo::ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }

    TEST_CASE("Vulkan barrier translates admitted compute storage writes to graphics reads", "[vulkan][barrier][compute]") {
        const auto resource = Buffer();
        const RenderGraphLogicalState before{RenderGraphSynchronizationAccess::Write,
                                             RenderGraphSynchronizationOperation::Storage,
                                             RenderGraphPipelineScope::Compute,
                                             RenderGraphTextureLayout::NotApplicable,
                                             {1}};
        const RenderGraphLogicalState after{RenderGraphSynchronizationAccess::Read,
                                            RenderGraphSynchronizationOperation::Storage,
                                            RenderGraphPipelineScope::Graphics,
                                            RenderGraphTextureLayout::NotApplicable,
                                            {1}};
        const VulkanGraphBarrierOptions options{.admittedSourceComputeQueue = {1}};
        const auto translated = TranslateVulkanGraphBarrier(resource, before, after, options);
        INFO((translated.HasError() ? translated.ErrorValue().message : "barrier admitted"));
        INFO((translated.HasError() ? translated.ErrorValue().code.Value() : "success"));
        REQUIRE(translated.HasValue());
        CHECK(translated.Value().buffer.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        CHECK(translated.Value().buffer.srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        CHECK(translated.Value().buffer.dstStageMask == VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
        CHECK(translated.Value().buffer.dstAccessMask == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    TEST_CASE("Vulkan barrier independently admits destination compute storage", "[vulkan][barrier][compute]") {
        const auto resource = Buffer();
        RenderGraphLogicalState before{RenderGraphSynchronizationAccess::Write,
                                       RenderGraphSynchronizationOperation::Storage,
                                       RenderGraphPipelineScope::Graphics,
                                       RenderGraphTextureLayout::NotApplicable,
                                       {1}};
        SECTION("graphics producer") {}
        SECTION("undefined producer") {
            before = {};
        }
        const RenderGraphLogicalState after{RenderGraphSynchronizationAccess::ReadWrite,
                                            RenderGraphSynchronizationOperation::Storage,
                                            RenderGraphPipelineScope::Compute,
                                            RenderGraphTextureLayout::NotApplicable,
                                            {1}};
        const VulkanGraphBarrierOptions options{.admittedDestinationComputeQueue = {1}};
        const auto translated = TranslateVulkanGraphBarrier(resource, before, after, options);
        INFO((translated.HasError() ? translated.ErrorValue().code.Value() : "success"));
        REQUIRE(translated.HasValue());
        CHECK(translated.Value().buffer.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        CHECK(translated.Value().buffer.dstAccessMask == (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT));
        CHECK(translated.Value().buffer.srcStageMask ==
              (before.access == RenderGraphSynchronizationAccess::None ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT));
    }

    TEST_CASE("Vulkan compute storage masks preserve every access category and buffer identity", "[vulkan][barrier][compute]") {
        const std::array accesses{Access::Read, Access::Write, Access::ReadWrite};
        const std::array<VkAccessFlags2, 3> masks{VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                                  VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT};
        const auto resource = Buffer();
        const VulkanGraphBarrierOptions options{.admittedSourceComputeQueue = {2}, .admittedDestinationComputeQueue = {3}};
        for (std::size_t source = 0; source < accesses.size(); ++source) {
            for (std::size_t destination = 0; destination < accesses.size(); ++destination) {
                INFO(source << " -> " << destination);
                const auto result = TranslateVulkanGraphBarrier(resource, Storage(Scope::Compute, accesses[source], {2}),
                                                                Storage(Scope::Compute, accesses[destination], {3}), options);
                REQUIRE(result.HasValue());
                const auto &barrier = result.Value().buffer;
                CHECK_FALSE(result.Value().texture);
                CHECK(barrier.sType == VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2);
                CHECK(barrier.buffer == resource.buffer);
                CHECK(barrier.offset == 0);
                CHECK(barrier.size == VK_WHOLE_SIZE);
                CHECK(barrier.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
                CHECK(barrier.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
                CHECK(barrier.srcAccessMask == masks[source]);
                CHECK(barrier.dstAccessMask == masks[destination]);
                CHECK(barrier.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
                CHECK(barrier.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
            }
        }
    }

    TEST_CASE("Vulkan compute storage feeds transfer readback without graphics scope", "[vulkan][barrier][compute]") {
        const auto before = Storage(Scope::Compute, Access::Write);
        const RenderGraphLogicalState after{Access::Read, Operation::CopySource, Scope::Transfer, Layout::NotApplicable, {2}};
        const auto result = TranslateVulkanGraphBarrier(Buffer(), before, after, {.admittedSourceComputeQueue = {1}});
        REQUIRE(result.HasValue());
        CHECK(result.Value().buffer.srcStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        CHECK(result.Value().buffer.srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        CHECK(result.Value().buffer.dstStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
        CHECK(result.Value().buffer.dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT);
    }

    TEST_CASE("Vulkan compute admissions are independent exact queue facts and default to denial", "[vulkan][barrier][compute]") {
        const auto before = Storage(Scope::Compute, Access::Write, {2});
        const auto after = Storage(Scope::Compute, Access::Read, {3});
        CheckError(TranslateVulkanGraphBarrier(Buffer(), before, Storage(Scope::Graphics, Access::Read)),
                   VulkanBackendErrors::UnsupportedOperation);
        CheckError(TranslateVulkanGraphBarrier(Buffer(), Storage(Scope::Graphics, Access::Write), after),
                   VulkanBackendErrors::UnsupportedOperation);
        VulkanGraphBarrierOptions options;
        SECTION("default denial") {}
        SECTION("only source admitted") {
            options.admittedSourceComputeQueue = {2};
        }
        SECTION("only destination admitted") {
            options.admittedDestinationComputeQueue = {3};
        }
        SECTION("foreign source identity") {
            options.admittedSourceComputeQueue = {4};
            options.admittedDestinationComputeQueue = {3};
        }
        SECTION("foreign destination identity") {
            options.admittedSourceComputeQueue = {2};
            options.admittedDestinationComputeQueue = {4};
        }
        CheckError(TranslateVulkanGraphBarrier(Buffer(), before, after, options), VulkanBackendErrors::UnsupportedOperation);
    }

    TEST_CASE("Vulkan admitted compute still rejects nonstorage operations on both sides", "[vulkan][barrier][compute]") {
        const std::array operations{Operation::None,
                                    Operation::Sampled,
                                    Operation::ColorAttachment,
                                    Operation::DepthStencilAttachment,
                                    Operation::GraphicsInputBuffer,
                                    Operation::CopySource,
                                    Operation::CopyDestination,
                                    static_cast<Operation>(255)};
        const auto graphics = Storage(Scope::Graphics, Access::Read);
        const VulkanGraphBarrierOptions options{.admittedSourceComputeQueue = {1}, .admittedDestinationComputeQueue = {1}};
        for (const auto operation : operations) {
            INFO(static_cast<int>(operation));
            auto compute = Storage(Scope::Compute, Access::ReadWrite);
            compute.operation = operation;
            CheckError(TranslateVulkanGraphBarrier(Buffer(), compute, graphics, options), VulkanBackendErrors::UnsupportedOperation);
            CheckError(TranslateVulkanGraphBarrier(Buffer(), graphics, compute, options), VulkanBackendErrors::UnsupportedOperation);
        }
    }

    TEST_CASE("Vulkan compute denies unknown native resource kinds", "[vulkan][barrier][compute]") {
        auto resource = Buffer();
        resource.kind = static_cast<RenderGraphResourceKind>(255);
        const auto compute = Storage(Scope::Compute, Access::Write);
        const auto graphics = Storage(Scope::Graphics, Access::Read);
        const VulkanGraphBarrierOptions options{.admittedSourceComputeQueue = {1}, .admittedDestinationComputeQueue = {1}};
        SECTION("compute source") {
            CheckError(TranslateVulkanGraphBarrier(resource, compute, graphics, options), VulkanBackendErrors::UnsupportedOperation);
        }
        SECTION("compute destination") {
            CheckError(TranslateVulkanGraphBarrier(resource, graphics, compute, options), VulkanBackendErrors::UnsupportedOperation);
        }
    }

    TEST_CASE("Vulkan compute never admits storage or sampled images", "[vulkan][barrier][compute]") {
        auto image = Buffer();
        image.kind = RenderGraphResourceKind::Texture;
        const RenderGraphLogicalState graphics{Access::Read, Operation::Sampled, Scope::Graphics, Layout::ShaderReadOnly, {1}};
        const VulkanGraphBarrierOptions options{.admittedSourceComputeQueue = {1}, .admittedDestinationComputeQueue = {1}};
        for (const auto operation : {Operation::Storage, Operation::Sampled, Operation::ColorAttachment, Operation::DepthStencilAttachment,
                                     Operation::GraphicsInputBuffer}) {
            const RenderGraphLogicalState compute{Access::ReadWrite, operation, Scope::Compute, Layout::General, {1}};
            CheckError(TranslateVulkanGraphBarrier(image, compute, graphics, options), VulkanBackendErrors::UnsupportedOperation);
            CheckError(TranslateVulkanGraphBarrier(image, graphics, compute, options), VulkanBackendErrors::UnsupportedOperation);
        }
    }

    TEST_CASE("Vulkan admitted compute validates access queue and buffer layout before translation", "[vulkan][barrier][compute]") {
        auto compute = Storage(Scope::Compute, Access::Write);
        const auto graphics = Storage(Scope::Graphics, Access::Read);
        const Horo::ErrorCodeDescriptor *expected = &VulkanBackendErrors::ResourceRequestInvalid;
        SECTION("unknown access") {
            compute.access = static_cast<Access>(255);
        }
        SECTION("noncanonical undefined compute") {
            compute.access = Access::None;
        }
        SECTION("image layout on a buffer") {
            compute.layout = Layout::General;
        }
        SECTION("unknown layout on a buffer") {
            compute.layout = static_cast<Layout>(255);
        }
        SECTION("missing queue") {
            compute.queue = {};
            expected = &VulkanBackendErrors::UnsupportedOperation;
        }
        SECTION("external scope") {
            compute.scope = Scope::External;
            expected = &VulkanBackendErrors::UnsupportedOperation;
        }
        SECTION("unknown scope") {
            compute.scope = static_cast<Scope>(255);
        }
        SECTION("transfer scope cannot perform shader storage") {
            compute.scope = Scope::Transfer;
        }
        const VulkanGraphBarrierOptions options{.admittedSourceComputeQueue = {1}, .admittedDestinationComputeQueue = {1}};
        CheckError(TranslateVulkanGraphBarrier(Buffer(), compute, graphics, options), *expected);
        CheckError(TranslateVulkanGraphBarrier(Buffer(), graphics, compute, options), *expected);
    }

    TEST_CASE("Vulkan compute release acquire retain exact family pairs and whole buffer", "[vulkan][barrier][compute]") {
        const auto resource = Buffer();
        auto before = Storage(Scope::Compute, Access::Write, {2});
        auto after = Storage(Scope::Graphics, Access::Read, {3});
        VulkanGraphBarrierOptions options{.sourceFamily = 5, .destinationFamily = 7, .release = true, .admittedSourceComputeQueue = {2}};
        SECTION("compute producer") {}
        SECTION("compute consumer") {
            before.scope = Scope::Graphics;
            after.scope = Scope::Compute;
            options.admittedSourceComputeQueue = {};
            options.admittedDestinationComputeQueue = {3};
        }
        const auto released = TranslateVulkanGraphBarrier(resource, before, after, options);
        options.release = false;
        options.acquire = true;
        const auto acquired = TranslateVulkanGraphBarrier(resource, before, after, options);
        REQUIRE(released.HasValue());
        REQUIRE(acquired.HasValue());
        const auto &release = released.Value().buffer;
        const auto &acquire = acquired.Value().buffer;
        CHECK((release.buffer == resource.buffer && acquire.buffer == resource.buffer));
        CHECK((release.offset == 0 && acquire.offset == 0));
        CHECK((release.size == VK_WHOLE_SIZE && acquire.size == VK_WHOLE_SIZE));
        CHECK((release.srcQueueFamilyIndex == 5 && acquire.srcQueueFamilyIndex == 5));
        CHECK((release.dstQueueFamilyIndex == 7 && acquire.dstQueueFamilyIndex == 7));
        CHECK(release.srcStageMask ==
              (before.scope == Scope::Compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT));
        CHECK(release.srcAccessMask == VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        CHECK((release.dstStageMask == VK_PIPELINE_STAGE_2_NONE && release.dstAccessMask == VK_ACCESS_2_NONE));
        CHECK((acquire.srcStageMask == VK_PIPELINE_STAGE_2_NONE && acquire.srcAccessMask == VK_ACCESS_2_NONE));
        CHECK(acquire.dstStageMask ==
              (after.scope == Scope::Compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT));
        CHECK(acquire.dstAccessMask == VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }

    TEST_CASE("Vulkan discarded compute dependencies retain prior access ordering", "[vulkan][barrier][compute]") {
        const auto compute = Storage(Scope::Compute, Access::Write);
        VulkanGraphBarrierOptions options{.discardPriorAccess = true, .admittedDestinationComputeQueue = {1}};
        const auto result = TranslateVulkanGraphBarrier(Buffer(), {}, compute, options);
        REQUIRE(result.HasValue());
        CHECK(result.Value().buffer.srcStageMask == VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        CHECK(result.Value().buffer.srcAccessMask == (VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT));
        CHECK(result.Value().buffer.dstStageMask == VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
        options.acquire = true;
        const auto acquired = TranslateVulkanGraphBarrier(Buffer(), {}, compute, options);
        REQUIRE(acquired.HasValue());
        CHECK((acquired.Value().buffer.srcStageMask == VK_PIPELINE_STAGE_2_NONE &&
               acquired.Value().buffer.srcAccessMask == VK_ACCESS_2_NONE));
    }

    TEST_CASE("Vulkan existing graphics transfer and undefined masks remain exact", "[vulkan][barrier][regression]") {
        const auto resource = Buffer();
        const auto graphics = Storage(Scope::Graphics, Access::ReadWrite);
        const auto initial = TranslateVulkanGraphBarrier(resource, {}, graphics);
        REQUIRE(initial.HasValue());
        CHECK(
            (initial.Value().buffer.srcStageMask == VK_PIPELINE_STAGE_2_NONE && initial.Value().buffer.srcAccessMask == VK_ACCESS_2_NONE));
        CHECK(initial.Value().buffer.dstStageMask == VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT);
        CHECK(initial.Value().buffer.dstAccessMask == (VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT));
        const RenderGraphLogicalState copy{Access::Write, Operation::CopyDestination, Scope::Transfer, Layout::NotApplicable, {1}};
        const auto transfer = TranslateVulkanGraphBarrier(resource, graphics, copy);
        REQUIRE(transfer.HasValue());
        CHECK(transfer.Value().buffer.dstStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
        CHECK(transfer.Value().buffer.dstAccessMask == VK_ACCESS_2_TRANSFER_WRITE_BIT);
        auto malformed = RenderGraphLogicalState{};
        malformed.queue = {1};
        CheckError(TranslateVulkanGraphBarrier(resource, malformed, copy), VulkanBackendErrors::ResourceRequestInvalid);
        CheckError(TranslateVulkanGraphBarrier(resource, copy, malformed), VulkanBackendErrors::ResourceRequestInvalid);
    }

    TEST_CASE("Vulkan barrier preserves source failure precedence over invalid destinations", "[vulkan][barrier][regression]") {
        auto malformed = RenderGraphLogicalState{};
        malformed.queue = {9};
        const auto compute = Storage(Scope::Compute, Access::Write);
        const auto denied = TranslateVulkanGraphBarrier(Buffer(), compute, malformed);
        CheckError(denied, VulkanBackendErrors::UnsupportedOperation);
        CHECK(denied.ErrorValue().message == "Native compute/external access scope is not admitted.");
        const auto invalid = TranslateVulkanGraphBarrier(Buffer(), malformed, compute);
        CheckError(invalid, VulkanBackendErrors::ResourceRequestInvalid);
        CHECK(invalid.ErrorValue().message == "Malformed undefined native state.");
        const auto admitted = TranslateVulkanGraphBarrier(Buffer(), compute, malformed, {.admittedSourceComputeQueue = {1}});
        CheckError(admitted, VulkanBackendErrors::ResourceRequestInvalid);
        CHECK(admitted.ErrorValue().message == "Malformed undefined native state.");
    }

    /** @brief Verifies the complete matched whole-image subresource range independently of pipeline masks. */
    void CheckImageSubresources(const VkImageMemoryBarrier2 &release, const VkImageMemoryBarrier2 &acquire) {
        CHECK(release.subresourceRange.aspectMask == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT));
        CHECK(acquire.subresourceRange.aspectMask == release.subresourceRange.aspectMask);
        CHECK((release.subresourceRange.baseMipLevel == 0 && acquire.subresourceRange.baseMipLevel == 0));
        CHECK((release.subresourceRange.levelCount == 3 && acquire.subresourceRange.levelCount == 3));
        CHECK((release.subresourceRange.baseArrayLayer == 0 && acquire.subresourceRange.baseArrayLayer == 0));
        CHECK((release.subresourceRange.layerCount == 4 && acquire.subresourceRange.layerCount == 4));
    }

    TEST_CASE("Vulkan image release acquire retain matching layouts aspects and ranges", "[vulkan][barrier][regression]") {
        VulkanGraphResource resource;
        resource.kind = RenderGraphResourceKind::Texture;
        resource.image = std::bit_cast<VkImage>(std::uintptr_t{0x5678});
        resource.textureDescriptor.mipCount = 3;
        resource.textureDescriptor.layerCount = 4;
        resource.textureDescriptor.format = RenderTextureFormat::Depth24Stencil8;
        const RenderGraphLogicalState before{Access::ReadWrite,
                                             Operation::DepthStencilAttachment,
                                             Scope::Graphics,
                                             Layout::DepthStencilAttachment,
                                             {1}};
        const RenderGraphLogicalState after{Access::Read, Operation::CopySource, Scope::Transfer, Layout::CopySource, {2}};
        VulkanGraphBarrierOptions options{.sourceFamily = 3, .destinationFamily = 7, .release = true};
        const auto released = TranslateVulkanGraphBarrier(resource, before, after, options);
        options.release = false;
        options.acquire = true;
        const auto acquired = TranslateVulkanGraphBarrier(resource, before, after, options);
        REQUIRE(released.HasValue());
        REQUIRE(acquired.HasValue());
        const auto &release = released.Value().image;
        const auto &acquire = acquired.Value().image;
        CHECK((released.Value().texture && acquired.Value().texture));
        CHECK((release.image == resource.image && acquire.image == resource.image));
        CHECK((release.oldLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL && acquire.oldLayout == release.oldLayout));
        CHECK((release.newLayout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && acquire.newLayout == release.newLayout));
        CHECK((release.srcQueueFamilyIndex == 3 && acquire.srcQueueFamilyIndex == 3));
        CHECK((release.dstQueueFamilyIndex == 7 && acquire.dstQueueFamilyIndex == 7));
        CheckImageSubresources(release, acquire);
        CHECK(release.srcStageMask == (VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT));
        CHECK(release.srcAccessMask == (VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT));
        CHECK((release.dstStageMask == VK_PIPELINE_STAGE_2_NONE && release.dstAccessMask == VK_ACCESS_2_NONE));
        CHECK((acquire.srcStageMask == VK_PIPELINE_STAGE_2_NONE && acquire.srcAccessMask == VK_ACCESS_2_NONE));
        CHECK(acquire.dstStageMask == VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
        CHECK(acquire.dstAccessMask == VK_ACCESS_2_TRANSFER_READ_BIT);
    }
}  // namespace
