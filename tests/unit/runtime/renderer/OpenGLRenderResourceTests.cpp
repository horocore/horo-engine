#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenGLBackendInternal.h"
#include "RenderMemoryTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>

namespace Horo::Render::OpenGLResourceTests {

    void Check(const bool condition) {
        REQUIRE((condition));
    }

    class ResourcePresentationPort final : public IOpenGLPresentationPort {
    public:
        Result<void> CreateContext(const OpenGLContextDescriptor &) override {
            return Result<void>::Success();
        }

        Result<void> MakeCurrent() override {
            return Result<void>::Success();
        }

        Result<void> LoadCommandDispatch() override {
            return Result<void>::Success();
        }

        Result<OpenGLContextFacts> QueryContextFacts() override {
            return Result<OpenGLContextFacts>::Success({.apiFamily = OpenGLApiFamily::Desktop,
                                                        .majorVersion = 4,
                                                        .minorVersion = 1,
                                                        .profile = OpenGLContextProfile::Core,
                                                        .requiredEntryPointsAvailable = true,
                                                        .maxTexture2DSize = 16384,
                                                        .maxColorAttachments = 8,
                                                        .maxVertexAttributes = 16});
        }

        Result<void> SetPresentMode(PresentMode) override {
            return Result<void>::Success();
        }

        Result<void> SwapBuffers() override {
            return Result<void>::Success();
        }

        void DestroyContext() noexcept override {
            // This test double owns no native context.
        }
    };

    struct ResourceCommandState {
        std::uint32_t nextObject{10};
        int generatedBuffers{0};
        int deletedBuffers{0};
        int generatedVertexArrays{0};
        int deletedVertexArrays{0};
        int generatedTextures{0};
        int deletedTextures{0};
        int generatedFramebuffers{0};
        int deletedFramebuffers{0};
        int uploads{0};
        std::size_t emptyBufferBytes{0};
        int attachments{0};
        std::array<std::uint32_t, 2> attachedTextures{};
        bool framebufferComplete{true};
        bool failBufferAllocation{false};
    };

    ResourceCommandState resourceCommandState;

    void ProbeNoOp(std::uint32_t) {
        // This probe intentionally records no state.
    }

    void ProbeViewport(std::int32_t, std::int32_t, std::int32_t, std::int32_t) {
        // Viewport state is outside this resource test's scope.
    }

    void ProbeClearColor(float, float, float, float) {
        // Clear color state is outside this resource test's scope.
    }

    void ProbeGenerateBuffers(const std::int32_t count, std::uint32_t *objects) {
        resourceCommandState.generatedBuffers += count;
        for (std::int32_t index = 0; index < count; ++index) {
            objects[index] = resourceCommandState.failBufferAllocation ? 0 : resourceCommandState.nextObject++;
            resourceCommandState.failBufferAllocation = false;
        }
    }

    void ProbeDeleteBuffers(const std::int32_t count, const std::uint32_t *) {
        resourceCommandState.deletedBuffers += count;
    }

    void ProbeGenerateVertexArrays(const std::int32_t count, std::uint32_t *objects) {
        resourceCommandState.generatedVertexArrays += count;
        for (std::int32_t index = 0; index < count; ++index)
            objects[index] = resourceCommandState.nextObject++;
    }

    void ProbeDeleteVertexArrays(const std::int32_t count, const std::uint32_t *) {
        resourceCommandState.deletedVertexArrays += count;
    }

    void ProbeGenerateTextures(const std::int32_t count, std::uint32_t *objects) {
        resourceCommandState.generatedTextures += count;
        for (std::int32_t index = 0; index < count; ++index)
            objects[index] = resourceCommandState.nextObject++;
    }

    void ProbeDeleteTextures(const std::int32_t count, const std::uint32_t *) {
        resourceCommandState.deletedTextures += count;
    }

    void ProbeGenerateFramebuffers(const std::int32_t count, std::uint32_t *objects) {
        resourceCommandState.generatedFramebuffers += count;
        for (std::int32_t index = 0; index < count; ++index)
            objects[index] = resourceCommandState.nextObject++;
    }

    void ProbeDeleteFramebuffers(const std::int32_t count, const std::uint32_t *) {
        resourceCommandState.deletedFramebuffers += count;
    }

    void ProbeBindObject(std::uint32_t, std::uint32_t) {
        // Binding state is outside this resource test's scope.
    }

    void ProbeBufferData(std::uint32_t, const std::size_t byteSize, const std::span<const std::byte> data, std::uint32_t) {
        ++resourceCommandState.uploads;
        if (data.empty())
            resourceCommandState.emptyBufferBytes = byteSize;
    }

    void ProbeVertexAttributePointer(std::uint32_t, std::int32_t, std::uint32_t, std::uint8_t, std::int32_t, std::uintptr_t) {
        // Vertex attribute state is outside this resource test's scope.
    }

    void ProbeEnableVertexAttribute(std::uint32_t) {
        // Vertex attribute enablement is outside this resource test's scope.
    }

    void ProbeTextureParameter(std::uint32_t, std::uint32_t, std::int32_t) {
        // Texture parameters are outside this resource test's scope.
    }

    void ProbeTextureImage(const Detail::OpenGLTextureImageDescriptor &) {
        // Texture uploads are outside this resource test's scope.
    }

    void ProbeFramebufferTexture(std::uint32_t, std::uint32_t, const std::uint32_t texture, std::int32_t) {
        const auto slot = static_cast<std::size_t>(resourceCommandState.attachments++);
        if (slot < resourceCommandState.attachedTextures.size())
            resourceCommandState.attachedTextures[slot] = texture;
    }

    std::uint32_t ProbeCheckFramebuffer(std::uint32_t) {
        return resourceCommandState.framebufferComplete ? 0x8CD5U : 0;
    }

    [[nodiscard]] Detail::OpenGLCommandFunctions ResourceProbeFunctions() noexcept {
        return {
            .viewport = &ProbeViewport,
            .clearColor = &ProbeClearColor,
            .clear = &ProbeNoOp,
            .buffers = {.generateBuffers = &ProbeGenerateBuffers,
                        .deleteBuffers = &ProbeDeleteBuffers,
                        .bindBuffer = &ProbeBindObject,
                        .bufferData = &ProbeBufferData},
            .vertexArrays = {.generateVertexArrays = &ProbeGenerateVertexArrays,
                             .deleteVertexArrays = &ProbeDeleteVertexArrays,
                             .bindVertexArray = &ProbeBindObject,
                             .vertexAttributePointer = &ProbeVertexAttributePointer,
                             .enableVertexAttribute = &ProbeEnableVertexAttribute},
            .textures = {.generateTextures = &ProbeGenerateTextures,
                         .deleteTextures = &ProbeDeleteTextures,
                         .bindTexture = &ProbeBindObject,
                         .textureParameter = &ProbeTextureParameter,
                         .textureImage = &ProbeTextureImage},
            .framebuffers = {.generateFramebuffers = &ProbeGenerateFramebuffers,
                             .deleteFramebuffers = &ProbeDeleteFramebuffers,
                             .bindFramebuffer = &ProbeBindObject,
                             .framebufferTexture = &ProbeFramebufferTexture,
                             .checkFramebuffer = &ProbeCheckFramebuffer,
                             .drawBuffer = &ProbeNoOp,
                             .readBuffer = &ProbeNoOp},
        };
    }

    [[nodiscard]] std::unique_ptr<IRenderBackend> CreateResourceBackend(ResourcePresentationPort &port) {
        RenderBackendRegistry registry;
        Check(
            Detail::RegisterOpenGLRenderBackendWithFunctions(registry, port, OpenGLBackendOptions{}, ResourceProbeFunctions()).HasValue());
        Check(registry.Seal().HasValue());
        auto created = registry.Create(RenderBackendId{"opengl"});
        Check(created.HasValue());
        return std::move(created).Value();
    }

    /** @brief Creates and initializes one resource-capable backend for a test case. */
    [[nodiscard]] std::unique_ptr<IRenderBackend> CreateInitializedResourceBackend(ResourcePresentationPort &port) {
        auto backend = CreateResourceBackend(port);
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());
        return backend;
    }

    struct ResourceInstances {
        std::uint64_t vertex{0};
        std::uint64_t index{0};
        std::uint64_t mesh{0};
        std::uint64_t color{0};
        std::uint64_t depth{0};
        std::uint64_t colorView{0};
        std::uint64_t depthView{0};
        std::uint64_t target{0};
    };

    /** @brief Builds the canonical triangle descriptor shared by success and rejection checks. */
    [[nodiscard]] RenderMeshDescriptor TriangleMeshDescriptor() noexcept {
        return {.vertexBuffer = {{1}, 1, 1},
                .indexBuffer = {{1}, 2, 1},
                .vertexStride = sizeof(MeshVertex),
                .vertexCount = 3,
                .indexCount = 3,
                .localBounds = {{-1, -1, -1}, {1, 1, 1}}};
    }

    /** @brief Builds a full-range texture-view descriptor for the requested test identity. */
    [[nodiscard]] RenderTextureViewDescriptor TextureViewDescriptor(const std::uint32_t identity, const RenderTextureFormat format,
                                                                    const RenderTextureAspect aspect) noexcept {
        return {.texture = {{1}, identity, 1}, .format = format, .aspect = aspect};
    }

    /** @brief Extracts the native texture object encoded in an opaque OpenGL texture-view identity. */
    [[nodiscard]] std::uint32_t NativeTexture(const std::uint64_t view) noexcept {
        return static_cast<std::uint32_t>(view);
    }

    /** @brief Creates one triangle's generic buffer and mesh resources. */
    void CreateMeshResources(IRenderBackend &backend, ResourceInstances &resources) {
        const std::array<std::byte, sizeof(MeshVertex) * 3> vertices{};
        const std::array<std::byte, sizeof(std::uint32_t) * 3> indices{};
        const RenderBufferDescriptor vertexDescriptor{.byteSize = vertices.size(),
                                                      .usage = RenderBufferUsage::Vertex,
                                                      .access = RenderBufferAccess::DeviceLocal};
        const RenderBufferDescriptor indexDescriptor{.byteSize = indices.size(),
                                                     .usage = RenderBufferUsage::Index,
                                                     .access = RenderBufferAccess::DeviceLocal};
        const auto vertexPlan = backend.QueryBufferMemoryCost(vertexDescriptor).Value();
        const auto indexPlan = backend.QueryBufferMemoryCost(indexDescriptor).Value();
        auto vertex = backend.CreateBuffer(vertexDescriptor, {}, TestSupport::PlacementFor(vertexPlan, 1, 1));
        auto index = backend.CreateBuffer(indexDescriptor, indices, TestSupport::PlacementFor(indexPlan, 2, 2));
        Check(vertex.HasValue() && index.HasValue());
        auto mesh = backend.CreateMesh(TriangleMeshDescriptor(), vertex.Value(), index.Value());
        Check(mesh.HasValue());
        resources.vertex = vertex.Value();
        resources.index = index.Value();
        resources.mesh = mesh.Value();
    }

    /** @brief Creates a color/depth texture pair, views, and their render target. */
    [[nodiscard]] RenderTargetDescriptor CreateTargetResources(IRenderBackend &backend, ResourceInstances &resources) {
        const RenderTextureDescriptor colorDescriptor{.extent = {64, 32},
                                                      .format = RenderTextureFormat::Rgba8Unorm,
                                                      .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
        const RenderTextureDescriptor depthDescriptor{.extent = {64, 32},
                                                      .format = RenderTextureFormat::Depth24Stencil8,
                                                      .usage = RenderTextureUsage::RenderAttachment};
        const auto colorPlan = backend.QueryTextureMemoryCost(colorDescriptor).Value();
        const auto depthPlan = backend.QueryTextureMemoryCost(depthDescriptor).Value();
        auto color = backend.CreateTexture(colorDescriptor, {}, TestSupport::PlacementFor(colorPlan, 3, 3));
        auto depth = backend.CreateTexture(depthDescriptor, {}, TestSupport::PlacementFor(depthPlan, 4, 4));
        Check(color.HasValue() && depth.HasValue());
        auto colorView =
            backend.CreateTextureView(TextureViewDescriptor(3, RenderTextureFormat::Rgba8Unorm, RenderTextureAspect::Color), color.Value());
        auto depthView =
            backend.CreateTextureView(TextureViewDescriptor(4, RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::DepthStencil),
                                      depth.Value());
        Check(colorView.HasValue() && depthView.HasValue());
        auto duplicateColorView =
            backend.CreateTextureView(TextureViewDescriptor(3, RenderTextureFormat::Rgba8Unorm, RenderTextureAspect::Color), color.Value());
        Check(duplicateColorView.HasValue());
        Check(duplicateColorView.Value() == colorView.Value());
        backend.DestroyTextureView(duplicateColorView.Value());
        Check(NativeTexture(colorView.Value()) == color.Value());
        Check(NativeTexture(depthView.Value()) == depth.Value());
        Check(colorView.Value() != depthView.Value());
        const RenderTargetDescriptor descriptor{.colorAttachment = {{1}, 5, 1}, .depthAttachment = {{1}, 6, 1}, .extent = {64, 32}};
        auto target = backend.CreateRenderTarget(descriptor, colorView.Value(), depthView.Value());
        Check(target.HasValue());
        resources.color = color.Value();
        resources.depth = depth.Value();
        resources.colorView = colorView.Value();
        resources.depthView = depthView.Value();
        resources.target = target.Value();
        return descriptor;
    }

    /** @brief Verifies creation accounting and incomplete-framebuffer rollback. */
    void CheckCreationAndRollback(IRenderBackend &backend, const ResourceInstances &resources, const RenderTargetDescriptor &descriptor) {
        Check(resourceCommandState.generatedBuffers == 2);
        Check(resourceCommandState.generatedVertexArrays == 1);
        Check(resourceCommandState.generatedTextures == 2);
        Check(resourceCommandState.generatedFramebuffers == 1);
        Check(resourceCommandState.uploads == 2);
        Check(resourceCommandState.emptyBufferBytes == sizeof(MeshVertex) * 3);
        Check(resourceCommandState.attachments == 2);
        Check(resourceCommandState.attachedTextures[0] == resources.color);
        Check(resourceCommandState.attachedTextures[1] == resources.depth);
        resourceCommandState.framebufferComplete = false;
        auto incomplete = backend.CreateRenderTarget(descriptor, resources.colorView, resources.depthView);
        Check(incomplete.HasError());
        Check(incomplete.ErrorValue().code.Value() == "render.opengl.resource_creation_failed");
        Check(resourceCommandState.deletedFramebuffers == 1);
    }

    /** @brief Verifies that native dependency identities and compatibility remain backend-owned. */
    void CheckDependencyValidation(IRenderBackend &backend, const ResourceInstances &resources, const RenderTargetDescriptor &descriptor) {
        const auto foreignMesh = backend.CreateMesh(TriangleMeshDescriptor(), 900, 901);
        Check(foreignMesh.HasError());
        Check(foreignMesh.ErrorValue().code.Value() == "render.opengl.resource_identity_invalid");

        auto mismatchedTarget = descriptor;
        mismatchedTarget.extent.width += 1;
        const auto target = backend.CreateRenderTarget(mismatchedTarget, resources.colorView, resources.depthView);
        Check(target.HasError());
        Check(target.ErrorValue().code.Value() == "render.opengl.resource_identity_invalid");

        const RenderTextureDescriptor unsupportedTexture{.extent = {64, 64},
                                                         .format = RenderTextureFormat::Rgba8Unorm,
                                                         .mipCount = 2,
                                                         .usage = RenderTextureUsage::Sampled};
        const auto unsupported = backend.QueryTextureMemoryCost(unsupportedTexture);
        Check(unsupported.HasError());
        Check(unsupported.ErrorValue().code.Value() == "render.opengl.unsupported_resource_operation");
    }

    /** @brief Verifies native allocation failure remains distinct from unsupported policy. */
    void CheckAllocationFailure(IRenderBackend &backend) {
        const RenderBufferDescriptor descriptor{.byteSize = 16,
                                                .usage = RenderBufferUsage::Vertex,
                                                .access = RenderBufferAccess::DeviceLocal};
        const auto cost = backend.QueryBufferMemoryCost(descriptor);
        Check(cost.HasValue());
        resourceCommandState.failBufferAllocation = true;
        const auto failed = backend.CreateBuffer(descriptor, {}, TestSupport::PlacementFor(cost.Value(), 20));
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.opengl.resource_creation_failed");
    }

    /** @brief Destroys resources in dependency order and verifies native release accounting. */
    void DestroyResources(IRenderBackend &backend, const ResourceInstances &resources) {
        backend.DestroyRenderTarget(resources.target);
        backend.DestroyTextureView(resources.depthView);
        backend.DestroyTextureView(resources.colorView);
        backend.DestroyTexture(resources.depth);
        backend.DestroyTexture(resources.color);
        backend.DestroyMesh(resources.mesh);
        backend.DestroyBuffer(resources.index);
        backend.DestroyBuffer(resources.vertex);
        Check(resourceCommandState.deletedFramebuffers == 2);
        Check(resourceCommandState.deletedTextures == 2);
        Check(resourceCommandState.deletedVertexArrays == 1);
        Check(resourceCommandState.deletedBuffers == 2);
    }

    /** @brief Verifies recycled native texture names do not retain stale view metadata. */
    void CheckRecycledTexture(IRenderBackend &backend, const std::uint64_t recycledTexture) {
        resourceCommandState.nextObject = static_cast<std::uint32_t>(recycledTexture);
        const RenderTextureDescriptor colorDescriptor{.extent = {16, 16},
                                                      .format = RenderTextureFormat::Rgba8Unorm,
                                                      .usage = RenderTextureUsage::Sampled};
        const auto colorPlan = backend.QueryTextureMemoryCost(colorDescriptor);
        Check(colorPlan.HasValue());
        const auto color = backend.CreateTexture(colorDescriptor, {}, TestSupport::PlacementFor(colorPlan.Value(), 31));
        Check(color.HasValue());
        Check(color.Value() == recycledTexture);
        const auto colorView =
            backend.CreateTextureView(TextureViewDescriptor(32, RenderTextureFormat::Rgba8Unorm, RenderTextureAspect::Color),
                                      color.Value());
        Check(colorView.HasValue());
        Check(NativeTexture(colorView.Value()) == color.Value());
        backend.DestroyTextureView(colorView.Value());
        backend.DestroyTexture(color.Value());
        Check(resourceCommandState.deletedTextures == 2);
    }

    TEST_CASE("OpenGL Generic Resources Realize And Roll Back Through Typed Contracts", "[unit][runtime][renderer][resource]") {
        resourceCommandState = {};
        ResourcePresentationPort port;
        std::unique_ptr<IRenderBackend> backend = CreateInitializedResourceBackend(port);
        Check(backend->Capabilities().supportsBufferResources);
        Check(backend->Capabilities().supportsMeshResources);
        Check(backend->Capabilities().supportsTextureResources);
        Check(backend->Capabilities().supportsRenderTargetResources);
        Check(backend->Capabilities().support.IsValid());
        Check(backend->Capabilities().support.features.Supports(RenderCapability::TextureResources));
        Check(backend->Capabilities().support.queues.Supports(RenderQueueKind::Graphics));
        Check(!backend->Capabilities().support.queues.Supports(RenderQueueKind::Copy));
        const RenderTextureDescriptor oversizedTexture{.extent = {16385, 1},
                                                       .format = RenderTextureFormat::Rgba8Unorm,
                                                       .usage = RenderTextureUsage::Sampled};
        const auto oversizedCost = backend->QueryTextureMemoryCost(oversizedTexture);
        Check(oversizedCost.HasValue());
        const auto oversized = backend->CreateTexture(oversizedTexture, {}, TestSupport::PlacementFor(oversizedCost.Value(), 1));
        Check(oversized.HasError());
        ResourceInstances resources;
        CreateMeshResources(*backend, resources);
        const RenderTargetDescriptor targetDescriptor = CreateTargetResources(*backend, resources);
        CheckDependencyValidation(*backend, resources, targetDescriptor);
        CheckCreationAndRollback(*backend, resources, targetDescriptor);
        CheckAllocationFailure(*backend);
        DestroyResources(*backend, resources);
        backend->Shutdown();
    }

    TEST_CASE("OpenGL Texture Views Preserve Parent Lifetime And Distinct Properties", "[unit][runtime][renderer][resource]") {
        resourceCommandState = {};
        ResourcePresentationPort port;
        std::unique_ptr<IRenderBackend> backend = CreateInitializedResourceBackend(port);

        const RenderTextureDescriptor depthDescriptor{.extent = {32, 32},
                                                      .format = RenderTextureFormat::Depth24Stencil8,
                                                      .usage = RenderTextureUsage::RenderAttachment};
        const auto depthPlan = backend->QueryTextureMemoryCost(depthDescriptor);
        Check(depthPlan.HasValue());
        const auto depth = backend->CreateTexture(depthDescriptor, {}, TestSupport::PlacementFor(depthPlan.Value(), 30));
        Check(depth.HasValue());
        const auto depthView =
            backend->CreateTextureView(TextureViewDescriptor(30, RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::Depth),
                                       depth.Value());
        const auto duplicateDepthView =
            backend->CreateTextureView(TextureViewDescriptor(30, RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::Depth),
                                       depth.Value());
        const auto depthStencilView =
            backend->CreateTextureView(TextureViewDescriptor(30, RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::DepthStencil),
                                       depth.Value());
        Check(depthView.HasValue() && duplicateDepthView.HasValue() && depthStencilView.HasValue());
        Check(depthView.Value() == duplicateDepthView.Value());
        Check(depthView.Value() != depthStencilView.Value());
        Check(NativeTexture(depthView.Value()) == depth.Value());
        Check(NativeTexture(depthStencilView.Value()) == depth.Value());

        backend->DestroyTexture(std::numeric_limits<std::uint64_t>::max());
        Check(resourceCommandState.deletedTextures == 0);
        backend->DestroyTexture(depth.Value());
        Check(resourceCommandState.deletedTextures == 0);
        const auto lateView =
            backend->CreateTextureView(TextureViewDescriptor(31, RenderTextureFormat::Depth24Stencil8, RenderTextureAspect::Depth),
                                       depth.Value());
        Check(lateView.HasError());
        Check(lateView.ErrorValue().code.Value() == "render.opengl.resource_identity_invalid");

        backend->DestroyTextureView(depthView.Value());
        backend->DestroyTextureView(duplicateDepthView.Value());
        Check(resourceCommandState.deletedTextures == 0);
        backend->DestroyTextureView(depthStencilView.Value());
        Check(resourceCommandState.deletedTextures == 1);

        CheckRecycledTexture(*backend, depth.Value());
        backend->Shutdown();
    }

}  // namespace Horo::Render::OpenGLResourceTests
