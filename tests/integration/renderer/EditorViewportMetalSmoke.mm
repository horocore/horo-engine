#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Scene/PrimitiveMesh.h"
#include "editor/renderer/metal/EditorViewportRendererMetal.h"
#include "editor/renderer/metal/SdlMetalPresentationPort.h"
#include "renderer/RenderBackendDrainTestSupport.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Render;

    void Check(const bool condition) {
        REQUIRE((condition));
    }

    void AwaitPresentation(RenderFrontend &frontend, FramebufferExtent extent) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        while (std::chrono::steady_clock::now() < deadline) {
            SDL_PumpEvents();
            const auto ready = frontend.PollPresentationAvailability(extent);
            if (ready.HasValue() && ready.Value())
                return;
            if (ready.HasError())
                Check(ready.ErrorValue().code.Value() == RenderPresentationErrors::DrawableUnavailable.code.Value());
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
        FAIL("The actual Metal presentation did not become ready within five seconds");
    }

    RenderTargetHandle PrepareViewportTarget(EditorViewportRendererMetal &viewport, RenderFrontend &frontend,
                                             const EditorViewportSceneView &scene, const EditorViewportExtent extent) {
        const RenderSceneView renderScene{ToRenderCamera(scene.camera), scene.meshResources, scene.instances, scene.lights};
        for (std::size_t attempt = 0; attempt < 4; ++attempt) {
            viewport.RequestExtent(extent);
            auto prepared = viewport.PrepareResources(frontend, renderScene);
            if (prepared.HasError()) {
                std::fprintf(stderr, "Metal viewport resource preparation failed: %s\n", prepared.ErrorValue().message.c_str());
            }
            Check(prepared.HasValue());
            const auto processed = frontend.ProcessResourceRequests();
            if (processed.HasError()) {
                std::fprintf(stderr, "Metal viewport resource processing failed: %s\n", processed.ErrorValue().message.c_str());
            }
            Check(processed.HasValue());
            if (prepared.Value().has_value())
                return *prepared.Value();
        }
        Check(false);
        return {};
    }

    void CheckSeamlessResourceTransition(EditorViewportRendererMetal &viewport, RenderFrontend &frontend,
                                         const EditorViewportSceneView &scene, const RenderTargetHandle activeTarget,
                                         const EditorViewportTextureView activeTexture) {
        constexpr EditorViewportExtent activeExtent{128, 128};
        constexpr EditorViewportExtent resizedExtent{96, 64};
        std::vector<EditorViewportMeshResourceView> replacementMeshes{scene.meshResources.begin(), scene.meshResources.end()};
        replacementMeshes.front().handle.generation = 2;
        std::vector<EditorViewportInstance> replacementInstances{scene.instances.begin(), scene.instances.end()};
        replacementInstances.front().mesh.generation = 2;
        const EditorViewportSceneView replacementScene{scene.camera, replacementMeshes, replacementInstances, scene.lights};
        const RenderSceneView renderScene{ToRenderCamera(replacementScene.camera), replacementScene.meshResources,
                                          replacementScene.instances, replacementScene.lights};

        RenderTargetHandle resizedTarget;
        RenderTargetHandle currentTarget = activeTarget;
        viewport.RequestExtent(resizedExtent);
        for (std::size_t attempt = 0; attempt < 6 && !resizedTarget.IsValid(); ++attempt) {
            auto prepared = viewport.PrepareResources(frontend, renderScene);
            Check(prepared.HasValue() && prepared.Value().has_value());
            currentTarget = *prepared.Value();
            Check(frontend.ProcessResourceRequests().HasValue());

            // Match the application frame order: prepare the preceding request,
            // then let the panel publish this frame's request before pass execution.
            viewport.RequestExtent(resizedExtent);
            const EditorViewportExtent passExtent = viewport.RequestedExtent();
            Check(passExtent.IsValid());

            AwaitPresentation(frontend, {256, 256});
            auto begun = frontend.BeginFrame(FrameDescriptor{.frameNumber = 2 + attempt, .outputExtent = {256, 256}});
            Check(begun.HasValue());
            RenderFrameScope frame = std::move(begun).Value();
            const std::array passes{RenderPassDescriptor{
                .id = RenderPassId{1},
                .kind = RenderPassKind::Graphics,
                .staticMesh = StaticMeshPassDescriptor{.target = currentTarget,
                                                       .extent = {passExtent.width, passExtent.height},
                                                       .scene = renderScene},
            }};
            Check(frame.Execute(passes).HasValue());
            Check(frame.Present().HasValue());

            if (currentTarget != activeTarget)
                resizedTarget = currentTarget;
        }
        Check(resizedTarget.IsValid());
        Check(viewport.RequestedExtent() == resizedExtent);
        Check(viewport.TextureView().textureId != activeTexture.textureId);
    }
}  // namespace

TEST_CASE("Editor Viewport Metal Smoke", "[integration][renderer][gpu]") {
    Check(SDL_Init(SDL_INIT_VIDEO));
    Check(SDL_SetHint(SDL_HINT_VIDEO_METAL_AUTO_RESIZE_DRAWABLE, "0"));
    SDL_Window *window = SDL_CreateWindow("Horo Metal viewport smoke", 256, 256, SDL_WINDOW_METAL | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    Check(window != nullptr);

    SdlMetalPresentationPort presentationPort{*window};
    MetalEditorGraphicsBridge graphicsBridge;
    RenderBackendRegistry registry;
    Check(RegisterMetalRenderBackend(registry, presentationPort, graphicsBridge).HasValue());
    Check(registry.Seal().HasValue());
    auto frontendResult = RenderFrontend::Create(registry, RenderBackendId{"metal"},
                                                 RenderBackendConfig{.requirePresentation = true,
                                                                     .enableValidation = true,
                                                                     .maxFramesInFlight = 2,
                                                                     .presentMode = PresentMode::Immediate});
    if (frontendResult.HasError()) {
        std::fprintf(stderr, "Metal frontend creation failed: %s\n", frontendResult.ErrorValue().message.c_str());
    }
    Check(frontendResult.HasValue());
    std::unique_ptr<RenderFrontend> frontend = std::move(frontendResult).Value();
    RenderTextureDescriptor shadowDepth{.extent = {2048, 2048},
                                        .format = RenderTextureFormat::Depth32Float,
                                        .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
    REQUIRE(frontend->Capabilities().support.Supports(shadowDepth));
    shadowDepth.usage = shadowDepth.usage | RenderTextureUsage::Storage;
    CHECK_FALSE(frontend->Capabilities().support.Supports(shadowDepth));

    EditorViewportRendererMetal viewport{*frontend, graphicsBridge};
    Check(viewport.Initialize().HasValue());
    Runtime::PrimitiveMeshCache meshCache;
    constexpr std::array primitiveTypes{Runtime::PrimitiveMeshType::Box,     Runtime::PrimitiveMeshType::Sphere,
                                        Runtime::PrimitiveMeshType::Capsule, Runtime::PrimitiveMeshType::Cylinder,
                                        Runtime::PrimitiveMeshType::Cone,    Runtime::PrimitiveMeshType::Plane,
                                        Runtime::PrimitiveMeshType::Quad};
    std::vector<Runtime::PrimitiveMeshLease> meshLeases;
    std::vector<EditorViewportMeshResourceView> meshResources;
    std::vector<EditorViewportInstance> viewportInstances;
    for (std::size_t index = 0; index < primitiveTypes.size(); ++index) {
        auto acquiredMesh = meshCache.Acquire(Runtime::PrimitiveMeshDescriptor::Defaults(primitiveTypes[index]));
        Check(acquiredMesh.HasValue());
        Runtime::PrimitiveMeshLease meshLease = std::move(acquiredMesh).Value();
        const Render::MeshData &mesh = meshLease.Data();
        const Render::RenderMeshSourceHandle meshHandle{meshLease.Id(), 1};
        meshResources.push_back({meshHandle, mesh.vertices, mesh.indices, mesh.localBounds});
        constexpr std::array positions{Math::Vec2{0, 0},         Math::Vec2{-1.0F, 0.7F}, Math::Vec2{0, 0.9F},    Math::Vec2{1.0F, 0.7F},
                                       Math::Vec2{-1.0F, -0.7F}, Math::Vec2{0, -0.9F},    Math::Vec2{1.0F, -0.7F}};
        const float scale = primitiveTypes[index] == Runtime::PrimitiveMeshType::Plane ? 0.08F : index == 0 ? 0.65F : 0.45F;
        viewportInstances.push_back(
            {meshHandle,
             Math::Transform{.translation = {positions[index].x, positions[index].y, 0}, .scale = {scale, scale, scale}}.ToMatrix(),
             mesh.localBounds,
             Render::CoreDefaultMaterial,
             {.tint = {0.12F, 0.72F, 1.0F}, .tintStrength = index == 0 ? 0.65F : 0.0F}});
        meshLeases.push_back(std::move(meshLease));
    }
    const std::array lights{
        Render::RenderLight{
            .kind = Render::RenderLightKind::Directional,
            .direction = Math::Normalize(Math::Vec3{0.5F, -1.0F, -0.5F}),
            .color = {1.0F, 0.95F, 0.88F},
            .intensity = 2.0F,
        },
    };
    const EditorViewportSceneView viewportScene{.camera = {},
                                                .meshResources = meshResources,
                                                .instances = viewportInstances,
                                                .lights = lights};
    Check(frontend->AttachStaticMeshPassExecutor(viewport).HasValue());
    const RenderTargetHandle viewportTarget = PrepareViewportTarget(viewport, *frontend, viewportScene, {128, 128});
    Check(frontend->Resize(FramebufferExtent{256, 256}).HasValue());
    AwaitPresentation(*frontend, {256, 256});
    auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {256, 256}});
    Check(begun.HasValue());
    RenderFrameScope frame = std::move(begun).Value();

    viewport.RequestExtent(EditorViewportExtent{128, 128});

    const std::array passes{
        RenderPassDescriptor{
            .id = RenderPassId{1},
            .kind = RenderPassKind::Graphics,
            .staticMesh =
                StaticMeshPassDescriptor{
                    .target = viewportTarget,
                    .extent = {128, 128},
                    .scene = RenderSceneView{ToRenderCamera(viewportScene.camera), viewportScene.meshResources, viewportScene.instances,
                                             viewportScene.lights},
                },
        },
        RenderPassDescriptor{
            .id = RenderPassId{2},
            .kind = RenderPassKind::Graphics,
            .primaryOutput =
                PrimaryOutputAttachment{
                    .loadOperation = AttachmentLoadOperation::Clear,
                    .storeOperation = AttachmentStoreOperation::Store,
                    .clearColor = ClearColor{0.02F, 0.03F, 0.05F, 1.0F},
                },
        },
    };
    Check(frame.Execute(passes).HasValue());
    Check(viewport.IsReady());
    const EditorViewportTextureView textureView = viewport.TextureView();
    Check(textureView.IsValid());
    Check(textureView.u0 == 0.0F && textureView.v0 == 0.0F);
    Check(textureView.u1 == 1.0F && textureView.v1 == 1.0F);
    Check(frame.Present().HasValue());
    graphicsBridge.WaitUntilIdle();

    id<MTLTexture> texture = (__bridge id<MTLTexture>)(reinterpret_cast<void *>(textureView.textureId));
    Check(texture != nil);
    Check(texture.width == 128 && texture.height == 128);
    const std::size_t bytesPerRow = texture.width * 4U;
    const std::size_t byteCount = bytesPerRow * texture.height;
    id<MTLDevice> device = (__bridge id<MTLDevice>)graphicsBridge.Device();
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)graphicsBridge.CommandQueue();
    id<MTLBuffer> readback = [device newBufferWithLength:byteCount options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> readbackCommands = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [readbackCommands blitCommandEncoder];
    [blit copyFromTexture:texture
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                        toBuffer:readback
               destinationOffset:0
          destinationBytesPerRow:bytesPerRow
        destinationBytesPerImage:byteCount];
    [blit endEncoding];
    [readbackCommands commit];
    [readbackCommands waitUntilCompleted];
    std::vector<std::uint8_t> rgba(byteCount);
    std::copy_n(static_cast<const std::uint8_t *>(readback.contents), byteCount, rgba.data());

    std::uint8_t minimum = 255;
    std::uint8_t maximum = 0;
    for (std::size_t index = 0; index < rgba.size(); index += 4) {
        minimum = std::min(minimum, std::min(rgba[index], std::min(rgba[index + 1], rgba[index + 2])));
        maximum = std::max(maximum, std::max(rgba[index], std::max(rgba[index + 1], rgba[index + 2])));
    }
    Check(maximum > minimum + 32);
    const std::size_t center = (static_cast<std::size_t>(texture.height / 2) * texture.width + texture.width / 2) * 4;
    Check(rgba[center] < 150 && rgba[center + 1] > 130 && rgba[center + 2] > 150);

    CheckSeamlessResourceTransition(viewport, *frontend, viewportScene, viewportTarget, textureView);
    frontend->DetachStaticMeshPassExecutor(viewport);
    Horo::Render::Test::RequireDrainedOwner(*frontend);
    viewport.Shutdown();
    frontend.reset();
    SDL_DestroyWindow(window);
    SDL_Quit();
}
