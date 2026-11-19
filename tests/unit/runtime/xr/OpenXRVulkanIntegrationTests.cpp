#include "../../../support/renderer/MultiviewTestSupport.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenXRMultiviewGraph.h"
#include "OpenXRSwapchainTestSupport.h"
#include "OpenXRVulkanSwapchain.h"
#include "RenderFrontendResourceAccess.h"
#include "TypedIdentityTestSupport.h"
#include "VulkanExternalGraphBackendTestSupport.h"
#include "runtime/renderer/modules/vulkan/VulkanRenderBackendErrors.h"
#include "support/OpenXRCensusTestSupport.h"

#include <catch2/generators/catch_generators.hpp>

namespace {
    using Horo::Result;
    using namespace Horo::Render;
    using namespace Horo::Render::VulkanResourceTest;
    using namespace Horo::Render::VulkanGraphTest;
    using Horo::Render::VulkanExternalTest::ExternalGraphPacket;

    using Horo::Render::VulkanExternalTest::ExternalGraphBackend;
    using Horo::Render::VulkanExternalTest::ExternalGraphProvider;

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

        Horo::Result<const XrBaseInStructure *> Prepare(XrInstance, XrSystemId, NativeXR::NativeOpenXRDispatch) override {
            return Horo::Result<const XrBaseInStructure *>::Success(reinterpret_cast<const XrBaseInStructure *>(&binding));
        }

        void Release() noexcept override {}
    };

    struct NativeXRSessionFixture {
        XRTest::Script script;
        XRTest::CensusScript census;
        NativeVulkanGraphics graphics;
        XRTest::Fence fence;
        Horo::XR::XRLoaderPreflightSnapshot preflight{XRTest::Preflight()};
        Horo::XR::XRCapabilitySnapshot capabilities{XRTest::Capabilities()};
        Horo::XR::XRFeaturePlan plan{XRTest::Plan(capabilities)};
        std::shared_ptr<XRTest::CensusLibrary> library{std::make_shared<XRTest::CensusLibrary>()};
        NativeXR::OpenXRNativeSession owner{library, graphics, fence};
        Horo::XR::XRSessionId identity{capabilities.System(), {.index = 1, .generation = 1}};

        explicit NativeXRSessionFixture(const bool depth,
                                        const NativeXR::NativeReprojectionRoute route = NativeXR::NativeReprojectionRoute::None,
                                        const bool enable = true, const bool prepared = false) {
            script.depthExtension = depth && route == NativeXR::NativeReprojectionRoute::None;
            script.synthesisExtension = route == NativeXR::NativeReprojectionRoute::FrameSynthesisExt;
            script.spaceWarpExtension = route == NativeXR::NativeReprojectionRoute::SpaceWarpFb;
            script.extensionCount = 1 + script.depthExtension + script.synthesisExtension + script.spaceWarpExtension;
            std::vector<NativeXR::NativeExtensionRequest> extensions;
            if (script.depthExtension)
                extensions.push_back({XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, true});
            if (enable && route != NativeXR::NativeReprojectionRoute::None)
                extensions.push_back(
                    {script.synthesisExtension ? XR_EXT_FRAME_SYNTHESIS_EXTENSION_NAME : XR_FB_SPACE_WARP_EXTENSION_NAME, true});
            if (prepared) {
                REQUIRE(owner
                            .PrepareDiscovery({.selection = {.contractVersion = preflight.ContractVersion(),
                                                             .attempt = preflight.Attempt(),
                                                             .backend = preflight.Backend(),
                                                             .installRecord = preflight.InstallRecord(),
                                                             .productProfile = preflight.ProductProfile(),
                                                             .admittedLoaderVersions = {{1, 0, 0}, {1, 1, 63}}},
                                               .requestedApi = preflight.LoaderApiVersion(),
                                               .extensions = extensions})
                            .HasValue());
                REQUIRE(owner.PrepareSession({preflight, preflight.Attempt()}).HasValue());
            } else {
                REQUIRE(owner
                            .Create({.candidate = identity,
                                     .preflight = preflight,
                                     .capabilities = capabilities,
                                     .plan = plan,
                                     .activeAttempt = preflight.Attempt(),
                                     .extensions = extensions})
                            .HasValue());
            }
        }

        void Activate() {
            REQUIRE(owner
                        .Activate({.candidate = identity,
                                   .preflight = preflight,
                                   .capabilities = capabilities,
                                   .plan = plan,
                                   .activeAttempt = preflight.Attempt()})
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
                                 const Detail::VulkanExternalViewDispatch views = {&FakeCreateImageView, &FakeDestroyImageView},
                                 const NativeXR::NativeReprojectionRoute route = NativeXR::NativeReprojectionRoute::None,
                                 const bool enable = true, const bool prepared = false)
            : session(depth, route, enable, prepared), frontend(CreateFrontend(native)),
              images(*frontend, session.graphics, session.graphics.binding, {1}, views), swapchain(session.owner, images),
              request{.target = {session.identity, {.index = 1, .generation = 1}},
                      .texture = {.extent = {640, 480},
                                  .format = depth ? RenderTextureFormat::Depth32Float : RenderTextureFormat::Rgba8Unorm,
                                  .layerCount = 2,
                                  .usage = RenderTextureUsage::RenderAttachment},
                      .role = depth ? NativeXR::NativeSwapchainRole::Depth : NativeXR::NativeSwapchainRole::Color} {
            xr.format = depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
            if (route != NativeXR::NativeReprojectionRoute::None) {
                const std::uint32_t right = route == NativeXR::NativeReprojectionRoute::FrameSynthesisExt ? 800 : 640;
                request.reprojection =
                    NativeXR::NativeReprojectionAllocation{session.preflight.Attempt(), route, {{{640, 512}, {right, 512}}}};
                request.role = depth ? NativeXR::NativeSwapchainRole::ReprojectionDepth : NativeXR::NativeSwapchainRole::Motion;
                request.texture.extent = {right, 512};
                request.texture.usage = RenderTextureUsage::RenderAttachment | RenderTextureUsage::Sampled;
                if (!depth) {
                    request.texture.format = RenderTextureFormat::Rgba16Float;
                    xr.format = VK_FORMAT_R16G16B16A16_SFLOAT;
                }
            }
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
            const bool depth = NativeXR::IsDepthSwapchain(request.role);
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
        auto route = NativeXR::NativeReprojectionRoute::None;
        SECTION("projection color") {}
        SECTION("projection depth") {
            depth = true;
        }
        SECTION("EXT motion uses signed floating color and asymmetric native bounds") {
            route = NativeXR::NativeReprojectionRoute::FrameSynthesisExt;
        }
        SECTION("FB synthesis depth without KHR projection depth") {
            route = NativeXR::NativeReprojectionRoute::SpaceWarpFb;
            depth = true;
        }
        XRBridgeFixture fixture{depth, {&FakeCreateImageView, &FakeDestroyImageView}, route};
        CHECK(fixture.session.owner.DiscoverReprojection(fixture.session.preflight.Attempt()).HasError());
        CHECK(fixture.swapchain.CurrentReprojection().HasError());
        fixture.Acquire();
        if (route != NativeXR::NativeReprojectionRoute::None) {
            const auto evidence = fixture.swapchain.CurrentReprojection();
            REQUIRE(evidence.HasValue());
            const auto &acquisition = evidence.Value();
            CHECK(acquisition.evidence.image == fixture.acquired.image);
            CHECK(acquisition.evidence.kind ==
                  (depth ? Horo::XR::XRReprojectionImageKind::Depth : Horo::XR::XRReprojectionImageKind::Motion));
            CHECK(acquisition.evidence.allocation.extent == fixture.request.texture.extent);
            CHECK(acquisition.texture == fixture.acquired.texture);
            CHECK(acquisition.eyeExtents == fixture.request.reprojection->eyeExtents);
            CHECK(fixture.xr.created.format == fixture.xr.format);
            CHECK(fixture.xr.created.usageFlags ==
                  (XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                   (depth ? XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)));
        } else {
            CHECK(fixture.swapchain.CurrentReprojection().HasError());
        }
        CHECK(fixture.native.resourceState.state.createViewCount == static_cast<int>(fixture.xr.imageCount * 3));
        auto packet = fixture.Packet();
        fixture.Render(packet);
        const auto pending = fixture.swapchain.PollRelease();
        REQUIRE(pending.HasValue());
        CHECK_FALSE(pending.Value());
        CHECK(fixture.swapchain.Current().HasError());
        CHECK(fixture.swapchain.CurrentReprojection().HasError());
        CHECK(fixture.swapchain.BorrowReleased(fixture.acquired.image).HasError());
        CHECK(fixture.xr.releases == 0);
        CHECK(fixture.native.resourceState.state.destroyViewCount == 0);
        fixture.Complete();
        const auto released = fixture.swapchain.PollRelease();
        INFO((released.HasError() ? released.ErrorValue().message : "native XR release progressed"));
        REQUIRE(released.HasValue());
        CHECK(released.Value());
        CHECK(fixture.xr.releases == 1);
        CHECK(fixture.swapchain.BorrowReleased(fixture.acquired.image).HasValue());
        CHECK(fixture.native.resourceState.state.destroyImageCount == 0);
        CHECK(fixture.native.commandState.state.waitCalls == 0);
        fixture.RejectReacquiredStaleGraph(packet);
        REQUIRE(fixture.swapchain.Close().Value());
        CHECK(fixture.xr.destroys == 1);
        CHECK(fixture.native.resourceState.state.createViewCount == static_cast<int>(fixture.xr.imageCount * 3));
        CHECK(fixture.native.resourceState.state.destroyViewCount == static_cast<int>(fixture.xr.imageCount * 3));
    }

    TEST_CASE("OpenXR auxiliary allocation rejects unnegotiated, stale or mismatched native sizing before swapchain dispatch",
              "[unit][xr][vulkan][reprojection]") {
        bool enable = true;
        bool staleAttempt = false;
        bool wrongRoute = false;
        bool wrongEyeBounds = false;
        bool missingWriterUsage = false;
        SECTION("only advertised") {
            enable = false;
        }
        SECTION("stale loader attempt") {
            staleAttempt = true;
        }
        SECTION("unselected route") {
            wrongRoute = true;
        }
        SECTION("wrong scale for either eye") {
            wrongEyeBounds = true;
        }
        SECTION("missing sampled usage") {
            missingWriterUsage = true;
        }
        XRBridgeFixture fixture{false,
                                {&FakeCreateImageView, &FakeDestroyImageView},
                                NativeXR::NativeReprojectionRoute::FrameSynthesisExt,
                                enable};
        if (staleAttempt)
            fixture.request.reprojection->attempt = {};
        if (wrongRoute)
            fixture.request.reprojection->route = NativeXR::NativeReprojectionRoute::SpaceWarpFb;
        if (wrongEyeBounds)
            fixture.request.reprojection->eyeExtents.front().width += 1;
        if (missingWriterUsage)
            fixture.request.texture.usage = RenderTextureUsage::RenderAttachment;
        CHECK(fixture.swapchain.Create(fixture.request, XRTest::SwapchainScript::GetProc).HasError());
        CHECK(fixture.xr.calls.empty());
        CHECK(fixture.swapchain.Phase() == NativeXR::NativeSwapchainPhase::Empty);
    }

    TEST_CASE("OpenXR auxiliary allocation cannot select an unadvertised native Vulkan format", "[unit][xr][vulkan][reprojection]") {
        XRBridgeFixture fixture{false, {&FakeCreateImageView, &FakeDestroyImageView}, NativeXR::NativeReprojectionRoute::SpaceWarpFb};
        fixture.xr.format = VK_FORMAT_R8G8B8A8_UNORM;
        CHECK(fixture.swapchain.Create(fixture.request, XRTest::SwapchainScript::GetProc).HasError());
        CHECK(fixture.xr.creates == 0);
        CHECK(fixture.xr.destroys == 0);
        CHECK(fixture.xr.calls == std::vector<std::string_view>{"formats", "formats"});
    }

    TEST_CASE("OpenXR cold auxiliary allocation is adopted once by the exact promoted session without recreating images",
              "[unit][xr][vulkan][reprojection][preactivation]") {
        using enum NativeXR::NativeSwapchainPhase;
        using enum NativeXR::NativeReprojectionRoute;
        const auto route = GENERATE(FrameSynthesisExt, SpaceWarpFb);
        const bool depth = GENERATE(false, true);
        XRBridgeFixture fixture{depth, {&FakeCreateImageView, &FakeDestroyImageView}, route, true, true};
        const auto &owner = fixture.session;
        const auto &request = fixture.request;
        const NativeXR::NativeSessionPreparation session{owner.preflight, owner.preflight.Attempt()};
        const NativeXR::NativePreparedReprojectionSwapchain allocation{request.target.slot, request.texture, request.role,
                                                                       *request.reprojection};
        CHECK_FALSE(owner.owner.Session().IsValid());
        CHECK(fixture.swapchain.AdoptReprojection(owner.identity).HasError());
        auto stale = allocation;
        stale.selection.attempt = Horo::Tests::IdentityValue<Horo::XR::XRLoaderPreflightAttempt>(99);
        CHECK(fixture.swapchain.StageReprojection(session, stale, XRTest::SwapchainScript::GetProc).HasError());
        CHECK(fixture.xr.calls.empty());
        REQUIRE(fixture.swapchain.StageReprojection(session, allocation, XRTest::SwapchainScript::GetProc).HasValue());
        REQUIRE(fixture.swapchain.Phase() == Prepared);
        const auto coldViews = fixture.native.resourceState.state.createViewCount;
        CHECK(coldViews == fixture.xr.imageCount * 3);
        CHECK(fixture.swapchain.Acquire().HasError());
        CHECK(fixture.swapchain.Import().HasError());
        CHECK(fixture.swapchain.AdoptReprojection(owner.identity).HasError());
        CHECK(std::ranges::count(fixture.xr.calls, "acquire") == 0);
        fixture.session.Activate();
        auto replacement = owner.identity;
        ++replacement.slot.generation;
        CHECK(fixture.swapchain.AdoptReprojection(replacement).HasError());
        const auto binding = fixture.session.graphics.binding.device;
        fixture.session.graphics.binding.device = NativeHandle<VkDevice>(5);
        CHECK(fixture.swapchain.AdoptReprojection(owner.identity).HasError());
        fixture.session.graphics.binding.device = binding;
        REQUIRE(fixture.swapchain.AdoptReprojection(owner.identity).HasValue());
        CHECK(fixture.swapchain.Phase() == Available);
        CHECK(fixture.swapchain.AdoptReprojection(owner.identity).HasError());
        CHECK(fixture.xr.creates == 1);
        CHECK(fixture.native.resourceState.state.createViewCount == coldViews);
        REQUIRE(fixture.swapchain.Acquire().HasValue());
        REQUIRE(SubmissionTest::Take(fixture.swapchain.Wait(0)));
        REQUIRE(SubmissionTest::Take(fixture.swapchain.Close()));
        CHECK(fixture.xr.releases == 1);
        CHECK(fixture.xr.destroys == 1);
        CHECK(fixture.native.resourceState.state.destroyViewCount == coldViews);
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

    TEST_CASE("OpenXR cold auxiliary view rollback never exposes an unissued image", "[unit][xr][vulkan][reprojection][preactivation]") {
        using enum NativeXR::NativeSwapchainPhase;
        const bool retry = GENERATE(false, true);
        viewAttempts = 0;
        failedViewAttempt = 3;
        XRBridgeFixture fixture{false,
                                {&FailSecondView, &FakeDestroyImageView},
                                NativeXR::NativeReprojectionRoute::FrameSynthesisExt,
                                true,
                                true};
        const auto &request = fixture.request;
        const auto &preflight = fixture.session.preflight;
        const NativeXR::NativeSessionPreparation session{preflight, preflight.Attempt()};
        const NativeXR::NativePreparedReprojectionSwapchain allocation{request.target.slot, request.texture, request.role,
                                                                       *request.reprojection};
        if (retry)
            fixture.xr.fail = "destroy";
        CHECK(fixture.swapchain.StageReprojection(session, allocation, XRTest::SwapchainScript::GetProc).HasError());
        CHECK(fixture.swapchain.Phase() == (retry ? Closing : Empty));
        CHECK(fixture.native.resourceState.state.createViewCount == 2);
        CHECK(fixture.native.resourceState.state.destroyViewCount == 2);
        CHECK(fixture.xr.destroys == (retry ? 0U : 1U));
        CHECK(fixture.swapchain.Acquire().HasError());
        CHECK(fixture.swapchain.AdoptReprojection(fixture.session.identity).HasError());
        CHECK(std::ranges::count(fixture.xr.calls, "acquire") == 0);
        fixture.xr.fail = {};
        REQUIRE(SubmissionTest::Take(fixture.swapchain.Close()));
        CHECK(fixture.xr.destroys == 1);
    }

#include "../renderer/OpenXRMultiviewGraphTests.inl"
}  // namespace
