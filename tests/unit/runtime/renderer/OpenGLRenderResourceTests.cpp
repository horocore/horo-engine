#include "Horo/Runtime/Render/RenderFrontend.h"
#include "OpenGLBackendInternal.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

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

        Result<void> SetPresentMode(PresentMode) override {
            return Result<void>::Success();
        }

        Result<void> SwapBuffers() override {
            return Result<void>::Success();
        }

        void DestroyContext() noexcept override {}
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
        bool framebufferComplete{true};
    };

    ResourceCommandState resourceCommandState;

    void ProbeNoOp(std::uint32_t) {}

    void ProbeViewport(std::int32_t, std::int32_t, std::int32_t, std::int32_t) {}

    void ProbeClearColor(float, float, float, float) {}

    void ProbeGenerateBuffers(const std::int32_t count, std::uint32_t *objects) {
        resourceCommandState.generatedBuffers += count;
        for (std::int32_t index = 0; index < count; ++index)
            objects[index] = resourceCommandState.nextObject++;
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

    void ProbeBindObject(std::uint32_t, std::uint32_t) {}

    void ProbeBufferData(std::uint32_t, const std::size_t byteSize, const std::span<const std::byte> data, std::uint32_t) {
        ++resourceCommandState.uploads;
        if (data.empty())
            resourceCommandState.emptyBufferBytes = byteSize;
    }

    void ProbeVertexAttributePointer(std::uint32_t, std::int32_t, std::uint32_t, std::uint8_t, std::int32_t, std::uintptr_t) {}

    void ProbeEnableVertexAttribute(std::uint32_t) {}

    void ProbeTextureParameter(std::uint32_t, std::uint32_t, std::int32_t) {}

    void ProbeTextureImage(const Detail::OpenGLTextureImageDescriptor &) {}

    void ProbeFramebufferTexture(std::uint32_t, std::uint32_t, std::uint32_t, std::int32_t) {
        ++resourceCommandState.attachments;
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

    [[nodiscard]] RenderMemoryPlacement PlacementFor(const RenderMemoryCostPlan &plan, const std::uint64_t attempt) {
        return {.pool = {{1}, attempt},
                .scope = {1, 1},
                .attempt = {attempt},
                .memoryClass = plan.memoryClass,
                .compatibility = plan.compatibility,
                .provenance = plan.provenance,
                .budgetRevision = 1,
                .payloadBytes = plan.payloadBytes,
                .requiredBytes = plan.requiredBytes,
                .backingBytes = plan.requiredBytes,
                .allocationClass = plan.allocationClass};
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
        auto vertex = backend.CreateBuffer(vertexDescriptor, {}, PlacementFor(vertexPlan, 1));
        auto index = backend.CreateBuffer(indexDescriptor, indices, PlacementFor(indexPlan, 2));
        Check(vertex.HasValue() && index.HasValue());
        auto mesh = backend.CreateMesh({.vertexBuffer = {{1}, 1, 1},
                                        .indexBuffer = {{1}, 2, 1},
                                        .vertexStride = sizeof(MeshVertex),
                                        .vertexCount = 3,
                                        .indexCount = 3,
                                        .localBounds = {{-1, -1, -1}, {1, 1, 1}}},
                                       vertex.Value(), index.Value());
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
        auto color = backend.CreateTexture(colorDescriptor, {}, PlacementFor(colorPlan, 3));
        auto depth = backend.CreateTexture(depthDescriptor, {}, PlacementFor(depthPlan, 4));
        Check(color.HasValue() && depth.HasValue());
        auto colorView = backend.CreateTextureView({.texture = {{1}, 3, 1},
                                                    .format = RenderTextureFormat::Rgba8Unorm,
                                                    .aspect = RenderTextureAspect::Color},
                                                   color.Value());
        auto depthView = backend.CreateTextureView({.texture = {{1}, 4, 1},
                                                    .format = RenderTextureFormat::Depth24Stencil8,
                                                    .aspect = RenderTextureAspect::DepthStencil},
                                                   depth.Value());
        Check(colorView.HasValue() && depthView.HasValue());
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
        resourceCommandState.framebufferComplete = false;
        auto incomplete = backend.CreateRenderTarget(descriptor, resources.colorView, resources.depthView);
        Check(incomplete.HasError());
        Check(incomplete.ErrorValue().code.Value() == "render.opengl.unsupported_resource_operation");
        Check(resourceCommandState.deletedFramebuffers == 1);
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
}  // namespace

TEST_CASE("OpenGL Generic Resources Realize And Roll Back Through Typed Contracts", "[unit][runtime][renderer][resource]") {
    resourceCommandState = {};
    ResourcePresentationPort port;
    std::unique_ptr<IRenderBackend> backend = CreateResourceBackend(port);
    Check(backend->Initialize(RenderBackendConfig{}).HasValue());
    Check(backend->Capabilities().supportsBufferResources);
    Check(backend->Capabilities().supportsMeshResources);
    Check(backend->Capabilities().supportsTextureResources);
    Check(backend->Capabilities().supportsRenderTargetResources);
    ResourceInstances resources;
    CreateMeshResources(*backend, resources);
    const RenderTargetDescriptor targetDescriptor = CreateTargetResources(*backend, resources);
    CheckCreationAndRollback(*backend, resources, targetDescriptor);
    DestroyResources(*backend, resources);
    backend->Shutdown();
}
