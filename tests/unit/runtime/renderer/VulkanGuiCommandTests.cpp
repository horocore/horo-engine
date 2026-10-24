#include "../../../support/renderer/MultiviewTestSupport.h"
#include "VulkanExternalGraphTestSupport.h"
#include "VulkanGraphTestSupport.h"
#include "VulkanShaderDispatchFixture.h"
#include "editor/renderer/vulkan/EditorVulkanGuiFunctionLoader.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"

namespace {
    using namespace Horo::Render::VulkanGraphTest;
    using namespace Horo::Render::VulkanResourceTest;

    struct ResolverTrace {
        std::vector<std::string> instanceNames;
        std::vector<std::string> deviceNames;
        bool missing{};
    };

    // These synchronous fixture callbacks borrow one owner on the executing test lane only.
    ResolverTrace *resolverTrace{};

    VKAPI_ATTR void VKAPI_CALL ResolvedFunction() {}

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ResolveInstance(VkInstance instance, const char *name) {
        CHECK(instance == NativeHandle<VkInstance>(41));
        resolverTrace->instanceNames.emplace_back(name);
        return resolverTrace->missing ? nullptr : &ResolvedFunction;
    }

    VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL ResolveDevice(VkDevice device, const char *name) {
        CHECK(device == NativeHandle<VkDevice>(42));
        resolverTrace->deviceNames.emplace_back(name);
        return resolverTrace->missing ? nullptr : &ResolvedFunction;
    }

    TEST_CASE("Vulkan GUI resolves instance commands without probing the device dispatch domain", "[vulkan][gui][dispatch]") {
        ResolverTrace trace;
        resolverTrace = &trace;
        const Horo::Render::Detail::VulkanEditorDevice device{.instance = NativeHandle<VkInstance>(41),
                                                              .device = NativeHandle<VkDevice>(42),
                                                              .instanceResolver = &ResolveInstance,
                                                              .deviceResolver = &ResolveDevice};
        const std::vector<std::string> instanceNames{"vkDestroySurfaceKHR", "vkGetPhysicalDeviceMemoryProperties",
                                                     "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", "vkGetPhysicalDeviceSurfaceFormatsKHR",
                                                     "vkGetPhysicalDeviceSurfacePresentModesKHR"};
        for (const auto &name : instanceNames)
            CHECK(Horo::Editor::Detail::ResolveVulkanGuiFunction(device, name.c_str()) == &ResolvedFunction);
        CHECK(trace.instanceNames == instanceNames);
        CHECK(trace.deviceNames.empty());
        CHECK(Horo::Editor::Detail::ResolveVulkanGuiFunction(device, "vkCreateImageView") == &ResolvedFunction);
        CHECK(Horo::Editor::Detail::ResolveVulkanGuiFunction(device, "vkCmdBeginRendering") == &ResolvedFunction);
        CHECK(trace.deviceNames == std::vector<std::string>{"vkCreateImageView", "vkCmdBeginRendering"});
        trace.missing = true;
        CHECK_FALSE(Horo::Editor::Detail::ResolveVulkanGuiFunction(device, "vkDestroySurfaceKHR"));
        CHECK_FALSE(Horo::Editor::Detail::ResolveVulkanGuiFunction(device, "vkCreateImageView"));
        CHECK(trace.instanceNames.size() == instanceNames.size() + 1);
        CHECK(trace.deviceNames.size() == 3);
        resolverTrace = nullptr;
    }

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

    struct MirrorSource final : Detail::IVulkanExternalTextureSource {
        bool current{true};
        bool admit{true};
        mutable bool retired{};
        RenderTextureDescriptor descriptor{.extent = {8, 8},
                                           .layerCount = 2,
                                           .usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled};

        Result<void> Validate() const override {
            return current ? Result<void>::Success() : Result<void>::Failure(MakeError(VulkanBackendErrors::ResourceIdentityInvalid));
        }

        std::uint64_t DeviceIncarnation() const noexcept override {
            return 11;
        }

        const RenderTextureDescriptor &Descriptor() const noexcept override {
            return descriptor;
        }

        VkDevice NativeDevice() const noexcept override {
            return NativeHandle<VkDevice>(1);
        }

        VkImage NativeImage() const noexcept override {
            return NativeHandle<VkImage>(901);
        }

        std::uint32_t NativeQueueFamily() const noexcept override {
            return 3;
        }

        Detail::VulkanExternalNativeViews PreparedNativeViews() const noexcept override {
            return {NativeHandle<VkImageView>(902), NativeHandle<VkImageView>(903)};
        }

        bool AdmitsFirstEyeGuiMirror(const RenderTextureViewDescriptor &view) const noexcept override {
            return admit && view.dimension == RenderTextureViewDimension::TwoD && view.baseLayer == 0 && view.layerCount == 1;
        }

        RenderGraphLogicalState AcquiredState() const noexcept override {
            return {RenderGraphSynchronizationAccess::ReadWrite,
                    RenderGraphSynchronizationOperation::ColorAttachment,
                    RenderGraphPipelineScope::Graphics,
                    RenderGraphTextureLayout::ColorAttachment,
                    {1}};
        }

        RenderGraphLogicalState RequiredFinalState() const noexcept override {
            auto state = AcquiredState();
            state.scope = RenderGraphPipelineScope::External;
            return state;
        }

        void RecordNativeRetirement(const RenderGraphLogicalState &state, const bool present) const noexcept override {
            retired = present && state == AcquiredState();
        }
    };

    struct MirrorLease final : IRenderResourceRetirementLease {
        int releases{};

        void Claim() noexcept override {}

        void Release() noexcept override {
            ++releases;
        }
    };

    TEST_CASE("Vulkan XR eye mirror restores the actual post GUI layout and retains the same lease until tail completion",
              "[vulkan][gui][xr][external]") {
        MirrorSource source;
        MirrorLease nativeLease;
        GuiFixture f;
        const auto texture =
            SubmissionTest::Take(f.frame.resources.ImportExternalTexture(source, RenderResourceNativeLifetime{nativeLease}));
        const auto view = SubmissionTest::Take(f.frame.resources.CreateTextureView({.texture = {{41}, 1, 1}}, texture));
        f.samples[0] = {view, f.sampler, 0};
        auto packet = VulkanExternalTest::ExternalGraph({{41}, 1, 1}, source.descriptor, source.RequiredFinalState(), texture);
        REQUIRE(f.frame.graph.BeginFrame({1}).HasValue());
        RenderGraphLeaseTransfer scene;
        REQUIRE(
            f.frame.graph
                .ExecuteGraph({{1}, packet.graph, packet.workloads, packet.instances, &f.frame.lease, false, &scene, packet.submissions})
                .HasValue());
        const std::array waits{RenderTimelinePoint{{1}, 1}};
        f.submission = {{1}, {2}, waits, {{1}, 2}};
        auto request = f.Request();
        request.extendsExistingLease = true;
        f.output.legacyUnmanagedOutput = true;
        Encoder encoder;
        REQUIRE(f.frame.graph.PrepareGuiComposition(request).HasValue());
        REQUIRE(f.frame.graph.ExecuteGui(request, f.output, encoder).HasValue());
        const auto &operations = f.frame.encoded.operations;
        const auto draw = std::ranges::find(operations, "gui-draw");
        REQUIRE(draw != operations.end());
        CHECK(std::ranges::find(draw + 1, operations.end(), "barrier") != operations.end());
        const auto &barriers = f.frame.encoded.imageBarriers;
        const auto restored = std::ranges::find_if(barriers.rbegin(), barriers.rend(), [&](const VkImageMemoryBarrier2 &barrier) {
            return barrier.image == source.NativeImage();
        });
        REQUIRE(restored != barriers.rend());
        CHECK(restored->oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        CHECK(restored->newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        REQUIRE(f.frame.graph.SubmitFrame({1}).HasValue());
        f.frame.resources.DestroyTexture(texture);
        CHECK(nativeLease.releases == 0);
        CHECK_FALSE(source.retired);
        const RenderCpuWait poll{.completion = {{1}, 2}, .expectedDeviceIncarnation = 11};
        REQUIRE(f.frame.graph.ObserveSubmission(poll).HasValue());
        CHECK(nativeLease.releases == 0);
        f.frame.commandState.state.Complete();
        REQUIRE(f.frame.graph.ObserveSubmission(poll).Value());
        CHECK(nativeLease.releases == 1);
        CHECK(source.retired);
        CHECK(f.frame.commandState.state.waitCalls == 0);
    }

    TEST_CASE("Vulkan XR mirror rejects unsupported output and stale acquisition and cancels failed GUI without false state",
              "[vulkan][gui][xr][abort]") {
        MirrorSource source;
        MirrorLease nativeLease;
        GuiFixture f;
        const auto texture =
            SubmissionTest::Take(f.frame.resources.ImportExternalTexture(source, RenderResourceNativeLifetime{nativeLease}));
        const auto view = SubmissionTest::Take(f.frame.resources.CreateTextureView({.texture = {{41}, 1, 1}}, texture));
        f.samples[0] = {view, f.sampler, 0};
        auto packet = VulkanExternalTest::ExternalGraph({{41}, 1, 1}, source.descriptor, source.RequiredFinalState(), texture);
        REQUIRE(f.frame.graph.BeginFrame({1}).HasValue());
        RenderGraphLeaseTransfer scene;
        REQUIRE(
            f.frame.graph
                .ExecuteGraph({{1}, packet.graph, packet.workloads, packet.instances, &f.frame.lease, false, &scene, packet.submissions})
                .HasValue());
        const std::array waits{RenderTimelinePoint{{1}, 1}};
        f.submission = {{1}, {2}, waits, {{1}, 2}};
        auto request = f.Request();
        request.extendsExistingLease = true;
        Encoder encoder;
        f.output.legacyUnmanagedOutput = true;
        SECTION("explicit color transform unsupported") {
            f.output.legacyUnmanagedOutput = false;
        }
        SECTION("stale acquisition") {
            source.current = false;
        }
        SECTION("unadmitted external source") {
            source.admit = false;
        }
        SECTION("GUI encoder fails") {
            encoder.fail = true;
        }
        REQUIRE(f.frame.graph.PrepareGuiComposition(request).HasValue());
        CHECK(f.frame.graph.ExecuteGui(request, f.output, encoder).HasError());
        f.frame.graph.AbortFrame({1});
        CHECK(f.frame.commandState.state.accepted.empty());
        CHECK(f.frame.lease.releases == 1);
        f.frame.resources.DestroyTexture(texture);
        CHECK(nativeLease.releases == 1);
        CHECK(source.retired);  // No submitted sample changed the initial native attachment state.
    }

    struct MixedEncoder final : Detail::IVulkanGuiEncoder {
        Result<void> Record(VkCommandBuffer, Detail::VulkanFrameRetentionSlot, std::span<const Detail::VulkanGuiSampleBinding> images,
                            std::span<const Detail::VulkanGuiBufferBinding>) override {
            REQUIRE(images.size() == 2);
            encodedGraph->operations.emplace_back("gui-draw");
            return Result<void>::Success();
        }
    };

    /** @brief Proves every unsent mixed attempt restores exactly the scene-owned prefix before a successful retry. */
    void RequireRejectedMixedAttempts(GuiFixture &f, const GuiCompositionRequest &request, MirrorSource &source, MixedEncoder &encoder) {
        const auto priorFont = f.State();
        const auto priorOperations = f.frame.encoded.operations;
        const auto priorEnds = f.frame.commandState.state.endCalls;
        for (std::size_t attempt = 0; attempt < 16; ++attempt) {
            const auto rejected = f.frame.graph.ExecuteGui(request, f.output, encoder);
            REQUIRE(rejected.HasError());
            if (!f.output.legacyUnmanagedOutput)
                CHECK(rejected.ErrorValue().code.Value() == VulkanBackendErrors::UnsupportedOperation.code.Value());
            else if (!source.current)
                CHECK(rejected.ErrorValue().code.Value() == VulkanBackendErrors::ResourceIdentityInvalid.code.Value());
            else
                CHECK(rejected.ErrorValue().message.find("vkBeginCommandBuffer") != std::string::npos);
            CHECK(f.frame.encoded.operations == priorOperations);
            CHECK(f.frame.commandState.state.accepted.empty());
            CHECK(f.frame.commandState.state.endCalls == priorEnds);
            CHECK(f.frame.lease.releases == 0);
            const auto afterFont = f.State();
            CHECK(afterFont.logical == priorFont.logical);
            CHECK(afterFont.completion == priorFont.completion);
            CHECK(afterFont.hasNativeState == priorFont.hasNativeState);
            CHECK(afterFont.contentsDefined == priorFont.contentsDefined);
        }
    }

    /** @brief Verifies upload-before-draw and actual scene/GUI completion before external-native retirement. */
    void RequireCompletedMixedTail(GuiFixture &f, std::uint64_t texture, MirrorSource &source, const MirrorLease &nativeLease) {
        const auto &operations = f.frame.encoded.operations;
        const auto upload = std::ranges::find(operations, "upload-image");
        const auto draw = std::ranges::find(operations, "gui-draw");
        REQUIRE(upload != operations.end());
        REQUIRE(draw != operations.end());
        CHECK(upload < draw);
        CHECK(std::ranges::count(operations, "upload-image") == 1);
        CHECK_FALSE(f.State().contentsDefined);
        REQUIRE(f.frame.graph.SubmitFrame({1}).HasValue());
        CHECK(f.State().contentsDefined);
        CHECK(f.State().logical.layout == RenderGraphTextureLayout::ShaderReadOnly);
        f.frame.resources.DestroyTexture(texture);
        CHECK(nativeLease.releases == 0);
        f.frame.commandState.state.Complete();
        REQUIRE(f.frame.graph.ObserveSubmission({.completion = {{1}, 2}, .expectedDeviceIncarnation = 11}).Value());
        CHECK(nativeLease.releases == 1);
        CHECK(source.retired);
        CHECK(f.frame.lease.releases == 1);
        CHECK(f.frame.commandState.state.waitCalls == 0);
    }

    TEST_CASE("Vulkan rejected mixed font upload and XR mirror preparation retries without publishing or retaining scratch",
              "[vulkan][gui][xr][retry]") {
        MirrorSource source;
        MirrorLease nativeLease;
        GuiFixture f;
        const auto texture =
            SubmissionTest::Take(f.frame.resources.ImportExternalTexture(source, RenderResourceNativeLifetime{nativeLease}));
        const auto mirror = SubmissionTest::Take(f.frame.resources.CreateTextureView({.texture = {{41}, 1, 1}}, texture));
        const std::array samples{f.samples[0], GuiSampledResourceInstance{mirror, f.sampler, 0}};
        auto packet = VulkanExternalTest::ExternalGraph({{41}, 1, 1}, source.descriptor, source.RequiredFinalState(), texture);
        REQUIRE(f.frame.graph.BeginFrame({1}).HasValue());
        RenderGraphLeaseTransfer scene;
        REQUIRE(
            f.frame.graph
                .ExecuteGraph({{1}, packet.graph, packet.workloads, packet.instances, &f.frame.lease, false, &scene, packet.submissions})
                .HasValue());
        const std::array waits{RenderTimelinePoint{{1}, 1}};
        f.submission = {{1}, {2}, waits, {{1}, 2}};
        auto request = f.Request();
        request.resources = samples;
        request.extendsExistingLease = true;

        MixedEncoder encoder;

        f.output.legacyUnmanagedOutput = true;
        SECTION("later output policy rejects the prepared font upload") {
            f.output.legacyUnmanagedOutput = false;
        }
        SECTION("later resource rejects the prepared font upload") {
            source.current = false;
        }
        SECTION("native ticket admission rejects the prepared font upload") {
            f.frame.commandState.state.beginResult = VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        REQUIRE(f.frame.graph.PrepareGuiComposition(request).HasValue());
        RequireRejectedMixedAttempts(f, request, source, encoder);
        f.output.legacyUnmanagedOutput = true;
        source.current = true;
        f.frame.commandState.state.beginResult = VK_SUCCESS;
        REQUIRE(f.frame.graph.ExecuteGui(request, f.output, encoder).HasValue());
        RequireCompletedMixedTail(f, texture, source, nativeLease);
    }

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
