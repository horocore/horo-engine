#include "Horo/Runtime/Render/RenderFrontend.h"

#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderFrameGraphResources.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderParallelWorkErrors.h"
#include "RenderParallelWorkInternal.h"
#include "RenderResourceOperations.h"
#include "RenderResourceRegistry.h"
#include "RenderResourceUploadQueue.h"

#include <array>
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

        [[nodiscard]] Result<void> ValidateFrontendConfiguration(const RenderResourceUploadLimits &uploadLimits,
                                                                 const RenderFrontendMemoryConfig &memoryConfig,
                                                                 const RenderResourceRetirementLimits &retirementLimits) {
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

        /** @brief Releases one native backend instance for a retiring frontend resource. */
        void DestroyNativeResource(IRenderBackend &backend, const Detail::RenderResourceClass resourceClass,
                                   const std::uint64_t backendInstance) {
            using enum Detail::RenderResourceClass;
            if (resourceClass == Buffer)
                backend.DestroyBuffer(backendInstance);
            else if (resourceClass == Mesh)
                backend.DestroyMesh(backendInstance);
            else if (resourceClass == Texture)
                backend.DestroyTexture(backendInstance);
            else if (resourceClass == TextureView)
                backend.DestroyTextureView(backendInstance);
            else if (resourceClass == RenderTarget)
                backend.DestroyRenderTarget(backendInstance);
        }

        /** @brief Applies backend-release disposition and retires admitted memory when it remains valid. */
        void ReleaseResource(IRenderBackend &backend, RenderMemoryBudget &memoryBudget, const Detail::RenderResourceClass resourceClass,
                             const std::uint64_t backendInstance, const std::optional<RenderMemoryAllocationId> memoryAllocation,
                             const Detail::BackendResourceReleaseMode releaseMode) {
            if (releaseMode == Detail::BackendResourceReleaseMode::DestroyNative)
                DestroyNativeResource(backend, resourceClass, backendInstance);
            if (!memoryAllocation.has_value() || releaseMode == Detail::BackendResourceReleaseMode::NativeUnavailable)
                return;
            static_cast<void>(memoryBudget.BeginRetire(*memoryAllocation));
            static_cast<void>(memoryBudget.AcknowledgeRetirement(*memoryAllocation));
        }

    }  // namespace

    /** @copydoc RenderFrameScope::~RenderFrameScope */
    RenderFrameScope::~RenderFrameScope() {
        Abort();
    }

    /** @copydoc RenderFrameScope::RenderFrameScope(RenderFrameScope&&) */
    RenderFrameScope::RenderFrameScope(RenderFrameScope &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), backend_(std::exchange(other.backend_, nullptr)),
          frame_(std::exchange(other.frame_, {})), executed_(std::exchange(other.executed_, false)),
          parallelWork_(std::move(other.parallelWork_)), parallelGraphWork_(std::move(other.parallelGraphWork_)) {
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
            parallelWork_ = std::move(other.parallelWork_);
            parallelGraphWork_ = std::move(other.parallelGraphWork_);
            if (owner_ != nullptr) {
                owner_->activeFrameScope_ = this;
            }
        }
        return *this;
    }

    /** @copydoc RenderFrameScope::Execute */
    Result<void> RenderFrameScope::Execute(const std::span<const RenderPassDescriptor> orderedPasses) {
        if (const auto admitted = ValidateExecutionAdmission(); admitted.HasError())
            return admitted;

        try {
            for (const RenderPassDescriptor &pass : orderedPasses) {
                if (const auto valid = ValidateStaticMeshBinding(pass); valid.HasError()) {
                    Abort();
                    return valid;
                }
                if (!pass.staticMesh)
                    continue;
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

    /** @copydoc RenderFrameScope::ExecuteGraph */
    Result<void> RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                const std::span<const RenderGraphPassWorkload> workloads) {
        return ExecuteGraphInternal(graph, workloads, nullptr);
    }

    /** @copydoc RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &, std::span<const RenderGraphPassWorkload>,
     * UiRenderSubmission) */
    Result<void> RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission ui) {
        return ExecuteGraphInternal(graph, workloads, &ui);
    }

    /** @copydoc RenderFrameScope::ExecuteGraphInternal */
    Result<void> RenderFrameScope::ExecuteGraphInternal(const CompiledRenderGraphExecution &graph,
                                                        const std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission *ui,
                                                        const RenderGraphTransientResourcesHandle transientResources) {
        if (const auto admitted = ValidateExecutionAdmission(); admitted.HasError())
            return admitted;
        try {
            Detail::RenderGraphTransientResourceSet *transient = nullptr;
            if (transientResources.IsValid()) {
                const auto found = owner_->graphTransientResources_->Resolve(transientResources, graph);
                if (found.HasError()) {
                    Abort();
                    return Result<void>::Failure(found.ErrorValue());
                }
                transient = found.Value();
            }
            const auto resolved = Detail::ResolveFrameGraphResources(graph, *owner_->resourceRegistry_, transient);
            if (resolved.HasError()) {
                Abort();
                return Result<void>::Failure(resolved.ErrorValue());
            }
            const auto leased = owner_->graphResourceLeases_->Acquire(graph.Resources(), ui, transient);
            if (leased.HasError()) {
                Abort();
                return Result<void>::Failure(leased.ErrorValue());
            }
            RenderGraphLeaseTransfer transfer;
            const auto releaseLease = [&transfer](IRenderGraphResourceLease *lease) {
                if (transfer.authority == RenderGraphLeaseAuthority::Frontend)
                    lease->Release();
            };
            std::unique_ptr<IRenderGraphResourceLease, decltype(releaseLease)> lease{leased.Value(), releaseLease};
            if (const auto result = backend_->ExecuteGraph(
                    {frame_, graph, workloads, resolved.Value().View(), lease.get(), transient != nullptr, &transfer});
                result.HasError()) {
                Abort();
                return result;
            }
            static_cast<void>(lease.release());
            executed_ = true;
            return Result<void>::Success();
        } catch (...) {  // NOSONAR(cpp:S2738) Frame ownership must recover across an arbitrary backend exception.
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &, std::span<const RenderGraphPassWorkload>,
     * RenderGraphTransientResourcesHandle) */
    Result<void> RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                const std::span<const RenderGraphPassWorkload> workloads,
                                                const RenderGraphTransientResourcesHandle transientResources) {
        if (!transientResources.IsValid()) {
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        }
        return ExecuteGraphInternal(graph, workloads, nullptr, transientResources);
    }

    /** @copydoc RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &, std::span<const RenderGraphPassWorkload>,
     * RenderGraphTransientResourcesHandle, UiRenderSubmission) */
    Result<void> RenderFrameScope::ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                const std::span<const RenderGraphPassWorkload> workloads,
                                                const RenderGraphTransientResourcesHandle transientResources, UiRenderSubmission ui) {
        if (!transientResources.IsValid()) {
            Abort();
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceHandleMalformed));
        }
        return ExecuteGraphInternal(graph, workloads, &ui, transientResources);
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
        parallelWork_.reset();
        parallelGraphWork_.reset();
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
                                                                   const RenderResourceUploadLimits &uploadLimits,
                                                                   const RenderFrontendMemoryConfig &memoryConfig,
                                                                   const RenderResourceRetirementLimits &retirementLimits) {
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
                                   const RenderResourceUploadLimits &uploadLimits, std::unique_ptr<RenderMemoryBudget> memoryBudget,
                                   const RenderFrontendMemoryConfig &memoryConfig, const RenderResourceRetirementLimits &retirementLimits,
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
                                                      ReleaseResource(*backend_, *memoryBudget_, resourceClass, backendInstance,
                                                                      memoryAllocation, releaseMode);
                                                  })),
          resourceUploadQueue_(std::make_unique<Detail::RenderResourceUploadQueue>(uploadLimits)),
          graphResourceLeases_(
              std::make_unique<Detail::RenderGraphResourceLeasePool>(*resourceRegistry_, retirementLimits.maximumSubmissionPins)),
          graphTransientResources_(
              std::make_unique<Detail::RenderGraphTransientResourcePool>(*backend_, *resourceRegistry_, *memoryBudget_)),
          inspectionFeed_(resourceOwner) {}

    /** @copydoc RenderFrontend::~RenderFrontend */
    RenderFrontend::~RenderFrontend() {
        static_cast<void>(inspectionFeed_.Shutdown());
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
            // Drain every now-empty backing block before invalidating the ledger.
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

    /** @copydoc RenderFrontend::PrepareTransientGraphResources */
    Result<RenderGraphTransientResourcesHandle> RenderFrontend::PrepareTransientGraphResources(const RenderGraphLifetimePlan &plan,
                                                                                               const RenderMemoryScopeId scope) {
        using PrepareResult = Result<RenderGraphTransientResourcesHandle>;
        if (activeFrameScope_ != nullptr)
            return PrepareResult::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        try {
            auto prepared = graphTransientResources_->Prepare(plan, scope);
            static_cast<void>(memoryBudget_->ReclaimEmptyBlocks(memoryConfig_.budget.maximumBlocks));
            return prepared;
        } catch (const std::bad_alloc &) {
            static_cast<void>(memoryBudget_->ReclaimEmptyBlocks(memoryConfig_.budget.maximumBlocks));
            return PrepareResult::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        } catch (const std::length_error &) {
            static_cast<void>(memoryBudget_->ReclaimEmptyBlocks(memoryConfig_.budget.maximumBlocks));
            return PrepareResult::Failure(MakeError(FrontendErrors::ResourceCapacityExhausted));
        }
    }

    /** @copydoc RenderFrontend::ReleaseTransientGraphResources */
    Result<void> RenderFrontend::ReleaseTransientGraphResources(const RenderGraphTransientResourcesHandle resources) {
        if (activeFrameScope_ != nullptr)
            return Result<void>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        const auto released = graphTransientResources_->Release(resources);
        static_cast<void>(memoryBudget_->ReclaimEmptyBlocks(memoryConfig_.budget.maximumBlocks));
        return released;
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
