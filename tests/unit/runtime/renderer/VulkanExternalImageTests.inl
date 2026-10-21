// Preserved XR image-owner regressions, sharing the canonical native graph fixture.
struct ExternalNativeLease final : IRenderResourceRetirementLease {
    std::uint32_t claims{};
    std::uint32_t releases{};

    void Claim() noexcept override {
        ++claims;
    }

    void Release() noexcept override {
        ++releases;
    }
};

struct ExternalNativeSource final : Detail::IVulkanExternalTextureSource {
    VkDevice device{NativeHandle<VkDevice>(1)};
    VkImage image{NativeHandle<VkImage>(900)};
    bool current{true};
    mutable bool releaseStateProved{};
    std::uint32_t family{};
    RenderTextureDescriptor descriptor{.extent = {640, 480}, .layerCount = 2, .usage = RenderTextureUsage::RenderAttachment};

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
        return device;
    }

    VkImage NativeImage() const noexcept override {
        return image;
    }

    std::uint32_t NativeQueueFamily() const noexcept override {
        return family;
    }

    void RecordNativeRetirement(const RenderGraphLogicalState &state, const bool hasNativeState) const noexcept override {
        const auto required = AcquiredState();
        releaseStateProved = hasNativeState && state.layout == required.layout && state.queue == required.queue &&
                             state.operation == required.operation && state.scope == required.scope;
    }

    RenderGraphLogicalState AcquiredState() const noexcept override {
        const bool depth = descriptor.format == RenderTextureFormat::Depth32Float;
        return {RenderGraphSynchronizationAccess::ReadWrite,
                depth ? RenderGraphSynchronizationOperation::DepthStencilAttachment : RenderGraphSynchronizationOperation::ColorAttachment,
                RenderGraphPipelineScope::Graphics,
                depth ? RenderGraphTextureLayout::DepthStencilAttachment : RenderGraphTextureLayout::ColorAttachment,
                {1}};
    }

    RenderGraphLogicalState RequiredFinalState() const noexcept override {
        auto state = AcquiredState();
        state.scope = RenderGraphPipelineScope::External;
        return state;
    }
};

TEST_CASE("Vulkan external image retirement destroys only Horo views and releases the canonical lifetime",
          "[unit][runtime][renderer][vulkan][external]") {
    NativeResourceScope native;
    ExternalNativeLease lease;
    ExternalNativeSource source;
    Detail::VulkanResourceRuntime runtime;
    InitializeNativeResourceRuntime(runtime);
    const auto imported = runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{lease});
    REQUIRE(imported.HasValue());
    CHECK(native.state.createImageCount == 0);
    CHECK(native.state.allocateCount == 0);
    CHECK(native.state.createViewCount == 1);
    CHECK(lease.claims == 1);
    CHECK(lease.releases == 0);
    CHECK(runtime.ExternalTextureSource(imported.Value()) == &source);
    runtime.DestroyTexture(imported.Value());
    CHECK(native.state.destroyImageCount == 0);
    CHECK(native.state.freeCount == 0);
    CHECK(native.state.destroyViewCount == 1);
    CHECK(lease.releases == 1);
    CHECK(source.releaseStateProved);
    CHECK(runtime.ExternalTextureSource(imported.Value()) == nullptr);
    runtime.DestroyTexture(imported.Value());
    CHECK(lease.releases == 1);
}

TEST_CASE("Vulkan external image admission rejects foreign stale duplicate and failed view imports",
          "[unit][runtime][renderer][vulkan][external]") {
    NativeResourceScope native;
    ExternalNativeLease lease;
    ExternalNativeSource source;
    Detail::VulkanResourceRuntime runtime;
    InitializeNativeResourceRuntime(runtime);
    SECTION("foreign device") {
        source.device = NativeHandle<VkDevice>(2);
    }
    SECTION("stale acquisition") {
        source.current = false;
    }
    SECTION("invalid native image") {
        source.image = VK_NULL_HANDLE;
    }
    SECTION("unsupported sample count") {
        source.descriptor.sampleCount = 4;
    }
    SECTION("native view failure") {
        native.state.createViewResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    SECTION("duplicate native allocation") {
        ExternalNativeLease firstLease;
        const auto first = runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{firstLease});
        REQUIRE(first.HasValue());
        CHECK(runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{lease}).HasError());
        runtime.DestroyTexture(first.Value());
        CHECK(firstLease.releases == 1);
        CHECK(native.state.destroyImageCount == 0);
        return;
    }
    CHECK(runtime.ImportExternalTexture(source, RenderResourceNativeLifetime{lease}).HasError());
    CHECK(lease.claims == 1);
    CHECK(lease.releases == 1);
    CHECK(native.state.destroyImageCount == 0);
    CHECK(native.state.freeCount == 0);
}

struct ExternalGraphPacket {
    CompiledRenderGraphExecution graph;
    std::array<RenderGraphPassWorkload, 1> workloads;
    std::array<RenderGraphResourceInstance, 1> instances;
    const std::array<RenderGraphQueueSubmission, 1> submissions{RenderGraphQueueSubmission{{{1}, {1}, {}, {{1}, 1}}, {0, 1}}};
};

ExternalGraphPacket ExternalGraph(const ExternalNativeSource &source, const std::uint64_t instance, const bool includeFinal = true) {
    auto builder =
        SubmissionTest::Take(RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 1, .maxUsages = 1, .maxDependencies = 1}));
    const auto pass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
    const auto resource = SubmissionTest::Take(builder.ImportTexture({{41}, 1, 1}, RenderGraphResourceClass::External));
    const bool depth = source.descriptor.format == RenderTextureFormat::Depth32Float;
    REQUIRE(builder
                .AddUsage({pass, resource, RenderGraphAccess::Write,
                           depth ? RenderGraphUsageKind::DepthStencilAttachment : RenderGraphUsageKind::ColorAttachment})
                .HasValue());
    auto graph = SubmissionTest::Take(builder.Finalize());
    auto schedule = SubmissionTest::Take(CompileRenderGraph(graph));
    const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    const std::array initial{RenderGraphImportedState{resource, {}, RenderGraphInitialContents::Discarded}};
    const std::array final{RenderGraphFinalState{resource, source.RequiredFinalState()}};
    auto synchronization =
        SubmissionTest::Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial,
                                                                  includeFinal ? std::span<const RenderGraphFinalState>{final}
                                                                               : std::span<const RenderGraphFinalState>{}));
    auto execution = SubmissionTest::Take(CompileRenderGraphExecution(graph, schedule, synchronization, queues));
    RenderGraphWorkload workload = depth ? RenderGraphWorkload{RenderGraphDepthAttachment{resource, 1.0F}}
                                         : RenderGraphWorkload{RenderGraphColorAttachment{resource, {}}};
    return {std::move(execution), {RenderGraphPassWorkload{pass, std::move(workload)}}, {RenderGraphResourceInstance{resource, instance}}};
}

TEST_CASE("Vulkan actual external color and depth graph encodes release after rendering and retires after GPU completion",
          "[unit][runtime][renderer][vulkan][external][graph]") {
    ExternalNativeLease nativeLease;
    ExternalNativeSource source;
    source.family = 3;
    SECTION("color array") {}
    SECTION("depth array") {
        source.descriptor.format = RenderTextureFormat::Depth32Float;
    }
    NativeGraphFixture fixture;
    const auto imported = fixture.resources.ImportExternalTexture(source, RenderResourceNativeLifetime{nativeLease});
    REQUIRE(imported.HasValue());
    auto packet = ExternalGraph(source, imported.Value());
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    const auto execution = fixture.graph.ExecuteGraph(
        {{1}, packet.graph, packet.workloads, packet.instances, &fixture.lease, false, &transfer, packet.submissions});
    INFO(execution.HasError() ? execution.ErrorValue().message : "external native graph admitted");
    REQUIRE(execution.HasValue());
    REQUIRE(fixture.encoded.imageBarriers.size() == 2);
    CHECK(fixture.encoded.layers == 2);
    CHECK(fixture.encoded.depthAttachment == (source.descriptor.format == RenderTextureFormat::Depth32Float));
    REQUIRE(fixture.encoded.operations.size() == 4);
    CHECK(fixture.encoded.operations[0] == "barrier");
    CHECK(fixture.encoded.operations[1] == "begin-rendering");
    CHECK(fixture.encoded.operations[2] == "end-rendering");
    CHECK(fixture.encoded.operations[3] == "barrier");
    CHECK(fixture.encoded.imageBarriers.back().image == source.image);
    CHECK(fixture.encoded.imageBarriers.back().subresourceRange.layerCount == 2);
    REQUIRE(fixture.graph.SubmitFrame({1}).HasValue());
    fixture.resources.DestroyTexture(imported.Value());
    CHECK(nativeLease.releases == 0);
    CHECK_FALSE(source.releaseStateProved);
    const RenderCpuWait poll{.completion = {{1}, 1}, .expectedDeviceIncarnation = 11};
    const auto pending = fixture.graph.ObserveSubmission(poll);
    REQUIRE(pending.HasValue());
    CHECK_FALSE(pending.Value());
    CHECK(fixture.resourceState.state.destroyViewCount == 0);
    fixture.commandState.state.Complete();
    const auto complete = fixture.graph.ObserveSubmission(poll);
    REQUIRE(complete.HasValue());
    CHECK(complete.Value());
    CHECK(nativeLease.releases == 1);
    CHECK(source.releaseStateProved);
    CHECK(fixture.resourceState.state.destroyViewCount == 1);
    CHECK(fixture.resourceState.state.destroyImageCount == 0);
    CHECK(fixture.commandState.state.waitCalls == 0);
}

TEST_CASE("Vulkan external release rejects mismatched native queue and revoked acquisition before command recording",
          "[unit][runtime][renderer][vulkan][external][graph]") {
    ExternalNativeLease nativeLease;
    ExternalNativeSource source;
    source.family = 3;
    NativeGraphFixture fixture;
    const auto imported = fixture.resources.ImportExternalTexture(source, RenderResourceNativeLifetime{nativeLease});
    REQUIRE(imported.HasValue());
    bool includeFinal = true;
    SECTION("missing release obligation") {
        includeFinal = false;
    }
    auto packet = ExternalGraph(source, imported.Value(), includeFinal);
    SECTION("native family differs") {
        source.family = 7;
    }
    SECTION("acquisition revoked") {
        source.current = false;
    }
    RenderGraphLeaseTransfer transfer;
    REQUIRE(fixture.graph.BeginFrame({1}).HasValue());
    const auto rejected = fixture.graph.ExecuteGraph(
        {{1}, packet.graph, packet.workloads, packet.instances, &fixture.lease, false, &transfer, packet.submissions});
    CHECK(rejected.HasError());
    CHECK(fixture.commandState.state.beginCalls == 0);
    CHECK(fixture.commandState.state.accepted.empty());
    CHECK(fixture.encoded.operations.empty());
    fixture.graph.AbortFrame({1});
    fixture.resources.DestroyTexture(imported.Value());
}

#if defined(HORO_TEST_OPENXR_VULKAN)
/** @brief Test composition adapter; all image, graph, submission and retirement behavior delegates to production owners. */
class ExternalGraphBackend final : public IRenderBackend, public Detail::IRenderExternalTextureBackend {
public:
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
        capabilities_.support.formats.usages.fill(RenderTextureUsage::RenderAttachment);
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

    Result<std::uint64_t> ImportExternalTexture(const Detail::IRenderExternalTextureSource &s, RenderResourceNativeLifetime l) override {
        return fixture_->resources.ImportExternalTexture(s, std::move(l));
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

TEST_CASE("OpenXR production swapchain adapter renders through frontend graph and releases only after native retirement",
          "[unit][xr][vulkan][external][integration]") {
    bool depth{};
    SECTION("projection color") {}
    SECTION("projection depth") {
        depth = true;
    }
    NativeGraphFixture native;
    NativeXRSessionFixture session{depth};
    XRTest::SwapchainScript xr;
    xr.format = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
    RenderBackendRegistry registry;
    REQUIRE(registry.Register({RenderBackendId{"xr-bridge-test"}, "XR bridge test", std::make_unique<ExternalGraphProvider>(native)})
                .HasValue());
    REQUIRE(registry.Seal().HasValue());
    auto frontend = SubmissionTest::Take(RenderFrontend::Create(registry, RenderBackendId{"xr-bridge-test"}, {}));
    NativeXR::OpenXRVulkanSwapchainImages images{*frontend, session.graphics, session.graphics.binding, {1}};
    NativeXR::OpenXRSwapchain swapchain{session.owner, images};
    REQUIRE(images.Bind(swapchain).HasValue());

    struct Cleanup {
        NativeGraphFixture &native;
        RenderFrontend &frontend;
        NativeXR::OpenXRSwapchain &swapchain;

        ~Cleanup() {
            native.commandState.state.Complete();
            (void)frontend.ObserveSubmission({.completion = {{1}, 1}, .expectedDeviceIncarnation = 11});
            (void)swapchain.Close();
            (void)frontend.BeginShutdown();
            (void)frontend.PollDrain();
            (void)swapchain.Close();
        }
    } cleanup{native, *frontend, swapchain};

    const NativeXR::NativeSwapchainRequest request{.target = {session.identity, {.index = 1, .generation = 1}},
                                                   .texture = {.extent = {640, 480},
                                                               .format = depth ? RenderTextureFormat::Depth32Float
                                                                               : RenderTextureFormat::Rgba8Unorm,
                                                               .layerCount = 2,
                                                               .usage = RenderTextureUsage::RenderAttachment},
                                                   .role =
                                                       depth ? NativeXR::NativeSwapchainRole::Depth : NativeXR::NativeSwapchainRole::Color};
    REQUIRE(swapchain.Create(request, XRTest::SwapchainScript::GetProc).HasValue());
    REQUIRE(swapchain.Acquire().HasValue());
    REQUIRE(swapchain.Wait(0).Value());
    const auto acquisition = SubmissionTest::Take(swapchain.Import());
    auto builder =
        SubmissionTest::Take(RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 1, .maxUsages = 1, .maxDependencies = 1}));
    const auto pass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
    const auto resource = SubmissionTest::Take(builder.ImportTexture(acquisition.texture, RenderGraphResourceClass::External));
    REQUIRE(builder
                .AddUsage({pass, resource, RenderGraphAccess::Write,
                           depth ? RenderGraphUsageKind::DepthStencilAttachment : RenderGraphUsageKind::ColorAttachment})
                .HasValue());
    auto graph = SubmissionTest::Take(builder.Finalize());
    auto schedule = SubmissionTest::Take(CompileRenderGraph(graph));
    const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
    const std::array initial{RenderGraphImportedState{resource, {}, RenderGraphInitialContents::Discarded}};
    const std::array final{
        RenderGraphFinalState{resource,
                              {RenderGraphSynchronizationAccess::ReadWrite,
                               depth ? RenderGraphSynchronizationOperation::DepthStencilAttachment
                                     : RenderGraphSynchronizationOperation::ColorAttachment,
                               RenderGraphPipelineScope::External,
                               depth ? RenderGraphTextureLayout::DepthStencilAttachment : RenderGraphTextureLayout::ColorAttachment,
                               {1}}}};
    const auto synchronization = SubmissionTest::Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial, final));
    auto execution = SubmissionTest::Take(CompileRenderGraphExecution(graph, schedule, synchronization, queues));
    const std::array workloads{RenderGraphPassWorkload{pass, depth ? RenderGraphWorkload{RenderGraphDepthAttachment{resource}}
                                                                   : RenderGraphWorkload{RenderGraphColorAttachment{resource, {}}}}};
    const std::array submissions{RenderGraphQueueSubmission{{{1}, {1}, {}, {{1}, 1}}, {0, 1}}};
    auto frame = SubmissionTest::Take(frontend->BeginFrame({1, {640, 480}}));
    const auto executed = frame.ExecuteSubmittedGraph(execution, workloads, submissions);
    INFO(executed.HasError() ? executed.ErrorValue().message : "complete XR frontend/native graph admitted");
    REQUIRE(executed.HasValue());
    REQUIRE(frame.Present().HasValue());
    REQUIRE(swapchain.PollRelease().HasValue());
    CHECK(swapchain.Current().HasError());
    CHECK(xr.releases == 0);
    CHECK(native.resourceState.state.destroyViewCount == 0);
    native.commandState.state.Complete();
    const auto completed = frontend->ObserveSubmission({.completion = {{1}, 1}, .expectedDeviceIncarnation = 11});
    REQUIRE(completed.HasValue());
    REQUIRE(completed.Value());
    const auto released = swapchain.PollRelease();
    INFO(released.HasError() ? released.ErrorValue().message : "native XR release progressed");
    REQUIRE(released.HasValue());
    CHECK(released.Value());
    CHECK(xr.releases == 1);
    CHECK(native.resourceState.state.destroyImageCount == 0);
    CHECK(native.commandState.state.waitCalls == 0);
    REQUIRE(swapchain.Acquire().HasValue());
    REQUIRE(swapchain.Wait(0).Value());
    const auto replacement = SubmissionTest::Take(swapchain.Import());
    CHECK(replacement.image.slot.index == acquisition.image.slot.index);
    CHECK(replacement.image.slot.generation > acquisition.image.slot.generation);
    CHECK(replacement.texture != acquisition.texture);
    auto staleFrame = SubmissionTest::Take(frontend->BeginFrame({2, {640, 480}}));
    CHECK(staleFrame.ExecuteSubmittedGraph(execution, workloads, submissions).HasError());
    CHECK(native.commandState.state.accepted.size() == 1);
    // No second submission was accepted: this acquisition follows the legal unrendered abort path.
    REQUIRE(swapchain.PollRelease().Value());
    CHECK(xr.releases == 2);
    REQUIRE(swapchain.Close().Value());
    CHECK(xr.destroys == 1);
}

#include "OpenXRMultiviewGraphTests.inl"
#endif
