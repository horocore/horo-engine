#include "MetalBackendInternal.h"
#include "MetalRenderBackendErrors.h"

#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        [[nodiscard]] Error MakeMetalError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        struct MetalPresentationLease {
            bool claimed{false};
        };

        class MetalRenderBackend final : public IRenderBackend {
        public:
            MetalRenderBackend(std::unique_ptr<Detail::IMetalRuntime> runtime,
                               std::shared_ptr<MetalPresentationLease> presentationLease) noexcept
                : runtime_(std::move(runtime)), presentationLease_(std::move(presentationLease)) {}

            ~MetalRenderBackend() override {
                Shutdown();
            }

            /** @copydoc IRenderBackend::Initialize */
            Result<void> Initialize(const RenderBackendConfig &config) override {
                if (initialized_) {
                    return Result<void>::Failure(
                        MakeMetalError(MetalBackendErrors::AlreadyInitialized, "Renderer backend is already initialized."));
                }
                if (!config.IsValid()) {
                    return Result<void>::Failure(
                        MakeMetalError(MetalBackendErrors::InvalidConfig, "Metal backend configuration is invalid."));
                }
                if (config.maxFramesInFlight < 2 || config.maxFramesInFlight > 3) {
                    return Result<void>::Failure(MakeMetalError(MetalBackendErrors::UnsupportedFramesInFlight,
                                                                "Metal interactive presentation supports two or three frames in flight."));
                }
                if (presentationLease_->claimed) {
                    return Result<void>::Failure(MakeMetalError(MetalBackendErrors::PresentationInUse,
                                                                "Metal presentation attachment is already owned by another backend."));
                }

                presentationLease_->claimed = true;
                ownsPresentationLease_ = true;
                // Claim cleanup before entering platform code. Typed failure guarantees no
                // retained resources; an exception may occur after native acquisition.
                runtimeInitialized_ = true;
                const Result<void> created = runtime_->Initialize(MetalPresentationDescriptor{
                    .enableValidation = config.enableValidation,
                    .maxFramesInFlight = config.maxFramesInFlight,
                    .presentMode = config.presentMode,
                });
                if (created.HasError()) {
                    runtime_->Shutdown();
                    runtimeInitialized_ = false;
                    ReleasePresentationLease();
                    return Result<void>::Failure(created.ErrorValue());
                }

                initialized_ = true;
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Capabilities */
            const RenderBackendCapabilities &Capabilities() const noexcept override {
                return capabilities_;
            }

            /** @copydoc IRenderBackend::QueryBufferMemoryCost */
            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                if (!initialized_)
                    return Result<RenderMemoryCostPlan>::Failure(
                        MakeMetalError(MetalBackendErrors::NotInitialized,
                                       "Metal buffer memory requirements require an initialized backend."));
                return runtime_->QueryBufferMemoryCost(descriptor);
            }

            /** @copydoc IRenderBackend::QueryTextureMemoryCost */
            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
                if (!initialized_)
                    return Result<RenderMemoryCostPlan>::Failure(
                        MakeMetalError(MetalBackendErrors::NotInitialized,
                                       "Metal texture memory requirements require an initialized backend."));
                return runtime_->QueryTextureMemoryCost(descriptor);
            }

            /** @copydoc IRenderBackend::CreateBuffer */
            Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> initialData,
                                               const RenderMemoryPlacement &placement) override {
                if (!initialized_)
                    return ResourceNotInitialized("Metal buffer creation requires an initialized backend.");
                return runtime_->CreateBuffer(descriptor, initialData, placement);
            }

            /** @copydoc IRenderBackend::CreateMesh */
            Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                             const std::uint64_t indexBuffer) override {
                if (!initialized_)
                    return ResourceNotInitialized("Metal mesh creation requires an initialized backend.");
                return runtime_->CreateMesh(descriptor, vertexBuffer, indexBuffer);
            }

            Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, const std::span<const std::byte> initialData,
                                                const RenderMemoryPlacement &placement) override {
                if (!initialized_)
                    return ResourceNotInitialized("Metal texture creation requires an initialized backend.");
                return runtime_->CreateTexture(descriptor, initialData, placement);
            }

            Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, const std::uint64_t texture) override {
                if (!initialized_)
                    return ResourceNotInitialized("Metal texture-view creation requires an initialized backend.");
                return runtime_->CreateTextureView(descriptor, texture);
            }

            Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                     const std::uint64_t depthAttachment) override {
                if (!initialized_)
                    return ResourceNotInitialized("Metal render-target creation requires an initialized backend.");
                return runtime_->CreateRenderTarget(descriptor, colorAttachment, depthAttachment);
            }

            /** @copydoc IRenderBackend::DestroyBuffer */
            void DestroyBuffer(const std::uint64_t backendInstance) noexcept override {
                if (runtimeInitialized_)
                    runtime_->DestroyBuffer(backendInstance);
            }

            /** @copydoc IRenderBackend::DestroyMesh */
            void DestroyMesh(const std::uint64_t backendInstance) noexcept override {
                if (runtimeInitialized_)
                    runtime_->DestroyMesh(backendInstance);
            }

            void DestroyTexture(const std::uint64_t backendInstance) noexcept override {
                if (runtimeInitialized_)
                    runtime_->DestroyTexture(backendInstance);
            }

            void DestroyTextureView(const std::uint64_t backendInstance) noexcept override {
                if (runtimeInitialized_)
                    runtime_->DestroyTextureView(backendInstance);
            }

            void DestroyRenderTarget(const std::uint64_t backendInstance) noexcept override {
                if (runtimeInitialized_)
                    runtime_->DestroyRenderTarget(backendInstance);
            }

            /** @copydoc IRenderBackend::BeginFrame */
            Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
                if (!initialized_) {
                    return Result<FrameToken>::Failure(
                        MakeMetalError(MetalBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (frameActive_) {
                    return Result<FrameToken>::Failure(
                        MakeMetalError(MetalBackendErrors::FrameAlreadyActive, "A renderer frame is already active."));
                }
                if (descriptor.frameNumber == 0 || !descriptor.outputExtent.IsValid()) {
                    return Result<FrameToken>::Failure(
                        MakeMetalError(MetalBackendErrors::InvalidFrameDescriptor, "Frame number and output extent must be valid."));
                }
                if (nextFrameToken_ == std::numeric_limits<std::uint64_t>::max()) {
                    return Result<FrameToken>::Failure(
                        MakeMetalError(MetalBackendErrors::FrameTokenExhausted, "Frame token space is exhausted."));
                }

                const Result<void> begun = runtime_->BeginFrame(descriptor.outputExtent);
                if (begun.HasError()) {
                    return Result<FrameToken>::Failure(begun.ErrorValue());
                }

                frameActive_ = true;
                activeFrame_ = FrameToken{nextFrameToken_++};
                return Result<FrameToken>::Success(activeFrame_);
            }

            /** @copydoc IRenderBackend::Execute */
            Result<void> Execute(const RenderExecutionPlan &plan) override {
                const Result<void> valid = ValidatePlan(plan);
                if (valid.HasError()) {
                    return valid;
                }

                for (const RenderPassDescriptor &pass : plan.orderedPasses) {
                    if (!pass.primaryOutput.has_value()) {
                        continue;
                    }
                    const Result<void> encoded = runtime_->ExecutePrimaryOutput(*pass.primaryOutput);
                    if (encoded.HasError()) {
                        return Result<void>::Failure(encoded.ErrorValue());
                    }
                }
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Present */
            Result<void> Present(const FrameToken frame) override {
                const Result<void> state = ValidateActiveFrame(frame);
                if (state.HasError()) {
                    return state;
                }

                const Result<void> presented = runtime_->Present();
                if (presented.HasError()) {
                    return Result<void>::Failure(presented.ErrorValue());
                }

                frameActive_ = false;
                activeFrame_ = {};
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::AbortFrame */
            void AbortFrame(const FrameToken frame) noexcept override {
                if (frameActive_ && frame == activeFrame_) {
                    AbortActiveFrame();
                }
            }

            /** @copydoc IRenderBackend::AbortActiveFrame */
            void AbortActiveFrame() noexcept override {
                if (frameActive_) {
                    runtime_->AbortFrame();
                    frameActive_ = false;
                    activeFrame_ = {};
                }
            }

            /** @copydoc IRenderBackend::Resize */
            Result<void> Resize(const FramebufferExtent extent) override {
                if (!initialized_) {
                    return Result<void>::Failure(
                        MakeMetalError(MetalBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!extent.IsValid()) {
                    return Result<void>::Failure(
                        MakeMetalError(MetalBackendErrors::InvalidExtent, "Renderer output extent must be non-zero."));
                }
                if (frameActive_) {
                    return Result<void>::Failure(
                        MakeMetalError(MetalBackendErrors::FrameActive, "Renderer output cannot resize while a frame is active."));
                }
                return runtime_->Resize(extent);
            }

            /** @copydoc IRenderBackend::Shutdown */
            void Shutdown() noexcept override {
                AbortActiveFrame();
                initialized_ = false;
                DestroyRuntime();
            }

        private:
            [[nodiscard]] static Result<std::uint64_t> ResourceNotInitialized(std::string message) {
                return Result<std::uint64_t>::Failure(MakeMetalError(MetalBackendErrors::NotInitialized, std::move(message)));
            }

            [[nodiscard]] Result<void> ValidateActiveFrame(const FrameToken frame) const {
                if (!initialized_) {
                    return Result<void>::Failure(
                        MakeMetalError(MetalBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!frameActive_) {
                    return Result<void>::Failure(MakeMetalError(MetalBackendErrors::NoActiveFrame, "No renderer frame is active."));
                }
                if (frame != activeFrame_) {
                    return Result<void>::Failure(
                        MakeMetalError(MetalBackendErrors::FrameTokenMismatch, "Frame token does not match the active frame."));
                }
                return Result<void>::Success();
            }

            [[nodiscard]] Result<void> ValidatePlan(const RenderExecutionPlan &plan) const {
                const Result<void> state = ValidateActiveFrame(plan.frame);
                if (state.HasError()) {
                    return state;
                }

                for (std::size_t index = 0; index < plan.orderedPasses.size(); ++index) {
                    const RenderPassDescriptor &pass = plan.orderedPasses[index];
                    if (!pass.id.IsValid()) {
                        return Result<void>::Failure(
                            MakeMetalError(MetalBackendErrors::InvalidExecutionPlan, "Execution plan contains an invalid render pass ID."));
                    }
                    if (pass.kind != RenderPassKind::Graphics) {
                        return Result<void>::Failure(MakeMetalError(MetalBackendErrors::UnsupportedPassKind,
                                                                    "Initial Metal backend supports graphics passes only."));
                    }
                    for (std::size_t previous = 0; previous < index; ++previous) {
                        if (pass.id == plan.orderedPasses[previous].id) {
                            return Result<void>::Failure(MakeMetalError(MetalBackendErrors::InvalidExecutionPlan,
                                                                        "Execution plan contains duplicate render pass IDs."));
                        }
                    }
                    if (pass.primaryOutput.has_value() && !pass.primaryOutput->IsValid()) {
                        return Result<void>::Failure(
                            MakeMetalError(MetalBackendErrors::InvalidExecutionPlan, "Primary output attachment operations are invalid."));
                    }
                }
                return Result<void>::Success();
            }

            void DestroyRuntime() noexcept {
                if (runtimeInitialized_) {
                    runtime_->Shutdown();
                    runtimeInitialized_ = false;
                }
                ReleasePresentationLease();
            }

            void ReleasePresentationLease() noexcept {
                if (ownsPresentationLease_) {
                    presentationLease_->claimed = false;
                    ownsPresentationLease_ = false;
                }
            }

            std::unique_ptr<Detail::IMetalRuntime> runtime_;
            std::shared_ptr<MetalPresentationLease> presentationLease_;
            RenderBackendCapabilities capabilities_{
                .backend = RenderBackendId{"metal"},
                .presentsToWindow = true,
                .supportsOffscreenTargets = true,
                .supportsTimestampQueries = false,
                .supportsCompute = false,
                .supportsBindlessResources = false,
                .supportsRayTracing = false,
                .supportsBufferResources = true,
                .supportsMeshResources = true,
                .supportsTextureResources = true,
                .supportsRenderTargetResources = true,
            };
            FrameToken activeFrame_{};
            std::uint64_t nextFrameToken_{1};
            bool initialized_{false};
            bool runtimeInitialized_{false};
            bool ownsPresentationLease_{false};
            bool frameActive_{false};
        };

        class MetalBackendProvider final : public IRenderBackendProvider {
        public:
            MetalBackendProvider(IMetalPresentationPort &presentationPort, MetalEditorGraphicsBridge &editorGraphicsBridge,
                                 const Detail::IMetalRuntimeFactory &runtimeFactory)
                : presentationPort_(&presentationPort), editorGraphicsBridge_(&editorGraphicsBridge), runtimeFactory_(&runtimeFactory),
                  presentationLease_(std::make_shared<MetalPresentationLease>()) {}

            /** @copydoc IRenderBackendProvider::Create */
            Result<std::unique_ptr<IRenderBackend>> Create() const override {
                auto runtime = runtimeFactory_->Create(*presentationPort_, *editorGraphicsBridge_);
                if (runtime.HasError()) {
                    return Result<std::unique_ptr<IRenderBackend>>::Failure(runtime.ErrorValue());
                }
                return Result<std::unique_ptr<IRenderBackend>>::Success(
                    std::make_unique<MetalRenderBackend>(std::move(runtime).Value(), presentationLease_));
            }

        private:
            IMetalPresentationPort *presentationPort_{nullptr};
            MetalEditorGraphicsBridge *editorGraphicsBridge_{nullptr};
            const Detail::IMetalRuntimeFactory *runtimeFactory_{nullptr};
            std::shared_ptr<MetalPresentationLease> presentationLease_;
        };

        class MetalRuntimeFactory final : public Detail::IMetalRuntimeFactory {
        public:
            Result<std::unique_ptr<Detail::IMetalRuntime>> Create(IMetalPresentationPort &presentationPort,
                                                                  MetalEditorGraphicsBridge &editorGraphicsBridge) const override {
                return Detail::CreateMetalRuntime(presentationPort, editorGraphicsBridge);
            }
        };
    }  // namespace

    /** @copydoc GetMetalRenderBackendModuleInfo */
    const RenderBackendModuleInfo &GetMetalRenderBackendModuleInfo() noexcept {
        static const RenderBackendModuleInfo info{
            .id = RenderBackendId{"metal"},
            .displayName = "Metal",
            .windowRequirements =
                RenderHostWindowRequirements{
                    .presentation = RenderPresentationKind::Metal,
                    .resizable = true,
                    .highPixelDensity = true,
                },
            .supportsInteractivePresentation = true,
        };
        return info;
    }

    /** @copydoc MetalEditorGraphicsBridge::Device */
    void *MetalEditorGraphicsBridge::Device() const noexcept {
        return device_;
    }

    /** @copydoc MetalEditorGraphicsBridge::CommandQueue */
    void *MetalEditorGraphicsBridge::CommandQueue() const noexcept {
        return commandQueue_;
    }

    /** @copydoc MetalEditorGraphicsBridge::CurrentCommandBuffer */
    void *MetalEditorGraphicsBridge::CurrentCommandBuffer() const noexcept {
        return commandBuffer_;
    }

    /** @copydoc MetalEditorGraphicsBridge::CurrentRenderPassDescriptor */
    void *MetalEditorGraphicsBridge::CurrentRenderPassDescriptor() const noexcept {
        return renderPassDescriptor_;
    }

    /** @copydoc MetalEditorGraphicsBridge::CurrentRenderEncoder */
    void *MetalEditorGraphicsBridge::CurrentRenderEncoder() const noexcept {
        return renderEncoder_;
    }

    /** @copydoc MetalEditorGraphicsBridge::WaitUntilIdle */
    void MetalEditorGraphicsBridge::WaitUntilIdle() noexcept {
        if (waitUntilIdle_ != nullptr) {
            waitUntilIdle_(waitContext_);
        }
    }

    /** @copydoc RegisterMetalRenderBackend */
    Result<void> RegisterMetalRenderBackend(RenderBackendRegistry &registry, IMetalPresentationPort &presentationPort,
                                            MetalEditorGraphicsBridge &editorGraphicsBridge) {
        static const MetalRuntimeFactory runtimeFactory;
        return Detail::RegisterMetalRenderBackendWithRuntimeFactory(registry, presentationPort, editorGraphicsBridge, runtimeFactory);
    }

    namespace Detail {
        void MetalEditorGraphicsAccess::PublishPersistent(MetalEditorGraphicsBridge &bridge, void *device, void *commandQueue,
                                                          void *waitContext,
                                                          MetalEditorGraphicsBridge::WaitUntilIdleFunction wait) noexcept {
            bridge.device_ = device;
            bridge.commandQueue_ = commandQueue;
            bridge.waitContext_ = waitContext;
            bridge.waitUntilIdle_ = wait;
        }

        void MetalEditorGraphicsAccess::PublishFrame(MetalEditorGraphicsBridge &bridge, void *commandBuffer, void *renderPassDescriptor,
                                                     void *renderEncoder) noexcept {
            bridge.commandBuffer_ = commandBuffer;
            bridge.renderPassDescriptor_ = renderPassDescriptor;
            bridge.renderEncoder_ = renderEncoder;
        }

        void MetalEditorGraphicsAccess::ClearFrame(MetalEditorGraphicsBridge &bridge) noexcept {
            PublishFrame(bridge, nullptr, nullptr, nullptr);
        }

        void MetalEditorGraphicsAccess::Clear(MetalEditorGraphicsBridge &bridge) noexcept {
            ClearFrame(bridge);
            PublishPersistent(bridge, nullptr, nullptr, nullptr, nullptr);
        }

        Result<void> RegisterMetalRenderBackendWithRuntimeFactory(RenderBackendRegistry &registry, IMetalPresentationPort &presentationPort,
                                                                  MetalEditorGraphicsBridge &editorGraphicsBridge,
                                                                  const IMetalRuntimeFactory &runtimeFactory) {
            return registry.Register(RenderBackendDescriptor{
                .id = RenderBackendId{"metal"},
                .displayName = "Metal",
                .provider = std::make_unique<MetalBackendProvider>(presentationPort, editorGraphicsBridge, runtimeFactory),
            });
        }
    }  // namespace Detail
}  // namespace Horo::Render
