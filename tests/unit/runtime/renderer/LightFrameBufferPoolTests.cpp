#include "Horo/Runtime/Render/LightFrameBufferPool.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderBackendRegistry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    /** @brief Records admission only; this double is not native completion or GPU parity evidence. */
    struct UploadAudit {
        bool pending{};
        std::uint64_t revision{};
        std::array<std::uint64_t, 4> instances{};
        std::size_t updates{};
        std::size_t destroys{};
    };

    class UploadBackend final : public IRenderBackend {
    public:
        UploadBackend(std::unique_ptr<IRenderBackend> backend, std::shared_ptr<UploadAudit> audit)
            : backend_(std::move(backend)), audit_(std::move(audit)) {}

        Result<void> Initialize(const RenderBackendConfig &config) override {
            auto initialized = backend_->Initialize(config);
            capabilities_ = backend_->Capabilities();
            capabilities_.backend = RenderBackendId{"light-upload-probe"};
            capabilities_.support.features.Enable(RenderCapability::LightCulling);
            capabilities_.support.limits.maxFramesInFlight = config.maxFramesInFlight;
            return initialized;
        }

        const RenderBackendCapabilities &Capabilities() const noexcept override {
            return capabilities_;
        }

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
            return backend_->QueryBufferMemoryCost(descriptor);
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
            return backend_->QueryTextureMemoryCost(descriptor);
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, std::span<const std::byte> bytes,
                                           const RenderMemoryPlacement &placement) override {
            return backend_->CreateBuffer(descriptor, bytes, placement);
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, std::uint64_t vertex, std::uint64_t index) override {
            return backend_->CreateMesh(descriptor, vertex, index);
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, std::span<const std::byte> bytes,
                                            const RenderMemoryPlacement &placement) override {
            return backend_->CreateTexture(descriptor, bytes, placement);
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, std::uint64_t texture) override {
            return backend_->CreateTextureView(descriptor, texture);
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, std::uint64_t color,
                                                 std::uint64_t depth) override {
            return backend_->CreateRenderTarget(descriptor, color, depth);
        }

        void DestroyBuffer(std::uint64_t instance) noexcept override {
            ++audit_->destroys;
            backend_->DestroyBuffer(instance);
        }

        void DestroyMesh(std::uint64_t instance) noexcept override {
            backend_->DestroyMesh(instance);
        }

        void DestroyTexture(std::uint64_t instance) noexcept override {
            backend_->DestroyTexture(instance);
        }

        void DestroyTextureView(std::uint64_t instance) noexcept override {
            backend_->DestroyTextureView(instance);
        }

        void DestroyRenderTarget(std::uint64_t instance) noexcept override {
            backend_->DestroyRenderTarget(instance);
        }

        Result<void> UpdateLightFrame(const NativeLightFrameUpdate &update) override {
            if (audit_->pending)
                return Result<void>::Failure(MakeError(LightCullingErrors::Pending));
            ++audit_->updates;
            audit_->revision = update.update.revision;
            audit_->instances = update.instances;
            return Result<void>::Success();
        }

        Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
            return backend_->BeginFrame(descriptor);
        }

        Result<void> Execute(const RenderExecutionPlan &plan) override {
            return backend_->Execute(plan);
        }

        Result<void> Present(FrameToken frame) override {
            return backend_->Present(frame);
        }

        void AbortFrame(FrameToken frame) noexcept override {
            backend_->AbortFrame(frame);
        }

        void AbortActiveFrame() noexcept override {
            backend_->AbortActiveFrame();
        }

        Result<void> Resize(FramebufferExtent extent) override {
            return backend_->Resize(extent);
        }

        void Shutdown() noexcept override {
            backend_->Shutdown();
        }

    private:
        std::unique_ptr<IRenderBackend> backend_;
        std::shared_ptr<UploadAudit> audit_;
        RenderBackendCapabilities capabilities_;
    };

    class UploadProvider final : public IRenderBackendProvider {
    public:
        explicit UploadProvider(std::shared_ptr<UploadAudit> audit) : audit_(std::move(audit)) {}

        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            RenderBackendRegistry registry;
            REQUIRE(RegisterNullRenderBackend(registry).HasValue());
            REQUIRE(registry.Seal().HasValue());
            auto backend = registry.Create(RenderBackendId{"null"});
            REQUIRE(backend.HasValue());
            return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<UploadBackend>(std::move(backend).Value(), audit_));
        }

    private:
        std::shared_ptr<UploadAudit> audit_;
    };

    std::unique_ptr<RenderFrontend> MakeUploadFrontend(const std::shared_ptr<UploadAudit> &audit) {
        RenderBackendRegistry registry;
        REQUIRE(registry
                    .Register({.id = RenderBackendId{"light-upload-probe"},
                               .displayName = "Light upload probe",
                               .provider = std::make_unique<UploadProvider>(audit)})
                    .HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto created = RenderFrontend::Create(registry, RenderBackendId{"light-upload-probe"}, {});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    std::array<PackedLightCluster, 1> Clusters() {
        std::array<PackedLightCluster, 1> clusters{};
        for (auto &plane : clusters[0].planes)
            plane = {1, 0, 0, 1};
        return clusters;
    }

    constexpr LightCullingBudget Budget{.maximumLights = 2, .maximumClusters = 1, .referencesPerCluster = 2};
}  // namespace

TEST_CASE("Light pool pending readiness and native backpressure preserve the next revision", "[renderer][light-pool]") {
    auto audit = std::make_shared<UploadAudit>();
    auto frontend = MakeUploadFrontend(audit);
    auto created = LightFrameBufferPool::Create(*frontend, Budget, 2);
    REQUIRE(created.HasValue());
    auto pool = std::move(created).Value();
    const auto clusters = Clusters();
    REQUIRE(pool->Update(0, {}, clusters).HasError());
    CHECK(audit->updates == 0);
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    audit->pending = true;
    auto pending = pool->Update(0, {}, clusters);
    REQUIRE(pending.HasError());
    CHECK(pending.ErrorValue().code.Value() == LightCullingErrors::Pending.code.Value());
    CHECK(audit->revision == 0);
    audit->pending = false;
    auto first = pool->Update(0, {}, clusters);
    REQUIRE(first.HasValue());
    CHECK(first.Value().revision == 1);
    const auto instances = audit->instances;
    auto second = pool->Update(0, {}, clusters);
    REQUIRE(second.HasValue());
    CHECK(second.Value().revision == 2);
    CHECK(audit->instances == instances);
    REQUIRE(pool->Update(1, {}, clusters).HasValue());
    CHECK(audit->revision == 1);
    CHECK(audit->instances != instances);
    REQUIRE(pool->Shutdown().HasValue());
    REQUIRE(pool->Shutdown().HasValue());
    CHECK(audit->destroys == 8);
    CHECK(pool->Update(0, {}, clusters).HasError());
}

TEST_CASE("Light pool failed frame close retains handles for owner-thread retry", "[renderer][light-pool]") {
    auto audit = std::make_shared<UploadAudit>();
    auto frontend = MakeUploadFrontend(audit);
    auto created = LightFrameBufferPool::Create(*frontend, Budget, 1);
    REQUIRE(created.HasValue());
    auto pool = std::move(created).Value();
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    {
        auto frame = frontend->BeginFrame({.frameNumber = 1, .outputExtent = {16, 16}});
        REQUIRE(frame.HasValue());
        CHECK(pool->Shutdown().HasError());
        CHECK(audit->destroys == 0);
    }
    CHECK(pool->Update(0, {}, Clusters()).HasError());
    REQUIRE(pool->Shutdown().HasValue());
    CHECK(audit->destroys == 4);
}

TEST_CASE("Light pool rejects foreign threads and excessive slots without closing", "[renderer][light-pool]") {
    auto audit = std::make_shared<UploadAudit>();
    auto frontend = MakeUploadFrontend(audit);
    CHECK(LightFrameBufferPool::Create(*frontend, Budget, 3).HasError());
    auto created = LightFrameBufferPool::Create(*frontend, Budget, 1);
    REQUIRE(created.HasValue());
    auto pool = std::move(created).Value();
    bool rejected = false;
    std::thread foreign([&] {
        rejected = pool->Shutdown().HasError() && pool->Update(0, {}, Clusters()).HasError();
    });
    foreign.join();
    CHECK(rejected);
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    REQUIRE(pool->Update(0, {}, Clusters()).HasValue());
    REQUIRE(pool->Shutdown().HasValue());
}

TEST_CASE("Headless light operations return unsupported without allocating a pool", "[renderer][light-pool]") {
    RenderBackendRegistry registry;
    REQUIRE(RegisterNullRenderBackend(registry).HasValue());
    REQUIRE(registry.Seal().HasValue());
    auto created = RenderFrontend::Create(registry, RenderBackendId{"null"}, {});
    REQUIRE(created.HasValue());
    auto frontend = std::move(created).Value();
    const auto pool = LightFrameBufferPool::Create(*frontend, Budget, 1);
    REQUIRE(pool.HasError());
    CHECK(pool.ErrorValue().code.Value() == LightCullingErrors::Unsupported.code.Value());
    CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    const auto kernel = frontend->RealizeLightCullingKernel({});
    REQUIRE(kernel.HasError());
    CHECK(kernel.ErrorValue().code.Value() == LightCullingErrors::Unsupported.code.Value());
}
