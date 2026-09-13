#include "Horo/Runtime/Render/RenderFrontend.h"

#include "RenderFrontendErrors.h"
#include "RenderResourceOperations.h"
#include "RenderResourceRegistry.h"
#include "RenderResourceUploadQueue.h"

#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        [[nodiscard]] Error MakeFrontendError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] Result<void> ValidateFrontendConfiguration(const RenderResourceUploadLimits uploadLimits,
                                                                 const RenderFrontendMemoryConfig memoryConfig,
                                                                 const RenderResourceRetirementLimits retirementLimits) {
            if (!uploadLimits.IsValid()) {
                return Result<void>::Failure(
                    MakeFrontendError(FrontendErrors::InvalidResourceUploadLimits, "Renderer resource upload limits are invalid."));
            }
            if (!memoryConfig.IsValid()) {
                return Result<void>::Failure(
                    MakeFrontendError(FrontendErrors::InvalidMemoryConfig, "Renderer memory admission limits or scope are invalid."));
            }
            if (!retirementLimits.IsValid()) {
                return Result<void>::Failure(MakeFrontendError(FrontendErrors::InvalidResourceRetirementLimits,
                                                               "Renderer resource retirement limits must use finite non-zero completion, "
                                                               "queue, and destruction bounds."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::unique_ptr<IRenderBackend>> CreateInitializedBackend(const RenderBackendRegistry &registry,
                                                                                       const RenderBackendId &backendId,
                                                                                       const RenderBackendConfig &config) {
            auto created = registry.Create(backendId);
            if (created.HasError()) {
                return Result<std::unique_ptr<IRenderBackend>>::Failure(created.ErrorValue());
            }

            std::unique_ptr<IRenderBackend> backend = std::move(created).Value();
            try {
                if (const Result<void> initialized = backend->Initialize(config); initialized.HasError()) {
                    backend->Shutdown();
                    return Result<std::unique_ptr<IRenderBackend>>::Failure(initialized.ErrorValue());
                }
            } catch (...) {  // NOSONAR(cpp:S2738)
                backend->Shutdown();
                return Result<std::unique_ptr<IRenderBackend>>::Failure(
                    MakeFrontendError(FrontendErrors::InitializeException, "Renderer backend initialization threw."));
            }
            return Result<std::unique_ptr<IRenderBackend>>::Success(std::move(backend));
        }

    }  // namespace

    /** @copydoc RenderFrameScope::~RenderFrameScope */
    RenderFrameScope::~RenderFrameScope() {
        Abort();
    }

    /** @copydoc RenderFrameScope::RenderFrameScope(RenderFrameScope&&) */
    RenderFrameScope::RenderFrameScope(RenderFrameScope &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), backend_(std::exchange(other.backend_, nullptr)),
          frame_(std::exchange(other.frame_, {})), executed_(std::exchange(other.executed_, false)) {
        if (owner_ != nullptr) {
            owner_->activeFrameScope_ = this;
        }
    }

    /** @copydoc RenderFrameScope::operator=(RenderFrameScope&&) */
    RenderFrameScope &RenderFrameScope::operator=(RenderFrameScope &&other) noexcept {
        if (this != &other) {
            Abort();
            owner_ = std::exchange(other.owner_, nullptr);
            backend_ = std::exchange(other.backend_, nullptr);
            frame_ = std::exchange(other.frame_, {});
            executed_ = std::exchange(other.executed_, false);
            if (owner_ != nullptr) {
                owner_->activeFrameScope_ = this;
            }
        }
        return *this;
    }

    /** @copydoc RenderFrameScope::Execute */
    Result<void> RenderFrameScope::Execute(const std::span<const RenderPassDescriptor> orderedPasses) {
        if (backend_ == nullptr) {
            return Result<void>::Failure(MakeFrontendError(FrontendErrors::FrameNotActive, "Renderer frame scope no longer owns a frame."));
        }
        if (executed_) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::FrameAlreadyExecuted, "Renderer frame scope has already executed its pass sequence."));
        }

        try {
            for (const RenderPassDescriptor &pass : orderedPasses) {
                if (pass.primaryOutput.has_value() && pass.staticMesh.has_value()) {
                    Abort();
                    return Result<void>::Failure(
                        MakeFrontendError(FrontendErrors::AmbiguousPassWorkload,
                                          "A render pass cannot bind primary-output and static-mesh workloads together."));
                }
                if (!pass.staticMesh.has_value()) {
                    continue;
                }
                if (owner_->staticMeshPassExecutor_ == nullptr) {
                    Abort();
                    return Result<void>::Failure(MakeFrontendError(FrontendErrors::StaticMeshExecutorMissing,
                                                                   "Static-mesh pass requires an attached backend executor."));
                }
                if (!pass.staticMesh->IsValid()) {
                    Abort();
                    return Result<void>::Failure(
                        MakeFrontendError(FrontendErrors::InvalidStaticMeshPass, "Static-mesh pass descriptor is invalid."));
                }
                if (!owner_->IsLiveTarget(pass.staticMesh->target, pass.staticMesh->extent)) {
                    Abort();
                    return Result<void>::Failure(
                        MakeFrontendError(FrontendErrors::StaleRenderTarget,
                                          "Static-mesh pass references a stale target or mismatched target extent."));
                }
                const Result<void> staticMeshExecuted = owner_->staticMeshPassExecutor_->ExecuteStaticMeshPass(*pass.staticMesh);
                if (staticMeshExecuted.HasError()) {
                    Abort();
                    return Result<void>::Failure(staticMeshExecuted.ErrorValue());
                }
            }
            if (const Result<void> executed = backend_->Execute(RenderExecutionPlan{.frame = frame_, .orderedPasses = orderedPasses});
                executed.HasError()) {
                Abort();
                return Result<void>::Failure(executed.ErrorValue());
            }
            executed_ = true;
            return Result<void>::Success();
        } catch (...) {  // NOSONAR(cpp:S2738)
            Abort();
            return Result<void>::Failure(MakeFrontendError(FrontendErrors::FrameException, "Renderer backend frame operation threw."));
        }
    }

    /** @copydoc RenderFrameScope::Present */
    Result<void> RenderFrameScope::Present() {
        if (backend_ == nullptr) {
            return Result<void>::Failure(MakeFrontendError(FrontendErrors::FrameNotActive, "Renderer frame scope no longer owns a frame."));
        }
        if (!executed_) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::FrameNotExecuted, "Renderer frame scope must execute before presentation."));
        }

        try {
            if (const Result<void> presented = backend_->Present(frame_); presented.HasError()) {
                Abort();
                return Result<void>::Failure(presented.ErrorValue());
            }
            Release();
            return Result<void>::Success();
        } catch (...) {  // NOSONAR(cpp:S2738)
            Abort();
            return Result<void>::Failure(MakeFrontendError(FrontendErrors::FrameException, "Renderer backend frame operation threw."));
        }
    }

    /** @copydoc RenderFrameScope::Cancel */
    void RenderFrameScope::Cancel() noexcept {
        Abort();
    }

    RenderFrameScope::RenderFrameScope(RenderFrontend &owner, IRenderBackend &backend, const FrameToken frame) noexcept
        : owner_(&owner), backend_(&backend), frame_(frame) {
        owner.activeFrameScope_ = this;
    }

    void RenderFrameScope::Abort() noexcept {
        if (backend_ != nullptr) {
            backend_->AbortFrame(frame_);
        }
        Release();
    }

    void RenderFrameScope::Release() noexcept {
        if (owner_ != nullptr && owner_->activeFrameScope_ == this) {
            owner_->activeFrameScope_ = nullptr;
        }
        owner_ = nullptr;
        backend_ = nullptr;
        frame_ = {};
        executed_ = false;
    }

    /** @copydoc RenderFrontend::Create */
    Result<std::unique_ptr<RenderFrontend>> RenderFrontend::Create(const RenderBackendRegistry &registry, const RenderBackendId &backendId,
                                                                   const RenderBackendConfig &config,
                                                                   const RenderResourceUploadLimits uploadLimits,
                                                                   const RenderFrontendMemoryConfig memoryConfig,
                                                                   const RenderResourceRetirementLimits retirementLimits) {
        if (const Result<void> valid = ValidateFrontendConfiguration(uploadLimits, memoryConfig, retirementLimits); valid.HasError()) {
            return Result<std::unique_ptr<RenderFrontend>>::Failure(valid.ErrorValue());
        }
        auto initializedBackend = CreateInitializedBackend(registry, backendId, config);
        if (initializedBackend.HasError()) {
            return Result<std::unique_ptr<RenderFrontend>>::Failure(initializedBackend.ErrorValue());
        }
        std::unique_ptr<IRenderBackend> backend = std::move(initializedBackend).Value();

        auto resourceOwner = Detail::AcquireRenderResourceOwnerId();
        if (resourceOwner.HasError()) {
            backend->Shutdown();
            return Result<std::unique_ptr<RenderFrontend>>::Failure(resourceOwner.ErrorValue());
        }
        auto memoryBudget = RenderMemoryBudget::Create(resourceOwner.Value(), memoryConfig.budget);
        if (memoryBudget.HasError()) {
            backend->Shutdown();
            return Result<std::unique_ptr<RenderFrontend>>::Failure(memoryBudget.ErrorValue());
        }
        try {
            return Result<std::unique_ptr<RenderFrontend>>::Success(
                std::make_unique<RenderFrontend>(std::move(backend), resourceOwner.Value(), uploadLimits, std::move(memoryBudget).Value(),
                                                 memoryConfig, retirementLimits, ConstructionKey{}));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<RenderFrontend>>::Failure(
                MakeFrontendError(FrontendErrors::ResourceCapacityExhausted, "Renderer frontend bounded queue storage allocation failed."));
        } catch (const std::length_error &) {
            return Result<std::unique_ptr<RenderFrontend>>::Failure(
                MakeFrontendError(FrontendErrors::ResourceCapacityExhausted,
                                  "Renderer frontend bounded queue limits exceed supported storage sizes."));
        }
    }

    RenderFrontend::RenderFrontend(std::unique_ptr<IRenderBackend> backend, const RenderResourceOwnerId resourceOwner,
                                   const RenderResourceUploadLimits uploadLimits, std::unique_ptr<RenderMemoryBudget> memoryBudget,
                                   const RenderFrontendMemoryConfig memoryConfig, const RenderResourceRetirementLimits retirementLimits,
                                   ConstructionKey)
        : backend_(std::move(backend)), memoryBudget_(std::move(memoryBudget)), memoryConfig_(memoryConfig),
          resourceRegistry_(
              std::make_unique<
                  Detail::RenderResourceRegistry>(resourceOwner,
                                                  Detail::RenderResourceRegistryLimits{.retirementDrainBudget =
                                                                                           retirementLimits.maximumRetirementsPerDrain,
                                                                                       .maximumSubmissionPins =
                                                                                           retirementLimits.maximumSubmissionPins,
                                                                                       .maximumTrackedQueues =
                                                                                           retirementLimits.maximumTrackedQueues,
                                                                                       .completionDrainBudget =
                                                                                           retirementLimits.maximumCompletionsPerDrain},
                                                  [this](const Detail::RenderResourceClass resourceClass,
                                                         const std::uint64_t backendInstance,
                                                         const std::optional<RenderMemoryAllocationId> memoryAllocation,
                                                         const Detail::BackendResourceReleaseMode releaseMode) {
                                                      using enum Detail::RenderResourceClass;
                                                      if (releaseMode == Detail::BackendResourceReleaseMode::DestroyNative) {
                                                          if (resourceClass == Buffer) {
                                                              backend_->DestroyBuffer(backendInstance);
                                                          } else if (resourceClass == Mesh) {
                                                              backend_->DestroyMesh(backendInstance);
                                                          } else if (resourceClass == Texture) {
                                                              backend_->DestroyTexture(backendInstance);
                                                          } else if (resourceClass == TextureView) {
                                                              backend_->DestroyTextureView(backendInstance);
                                                          } else if (resourceClass == RenderTarget) {
                                                              backend_->DestroyRenderTarget(backendInstance);
                                                          }
                                                      }
                                                      if (memoryAllocation.has_value() &&
                                                          releaseMode != Detail::BackendResourceReleaseMode::NativeUnavailable) {
                                                          static_cast<void>(memoryBudget_->BeginRetire(*memoryAllocation));
                                                          static_cast<void>(memoryBudget_->AcknowledgeRetirement(*memoryAllocation));
                                                      }
                                                  })),
          resourceUploadQueue_(std::make_unique<Detail::RenderResourceUploadQueue>(uploadLimits)) {}

    /** @copydoc RenderFrontend::~RenderFrontend */
    RenderFrontend::~RenderFrontend() {
        if (activeFrameScope_ != nullptr) {
            activeFrameScope_->Abort();
        }
        resourceUploadQueue_->StopAdmission();
        while (!resourceUploadQueue_->Empty()) {
            const Detail::RenderResourceUploadQueue::Request request = resourceUploadQueue_->Pop();
            if (request.memoryReservation.IsValid())
                static_cast<void>(memoryBudget_->Cancel(request.memoryReservation));
        }
        backend_->Shutdown();
        resourceRegistry_->Shutdown(Detail::BackendResourceReleaseMode::NativeAlreadyReleased);
        while (memoryBudget_->ReclaimEmptyBlocks(memoryConfig_.budget.maximumBlocks) != 0) {
        }
        memoryBudget_->Shutdown();
    }

    /** @copydoc RenderFrontend::Capabilities */
    const RenderBackendCapabilities &RenderFrontend::Capabilities() const noexcept {
        return backend_->Capabilities();
    }

    /** @copydoc RenderFrontend::MemorySnapshot */
    RenderMemoryBudgetSnapshot RenderFrontend::MemorySnapshot() const noexcept {
        return memoryBudget_->Snapshot();
    }

    /** @copydoc RenderFrontend::BeginFrame */
    Result<RenderFrameScope> RenderFrontend::BeginFrame(const FrameDescriptor &descriptor) {
        if (activeFrameScope_ != nullptr) {
            return Result<RenderFrameScope>::Failure(
                MakeFrontendError(FrontendErrors::FrameAlreadyActive, "Renderer frontend already owns an active frame scope."));
        }

        if (const Result<std::size_t> processed = ProcessResourceRequests(); processed.HasError()) {
            return Result<RenderFrameScope>::Failure(processed.ErrorValue());
        }

        try {
            auto begun = backend_->BeginFrame(descriptor);
            if (begun.HasError()) {
                backend_->AbortActiveFrame();
                return Result<RenderFrameScope>::Failure(begun.ErrorValue());
            }

            const FrameToken frame = begun.Value();
            if (!frame.IsValid()) {
                backend_->AbortActiveFrame();
                return Result<RenderFrameScope>::Failure(
                    MakeFrontendError(FrontendErrors::InvalidFrameToken, "Renderer backend returned an invalid frame token."));
            }
            return Result<RenderFrameScope>::Success(RenderFrameScope{*this, *backend_, frame});
        } catch (...) {  // NOSONAR(cpp:S2738)
            backend_->AbortActiveFrame();
            return Result<RenderFrameScope>::Failure(
                MakeFrontendError(FrontendErrors::FrameException, "Renderer backend frame operation threw."));
        }
    }

    /** @copydoc RenderFrontend::SubmitFrame */
    Result<void> RenderFrontend::SubmitFrame(const FrameDescriptor &descriptor, const std::span<const RenderPassDescriptor> orderedPasses) {
        auto begun = BeginFrame(descriptor);
        if (begun.HasError()) {
            return Result<void>::Failure(begun.ErrorValue());
        }

        RenderFrameScope frame = std::move(begun).Value();
        if (const Result<void> executed = frame.Execute(orderedPasses); executed.HasError()) {
            return Result<void>::Failure(executed.ErrorValue());
        }
        return frame.Present();
    }

    /** @copydoc RenderFrontend::Resize */
    Result<void> RenderFrontend::Resize(const FramebufferExtent extent) {
        if (activeFrameScope_ != nullptr) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::ResizeDuringFrame, "Renderer output cannot be resized during an active frame."));
        }

        try {
            return backend_->Resize(extent);
        } catch (...) {  // NOSONAR(cpp:S2738)
            return Result<void>::Failure(MakeFrontendError(FrontendErrors::ResizeException, "Renderer backend resize operation threw."));
        }
    }

    /** @copydoc RenderFrontend::AttachStaticMeshPassExecutor */
    Result<void> RenderFrontend::AttachStaticMeshPassExecutor(IStaticMeshPassExecutor &executor) {
        if (activeFrameScope_ != nullptr) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::ExecutorChangeDuringFrame, "Render pass executor cannot change during a frame."));
        }
        if (staticMeshPassExecutor_ != nullptr && staticMeshPassExecutor_ != &executor) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::StaticMeshExecutorAlreadyAttached, "A static-mesh pass executor is already attached."));
        }
        staticMeshPassExecutor_ = &executor;
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::DetachStaticMeshPassExecutor */
    void RenderFrontend::DetachStaticMeshPassExecutor(const IStaticMeshPassExecutor &executor) noexcept {
        if (activeFrameScope_ == nullptr && staticMeshPassExecutor_ == &executor) {
            staticMeshPassExecutor_ = nullptr;
        }
    }

}  // namespace Horo::Render
