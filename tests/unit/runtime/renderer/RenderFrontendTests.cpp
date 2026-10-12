#include "RenderFrontendTestSupport.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;

    TEST_CASE("Frontend Publishes Bounded Buffer And Mesh Uploads", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const PendingTriangleBuffers buffers = QueueTriangleBuffers(*frontend);

        Check(frontend->ResourceState(buffers.vertex.handle).Value() == RenderResourceState::Pending);
        Check(frontend->ResourceOperationResult(buffers.vertex.operation).HasError());
        auto earlyMesh = frontend->CreateMesh(MeshDescriptor(buffers.vertex.handle, buffers.index.handle));
        Check(earlyMesh.HasError());
        Check(earlyMesh.ErrorValue().code.Value() == "render.frontend.resource.dependency_not_ready");

        const auto buffersProcessed = frontend->ProcessResourceRequests();
        Check(buffersProcessed.HasValue());
        Check(buffersProcessed.Value() == 2);
        Check(lifecycleState.createBufferCount == 2);
        Check(frontend->ResourceOperationResult(buffers.vertex.operation).HasValue());
        Check(frontend->ResourceState(buffers.index.handle).Value() == RenderResourceState::Ready);

        auto mesh = frontend->CreateMesh(MeshDescriptor(buffers.vertex.handle, buffers.index.handle));
        Check(mesh.HasValue());
        Check(frontend->ResourceState(mesh.Value().handle).Value() == RenderResourceState::Pending);
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(lifecycleState.createMeshCount == 1);
        Check(frontend->ResourceState(mesh.Value().handle).Value() == RenderResourceState::Ready);

        Check(frontend->ReleaseBuffer(buffers.vertex.handle).HasValue());
        Check(frontend->ReleaseBuffer(buffers.index.handle).HasValue());
        Check(lifecycleState.destroyBufferCount == 0);
        Check(frontend->ReleaseMesh(mesh.Value().handle).HasValue());
        Check(lifecycleState.destroyMeshCount == 1);
        Check(lifecycleState.destroyBufferCount == 2);
    }

    TEST_CASE("Frontend admits native backing by explicit scope and reclaims it after destruction",
              "[unit][runtime][renderer][resource][memory]") {
        lifecycleState = {};
        RenderFrontendMemoryConfig memoryConfig;
        memoryConfig.budget = {.hardCapBytes = 64,
                               .defaultBlockBytes = 64,
                               .maximumBlockBytes = 64,
                               .maximumAlignment = 32,
                               .maximumPools = 4,
                               .maximumBlocks = 4,
                               .maximumReservations = 4,
                               .maximumAllocations = 4};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend(memoryConfig);
        const std::array<std::byte, 12> bytes{};
        auto first = frontend->CreateBuffer({11, 1},
                                            {.byteSize = bytes.size(),
                                             .usage = RenderBufferUsage::Vertex,
                                             .access = RenderBufferAccess::DeviceLocal},
                                            bytes);
        auto second =
            frontend->CreateBuffer({12, 1},
                                   {.byteSize = bytes.size(), .usage = RenderBufferUsage::Index, .access = RenderBufferAccess::DeviceLocal},
                                   bytes);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 24);
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        const auto committed = frontend->MemorySnapshot();
        CHECK(committed.committedBackingBytes == 24);
        CHECK(committed.livePayloadBytes == 24);
        CHECK(committed.poolCount == 2);

        REQUIRE(frontend->ReleaseBuffer(first.Value().handle).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 12);
        REQUIRE(frontend->ReleaseBuffer(second.Value().handle).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
        CHECK(lifecycleState.destroyBufferCount == 2);
    }

    TEST_CASE("Frontend memory denial and pending cancellation never reach native allocation",
              "[unit][runtime][renderer][resource][memory]") {
        lifecycleState = {};
        RenderFrontendMemoryConfig memoryConfig;
        memoryConfig.budget = {.hardCapBytes = 8,
                               .defaultBlockBytes = 8,
                               .maximumBlockBytes = 8,
                               .maximumAlignment = 8,
                               .maximumPools = 2,
                               .maximumBlocks = 2,
                               .maximumReservations = 2,
                               .maximumAllocations = 2};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend(memoryConfig);
        const std::array<std::byte, 12> tooLarge{};
        const auto denied = frontend->CreateBuffer({11, 1},
                                                   {.byteSize = tooLarge.size(),
                                                    .usage = RenderBufferUsage::Vertex,
                                                    .access = RenderBufferAccess::DeviceLocal},
                                                   tooLarge);
        REQUIRE(denied.HasError());
        CHECK(denied.ErrorValue().code.Value() == RenderMemoryBudgetErrors::BudgetExceeded.code.Value());
        CHECK(lifecycleState.createBufferCount == 0);
        CHECK(frontend->MemorySnapshot().reservationCount == 0);

        const std::array<std::byte, 4> admittedBytes{};
        auto admitted = frontend->CreateBuffer({11, 1},
                                               {.byteSize = admittedBytes.size(),
                                                .usage = RenderBufferUsage::Vertex,
                                                .access = RenderBufferAccess::DeviceLocal},
                                               admittedBytes);
        REQUIRE(admitted.HasValue());
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 4);
        REQUIRE(frontend->ReleaseBuffer(admitted.Value().handle).HasValue());
        CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
        CHECK(frontend->ProcessResourceRequests().Value() == 0);
        CHECK(lifecycleState.createBufferCount == 0);
    }

    TEST_CASE("Frontend native allocation failure cancels its exact memory claim", "[unit][runtime][renderer][resource][memory]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const std::array<std::byte, 12> bytes{};
        auto buffer = frontend->CreateBuffer({11, 1},
                                             {.byteSize = bytes.size(),
                                              .usage = RenderBufferUsage::Vertex,
                                              .access = RenderBufferAccess::DeviceLocal},
                                             bytes);
        REQUIRE(buffer.HasValue());
        REQUIRE(frontend->ResourceOperationState(buffer.Value().operation).HasValue());
        CHECK(frontend->ResourceOperationState(buffer.Value().operation).Value() == RenderResourceOperationState::Pending);
        lifecycleState.failResourceCreation = true;
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(frontend->ResourceOperationState(buffer.Value().operation).HasValue());
        CHECK(frontend->ResourceOperationState(buffer.Value().operation).Value() == RenderResourceOperationState::Failed);
        CHECK(frontend->ResourceOperationState({}).HasError());
        CHECK(frontend->ResourceOperationResult(buffer.Value().operation).HasError());
        const auto snapshot = frontend->MemorySnapshot();
        CHECK(snapshot.reservedUnallocatedBytes == 0);
        CHECK(snapshot.committedBackingBytes == 0);
        CHECK(snapshot.reservationCount == 0);
        CHECK(snapshot.allocationCount == 0);
    }

    TEST_CASE("Frontend Accepts Absent Buffer Initial Data And Copies Present Bytes", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const RenderBufferDescriptor descriptor{
            .byteSize = 4,
            .usage = RenderBufferUsage::Vertex,
            .access = RenderBufferAccess::DeviceLocal,
        };

        const auto withoutInitialData = frontend->CreateBuffer(descriptor, {});
        Check(withoutInitialData.HasValue());
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(lifecycleState.lastBufferInitialData.empty());

        std::array<std::byte, 4> bytes{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
        const auto withInitialData = frontend->CreateBuffer(descriptor, bytes);
        Check(withInitialData.HasValue());
        bytes[0] = std::byte{0xFF};
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(lifecycleState.lastBufferInitialData.front() == std::byte{0x12});

        const auto wrongSize = frontend->CreateBuffer(descriptor, std::span{bytes}.first<3>());
        Check(wrongSize.HasError());
        Check(wrongSize.ErrorValue().code.Value() == "render.frontend.resource.buffer_upload_size_mismatch");
    }

    TEST_CASE("Frontend Publishes Texture Views And Targets With Dependency-Pinned Retirement", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const RenderTextureDescriptor colorDescriptor{.extent = {320, 180},
                                                      .format = RenderTextureFormat::Rgba8Unorm,
                                                      .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment};
        const RenderTextureDescriptor depthDescriptor{.extent = {320, 180},
                                                      .format = RenderTextureFormat::Depth24Stencil8,
                                                      .usage = RenderTextureUsage::RenderAttachment};
        auto color = frontend->CreateTexture(colorDescriptor);
        auto depth = frontend->CreateTexture(depthDescriptor);
        Check(color.HasValue() && depth.HasValue());
        Check(frontend->CreateTextureView({}).ErrorValue().code.Value() == "render.frontend.resource.invalid_texture_view_descriptor");
        Check(frontend
                  ->CreateTextureView(
                      {.texture = color.Value().handle, .format = RenderTextureFormat::Rgba8Unorm, .aspect = RenderTextureAspect::Color})
                  .ErrorValue()
                  .code.Value() == "render.frontend.resource.dependency_not_ready");
        Check(frontend->ProcessResourceRequests().Value() == 2);

        auto colorView = frontend->CreateTextureView(
            {.texture = color.Value().handle, .format = RenderTextureFormat::Rgba8Unorm, .aspect = RenderTextureAspect::Color});
        auto depthView = frontend->CreateTextureView(
            {.texture = depth.Value().handle, .format = RenderTextureFormat::Depth24Stencil8, .aspect = RenderTextureAspect::DepthStencil});
        Check(colorView.HasValue() && depthView.HasValue());
        Check(frontend->ProcessResourceRequests().Value() == 2);
        auto target = frontend->CreateRenderTarget(
            {.colorAttachment = colorView.Value().handle, .depthAttachment = depthView.Value().handle, .extent = {320, 180}});
        Check(target.HasValue());
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(frontend->ResourceState(target.Value().handle).Value() == RenderResourceState::Ready);

        Check(frontend->ReleaseTexture(color.Value().handle).HasValue());
        Check(frontend->ReleaseTextureView(colorView.Value().handle).HasValue());
        Check(lifecycleState.destroyTextureCount == 0);
        Check(lifecycleState.destroyTextureViewCount == 0);
        Check(frontend->ReleaseRenderTarget(target.Value().handle).HasValue());
        Check(lifecycleState.destroyRenderTargetCount == 1);
        Check(lifecycleState.destroyTextureViewCount == 1);
        Check(lifecycleState.destroyTextureCount == 1);
        Check(frontend->ReleaseTextureView(depthView.Value().handle).HasValue());
        Check(frontend->ReleaseTexture(depth.Value().handle).HasValue());
        Check(lifecycleState.destroyTextureViewCount == 2);
        Check(lifecycleState.destroyTextureCount == 2);
    }

    TEST_CASE("Frontend validates and preserves complete texture base-level uploads", "[unit][runtime][renderer][resource][memory]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const RenderTextureDescriptor descriptor{.extent = {2, 2},
                                                 .format = RenderTextureFormat::Rgba8Unorm,
                                                 .usage = RenderTextureUsage::Sampled};
        const std::array<std::byte, 16> pixels{std::byte{0x2a}};
        const auto truncated = frontend->CreateTexture(descriptor, std::span{pixels}.first<15>());
        REQUIRE(truncated.HasError());
        CHECK(truncated.ErrorValue().code.Value() == "render.frontend.resource.invalid_texture_descriptor");

        auto texture = frontend->CreateTexture({102, 1}, descriptor, pixels);
        REQUIRE(texture.HasValue());
        CHECK(frontend->MemorySnapshot().reservedPayloadBytes == pixels.size());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(frontend->ResourceOperationResult(texture.Value().operation).HasValue());
        CHECK(lifecycleState.lastTextureInitialData == std::vector<std::byte>(pixels.begin(), pixels.end()));
        CHECK(frontend->MemorySnapshot().livePayloadBytes == pixels.size());
        REQUIRE(frontend->ReleaseTexture(texture.Value().handle).HasValue());
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    }

    TEST_CASE("Frontend Cancels Pending Resource Generations Without Leaking Late Realization", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const std::array<std::byte, 16> bytes{};
        auto buffer = frontend->CreateBuffer({.byteSize = bytes.size(),
                                              .usage = RenderBufferUsage::Vertex,
                                              .access = RenderBufferAccess::DeviceLocal},
                                             bytes);
        Check(buffer.HasValue());
        Check(frontend->ReleaseBuffer(buffer.Value().handle).HasValue());
        const Result<void> completion = frontend->ResourceOperationResult(buffer.Value().operation);
        Check(completion.HasError());
        Check(completion.ErrorValue().code.Value() == "render.frontend.resource.operation_cancelled");
        Check(frontend->ProcessResourceRequests().Value() == 0);
        Check(lifecycleState.createBufferCount == 0);
        Check(lifecycleState.destroyBufferCount == 0);
        Check(frontend->ResourceState(buffer.Value().handle).ErrorValue().code.Value() == "render.frontend.resource.stale");
    }

    TEST_CASE("Frontend Mesh Replacement Preserves The Old Generation On Failure", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        const PendingTriangleBuffers buffers = QueueTriangleBuffers(*frontend);
        Check(frontend->ProcessResourceRequests().Value() == 2);
        auto original = frontend->CreateMesh(MeshDescriptor(buffers.vertex.handle, buffers.index.handle));
        Check(original.HasValue());
        Check(frontend->ProcessResourceRequests().Value() == 1);

        lifecycleState.failResourceCreation = true;
        auto failedReplacement =
            frontend->ReplaceMesh(original.Value().handle, MeshDescriptor(buffers.vertex.handle, buffers.index.handle));
        Check(failedReplacement.HasValue());
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(frontend->ResourceOperationResult(failedReplacement.Value().operation).HasError());
        Check(frontend->ResourceOperationResult(failedReplacement.Value().operation).ErrorValue().code.Value() ==
              "render.test.resource_failed");
        Check(frontend->ResourceState(original.Value().handle).Value() == RenderResourceState::Ready);

        lifecycleState.failResourceCreation = false;
        auto replacement = frontend->ReplaceMesh(original.Value().handle, MeshDescriptor(buffers.vertex.handle, buffers.index.handle));
        Check(replacement.HasValue());
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(frontend->ResourceState(replacement.Value().handle).Value() == RenderResourceState::Ready);
        const auto staleOriginal = frontend->ResourceState(original.Value().handle);
        Check(staleOriginal.HasError());
        Check(staleOriginal.ErrorValue().code.Value() == "render.frontend.resource.stale");
        Check(lifecycleState.destroyMeshCount == 1);
    }

    /** @brief Checks independent invalid upload, memory and retirement configurations. */
    void VerifyInvalidFrontendLimits(RenderBackendRegistry &registry) {
        auto invalid = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{},
                                              {.maximumPendingBytes = 8, .maximumBytesPerDrain = 16, .maximumRequestsPerDrain = 1});
        Check(invalid.HasError());
        Check(invalid.ErrorValue().code.Value() == "render.frontend.resource.invalid_upload_limits");
        auto invalidAlignment = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{},
                                                       {.maximumPendingBytes = 16,
                                                        .maximumBytesPerDrain = 8,
                                                        .maximumRequestsPerDrain = 2,
                                                        .stagingOffsetAlignment = 3});
        Check(invalidAlignment.HasError());
        Check(invalidAlignment.ErrorValue().code.Value() == "render.frontend.resource.invalid_upload_limits");
        auto invalidRequestBounds = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{},
                                                           {.maximumPendingBytes = 16,
                                                            .maximumBytesPerDrain = 8,
                                                            .maximumRequestsPerDrain = 2,
                                                            .maximumPendingRequests = 1});
        Check(invalidRequestBounds.HasError());
        Check(invalidRequestBounds.ErrorValue().code.Value() == "render.frontend.resource.invalid_upload_limits");

        RenderFrontendMemoryConfig invalidMemory;
        invalidMemory.budget.hardCapBytes = 0;
        const auto invalidMemoryFrontend =
            RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{}, {}, invalidMemory);
        Check(invalidMemoryFrontend.HasError());
        Check(invalidMemoryFrontend.ErrorValue().code.Value() == "render.frontend.memory.invalid_config");

        RenderResourceRetirementLimits invalidRetirement;
        invalidRetirement.maximumTrackedQueues = 0;
        const auto invalidRetirementFrontend =
            RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{}, {}, {}, invalidRetirement);
        Check(invalidRetirementFrontend.HasError());
        Check(invalidRetirementFrontend.ErrorValue().code.Value() == "render.frontend.resource.invalid_retirement_limits");
    }

    TEST_CASE("Frontend Enforces Upload Byte And Drain Budgets", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        RenderBackendRegistry registry;
        RegisterTrackingBackend(registry);
        VerifyInvalidFrontendLimits(registry);

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{},
                                              {.maximumPendingBytes = 16, .maximumBytesPerDrain = 8, .maximumRequestsPerDrain = 4});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        const std::array<std::byte, 8> bytes{};
        auto first = frontend->CreateBuffer({.byteSize = bytes.size(),
                                             .usage = RenderBufferUsage::Vertex,
                                             .access = RenderBufferAccess::HostVisible},
                                            bytes);
        auto second =
            frontend->CreateBuffer({.byteSize = bytes.size(), .usage = RenderBufferUsage::Index, .access = RenderBufferAccess::HostVisible},
                                   bytes);
        Check(first.HasValue());
        Check(second.HasValue());
        const auto queueFull = frontend->CreateBuffer({.byteSize = bytes.size(),
                                                       .usage = RenderBufferUsage::Vertex,
                                                       .access = RenderBufferAccess::HostVisible},
                                                      bytes);
        Check(queueFull.HasError());
        Check(queueFull.ErrorValue().code.Value() == "render.frontend.resource.upload_capacity_exceeded");
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(frontend->ResourceState(first.Value().handle).Value() == RenderResourceState::Ready);
        Check(frontend->ResourceState(second.Value().handle).Value() == RenderResourceState::Pending);
        Check(frontend->ProcessResourceRequests().Value() == 1);

        const std::array<std::byte, 12> oversizedForDrain{};
        const auto admitted = frontend->CreateBuffer({.byteSize = oversizedForDrain.size(),
                                                      .usage = RenderBufferUsage::Vertex,
                                                      .access = RenderBufferAccess::DeviceLocal},
                                                     oversizedForDrain);
        Check(admitted.HasValue());
        Check(frontend->ProcessResourceRequests().Value() == 1);
        Check(frontend->ResourceState(admitted.Value().handle).Value() == RenderResourceState::Ready);
        const RenderResourceUploadSnapshot drained = frontend->UploadSnapshot();
        Check(drained.pendingPayloadBytes == 0);
        Check(drained.pendingRequests == 0);
        Check(drained.completedBatchCount == 3);
        Check(drained.lastBatchPayloadBytes == oversizedForDrain.size());
        Check(drained.lastBatchRequestCount == 1);
        Check(drained.acceptingRequests);
    }

    TEST_CASE("Frontend Upload Arena Aligns Reclaims And Preserves Staged Bytes", "[unit][runtime][renderer][resource][upload]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend({}, {.maximumPendingBytes = 16,
                                                                               .maximumBytesPerDrain = 16,
                                                                               .maximumRequestsPerDrain = 2,
                                                                               .maximumPendingRequests = 2,
                                                                               .stagingOffsetAlignment = 8});

        const std::array firstBytes{std::byte{0x11}, std::byte{0x12}, std::byte{0x13}};
        const std::array secondBytes{std::byte{0x21}, std::byte{0x22}, std::byte{0x23}};
        auto first = frontend->CreateBuffer({.byteSize = firstBytes.size(),
                                             .usage = RenderBufferUsage::Vertex,
                                             .access = RenderBufferAccess::HostVisible},
                                            firstBytes);
        auto second = frontend->CreateBuffer({.byteSize = secondBytes.size(),
                                              .usage = RenderBufferUsage::Vertex,
                                              .access = RenderBufferAccess::HostVisible},
                                             secondBytes);
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        const RenderResourceUploadSnapshot aligned = frontend->UploadSnapshot();
        CHECK(aligned.pendingPayloadBytes == 6);
        CHECK(aligned.occupiedStagingBytes == 11);
        CHECK(aligned.pendingRequests == 2);

        SECTION("Rejects requests after reaching the request limit") {
            VerifyUploadRequestLimit(*frontend);
        }

        SECTION("Compacts once after cancellation and preserves staged bytes") {
            VerifyUploadCompaction(*frontend, first.Value().handle, secondBytes);
        }
    }

}  // namespace
