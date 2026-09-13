#include "EditorViewportRendererMetal.h"

#include "MetalViewportResourceBridge.h"
#include "MetalViewportShaderTypes.h"
#include "MetalViewportShaders.h"
#include "editor/renderer/EditorRenderMemoryScopes.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "editor/renderer/EditorViewportResources.h"
#include "editor/renderer/grid/EditorViewportGridGeometry.h"
#include "editor/screens/workspace/panels/viewport/visualizers/light/LightVisualizerGeometry.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        constexpr std::uint32_t maxViewportDimension = 8192;

        struct MetalViewportFrameResources {
            __strong id<MTLTexture> colorTexture{nil};
            __strong id<MTLTexture> depthTexture{nil};
            __strong id<MTLTexture> shadowTexture{nil};
        };

        [[nodiscard]] Error MakeViewportError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] std::string ErrorMessage(NSString *prefix, NSError *error) {
            std::string message{prefix.UTF8String};
            if (error != nil) {
                message += ": ";
                message += error.localizedDescription.UTF8String;
            }
            return message;
        }

    }  // namespace

    struct EditorViewportRendererMetal::Impl {
        Impl(Render::RenderFrontend &resourceFrontend, Render::MetalEditorGraphicsBridge &borrowedGraphicsBridge) noexcept
            : frontend(&resourceFrontend), graphicsBridge(&borrowedGraphicsBridge),
              resources(resourceFrontend, {.depthFormat = Render::RenderTextureFormat::Depth32Float,
                                           .depthAspect = Render::RenderTextureAspect::Depth,
                                           .resolveImage = &MetalViewportResourceBridge::EditorImageIdentity}) {}

        Render::RenderFrontend *frontend{nullptr};
        Render::MetalEditorGraphicsBridge *graphicsBridge{nullptr};
        EditorViewportResources resources;
        __strong id<MTLDevice> device{nil};
        __strong id<MTLLibrary> library{nil};
        __strong id<MTLRenderPipelineState> pipeline{nil};
        __strong id<MTLRenderPipelineState> gridPipeline{nil};
        __strong id<MTLRenderPipelineState> shadowPipeline{nil};
        __strong id<MTLDepthStencilState> depthState{nil};
        __strong id<MTLDepthStencilState> gridDepthState{nil};
        __strong id<MTLSamplerState> shadowSampler{nil};
        __strong id<MTLBuffer> gridVertexBuffer{nil};
        Render::RenderBufferHandle gridVertexBufferHandle;
        EditorViewportExtent requestedExtent{};
        EditorViewportGridOptions gridOptions{};
        EditorViewportLightVisualizerOptions lightVisualizerOptions{};
        bool initialized{false};

        [[nodiscard]] Result<void> CreateMainPipelines();
        [[nodiscard]] Result<void> CreateShadowPipeline();
        [[nodiscard]] Result<void> CreateSupportResources();
        [[nodiscard]] Result<void> ValidatePassRequest(const Render::StaticMeshPassDescriptor &descriptor,
                                                       EditorViewportExtent renderExtent) const;
        [[nodiscard]] Result<MetalViewportFrameResources> ResolveFrameResources() const;
        [[nodiscard]] Result<void> EncodeShadowPass(id<MTLCommandBuffer> commandBuffer, const Render::RenderSceneView &scene,
                                                    const EditorViewportDirectionalShadowView &shadow, id<MTLTexture> shadowTexture) const;
        [[nodiscard]] Result<void> EncodeViewportPass(id<MTLCommandBuffer> commandBuffer,
                                                      const Render::StaticMeshPassDescriptor &descriptor,
                                                      const std::optional<EditorViewportDirectionalShadowView> &shadow,
                                                      const MetalViewportFrameResources &frameResources, float aspect);
        [[nodiscard]] Result<void> DrawGrid(id<MTLRenderCommandEncoder> encoder, const Render::RenderCameraView &camera, float aspect,
                                            float viewportHeightPixels) const;
        [[nodiscard]] Result<void> DrawMeshes(id<MTLRenderCommandEncoder> encoder, const Render::RenderSceneView &scene,
                                              const std::optional<EditorViewportDirectionalShadowView> &shadow, float aspect) const;
        [[nodiscard]] Result<void> DrawLightVisualizer(id<MTLRenderCommandEncoder> encoder, const Render::RenderCameraView &camera,
                                                       float aspect);
        void UploadLighting(id<MTLRenderCommandEncoder> encoder, const Render::RenderSceneView &scene,
                            const std::optional<EditorViewportDirectionalShadowView> &shadow) const;
    };

    Result<void> EditorViewportRendererMetal::Impl::CreateMainPipelines() {
        NSError *error = nil;
        MTLRenderPipelineDescriptor *pipelineDescriptor = [MTLRenderPipelineDescriptor new];
        pipelineDescriptor.label = @"Horo Editor Viewport";
        pipelineDescriptor.vertexFunction = [library newFunctionWithName:@"viewport_vertex"];
        pipelineDescriptor.fragmentFunction = [library newFunctionWithName:@"viewport_fragment"];
        pipelineDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        pipelineDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDescriptor error:&error];
        if (pipeline == nil) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportMetalPipelineCreationFailed,
                                                           ErrorMessage(@"Metal viewport pipeline creation failed", error)));
        }

        MTLRenderPipelineDescriptor *gridDescriptor = [MTLRenderPipelineDescriptor new];
        gridDescriptor.label = @"Horo Editor Viewport Grid";
        gridDescriptor.vertexFunction = [library newFunctionWithName:@"viewport_grid_vertex"];
        gridDescriptor.fragmentFunction = [library newFunctionWithName:@"viewport_grid_fragment"];
        gridDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        gridDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        gridPipeline = [device newRenderPipelineStateWithDescriptor:gridDescriptor error:&error];
        if (gridPipeline == nil) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportMetalPipelineCreationFailed,
                                                           ErrorMessage(@"Metal viewport grid pipeline creation failed", error)));
        }
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererMetal::Impl::CreateShadowPipeline() {
        NSError *error = nil;
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.label = @"Horo Editor Directional Shadow";
        descriptor.vertexFunction = [library newFunctionWithName:@"viewport_shadow_vertex"];
        descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        shadowPipeline = [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
        if (shadowPipeline == nil) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportMetalPipelineCreationFailed,
                                                           ErrorMessage(@"Metal viewport shadow pipeline creation failed", error)));
        }
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererMetal::Impl::CreateSupportResources() {
        MTLDepthStencilDescriptor *depthDescriptor = [MTLDepthStencilDescriptor new];
        depthDescriptor.depthCompareFunction = MTLCompareFunctionLess;
        depthDescriptor.depthWriteEnabled = YES;
        depthState = [device newDepthStencilStateWithDescriptor:depthDescriptor];

        MTLDepthStencilDescriptor *gridDepthDescriptor = [MTLDepthStencilDescriptor new];
        gridDepthDescriptor.depthCompareFunction = MTLCompareFunctionLess;
        gridDepthDescriptor.depthWriteEnabled = NO;
        gridDepthState = [device newDepthStencilStateWithDescriptor:gridDepthDescriptor];
        constexpr std::size_t gridBufferSize = sizeof(Math::Vec3) * (ViewportGridGeometry::MaxRegularVertices + 4);
        const std::vector<std::byte> emptyGrid(gridBufferSize);
        auto gridBuffer = frontend->CreateBuffer(RenderMemoryScopes::ViewportResources,
                                                 {.byteSize = gridBufferSize,
                                                  .usage = Render::RenderBufferUsage::Vertex | Render::RenderBufferUsage::CopyDestination,
                                                  .access = Render::RenderBufferAccess::HostVisible},
                                                 std::span<const std::byte>{emptyGrid});
        if (gridBuffer.HasError())
            return Result<void>::Failure(gridBuffer.ErrorValue());
        gridVertexBufferHandle = gridBuffer.Value().handle;
        const auto processed = frontend->ProcessResourceRequests();
        const auto completed = frontend->ResourceOperationResult(gridBuffer.Value().operation);
        const auto nativeBuffer = MetalViewportResourceBridge::ResolveBuffer(*frontend, gridVertexBufferHandle);
        if (processed.HasError() || completed.HasError() || nativeBuffer.HasError()) {
            const Error error = processed.HasError()   ? processed.ErrorValue()
                                : completed.HasError() ? completed.ErrorValue()
                                                       : nativeBuffer.ErrorValue();
            static_cast<void>(frontend->ReleaseBuffer(gridVertexBufferHandle));
            gridVertexBufferHandle = {};
            return Result<void>::Failure(error);
        }
        gridVertexBuffer = (__bridge id<MTLBuffer>)nativeBuffer.Value();

        MTLSamplerDescriptor *samplerDescriptor = [MTLSamplerDescriptor new];
        samplerDescriptor.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDescriptor.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
        samplerDescriptor.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
        samplerDescriptor.borderColor = MTLSamplerBorderColorOpaqueWhite;
        shadowSampler = [device newSamplerStateWithDescriptor:samplerDescriptor];
        if (depthState == nil || gridDepthState == nil || gridVertexBuffer == nil || shadowSampler == nil) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportMetalResourceCreationFailed,
                                                           "Failed to create Metal viewport geometry or depth state."));
        }
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererMetal::Impl::ValidatePassRequest(const Render::StaticMeshPassDescriptor &descriptor,
                                                                        const EditorViewportExtent renderExtent) const {
        if (!descriptor.IsValid() || descriptor.extent.width != renderExtent.width || descriptor.extent.height != renderExtent.height) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportInvalidScene, "Editor viewport scene data is invalid."));
        }
        if (resources.Target().IsValid() && resources.Target() != descriptor.target) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportStaleTarget, "Viewport pass references a stale render target."));
        }
        const EditorViewportExtent allocatedExtent = resources.AllocatedExtent();
        if (renderExtent.width != allocatedExtent.width || renderExtent.height != allocatedExtent.height) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportStaleTarget, "Viewport target extent is not ready."));
        }
        return Result<void>::Success();
    }

    Result<MetalViewportFrameResources> EditorViewportRendererMetal::Impl::ResolveFrameResources() const {
        const auto viewportTarget = MetalViewportResourceBridge::ResolveRenderTarget(*frontend, resources.Target());
        if (viewportTarget.HasError())
            return Result<MetalViewportFrameResources>::Failure(viewportTarget.ErrorValue());
        const auto shadowTexture = MetalViewportResourceBridge::ResolveTexture(*frontend, resources.ShadowTextureView());
        if (shadowTexture.HasError())
            return Result<MetalViewportFrameResources>::Failure(shadowTexture.ErrorValue());
        return Result<MetalViewportFrameResources>::Success({
            .colorTexture = (__bridge id<MTLTexture>)viewportTarget.Value().colorTexture,
            .depthTexture = (__bridge id<MTLTexture>)viewportTarget.Value().depthTexture,
            .shadowTexture = (__bridge id<MTLTexture>)shadowTexture.Value(),
        });
    }

    Result<void> EditorViewportRendererMetal::Impl::EncodeShadowPass(id<MTLCommandBuffer> commandBuffer,
                                                                     const Render::RenderSceneView &scene,
                                                                     const EditorViewportDirectionalShadowView &shadow,
                                                                     id<MTLTexture> shadowTexture) const {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.depthAttachment.texture = shadowTexture;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionStore;
        pass.depthAttachment.clearDepth = 1.0;
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (encoder == nil) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportMetalEncoderCreationFailed,
                                                           "Failed to create the Metal directional shadow render encoder."));
        }
        [encoder pushDebugGroup:@"Horo Editor Directional Shadow"];
        [encoder setRenderPipelineState:shadowPipeline];
        [encoder setDepthStencilState:depthState];
        [encoder setCullMode:MTLCullModeFront];
        [encoder setDepthBias:0.00045F slopeScale:2.5F clamp:0.01F];
        for (const Render::RenderStaticMeshInstance &instance : scene.instances) {
            const auto mesh = resources.FindMesh(instance.mesh.id.value);
            if (!mesh.has_value()) {
                [encoder endEncoding];
                return Result<void>::Failure(
                    MakeViewportError(RendererErrors::ViewportStaleMeshResource, "Shadow pass instance references a stale mesh resource."));
            }
            const auto binding = MetalViewportResourceBridge::ResolveMesh(*frontend, mesh->mesh);
            if (binding.HasError()) {
                [encoder endEncoding];
                return Result<void>::Failure(binding.ErrorValue());
            }
            const Math::Mat4 shadowMvp = Math::Multiply(shadow.viewProjection, instance.localToWorld);
            [encoder setVertexBuffer:(__bridge id<MTLBuffer>)binding.Value().vertexBuffer offset:0 atIndex:0];
            [encoder setVertexBytes:&shadowMvp length:sizeof(shadowMvp) atIndex:1];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:mesh->indexCount
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:(__bridge id<MTLBuffer>)binding.Value().indexBuffer
                         indexBufferOffset:0];
        }
        [encoder popDebugGroup];
        [encoder endEncoding];
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererMetal::Impl::EncodeViewportPass(id<MTLCommandBuffer> commandBuffer,
                                                                       const Render::StaticMeshPassDescriptor &descriptor,
                                                                       const std::optional<EditorViewportDirectionalShadowView> &shadow,
                                                                       const MetalViewportFrameResources &frameResources,
                                                                       const float aspect) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = frameResources.colorTexture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(descriptor.clearColor.red, descriptor.clearColor.green,
                                                                descriptor.clearColor.blue, descriptor.clearColor.alpha);
        pass.depthAttachment.texture = frameResources.depthTexture;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 1.0;
        id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (encoder == nil) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportMetalEncoderCreationFailed,
                                                           "Failed to create the Metal viewport render encoder."));
        }
        [encoder pushDebugGroup:@"Horo Editor Viewport"];
        [encoder setCullMode:MTLCullModeNone];
        const EditorViewportExtent extent = resources.AllocatedExtent();
        if (const Result<void> grid = DrawGrid(encoder, descriptor.scene.camera, aspect, static_cast<float>(extent.height));
            grid.HasError()) {
            [encoder endEncoding];
            return grid;
        }
        [encoder setRenderPipelineState:pipeline];
        [encoder setDepthStencilState:depthState];
        UploadLighting(encoder, descriptor.scene, shadow);
        [encoder setFragmentTexture:frameResources.shadowTexture atIndex:0];
        [encoder setFragmentSamplerState:shadowSampler atIndex:0];
        if (const Result<void> meshes = DrawMeshes(encoder, descriptor.scene, shadow, aspect); meshes.HasError()) {
            [encoder endEncoding];
            return meshes;
        }
        if (const Result<void> visualizer = DrawLightVisualizer(encoder, descriptor.scene.camera, aspect); visualizer.HasError()) {
            [encoder endEncoding];
            return visualizer;
        }
        [encoder popDebugGroup];
        [encoder endEncoding];
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererMetal::Impl::DrawGrid(id<MTLRenderCommandEncoder> encoder, const Render::RenderCameraView &camera,
                                                             const float aspect, const float viewportHeightPixels) const {
        if (!gridOptions.visible)
            return Result<void>::Success();
        ViewportGridGeometry grid;
        if (!BuildViewportGridGeometry({.camera = camera,
                                        .aspect = aspect,
                                        .viewportHeightPixels = viewportHeightPixels,
                                        .targetMinorSpacingPixels = gridOptions.targetMinorSpacingPixels,
                                        .targetLineWidthPixels = gridOptions.targetLineWidthPixels},
                                       grid)) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportInvalidScene, "Failed to build finite viewport grid geometry."));
        }
        const Result<Math::Mat4> viewProjection = BuildRenderMvp(camera, Math::Mat4::Identity(), aspect, Math::ClipDepthRange::ZeroToOne);
        if (viewProjection.HasError())
            return Result<void>::Failure(viewProjection.ErrorValue());
        std::byte *gridBytes = static_cast<std::byte *>(gridVertexBuffer.contents);
        NSUInteger gridOffset = 0;
        const auto encodeBatch = [&](const ViewportGridLineBatch batch) {
            if (batch.positions.empty())
                return;
            const NSUInteger byteCount = static_cast<NSUInteger>(batch.positions.size_bytes());
            std::memcpy(gridBytes + gridOffset, batch.positions.data(), byteCount);
            [encoder setVertexBuffer:gridVertexBuffer offset:gridOffset atIndex:0];
            [encoder setVertexBytes:viewProjection.Value().values.data() length:sizeof(viewProjection.Value().values) atIndex:1];
            const MetalGridStyle style{.color = batch.color};
            [encoder setFragmentBytes:&style length:sizeof(style) atIndex:0];
            const MTLPrimitiveType primitive =
                batch.topology == ViewportGridPrimitiveTopology::Triangles ? MTLPrimitiveTypeTriangle : MTLPrimitiveTypeLine;
            [encoder drawPrimitives:primitive vertexStart:0 vertexCount:static_cast<NSUInteger>(batch.positions.size())];
            gridOffset += byteCount;
        };
        [encoder setRenderPipelineState:gridPipeline];
        [encoder setDepthStencilState:gridDepthState];
        encodeBatch(grid.RegularLines());
        encodeBatch(grid.Axes());
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererMetal::Impl::DrawMeshes(id<MTLRenderCommandEncoder> encoder, const Render::RenderSceneView &scene,
                                                               const std::optional<EditorViewportDirectionalShadowView> &shadow,
                                                               const float aspect) const {
        for (const Render::RenderStaticMeshInstance &instance : scene.instances) {
            const auto mesh = resources.FindMesh(instance.mesh.id.value);
            if (!mesh.has_value()) {
                return Result<void>::Failure(
                    MakeViewportError(RendererErrors::ViewportStaleMeshResource, "Viewport instance references a stale mesh resource."));
            }
            const auto binding = MetalViewportResourceBridge::ResolveMesh(*frontend, mesh->mesh);
            if (binding.HasError())
                return Result<void>::Failure(binding.ErrorValue());
            const Result<Math::Mat4> mvp = BuildRenderMvp(scene.camera, instance.localToWorld, aspect, Math::ClipDepthRange::ZeroToOne);
            if (mvp.HasError())
                return Result<void>::Failure(mvp.ErrorValue());
            const MetalObjectTransforms transforms{
                .mvp = mvp.Value(),
                .model = instance.localToWorld,
                .shadowMvp = shadow.has_value() ? Math::Multiply(shadow->viewProjection, instance.localToWorld) : Math::Mat4::Identity(),
            };
            const MetalSelectionStyle selectionStyle{instance.presentation.tint, instance.presentation.tintStrength};
            [encoder setVertexBuffer:(__bridge id<MTLBuffer>)binding.Value().vertexBuffer offset:0 atIndex:0];
            [encoder setVertexBytes:&transforms length:sizeof(transforms) atIndex:1];
            [encoder setFragmentBytes:&selectionStyle length:sizeof(selectionStyle) atIndex:0];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:mesh->indexCount
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:(__bridge id<MTLBuffer>)binding.Value().indexBuffer
                         indexBufferOffset:0];
        }
        return Result<void>::Success();
    }

    Result<void> EditorViewportRendererMetal::Impl::DrawLightVisualizer(id<MTLRenderCommandEncoder> encoder,
                                                                        const Render::RenderCameraView &camera, const float aspect) {
        if (!lightVisualizerOptions.selectedLight.has_value())
            return Result<void>::Success();
        LightVisualizerGeometry geometry;
        if (!BuildLightVisualizerGeometry({.camera = camera, .light = *lightVisualizerOptions.selectedLight}, geometry)) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportInvalidScene, "Failed to build selected Light visualizer geometry."));
        }
        const Result<Math::Mat4> viewProjection = BuildRenderMvp(camera, Math::Mat4::Identity(), aspect, Math::ClipDepthRange::ZeroToOne);
        if (viewProjection.HasError())
            return Result<void>::Failure(viewProjection.ErrorValue());
        std::memcpy(gridVertexBuffer.contents, geometry.Lines().data(), geometry.Lines().size_bytes());
        [encoder setRenderPipelineState:gridPipeline];
        [encoder setDepthStencilState:gridDepthState];
        [encoder setVertexBuffer:gridVertexBuffer offset:0 atIndex:0];
        [encoder setVertexBytes:viewProjection.Value().values.data() length:sizeof(viewProjection.Value().values) atIndex:1];
        const MetalGridStyle style{.color = geometry.color};
        [encoder setFragmentBytes:&style length:sizeof(style) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:static_cast<NSUInteger>(geometry.vertexCount)];
        return Result<void>::Success();
    }

    void EditorViewportRendererMetal::Impl::UploadLighting(id<MTLRenderCommandEncoder> encoder, const Render::RenderSceneView &scene,
                                                           const std::optional<EditorViewportDirectionalShadowView> &shadow) const {
        MetalSceneLighting sceneLighting;
        sceneLighting.cameraPosition = scene.camera.position;
        sceneLighting.lightCount = static_cast<std::uint32_t>(scene.lights.size());
        sceneLighting.shadowEnabled = shadow.has_value() ? 1U : 0U;
        sceneLighting.shadowLightIndex = shadow.has_value() ? static_cast<std::uint32_t>(shadow->lightIndex) : 0U;
        for (std::size_t index = 0; index < scene.lights.size(); ++index) {
            const Render::RenderLight &light = scene.lights[index];
            sceneLighting.lights[index] = {
                .positionKind = {light.position.x, light.position.y, light.position.z, static_cast<float>(light.kind)},
                .directionRange = {light.direction.x, light.direction.y, light.direction.z, light.range},
                .colorIntensity = {light.color.x, light.color.y, light.color.z, light.intensity},
                .cone = {light.innerConeCosine, light.outerConeCosine, 0.0F, 0.0F},
            };
        }
        [encoder setFragmentBytes:&sceneLighting length:sizeof(sceneLighting) atIndex:1];
    }

    /** @copydoc EditorViewportRendererMetal::EditorViewportRendererMetal */
    EditorViewportRendererMetal::EditorViewportRendererMetal(Render::RenderFrontend &frontend,
                                                             Render::MetalEditorGraphicsBridge &graphicsBridge) noexcept
        : impl_(std::make_unique<Impl>(frontend, graphicsBridge)) {}

    /** @copydoc EditorViewportRendererMetal::~EditorViewportRendererMetal */
    EditorViewportRendererMetal::~EditorViewportRendererMetal() {
        Shutdown();
    }

    /** @copydoc EditorViewportRendererMetal::Initialize */
    Result<void> EditorViewportRendererMetal::Initialize() {
        if (impl_->initialized) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportAlreadyInitialized, "Editor viewport renderer is already initialized."));
        }
        impl_->device = (__bridge id<MTLDevice>)impl_->graphicsBridge->Device();
        if (impl_->device == nil) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportMetalDeviceUnavailable, "Metal presentation device is unavailable."));
        }

        NSError *error = nil;
        NSString *shaderSource = [NSString stringWithUTF8String:MetalViewportShaderSource()];
        impl_->library = [impl_->device newLibraryWithSource:shaderSource options:nil error:&error];
        if (impl_->library == nil) {
            Shutdown();
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportShaderCompileFailed,
                                                           ErrorMessage(@"Metal viewport shader compilation failed", error)));
        }

        if (const Result<void> pipelines = impl_->CreateMainPipelines(); pipelines.HasError()) {
            Shutdown();
            return pipelines;
        }
        if (const Result<void> shadowPipeline = impl_->CreateShadowPipeline(); shadowPipeline.HasError()) {
            Shutdown();
            return shadowPipeline;
        }
        if (const Result<void> resources = impl_->CreateSupportResources(); resources.HasError()) {
            Shutdown();
            return resources;
        }

        impl_->initialized = true;
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererMetal::Shutdown */
    void EditorViewportRendererMetal::Shutdown() noexcept {
        impl_->graphicsBridge->WaitUntilIdle();
        impl_->resources.Shutdown();
        impl_->gridVertexBuffer = nil;
        if (impl_->gridVertexBufferHandle.IsValid())
            static_cast<void>(impl_->frontend->ReleaseBuffer(impl_->gridVertexBufferHandle));
        impl_->gridVertexBufferHandle = {};
        impl_->gridDepthState = nil;
        impl_->depthState = nil;
        impl_->shadowSampler = nil;
        impl_->shadowPipeline = nil;
        impl_->gridPipeline = nil;
        impl_->pipeline = nil;
        impl_->library = nil;
        impl_->device = nil;
        impl_->requestedExtent = {};
        impl_->gridOptions = {};
        impl_->lightVisualizerOptions = {};
        impl_->initialized = false;
    }

    /** @copydoc EditorViewportRendererMetal::RequestExtent */
    void EditorViewportRendererMetal::RequestExtent(const EditorViewportExtent extent) noexcept {
        impl_->requestedExtent.width = std::min(extent.width, maxViewportDimension);
        impl_->requestedExtent.height = std::min(extent.height, maxViewportDimension);
    }

    /** @copydoc EditorViewportRendererMetal::RequestGrid */
    void EditorViewportRendererMetal::RequestGrid(const EditorViewportGridOptions &options) noexcept {
        impl_->gridOptions = options;
        if (!std::isfinite(impl_->gridOptions.targetMinorSpacingPixels) || impl_->gridOptions.targetMinorSpacingPixels <= 0.0F)
            impl_->gridOptions.targetMinorSpacingPixels = 48.0F;
    }

    /** @copydoc EditorViewportRendererMetal::RequestLightVisualizer */
    void EditorViewportRendererMetal::RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept {
        impl_->lightVisualizerOptions = options;
    }

    /** @copydoc EditorViewportRendererMetal::RequestedExtent */
    EditorViewportExtent EditorViewportRendererMetal::RequestedExtent() const noexcept {
        const EditorViewportExtent allocatedExtent = impl_->resources.AllocatedExtent();
        return impl_->requestedExtent.IsValid() && allocatedExtent.IsValid() ? allocatedExtent : impl_->requestedExtent;
    }

    /** @copydoc EditorViewportRendererMetal::ClipDepthRange */
    Math::ClipDepthRange EditorViewportRendererMetal::ClipDepthRange() const noexcept {
        return Math::ClipDepthRange::ZeroToOne;
    }

    /** @copydoc EditorViewportRendererMetal::PrepareResources */
    Result<std::optional<Render::RenderTargetHandle>> EditorViewportRendererMetal::PrepareResources(Render::RenderFrontend &frontend,
                                                                                                    const Render::RenderSceneView &scene) {
        if (!impl_->initialized || impl_->frontend != &frontend) {
            return Result<std::optional<Render::RenderTargetHandle>>::Failure(
                MakeViewportError(RendererErrors::ViewportNotInitialized, "Viewport resource owner is not initialized."));
        }
        // Resource preparation runs before the panel draws. Consume the preceding
        // UI frame's request here so the current frame can publish a fresh request
        // that survives pass execution and advances an asynchronous resize next frame.
        return impl_->resources.Prepare(scene, std::exchange(impl_->requestedExtent, EditorViewportExtent{}));
    }

    /** @copydoc EditorViewportRendererMetal::ExecuteStaticMeshPass */
    Result<void> EditorViewportRendererMetal::ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &descriptor) {
        if (!impl_->initialized) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportNotInitialized, "Viewport renderer is not initialized."));
        }
        // A panel must request an extent every UI frame. PrepareResources consumes
        // the request at the next frame boundary, so hidden/inactive tabs still stop
        // rendering without starving a resize that needs multiple boundary advances.
        const EditorViewportExtent requestedExtent = impl_->requestedExtent;
        if (!requestedExtent.IsValid())
            return Result<void>::Success();
        const EditorViewportExtent allocatedExtent = impl_->resources.AllocatedExtent();
        const EditorViewportExtent renderExtent = allocatedExtent.IsValid() ? allocatedExtent : requestedExtent;
        if (const Result<void> valid = impl_->ValidatePassRequest(descriptor, renderExtent); valid.HasError())
            return valid;

        id<MTLCommandBuffer> commandBuffer = (__bridge id<MTLCommandBuffer>)impl_->graphicsBridge->CurrentCommandBuffer();
        if (commandBuffer == nil) {
            return Result<void>::Failure(MakeViewportError(RendererErrors::ViewportMetalNoActiveFrame,
                                                           "Metal viewport rendering requires an active renderer frame."));
        }
        const Result<std::optional<EditorViewportDirectionalShadowView>> shadow =
            BuildEditorViewportDirectionalShadowView(descriptor.scene, Math::ClipDepthRange::ZeroToOne);
        if (shadow.HasError())
            return Result<void>::Failure(shadow.ErrorValue());
        const auto frameResources = impl_->ResolveFrameResources();
        if (frameResources.HasError())
            return Result<void>::Failure(frameResources.ErrorValue());
        const float aspect = static_cast<float>(allocatedExtent.width) / static_cast<float>(allocatedExtent.height);
        if (shadow.Value().has_value()) {
            const Result<void> encoded =
                impl_->EncodeShadowPass(commandBuffer, descriptor.scene, *shadow.Value(), frameResources.Value().shadowTexture);
            if (encoded.HasError())
                return encoded;
        }
        return impl_->EncodeViewportPass(commandBuffer, descriptor, shadow.Value(), frameResources.Value(), aspect);
    }

    /** @copydoc EditorViewportRendererMetal::TextureView */
    EditorViewportTextureView EditorViewportRendererMetal::TextureView() const noexcept {
        return EditorViewportTextureView{
            .textureId = impl_->resources.ImageIdentity(),
            .u0 = 0.0F,
            .v0 = 0.0F,
            .u1 = 1.0F,
            .v1 = 1.0F,
        };
    }

    /** @copydoc EditorViewportRendererMetal::IsReady */
    bool EditorViewportRendererMetal::IsReady() const noexcept {
        return impl_->initialized && impl_->resources.IsReady();
    }
}  // namespace Horo::Editor
