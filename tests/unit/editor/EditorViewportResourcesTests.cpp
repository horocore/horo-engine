#include "../../support/renderer/RenderBackendDrainTestSupport.h"
#include "../runtime/renderer/RenderFrontendTestSupport.h"
#include "EditorViewportMeshReadiness.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraph.h"
#include "editor/renderer/EditorGraphViewportImageCapture.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/renderer/EditorViewportResources.h"
#include "runtime/renderer/frontend/RenderFrontendErrors.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <memory>

#ifdef HORO_TEST_VIEWPORT_MCP
#include "../mcp/McpAuthorizationTestSupport.h"
#include "EditorCommandMcpSession.h"
#include "Horo/Mcp/McpInProcessAdapter.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#endif

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Render;

    constexpr int MaxResourcePasses = 10;

    class ImageResolverFixture final {
    public:
        ImageResolverFixture() noexcept {
            active_ = this;
        }

        ~ImageResolverFixture() {
            active_ = nullptr;
        }

        ImageResolverFixture(const ImageResolverFixture &) = delete;
        ImageResolverFixture &operator=(const ImageResolverFixture &) = delete;

        [[nodiscard]] static Result<std::uintptr_t> Resolve(const RenderFrontend &, const RenderTextureViewHandle view) {
            REQUIRE(active_ != nullptr);
            active_->lastView = view;
            if (active_->reject) {
                return Result<std::uintptr_t>::Failure(
                    MakeError(RendererErrors::ViewportGeometryCreationFailed, "Injected viewport image resolution failure."));
            }
            return Result<std::uintptr_t>::Success(view.slot);
        }

        RenderTextureViewHandle lastView;
        bool reject{false};

    private:
        static thread_local ImageResolverFixture *active_;
    };

    thread_local ImageResolverFixture *ImageResolverFixture::active_ = nullptr;

    [[nodiscard]] std::unique_ptr<RenderFrontend> CreateFrontend(const RenderResourceUploadLimits &uploadLimits = {}) {
        RenderBackendRegistry registry;
        REQUIRE(RegisterNullRenderBackend(registry).HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto created = RenderFrontend::Create(registry, RenderBackendId{"null"}, RenderBackendConfig{}, uploadLimits);
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    [[nodiscard]] EditorViewportResources CreateResources(RenderFrontend &frontend) {
        return EditorViewportResources{
            frontend,
            {.depthFormat = RenderTextureFormat::Depth32Float,
             .depthAspect = RenderTextureAspect::Depth,
             .resolveImage = &ImageResolverFixture::Resolve},
        };
    }

    /** @brief Provides explicit reconstruction admission metadata without changing the Null backend's capability contract. */
    [[nodiscard]] std::unique_ptr<RenderFrontend> CreateTemporalResourceFrontend() {
        using namespace Horo::Tests::RenderFrontendTesting;
        using enum RenderCapability;
        using enum RenderTextureUsage;
        lifecycleState = {};
        RenderCapabilitySnapshot support{.capabilityRevision = 1};
        for (const auto feature : {TextureResources, RenderTargetResources, NativeReconstruction, Compute})
            support.features.Enable(feature);
        support.queues.graphics = true;
        support.queues.compute = true;
        support.limits.maxTextureDimension2D = 4096;
        support.formats.sampleCountMask = 1 << 1;
        support.formats.usages.fill(Sampled | RenderAttachment | Storage | CopySource);
        lifecycleState.syntheticCapabilities = support;
        return CreateTrackingFrontend();
    }

    void SettleResources(EditorViewportResources &resources, RenderFrontend &frontend, const RenderSceneView &scene,
                         const EditorViewportExtent extent) {
        for (int step = 0; step < MaxResourcePasses && (!resources.IsReady() || resources.AllocatedExtent() != extent); ++step) {
            const auto prepared = resources.Prepare(scene, extent);
            INFO((prepared.HasError() ? prepared.ErrorValue().message : "viewport resources prepared"));
            REQUIRE(prepared.HasValue());
            REQUIRE(frontend.ProcessResourceRequests().HasValue());
        }
        REQUIRE(resources.IsReady());
        REQUIRE(resources.AllocatedExtent() == extent);
    }

    struct MeshSceneFixture {
        std::array<MeshVertex, 3> vertices{{
            {.position = {-1.0F, -1.0F, 0.0F}, .normal = {0.0F, 0.0F, 1.0F}, .uv = {0.0F, 0.0F}},
            {.position = {1.0F, -1.0F, 0.0F}, .normal = {0.0F, 0.0F, 1.0F}, .uv = {1.0F, 0.0F}},
            {.position = {0.0F, 1.0F, 0.0F}, .normal = {0.0F, 0.0F, 1.0F}, .uv = {0.5F, 1.0F}},
        }};
        std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::array<RenderMeshResourceView, 1> resources;

        [[nodiscard]] RenderSceneView Scene(const std::uint32_t generation) {
            resources[0] = {
                .handle = {.id = MeshResourceId{41}, .generation = generation},
                .vertices = vertices,
                .indices = indices,
                .localBounds = {{-1.0F, -1.0F, 0.0F}, {1.0F, 1.0F, 0.0F}},
            };
            return {.meshResources = resources};
        }
    };

#ifdef HORO_TEST_VIEWPORT_MCP
    /** @brief Exercises the production graph capture owner with a deterministic readback port. */
    class GraphCaptureViewport final : public IEditorViewportRenderer {
    public:
        GraphCaptureViewport(RenderFrontend &frontend, EditorViewportResources &resources)
            : frontend_(frontend), resources_(resources), extent_(resources.AllocatedExtent()) {}

        GraphCaptureViewport(const GraphCaptureViewport &) = delete;
        GraphCaptureViewport &operator=(const GraphCaptureViewport &) = delete;

        ~GraphCaptureViewport() override {
            capture_.Shutdown(frontend_);
        }

        Result<std::uint64_t> RequestImageCapture(EditorViewportImageRegion region, std::function<bool()> authorized) override {
            ++requests_;
            return capture_.Request(Source(), region, std::move(authorized));
        }

        std::optional<EditorViewportImageSnapshot> ReadImageCapture(std::uint64_t id) override {
            return capture_.Read(id, Source());
        }

        bool CancelImageCapture(std::uint64_t id) noexcept override {
            return capture_.Cancel(id);
        }

        /** @brief Drive accepted staging without depending on a physical GPU. */
        void CompleteFrame() {
            const EditorGraphViewportImageCapture::Readback readback{nullptr, [](void *, std::uint64_t, RenderTimelinePoint,
                                                                                 std::span<std::byte> pixels) {
                std::ranges::fill(pixels, std::byte{0x42});
                return Result<bool>::Success(true);
            }};
            capture_.Prepare(frontend_, Source(), readback);
            REQUIRE(capture_.Pending());
            capture_.Recorded();
            capture_.Finish(true);
            capture_.Prepare(frontend_, Source(), readback);
        }

        Result<void> ExecutePreparedFrame(RenderFrameScope &, std::span<const RenderPassDescriptor>) override {
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        }

        Result<void> ExecuteStaticMeshPass(const StaticMeshPassDescriptor &) override {
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        }

        void RequestExtent(EditorViewportExtent extent) noexcept override {
            extent_ = extent;
        }

        void RequestGrid(const EditorViewportGridOptions &) noexcept override {
            // The fixture records only viewport readback work.
        }

        void RequestLightVisualizer(const EditorViewportLightVisualizerOptions &) noexcept override {
            // The fixture records only viewport readback work.
        }

        EditorViewportExtent RequestedExtent() const noexcept override {
            return extent_;
        }

        Math::ClipDepthRange ClipDepthRange() const noexcept override {
            return Math::ClipDepthRange::ZeroToOne;
        }

        EditorViewportTextureView TextureView() const noexcept override {
            return {};
        }

        bool IsReady() const noexcept override {
            return resources_.IsReady();
        }

        [[nodiscard]] std::size_t CaptureRequests() const noexcept {
            return requests_;
        }

    private:
        EditorGraphViewportImageCapture::Source Source() const {
            return {resources_.Target(), resources_.PresentationTexture(), {extent_.width, extent_.height}};
        }

        RenderFrontend &frontend_;
        EditorViewportResources &resources_;
        EditorViewportExtent extent_;
        EditorGraphViewportImageCapture capture_;
        std::size_t requests_{};
    };
#endif
}  // namespace

#ifdef HORO_TEST_VIEWPORT_MCP
TEST_CASE("Approved viewport pixels are published through the workspace and withdrawn on retirement",
          "[unit][editor][mcp][graph-image-capture]") {
    using Json = nlohmann::json;
    using namespace Horo::Mcp;
    auto frontend = CreateFrontend();
    ImageResolverFixture resolver;
    auto resources = CreateResources(*frontend);
    SettleResources(resources, *frontend, {}, {64, 32});
    GraphCaptureViewport viewport{*frontend, resources};

    CancellationSource process;
    Runtime::RuntimeSceneService scenes;
    EditorWorkspaceController workspace{"test-project", scenes, {}};
    REQUIRE(scenes.Startup(process.Token()).HasValue());
    const Runtime::FrameContext frame{1, {}, 0.0, 0, {}, false, process.Token()};
    REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
    workspace.SynchronizeRuntimeScenePreview();
    auto opened =
        EditorCommandMcpSession::Create({.debugCapture = workspace.BindDebugCapture("project-one", 3, &viewport)},
                                        {"viewport-client", "project-one", 3, {"editor.debug.capture"}}, Horo::Mcp::Test::Authorization());
    REQUIRE(opened.HasValue());
    auto &session = *opened.Value();
    const auto client = session.Client();
    const auto command = [](const char *name, Json arguments = Json::object()) {
        return McpRequest{.id = name, .method = "tools/call", .params = {{"name", name}, {"arguments", std::move(arguments)}}};
    };
    const auto approve = [&](const McpRequest &request) {
        const auto challenge = session.RequestApproval(request);
        REQUIRE(challenge.HasValue());
        REQUIRE(session.Decide(challenge.Value(), true).HasValue());
    };
    const auto submit = [&](const McpRequest &request) {
        const auto accepted = client->Call(request);
        REQUIRE(accepted.HasValue());
        REQUIRE(session.Pump().HasValue());
        return McpRequest{.id = "image-result",
                          .method = "operations/get",
                          .params = {{"operationId", accepted.Value().at("operationId")}}};
    };
    const auto execute = [&](const McpRequest &request) {
        auto completed = client->Call(submit(request));
        REQUIRE(completed.HasValue());
        return std::move(completed).Value();
    };

    Json crop{{"source", execute(command("viewport.image_source")).at("result")}, {"x", 4}, {"y", 3}, {"width", 2}, {"height", 1}};
    const auto stale = command("viewport.capture_image", crop);
    CHECK(client->Call(stale).HasError());
    approve(stale);
    EditorWorkspaceViewCommandData navigate;
    navigate.command = EditorWorkspaceViewCommand::NavigateViewport;
    navigate.viewportNavigationPayload = EditorViewportNavigationDelta{.yawRadians = 0.2F};
    workspace.ProcessCommand(navigate);
    CHECK(execute(stale).at("status") == "failed");
    CHECK(viewport.CaptureRequests() == 0);

    crop["source"] = execute(command("viewport.image_source")).at("result");
    const auto request = command("viewport.capture_image", crop);
    approve(request);
    const auto started = execute(request);
    REQUIRE(started.at("status") == "succeeded");
    REQUIRE(viewport.CaptureRequests() == 1);
    const auto read = command("debug.get_image", started.at("result"));
    CHECK(execute(read).at("result").at("phase") == "pending");
    viewport.CompleteFrame();
    const auto delivery = submit(read);
    const auto ready = client->Call(delivery);
    REQUIRE(ready.HasValue());
    CHECK(ready.Value().at("result").at("data") == "4242424242424242");
    CHECK(client->Call(delivery).HasError());

    const auto pendingDelivery = submit(read);
    workspace.RevokeWorkspaceCommands();
    CHECK(client->Call(pendingDelivery).HasError());
    REQUIRE(session.Close().HasValue());
    resources.Shutdown();
}
#endif

TEST_CASE("Graph viewport observes a copy without a timeline only after frame acceptance", "[unit][editor][graph-image-capture]") {
    auto frontend = CreateFrontend();
    ImageResolverFixture imageResolver;
    auto resources = CreateResources(*frontend);
    SettleResources(resources, *frontend, {}, {64, 32});
    const EditorGraphViewportImageCapture::Source source{resources.Target(), resources.PresentationTexture(), {64, 32}};
    EditorGraphViewportImageCapture capture;
    constexpr EditorViewportImageRegion region{4, 3, 2, 1};
    CHECK(capture.Request(source, region, {}).HasError());
    CHECK(capture
              .Request(source, region, [] {
        return false;
    }).HasError());
    const auto requested = capture.Request(source, region, [] {
        return true;
    });
    REQUIRE(requested.HasValue());
    unsigned polls{};
    const EditorGraphViewportImageCapture::Readback reader{&polls, [](void *context, std::uint64_t instance, RenderTimelinePoint signal,
                                                                      std::span<std::byte> bytes) {
        CHECK(instance != 0);
        CHECK_FALSE(signal.IsValid());
        ++*static_cast<unsigned *>(context);
        std::ranges::fill(bytes, std::byte{0x42});
        return Result<bool>::Success(true);
    }};
    capture.Prepare(*frontend, source, reader);
    REQUIRE(capture.Pending());
    capture.Recorded();
    capture.Prepare(*frontend, source, reader);
    CHECK(polls == 0);

    SECTION("frame rejection never exposes staging bytes") {
        capture.Finish(false);
        capture.Prepare(*frontend, source, reader);
        CHECK(polls == 0);
        const auto image = capture.Read(requested.Value(), source);
        REQUIRE(image);
        CHECK(image->phase == EditorViewportImagePhase::Failed);
        CHECK_FALSE(image->artifact);
    }
    SECTION("frame acceptance publishes detached bytes once the backend approves them") {
        capture.Finish(true);
        capture.Prepare(*frontend, source, reader);
        CHECK(polls == 1);
        const auto image = capture.Read(requested.Value(), source);
        REQUIRE(image);
        REQUIRE(image->phase == EditorViewportImagePhase::Ready);
        REQUIRE(image->artifact);
        CHECK(image->artifact->rgba.size() == 8);
        CHECK(image->artifact->rgba.front() == std::byte{0x42});
        CHECK(capture.Cancel(requested.Value()));
        CHECK(image->publication.expired());
    }
    capture.Shutdown(*frontend);
    resources.Shutdown();
}

TEST_CASE("Color viewport publishes its encoded image while retaining distinct scene and display targets",
          "[unit][headless][renderer][resource][color]") {
    auto frontend = CreateFrontend();
    ImageResolverFixture imageResolver;
    auto resources = CreateResources(*frontend);
    REQUIRE(resources.EnableColorPipeline().HasValue());
    SettleResources(resources, *frontend, {}, {128, 128});
    REQUIRE(resources.ColorTexture().IsValid());
    REQUIRE(resources.DisplayTexture().IsValid());
    REQUIRE(resources.PresentationTexture().IsValid());
    CHECK(resources.ColorTexture() != resources.DisplayTexture());
    CHECK(resources.ColorTexture() != resources.PresentationTexture());
    CHECK(resources.DisplayTexture() != resources.PresentationTexture());
    CHECK_FALSE(resources.TemporalImages());
    CHECK(imageResolver.lastView == resources.PresentationTextureView());
    CHECK(resources.ImageIdentity() == resources.PresentationTextureView().slot);

    const auto previousScene = resources.ColorTexture();
    const auto previousDisplay = resources.DisplayTexture();
    const auto previousPresentation = resources.PresentationTexture();
    const auto previousImage = resources.ImageIdentity();
    imageResolver.reject = true;
    bool failed = false;
    for (int step = 0; step < MaxResourcePasses && !failed; ++step) {
        const auto replacement = resources.Prepare({}, {256, 256});
        failed = replacement.HasError();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
    }
    REQUIRE(failed);
    CHECK(resources.IsReady());
    CHECK((resources.AllocatedExtent() == EditorViewportExtent{128, 128}));
    CHECK(resources.ColorTexture() == previousScene);
    CHECK(resources.DisplayTexture() == previousDisplay);
    CHECK(resources.PresentationTexture() == previousPresentation);
    CHECK(resources.ImageIdentity() == previousImage);
    imageResolver.reject = false;
    SettleResources(resources, *frontend, {}, {256, 256});
    CHECK(resources.ColorTexture() != previousScene);
    CHECK(resources.DisplayTexture() != previousDisplay);
    CHECK(resources.PresentationTexture() != previousPresentation);
    CHECK(imageResolver.lastView == resources.PresentationTextureView());
    resources.Shutdown();
}

TEST_CASE("Temporal viewport publishes and retires an atomic guide and history generation", "[unit][headless][renderer][resource][color]") {
    const bool upscale = GENERATE(false, true);
    CAPTURE(upscale);
    const auto frontend = CreateTemporalResourceFrontend();
    ImageResolverFixture imageResolver;
    auto resources = CreateResources(*frontend);
    using enum EditorViewportResources::ColorReconstruction;
    REQUIRE(resources.EnableColorPipeline(upscale ? TemporalUpscale : Temporal).HasValue());
    REQUIRE(resources.Prepare({}, {128, 128}).HasValue());
    CHECK_FALSE(resources.TemporalImages());
    SettleResources(resources, *frontend, {}, {128, 128});
    const auto first = resources.TemporalImages();
    REQUIRE(first);
    CHECK(resources.SceneColorDescriptor().extent == (upscale ? FramebufferExtent{86, 86} : FramebufferExtent{128, 128}));
    CHECK(resources.SpatialColorDescriptor().extent == FramebufferExtent{128, 128});
    const auto firstDisplayDepth = resources.DisplayDepthImages();
    if (upscale) {
        REQUIRE(firstDisplayDepth);
        CHECK(firstDisplayDepth->depth.texture != resources.DepthTexture());
        CHECK(frontend->ResourceState(firstDisplayDepth->depth.texture).Value() == RenderResourceState::Ready);
        CHECK(frontend->ResourceState(resources.SpatialTexture()).Value() == RenderResourceState::Ready);
    } else {
        CHECK_FALSE(firstDisplayDepth);
    }
    const std::array images{first->linearDepth,      first->motion,           first->history[0].color,
                            first->history[0].depth, first->history[1].color, first->history[1].depth};
    for (const auto &image : images) {
        const auto texture = frontend->ResourceState(image.texture);
        const auto view = frontend->ResourceState(image.view);
        REQUIRE(texture.HasValue());
        REQUIRE(view.HasValue());
        CHECK(texture.Value() == RenderResourceState::Ready);
        CHECK(view.Value() == RenderResourceState::Ready);
        CHECK(std::ranges::count(images, image.texture, &NativeReconstructionSceneImage::texture) == 1);
    }

    imageResolver.reject = true;
    bool rejected = false;
    for (int step = 0; step < MaxResourcePasses; ++step) {
        rejected = resources.Prepare({}, {256, 256}).HasError();
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        if (rejected)
            break;
    }
    REQUIRE(rejected);
    REQUIRE(resources.TemporalImages());
    CHECK(resources.TemporalImages()->motion.view == first->motion.view);
    if (upscale)
        CHECK(resources.DisplayDepthImages()->depth.view == firstDisplayDepth->depth.view);

    imageResolver.reject = false;
    SettleResources(resources, *frontend, {}, {256, 256});
    const auto second = resources.TemporalImages();
    REQUIRE(second);
    CHECK(second->motion.view != first->motion.view);
    if (upscale) {
        CHECK(resources.SceneExtent() == FramebufferExtent{171, 171});
        CHECK(frontend->ResourceState(firstDisplayDepth->depth.texture).HasError());
    }
    for (const auto &image : images) {
        CHECK(frontend->ResourceState(image.texture).HasError());
        CHECK(frontend->ResourceState(image.view).HasError());
    }
    resources.Shutdown();
    CHECK_FALSE(resources.TemporalImages());
    CHECK(frontend->ResourceState(second->history[0].color.texture).HasError());
    Render::Test::RequireDrainedOwner(*frontend);
}

TEST_CASE("Color activation cannot reinterpret already published legacy viewport storage", "[unit][headless][renderer][resource][color]") {
    auto frontend = CreateFrontend();
    ImageResolverFixture imageResolver;
    auto resources = CreateResources(*frontend);
    SettleResources(resources, *frontend, {}, {128, 128});
    const auto color = resources.ColorTexture();
    const auto view = resources.ColorTextureView();
    REQUIRE(resources.EnableColorPipeline().HasError());
    CHECK(resources.IsReady());
    CHECK(resources.ColorTexture() == color);
    CHECK(resources.PresentationTextureView() == view);
    CHECK_FALSE(resources.DisplayTexture().IsValid());
    resources.Shutdown();
}

TEST_CASE("Viewport light admission defers newer draw bounds until their exact resident mesh generation is ready",
          "[unit][headless][renderer][resource][lighting]") {
    auto frontend = CreateFrontend();
    ImageResolverFixture imageResolver;
    auto resources = CreateResources(*frontend);
    MeshSceneFixture fixture;
    auto scene = fixture.Scene(1);
    std::array<RenderStaticMeshInstance, 1> instances{{{.mesh = {{41}, 1}}}};
    REQUIRE(resources.Prepare(scene, {128, 128}).HasValue());
    REQUIRE(resources.FindMesh(41));
    CHECK(resources.FindMesh(41)->sourceGeneration == 1);
    CHECK_FALSE(resources.FindMesh(41)->ready);
    const auto queued = ValidateEditorViewportMeshReadiness(resources, instances);
    REQUIRE(queued.HasError());
    CHECK(queued.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
    SettleResources(resources, *frontend, scene, {128, 128});
    REQUIRE(ValidateEditorViewportMeshReadiness(resources, {}).HasValue());
    REQUIRE(ValidateEditorViewportMeshReadiness(resources, instances).HasValue());
    CHECK(resources.FindMesh(41)->ready);
    instances[0].mesh.generation = 2;
    auto pending = ValidateEditorViewportMeshReadiness(resources, instances);
    REQUIRE(pending.HasError());
    CHECK(pending.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
    CHECK_FALSE(pending.ErrorValue().message.empty());
    scene = fixture.Scene(2);
    REQUIRE(resources.Prepare(scene, {128, 128}).HasValue());
    REQUIRE(resources.FindMesh(41));
    CHECK(resources.FindMesh(41)->sourceGeneration == 1);
    CHECK(ValidateEditorViewportMeshReadiness(resources, instances).HasError());
    for (int step = 0; step < MaxResourcePasses && ValidateEditorViewportMeshReadiness(resources, instances).HasError(); ++step) {
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(resources.Prepare(scene, {128, 128}).HasValue());
    }
    REQUIRE(ValidateEditorViewportMeshReadiness(resources, instances).HasValue());
    CHECK(resources.FindMesh(41)->sourceGeneration == 2);
    instances[0].mesh.id = {99};
    pending = ValidateEditorViewportMeshReadiness(resources, instances);
    REQUIRE(pending.HasError());
    CHECK(pending.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
    resources.Shutdown();
}

TEST_CASE("Cold mesh preparation admits exact ready buffers and mesh before graph imports", "[unit][headless][renderer][resource]") {
    auto frontend = CreateFrontend();
    SECTION("default canonical drain limits") {
        REQUIRE(frontend->UploadSnapshot().acceptingRequests);
    }
    SECTION("one request per canonical drain") {
        frontend = CreateFrontend({.maximumRequestsPerDrain = 1});
    }
    auto resources = CreateResources(*frontend);
    MeshSceneFixture fixture;
    const auto scene = fixture.Scene(1);
    REQUIRE(resources.PrepareMeshesCold(scene.meshResources).HasValue());
    const auto mesh = resources.FindMesh(41);
    REQUIRE(mesh);
    REQUIRE(mesh->mesh.IsValid());
    const auto meshState = frontend->ResourceState(mesh->mesh);
    const auto vertexState = frontend->ResourceState(mesh->descriptor.vertexBuffer);
    const auto indexState = frontend->ResourceState(mesh->descriptor.indexBuffer);
    REQUIRE(meshState.HasValue());
    REQUIRE(vertexState.HasValue());
    REQUIRE(indexState.HasValue());
    CHECK(meshState.Value() == RenderResourceState::Ready);
    CHECK(vertexState.Value() == RenderResourceState::Ready);
    CHECK(indexState.Value() == RenderResourceState::Ready);
    CHECK(mesh->descriptor.vertexCount == fixture.vertices.size());
    CHECK(mesh->descriptor.indexCount == fixture.indices.size());
    auto created = RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 2, .maxUsages = 2, .maxDependencies = 1});
    REQUIRE(created.HasValue());
    auto builder = std::move(created).Value();
    const auto vertices = builder.ImportBuffer(mesh->descriptor.vertexBuffer, RenderGraphResourceClass::Persistent);
    const auto indices = builder.ImportBuffer(mesh->descriptor.indexBuffer, RenderGraphResourceClass::Persistent);
    const auto pass = builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics);
    REQUIRE(vertices.HasValue());
    REQUIRE(indices.HasValue());
    REQUIRE(pass.HasValue());
    const auto vertexUse = builder.AddUsage({pass.Value(), vertices.Value(), RenderGraphAccess::Read, RenderGraphUsageKind::VertexBuffer});
    const auto indexUse = builder.AddUsage({pass.Value(), indices.Value(), RenderGraphAccess::Read, RenderGraphUsageKind::IndexBuffer});
    REQUIRE(vertexUse.HasValue());
    REQUIRE(indexUse.HasValue());
    REQUIRE(builder.Finalize().HasValue());
    const auto replacementScene = fixture.Scene(2);
    REQUIRE(resources.PrepareMeshesCold(replacementScene.meshResources).HasValue());
    const auto replacement = resources.FindMesh(41);
    REQUIRE(replacement);
    CHECK(replacement->mesh != mesh->mesh);
    CHECK(replacement->descriptor.vertexBuffer != mesh->descriptor.vertexBuffer);
    resources.Shutdown();
}

TEST_CASE("Cold mesh preparation preserves canonical partial rollback and shutdown safety", "[unit][headless][renderer][resource]") {
    auto frontend = CreateFrontend({.maximumRequestsPerDrain = 1, .maximumPendingRequests = 1});
    auto resources = CreateResources(*frontend);
    MeshSceneFixture fixture;
    const auto scene = fixture.Scene(1);
    const auto prepared = resources.PrepareMeshesCold(scene.meshResources);
    REQUIRE(prepared.HasError());
    CHECK(prepared.ErrorValue().code.Value() == FrontendErrors::ResourceUploadCapacityExceeded.code.Value());
    CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    CHECK(frontend->UploadSnapshot().cancelledRequestCount == 1);
    resources.Shutdown();
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    CHECK(frontend->UploadSnapshot().cancelledRequestCount == 1);
}

TEST_CASE("Headless viewport resources replace resize generations atomically", "[unit][headless][renderer][resource]") {
    ImageResolverFixture imageResolver;
    std::unique_ptr<RenderFrontend> frontend = CreateFrontend();
    EditorViewportResources resources = CreateResources(*frontend);
    const RenderSceneView scene{};

    SettleResources(resources, *frontend, scene, {320, 180});
    REQUIRE(resources.IsReady());
    REQUIRE(resources.AllocatedExtent() == EditorViewportExtent{320, 180});
    const RenderTargetHandle original = resources.Target();
    const std::uintptr_t originalImage = resources.ImageIdentity();

    const auto pendingResize = resources.Prepare(scene, {640, 360});
    REQUIRE(pendingResize.HasValue());
    REQUIRE(pendingResize.Value() == std::optional<RenderTargetHandle>{original});
    REQUIRE(resources.Target() == original);
    REQUIRE(resources.ImageIdentity() == originalImage);
    REQUIRE(frontend->ProcessResourceRequests().HasValue());

    SettleResources(resources, *frontend, scene, {640, 360});
    REQUIRE(resources.IsReady());
    REQUIRE(resources.AllocatedExtent() == EditorViewportExtent{640, 360});
    REQUIRE(resources.Target() != original);
    const auto retired = frontend->ResourceState(original);
    REQUIRE(retired.HasError());
    REQUIRE(retired.ErrorValue().code.Value() == "render.frontend.resource.stale");

    const RenderTargetHandle replacement = resources.Target();
    const auto steady = resources.Prepare(scene, {640, 360});
    REQUIRE(steady.HasValue());
    REQUIRE(steady.Value() == std::optional<RenderTargetHandle>{replacement});
    const auto processed = frontend->ProcessResourceRequests();
    REQUIRE(processed.HasValue());
    REQUIRE(processed.Value() == 0);
    REQUIRE(resources.Target() == replacement);
}

TEST_CASE("Headless viewport resources preserve active meshes until replacements are ready", "[unit][headless][renderer][resource]") {
    ImageResolverFixture imageResolver;
    std::unique_ptr<RenderFrontend> frontend = CreateFrontend();
    EditorViewportResources resources = CreateResources(*frontend);
    MeshSceneFixture fixture;

    RenderSceneView scene = fixture.Scene(1);
    SettleResources(resources, *frontend, scene, {320, 180});
    const auto original = resources.FindMesh(41);
    REQUIRE(original.has_value());

    scene = fixture.Scene(2);
    REQUIRE(resources.Prepare(scene, {320, 180}).HasValue());
    const auto firstProcessed = frontend->ProcessResourceRequests();
    REQUIRE(firstProcessed.HasValue());
    REQUIRE(firstProcessed.Value() > 0);
    const auto firstPending = resources.FindMesh(41);
    REQUIRE(firstPending.has_value());
    REQUIRE(firstPending->mesh == original->mesh);
    REQUIRE(resources.Prepare(scene, {320, 180}).HasValue());
    const auto secondProcessed = frontend->ProcessResourceRequests();
    REQUIRE(secondProcessed.HasValue());
    REQUIRE(secondProcessed.Value() > 0);
    const auto secondPending = resources.FindMesh(41);
    REQUIRE(secondPending.has_value());
    REQUIRE(secondPending->mesh == original->mesh);
    REQUIRE(resources.Prepare(scene, {320, 180}).HasValue());

    const auto replacement = resources.FindMesh(41);
    REQUIRE(replacement.has_value());
    REQUIRE(replacement->mesh != original->mesh);
    REQUIRE(frontend->ResourceState(original->mesh).HasError());

    REQUIRE(resources.Prepare(RenderSceneView{}, {320, 180}).HasValue());
    REQUIRE_FALSE(resources.FindMesh(41).has_value());
    REQUIRE(frontend->ResourceState(replacement->mesh).HasError());
}

TEST_CASE("Headless viewport resources recover image resolution and shut down repeatedly", "[unit][headless][renderer][resource]") {
    ImageResolverFixture imageResolver;
    imageResolver.reject = true;
    std::unique_ptr<RenderFrontend> frontend = CreateFrontend();
    EditorViewportResources resources = CreateResources(*frontend);
    const RenderSceneView scene{};

    std::optional<Error> resolutionError;
    for (int step = 0; step < MaxResourcePasses && !resolutionError.has_value(); ++step) {
        const auto prepared = resources.Prepare(scene, {320, 180});
        if (prepared.HasError()) {
            resolutionError = prepared.ErrorValue();
            break;
        }
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
    }
    REQUIRE(resolutionError.has_value());
    REQUIRE(resolutionError->code.Value() == RendererErrors::ViewportGeometryCreationFailed.code.Value());
    REQUIRE(imageResolver.lastView.IsValid());
    REQUIRE_FALSE(resources.Target().IsValid());

    imageResolver.reject = false;
    REQUIRE(resources.Prepare(scene, {320, 180}).HasValue());
    REQUIRE(resources.IsReady());
    const RenderTargetHandle target = resources.Target();
    const RenderTargetHandle shadowTarget = resources.ShadowTarget();

    resources.Shutdown();
    resources.Shutdown();
    REQUIRE(frontend->ResourceState(target).HasError());
    REQUIRE(frontend->ResourceState(shadowTarget).HasError());
    REQUIRE_FALSE(resources.IsReady());
}

TEST_CASE("Directional shadow atlas quality publication retains the complete previous generation until ready",
          "[unit][headless][renderer][resource][shadow]") {
    ImageResolverFixture resolver;
    auto frontend = CreateFrontend();
    auto resources = CreateResources(*frontend);
    const RenderSceneView scene;
    SettleResources(resources, *frontend, scene, {64, 64});
    const auto previousProfile = resources.ActiveDirectionalShadowProfile();
    const auto previousTarget = resources.ShadowTarget();
    const auto previousView = resources.ShadowTextureView();
    const auto previousGeneration = resources.ShadowGeneration();
    const auto replacement = MakeDirectionalShadowProfile(DirectionalShadowQuality::Low);
    REQUIRE(resources.RequestDirectionalShadowProfile(replacement).HasValue());
    REQUIRE(resources.Prepare(scene, {64, 64}).HasValue());
    CHECK(resources.ActiveDirectionalShadowProfile() == previousProfile);
    CHECK(resources.ShadowTarget() == previousTarget);
    CHECK(resources.ShadowTextureView() == previousView);
    REQUIRE(frontend->ResourceState(previousTarget).HasValue());
    for (int step = 0; step < MaxResourcePasses && resources.ActiveDirectionalShadowProfile() != replacement; ++step) {
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(resources.Prepare(scene, {64, 64}).HasValue());
    }
    CHECK(resources.ActiveDirectionalShadowProfile() == replacement);
    CHECK(resources.ShadowTarget() != previousTarget);
    CHECK(resources.ShadowTextureView() != previousView);
    CHECK(resources.ShadowGeneration() == previousGeneration + 1);
    const auto extent = ComputeDirectionalShadowAtlasExtent(replacement);
    REQUIRE(extent.HasValue());
    CHECK(resources.ShadowExtent() == EditorViewportExtent{extent.Value().width, extent.Value().height});
    CHECK(frontend->ResourceState(previousTarget).HasError());
    resources.Shutdown();
}

TEST_CASE("Directional shadow atlas rejects malformed quality without replacing its last ready binding",
          "[unit][headless][renderer][resource][shadow]") {
    ImageResolverFixture resolver;
    auto frontend = CreateFrontend();
    auto resources = CreateResources(*frontend);
    const RenderSceneView scene;
    SettleResources(resources, *frontend, scene, {64, 64});
    const auto target = resources.ShadowTarget();
    const auto active = resources.ActiveDirectionalShadowProfile();
    auto invalid = active;
    invalid.cascadeCount = MaximumDirectionalShadowCascades + 1;
    CHECK(resources.RequestDirectionalShadowProfile(invalid).HasError());
    REQUIRE(resources.Prepare(scene, {64, 64}).HasValue());
    CHECK(resources.ShadowTarget() == target);
    CHECK(resources.ActiveDirectionalShadowProfile() == active);
    resources.Shutdown();
}
