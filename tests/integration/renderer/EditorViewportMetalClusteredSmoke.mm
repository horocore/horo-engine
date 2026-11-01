#include "EditorClusteredLightingAssets.h"
#include "editor/renderer/metal/EditorViewportRendererMetal.h"
#include "editor/renderer/metal/MetalViewportResourceBridge.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "renderer/RenderBackendDrainTestSupport.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Render;

    /** @brief Preserves the original typed failure at frontend preparation and submission boundaries. */
    template <typename T> void RequireResultInfo(const Result<T> &result, const char *phase = "Metal frontend operation") {
        INFO(phase);
        INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Operation accepted."));
        REQUIRE(result.HasValue());
    }

    void RequireCompleted(id<MTLCommandBuffer> commands) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (commands.status != MTLCommandBufferStatusCompleted && commands.status != MTLCommandBufferStatusError &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        REQUIRE(commands.status == MTLCommandBufferStatusCompleted);
    }

    /** @brief Waits only for real SDL visibility, admitted output resize and drawable acquisition. */
    void AwaitPresentation(RenderFrontend &frontend, const FramebufferExtent extent) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            SDL_PumpEvents();
            const auto available = frontend.PollPresentationAvailability(extent);
            if (available.HasValue() && available.Value())
                return;
            if (available.HasError()) {
                INFO((available.ErrorValue().code.Value() + ": " + available.ErrorValue().message));
                REQUIRE(available.ErrorValue().code.Value() == RenderPresentationErrors::DrawableUnavailable.code.Value());
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        FAIL("The actual Metal presentation did not become ready within five seconds.");
    }

    /** @brief Prints real cleanup failure evidence without invoking Catch during exception unwinding. */
    template <typename T> bool CleanupAccepted(const Result<T> &result) noexcept {
        if (result.HasValue())
            return true;
        std::fprintf(stderr, "Metal frontend cleanup failed: %s: %s\n", result.ErrorValue().code.Value().c_str(),
                     result.ErrorValue().message.c_str());
        return false;
    }

    /** @brief Retains every native owner while polling the established finite test drain budget. */
    bool DrainFrontendDuringCleanup(RenderFrontend &frontend) noexcept {
        try {
            if (!CleanupAccepted(frontend.BeginShutdown()))
                return false;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            for (;;) {
                const auto progress = frontend.PollDrain();
                if (!CleanupAccepted(progress))
                    return false;
                if (progress.Value() == RenderBackendDrainProgress::Drained)
                    return true;
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::fputs("Metal frontend cleanup exceeded its five-second drain budget.\n", stderr);
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
        } catch (...) {
            std::fputs("Metal frontend cleanup threw while retaining native owners.\n", stderr);
            return false;
        }
    }

    struct ClusteredScene final {
        std::array<MeshVertex, 3> vertices{{{{-1, -1, 0}, {0, 0, 1}, {}}, {{1, -1, 0}, {0, 0, 1}, {}}, {{0, 1, 0}, {0, 0, 1}, {}}}};
        std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::array<RenderMeshResourceView, 1> meshes;
        std::array<RenderStaticMeshInstance, 1> instances;
        std::array<RenderLight, 32> lights;

        ClusteredScene() {
            const RenderMeshSourceHandle handle{{382}, 1};
            const Math::Aabb bounds{{-1, -1, -0.01F}, {1, 1, 0.01F}};
            meshes[0] = {handle, vertices, indices, bounds};
            instances[0] = {.mesh = handle, .localBounds = bounds};
            for (std::size_t index = 0; index < lights.size(); ++index)
                lights[index] = {.kind = RenderLightKind::Point,
                                 .position = {0, 0, 2},
                                 .intensity = 0.1F,
                                 .range = 1000,
                                 .stableIdentity = index + 1};
        }

        RenderSceneView View() const {
            return {.camera = {.position = {0, 0, 3}, .target = {0, 0, 0}},
                    .meshResources = meshes,
                    .instances = instances,
                    .lights = lights,
                    .lightSceneDomain = 382,
                    .lightSnapshotRevision = ComputeEditorViewportLightRevision(lights)};
        }
    };

    struct ClusteredFixture final {
        SDL_Window *window{};
        std::unique_ptr<SdlMetalPresentationPort> presentation;
        MetalEditorGraphicsBridge bridge;
        RenderBackendRegistry registry;
        std::unique_ptr<RenderFrontend> frontend;
        std::unique_ptr<EditorViewportRendererMetal> viewport;
        RenderViewId view;
        RenderTargetHandle target;
        __strong id<MTLCommandBuffer> lastCommands{nil};

        ~ClusteredFixture() {
            if (frontend) {
                if (viewport)
                    frontend->DetachStaticMeshPassExecutor(*viewport);
                if (!DrainFrontendDuringCleanup(*frontend))
                    std::terminate();
            }
            viewport.reset();
            frontend.reset();
            presentation.reset();
            if (window)
                SDL_DestroyWindow(window);
            SDL_Quit();
        }

        void Initialize() {
            REQUIRE(SDL_Init(SDL_INIT_VIDEO));
            REQUIRE(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
            REQUIRE_FALSE(SDL_GetHintBoolean(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, true));
            window = SDL_CreateWindow("Horo configured clustered Metal smoke", 128, 128, SDL_WINDOW_METAL);
            REQUIRE(window);
            presentation = std::make_unique<SdlMetalPresentationPort>(*window);
            RequireResultInfo(RegisterMetalRenderBackend(registry, *presentation, bridge));
            RequireResultInfo(registry.Seal());
            auto created = RenderFrontend::Create(registry, RenderBackendId{"metal"},
                                                  {.requirePresentation = true,
                                                   .enableValidation = true,
                                                   .maxFramesInFlight = 2,
                                                   .presentMode = PresentMode::Immediate});
            RequireResultInfo(created, "Create Metal frontend with actual presentation");
            frontend = std::move(created).Value();
            CHECK_FALSE(frontend->Capabilities().supportsMaterialPassBatches);
            if (!frontend->Capabilities().supportsMaterialRasterGraph)
                SKIP("The actual Metal backend does not advertise its graph-material consumer; no clustered execution was qualified.");
            viewport = std::make_unique<EditorViewportRendererMetal>(*frontend, bridge);
            RequireResultInfo(viewport->Initialize());
            const auto issued = frontend->CreateRenderViewIdentity();
            RequireResultInfo(issued);
            view = issued.Value();
            RequireResultInfo(viewport->BindLightingViewIdentity(view));
            RequireResultInfo(frontend->AttachStaticMeshPassExecutor(*viewport));
        }

        void Prepare(const RenderSceneView &scene) {
            for (std::size_t attempt = 0; attempt < 8; ++attempt) {
                viewport->RequestExtent({128, 128});
                const auto prepared = viewport->PrepareResources(*frontend, scene);
                RequireResultInfo(prepared, "Prepare viewport resident resources");
                RequireResultInfo(frontend->ProcessResourceRequests());
                if (prepared.Value()) {
                    target = *prepared.Value();
                    return;
                }
            }
            FAIL("Actual clustered viewport resources did not become ready within their bounded cold preparation.");
        }

        void Render(const RenderSceneView &scene, const std::uint64_t number) {
            viewport->RequestExtent({128, 128});
            const StaticMeshPassDescriptor pass{.target = target, .extent = {128, 128}, .scene = scene};
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
            for (;;) {
                const auto prepared = viewport->PrepareLightingFrame(pass, view);
                if (prepared.HasValue())
                    break;
                INFO((prepared.ErrorValue().code.Value() + ": " + prepared.ErrorValue().message));
                REQUIRE(prepared.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
                REQUIRE(std::chrono::steady_clock::now() < deadline);
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            AwaitPresentation(*frontend, {128, 128});
            auto begun = frontend->BeginFrame({.frameNumber = number, .outputExtent = {128, 128}});
            RequireResultInfo(begun, "Begin actual frontend frame");
            auto frame = std::move(begun).Value();
            const std::array passes{RenderPassDescriptor{.id = {1}, .kind = RenderPassKind::Graphics, .staticMesh = pass},
                                    RenderPassDescriptor{.id = {2},
                                                         .kind = RenderPassKind::Graphics,
                                                         .primaryOutput = PrimaryOutputAttachment{}}};
            RequireResultInfo(viewport->ExecutePreparedFrame(frame, passes), "Execute clustered frontend graph");
            lastCommands = (__bridge id<MTLCommandBuffer>)bridge.CurrentCommandBuffer();
            RequireResultInfo(frame.Present(), "Submit and present actual frontend frame");
        }

        void Close() {
            INFO("Drain actual frontend before releasing the clustered viewport and presentation owners.");
            frontend->DetachStaticMeshPassExecutor(*viewport);
            Test::RequireDrainedOwner(*frontend);
            viewport->Shutdown();
            viewport.reset();
            frontend.reset();
        }
    };

    void CheckActualPixels(ClusteredFixture &fixture) {
        RequireCompleted(fixture.lastCommands);
        const auto resolved = MetalViewportResourceBridge::ResolveTexture(*fixture.frontend, fixture.viewport->TextureView().sampledView);
        RequireResultInfo(resolved);
        id<MTLTexture> texture = (__bridge id<MTLTexture>)resolved.Value();
        id<MTLDevice> device = (__bridge id<MTLDevice>)fixture.bridge.Device();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLBuffer> bytes = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
        REQUIRE(queue);
        REQUIRE(bytes);
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        [blit copyFromTexture:texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(64, 64, 0)
                          sourceSize:MTLSizeMake(1, 1, 1)
                            toBuffer:bytes
                   destinationOffset:0
              destinationBytesPerRow:256
            destinationBytesPerImage:256];
        [blit endEncoding];
        [commands commit];
        RequireCompleted(commands);
        const auto *pixel = static_cast<const std::uint8_t *>(bytes.contents);
        CHECK(pixel[0] > 20);
        CHECK(pixel[1] > 20);
        CHECK(pixel[2] > 20);
    }

    void CheckFamilyReplacement(ClusteredFixture &fixture, const EditorClusteredLightingAssets &assets) {
        const auto state = fixture.viewport->CaptureState();
        RequireResultInfo(state);
        auto invalid = assets.ProgramViews();
        invalid[0].id.value = 0;
        REQUIRE(fixture.viewport->Prepare(invalid, state.Value()).HasError());
        REQUIRE(fixture.viewport->CaptureState().Value() == state.Value());
        const auto views = assets.ProgramViews();
        auto candidate = fixture.viewport->Prepare(views, state.Value());
        RequireResultInfo(candidate);
        const auto adopted = fixture.viewport->Adopt(std::move(candidate).Value(), state.Value());
        RequireResultInfo(adopted);
        CHECK(adopted.Value().liveGeneration == state.Value().liveGeneration + 1);
    }

    void CheckProfileAdmission(ClusteredFixture &fixture, const EditorClusteredLightingAssets &assets) {
        const auto expected = fixture.viewport->CaptureState();
        RequireResultInfo(expected);
        const auto views = assets.ProgramViews();
        EditorViewportLightingProfile profile;
        profile.requested = LightPreparationMode::BaselineForwardCpu;
        const auto cpu = fixture.viewport->PrepareClusteredLighting(views, assets.kernel, profile);
        REQUIRE(cpu.HasError());
        CHECK(cpu.ErrorValue().code.Value() == LightCullingErrors::InvalidInput.code.Value());
        profile.requested = static_cast<LightPreparationMode>(255);
        const auto unknown = fixture.viewport->PrepareClusteredLighting(views, assets.kernel, profile);
        REQUIRE(unknown.HasError());
        CHECK(unknown.ErrorValue().code.Value() == LightCullingErrors::InvalidInput.code.Value());
        CHECK(fixture.viewport->CaptureState().Value() == expected.Value());
    }
}  // namespace

TEST_CASE("Configured cooked Metal viewport consumes real clustered storage through one frontend graph",
          "[integration][renderer][metal][lighting][gpu][configured-artifact]") {
    const char *root = std::getenv("HORO_CLUSTERED_LIGHTING_ASSET_ROOT");
    if (!root)
        SKIP("Supply the genuine complete approved-tool Metal clustered bundle; no synthetic shader artifact is accepted.");
    const auto assets = LoadPackagedMetalClusteredLighting(root);
    RequireResultInfo(assets);
    ClusteredFixture fixture;
    fixture.Initialize();
    CheckProfileAdmission(fixture, assets.Value());
    EditorViewportLightingProfile profile;
    profile.fallback = LightPreparationFallback::Forbidden;
    const auto views = assets.Value().ProgramViews();
    RequireResultInfo(fixture.viewport->PrepareClusteredLighting(views, assets.Value().kernel, profile));
    RequireResultInfo(fixture.viewport->RequestLightingProfile(profile, {LightPreparationMode::ClusteredForwardNative, 0}));
    fixture.viewport->RequestGrid({.visible = false});
    ClusteredScene scene;
    fixture.Prepare(scene.View());
    for (std::uint64_t frame = 1; frame <= 4; ++frame)
        fixture.Render(scene.View(), frame);
    const auto completed = fixture.viewport->LightingDiagnostics();
    CHECK(completed.sourceLightCount == 32);
    CHECK(completed.completedLightRevision == scene.View().lightSnapshotRevision);
    CHECK(completed.shadedReferences > 16);
    CHECK(completed.omittedReferences == 0);
    CheckFamilyReplacement(fixture, assets.Value());
    for (std::uint64_t frame = 5; frame <= 8; ++frame)
        fixture.Render(scene.View(), frame);
    CHECK(fixture.viewport->LightingDiagnostics().completedLightRevision == scene.View().lightSnapshotRevision);
    CheckActualPixels(fixture);
    fixture.Close();
}
