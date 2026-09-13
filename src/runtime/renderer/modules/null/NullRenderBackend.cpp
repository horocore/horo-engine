#include "Horo/Runtime/Render/NullBackendModule.h"
#include "NullRenderBackendErrors.h"

#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Creates one backend-domain typed error. */
        [[nodiscard]] Error MakeBackendError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool MatchesPlacement(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) noexcept {
            return placement.IsValid() && placement.memoryClass == plan.memoryClass && placement.allocationClass == plan.allocationClass &&
                   placement.provenance == plan.provenance && placement.compatibility == plan.compatibility &&
                   placement.payloadBytes == plan.payloadBytes && placement.requiredBytes == plan.requiredBytes &&
                   placement.offsetBytes == 0;
        }

        /** @brief Headless backend that validates renderer lifecycle without acquiring GPU resources. */
        class NullRenderBackend final : public IRenderBackend {
        public:
            NullRenderBackend() = default;
            NullRenderBackend(const NullRenderBackend &) = delete;
            NullRenderBackend &operator=(const NullRenderBackend &) = delete;
            NullRenderBackend(NullRenderBackend &&) = delete;
            NullRenderBackend &operator=(NullRenderBackend &&) = delete;

            ~NullRenderBackend() override {
                Shutdown();
            }

            /** @copydoc IRenderBackend::Initialize */
            Result<void> Initialize(const RenderBackendConfig &config) override {
                if (initialized_) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::AlreadyInitialized, "Renderer backend is already initialized."));
                }
                if (config.requirePresentation) {
                    return Result<void>::Failure(MakeBackendError(NullBackendErrors::PresentationUnsupported,
                                                                  "Null renderer cannot satisfy a presentation requirement."));
                }
                if (!config.IsValid()) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidConfig, "Frames in flight must be in the inclusive range [1, 8]."));
                }

                config_ = config;
                initialized_ = true;
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Capabilities */
            const RenderBackendCapabilities &Capabilities() const noexcept override {
                return capabilities_;
            }

            /** @copydoc IRenderBackend::QueryBufferMemoryCost */
            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                if (!initialized_ || !descriptor.IsValid())
                    return Result<RenderMemoryCostPlan>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidConfig, "Null buffer memory requirement request is invalid."));
                return Result<RenderMemoryCostPlan>::Success({.memoryClass = RenderMemoryClass::PersistentDevice,
                                                              .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                              .provenance = RenderMemoryCostProvenance::Exact,
                                                              .compatibility = RenderMemoryCompatibilityId{1},
                                                              .payloadBytes = descriptor.byteSize,
                                                              .requiredBytes = descriptor.byteSize,
                                                              .alignment = 1});
            }

            /** @copydoc IRenderBackend::QueryTextureMemoryCost */
            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
                const auto bytes = RenderTextureBaseLevelByteSize(descriptor);
                if (!initialized_ || !bytes.has_value())
                    return Result<RenderMemoryCostPlan>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidConfig, "Null texture memory requirement request is invalid."));
                return Result<RenderMemoryCostPlan>::Success({.memoryClass = RenderMemoryClass::PersistentDevice,
                                                              .allocationClass = RenderMemoryAllocationClass::Dedicated,
                                                              .provenance = RenderMemoryCostProvenance::Exact,
                                                              .compatibility = RenderMemoryCompatibilityId{2},
                                                              .payloadBytes = *bytes,
                                                              .requiredBytes = *bytes,
                                                              .alignment = 1});
            }

            /** @copydoc IRenderBackend::CreateBuffer */
            Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> initialData,
                                               const RenderMemoryPlacement &placement) override {
                if (!initialized_) {
                    return Result<std::uint64_t>::Failure(
                        MakeBackendError(NullBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (const auto cost = QueryBufferMemoryCost(descriptor);
                    !descriptor.IsValid() || (!initialData.empty() && initialData.size() != descriptor.byteSize) || cost.HasError() ||
                    !MatchesPlacement(cost.Value(), placement)) {
                    return Result<std::uint64_t>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidConfig, "Null buffer realization request is invalid."));
                }
                return NextResourceInstance();
            }

            /** @copydoc IRenderBackend::CreateMesh */
            Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                             const std::uint64_t indexBuffer) override {
                if (!initialized_) {
                    return Result<std::uint64_t>::Failure(
                        MakeBackendError(NullBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!descriptor.IsValid() || vertexBuffer == 0 || indexBuffer == 0) {
                    return Result<std::uint64_t>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidConfig, "Null mesh realization request is invalid."));
                }
                return NextResourceInstance();
            }

            /** @copydoc IRenderBackend::CreateTexture */
            Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, const std::span<const std::byte> initialData,
                                                const RenderMemoryPlacement &placement) override {
                const auto bytes = RenderTextureBaseLevelByteSize(descriptor);
                if (const auto cost = QueryTextureMemoryCost(descriptor); !initialized_ || !bytes.has_value() ||
                                                                          (!initialData.empty() && initialData.size() != *bytes) ||
                                                                          cost.HasError() || !MatchesPlacement(cost.Value(), placement))
                    return Result<std::uint64_t>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidConfig, "Null texture realization request is invalid."));
                return NextResourceInstance();
            }

            Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, const std::uint64_t texture) override {
                if (!initialized_ || !descriptor.IsValid() || texture == 0)
                    return Result<std::uint64_t>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidConfig, "Null texture-view realization request is invalid."));
                return NextResourceInstance();
            }

            Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                     const std::uint64_t depthAttachment) override {
                if (!initialized_ || !descriptor.IsValid() || (colorAttachment == 0 && depthAttachment == 0))
                    return Result<std::uint64_t>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidConfig, "Null render-target realization request is invalid."));
                return NextResourceInstance();
            }

            /** @copydoc IRenderBackend::DestroyBuffer */
            void DestroyBuffer(std::uint64_t) noexcept override {
                // Null resources are opaque monotonic identities with no native allocation to release.
            }

            /** @copydoc IRenderBackend::DestroyMesh */
            void DestroyMesh(std::uint64_t) noexcept override {
                // Null resources are opaque monotonic identities with no native allocation to release.
            }

            void DestroyTexture(std::uint64_t) noexcept override {
                // Null resources are opaque monotonic identities with no native allocation to release.
            }

            void DestroyTextureView(std::uint64_t) noexcept override {
                // Null resources are opaque monotonic identities with no native allocation to release.
            }

            void DestroyRenderTarget(std::uint64_t) noexcept override {
                // Null resources are opaque monotonic identities with no native allocation to release.
            }

            /** @copydoc IRenderBackend::BeginFrame */
            Result<FrameToken> BeginFrame(const FrameDescriptor &descriptor) override {
                if (!initialized_) {
                    return Result<FrameToken>::Failure(
                        MakeBackendError(NullBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (frameActive_) {
                    return Result<FrameToken>::Failure(
                        MakeBackendError(NullBackendErrors::FrameAlreadyActive, "A renderer frame is already active."));
                }
                if (descriptor.frameNumber == 0 || !descriptor.outputExtent.IsValid()) {
                    return Result<FrameToken>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidFrameDescriptor, "Frame number and output extent must be valid."));
                }
                if (nextFrameToken_ == std::numeric_limits<std::uint64_t>::max()) {
                    return Result<FrameToken>::Failure(
                        MakeBackendError(NullBackendErrors::FrameTokenExhausted, "Frame token space is exhausted."));
                }

                frameActive_ = true;
                activeFrame_ = FrameToken{nextFrameToken_++};
                return Result<FrameToken>::Success(activeFrame_);
            }

            /** @copydoc IRenderBackend::Execute */
            Result<void> Execute(const RenderExecutionPlan &plan) override {
                if (!initialized_) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!frameActive_) {
                    return Result<void>::Failure(MakeBackendError(NullBackendErrors::NoActiveFrame, "No renderer frame is active."));
                }
                if (plan.frame != activeFrame_) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::FrameTokenMismatch, "Execution plan does not match the active frame."));
                }

                using enum RenderPassKind;
                for (std::size_t index = 0; index < plan.orderedPasses.size(); ++index) {
                    const RenderPassDescriptor &pass = plan.orderedPasses[index];
                    if (!pass.id.IsValid()) {
                        return Result<void>::Failure(MakeBackendError(NullBackendErrors::InvalidExecutionPlan,
                                                                      "Execution plan contains an invalid render pass ID."));
                    }
                    switch (pass.kind) {
                        case Graphics:
                        case Copy:
                            break;
                        case Compute:
                            if (!capabilities_.supportsCompute) {
                                return Result<void>::Failure(MakeBackendError(NullBackendErrors::UnsupportedPassKind,
                                                                              "Execution plan requires unsupported compute work."));
                            }
                            break;
                        default:
                            return Result<void>::Failure(MakeBackendError(NullBackendErrors::InvalidExecutionPlan,
                                                                          "Execution plan contains an invalid render pass kind."));
                    }
                    if (pass.primaryOutput.has_value()) {
                        if (pass.kind != Graphics) {
                            return Result<void>::Failure(MakeBackendError(NullBackendErrors::InvalidExecutionPlan,
                                                                          "Only graphics passes may bind the primary output attachment."));
                        }

                        if (!pass.primaryOutput->IsValid()) {
                            return Result<void>::Failure(MakeBackendError(NullBackendErrors::InvalidExecutionPlan,
                                                                          "Primary output attachment operations are invalid."));
                        }
                    }
                    for (std::size_t previous = 0; previous < index; ++previous) {
                        if (pass.id == plan.orderedPasses[previous].id) {
                            return Result<void>::Failure(MakeBackendError(NullBackendErrors::InvalidExecutionPlan,
                                                                          "Execution plan contains duplicate render pass IDs."));
                        }
                    }
                }

                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Present */
            Result<void> Present(FrameToken frame) override {
                if (!initialized_) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!frameActive_) {
                    return Result<void>::Failure(MakeBackendError(NullBackendErrors::NoActiveFrame, "No renderer frame is active."));
                }
                if (frame != activeFrame_) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::FrameTokenMismatch, "Frame token does not match the active frame."));
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
                frameActive_ = false;
                activeFrame_ = {};
            }

            /** @copydoc IRenderBackend::Resize */
            Result<void> Resize(FramebufferExtent extent) override {
                if (!initialized_) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::NotInitialized, "Renderer backend is not initialized."));
                }
                if (!extent.IsValid()) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::InvalidExtent, "Renderer output extent must be non-zero."));
                }
                if (frameActive_) {
                    return Result<void>::Failure(
                        MakeBackendError(NullBackendErrors::FrameActive, "Renderer output cannot resize while a frame is active."));
                }

                extent_ = extent;
                return Result<void>::Success();
            }

            /** @copydoc IRenderBackend::Shutdown */
            void Shutdown() noexcept override {
                frameActive_ = false;
                initialized_ = false;
                activeFrame_ = {};
                extent_ = {};
                config_ = {};
            }

        private:
            [[nodiscard]] Result<std::uint64_t> NextResourceInstance() {
                if (nextResourceInstance_ == std::numeric_limits<std::uint64_t>::max()) {
                    return Result<std::uint64_t>::Failure(
                        MakeBackendError(NullBackendErrors::ResourceInstanceExhausted, "Null resource instance space is exhausted."));
                }
                return Result<std::uint64_t>::Success(nextResourceInstance_++);
            }

            RenderBackendCapabilities capabilities_{
                .backend = RenderBackendId{"null"},
                .supportsBufferResources = true,
                .supportsMeshResources = true,
                .supportsTextureResources = true,
                .supportsRenderTargetResources = true,
            };
            RenderBackendConfig config_{};
            FramebufferExtent extent_{};
            FrameToken activeFrame_{};
            std::uint64_t nextFrameToken_{1};
            std::uint64_t nextResourceInstance_{1};
            bool initialized_{false};
            bool frameActive_{false};
        };

        /** @brief Provides inert null renderer instances without process side effects. */
        class NullBackendProvider final : public IRenderBackendProvider {
        public:
            Result<std::unique_ptr<IRenderBackend>> Create() const override {
                return Result<std::unique_ptr<IRenderBackend>>::Success(std::make_unique<NullRenderBackend>());
            }
        };
    }  // namespace

    /** @copydoc RegisterNullRenderBackend */
    Result<void> RegisterNullRenderBackend(RenderBackendRegistry &registry) {
        return registry.Register(RenderBackendDescriptor{
            .id = RenderBackendId{"null"},
            .displayName = "Null",
            .provider = std::make_unique<NullBackendProvider>(),
        });
    }
}  // namespace Horo::Render
