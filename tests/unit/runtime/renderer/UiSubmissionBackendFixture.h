#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderMemoryTestSupport.h"
#include "UiSubmissionSourceFixture.h"

#include <array>
#include <memory>
#include <stdexcept>

namespace Horo::Render::Test {
    /** @brief Explicit asynchronous backend ledger; production frontend admission/retention remains under test. */
    struct UiSubmissionBackendState final {
        IRenderGraphResourceLease *active{};
        std::array<IRenderGraphResourceLease *, 8> submitted{};
        bool failExecute{};
        bool failPresent{};
        bool throwExecute{};
        bool failedCompletion{}; /**< Models terminal native submission failure; submitted leases remain until Shutdown. */
        std::size_t executions{};
        std::size_t abandoned{};
        std::size_t destroyedTextures{};
        std::uint64_t nextInstance{1};

        void Complete() noexcept {
            for (auto *&lease : submitted) {
                if (lease != nullptr)
                    lease->Release();
                lease = nullptr;
            }
        }
    };

    class UiSubmissionBackend final : public IRenderBackend {
    public:
        explicit UiSubmissionBackend(UiSubmissionBackendState &state) : state_(&state) {}

        Result<void> Initialize(const RenderBackendConfig &) override {
            return Result<void>::Success();
        }

        const RenderBackendCapabilities &Capabilities() const noexcept override {
            return capabilities_;
        }

        Result<FrameToken> BeginFrame(const FrameDescriptor &frame) override {
            if (state_->failedCompletion)
                return Unsupported<FrameToken>();
            return Result<FrameToken>::Success({frame.frameNumber});
        }

        Result<void> Execute(const RenderExecutionPlan &) override {
            return Unsupported<void>();
        }

        Result<void> ExecuteGraph(const RenderGraphExecutionRequest &request) override {
            ++state_->executions;
            state_->active = request.lease;
            if (state_->throwExecute)
                throw std::runtime_error("injected graph admission exception");
            return state_->failExecute ? Unsupported<void>() : Result<void>::Success();
        }

        Result<void> Present(FrameToken) override {
            if (state_->failPresent)
                return Unsupported<void>();
            for (auto *&lease : state_->submitted) {
                if (lease == nullptr) {
                    lease = std::exchange(state_->active, nullptr);
                    return Result<void>::Success();
                }
            }
            return Unsupported<void>();
        }

        void AbortFrame(FrameToken) noexcept override {
            AbortActiveFrame();
        }

        void AbortActiveFrame() noexcept override {
            if (state_->active != nullptr) {
                state_->active->Release();
                state_->active = nullptr;
                ++state_->abandoned;
            }
        }

        Result<void> Resize(FramebufferExtent) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            AbortActiveFrame();
            state_->Complete();
        }

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &) const override {
            return Unsupported<RenderMemoryCostPlan>();
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
            return Result<RenderMemoryCostPlan>::Success(
                TestSupport::DedicatedMemoryCost(RenderTextureBaseLevelByteSize(descriptor).value(), 2));
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &, std::span<const std::byte>,
                                           const RenderMemoryPlacement &) override {
            return Unsupported<std::uint64_t>();
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &, std::uint64_t, std::uint64_t) override {
            return Unsupported<std::uint64_t>();
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &, std::span<const std::byte>,
                                            const RenderMemoryPlacement &) override {
            return Result<std::uint64_t>::Success(state_->nextInstance++);
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &, std::uint64_t) override {
            return Unsupported<std::uint64_t>();
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &, std::uint64_t, std::uint64_t) override {
            return Unsupported<std::uint64_t>();
        }

        void DestroyBuffer(std::uint64_t) noexcept override {}

        void DestroyMesh(std::uint64_t) noexcept override {}

        void DestroyTexture(std::uint64_t) noexcept override {
            ++state_->destroyedTextures;
        }

        void DestroyTextureView(std::uint64_t) noexcept override {}

        void DestroyRenderTarget(std::uint64_t) noexcept override {}

    private:
        template <typename T> static Result<T> Unsupported() {
            return Result<T>::Failure(MakeError(UiErrors::RenderCompositionInvalid));
        }

        UiSubmissionBackendState *state_;
        RenderBackendCapabilities capabilities_{.backend = RenderBackendId{"ui-lifetime"}, .supportsTextureResources = true};
    };

    class UiSubmissionProvider final : public IRenderBackendProvider {
    public:
        explicit UiSubmissionProvider(UiSubmissionBackendState &state) : state_(&state) {}

        Result<std::unique_ptr<IRenderBackend>> Create() const override {
            return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<UiSubmissionBackend>(*state_));
        }

    private:
        UiSubmissionBackendState *state_;
    };

    inline std::unique_ptr<RenderFrontend> UiFrontend(UiSubmissionBackendState &state) {
        RenderBackendRegistry registry;
        REQUIRE(
            registry.Register({RenderBackendId{"ui-lifetime"}, "UI lifetime test backend", std::make_unique<UiSubmissionProvider>(state)})
                .HasValue());
        REQUIRE(registry.Seal().HasValue());
        return UiRequire(RenderFrontend::Create(registry, RenderBackendId{"ui-lifetime"}, {}));
    }
}  // namespace Horo::Render::Test
