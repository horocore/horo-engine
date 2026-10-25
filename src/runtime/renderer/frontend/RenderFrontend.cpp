#include "Horo/Runtime/Render/RenderFrontend.h"

#include "Horo/Runtime/Render/FramePacingErrors.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderFrontendErrors.h"
#include "RenderGraphResourceLeasePool.h"
#include "RenderGraphTransientResourcePool.h"
#include "RenderParallelWorkErrors.h"
#include "RenderParallelWorkInternal.h"
#include "RenderResourceOperations.h"
#include "RenderResourceRegistry.h"
#include "RenderResourceUploadQueue.h"
#include "RenderSubmissionAllocator.h"

#include <algorithm>
#include <array>
#include <exception>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Failed initialization has no submitted work; unresolved rollback cannot be destroyed safely. */
        void CloseUnsubmittedBackend(IRenderBackend &backend) noexcept {
            try {
                const auto begun = backend.BeginShutdown();
                const auto drained = backend.PollDrain();
                if (begun.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                    std::terminate();
            } catch (...) {
                std::terminate();
            }
        }

        /** @brief Contains arbitrary foreign backend exceptions without allocating a replacement diagnostic. */
        [[nodiscard]] std::optional<Result<bool>> ObserveBackendSubmission(IRenderBackend &backend, const RenderCpuWait &wait) noexcept {
            try {
                return backend.ObserveSubmission(wait);
            } catch (...) {
                return std::nullopt;
            }
        }

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
                    CloseUnsubmittedBackend(*backend);
                    return Result<std::unique_ptr<IRenderBackend>>::Failure(initialized.ErrorValue());
                }
            } catch (...) {  // NOSONAR(cpp:S2738)
                CloseUnsubmittedBackend(*backend);
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
            else if (resourceClass == ShaderModule)
                backend.DestroyShaderModule(backendInstance);
            else if (resourceClass == Pipeline)
                backend.DestroyGraphicsPipeline(backendInstance);
            else if (resourceClass == Sampler)
                backend.DestroySampler(backendInstance);
        }

        /** @brief Requests native retirement; only the registry acknowledges its existing memory charge after lease completion. */
        void ReleaseResource(IRenderBackend &backend, const Detail::RenderResourceClass resourceClass, const std::uint64_t backendInstance,
                             const Detail::BackendResourceReleaseMode releaseMode) {
            if (releaseMode == Detail::BackendResourceReleaseMode::DestroyNative)
                DestroyNativeResource(backend, resourceClass, backendInstance);
        }

    }  // namespace

    /** @copydoc RenderFrameScope::~RenderFrameScope */
    RenderFrameScope::~RenderFrameScope() {
        Abort();
    }

    /** @copydoc RenderFrameScope::RenderFrameScope(RenderFrameScope&&) */
    RenderFrameScope::RenderFrameScope(RenderFrameScope &&other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), backend_(std::exchange(other.backend_, nullptr)),
          frame_(std::exchange(other.frame_, {})), hostFrame_(std::exchange(other.hostFrame_, 0)),
          executed_(std::exchange(other.executed_, false)), guiComposed_(std::exchange(other.guiComposed_, false)),
          completionLease_(std::exchange(other.completionLease_, nullptr)), parallelWork_(std::move(other.parallelWork_)),
          parallelGraphWork_(std::move(other.parallelGraphWork_)) {
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
            hostFrame_ = std::exchange(other.hostFrame_, 0);
            executed_ = std::exchange(other.executed_, false);
            guiComposed_ = std::exchange(other.guiComposed_, false);
            completionLease_ = std::exchange(other.completionLease_, nullptr);
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

    /** @copydoc RenderFrontend::PollNativePresentTiming */
    Result<std::optional<NativePresentTiming>> RenderFrontend::PollNativePresentTiming() {
        try {
            return backend_->PollNativePresentTiming();
        } catch (...) {  // NOSONAR(cpp:S2738) Backend exceptions must not cross the host boundary.
            return Result<std::optional<NativePresentTiming>>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrameScope::ReserveSubmission */
    Result<RenderQueueSubmission> RenderFrameScope::ReserveSubmission(const RenderQueueRole role,
                                                                      const std::span<const RenderTimelinePoint> waits) {
        if (owner_ == nullptr || backend_ == nullptr)
            return Result<RenderQueueSubmission>::Failure(MakeError(FrontendErrors::FrameNotActive));
        if (auto owner = owner_->RequireOpenOwner(); owner.HasError())
            return Result<RenderQueueSubmission>::Failure(owner.ErrorValue());
        return owner_->submissionAllocator_->Reserve(role, waits);
    }

    /** @copydoc RenderFrameScope::Present */
    Result<void> RenderFrameScope::Present(const PresentationTimingRequest *timing) {
        if (backend_ == nullptr) {
            return Result<void>::Failure(MakeFrontendError(FrontendErrors::FrameNotActive, "Renderer frame scope no longer owns a frame."));
        }
        if (!executed_) {
            return Result<void>::Failure(
                MakeFrontendError(FrontendErrors::FrameNotExecuted, "Renderer frame scope must execute before presentation."));
        }

        if (timing != nullptr && (!timing->surface.IsAttachedGeneration() || timing->frameNumber != hostFrame_))
            return Result<void>::Failure(MakeError(FramePacingErrors::InvalidSurface));
        try {
            if (const Result<void> presented = timing != nullptr ? backend_->PresentWithTiming(frame_, *timing) : backend_->Present(frame_);
                presented.HasError()) {
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

    RenderFrameScope::RenderFrameScope(RenderFrontend &owner, IRenderBackend &backend, const FrameToken frame,
                                       const std::uint64_t hostFrame) noexcept
        : owner_(&owner), backend_(&backend), frame_(frame), hostFrame_(hostFrame) {
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
        guiComposed_ = false;
        completionLease_ = nullptr;
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
            CloseUnsubmittedBackend(*backend);
            return Result<std::unique_ptr<RenderFrontend>>::Failure(resourceOwner.ErrorValue());
        }
        auto memoryBudget = RenderMemoryBudget::Create(resourceOwner.Value(), memoryConfig.budget);
        if (memoryBudget.HasError()) {
            CloseUnsubmittedBackend(*backend);
            return Result<std::unique_ptr<RenderFrontend>>::Failure(memoryBudget.ErrorValue());
        }
        try {
            auto frontend =
                std::make_unique<RenderFrontend>(std::move(backend), resourceOwner.Value(), uploadLimits, std::move(memoryBudget).Value(),
                                                 memoryConfig, retirementLimits, ConstructionKey{});
            if (auto configured = frontend->submissionAllocator_->Configure(config.queueAssignments); configured.HasError())
                return Result<std::unique_ptr<RenderFrontend>>::Failure(configured.ErrorValue());
            return Result<std::unique_ptr<RenderFrontend>>::Success(std::move(frontend));
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
        : backend_(std::move(backend)), submissionAllocator_(std::make_unique<Detail::RenderSubmissionAllocator>()),
          memoryBudget_(std::move(memoryBudget)), memoryConfig_(memoryConfig),
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
                                                         const std::uint64_t backendInstance, const std::optional<RenderMemoryAllocationId>,
                                                         const Detail::BackendResourceReleaseMode releaseMode) {
                                                      ReleaseResource(*backend_, resourceClass, backendInstance, releaseMode);
                                                  },
                                                  memoryBudget_.get())),
          resourceUploadQueue_(std::make_unique<Detail::RenderResourceUploadQueue>(uploadLimits)),
          graphResourceLeases_(
              std::make_unique<Detail::RenderGraphResourceLeasePool>(*resourceRegistry_, retirementLimits.maximumSubmissionPins)),
          graphTransientResources_(
              std::make_unique<Detail::RenderGraphTransientResourcePool>(*backend_, *resourceRegistry_, *memoryBudget_)),
          inspectionFeed_(resourceOwner) {}

    /** @copydoc RenderFrontend::~RenderFrontend */
    RenderFrontend::~RenderFrontend() {
        try {
            const auto begun = BeginShutdown();
            const auto drained = PollDrain();
            if (begun.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                std::terminate();
        } catch (...) {
            std::terminate();
        }
    }

    /** @copydoc RenderFrontend::RequireOpenOwner */
    Result<void> RenderFrontend::RequireOpenOwner() const {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<void>::Failure(MakeError(FrontendErrors::WrongThread));
        if (closing_)
            return Result<void>::Failure(MakeError(FrontendErrors::Closing));
        return Result<void>::Success();
    }

    /** @copydoc RenderFrontend::ObserveSubmission */
    Result<bool> RenderFrontend::ObserveSubmission(const RenderCpuWait &wait) {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<bool>::Failure(MakeError(FrontendErrors::WrongThread));
        if (drained_)
            return Result<bool>::Failure(MakeError(FrontendErrors::Closing, "Native submission ownership is already drained."));
        if (!wait.IsValid() || wait.expectedDeviceIncarnation == 0 ||
            wait.expectedDeviceIncarnation != backend_->Capabilities().support.deviceIncarnation ||
            (activeFrameScope_ != nullptr && wait.purpose != RenderCpuWaitPurpose::Poll))
            return Result<bool>::Failure(MakeError(RenderGraphExecutionErrors::InvalidSynchronization,
                                                   "Active frames permit only zero-time GPU completion polling; "
                                                   "blocking purposes require a valid finite owner policy."));
        if (auto observed = ObserveBackendSubmission(*backend_, wait); observed.has_value())
            return std::move(*observed);
        return Result<bool>::Failure(MakeError(FrontendErrors::FrameException));
    }

    /** @copydoc RenderFrontend::BeginShutdown */
    Result<void> RenderFrontend::BeginShutdown() {
        if (std::this_thread::get_id() != preparationOwner_)
            return Result<void>::Failure(MakeError(FrontendErrors::WrongThread));
        if (drained_)
            return Result<void>::Success();
        closing_ = true;
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
        return backend_->BeginShutdown();
    }

    /** @copydoc RenderFrontend::PollDrain */
    Result<RenderBackendDrainProgress> RenderFrontend::PollDrain() {
        using enum RenderBackendDrainProgress;
        using DrainResult = Result<RenderBackendDrainProgress>;
        if (std::this_thread::get_id() != preparationOwner_)
            return DrainResult::Failure(MakeError(FrontendErrors::WrongThread));
        if (drained_)
            return DrainResult::Success(Drained);
        if (!closing_)
            return DrainResult::Failure(MakeError(FrontendErrors::Closing, "BeginShutdown must close admission before drain polling."));
        if (const auto progress = backend_->PollDrain(); progress.HasError() || progress.Value() == Pending)
            return progress;
        resourceRegistry_->Shutdown(Detail::BackendResourceReleaseMode::NativeAlreadyReleased);
        if (resourceRegistry_->HasPendingNativeRetirements())
            return DrainResult::Success(Pending);
        while (memoryBudget_->ReclaimEmptyBlocks(memoryConfig_.budget.maximumBlocks) != 0) {
            // Drain every now-empty backing block before invalidating the ledger.
        }
        memoryBudget_->Shutdown();
        drained_ = true;
        return DrainResult::Success(Drained);
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
        if (auto admission = RequireOpenOwner(); admission.HasError())
            return PrepareResult::Failure(std::move(admission).ErrorValue());
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

    /** @copydoc RenderFrontend::PollPresentationAvailability */
    Result<bool> RenderFrontend::PollPresentationAvailability(FramebufferExtent extent) {
        if (const auto admission = RequireOpenOwner(); admission.HasError())
            return Result<bool>::Failure(admission.ErrorValue());
        if (activeFrameScope_ != nullptr)
            return Result<bool>::Failure(MakeError(FrontendErrors::FrameAlreadyActive));
        try {
            return backend_->PollPresentationAvailability(extent);
        } catch (...) {  // NOSONAR(cpp:S2738) Native exception containment boundary.
            return Result<bool>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrontend::ConfigurePresentationOutput */
    Result<RenderPresentationOutput> RenderFrontend::ConfigurePresentationOutput(const RenderPresentationOutputRequest &request) {
        if (const auto admission = RequireOpenOwner(); admission.HasError())
            return Result<RenderPresentationOutput>::Failure(admission.ErrorValue());
        if (activeFrameScope_ != nullptr)
            return Result<RenderPresentationOutput>::Failure(MakeError(FrontendErrors::ResourceChangeDuringFrame));
        try {
            return backend_->ConfigurePresentationOutput(request);
        } catch (...) {  // NOSONAR(cpp:S2738) Native exception containment boundary.
            return Result<RenderPresentationOutput>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrontend::PresentationOutput */
    Result<RenderPresentationOutput> RenderFrontend::PresentationOutput() const {
        if (const auto admission = RequireOpenOwner(); admission.HasError())
            return Result<RenderPresentationOutput>::Failure(admission.ErrorValue());
        try {
            return backend_->PresentationOutput();
        } catch (...) {  // NOSONAR(cpp:S2738) Native exception containment boundary.
            return Result<RenderPresentationOutput>::Failure(MakeError(FrontendErrors::FrameException));
        }
    }

    /** @copydoc RenderFrontend::BeginFrame */
    Result<RenderFrameScope> RenderFrontend::BeginFrame(const FrameDescriptor &descriptor) {
        if (auto admission = RequireOpenOwner(); admission.HasError())
            return Result<RenderFrameScope>::Failure(std::move(admission).ErrorValue());
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
            submissionAllocator_->BeginFrame();
            return Result<RenderFrameScope>::Success(RenderFrameScope{*this, *backend_, frame, descriptor.frameNumber});
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
