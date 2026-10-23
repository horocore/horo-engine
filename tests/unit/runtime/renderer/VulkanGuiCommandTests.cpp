#include "../../../support/renderer/MultiviewTestSupport.h"
#include "VulkanGraphTestSupport.h"
#include "VulkanShaderDispatchFixture.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"

namespace {
    using namespace Horo::Render::VulkanGraphTest;
    using namespace Horo::Render::VulkanResourceTest;

    struct GuiFixture {
        Driver shader;
        NativeGraphFixture frame;
        std::uint64_t texture{}, view{}, sampler{}, staging{};
        std::array<GuiSampledResourceInstance, 1> samples;
        RenderGraphLeaseTransfer transfer;
        RenderQueueSubmission submission{{1}, {1}, {}, {{1}, 1}};
        Detail::VulkanGuiOutput output{NativeHandle<VkImage>(800), NativeHandle<VkImageView>(801), NativeHandle<VkSemaphore>(802), {8, 8}};

        GuiFixture() {
            driver = &shader;
            VkPhysicalDeviceProperties properties{};
            properties.limits.maxSamplerAllocationCount = 4096;
            REQUIRE(frame.resources.InitializeShaders(properties, RenderAdapterId{"vulkan:gui-test"}, Resolve, 11).HasValue());
            frame.resourceState.state.bufferRequirements.size = 16;
            frame.resourceState.state.imageRequirements.size = 16;
            const RenderTextureDescriptor descriptor{.extent = {2, 2},
                                                     .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination};
            auto plan = SubmissionTest::Take(frame.resources.QueryTextureMemoryCost(descriptor));
            texture = SubmissionTest::Take(frame.resources.CreateTexture(descriptor, {}, TestSupport::PlacementFor(plan, 1)));
            view = SubmissionTest::Take(frame.resources.CreateTextureView({.texture = {{1}, 1, 1}}, texture));
            plan = SubmissionTest::Take(frame.resources.QuerySamplerMemoryCost({}));
            sampler = SubmissionTest::Take(frame.resources.CreateSampler({}, TestSupport::PlacementFor(plan, 2)));
            const RenderBufferDescriptor buffer{16, RenderBufferUsage::CopySource, RenderBufferAccess::HostVisible};
            const std::array<std::byte, 16> pixels{};
            plan = SubmissionTest::Take(frame.resources.QueryBufferMemoryCost(buffer));
            staging = SubmissionTest::Take(frame.resources.CreateBuffer(buffer, pixels, TestSupport::PlacementFor(plan, 3)));
            samples[0] = {view, sampler, staging};
        }

        GuiCompositionRequest Request(FrameToken token = {1}) {
            return {token, GuiCompositionMode::ExplicitGraphicsSubmission, submission, samples, &frame.lease, &transfer};
        }

        const Detail::VulkanGraphResourceState &State() {
            Detail::VulkanCommandRetention borrowed;
            borrowed.resources.reserve(3);
            return *SubmissionTest::Take(frame.resources.CaptureGuiSample({view, sampler, 0}, borrowed)).texture.state;
        }
    };

    struct Encoder final : Detail::IVulkanGuiEncoder {
        bool fail{};

        Result<void> Record(VkCommandBuffer command, Detail::VulkanFrameRetentionSlot slot,
                            std::span<const Detail::VulkanGuiSampleBinding> samples,
                            std::span<const Detail::VulkanGuiBufferBinding>) override {
            CHECK(command != VK_NULL_HANDLE);
            CHECK(slot.index < slot.count);
            REQUIRE(samples.size() == 1);
            CHECK(samples[0].view != VK_NULL_HANDLE);
            CHECK(samples[0].sampler != VK_NULL_HANDLE);
            encodedGraph->operations.emplace_back("gui-draw");
            return fail ? Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceRequestInvalid)) : Result<void>::Success();
        }
    };

    TEST_CASE("Vulkan GUI uploads before sampling and publishes contents only after canonical submit", "[vulkan][gui][upload]") {
        GuiFixture f;
        Encoder encoder;
        REQUIRE(f.frame.graph.BeginFrame({1}).HasValue());
        auto request = f.Request();
        REQUIRE(f.frame.graph.PrepareGuiComposition(request).HasValue());
        REQUIRE(f.frame.graph.ExecuteGui(request, f.output, encoder).HasValue());
        CHECK_FALSE(f.State().contentsDefined);
        const auto &operations = f.frame.encoded.operations;
        const auto upload = std::ranges::find(operations, "upload-image");
        const auto draw = std::ranges::find(operations, "gui-draw");
        REQUIRE(upload != operations.end());
        REQUIRE(draw != operations.end());
        CHECK(upload < draw);
        REQUIRE(f.frame.graph.SubmitFrame({1}).HasValue());
        CHECK(f.State().contentsDefined);
        CHECK(f.State().logical.layout == RenderGraphTextureLayout::ShaderReadOnly);
        CHECK(f.frame.lease.releases == 0);
        f.frame.commandState.state.Complete();
        REQUIRE(f.frame.graph.ObserveSubmission({{{1}, 1}, RenderCpuWaitPurpose::Poll, {}, 11}).Value());
        CHECK(f.frame.lease.releases == 1);
    }

    TEST_CASE("Vulkan aborted GUI upload stays undefined and retries the exact staging copy", "[vulkan][gui][abort]") {
        GuiFixture f;
        Encoder encoder;
        REQUIRE(f.frame.graph.BeginFrame({1}).HasValue());
        auto request = f.Request();
        REQUIRE(f.frame.graph.PrepareGuiComposition(request).HasValue());
        REQUIRE(f.frame.graph.ExecuteGui(request, f.output, encoder).HasValue());
        f.frame.graph.AbortFrame({1});
        CHECK_FALSE(f.State().contentsDefined);
        CHECK(f.frame.lease.releases == 1);
        f.transfer = {};
        REQUIRE(f.frame.graph.BeginFrame({2}).HasValue());
        request = f.Request({2});
        REQUIRE(f.frame.graph.PrepareGuiComposition(request).HasValue());
        REQUIRE(f.frame.graph.ExecuteGui(request, f.output, encoder).HasValue());
        CHECK(std::ranges::count(f.frame.encoded.operations, "upload-image") == 2);
        REQUIRE(f.frame.graph.SubmitFrame({2}).HasValue());
        CHECK(f.State().contentsDefined);
    }

    TEST_CASE("Vulkan GUI rejects missing or retired staging before drawing", "[vulkan][gui][stale]") {
        GuiFixture f;
        Encoder encoder;
        SECTION("missing source") {
            f.samples[0].uploadSource = 0;
        }
        SECTION("retired source") {
            f.frame.resources.DestroyBuffer(f.staging);
        }
        REQUIRE(f.frame.graph.BeginFrame({1}).HasValue());
        auto request = f.Request();
        REQUIRE(f.frame.graph.PrepareGuiComposition(request).HasValue());
        CHECK(f.frame.graph.ExecuteGui(request, f.output, encoder).HasError());
        CHECK(std::ranges::count(f.frame.encoded.operations, "gui-draw") == 0);
        f.frame.graph.AbortFrame({1});
        CHECK_FALSE(f.State().contentsDefined);
    }

    TEST_CASE("Vulkan failed GUI native submit never publishes sampled contents", "[vulkan][gui][failure]") {
        GuiFixture f;
        Encoder encoder;
        REQUIRE(f.frame.graph.BeginFrame({1}).HasValue());
        auto request = f.Request();
        REQUIRE(f.frame.graph.PrepareGuiComposition(request).HasValue());
        REQUIRE(f.frame.graph.ExecuteGui(request, f.output, encoder).HasValue());
        f.frame.commandState.state.submitResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        CHECK(f.frame.graph.SubmitFrame({1}).HasError());
        CHECK_FALSE(f.State().contentsDefined);
        CHECK(f.frame.lease.releases == 1);
    }
}  // namespace
