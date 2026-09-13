#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderBackendRegistry.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderMemoryBudgetErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
    class TrackingStaticMeshExecutor final : public Horo::Render::IStaticMeshPassExecutor {
    public:
        Horo::Result<void> ExecuteStaticMeshPass(const Horo::Render::StaticMeshPassDescriptor &) override {
            ++executeCount;
            return Horo::Result<void>::Success();
        }

        std::size_t executeCount{0};
    };

    using namespace Horo;
    using namespace Horo::Render;

    void Check(const bool condition) {
        REQUIRE((condition));
    }

    enum class FrameThrowPoint {
        None,
        Begin,
        Execute,
        Present,
    };

    struct BackendLifecycleState {
        int initializeCount{0};
        int shutdownCount{0};
        int abortCount{0};
        int executeCount{0};
        int presentCount{0};
        int resizeCount{0};
        int createBufferCount{0};
        int createMeshCount{0};
        int createTextureCount{0};
        int createTextureViewCount{0};
        int createRenderTargetCount{0};
        int destroyBufferCount{0};
        int destroyMeshCount{0};
        int destroyTextureCount{0};
        int destroyTextureViewCount{0};
        int destroyRenderTargetCount{0};
        std::vector<std::byte> lastBufferInitialData;
        std::vector<std::byte> lastTextureInitialData;
        bool failPresentation{false};
        bool throwDuringInitialize{false};
        bool throwDuringResize{false};
        bool failResize{false};
        bool returnInvalidFrameToken{false};
        bool failBeginAfterActivation{false};
        FrameThrowPoint frameThrowPoint{FrameThrowPoint::None};
        bool frameActive{false};
        FrameToken activeFrame{};
        std::uint64_t nextResourceInstance{1};
        bool failResourceCreation{false};
        bool throwDuringResourceCreation{false};
        bool supportsBufferResources{true};
        bool supportsMeshResources{true};
        bool supportsTextureResources{true};
        bool supportsRenderTargetResources{true};
    };

    BackendLifecycleState lifecycleState;

    class TrackingBackend final : public IRenderBackend {
    public:
        TrackingBackend()
            : capabilities_{.backend = RenderBackendId{"tracking"},
                            .supportsBufferResources = lifecycleState.supportsBufferResources,
                            .supportsMeshResources = lifecycleState.supportsMeshResources,
                            .supportsTextureResources = lifecycleState.supportsTextureResources,
                            .supportsRenderTargetResources = lifecycleState.supportsRenderTargetResources} {}

        Result<void> Initialize(const RenderBackendConfig &) override {
            ++lifecycleState.initializeCount;
            initialized_ = true;
            if (lifecycleState.throwDuringInitialize) {
                throw std::runtime_error{"Injected initialization failure."};
            }
            return Result<void>::Success();
        }

        [[nodiscard]] const RenderBackendCapabilities &Capabilities() const noexcept override {
            return capabilities_;
        }

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
            return Result<RenderMemoryCostPlan>::Success({.allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                          .compatibility = RenderMemoryCompatibilityId{1},
                                                          .payloadBytes = descriptor.byteSize,
                                                          .requiredBytes = descriptor.byteSize,
                                                          .alignment = 1});
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
            const std::size_t bytes = RenderTextureBaseLevelByteSize(descriptor).value_or(0);
            return Result<RenderMemoryCostPlan>::Success({.allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                          .compatibility = RenderMemoryCompatibilityId{2},
                                                          .payloadBytes = bytes,
                                                          .requiredBytes = bytes,
                                                          .alignment = 1});
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &, const std::span<const std::byte> initialData,
                                           const RenderMemoryPlacement &) override {
            ++lifecycleState.createBufferCount;
            lifecycleState.lastBufferInitialData.assign(initialData.begin(), initialData.end());
            if (lifecycleState.throwDuringResourceCreation) {
                throw std::runtime_error{"Injected resource creation exception."};
            }
            if (lifecycleState.failResourceCreation) {
                return Result<std::uint64_t>::Failure({ErrorCode{"render.test.resource_failed"},
                                                       ErrorDomainId{"render.test"},
                                                       ErrorSeverity::Error,
                                                       "Injected resource creation failure.",
                                                       {}});
            }
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &, std::uint64_t, std::uint64_t) override {
            ++lifecycleState.createMeshCount;
            if (lifecycleState.throwDuringResourceCreation) {
                throw std::runtime_error{"Injected resource creation exception."};
            }
            if (lifecycleState.failResourceCreation) {
                return Result<std::uint64_t>::Failure({ErrorCode{"render.test.resource_failed"},
                                                       ErrorDomainId{"render.test"},
                                                       ErrorSeverity::Error,
                                                       "Injected resource creation failure.",
                                                       {}});
            }
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &, const std::span<const std::byte> initialData,
                                            const RenderMemoryPlacement &) override {
            ++lifecycleState.createTextureCount;
            lifecycleState.lastTextureInitialData.assign(initialData.begin(), initialData.end());
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &, std::uint64_t) override {
            ++lifecycleState.createTextureViewCount;
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &, std::uint64_t, std::uint64_t) override {
            ++lifecycleState.createRenderTargetCount;
            return Result<std::uint64_t>::Success(lifecycleState.nextResourceInstance++);
        }

        void DestroyBuffer(std::uint64_t) noexcept override {
            ++lifecycleState.destroyBufferCount;
        }

        void DestroyMesh(std::uint64_t) noexcept override {
            ++lifecycleState.destroyMeshCount;
        }

        void DestroyTexture(std::uint64_t) noexcept override {
            ++lifecycleState.destroyTextureCount;
        }

        void DestroyTextureView(std::uint64_t) noexcept override {
            ++lifecycleState.destroyTextureViewCount;
        }

        void DestroyRenderTarget(std::uint64_t) noexcept override {
            ++lifecycleState.destroyRenderTargetCount;
        }

        Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
            Check(!lifecycleState.frameActive);
            if (lifecycleState.frameThrowPoint == FrameThrowPoint::Begin) {
                lifecycleState.frameActive = true;
                lifecycleState.activeFrame = FrameToken{descriptor.frameNumber};
                throw std::runtime_error{"Injected begin failure."};
            }
            if (lifecycleState.failBeginAfterActivation) {
                lifecycleState.frameActive = true;
                lifecycleState.activeFrame = FrameToken{descriptor.frameNumber};
                return Result<FrameToken>::Failure({ErrorCode{"render.test.begin_failed"},
                                                    ErrorDomainId{"render.test"},
                                                    ErrorSeverity::Error,
                                                    "Injected begin failure.",
                                                    {}});
            }
            if (lifecycleState.returnInvalidFrameToken) {
                lifecycleState.frameActive = true;
                lifecycleState.activeFrame = {};
                return Result<FrameToken>::Success({});
            }
            lifecycleState.frameActive = true;
            lifecycleState.activeFrame = FrameToken{descriptor.frameNumber};
            return Result<FrameToken>::Success(lifecycleState.activeFrame);
        }

        Result<void> Execute(const RenderExecutionPlan &plan) override {
            ++lifecycleState.executeCount;
            Check(lifecycleState.frameActive);
            Check(plan.frame == lifecycleState.activeFrame);
            if (lifecycleState.frameThrowPoint == FrameThrowPoint::Execute) {
                throw std::runtime_error{"Injected execution failure."};
            }
            return Result<void>::Success();
        }

        Result<void> Present(const FrameToken frame) override {
            ++lifecycleState.presentCount;
            Check(lifecycleState.frameActive);
            Check(frame == lifecycleState.activeFrame);
            if (lifecycleState.frameThrowPoint == FrameThrowPoint::Present) {
                throw std::runtime_error{"Injected presentation exception."};
            }
            if (lifecycleState.failPresentation) {
                return Result<void>::Failure({ErrorCode{"render.test.present_failed"},
                                              ErrorDomainId{"render.test"},
                                              ErrorSeverity::Error,
                                              "Injected presentation failure.",
                                              {}});
            }
            lifecycleState.frameActive = false;
            lifecycleState.activeFrame = {};
            return Result<void>::Success();
        }

        void AbortFrame(const FrameToken frame) noexcept override {
            if (lifecycleState.frameActive && frame == lifecycleState.activeFrame) {
                ++lifecycleState.abortCount;
                lifecycleState.frameActive = false;
                lifecycleState.activeFrame = {};
            }
        }

        void AbortActiveFrame() noexcept override {
            if (lifecycleState.frameActive) {
                ++lifecycleState.abortCount;
                lifecycleState.frameActive = false;
                lifecycleState.activeFrame = {};
            }
        }

        Result<void> Resize(FramebufferExtent) override {
            ++lifecycleState.resizeCount;
            if (lifecycleState.throwDuringResize) {
                throw std::runtime_error{"Injected resize failure."};
            }
            if (lifecycleState.failResize) {
                return Result<void>::Failure({ErrorCode{"render.test.resize_failed"},
                                              ErrorDomainId{"render.test"},
                                              ErrorSeverity::Critical,
                                              "Injected typed resize failure.",
                                              {}});
            }
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            if (initialized_) {
                ++lifecycleState.shutdownCount;
                initialized_ = false;
            }
        }

    private:
        RenderBackendCapabilities capabilities_;
        bool initialized_{false};
    };

    class TrackingBackendProvider final : public IRenderBackendProvider {
    public:
        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<TrackingBackend>());
        }
    };

    [[nodiscard]] std::unique_ptr<IRenderBackendProvider> MakeTrackingBackendProvider() {
        return std::make_unique<TrackingBackendProvider>();
    }

    static_assert(!std::is_copy_constructible_v<RenderFrameScope>);
    static_assert(!std::is_copy_assignable_v<RenderFrameScope>);
    static_assert(std::is_nothrow_move_constructible_v<RenderFrameScope>);
    static_assert(std::is_nothrow_move_assignable_v<RenderFrameScope>);
    static_assert(!std::is_same_v<RenderMeshHandle, RenderMeshSourceHandle>);

    [[nodiscard]] std::unique_ptr<RenderFrontend> CreateTrackingFrontend(const RenderFrontendMemoryConfig memoryConfig = {}) {
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());
        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{}, {}, memoryConfig);
        Check(created.HasValue());
        return std::move(created).Value();
    }

    [[nodiscard]] RenderMeshDescriptor MeshDescriptor(const RenderBufferHandle vertexBuffer, const RenderBufferHandle indexBuffer) {
        return {.vertexBuffer = vertexBuffer,
                .indexBuffer = indexBuffer,
                .vertexStride = sizeof(float) * 3,
                .vertexCount = 3,
                .indexFormat = RenderIndexFormat::UInt32,
                .indexCount = 3,
                .topology = RenderPrimitiveTopology::Triangles,
                .localBounds = {{0.0F, 0.0F, 0.0F}, {1.0F, 1.0F, 0.0F}}};
    }

    struct PendingTriangleBuffers {
        ResourceCreation<RenderBufferHandle> vertex;
        ResourceCreation<RenderBufferHandle> index;
    };

    [[nodiscard]] PendingTriangleBuffers QueueTriangleBuffers(RenderFrontend &frontend) {
        const std::array<float, 9> vertices{};
        const std::array<std::uint32_t, 3> indices{0, 1, 2};
        auto vertex = frontend.CreateBuffer({.byteSize = sizeof(vertices),
                                             .usage = RenderBufferUsage::Vertex,
                                             .access = RenderBufferAccess::DeviceLocal},
                                            std::as_bytes(std::span{vertices}));
        auto index = frontend.CreateBuffer({.byteSize = sizeof(indices),
                                            .usage = RenderBufferUsage::Index,
                                            .access = RenderBufferAccess::DeviceLocal},
                                           std::as_bytes(std::span{indices}));
        Check(vertex.HasValue());
        Check(index.HasValue());
        return {.vertex = vertex.Value(), .index = index.Value()};
    }

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
        lifecycleState.failResourceCreation = true;
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
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

    TEST_CASE("Frontend Enforces Upload Byte And Drain Budgets", "[unit][runtime][renderer][resource]") {
        lifecycleState = {};
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());
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
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());
        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{},
                                              {.maximumPendingBytes = 16,
                                               .maximumBytesPerDrain = 16,
                                               .maximumRequestsPerDrain = 2,
                                               .maximumPendingRequests = 2,
                                               .stagingOffsetAlignment = 8});
        REQUIRE(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();

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

        const auto requestFull =
            frontend->CreateTexture({.extent = {1, 1}, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::Sampled});
        REQUIRE(requestFull.HasError());
        CHECK(requestFull.ErrorValue().code.Value() == "render.frontend.resource.upload_capacity_exceeded");

        REQUIRE(frontend->ReleaseBuffer(first.Value().handle).HasValue());
        const RenderResourceUploadSnapshot compacted = frontend->UploadSnapshot();
        CHECK(compacted.pendingPayloadBytes == secondBytes.size());
        CHECK(compacted.occupiedStagingBytes == secondBytes.size());
        CHECK(compacted.pendingRequests == 1);
        CHECK(compacted.cancelledRequestCount == 1);

        const std::array<std::byte, 9> paddingOverflow{};
        const auto alignmentFull = frontend->CreateBuffer({.byteSize = paddingOverflow.size(),
                                                           .usage = RenderBufferUsage::Vertex,
                                                           .access = RenderBufferAccess::HostVisible},
                                                          paddingOverflow);
        REQUIRE(alignmentFull.HasError());
        CHECK(alignmentFull.ErrorValue().code.Value() == "render.frontend.resource.upload_capacity_exceeded");

        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(lifecycleState.lastBufferInitialData == std::vector<std::byte>(secondBytes.begin(), secondBytes.end()));
        const RenderResourceUploadSnapshot completed = frontend->UploadSnapshot();
        CHECK(completed.pendingPayloadBytes == 0);
        CHECK(completed.occupiedStagingBytes == 0);
        CHECK(completed.completedBatchCount == 1);
        CHECK(completed.lastBatchPayloadBytes == secondBytes.size());
        CHECK(completed.lastBatchRequestCount == 1);
    }

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

    TEST_CASE("Frontend Stages Execution And Presentation", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 50, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();
        Check(lifecycleState.frameActive);
        Check(lifecycleState.executeCount == 0);
        Check(lifecycleState.presentCount == 0);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
        };
        Check(frame.Execute(passes).HasValue());
        Check(lifecycleState.frameActive);
        Check(lifecycleState.executeCount == 1);
        Check(lifecycleState.presentCount == 0);
        Check(frame.Present().HasValue());
        Check(!lifecycleState.frameActive);
        Check(lifecycleState.presentCount == 1);
        Check(lifecycleState.abortCount == 0);
    }

    TEST_CASE("Frame Scope Move Transfers Abort Ownership", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        {
            auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 51, .outputExtent = {800, 600}});
            Check(begun.HasValue());
            RenderFrameScope original = std::move(begun).Value();
            RenderFrameScope moved = std::move(original);
            Check(lifecycleState.frameActive);
            (void)moved;
        }
        Check(!lifecycleState.frameActive);
        Check(lifecycleState.abortCount == 1);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{2}, .kind = RenderPassKind::Graphics},
        };
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 52, .outputExtent = {800, 600}}, passes).HasValue());
    }

    TEST_CASE("Frame Scope Rejects Invalid Stage Transitions Without Losing Recovery", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 53, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        const Result<void> earlyPresent = frame.Present();
        Check(earlyPresent.HasError());
        Check(earlyPresent.ErrorValue().code.Value() == "render.frontend.frame_not_executed");
        Check(lifecycleState.frameActive);
        Check(lifecycleState.abortCount == 0);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{3}, .kind = RenderPassKind::Graphics},
        };
        Check(frame.Execute(passes).HasValue());
        const Result<void> duplicateExecute = frame.Execute(passes);
        Check(duplicateExecute.HasError());
        Check(duplicateExecute.ErrorValue().code.Value() == "render.frontend.frame_already_executed");
        Check(lifecycleState.executeCount == 1);
        Check(frame.Present().HasValue());
        Check(!lifecycleState.frameActive);
    }

    TEST_CASE("Frontend Rejects A Second Frame Without Aborting The Owned Scope", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 54, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        const auto duplicate = frontend->BeginFrame(FrameDescriptor{.frameNumber = 55, .outputExtent = {800, 600}});
        Check(duplicate.HasError());
        Check(duplicate.ErrorValue().code.Value() == "render.frontend.frame_already_active");
        Check(lifecycleState.frameActive);
        Check(lifecycleState.abortCount == 0);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{4}, .kind = RenderPassKind::Graphics},
        };
        Check(frame.Execute(passes).HasValue());
        Check(frame.Present().HasValue());
    }

    TEST_CASE("Frontend Destruction Aborts And Invalidates An Outstanding Scope", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 56, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        frontend.reset();
        Check(lifecycleState.abortCount == 1);
        Check(lifecycleState.shutdownCount == 1);
        Check(!lifecycleState.frameActive);

        const Result<void> inactive = frame.Present();
        Check(inactive.HasError());
        Check(inactive.ErrorValue().code.Value() == "render.frontend.frame_not_active");
    }

    TEST_CASE("Frame Scope Move Assignment Aborts The Previous Frame And Transfers Ownership", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> trackingFrontend = CreateTrackingFrontend();
        auto trackingBegun = trackingFrontend->BeginFrame(FrameDescriptor{.frameNumber = 57, .outputExtent = {800, 600}});
        Check(trackingBegun.HasValue());
        RenderFrameScope destination = std::move(trackingBegun).Value();

        RenderBackendRegistry nullRegistry;
        Check(RegisterNullRenderBackend(nullRegistry).HasValue());
        Check(nullRegistry.Seal().HasValue());
        auto nullCreated = RenderFrontend::Create(nullRegistry, RenderBackendId{"null"}, RenderBackendConfig{});
        Check(nullCreated.HasValue());
        std::unique_ptr<RenderFrontend> nullFrontend = std::move(nullCreated).Value();
        auto nullBegun = nullFrontend->BeginFrame(FrameDescriptor{.frameNumber = 58, .outputExtent = {800, 600}});
        Check(nullBegun.HasValue());
        RenderFrameScope source = std::move(nullBegun).Value();

        destination = std::move(source);
        Check(lifecycleState.abortCount == 1);
        Check(!lifecycleState.frameActive);
        const Result<void> movedFrom = source.Present();
        Check(movedFrom.HasError());
        Check(movedFrom.ErrorValue().code.Value() == "render.frontend.frame_not_active");

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{5}, .kind = RenderPassKind::Graphics},
        };
        Check(destination.Execute(passes).HasValue());
        Check(destination.Present().HasValue());
    }

    TEST_CASE("Frame Scope Supports Self Move And Move After Execute", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 59, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        frame = std::move(frame);
        Check(lifecycleState.frameActive);
        Check(lifecycleState.abortCount == 0);

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{6}, .kind = RenderPassKind::Graphics},
        };
        Check(frame.Execute(passes).HasValue());
        RenderFrameScope moved = std::move(frame);
        Check(moved.Present().HasValue());
        Check(lifecycleState.executeCount == 1);
        Check(lifecycleState.presentCount == 1);
        Check(lifecycleState.abortCount == 0);
    }

    TEST_CASE("Frontend Rejects Resize During Active Frame And Supports Explicit Cancel", "[unit][runtime][renderer]") {
        lifecycleState = {};
        std::unique_ptr<RenderFrontend> frontend = CreateTrackingFrontend();
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 60, .outputExtent = {800, 600}});
        Check(begun.HasValue());
        RenderFrameScope frame = std::move(begun).Value();

        Check(frontend->Capabilities().backend == RenderBackendId{"tracking"});
        const Result<void> resize = frontend->Resize(FramebufferExtent{1024, 768});
        Check(resize.HasError());
        Check(resize.ErrorValue().code.Value() == "render.frontend.resize_during_frame");
        Check(lifecycleState.resizeCount == 0);
        Check(lifecycleState.frameActive);

        frame.Cancel();
        Check(lifecycleState.abortCount == 1);
        Check(!lifecycleState.frameActive);
        frame.Cancel();
        Check(lifecycleState.abortCount == 1);
        Check(frontend->Resize(FramebufferExtent{1024, 768}).HasValue());
        Check(lifecycleState.resizeCount == 1);
    }

    TEST_CASE("Frontend Initializes And Owns Selected Backend Lifetime", "[unit][runtime][renderer]") {
        lifecycleState = {};
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        Check(lifecycleState.initializeCount == 1);

        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        Check(frontend->Capabilities().backend == RenderBackendId{"tracking"});
        frontend.reset();

        Check(lifecycleState.shutdownCount == 1);
    }

    TEST_CASE("Frontend Submits Frames And Recovers After Plan Failure", "[unit][runtime][renderer]") {
        RenderBackendRegistry registry;
        Check(RegisterNullRenderBackend(registry).HasValue());
        Check(registry.Seal().HasValue());

        auto created = RenderFrontend::Create(registry, RenderBackendId{"null"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();

        const std::array validPasses{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
            RenderPassDescriptor{.id = RenderPassId{2}, .kind = RenderPassKind::Copy},
        };
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 1, .outputExtent = {1280, 720}}, validPasses).HasValue());

        const std::array unsupportedPasses{
            RenderPassDescriptor{.id = RenderPassId{3}, .kind = RenderPassKind::Compute},
        };
        const Result<void> unsupported =
            frontend->SubmitFrame(FrameDescriptor{.frameNumber = 2, .outputExtent = {1280, 720}}, unsupportedPasses);
        Check(unsupported.HasError());
        Check(unsupported.ErrorValue().code.Value() == "render.backend.unsupported_pass_kind");

        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 3, .outputExtent = {1280, 720}}, validPasses).HasValue());
    }

    TEST_CASE("Frontend Recovers After Presentation Failure", "[unit][runtime][renderer]") {
        lifecycleState = {};
        lifecycleState.failPresentation = true;
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();

        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
        };
        const Result<void> failed = frontend->SubmitFrame(FrameDescriptor{.frameNumber = 10, .outputExtent = {640, 480}}, passes);
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.test.present_failed");
        Check(lifecycleState.abortCount == 1);

        lifecycleState.failPresentation = false;
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 11, .outputExtent = {640, 480}}, passes).HasValue());
    }

    TEST_CASE("Frontend Contains Initialization Exceptions And Cleans Up Partial State", "[unit][runtime][renderer]") {
        lifecycleState = {};
        lifecycleState.throwDuringInitialize = true;
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasError());
        Check(created.ErrorValue().code.Value() == "render.frontend.initialize_exception");
        Check(lifecycleState.initializeCount == 1);
        Check(lifecycleState.shutdownCount == 1);
    }

    TEST_CASE("Frontend Contains Frame Exceptions And Recovers Owned Frame State", "[unit][runtime][renderer]") {
        constexpr std::array throwPoints{FrameThrowPoint::Begin, FrameThrowPoint::Execute, FrameThrowPoint::Present};
        for (const FrameThrowPoint throwPoint : throwPoints) {
            lifecycleState = {};
            lifecycleState.frameThrowPoint = throwPoint;
            RenderBackendRegistry registry;
            Check(registry
                      .Register(RenderBackendDescriptor{
                          .id = RenderBackendId{"tracking"},
                          .displayName = "Tracking",
                          .provider = MakeTrackingBackendProvider(),
                      })
                      .HasValue());
            Check(registry.Seal().HasValue());

            auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
            Check(created.HasValue());
            std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
            const std::array passes{
                RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
            };

            const Result<void> failed = frontend->SubmitFrame(FrameDescriptor{.frameNumber = 20, .outputExtent = {640, 480}}, passes);
            Check(failed.HasError());
            Check(failed.ErrorValue().code.Value() == "render.frontend.frame_exception");
            Check(lifecycleState.abortCount == 1);

            lifecycleState.frameThrowPoint = FrameThrowPoint::None;
            Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 21, .outputExtent = {640, 480}}, passes).HasValue());
        }
    }

    TEST_CASE("Frontend Owns Resize Boundary And Contains Backend Exceptions", "[unit][runtime][renderer]") {
        lifecycleState = {};
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        Check(frontend->Resize(FramebufferExtent{1920, 1080}).HasValue());
        Check(lifecycleState.resizeCount == 1);

        lifecycleState.failResize = true;
        const Result<void> typedFailure = frontend->Resize(FramebufferExtent{1600, 900});
        Check(typedFailure.HasError());
        Check(typedFailure.ErrorValue().code.Value() == "render.test.resize_failed");
        Check(typedFailure.ErrorValue().domain.Value() == "render.test");
        Check(typedFailure.ErrorValue().severity == ErrorSeverity::Critical);
        Check(typedFailure.ErrorValue().message == "Injected typed resize failure.");

        lifecycleState.failResize = false;
        lifecycleState.throwDuringResize = true;
        const Result<void> failed = frontend->Resize(FramebufferExtent{1280, 720});
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.frontend.resize_exception");

        lifecycleState.throwDuringResize = false;
        Check(frontend->Resize(FramebufferExtent{1280, 720}).HasValue());
    }

    TEST_CASE("Frontend Rejects Invalid Successful Frame Tokens And Recovers Backend State", "[unit][runtime][renderer]") {
        lifecycleState = {};
        lifecycleState.returnInvalidFrameToken = true;
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
        };

        const Result<void> failed = frontend->SubmitFrame(FrameDescriptor{.frameNumber = 30, .outputExtent = {640, 480}}, passes);
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.frontend.invalid_frame_token");
        Check(lifecycleState.abortCount == 1);

        lifecycleState.returnInvalidFrameToken = false;
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 31, .outputExtent = {640, 480}}, passes).HasValue());
    }

    TEST_CASE("Frontend Preserves Typed Begin Failures And Recovers Partial Backend State", "[unit][runtime][renderer]") {
        lifecycleState = {};
        lifecycleState.failBeginAfterActivation = true;
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{
                      .id = RenderBackendId{"tracking"},
                      .displayName = "Tracking",
                      .provider = MakeTrackingBackendProvider(),
                  })
                  .HasValue());
        Check(registry.Seal().HasValue());

        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();
        const std::array passes{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics},
        };

        const Result<void> failed = frontend->SubmitFrame(FrameDescriptor{.frameNumber = 40, .outputExtent = {640, 480}}, passes);
        Check(failed.HasError());
        Check(failed.ErrorValue().code.Value() == "render.test.begin_failed");
        Check(failed.ErrorValue().domain.Value() == "render.test");
        Check(failed.ErrorValue().severity == ErrorSeverity::Error);
        Check(lifecycleState.abortCount == 1);

        lifecycleState.failBeginAfterActivation = false;
        Check(frontend->SubmitFrame(FrameDescriptor{.frameNumber = 41, .outputExtent = {640, 480}}, passes).HasValue());
    }

    TEST_CASE("Frontend Owns Static Mesh Executor Attachment And Rejects Missing Execution", "[unit][runtime][renderer]") {
        lifecycleState = {};
        RenderBackendRegistry registry;
        Check(registry
                  .Register(RenderBackendDescriptor{.id = RenderBackendId{"tracking"},
                                                    .displayName = "Tracking",
                                                    .provider = MakeTrackingBackendProvider()})
                  .HasValue());
        Check(registry.Seal().HasValue());
        auto created = RenderFrontend::Create(registry, RenderBackendId{"tracking"}, RenderBackendConfig{});
        Check(created.HasValue());
        std::unique_ptr<RenderFrontend> frontend = std::move(created).Value();

        const std::array staticPass{
            RenderPassDescriptor{.id = RenderPassId{1}, .kind = RenderPassKind::Graphics, .staticMesh = StaticMeshPassDescriptor{}}};
        auto begun = frontend->BeginFrame(FrameDescriptor{.frameNumber = 50, .outputExtent = {64, 64}});
        Check(begun.HasValue());
        RenderFrameScope missingExecutorFrame = std::move(begun).Value();
        const Result<void> missing = missingExecutorFrame.Execute(staticPass);
        Check(missing.HasError());
        Check(missing.ErrorValue().code.Value() == "render.frontend.static_mesh_executor_missing");

        TrackingStaticMeshExecutor first;
        TrackingStaticMeshExecutor second;
        Check(frontend->AttachStaticMeshPassExecutor(first).HasValue());
        const Result<void> duplicate = frontend->AttachStaticMeshPassExecutor(second);
        Check(duplicate.HasError());
        Check(duplicate.ErrorValue().code.Value() == "render.frontend.static_mesh_executor_already_attached");
        frontend->DetachStaticMeshPassExecutor(first);
        Check(frontend->AttachStaticMeshPassExecutor(second).HasValue());
        frontend->DetachStaticMeshPassExecutor(second);

        const Result<RenderTargetHandle> invalidTarget = frontend->CreateOffscreenTarget({});
        Check(invalidTarget.HasError());
        auto target = frontend->CreateOffscreenTarget({32, 24});
        Check(target.HasValue());
        Check(frontend->ResizeOffscreenTarget(target.Value(), {64, 48}).HasValue());
        const Result<void> invalidResize = frontend->ResizeOffscreenTarget(target.Value(), {});
        Check(invalidResize.HasError());
        Check(invalidResize.ErrorValue().code.Value() == "render.frontend.invalid_target_extent");
        Check(frontend->ReleaseOffscreenTarget(target.Value()).HasValue());
        const Result<void> staleResize = frontend->ResizeOffscreenTarget(target.Value(), {16, 16});
        Check(staleResize.HasError());
        Check(staleResize.ErrorValue().code.Value() == "render.frontend.resource.stale");
        auto replacement = frontend->CreateOffscreenTarget({16, 16});
        Check(replacement.HasValue());
        Check(replacement.Value().owner == target.Value().owner);
        Check(replacement.Value().slot == target.Value().slot);
        Check(replacement.Value().generation == target.Value().generation + 1);
    }
}  // namespace
