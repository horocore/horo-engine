#include "RenderFrontendResourceAccess.h"
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

namespace Horo::Tests::RenderFrontendTesting {
    TEST_CASE("Frontend admits atlas CopyDestination through queued texture readiness", "[renderer][resource][descriptor][atlas]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        const RenderTextureDescriptor descriptor{.extent = {512, 128},
                                                 .format = RenderTextureFormat::Rgba8Unorm,
                                                 .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination};
        const std::vector<std::byte> pixels(512 * 128 * 4, std::byte{0x7f});
        const auto created = frontend->CreateTexture(descriptor, pixels);
        REQUIRE(created.HasValue());
        CHECK(lifecycleState.createTextureCount == 0);
        CHECK(frontend->ResourceOperationResult(created.Value().operation).HasError());
        CHECK(frontend->UploadSnapshot().pendingRequests == 1);
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(frontend->ResourceOperationResult(created.Value().operation).HasValue());
        CHECK(lifecycleState.createTextureCount == 1);
        CHECK(lifecycleState.lastTextureInitialData == pixels);
        REQUIRE(frontend->ReleaseTexture(created.Value().handle).HasValue());
        CHECK(lifecycleState.destroyTextureCount == 1);
    }

    TEST_CASE("Frontend copy destination admission retains closed descriptor and backend gates",
              "[renderer][resource][descriptor][atlas]") {
        lifecycleState = {};
        RenderTextureDescriptor descriptor{.extent = {512, 128},
                                           .format = RenderTextureFormat::Rgba8Unorm,
                                           .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination};
        SECTION("Storage is not implemented by this frontend path") {
            descriptor.usage = descriptor.usage | RenderTextureUsage::Storage;
        }
        SECTION("CopySource is not opened by destination upload support") {
            descriptor.usage = descriptor.usage | RenderTextureUsage::CopySource;
        }
        SECTION("Unknown usage bit remains invalid") {
            descriptor.usage = static_cast<RenderTextureUsage>(0x80);
        }
        SECTION("Multisample remains outside the current texture path") {
            descriptor.sampleCount = 4;
        }
        SECTION("Unsupported format remains outside the current texture path") {
            descriptor.format = RenderTextureFormat::Depth32FloatStencil8;
        }
        SECTION("Backend generic texture capability remains required") {
            lifecycleState.supportsTextureResources = false;
        }
        auto frontend = CreateTrackingFrontend();
        REQUIRE(frontend->CreateTexture(descriptor).HasError());
        CHECK(lifecycleState.createTextureCount == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
    }
}  // namespace Horo::Tests::RenderFrontendTesting

namespace Horo::Tests::RenderFrontendTesting {
    TEST_CASE("Frontend atlas memory query preserves native typed failure before reservation", "[renderer][resource][descriptor][atlas]") {
        lifecycleState = {};
        const auto expected = MakeError(RenderMemoryBudgetErrors::UnsupportedAllocation, "Native sampled-transfer format is unavailable.");
        lifecycleState.textureMemoryCostFailure = expected;
        auto frontend = CreateTrackingFrontend();
        const auto rejected = frontend->CreateTexture({.extent = {512, 128},
                                                       .format = RenderTextureFormat::Rgba8Unorm,
                                                       .usage = RenderTextureUsage::Sampled | RenderTextureUsage::CopyDestination});
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(rejected.ErrorValue().code.Value() == expected.code.Value());
        CHECK(rejected.ErrorValue().message == expected.message);
        CHECK(lifecycleState.createTextureCount == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
    }
}  // namespace Horo::Tests::RenderFrontendTesting

namespace Horo::Tests::RenderFrontendTesting {
    TEST_CASE("Frontend UI stencil admission retains selected capability exact usage and native cost failure",
              "[renderer][resource][descriptor][ui_paint]") {
        lifecycleState = {};
        const RenderTextureDescriptor descriptor{.extent = {100, 100},
                                                 .format = RenderTextureFormat::Depth32FloatStencil8,
                                                 .usage = RenderTextureUsage::RenderAttachment};
        auto unsupported = CreateTrackingFrontend();
        REQUIRE(unsupported->CreateTexture(descriptor).HasError());
        CHECK(unsupported->UploadSnapshot().pendingRequests == 0);
        lifecycleState.uiPaint.enabled = true;
        auto frontend = CreateTrackingFrontend();
        const auto created = frontend->CreateTexture(descriptor);
        REQUIRE(created.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(frontend->ResourceOperationResult(created.Value().operation).HasValue());
        REQUIRE(frontend->ReleaseTexture(created.Value().handle).HasValue());
        auto sampled = descriptor;
        sampled.usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
        REQUIRE(frontend->CreateTexture(sampled).HasError());
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        const auto failure = MakeError(RenderMemoryBudgetErrors::UnsupportedAllocation, "Selected stencil allocation rejected.");
        lifecycleState.textureMemoryCostFailure = failure;
        const auto failed = frontend->CreateTexture(descriptor);
        REQUIRE(failed.HasError());
        CHECK(failed.ErrorValue().domain.Value() == failure.domain.Value());
        CHECK(failed.ErrorValue().code.Value() == failure.code.Value());
        CHECK(failed.ErrorValue().message == failure.message);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    }
}  // namespace Horo::Tests::RenderFrontendTesting

namespace {
    using DepthAccess = Horo::Render::Detail::RenderFrontendResourceAccess;

    TEST_CASE("Multiview depth admission rejects absent support and malformed bounds before reservation",
              "[unit][renderer][resource][multiview]") {
        lifecycleState = {};
        auto frontend = CreateTrackingFrontend();
        const auto unsupported = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2);
        REQUIRE(unsupported.HasError());
        CHECK(unsupported.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
        CHECK(DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 0).HasError());
        CHECK(DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {0, 2}, 2).HasError());
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CHECK(lifecycleState.createTextureCount == 0);
    }

    TEST_CASE("Multiview depth retains ordinary texture admission and exact view generations", "[unit][renderer][resource][multiview]") {
        lifecycleState = {};
        lifecycleState.completeMultiviewViews = 2;  // CPU admission/ownership proof only; no native rendering claim.
        auto frontend = CreateTrackingFrontend();
        const RenderTextureDescriptor ordinary{.extent = {2, 2},
                                               .format = RenderTextureFormat::Depth32Float,
                                               .layerCount = 2,
                                               .usage = RenderTextureUsage::RenderAttachment};
        REQUIRE(frontend->CreateTexture(ordinary).HasError());
        REQUIRE(DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 3).HasError());
        REQUIRE(DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {129, 2}, 2).HasError());
        const auto created = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2);
        REQUIRE(created.HasValue());
        CHECK(frontend->ResourceState(created.Value().handle).Value() == RenderResourceState::Pending);
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(frontend->ResourceOperationResult(created.Value().operation).HasValue());
        REQUIRE(lifecycleState.lastTextureDescriptor.has_value());
        const auto &actual = *lifecycleState.lastTextureDescriptor;
        CHECK(actual.extent == ordinary.extent);
        CHECK(actual.format == ordinary.format);
        CHECK(actual.layerCount == 2);
        CHECK(actual.usage == RenderTextureUsage::RenderAttachment);
        CHECK(actual.dimension == RenderTextureDimension::TwoD);
        CHECK(actual.mipCount == 1);
        CHECK(actual.sampleCount == 1);
        CHECK(actual.depth == 1);
        CHECK(lifecycleState.lastTextureInitialData.empty());
        const auto view = frontend->CreateTextureView({.texture = created.Value().handle,
                                                       .format = RenderTextureFormat::Depth32Float,
                                                       .aspect = RenderTextureAspect::Depth,
                                                       .baseLayer = 1});
        REQUIRE(view.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(frontend->ResourceOperationResult(view.Value().operation).HasValue());
        REQUIRE(frontend->ReleaseTextureView(view.Value().handle).HasValue());
        REQUIRE(frontend->ReleaseTexture(created.Value().handle).HasValue());
        REQUIRE(frontend->ResourceState(created.Value().handle).HasError());
        const auto replacement = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2);
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().handle != created.Value().handle);
        REQUIRE(frontend->ReleaseTexture(replacement.Value().handle).HasValue());
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    }

    TEST_CASE("Multiview depth budget denial and pending cancellation preserve canonical accounting",
              "[unit][renderer][resource][multiview]") {
        lifecycleState = {};
        lifecycleState.completeMultiviewViews = 2;
        RenderFrontendMemoryConfig memory;
        memory.budget = {.hardCapBytes = 8,
                         .defaultBlockBytes = 8,
                         .maximumBlockBytes = 8,
                         .maximumAlignment = 8,
                         .maximumPools = 2,
                         .maximumBlocks = 2,
                         .maximumReservations = 2,
                         .maximumAllocations = 2};
        auto frontend = CreateTrackingFrontend(memory);
        const auto denied = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == RenderMemoryBudgetErrors::BudgetExceeded.code.Value());
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
        CHECK(lifecycleState.createTextureCount == 0);
        const auto pending = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {1, 1}, 2);
        REQUIRE(pending.HasValue());
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 8);
        REQUIRE(frontend->ReleaseTexture(pending.Value().handle).HasValue());
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
        CHECK(frontend->UploadSnapshot().pendingRequests == 0);
        CHECK(frontend->UploadSnapshot().cancelledRequestCount == 1);
        CHECK(lifecycleState.createTextureCount == 0);
    }

    TEST_CASE("Multiview depth propagates failed realization and releases its exact memory claim",
              "[unit][renderer][resource][multiview]") {
        lifecycleState = {};
        lifecycleState.completeMultiviewViews = 2;
        auto frontend = CreateTrackingFrontend();
        const auto created = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2);
        REQUIRE(created.HasValue());
        const auto failure = MakeError(RenderMemoryBudgetErrors::BudgetExceeded, "Injected depth realization failure.");
        lifecycleState.textureCreationFailure = failure;
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        const auto realized = frontend->ResourceOperationResult(created.Value().operation);
        REQUIRE(realized.HasError());
        CHECK(realized.ErrorValue().code.Value() == failure.code.Value());
        CHECK(realized.ErrorValue().message == failure.message);
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
        CHECK(frontend->MemorySnapshot().allocationCount == 0);
        CHECK(frontend->ReleaseTexture(created.Value().handle).HasError());
        auto unknown = created.Value();
        unknown.operation = {};
        CHECK(DepthAccess::RetireCreation(*frontend, unknown).HasError());
        REQUIRE(DepthAccess::RetireCreation(*frontend, created.Value()).HasValue());
        lifecycleState.textureCreationFailure.reset();
        const auto retry = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2);
        REQUIRE(retry.HasValue());
        CHECK(retry.Value().handle != created.Value().handle);
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(DepthAccess::RetireCreation(*frontend, retry.Value()).HasValue());
        CHECK(DepthAccess::RetireCreation(*frontend, retry.Value()).HasError());
        REQUIRE(frontend->BeginShutdown().HasValue());
        REQUIRE(frontend->PollDrain().HasValue());
    }

    TEST_CASE("Multiview depth rejects frame-hot and closing mutations before native work", "[unit][renderer][resource][multiview]") {
        lifecycleState = {};
        lifecycleState.completeMultiviewViews = 2;
        auto frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {2, 2}});
        REQUIRE(begun.HasValue());
        auto frame = std::move(begun).Value();
        const auto active = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2);
        REQUIRE(active.HasError());
        CHECK(active.ErrorValue().code.Value() == "render.frontend.resource.change_during_frame");
        frame.Cancel();
        std::optional<Error> foreignError;
        std::thread foreign([&] {
            const auto result = DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2);
            if (result.HasError())
                foreignError = result.ErrorValue();
        });
        foreign.join();
        REQUIRE(foreignError.has_value());
        CHECK(foreignError->code.Value() == "render.frontend.wrong_thread");
        REQUIRE(frontend->BeginShutdown().HasValue());
        REQUIRE(DepthAccess::CreateMultiviewDepth(*frontend, {11, 1}, {2, 2}, 2).HasError());
        CHECK(lifecycleState.createTextureCount == 0);
        CHECK(frontend->MemorySnapshot().reservationCount == 0);
    }
}  // namespace
