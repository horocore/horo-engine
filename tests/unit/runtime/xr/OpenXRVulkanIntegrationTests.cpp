#include "../../../support/renderer/MultiviewTestSupport.h"
#include "../renderer/VulkanExternalGraphTestSupport.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenXRMultiviewGraph.h"
#include "OpenXRSwapchainTestSupport.h"
#include "OpenXRVulkanSwapchain.h"
#include "TypedIdentityTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"

namespace {
    using Horo::Result;
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanResourceTest;
    using namespace Horo::Render::VulkanGraphTest;
    using Horo::Render::VulkanExternalTest::ExternalGraphPacket;

    /** @brief Test composition adapter; all image, graph, submission and retirement behavior delegates to production owners. */
    class ExternalGraphBackend final : public IRenderBackend, public Detail::IRenderExternalTextureBackend {
    public:
        static inline const Detail::IVulkanExternalTextureSource
            *lastImportedSource{};  // Synchronous test-only observation, never an owner.

        explicit ExternalGraphBackend(NativeGraphFixture &fixture) : fixture_(&fixture) {
            capabilities_.backend = RenderBackendId{"xr-bridge-test"};
            capabilities_.support.deviceIncarnation = 11;
            capabilities_.support.capabilityRevision = 1;
            capabilities_.support.features.Enable(RenderCapability::BufferResources);
            capabilities_.support.features.Enable(RenderCapability::MeshResources);
            capabilities_.support.features.Enable(RenderCapability::TextureResources);
            capabilities_.support.features.Enable(RenderCapability::RenderTargetResources);
            capabilities_.support.features.Enable(RenderCapability::ViewIndexedShaders);
            capabilities_.support.limits = {65536, 2048, 1, 3, 2, 2};
            capabilities_.support.formats.sampleCountMask = 2;
            capabilities_.support.formats.usages.fill(RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled);
            capabilities_.supportsBufferResources = true;
            capabilities_.supportsMeshResources = true;
            capabilities_.supportsRenderTargetResources = true;
            capabilities_.supportsTextureResources = true;
            capabilities_.supportsExplicitGraphSubmissions = true;
            capabilities_.supportsSubmissionObservation = true;
        }

        Result<void> Initialize(const RenderBackendConfig &) override {
            return Result<void>::Success();
        }

        const RenderBackendCapabilities &Capabilities() const noexcept override {
            return capabilities_;
        }

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &d) const override {
            return fixture_->resources.QueryBufferMemoryCost(d);
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &d) const override {
            return fixture_->resources.QueryTextureMemoryCost(d);
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &d, std::span<const std::byte> b, const RenderMemoryPlacement &p,
                                           RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateBuffer(d, b, p, std::move(l));
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &d, std::uint64_t v, std::uint64_t i,
                                         RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateMesh(d, v, i, std::move(l));
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &d, std::span<const std::byte> b, const RenderMemoryPlacement &p,
                                            RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateTexture(d, b, p, std::move(l));
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &d, std::uint64_t t,
                                                RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateTextureView(d, t, std::move(l));
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &d, std::uint64_t c, std::uint64_t z,
                                                 RenderResourceNativeLifetime l = {}) override {
            return fixture_->resources.CreateRenderTarget(d, c, z, std::move(l));
        }

        Result<std::uint64_t> ImportExternalTexture(const Detail::IRenderExternalTextureSource &s,
                                                    RenderResourceNativeLifetime l) override {
            auto imported = fixture_->resources.ImportExternalTexture(s, std::move(l));
            lastImportedSource = imported.HasValue() ? dynamic_cast<const Detail::IVulkanExternalTextureSource *>(&s) : nullptr;
            return imported;
        }

        void DestroyBuffer(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyBuffer(i);
        }

        void DestroyMesh(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyMesh(i);
        }

        void DestroyTexture(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyTexture(i);
        }

        void DestroyTextureView(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyTextureView(i);
        }

        void DestroyRenderTarget(std::uint64_t i) noexcept override {
            fixture_->resources.DestroyRenderTarget(i);
        }

        Result<FrameToken> BeginFrame(const FrameDescriptor &d) override {
            active_ = {d.frameNumber};
            const auto begun = fixture_->graph.BeginFrame(active_);
            return begun.HasValue() ? Result<FrameToken>::Success(active_) : Result<FrameToken>::Failure(begun.ErrorValue());
        }

        Result<void> Execute(const RenderExecutionPlan &) override {
            return Result<void>::Failure(MakeError(VulkanBackendErrors::UnsupportedOperation));
        }

        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &r) override {
            return fixture_->graph.ExecuteGraph(r);
        }

        Result<void> Present(FrameToken f) override {
            active_ = {};
            return fixture_->graph.SubmitFrame(f);
        }

        void AbortFrame(FrameToken f) noexcept override {
            fixture_->graph.AbortFrame(f);
            active_ = {};
        }

        void AbortActiveFrame() noexcept override {
            fixture_->graph.AbortFrame(active_);
            active_ = {};
        }

        Result<void> Resize(FramebufferExtent) override {
            return Result<void>::Success();
        }

        Result<bool> ObserveSubmission(const RenderCpuWait &w) override {
            return fixture_->graph.ObserveSubmission(w);
        }

        Result<void> BeginShutdown() override {
            return fixture_->graph.BeginShutdown();
        }

        Result<RenderBackendDrainProgress> PollDrain() override {
            auto drained = fixture_->graph.PollDrain();
            if (drained.HasValue() && drained.Value() == RenderBackendDrainProgress::Drained)
                fixture_->resources.ShutdownResources();
            return drained;
        }

    private:
        NativeGraphFixture *fixture_;
        RenderBackendCapabilities capabilities_;
        FrameToken active_;
    };

    class ExternalGraphProvider final : public IRenderBackendProvider {
    public:
        explicit ExternalGraphProvider(NativeGraphFixture &fixture) : fixture_(&fixture) {}

        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<ExternalGraphBackend>(*fixture_));
        }

    private:
        NativeGraphFixture *fixture_;
    };

    namespace XRTest = Horo::Tests::OpenXR;
    namespace NativeXR = Horo::XR::OpenXRInternal;

    class NativeVulkanGraphics final : public NativeXR::IOpenXRGraphicsBinding {
    public:
        XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};

        NativeVulkanGraphics() {
            binding.instance = NativeHandle<VkInstance>(2);
            binding.physicalDevice = NativeHandle<VkPhysicalDevice>(3);
            binding.device = NativeHandle<VkDevice>(1);
            binding.queueFamilyIndex = 3;
        }

        const char *RequiredExtension() const noexcept override {
            return "XR_KHR_vulkan_enable2";
        }

        Horo::Result<void> ValidatePreparation() const override {
            return binding.instance && binding.physicalDevice && binding.device
                       ? Horo::Result<void>::Success()
                       : Horo::Result<void>::Failure(Horo::MakeError(Horo::XR::XRErrors::OperationUnavailable));
        }

        Horo::Result<void> Validate(const Horo::XR::XRSessionId &) const override {
            return Horo::Result<void>::Success();
        }

        Horo::Result<const XrBaseInStructure *> Prepare(XrInstance, XrSystemId, PFN_xrGetInstanceProcAddr) override {
            return Horo::Result<const XrBaseInStructure *>::Success(reinterpret_cast<const XrBaseInStructure *>(&binding));
        }

        void Release() noexcept override {}
    };

    struct NativeXRSessionFixture {
        XRTest::Script script;
        NativeVulkanGraphics graphics;
        XRTest::Fence fence;
        Horo::XR::XRLoaderPreflightSnapshot preflight{XRTest::Preflight()};
        Horo::XR::XRCapabilitySnapshot capabilities{XRTest::Capabilities()};
        Horo::XR::XRFeaturePlan plan{XRTest::Plan(capabilities)};
        std::shared_ptr<XRTest::Library> library{std::make_shared<XRTest::Library>()};
        NativeXR::OpenXRNativeSession owner{library, graphics, fence};
        Horo::XR::XRSessionId identity{capabilities.System(), {.index = 1, .generation = 1}};

        explicit NativeXRSessionFixture(bool depth) {
            script.depthExtension = depth;
            script.extensionCount = depth ? 2 : 1;
            const std::array extensions{NativeXR::NativeExtensionRequest{"XR_KHR_composition_layer_depth", depth}};
            REQUIRE(owner
                        .Create({.candidate = identity,
                                 .preflight = preflight,
                                 .capabilities = capabilities,
                                 .plan = plan,
                                 .activeAttempt = preflight.Attempt(),
                                 .extensions = extensions})
                        .HasValue());
        }
    };

    /** @brief Complete XR/native/frontend composition with deterministic reverse-order failure cleanup. */
    struct XRBridgeFixture {
        NativeGraphFixture native;
        NativeXRSessionFixture session;
        XRTest::SwapchainScript xr;
        std::unique_ptr<RenderFrontend> frontend;
        NativeXR::OpenXRVulkanSwapchainImages images;
        NativeXR::OpenXRSwapchain swapchain;
        NativeXR::NativeSwapchainRequest request;
        NativeXR::NativeSwapchainAcquisition acquired;

        static std::unique_ptr<RenderFrontend> CreateFrontend(NativeGraphFixture &native) {
            RenderBackendRegistry registry;
            REQUIRE(
                registry.Register({RenderBackendId{"xr-bridge-test"}, "XR bridge test", std::make_unique<ExternalGraphProvider>(native)})
                    .HasValue());
            REQUIRE(registry.Seal().HasValue());
            return SubmissionTest::Take(RenderFrontend::Create(registry, RenderBackendId{"xr-bridge-test"}, {}));
        }

        explicit XRBridgeFixture(const bool depth,
                                 const Detail::VulkanExternalViewDispatch views = {&FakeCreateImageView, &FakeDestroyImageView})
            : session(depth), frontend(CreateFrontend(native)), images(*frontend, session.graphics, session.graphics.binding, {1}, views),
              swapchain(session.owner, images),
              request{.target = {session.identity, {.index = 1, .generation = 1}},
                      .texture = {.extent = {640, 480},
                                  .format = depth ? RenderTextureFormat::Depth32Float : RenderTextureFormat::Rgba8Unorm,
                                  .layerCount = 2,
                                  .usage = RenderTextureUsage::RenderAttachment},
                      .role = depth ? NativeXR::NativeSwapchainRole::Depth : NativeXR::NativeSwapchainRole::Color} {
            xr.format = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
            REQUIRE(images.Bind(swapchain).HasValue());
        }

        ~XRBridgeFixture() {
            native.commandState.state.Complete();
            (void)frontend->ObserveSubmission({.completion = {{1}, 1}, .expectedDeviceIncarnation = 11});
            (void)swapchain.Close();
            // Drain native commands while retaining the selected device and frontend registry.
            (void)native.graph.BeginShutdown();
            (void)native.graph.PollDrain();
            (void)swapchain.Close();
            (void)session.owner.Close();
            (void)frontend->BeginShutdown();
            (void)frontend->PollDrain();
        }

        void Acquire() {
            REQUIRE(swapchain.Create(request, XRTest::SwapchainScript::GetProc).HasValue());
            REQUIRE(swapchain.Acquire().HasValue());
            REQUIRE(swapchain.Wait(0).Value());
            acquired = SubmissionTest::Take(swapchain.Import());
        }

        ExternalGraphPacket Packet() const {
            const bool depth = request.role == NativeXR::NativeSwapchainRole::Depth;
            const RenderGraphLogicalState final{RenderGraphSynchronizationAccess::ReadWrite,
                                                depth ? RenderGraphSynchronizationOperation::DepthStencilAttachment
                                                      : RenderGraphSynchronizationOperation::ColorAttachment,
                                                RenderGraphPipelineScope::External,
                                                depth ? RenderGraphTextureLayout::DepthStencilAttachment
                                                      : RenderGraphTextureLayout::ColorAttachment,
                                                {1}};
            return VulkanExternalTest::ExternalGraph(acquired.texture, request.texture, final);
        }

        void Render(const ExternalGraphPacket &packet) {
            auto frame = SubmissionTest::Take(frontend->BeginFrame({1, {640, 480}}));
            const auto executed = frame.ExecuteSubmittedGraph(packet.graph, packet.workloads, packet.submissions);
            INFO((executed.HasError() ? executed.ErrorValue().message : "complete XR frontend/native graph admitted"));
            REQUIRE(executed.HasValue());
            REQUIRE(frame.Present().HasValue());
        }

        void Complete() {
            native.commandState.state.Complete();
            const auto completed = frontend->ObserveSubmission({.completion = {{1}, 1}, .expectedDeviceIncarnation = 11});
            REQUIRE(completed.HasValue());
            REQUIRE(completed.Value());
        }

        void RejectReacquiredStaleGraph(const ExternalGraphPacket &packet) {
            REQUIRE(swapchain.Acquire().HasValue());
            REQUIRE(swapchain.Wait(0).Value());
            const auto replacement = SubmissionTest::Take(swapchain.Import());
            CHECK(replacement.image.slot.index == acquired.image.slot.index);
            CHECK(replacement.image.slot.generation > acquired.image.slot.generation);
            CHECK(replacement.texture != acquired.texture);
            auto staleFrame = SubmissionTest::Take(frontend->BeginFrame({2, {640, 480}}));
            CHECK(staleFrame.ExecuteSubmittedGraph(packet.graph, packet.workloads, packet.submissions).HasError());
            CHECK(native.commandState.state.accepted.size() == 1);
            // No second submission was accepted: resolve this acquisition through the unrendered abort path.
            REQUIRE(swapchain.PollRelease().Value());
            CHECK(xr.releases == 2);
        }
    };

    TEST_CASE("OpenXR production swapchain adapter renders through frontend graph and releases only after native retirement",
              "[unit][xr][vulkan][external][integration]") {
        bool depth{};
        SECTION("projection color") {}
        SECTION("projection depth") {
            depth = true;
        }
        XRBridgeFixture fixture{depth};
        fixture.Acquire();
        CHECK(fixture.native.resourceState.state.createViewCount == static_cast<int>(fixture.xr.imageCount * 3));
        auto packet = fixture.Packet();
        fixture.Render(packet);
        const auto pending = fixture.swapchain.PollRelease();
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(fixture.swapchain.Current().HasError());
        CHECK(fixture.xr.releases == 0);
        CHECK(fixture.native.resourceState.state.destroyViewCount == 0);
        fixture.Complete();
        const auto released = fixture.swapchain.PollRelease();
        INFO((released.HasError() ? released.ErrorValue().message : "native XR release progressed"));
        REQUIRE(released.HasValue());
        CHECK(released.Value());
        CHECK(fixture.xr.releases == 1);
        CHECK(fixture.native.resourceState.state.destroyImageCount == 0);
        CHECK(fixture.native.commandState.state.waitCalls == 0);
        fixture.RejectReacquiredStaleGraph(packet);
        REQUIRE(fixture.swapchain.Close().Value());
        CHECK(fixture.xr.destroys == 1);
        CHECK(fixture.native.resourceState.state.createViewCount == static_cast<int>(fixture.xr.imageCount * 3));
        CHECK(fixture.native.resourceState.state.destroyViewCount == static_cast<int>(fixture.xr.imageCount * 3));
    }

    std::vector<VkImageViewCreateInfo> coldViewRequests;

    VKAPI_ATTR VkResult VKAPI_CALL RecordColdView(VkDevice device, const VkImageViewCreateInfo *info,
                                                  const VkAllocationCallbacks *callbacks, VkImageView *view) {
        coldViewRequests.push_back(*info);
        return FakeCreateImageView(device, info, callbacks, view);
    }

    TEST_CASE("OpenXR stereo layer views are prepared before acquisition while only the sampled first eye admits GUI mirroring",
              "[xr][vulkan][mirror]") {
        coldViewRequests.clear();
        XRBridgeFixture fixture{false, {&RecordColdView, &FakeDestroyImageView}};
        fixture.request.texture.usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled;
        fixture.Acquire();
        const auto coldCount = static_cast<int>(fixture.xr.imageCount * 3);
        CHECK(fixture.native.resourceState.state.createViewCount == coldCount);
        REQUIRE(coldViewRequests.size() == static_cast<std::size_t>(coldCount));
        for (std::size_t image = 0; image < fixture.xr.imageCount; ++image) {
            const auto &array = coldViewRequests[image * 3];
            CHECK(array.viewType == VK_IMAGE_VIEW_TYPE_2D_ARRAY);
            CHECK(array.subresourceRange.layerCount == 2);
            for (std::uint32_t layer = 0; layer < 2; ++layer) {
                const auto &view = coldViewRequests[image * 3 + layer + 1];
                CHECK(view.image == array.image);
                CHECK(view.viewType == VK_IMAGE_VIEW_TYPE_2D);
                CHECK(view.subresourceRange.baseArrayLayer == layer);
                CHECK(view.subresourceRange.layerCount == 1);
                CHECK(view.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
            }
        }
        const auto *source = ExternalGraphBackend::lastImportedSource;
        REQUIRE(source != nullptr);
        REQUIRE(source->PreparedNativeViews().layerCount == 2);
        CHECK(source->PreparedNativeViews().layers[0] != VK_NULL_HANDLE);
        CHECK(source->PreparedNativeViews().layers[1] != VK_NULL_HANDLE);
        RenderTextureViewDescriptor eye{.texture = fixture.acquired.texture};
        CHECK(source->AdmitsFirstEyeGuiMirror(eye));
        eye.baseLayer = 1;
        CHECK_FALSE(source->AdmitsFirstEyeGuiMirror(eye));
        eye.baseLayer = 0;
        eye.format = RenderTextureFormat::Rgba8UnormSrgb;
        CHECK_FALSE(source->AdmitsFirstEyeGuiMirror(eye));
        fixture.Render(fixture.Packet());
        CHECK(coldViewRequests.size() == static_cast<std::size_t>(coldCount));
        fixture.Complete();
        REQUIRE(fixture.swapchain.PollRelease().Value());
        CHECK(fixture.native.resourceState.state.destroyViewCount == 0);
        REQUIRE(fixture.swapchain.Close().Value());
        CHECK(fixture.native.resourceState.state.createViewCount == coldCount);
        CHECK(fixture.native.resourceState.state.destroyViewCount == coldCount);
    }

    int viewAttempts{};
    int failedViewAttempt{2};

    VKAPI_ATTR VkResult VKAPI_CALL FailSecondView(VkDevice device, const VkImageViewCreateInfo *info,
                                                  const VkAllocationCallbacks *callbacks, VkImageView *view) {
        if (++viewAttempts == failedViewAttempt) {
            *view = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return FakeCreateImageView(device, info, callbacks, view);
    }

    TEST_CASE("OpenXR cold image view preparation failure rolls back the prepared prefix before destroying its swapchain",
              "[xr][vulkan][rollback]") {
        viewAttempts = 0;
        SECTION("first eye fails after the array") {
            failedViewAttempt = 2;
        }
        SECTION("second eye fails after the array and first eye") {
            failedViewAttempt = 3;
        }
        XRBridgeFixture fixture{false, {&FailSecondView, &FakeDestroyImageView}};
        const auto created = fixture.swapchain.Create(fixture.request, XRTest::SwapchainScript::GetProc);
        CHECK(created.HasError());
        CHECK(viewAttempts == failedViewAttempt);
        CHECK(fixture.native.resourceState.state.createViewCount == failedViewAttempt - 1);
        CHECK(fixture.native.resourceState.state.destroyViewCount == failedViewAttempt - 1);
        CHECK(fixture.xr.destroys == 1);
        CHECK(std::ranges::find(fixture.xr.calls, "acquire") == fixture.xr.calls.end());
    }

#include "../renderer/OpenXRMultiviewGraphTests.inl"
}  // namespace
