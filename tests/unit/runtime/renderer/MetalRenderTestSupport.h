#pragma once

#include "BackendTestSupport.h"
#include "MetalDeviceCapabilityTestSupport.h"
#include "RenderMemoryTestSupport.h"
#include "runtime/renderer/modules/metal/MetalBackendInternal.h"

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <utility>

namespace Horo::Render::MetalBackendTests {
    using BackendTestSupport::Check;
    using BackendTestSupport::MakePortError;

    enum class PortFailure {
        None,
        Create,
        Begin,
        Execute,
        Present,
    };

    struct PortState {
        int createCount{0};
        int beginCount{0};
        int executeCount{0};
        int presentCount{0};
        int abortCount{0};
        int resizeCount{0};
        int destroyCount{0};
        int resourceCreateCount{0};
        int resourceDestroyCount{0};
        MetalPresentationDescriptor descriptor{};
        FramebufferExtent frameExtent{};
        FramebufferExtent resizedExtent{};
        PrimaryOutputAttachment attachment{};
        PortFailure failure{PortFailure::None};
        bool presentationCreated{false};
        bool frameActive{false};
    };

    class FakePresentationPort final : public IMetalPresentationPort {
    public:
        explicit FakePresentationPort(PortState &state) noexcept : state_(&state) {}

        Result<void> CreateSurface() override {
            ++state_->createCount;
            if (state_->failure == PortFailure::Create) {
                return Result<void>::Failure(MakePortError("render.test.create_failed", "Injected Metal creation failure."));
            }
            state_->presentationCreated = true;
            return Result<void>::Success();
        }

        void *Layer() const noexcept override {  // NOSONAR(cpp:S5008) Required opaque platform seam.
            return state_->presentationCreated ? state_ : nullptr;
        }

        void DestroySurface() noexcept override {
            if (state_->presentationCreated) {
                ++state_->destroyCount;
                state_->presentationCreated = false;
            }
        }

    private:
        PortState *state_{nullptr};
    };

    class FakeMetalRuntime final : public Detail::IMetalRuntime {
    public:
        FakeMetalRuntime(IMetalPresentationPort &presentationPort, PortState &state) noexcept
            : presentationPort_(&presentationPort), state_(&state) {}

        Result<Detail::MetalDeviceCapabilities> Initialize(const MetalPresentationDescriptor &descriptor,
                                                           const Detail::MetalDeviceAdmissionRequest &request) override {
            state_->descriptor = descriptor;
            const Result<void> created = presentationPort_->CreateSurface();
            initialized_ = created.HasValue();
            if (created.HasError()) {
                return Result<Detail::MetalDeviceCapabilities>::Failure(created.ErrorValue());
            }
            Detail::MetalDeviceCapabilities capabilities = Test::MakeMetalCapabilities();
            if (request.adapter && (*request.adapter != capabilities.adapter.id || request.discoveryRevision != 1)) {
                return Result<Detail::MetalDeviceCapabilities>::Failure(
                    MakePortError("render.test.adapter_mismatch", "Injected Metal adapter selection mismatch."));
            }
            return Result<Detail::MetalDeviceCapabilities>::Success(std::move(capabilities));
        }

        Result<void> BeginFrame(const FramebufferExtent extent) override {
            ++state_->beginCount;
            state_->frameExtent = extent;
            if (state_->failure == PortFailure::Begin) {
                return Result<void>::Failure(MakePortError("render.test.begin_failed", "Injected Metal begin failure."));
            }
            state_->frameActive = true;
            return Result<void>::Success();
        }

        Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
            return Result<RenderMemoryCostPlan>::Success(TestSupport::DedicatedMemoryCost(descriptor.byteSize, 1));
        }

        Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
            const std::size_t bytes = RenderTextureBaseLevelByteSize(descriptor).value_or(0);
            return Result<RenderMemoryCostPlan>::Success(TestSupport::DedicatedMemoryCost(bytes, 2));
        }

        Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &, std::span<const std::byte>,
                                           const RenderMemoryPlacement &) override {
            ++state_->resourceCreateCount;
            return Result<std::uint64_t>::Success(nextResourceIdentity_++);
        }

        Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &, std::uint64_t, std::uint64_t) override {  // NOSONAR(cpp:S4144)
            ++state_->resourceCreateCount;
            return Result<std::uint64_t>::Success(nextResourceIdentity_++);
        }

        Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &, std::span<const std::byte>,  // NOSONAR(cpp:S4144)
                                            const RenderMemoryPlacement &) override {                     // NOSONAR(cpp:S4144)
            ++state_->resourceCreateCount;
            return Result<std::uint64_t>::Success(nextResourceIdentity_++);
        }

        Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &, std::uint64_t) override {  // NOSONAR(cpp:S4144)
            ++state_->resourceCreateCount;
            return Result<std::uint64_t>::Success(nextResourceIdentity_++);
        }

        Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &, std::uint64_t,  // NOSONAR(cpp:S4144)
                                                 std::uint64_t) override {                       // NOSONAR(cpp:S4144)
            ++state_->resourceCreateCount;
            return Result<std::uint64_t>::Success(nextResourceIdentity_++);
        }

        void DestroyBuffer(std::uint64_t) noexcept override {
            ++state_->resourceDestroyCount;
        }

        void DestroyMesh(std::uint64_t) noexcept override {
            ++state_->resourceDestroyCount;
        }

        void DestroyTexture(std::uint64_t) noexcept override {
            ++state_->resourceDestroyCount;
        }

        void DestroyTextureView(std::uint64_t) noexcept override {
            ++state_->resourceDestroyCount;
        }

        void DestroyRenderTarget(std::uint64_t) noexcept override {
            ++state_->resourceDestroyCount;
        }

        Result<void> ExecutePrimaryOutput(const PrimaryOutputAttachment &attachment) override {
            ++state_->executeCount;
            state_->attachment = attachment;
            if (state_->failure == PortFailure::Execute) {
                return Result<void>::Failure(MakePortError("render.test.execute_failed", "Injected Metal execute failure."));
            }
            return Result<void>::Success();
        }

        Result<void> Present() override {
            ++state_->presentCount;
            if (state_->failure == PortFailure::Present) {
                return Result<void>::Failure(MakePortError("render.test.present_failed", "Injected Metal present failure."));
            }
            state_->frameActive = false;
            return Result<void>::Success();
        }

        void AbortFrame() noexcept override {
            if (state_->frameActive) {
                ++state_->abortCount;
                state_->frameActive = false;
            }
        }

        Result<void> Resize(const FramebufferExtent extent) override {
            ++state_->resizeCount;
            state_->resizedExtent = extent;
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            AbortFrame();
            if (initialized_) {
                presentationPort_->DestroySurface();
                initialized_ = false;
            }
        }

    private:
        IMetalPresentationPort *presentationPort_{nullptr};
        PortState *state_{nullptr};
        std::uint64_t nextResourceIdentity_{1};
        bool initialized_{false};
    };

    class FakeMetalRuntimeFactory final : public Detail::IMetalRuntimeFactory {
    public:
        explicit FakeMetalRuntimeFactory(PortState &state) noexcept : state_(&state) {}

        Result<std::unique_ptr<Detail::IMetalRuntime>> Create(IMetalPresentationPort &presentationPort,
                                                              MetalEditorGraphicsBridge &) const override {
            return Result<std::unique_ptr<Detail::IMetalRuntime>>::Success(std::make_unique<FakeMetalRuntime>(presentationPort, *state_));
        }

    private:
        PortState *state_{nullptr};
    };

    [[nodiscard]] inline std::unique_ptr<IRenderBackend> CreateBackend(FakePresentationPort &port, PortState &state,
                                                                       MetalEditorGraphicsBridge &bridge) {
        FakeMetalRuntimeFactory runtimeFactory{state};
        RenderBackendRegistry registry;
        Check(Detail::RegisterMetalRenderBackendWithRuntimeFactory(registry, port, bridge, runtimeFactory).HasValue());
        Check(registry.Seal().HasValue());
        auto created = registry.Create(RenderBackendId{"metal"});
        Check(created.HasValue());
        return std::move(created).Value();
    }

    [[nodiscard]] inline std::unique_ptr<IRenderBackend> CreateInitializedBackend(FakePresentationPort &port, PortState &state,
                                                                                  MetalEditorGraphicsBridge &bridge) {
        std::unique_ptr<IRenderBackend> backend = CreateBackend(port, state, bridge);
        Check(backend->Initialize(RenderBackendConfig{}).HasValue());
        return backend;
    }
}  // namespace Horo::Render::MetalBackendTests
