#include "RenderFrontendTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;

    TEST_CASE("Frontend Rejects Unsupported And In-Frame Resource Mutations", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        lifecycleState.supportsBufferResources = false;
        lifecycleState.supportsMeshResources = false;
        std::unique_ptr<RenderFrontend> unsupported = CreateTrackingFrontend();
        const std::array<std::byte, 4> bytes{};
        const auto rejectedBuffer = unsupported->CreateBuffer({.byteSize = bytes.size(),
                                                               .usage = RenderBufferUsage::Vertex,
                                                               .access = RenderBufferAccess::DeviceLocal},
                                                              bytes);
        Check(rejectedBuffer.HasError());
        Check(rejectedBuffer.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
        const auto rejectedMesh = unsupported->CreateMesh({.vertexBuffer = {.owner = {1}, .slot = 1, .generation = 1},
                                                           .indexBuffer = {.owner = {1}, .slot = 2, .generation = 1},
                                                           .vertexStride = 4,
                                                           .vertexCount = 1,
                                                           .indexFormat = RenderIndexFormat::UInt32,
                                                           .indexCount = 3,
                                                           .topology = RenderPrimitiveTopology::Triangles,
                                                           .localBounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 1.0F}}});
        Check(rejectedMesh.HasError());
        Check(rejectedMesh.ErrorValue().code.Value() == "render.frontend.resource.unsupported");

        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const PendingTriangleBuffers buffers = QueueTriangleBuffers(*frontend);
        Check(frontend->ProcessResourceRequests().Value() == 2);
        auto pendingMesh = frontend->CreateMesh(MeshDescriptor(buffers.vertex.handle, buffers.index.handle));
        Check(pendingMesh.HasValue());
        const auto pendingReplacement =
            frontend->ReplaceMesh(pendingMesh.Value().handle, MeshDescriptor(buffers.vertex.handle, buffers.index.handle));
        Check(pendingReplacement.HasError());
        Check(pendingReplacement.ErrorValue().code.Value() == "render.frontend.resource.not_ready");
        Check(frontend->CreateMesh({}).ErrorValue().code.Value() == "render.frontend.resource.invalid_mesh_descriptor");

        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {640, 360}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();
        Check(frontend
                  ->CreateBuffer({.byteSize = bytes.size(), .usage = RenderBufferUsage::Vertex, .access = RenderBufferAccess::DeviceLocal},
                                 bytes)
                  .ErrorValue()
                  .code.Value() == "render.frontend.resource.change_during_frame");
        Check(frontend->CreateMesh(MeshDescriptor(buffers.vertex.handle, buffers.index.handle)).ErrorValue().code.Value() ==
              "render.frontend.resource.change_during_frame");
        Check(frontend->ProcessResourceRequests().ErrorValue().code.Value() == "render.frontend.resource.change_during_frame");
        Check(frontend->ReleaseBuffer(buffers.vertex.handle).ErrorValue().code.Value() == "render.frontend.resource.change_during_frame");
        Check(frontend->ReleaseMesh(pendingMesh.Value().handle).ErrorValue().code.Value() ==
              "render.frontend.resource.change_during_frame");
        frame.Cancel();
    }

    TEST_CASE("Frontend Separates Buffer Structure From Current Resource Admission", "[unit][runtime][renderer][resource][descriptor]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const std::array<std::byte, 16> bytes{};

        const RenderBufferDescriptor storageBuffer{.byteSize = bytes.size(),
                                                   .usage = RenderBufferUsage::Storage | RenderBufferUsage::CopyDestination,
                                                   .access = RenderBufferAccess::DeviceLocal};
        Check(ValidateRenderBufferDescriptor(storageBuffer).HasValue());
        const auto unsupportedBuffer = frontend->CreateBuffer(storageBuffer, bytes);
        Check(unsupportedBuffer.HasError());
        Check(unsupportedBuffer.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
        Check(lifecycleState.createBufferCount == 0);

        const auto malformedBuffer = frontend->CreateBuffer({.byteSize = 0, .usage = RenderBufferUsage::Storage}, {});
        Check(malformedBuffer.HasError());
        Check(malformedBuffer.ErrorValue().code.Value() == "render.frontend.resource.invalid_buffer_descriptor");

        const auto baselineBuffer = frontend->CreateBuffer({.byteSize = bytes.size(),
                                                            .usage = RenderBufferUsage::Vertex,
                                                            .access = RenderBufferAccess::DeviceLocal},
                                                           bytes);
        Check(baselineBuffer.HasValue());
        Check(baselineBuffer.Value().handle.slot == 1);
        Check(baselineBuffer.Value().operation.value == 1);
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(lifecycleState.createBufferCount == 1);
    }

    TEST_CASE("Frontend Separates Texture Structure From Current Resource Admission", "[unit][runtime][renderer][resource][descriptor]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();

        const RenderTextureDescriptor volume{.dimension = RenderTextureDimension::ThreeD,
                                             .extent = {8, 8},
                                             .format = RenderTextureFormat::Rgba16Float,
                                             .mipCount = 4,
                                             .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination,
                                             .depth = 8};
        Check(ValidateRenderTextureDescriptor(volume).HasValue());
        const auto unsupportedTexture = frontend->CreateTexture(volume);
        Check(unsupportedTexture.HasError());
        Check(unsupportedTexture.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
        Check(lifecycleState.createTextureCount == 0);

        auto malformedTexture = volume;
        malformedTexture.depth = 0;
        const auto rejectedTexture = frontend->CreateTexture(malformedTexture);
        Check(rejectedTexture.HasError());
        Check(rejectedTexture.ErrorValue().code.Value() == "render.frontend.resource.invalid_texture_descriptor");

        const auto baselineTexture =
            frontend->CreateTexture({.extent = {8, 8}, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::Sampled});
        Check(baselineTexture.HasValue());
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(lifecycleState.createBufferCount == 0);
        Check(lifecycleState.createTextureCount == 1);

        const RenderTextureViewDescriptor arrayView{.texture = baselineTexture.Value().handle,
                                                    .format = RenderTextureFormat::Rgba8Unorm,
                                                    .aspect = RenderTextureAspect::Color,
                                                    .dimension = RenderTextureViewDimension::TwoDArray};
        Check(ValidateRenderTextureViewDescriptor(arrayView).HasValue());
        const auto unsupportedView = frontend->CreateTextureView(arrayView);
        Check(unsupportedView.HasError());
        Check(unsupportedView.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
        Check(lifecycleState.createTextureViewCount == 0);

        const auto baselineView = frontend->CreateTextureView(
            {.texture = baselineTexture.Value().handle, .format = RenderTextureFormat::Rgba8Unorm, .aspect = RenderTextureAspect::Color});
        Check(baselineView.HasValue());
        Check(baselineView.Value().handle.slot == 2);
        Check(baselineView.Value().operation.value == 2);
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(lifecycleState.createTextureViewCount == 1);
    }

    TEST_CASE("Frontend Stores Backend Resource Exceptions As Operation Results", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const std::array<std::byte, 4> bytes{};
        auto buffer = frontend->CreateBuffer({.byteSize = bytes.size(),
                                              .usage = RenderBufferUsage::Vertex,
                                              .access = RenderBufferAccess::DeviceLocal},
                                             bytes);
        Check(buffer.HasValue());
        lifecycleState.throwDuringResourceCreation = true;
        Check(frontend->ProcessResourceRequests().Value() == 1);
        const Result<void> failed = frontend->ResourceOperationResult(buffer.Value().operation);
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.frontend.resource.backend_exception");
    }

    TEST_CASE("Frontend Rejects Malformed Incompatible And Foreign Mesh Uploads", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const std::array<std::byte, 4> bytes{};
        const auto malformed =
            frontend->CreateBuffer({.byteSize = 0, .usage = RenderBufferUsage::Vertex, .access = RenderBufferAccess::DeviceLocal}, {});
        Check(malformed.HasError());
        Check(malformed.ErrorValue().code.Value() == "render.frontend.resource.invalid_buffer_descriptor");
        const auto sizeMismatch =
            frontend->CreateBuffer({.byteSize = 8, .usage = RenderBufferUsage::Vertex, .access = RenderBufferAccess::DeviceLocal}, bytes);
        Check(sizeMismatch.HasError());
        Check(sizeMismatch.ErrorValue().code.Value() == "render.frontend.resource.buffer_upload_size_mismatch");

        const PendingTriangleBuffers buffers = QueueTriangleBuffers(*frontend);
        Check(frontend->ProcessResourceRequests().Value() == 2);
        RenderMeshDescriptor incompatible = MeshDescriptor(buffers.vertex.handle, buffers.index.handle);
        incompatible.vertexCount = std::numeric_limits<std::uint32_t>::max();
        const auto rejectedLayout = frontend->CreateMesh(incompatible);
        Check(rejectedLayout.HasError());
        Check(rejectedLayout.ErrorValue().code.Value() == "render.frontend.resource.invalid_mesh_descriptor");

        std::unique_ptr<RenderFrontend> other = CreateTrackingFrontend();
        const PendingTriangleBuffers foreign = QueueTriangleBuffers(*other);
        Check(other->ProcessResourceRequests().Value() == 2);
        const auto rejectedOwner = frontend->CreateMesh(MeshDescriptor(foreign.vertex.handle, buffers.index.handle));
        Check(rejectedOwner.HasError());
        Check(rejectedOwner.ErrorValue().code.Value() == "render.frontend.resource.wrong_owner");
    }

}  // namespace
