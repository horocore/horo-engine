#pragma once

#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "support/AllocationProbe.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Horo::Render::TransientTest {
    /** @brief Native admission and lifetime evidence observed at the real headless backend boundary. */
    struct ResourceAudit {
        std::size_t queries{};
        std::size_t creates{};
        std::size_t destroyed{};
        std::size_t queriesAtFirstCreate{};
    };

    /** @brief Controlled failure points exercised through the normal public frontend. */
    enum class QueryFaultKind : std::uint8_t {
        TypedFailure,
        Allocation,
        Length,
        RollbackAllocation,
    };

    /** @brief Controlled failure points exercised through the normal public frontend. */
    struct Faults {
        std::size_t failCreate{};
        std::size_t failQuery{};
        QueryFaultKind queryFault{QueryFaultKind::TypedFailure};
        bool delayCompletion{};
        bool failAfterTransfer{};
        bool supportsReuse{true};
    };

    /** @brief Test-owned observation storage; recording graph evidence performs no allocation. */
    struct Audit {
        ResourceAudit resources;
        Faults faults;
        std::array<RenderGraphResourceInstance, 16> instances{};
        std::size_t instanceCount{};
        IRenderGraphResourceLease *retained{};
        bool completionObserved{};
        std::size_t shutdowns{};
        std::optional<Tests::AllocationProbe::ScopedFailure> rollbackAllocationFailure;

        void Complete() noexcept {
            if (retained != nullptr)
                std::exchange(retained, nullptr)->Release();
        }
    };

    /** @brief Delegates native validation to Null while controlling admission failure and completion timing. */
    class ProbeBackend final : public IRenderBackend {
    public:
        ProbeBackend(std::unique_ptr<IRenderBackend> backend, std::shared_ptr<Audit> audit)
            : backend_(std::move(backend)), audit_(std::move(audit)), completion_(*audit_) {}

        ~ProbeBackend() override {
            Shutdown();
        }

        Result<void> Initialize(const RenderBackendConfig &config) override {
            const auto initialized = backend_->Initialize(config);
            capabilities_ = backend_->Capabilities();
            capabilities_.backend = RenderBackendId{"transient-probe"};
            capabilities_.supportsExactTransientResourceReuse = audit_->faults.supportsReuse;
            live_ = initialized.HasValue();
            return initialized;
        }

        const RenderBackendCapabilities &Capabilities() const noexcept override {
            return capabilities_;
        }

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
            if (auto failure = QueryFailure(); failure.has_value())
                return Result<RenderMemoryCostPlan>::Failure(std::move(*failure));
            return backend_->QueryBufferMemoryCost(descriptor);
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
            if (auto failure = QueryFailure(); failure.has_value())
                return Result<RenderMemoryCostPlan>::Failure(std::move(*failure));
            return backend_->QueryTextureMemoryCost(descriptor);
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> bytes,
                                           const RenderMemoryPlacement &placement) override {
            if (FailCreation())
                return Result<std::uint64_t>::Failure(CreationError());
            return backend_->CreateBuffer(descriptor, bytes, placement);
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, const std::span<const std::byte> bytes,
                                            const RenderMemoryPlacement &placement) override {
            if (FailCreation())
                return Result<std::uint64_t>::Failure(CreationError());
            return backend_->CreateTexture(descriptor, bytes, placement);
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertex,
                                         const std::uint64_t index) override {
            return backend_->CreateMesh(descriptor, vertex, index);
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, const std::uint64_t texture) override {
            return backend_->CreateTextureView(descriptor, texture);
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t color,
                                                 const std::uint64_t depth) override {
            return backend_->CreateRenderTarget(descriptor, color, depth);
        }

        void DestroyBuffer(const std::uint64_t instance) noexcept override {
            ++audit_->resources.destroyed;
            backend_->DestroyBuffer(instance);
        }

        void DestroyTexture(const std::uint64_t instance) noexcept override {
            ++audit_->resources.destroyed;
            backend_->DestroyTexture(instance);
        }

        void DestroyMesh(const std::uint64_t instance) noexcept override {
            backend_->DestroyMesh(instance);
        }

        void DestroyTextureView(const std::uint64_t instance) noexcept override {
            backend_->DestroyTextureView(instance);
        }

        void DestroyRenderTarget(const std::uint64_t instance) noexcept override {
            backend_->DestroyRenderTarget(instance);
        }

        Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
            return backend_->BeginFrame(descriptor);
        }

        Result<void> Execute(const RenderExecutionPlan &plan) override {
            return backend_->Execute(plan);
        }

        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request) override {
            REQUIRE(request.resources.size() <= audit_->instances.size());
            audit_->instanceCount = request.resources.size();
            std::copy(request.resources.begin(), request.resources.end(), audit_->instances.begin());
            auto nativeRequest = request;
            nativeRequest.lease = &completion_;
            const auto admitted = backend_->ExecuteGraph(nativeRequest);
            if (admitted.HasError())
                return admitted;
            audit_->retained = request.lease;
            if (audit_->faults.failAfterTransfer) {
                REQUIRE(request.transfer != nullptr);
                request.transfer->authority = RenderGraphLeaseAuthority::BackendCompletion;
                return Result<void>::Failure(CreationError());
            }
            return admitted;
        }

        Result<void> Present(const FrameToken frame) override {
            return backend_->Present(frame);
        }

        void AbortFrame(const FrameToken frame) noexcept override {
            backend_->AbortFrame(frame);
        }

        void AbortActiveFrame() noexcept override {
            backend_->AbortActiveFrame();
        }

        Result<void> Resize(const FramebufferExtent extent) override {
            return backend_->Resize(extent);
        }

        void Shutdown() noexcept override {
            backend_->Shutdown();
            audit_->Complete();
            if (std::exchange(live_, false))
                ++audit_->shutdowns;
        }

    private:
        class Completion final : public IRenderGraphResourceLease {
        public:
            explicit Completion(Audit &audit) : audit_(audit) {}

            void Release() noexcept override {
                audit_.completionObserved = true;
                if (!audit_.faults.delayCompletion)
                    audit_.Complete();
            }

        private:
            Audit &audit_;
        };

        std::optional<Error> QueryFailure() const {
            if (++audit_->resources.queries != audit_->faults.failQuery)
                return std::nullopt;
            if (audit_->faults.queryFault == QueryFaultKind::Allocation)
                throw std::bad_alloc{};
            if (audit_->faults.queryFault == QueryFaultKind::Length)
                throw std::length_error{"Injected requirement metadata capacity failure."};
            if (audit_->faults.queryFault == QueryFaultKind::RollbackAllocation) {
                audit_->rollbackAllocationFailure.emplace();
                throw std::bad_alloc{};
            }
            return WithCause(CreationError(), {ErrorCode{"render.test.native_requirement_cause"},
                                               ErrorDomainId{"render.test.native"},
                                               ErrorSeverity::Warning,
                                               "Original native requirement evidence.",
                                               {}});
        }

        bool FailCreation() {
            if (++audit_->resources.creates == 1)
                audit_->resources.queriesAtFirstCreate = audit_->resources.queries;
            return audit_->resources.creates == audit_->faults.failCreate;
        }

        static Error CreationError() {
            return {ErrorCode{"render.test.transient_native_failure"},
                    ErrorDomainId{"render.test"},
                    ErrorSeverity::Error,
                    "Injected native resource failure.",
                    {}};
        }

        std::unique_ptr<IRenderBackend> backend_;
        std::shared_ptr<Audit> audit_;
        Completion completion_;
        RenderBackendCapabilities capabilities_;
        bool live_{};
    };

    /** @brief Explicit test composition; descriptors remain inert until their provider is invoked. */
    class ProbeProvider final : public IRenderBackendProvider {
    public:
        explicit ProbeProvider(std::shared_ptr<Audit> audit) : audit_(std::move(audit)) {}

        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            RenderBackendRegistry registry;
            REQUIRE(RegisterNullRenderBackend(registry).HasValue());
            REQUIRE(registry.Seal().HasValue());
            auto backend = registry.Create(RenderBackendId{"null"});
            REQUIRE(backend.HasValue());
            return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<ProbeBackend>(std::move(backend).Value(), audit_));
        }

    private:
        std::shared_ptr<Audit> audit_;
    };

    inline std::unique_ptr<RenderFrontend> MakeFrontend(const std::shared_ptr<Audit> &audit, const RenderFrontendMemoryConfig &memory = {},
                                                        const RenderResourceRetirementLimits &retirement = {},
                                                        const RenderResourceUploadLimits &upload = {}) {
        RenderBackendRegistry registry;
        REQUIRE(registry
                    .Register({.id = RenderBackendId{"transient-probe"},
                               .displayName = "Transient probe",
                               .provider = std::make_unique<ProbeProvider>(audit)})
                    .HasValue());
        REQUIRE(registry.Seal().HasValue());
        auto frontend = RenderFrontend::Create(registry, RenderBackendId{"transient-probe"}, {}, upload, memory, retirement);
        REQUIRE(frontend.HasValue());
        return std::move(frontend).Value();
    }
}  // namespace Horo::Render::TransientTest
