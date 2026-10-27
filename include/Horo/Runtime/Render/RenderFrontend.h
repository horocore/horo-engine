#pragma once

#include "Horo/Runtime/Render/RenderGraphInspection.h"
#include "Horo/Runtime/Render/VfxGraphScheduling.h"

/**
 * @file RenderFrontend.h
 * @brief Host-facing owner of one selected and initialized renderer backend.
 */

#include "Horo/Runtime/Render/RenderBackendRegistry.h"
#include "Horo/Runtime/Render/RenderGraphTransientResources.h"
#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "Horo/Runtime/Render/RenderParallelWork.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"

#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace Horo::Render {
    class RenderFrontend;
    class IRenderGraphResourceLease;
    struct UiPaintFrameBuffers;
    struct UiPaintFrameUpdate;
    class PreparedVfxFrameSlot;
    struct VfxPackedDrawInstance;
    enum class RenderGraphExecutionRole : std::uint8_t;
    struct RenderMaterialPass;
    struct RenderGraphQueueSubmission;
    class CompiledRenderGraphExecution;
    class RenderGraphLifetimePlan;
    class UiPaintFrameBufferPool;
    class UiPaintProgram;
    struct RenderGraphPassWorkload;
    struct UiRenderSubmission;
    struct UiPaintGraphComposition;
    class UiRenderImageTexture;
    class UiRenderAtlasTexture;

}  // namespace Horo::Render

namespace Horo::Runtime::Ui {
    class UiImageResourceRegistry;
    class UiGlyphAtlas;
    struct UiImageResourceHandleTag;
    struct UiGlyphAtlasPageHandleTag;
    template <typename Tag> struct UiRuntimeHandle;
}  // namespace Horo::Runtime::Ui

namespace Horo::Render {

    namespace Detail {
        struct UiPaintFrontendState;
        struct UiPaintOwnerRetirement;
        struct RenderResourceIdentity;
        struct VfxSchedulingStorage;
        struct RenderGraphTransientResourceSet;
        struct VfxFrontendState;
        class RenderResourceRegistry;
        class RenderResourceUploadQueue;
        class RenderFrontendResourceAccess;
        class RenderExternalTextureAccess;
        class PreparedRenderGraphTemplate;
        class RenderGraphResourceLeasePool;
        class RenderGraphTransientResourcePool;
        class RenderParallelWorkState;
        class RenderParallelGraphWorkState;
        class RenderSubmissionAllocator;
    }  // namespace Detail

    /** @brief Finite frontend admission and per-drain limits for initial resource uploads. */
    struct RenderResourceUploadLimits {
        std::size_t maximumPendingBytes{64U * 1024U * 1024U};  /**< Arena capacity including requested alignment padding. */
        std::size_t maximumBytesPerDrain{16U * 1024U * 1024U}; /**< Maximum payload processed after the progress-guaranteed first item. */
        std::uint32_t maximumRequestsPerDrain{64};             /**< Maximum items in one frame-boundary batch. */
        std::uint32_t maximumPendingRequests{1024};            /**< Maximum metadata records retained by the arena. */
        std::size_t stagingOffsetAlignment{1};                 /**< Power-of-two alignment applied to every staged payload offset. */

        /** @brief Reports whether every upload bound is finite and non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumPendingBytes > 0 && maximumBytesPerDrain > 0 && maximumBytesPerDrain <= maximumPendingBytes &&
                   maximumPendingRequests > 0 && maximumRequestsPerDrain > 0 && maximumRequestsPerDrain <= maximumPendingRequests &&
                   stagingOffsetAlignment > 0 && (stagingOffsetAlignment & (stagingOffsetAlignment - 1U)) == 0 &&
                   stagingOffsetAlignment <= maximumPendingBytes;
        }
    };

    /** @brief Bounded owner-thread upload-arena state captured without exposing backend-native storage. */
    struct RenderResourceUploadSnapshot {
        std::size_t pendingPayloadBytes{0};     /**< Source bytes still waiting for realization. */
        std::size_t occupiedStagingBytes{0};    /**< Arena bytes occupied including alignment padding. */
        std::uint32_t pendingRequests{0};       /**< Metadata records awaiting an owner-thread boundary. */
        std::uint64_t completedBatchCount{0};   /**< Non-empty bounded batches processed by this frontend. */
        std::uint64_t cancelledRequestCount{0}; /**< Pending requests cancelled before native realization. */
        std::size_t lastBatchPayloadBytes{0};   /**< Source payload consumed by the most recent non-empty batch. */
        std::uint32_t lastBatchRequestCount{0}; /**< Requests completed by the most recent non-empty batch. */
        bool acceptingRequests{false};          /**< False after teardown closes producer admission. */
    };

    /** @brief Host-composed renderer memory envelope, default scope, and bounded reclaim policy. */
    struct RenderFrontendMemoryConfig {
        RenderMemoryBudgetConfig budget;
        RenderMemoryScopeId defaultResourceScope{1, 1};
        std::uint32_t maximumEmptyBlocksReclaimedPerDrain{16};

        /** @brief Reports whether the envelope, scope, and reclaim bound are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return budget.IsValid() && defaultResourceScope.IsValid() && maximumEmptyBlocksReclaimedPerDrain > 0;
        }
    };

    /** @brief Host-composed bounds for GPU-completion pins and owner-thread native destruction. */
    struct RenderResourceRetirementLimits {
        std::uint32_t maximumSubmissionPins{4'096}; /**< Maximum accepted resource uses awaiting GPU completion. */
        std::uint32_t maximumTrackedQueues{8};      /**< Maximum logical queue timelines tracked by one frontend. */
        std::uint32_t maximumCompletionsPerDrain{
            128}; /**< Maximum timeline-pin records inspected per drain; complete graph leases use maximumSubmissionPins. */
        std::uint32_t maximumRetirementsPerDrain{64}; /**< Maximum native instances destroyed at one safe point. */

        std::uint32_t maximumUiCapturesPerFrame{8}; /**< Explicit product canvas bound; native leases reserve this once, hard ceiling 64. */

        /** @brief Reports whether every completion and destruction bound is finite and non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumSubmissionPins > 0 && maximumTrackedQueues > 0 && maximumCompletionsPerDrain > 0 &&
                   maximumRetirementsPerDrain > 0 && maximumUiCapturesPerFrame > 0 && maximumUiCapturesPerFrame <= 64;
        }
    };

    /**
     * @brief Move-only owner of one begun backend frame until presentation or abort.
     *
     * Destruction aborts an unpresented frame with its matching token. Destroying
     * the creating frontend first aborts and makes the outstanding scope inert.
     * Execute may succeed exactly once;
     * Present may succeed only after Execute. Invalid stage calls return typed errors
     * without consuming the scope so the caller may recover. Backend failures and
     * exceptions abort the scope before returning.
     *
     * All methods, moves, and destruction must run serially on the same host-declared
     * render-capable thread as the creating frontend. The scope is not thread-safe and
     * must not be transferred across threads or accessed concurrently.
     */
    class RenderFrameScope final {
    public:
        /**
         * @brief Explicitly captures a compiled graph at this real frame's pre-submission safe point.
         * @param graph Intact immutable authored graph. @param schedule Exact compiled schedule.
         * @param lifetime Exact logical lifetime plan. @param execution Exact compiled execution plan.
         * @param limits Finite inspection allowances. @param cancellation Cooperative tooling cancellation.
         * @return Detached snapshot or typed failure; inspection failure never aborts rendering.
         * @details Owner-thread tooling only, before Execute/Present. Capture records planned logical facts,
         * not native realization. No capture occurs unless explicitly requested by the host.
         */
        [[nodiscard]] Result<std::shared_ptr<const RenderGraphInspectionSnapshot>> CaptureInspection(
            const RenderGraph &graph, const RenderGraphSchedule &schedule, const RenderGraphLifetimePlan &lifetime,
            const CompiledRenderGraphExecution &execution, RenderGraphInspectionLimits limits = {}, std::stop_token cancellation = {});
        /** @brief Aborts the matching frame when this scope still owns one. */
        ~RenderFrameScope();

        RenderFrameScope(const RenderFrameScope &) = delete;
        RenderFrameScope &operator=(const RenderFrameScope &) = delete;

        /** @brief Transfers matching-frame ownership and leaves source inert. */
        RenderFrameScope(RenderFrameScope &&other) noexcept;

        /** @brief Aborts any currently owned frame, then transfers ownership. */
        RenderFrameScope &operator=(RenderFrameScope &&other) noexcept;

        /** @brief Borrows this scope's existing active backend frame identity without issuing or submitting work.
         * @return Exact active token, or invalid after ownership was moved, presented or cancelled.
         * @details Read on the frame owner. Native private bridges still validate thread/current-frame admission. */
        [[nodiscard]] FrameToken Token() const noexcept {
            return backend_ != nullptr ? frame_ : FrameToken{};
        }

        /**
         * @brief Executes the ordered pass sequence for this frame exactly once.
         * @param orderedPasses Non-owning pass sequence valid for this call.
         * @return Success, a typed invalid-stage error, the original backend failure,
         * or a translated backend exception.
         */
        [[nodiscard]] Result<void> Execute(std::span<const RenderPassDescriptor> orderedPasses);

        /** @brief Reserves one ordered submission from this frontend's admitted queue topology.
         * @param role Admitted queue role; aliased roles share the same timeline.
         * @param waits Exact admitted points or earlier reservations in this frame, copied into bounded storage.
         * @return A record valid until this frame ends, or a typed owner, capacity or history failure.
         * @details Owner-thread only. Abandonment burns identities; later frames never recycle them.
         * Pass the result to explicit graph/material execution; reservation alone submits no work.
         * Reserved dependencies must be admitted earlier in the same batch or an earlier call.
         */
        [[nodiscard]] Result<RenderQueueSubmission> ReserveSubmission(RenderQueueRole role,
                                                                      std::span<const RenderTimelinePoint> waits = {});

        /**
         * @brief Composes GUI output once after successful execution and before presentation.
         * @param executor Matching host-selected integration; borrowed only for this call.
         * @param cancellation Cooperative cancellation sampled before encoding.
         * @return Success, typed stage/cancellation error, original encoder failure, or translated exception.
         * @details Cancellation, encoder failure and exceptions abort the frame. Invalid stage calls
         * leave it recoverable. No allocation or GPU wait is introduced by the composition stage.
         */
        [[nodiscard]] Result<void> ComposeGui(IGuiPassExecutor &executor, const CancellationToken &cancellation = {});

        /** @brief Consumes one bounded prepared material pass, retaining exact generations until native completion.
         * @param pass Ready target/meshes and immutable material leases, borrowed only during this call.
         * @return Success or typed stage, identity, unsupported or native failure. No frame-hot preparation occurs.
         * @details Native exceptions unwind leases and abort the frame before translation to FrameException.
         * @throws std::bad_alloc If owned error construction cannot allocate, after exception cleanup has completed.
         * @throws std::length_error If owned error construction exceeds its representation limits. */
        [[nodiscard]] Result<void> ExecuteMaterialPass(const RenderMaterialPass &pass);

        /** @brief Records the explicit lawful no-source UI contribution after scene execution and before GUI.
         * @return Success or typed inactive/order/repeated-composition failure; issues no native work. */
        [[nodiscard]] Result<void> ComposeUi();
        /** @brief Encodes one exact prepared UI graph after scene execution and before GUI composition.
         * @param graph Exact immutable UI-only compiled graph. @param workloads Bounded paint-only retained operations.
         * @param ui Exact canonical geometry/resource leases transferred to native completion.
         * @param transientResources Prepared isolation surfaces matching graph lifetimes.
         * @return Success or original admission/encoding failure; failure aborts presentation of the whole frame.
         * @details This named stage does not reopen general Execute/ExecuteGraph admission or allow repeated scene execution. */
        [[nodiscard]] Result<void> ComposeUi(const CompiledRenderGraphExecution &graph, std::span<const RenderGraphPassWorkload> workloads,
                                             UiRenderSubmission ui, RenderGraphTransientResourcesHandle transientResources = {});

        /** @brief Encodes one bounded ordered UI phase containing every admitted canvas before GUI composition.
         * @param canvases Complete borrowed mutable handoff, transferring source leases after whole-batch validation.
         * @return Original typed failure or success; malformed later entries issue no native work and partial native failure
         * aborts presentation while already queued source/resource leases remain with the same frame completion owner.
         * @details At most 64 entries and the configured capture bound; no allocation, waits or generic Execute reopening.
         */
        [[nodiscard]] Result<void> ComposeUi(std::span<UiPaintGraphComposition> canvases);

        /**
         * @brief Resolves resident resources and executes one compiled graph exactly once.
         * @param graph Intact compiled graph borrowed synchronously.
         * @param workloads One typed operation per retained pass, in compiled order.
         * @return Success or a typed stage, resource, unsupported or encoding failure.
         * Transient materialization and multiple effective queues are rejected explicitly.
         */
        [[nodiscard]] Result<void> ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                std::span<const RenderGraphPassWorkload> workloads);
        /**
         * @brief Executes a graph with one explicit frontend-owned submission authority.
         * @param graph Exact compiled graph borrowed through synchronous admission.
         * @param workloads One typed workload for each compiled pass.
         * @param submissions Exhaustive ordered pass partitions with exact queue waits/signals.
         * @param transientResources Optional exact prepared transient set owned by this frontend.
         * @return Success or typed stage, identity, submission, unsupported or native failure.
         * @details The caller's frontend scheduler owns submission values and wait storage.
         * They are not synthesized by the backend. Existing ExecuteGraph remains the
         * transitional single-queue surface; neither path retries through the other.
         */
        [[nodiscard]] Result<void> ExecuteSubmittedGraph(const CompiledRenderGraphExecution &graph,
                                                         std::span<const RenderGraphPassWorkload> workloads,
                                                         std::span<const RenderGraphQueueSubmission> submissions,
                                                         RenderGraphTransientResourcesHandle transientResources = {});
        /**
         * @brief Freezes borrowed inputs and schedules bounded preparation and CPU command recording.
         * @param jobs Const facade of the host worker pool; its owned scheduler continues servicing accepted jobs until host shutdown.
         * @param orderedPasses Immutable source views borrowed only during this call; all arrays are copied.
         * @param limits Finite input/output payload and job-count envelope.
         * @param cancellation Parent cancellation checked again before owner publication.
         * @return Admission success, or a typed capture, capacity, lifecycle, or scheduler error.
         * @details Call on the render owner. No scheduler wait is allowed, including admission waits.
         * Workers retain owned CPU bytes only; neither backend nor attached executor is borrowed.
         * A failed partial admission cancels its accepted siblings and leaves this frame unexecuted.
         */
        [[nodiscard]] Result<void> PrepareParallelExecution(const JobSystem &jobs, std::span<const RenderPassDescriptor> orderedPasses,
                                                            const RenderParallelWorkLimits &limits = {},
                                                            const CancellationToken &cancellation = {});

        /**
         * @brief Freezes native graph payload on the owner and records it through the host worker pool.
         * @param jobs Const host scheduler facade; its owned state performs callback admission, execution and cancellation acknowledgement.
         * @param graph Exact compiled graph borrowed only during this call; not retained by workers.
         * @param workloads One operation per compiled pass, borrowed only during owner capture.
         * @param cancellation Parent token checked during worker recording and before owner acceptance.
         * @return Success or typed capacity, unsupported, resource, job or native failure, without fallback.
         * @details Native recording is explicitly capability-gated. OpenGL never receives worker
         * context calls. PollParallelExecution accepts ready records in graph order; Present commits
         * them on the render owner. Abandonment closes submission without joining workers. Existing
         * frontend leases remain the sole resident authority until CPU abandonment or GPU completion.
         * This is an exception containment boundary: arbitrary backend exceptions abort the frame
         * and return render.frontend.frame_exception. No exception escapes; inability to construct
         * an error under catastrophic allocation failure follows the process noexcept contract.
         */
        [[nodiscard]] Result<void> PrepareParallelGraphExecution(const JobSystem &jobs, const CompiledRenderGraphExecution &graph,
                                                                 std::span<const RenderGraphPassWorkload> workloads,
                                                                 const CancellationToken &cancellation = {}) noexcept;

        /**
         * @brief Polls bounded work without waiting, then executes ready commands on the render owner.
         * @return Pending, Executed, or the original typed job/backend failure after aborting the frame.
         * @details Canonical pass order is preserved regardless of worker completion order. Once
         * owner execution is accepted, a later cancellation does not undo submitted work. GPU
         * completion and native retirement retain their existing backend/frontend authorities.
         */
        [[nodiscard]] Result<RenderParallelExecutionProgress> PollParallelExecution();

        /**
         * @brief Executes an existing graph while transferring exact Runtime UI generation ownership to native completion.
         * @param graph Compiled graph whose imported textures contain the host-resolved image/atlas resources.
         * @param workloads Existing typed graph operations; this overload adds no UI draw operation.
         * @param ui Synchronous UI source borrows and owned sealed atlas pins, consumed on success or abandonment.
         * @return Success or typed resource, source-generation, capacity, stage or backend failure.
         * @post On success the backend completion lease, not Present, owns every retained UI generation.
         */
        [[nodiscard]] Result<void> ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission ui);

        /**
         * @brief Executes a graph using an explicitly prepared transient resource set.
         * @param graph Exact execution matching the prepared graph and lifetime proof.
         * @param workloads One typed operation per retained pass.
         * @param transientResources Exact ready set owned by this frame's frontend.
         * @return Success or typed identity, lifetime, capacity, in-flight, or backend failure.
         * @post Accepted native work retains one combined imported/transient completion lease.
         */
        [[nodiscard]] Result<void> ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                std::span<const RenderGraphPassWorkload> workloads,
                                                RenderGraphTransientResourcesHandle transientResources);

        /**
         * @brief Executes prepared transient resources while retaining exact UI generations until native completion.
         * @param graph Exact execution matching the prepared graph and lifetime proof.
         * @param workloads One typed operation per retained pass.
         * @param transientResources Exact ready set owned by this frame's frontend.
         * @param ui Synchronous UI source borrows and owned sealed atlas pins, consumed on success or abandonment.
         * @return Success or typed identity, lifetime, UI generation, capacity, stage or backend failure.
         * @post Accepted work retains one combined imported, transient and UI completion lease.
         */
        [[nodiscard]] Result<void> ExecuteGraph(const CompiledRenderGraphExecution &graph,
                                                std::span<const RenderGraphPassWorkload> workloads,
                                                RenderGraphTransientResourcesHandle transientResources, UiRenderSubmission ui);

        /**
         * @brief Presents and consumes a successfully executed frame.
         * @param timing Optional synchronous host clock/identity for native feedback registration.
         * @return Success, a typed invalid-stage error, the original backend failure,
         * or a translated backend exception.
         */
        [[nodiscard]] Result<void> Present(const PresentationTimingRequest *timing = nullptr);

        /** @brief Explicitly aborts the owned frame; safe to call repeatedly. */
        void Cancel() noexcept;

    private:
        friend class RenderFrontend;

        RenderFrameScope(RenderFrontend &owner, IRenderBackend &backend, FrameToken frame, std::uint64_t hostFrame) noexcept;
        /** @brief Common graph admission preserving one authoritative native completion lease. */
        [[nodiscard]] Result<void> ExecuteGraphInternal(const CompiledRenderGraphExecution &graph,
                                                        std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission *ui,
                                                        RenderGraphTransientResourcesHandle transientResources = {},
                                                        std::span<const RenderGraphQueueSubmission> submissions = {});
        /** @brief Guards one acquired graph lease and commits queue history only after backend acceptance. */
        [[nodiscard]] Result<void> ExecuteLeasedGraph(RenderGraphExecutionRequest request,
                                                      const Detail::RenderSubmissionAllocator &submissionHistory);
        /** @brief Encodes a graph only after its named scene or UI stage has independently admitted it. */
        [[nodiscard]] Result<void> EncodeAdmittedGraph(const CompiledRenderGraphExecution &graph,
                                                       std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission *ui,
                                                       RenderGraphTransientResourcesHandle transientResources,
                                                       std::span<const RenderGraphQueueSubmission> submissions,
                                                       RenderGraphExecutionRole role);
        /** @brief Resolves admitted graph resources and transfers their exact lifetime lease under the caller's exception boundary. */
        [[nodiscard]] Result<void> EncodeResolvedGraph(const CompiledRenderGraphExecution &graph,
                                                       std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission *ui,
                                                       Detail::RenderGraphTransientResourceSet *transient,
                                                       std::span<const RenderGraphQueueSubmission> submissions,
                                                       RenderGraphExecutionRole role,
                                                       const Detail::RenderSubmissionAllocator &submissionHistory);
        /** @brief Completes an internally assembled request's exact bindings and lease before dispatch to the backend. */
        [[nodiscard]] Result<void> EncodeGraphBindings(RenderGraphExecutionRequest request, UiRenderSubmission *ui,
                                                       Detail::RenderGraphTransientResourceSet *transient,
                                                       const Detail::RenderSubmissionAllocator &submissionHistory);
        /** @brief Requires the unique UI stage between successful scene execution and GUI encoding. */
        [[nodiscard]] Result<void> ValidateUiCompositionAdmission() const;
        struct UiCompositionState;
        [[nodiscard]] Result<void> PrepareUiComposition(std::span<UiPaintGraphComposition> canvases, UiCompositionState &state);
        [[nodiscard]] Result<void> PrepareUiCompositionEntry(UiPaintGraphComposition &canvas, UiCompositionState &state, std::size_t index);
        [[nodiscard]] Result<void> EncodeUiComposition(std::span<UiPaintGraphComposition> canvases, UiCompositionState &state);
        /** @brief Transfers one acquired material lease to backend completion; failures retain frontend rollback authority. */
        [[nodiscard]] Result<void> ExecuteMaterialRequest(RenderMaterialExecutionRequest request);
        struct MaterialExecutionOutcome;
        struct MaterialExecutionInstances;
        struct GuiCompositionState;
        /** @brief Validates GUI declaration, resolves sampled generations and acquires the existing completion lease. */
        [[nodiscard]] Result<void> PrepareGuiComposition(IGuiPassExecutor &executor, GuiCompositionState &state);

        /** @brief Resolves bounded draw identities without acquiring or transferring completion ownership. */
        [[nodiscard]] Result<void> ResolveMaterialDraws(const RenderMaterialPass &pass, MaterialExecutionInstances &instances) const;
        /** @brief Contains admission/execution exceptions and completes rollback before allocation-free outcome transport. */
        [[nodiscard]] MaterialExecutionOutcome ExecuteMaterialPassBoundary(const RenderMaterialPass &pass) noexcept;
        void Abort() noexcept;
        void Release() noexcept;
        /** @brief Enforces the one-shot frame admission gate shared by synchronous and worker routes. */
        [[nodiscard]] Result<void> ValidateExecutionAdmission() const;
        /** @brief Validates owner-side executor and target liveness before synchronous borrowing. */
        [[nodiscard]] Result<void> ValidateStaticMeshBinding(const RenderPassDescriptor &pass) const;
        /** @brief Polls and accepts only on the frame owner; contains every backend exception before returning. */
        [[nodiscard]] Result<RenderParallelExecutionProgress> PollParallelGraph() noexcept;
        /** @brief Cancels this frame and preserves a native or job error across the polling boundary. */
        [[nodiscard]] Result<RenderParallelExecutionProgress> RejectParallelPoll(const Error &error);
        /** @brief Captures native slots and transfers the existing owner lease only on backend success. */
        [[nodiscard]] Result<std::shared_ptr<IRenderParallelGraphRecording>> CaptureParallelGraph(
            const CompiledRenderGraphExecution &graph, std::span<const RenderGraphPassWorkload> workloads);
        /** @brief Executes frozen slots in canonical order and contains arbitrary executor/backend exceptions. */
        [[nodiscard]] Result<void> ExecuteCapturedCommands(std::span<const RenderPassDescriptor> commands) noexcept;

        RenderFrontend *owner_{nullptr};
        IRenderBackend *backend_{nullptr};
        FrameToken frame_{};
        std::uint64_t hostFrame_{};
        bool executed_{false};
        bool guiComposed_{false};
        IRenderGraphResourceLease *completionLease_{nullptr}; /**< Borrowed exact active frame lease; backend owns retirement. */
        bool uiComposed_{false};
        bool uiComposing_{false}; /**< Owner-thread reentry fence spanning the complete ordered UI batch. */
        std::unique_ptr<Detail::RenderParallelWorkState> parallelWork_;
        std::unique_ptr<Detail::RenderParallelGraphWorkState> parallelGraphWork_;
    };

    /**
     * @brief Owns the initialized renderer backend for one host lifetime.
     *
     * Construction performs explicit registry selection and backend initialization.
     * BeginShutdown closes admission; PollDrain proves terminal retirement before
     * native parents and registries can be released. Hosts retain the complete
     * composition while Pending, including after their finite deadline expires.
     * Destruction of an unresolved closing owner is a fail-closed invariant violation.
     * All methods and destruction must run serially on one host-declared render-capable
     * thread. The frontend and its frame scope are not thread-safe.
     */
    class RenderFrontend final
        : public IMaterialBindingBackend {  // NOSONAR(cpp:S1448) Cohesive public facade for one renderer frontend lifetime.
    public:
        /**
         * @brief Creates and initializes the selected backend from a sealed registry.
         * @param registry Host-owned sealed backend registry.
         * @param backendId Canonical backend identity selected by host policy.
         * @param config Backend-neutral initialization policy.
         * @param uploadLimits Finite initial-upload queue and per-safe-point work limits.
         * @param memoryConfig Finite host envelope, default resource scope, and reclaim bound.
         * @param retirementLimits Finite completion-pin, queue, and owner-thread destruction limits.
         * @return Owned frontend, or the backend creation/initialization failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<RenderFrontend>> Create(const RenderBackendRegistry &registry,
                                                                            const RenderBackendId &backendId,
                                                                            const RenderBackendConfig &config,
                                                                            const RenderResourceUploadLimits &uploadLimits = {},
                                                                            const RenderFrontendMemoryConfig &memoryConfig = {},
                                                                            const RenderResourceRetirementLimits &retirementLimits = {});

        /** @brief Polls the selected backend for qualified native display time without waiting.
         * @return New observation, pending empty value, or typed unsupported/failure.
         */
        [[nodiscard]] Result<std::optional<NativePresentTiming>> PollNativePresentTiming();

        /** @brief Shuts down and releases the owned backend. */
        ~RenderFrontend();
        /** @brief Observes exact issued GPU completion on the render owner, including retained closing state.
         * @param wait Existing Horo finite-purpose policy; normal frames permit zero-time Poll only.
         * @return Native completion, pending(false), or the selected backend's typed failure. */
        [[nodiscard]] Result<bool> ObserveSubmission(const RenderCpuWait &wait);

        /** @brief Closes owner-thread frame/resource admission and cancels unsent work without waiting.
         * @return Success or original typed shutdown failure; no failure proves completion. */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Polls native drain and releases frontend registries only after terminal proof.
         * @return Pending, Drained or original failure; the host retains this owner until Drained. */
        [[nodiscard]] Result<RenderBackendDrainProgress> PollDrain();

        RenderFrontend(const RenderFrontend &) = delete;
        RenderFrontend &operator=(const RenderFrontend &) = delete;
        RenderFrontend(RenderFrontend &&) = delete;
        RenderFrontend &operator=(RenderFrontend &&) = delete;

        /**
         * @brief Returns the immutable capability snapshot of the initialized backend.
         * @return Reference valid until frontend destruction; safe to query during an active frame.
         */
        [[nodiscard]] const RenderBackendCapabilities &Capabilities() const noexcept;
        /** @brief Observes the actual preallocated per-frame UI capture bound for whole-product source admission.
         * @return Configured capture capacity, or zero after renderer ownership has moved/revoked.
         */
        [[nodiscard]] std::size_t MaximumUiCapturesPerFrame() const noexcept;

        /** @brief Consumes retained live-effect instances into actual typed passes of the host's recipe graph.
         * @param builder Open host-owned recipe graph. @param snapshot Immutable scene extraction lease.
         * @param request Exact frozen recipe anchors, view and remaining global budget.
         * @return Inserted retained workloads and draw accounting or typed failure; no physical execution is claimed.
         * @details Requires PrepareVfxScheduling before activation. Success and expected capacity/backpressure failures
         * allocate no scheduling storage. Retained workloads pin their prepared view slot until every reader releases it.
         * Every failure preserves prior graph records; the appended contribution publishes atomically.
         * Const access still requires the render owner: scheduling mutates prepared slot storage and the borrowed graph.
         * Native ExecuteGraph rejects these payloads explicitly until physical encoding is available.
         */
        [[nodiscard]] VfxGraphSchedulingOutcome ScheduleVfx(RenderGraphBuilder &builder, const RenderWorldSnapshot &snapshot,
                                                            const VfxGraphRequest &request) const;

        /** @brief Prepares finite per-view order and radix storage before scene activation.
         * @param budget Aggregate host-admitted capacity across views and frames in flight.
         * @return Prepared storage or typed failure. Reconfiguration rejects retained old view leases and preserves old storage on failure.
         * @pre Call on the render owner before scheduling; no frame-hot resize or automatic growth is permitted.
         */
        [[nodiscard]] Result<void> PrepareVfxScheduling(VfxSchedulingBudget budget);

        /** @brief Queries actual selected backend/device facts for pipeline cache identity outside an active frame.
         * @return Native-free compatibility facts or typed thread/lifecycle/unsupported failure. */
        [[nodiscard]] Result<RenderPipelineDeviceIdentity> QueryPipelineDeviceIdentity() const;
        /** @copydoc IMaterialBindingBackend::Realize */
        [[nodiscard]] Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &descriptor) override;

        /** @brief Synchronously prepares an exact cooked stage on the render owner, outside an active frame.
         * @param descriptor Validated target, stage, authored entry and original artifact/interface identity.
         * @param cookedPackage Entire immutable HOROSHDR package borrowed only during this call.
         * @return Ready generation and completed operation, or typed failure with no partial publication.
         * @details Admits backing before native compilation. No job, source cooker, fallback or borrowed byte retention occurs.
         * Host cancellation/shutdown is serialized before/after this bounded loading operation. */
        [[nodiscard]] Result<ResourceCreation<RenderShaderModuleHandle>> CreateShaderModule(const RenderShaderModuleDescriptor &descriptor,
                                                                                            std::span<const std::uint8_t> cookedPackage);
        /** @brief Synchronously prepares an exact graphics pipeline during render-owner loading.
         * @param descriptor Cooked manifest/reflection, exact shader generations and immutable state/cache identity.
         * @return Ready pipeline generation and completed operation, or original typed failure.
         * @details Retains both shader dependencies until pipeline retirement; active-frame creation is rejected. */
        [[nodiscard]] Result<ResourceCreation<RenderPipelineHandle>> CreateGraphicsPipeline(
            const RenderGraphicsPipelineDescriptor &descriptor);
        /** @brief Synchronously prepares one admitted immutable sampler outside an active frame.
         * @param descriptor Exact filtering, addressing and comparison policy.
         * @return Ready sampler generation and completed operation, or typed failure without policy substitution. */
        [[nodiscard]] Result<ResourceCreation<RenderSamplerHandle>> CreateSampler(const RenderSamplerDescriptor &descriptor);
        /** @brief Retires an exact shader generation once pipeline and submission pins drain.
         * @param handle Exact frontend-owned generation. @return Success or typed identity/lifecycle failure.
         * @details Owner-thread preparation operation; resource changes during an active frame are rejected. */
        [[nodiscard]] Result<void> ReleaseShaderModule(RenderShaderModuleHandle handle);
        /** @brief Retires an exact pipeline generation once its consumer and submission pins drain.
         * @param handle Exact frontend-owned generation. @return Success or typed identity/lifecycle failure.
         * @details Owner-thread preparation operation; resource changes during an active frame are rejected. */
        [[nodiscard]] Result<void> ReleaseGraphicsPipeline(RenderPipelineHandle handle);
        /** @brief Retires an exact sampler generation once material and submission pins drain.
         * @param handle Exact frontend-owned generation. @return Success or typed identity/lifecycle failure.
         * @details Owner-thread preparation operation; resource changes during an active frame are rejected. */
        [[nodiscard]] Result<void> ReleaseSampler(RenderSamplerHandle handle);
        /** @brief Queries one exact shader generation. @param handle Exact identity. @return State or typed identity failure. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderShaderModuleHandle handle) const;
        /** @brief Queries one exact pipeline generation. @param handle Exact identity. @return State or typed identity failure. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderPipelineHandle handle) const;
        /** @brief Queries one exact sampler generation. @param handle Exact identity. @return State or typed identity failure. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderSamplerHandle handle) const;

        /**
         * @brief Realizes one cooked light-culling kernel before frame admission on the render owner thread.
         * @param kernel Exact cooked artifact and final-target reflection produced by the admitted toolchain route.
         * @return Owned selected-backend kernel lease or typed capability, lifecycle or native failure.
         * @details Source compilation and implicit quality fallback are prohibited; callers drain leases before frontend destruction.
         */
        [[nodiscard]] Result<std::shared_ptr<IResidentLightCullingKernel>> RealizeLightCullingKernel(
            const CookedLightCullingKernel &kernel);

        /**
         * @brief Updates exact ready frame-slot buffers before opening a frame; submitted slots remain immutable until complete.
         * @param buffers Four distinct host-visible storage generations allocated at preparation time.
         * @param update Validated packed table, finite budget and increasing slot revision.
         * @return Original typed failure or successful update; pending never blocks or changes the active recipe.
         * @details The host allocates at most its admitted frames-in-flight slots and retries only a native-complete slot.
         */
        [[nodiscard]] Result<void> UpdateLightFrame(const LightFrameBuffers &buffers, const LightFrameUpdate &update);

        /**
         * @brief Begins one staged frame owned by a move-only recovery scope.
         * @param descriptor Host frame identity and output extent.
         * @return Frame scope, the original typed backend failure, or a translated
         * backend exception. Destroying the frontend first safely invalidates the scope.
         */
        [[nodiscard]] Result<RenderFrameScope> BeginFrame(const FrameDescriptor &descriptor);

        /** @brief Polls exact output readiness on the owner outside an active frame.
         * @param extent Current coalesced framebuffer size. @return Ready, pending value, or original typed failure.
         * @details Closing frontends reject admission. The call never starts a frame or waits for native completion.
         */
        [[nodiscard]] Result<bool> PollPresentationAvailability(FramebufferExtent extent);
        /** @brief Realizes the host's exact output request outside an active frame.
         * @param request Required graph-final output encoding. @return Read-back output or original typed failure/pending.
         */
        [[nodiscard]] Result<RenderPresentationOutput> ConfigurePresentationOutput(const RenderPresentationOutputRequest &request);
        /** @brief Queries admitted output and current host evidence on the owner.
         * @return Exact snapshot or original unavailable/unsupported/lifecycle failure.
         */
        [[nodiscard]] Result<RenderPresentationOutput> PresentationOutput() const;

        /**
         * @brief Executes and presents one ordered frame, aborting backend frame state on failure.
         * @param descriptor Host frame identity and output extent.
         * @param orderedPasses Non-owning pass sequence valid for this call.
         * @return Success, the original typed backend failure, or a translated backend exception.
         */
        [[nodiscard]] Result<void> SubmitFrame(const FrameDescriptor &descriptor, std::span<const RenderPassDescriptor> orderedPasses);

        /**
         * @brief Host handoff that begins, admits and presents one UI-owning graph without retiring submitted sources.
         * @param descriptor Exact output frame identity and extent.
         * @param graph Compiled graph borrowed until synchronous native admission returns.
         * @param workloads Existing bounded native graph operations.
         * @param ui Exact UI geometry/source generations and sealed atlas pins transferred into submission ownership.
         * @return Success or typed original failure; unsent failures abandon the owned pin set exactly once.
         */
        [[nodiscard]] Result<void> SubmitUiGraph(const FrameDescriptor &descriptor, const CompiledRenderGraphExecution &graph,
                                                 std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission ui);

        /**
         * @brief Realizes one exact current UI image page through the normal owned texture-upload path.
         * @param registry Synchronously borrowed active image authority.
         * @param image Exact current image generation. @param page Source page index.
         * @param pixels One complete tightly packed RGBA8 page copied by the bounded renderer upload queue.
         * @return Frontend-issued exact source/texture provenance and readiness operation, or typed original failure.
         * @details Load/realization boundary; performs no provider discovery or I/O. New image revisions require new realization.
         * Unsupported source formats/color spaces fail through normal texture admission without conversion or fallback.
         */
        [[nodiscard]] Result<UiRenderImageTexture> CreateUiImageTexture(
            const Runtime::Ui::UiImageResourceRegistry &registry, Runtime::Ui::UiRuntimeHandle<Runtime::Ui::UiImageResourceHandleTag> image,
            std::uint32_t page, std::span<const std::byte> pixels);
        /** @brief Realizes the exact cooked UI raster interface before frame acquisition.
         * @param kernel Immutable explicit target/cook/attachment evidence.
         * @return Selected-backend native lease or original typed failure; no source compilation/fallback. */
        [[nodiscard]] Result<std::shared_ptr<IResidentUiPaintKernel>> RealizeUiPaintKernel(const CookedUiPaintKernel &kernel);
        /** @brief Updates an already allocated free UI frame slot before opening a native frame.
         * @param buffers Exact resident ring handles. @param update Complete synchronous finite CPU payload.
         * @return Success or typed owner/ready/busy/capacity failure without advancing accepted UI state. */
        [[nodiscard]] Result<void> UpdateUiPaintFrame(const UiPaintFrameBuffers &buffers, const UiPaintFrameUpdate &update);
        /** @brief Issues one exact VFX schema/material/buffer proof at a cold preparation point.
         * @param material Canonical ready VFX generation with a dedicated host-visible uniform block at offset zero.
         * @return Issued slot with fixed staging or typed invalid/unsupported/allocation failure.
         * @details Host configuration and existing retained-material/resource budgets bound slot count; no
         * arbitrary draw-count ceiling, separate registry or native backing authority is introduced.
         * Repeated exact-material preparation returns the same live control block. A conflicting material,
         * recursive preparation, or expired claim is rejected; only a fresh buffer generation resets the claim.
         */
        [[nodiscard]] Result<std::shared_ptr<PreparedVfxFrameSlot>> PrepareVfxFrameSlot(
            std::shared_ptr<const ResidentMaterialBinding> material);
        /** @brief Packs and publishes one exact free VFX slot before opening a frame.
         * @param slot Unique shared-handle owner of this frontend-issued preparation; every facade copy rejects reuse.
         * @param revision Strictly newer positive revision. @param instances One through128 finite records in draw order.
         * @param cancellation Observed before native writes. @return Success or original typed failure; never waits.
         * @details No handle borrowing or reentrant publication during this call. Unique CPU ownership is
         * conservative admission only; selected-adapter completion independently protects all native aliases.
         * Admission failure preserves the accepted publication. A native write failure invalidates it.
         */
        [[nodiscard]] Result<void> UpdateVfxFrame(const std::shared_ptr<PreparedVfxFrameSlot> &slot, std::uint64_t revision,
                                                  std::span<const VfxPackedDrawInstance> instances,
                                                  const CancellationToken &cancellation = {});
        /** @brief Packs immutable scheduled instances and binds exact ready draw slots before opening a frame.
         * @param builder Same open graph receiving uniform reads. @param schedule Canonical issued VFX schedule.
         * @param view Exact captured scene/frame/origin/view projection. @param bindings Complete ordered run-slot assignments.
         * @return Complete graph-ready preparation or original typed failure; no allocation or wait on success.
         * @details Capacity is reserved by PrepareVfxScheduling from the explicit scene budget. On failure no partial
         * graph draw program is published; successful earlier free-slot uploads retain their actual newer revisions.
         */
        [[nodiscard]] Result<void> PrepareVfxDraws(RenderGraphBuilder &builder, VfxGraphSchedule &schedule, const VfxDrawView &view,
                                                   std::span<const VfxGraphDrawBinding> bindings);

        /**
         * @brief Realizes a current image page explicitly for alpha-mask sampling.
         * @param registry Borrowed image authority. @param image Exact current image generation.
         * @param page Source page index. @param pixels Complete tightly packed RGBA8 page; alpha supplies coverage.
         * @return Exact Mask-role provenance and readiness, or the original typed image/upload failure.
         * @details Uses the same owned upload path as images. A paint-image realization cannot impersonate this role.
         */
        [[nodiscard]] Result<UiRenderImageTexture> CreateUiMaskTexture(
            const Runtime::Ui::UiImageResourceRegistry &registry, Runtime::Ui::UiRuntimeHandle<Runtime::Ui::UiImageResourceHandleTag> image,
            std::uint32_t page, std::span<const std::byte> pixels);
        /**
         * @brief Realizes one exact current glyph atlas page through the normal owned texture-upload path.
         * @param atlas Synchronously borrowed active atlas authority. @param page Exact current atlas page generation.
         * @param pixels One complete tightly packed Alpha8 or RGBA8 page copied by the bounded renderer upload queue.
         * @return Frontend-issued page/texture provenance and readiness operation, or typed original failure.
         * @details Load/realization boundary; no rasterization, provider discovery, new draw workload or normal-frame wait.
         * A source encoding unsupported by current texture admission returns its typed failure, never an implicit conversion.
         */
        [[nodiscard]] Result<UiRenderAtlasTexture> CreateUiGlyphAtlasTexture(
            const Runtime::Ui::UiGlyphAtlas &atlas, Runtime::Ui::UiRuntimeHandle<Runtime::Ui::UiGlyphAtlasPageHandleTag> page,
            std::span<const std::byte> pixels);

        /**
         * @brief Commits a framebuffer resize through the owned backend.
         * @param extent Non-zero framebuffer extent committed by the host.
         * @return Backend result, a typed active-frame rejection, or a translated backend exception.
         */
        [[nodiscard]] Result<void> Resize(FramebufferExtent extent);

        /**
         * @brief Attaches the synchronously borrowed static-mesh executor used by frame execution.
         * @param executor Executor that must outlive its attachment or be detached before destruction.
         * @return Success or a typed rejection when an executor is already attached or a frame is active.
         */
        [[nodiscard]] Result<void> AttachStaticMeshPassExecutor(IStaticMeshPassExecutor &executor);

        /** @brief Detaches the matching executor; safe to call repeatedly outside an active frame. */
        void DetachStaticMeshPassExecutor(const IStaticMeshPassExecutor &executor) noexcept;

        /** @brief Creates one logical offscreen target identity with an initial non-zero extent. */
        [[nodiscard]] Result<RenderTargetHandle> CreateOffscreenTarget(FramebufferExtent extent);

        /** @brief Updates the extent associated with a live generation-safe target handle. */
        [[nodiscard]] Result<void> ResizeOffscreenTarget(RenderTargetHandle target, FramebufferExtent extent);

        /** @brief Releases a live target and invalidates its generation; repeated stale release is rejected. */
        [[nodiscard]] Result<void> ReleaseOffscreenTarget(RenderTargetHandle target);

        /**
         * @brief Queues an owned initial upload for one immutable buffer generation.
         * @param descriptor Valid backend-neutral buffer descriptor.
         * @param initialData Bytes copied into the bounded frontend queue before return.
         * @return Pending typed handle and completion operation, or an admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderBufferHandle>> CreateBuffer(const RenderBufferDescriptor &descriptor,
                                                                                std::span<const std::byte> initialData);

        /** @brief Queues one buffer against an explicit admitted owner-scope incarnation. */
        [[nodiscard]] Result<ResourceCreation<RenderBufferHandle>> CreateBuffer(RenderMemoryScopeId scope,
                                                                                const RenderBufferDescriptor &descriptor,
                                                                                std::span<const std::byte> initialData);

        /**
         * @brief Queues one immutable mesh over exact ready vertex and index buffers.
         * @param descriptor Valid mesh descriptor whose dependencies belong to this frontend.
         * @return Pending typed handle and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderMeshHandle>> CreateMesh(const RenderMeshDescriptor &descriptor);

        /**
         * @brief Queues one immutable texture allocation without initial pixel data.
         * @param descriptor Valid backend-neutral texture descriptor.
         * @return Pending typed handle and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderTextureHandle>> CreateTexture(const RenderTextureDescriptor &descriptor,
                                                                                  std::span<const std::byte> initialData = {});

        /** @brief Queues one texture and optional base-level upload against an explicit owner scope. */
        [[nodiscard]] Result<ResourceCreation<RenderTextureHandle>> CreateTexture(RenderMemoryScopeId scope,
                                                                                  const RenderTextureDescriptor &descriptor,
                                                                                  std::span<const std::byte> initialData = {});

        /**
         * @brief Queues one immutable view over an exact ready texture generation.
         * @param descriptor Valid view descriptor whose texture belongs to this frontend.
         * @return Pending typed handle and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderTextureViewHandle>> CreateTextureView(const RenderTextureViewDescriptor &descriptor);

        /**
         * @brief Queues one immutable render target over exact ready attachment views.
         * @param descriptor Valid target descriptor whose views belong to this frontend.
         * @return Pending typed handle and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderTargetHandle>> CreateRenderTarget(const RenderTargetDescriptor &descriptor);

        /**
         * @brief Queues a new mesh generation and retires the old generation only after publication.
         * @param current Ready mesh generation to replace without retargeting its dependents.
         * @param descriptor Descriptor for the independent replacement generation.
         * @return Pending replacement and completion operation, or a validation/admission failure.
         */
        [[nodiscard]] Result<ResourceCreation<RenderMeshHandle>> ReplaceMesh(RenderMeshHandle current,
                                                                             const RenderMeshDescriptor &descriptor);

        /**
         * @brief Processes one bounded upload batch on the render-capable owner thread.
         * @return Number of completed requests, including typed backend failures.
         */
        [[nodiscard]] Result<std::size_t> ProcessResourceRequests();

        /** @brief Returns bounded staging occupancy and completed-batch counters for this frontend. */
        [[nodiscard]] RenderResourceUploadSnapshot UploadSnapshot() const noexcept;

        /** @brief Returns the current state of one buffer generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderBufferHandle buffer) const;

        /** @brief Returns the current state of one mesh generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderMeshHandle mesh) const;

        /** @brief Returns the current state of one texture generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderTextureHandle texture) const;

        /** @brief Returns the current state of one texture-view generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderTextureViewHandle view) const;

        /** @brief Returns the current state of one generic render-target generation. */
        [[nodiscard]] Result<RenderResourceState> ResourceState(RenderTargetHandle target) const;

        /** @brief Observes a known creation outcome without conflating failures with query errors.
         * @param operation Exact frontend-issued operation.
         * @return Pending, Succeeded or Failed; unknown/evicted identities and stopped frontend are typed errors.
         * @details Failed retains the original diagnostic in ResourceOperationResult. Bounded history can evict
         * completed operations; callers must preserve ownership when their outcome cannot be proven. */
        [[nodiscard]] Result<RenderResourceOperationState> ResourceOperationState(ResourceOperationId operation) const;

        /** @brief Returns success, pending, or the stored typed result for one resource operation. */
        [[nodiscard]] Result<void> ResourceOperationResult(ResourceOperationId operation) const;

        /** @brief Logically releases one buffer generation; dependent meshes retain its native realization. */
        [[nodiscard]] Result<void> ReleaseBuffer(RenderBufferHandle buffer);

        /** @brief Logically releases one mesh generation and drains newly eligible dependencies. */
        [[nodiscard]] Result<void> ReleaseMesh(RenderMeshHandle mesh);

        /** @brief Logically releases one texture generation after its dependent views retire. */
        [[nodiscard]] Result<void> ReleaseTexture(RenderTextureHandle texture);

        /** @brief Logically releases one texture-view generation after dependent targets retire. */
        [[nodiscard]] Result<void> ReleaseTextureView(RenderTextureViewHandle view);

        /** @brief Logically releases one generic render-target generation. */
        [[nodiscard]] Result<void> ReleaseRenderTarget(RenderTargetHandle target);

        /** @brief Reads the last explicitly captured owned graph without querying native state.
         * @return Snapshot, empty before capture, or owner-thread failure. Readers cannot keep GPU resources alive. */
        [[nodiscard]] Result<std::shared_ptr<const RenderGraphInspectionSnapshot>> GraphInspectionSnapshot() const;

        /** @brief Returns non-additive renderer memory accounting for the current frontend envelope. */
        [[nodiscard]] RenderMemoryBudgetSnapshot MemorySnapshot() const noexcept;

        /**
         * @brief Allocates one physical generation per admitted graph alias slot outside active frames.
         * @param plan Immutable graph lifetime and exact-descriptor compatibility proof.
         * @param scope Explicit admitted memory scope incarnation.
         * @return Completely realized set identity or a typed failure after partial admission rollback.
         * @details Owner-thread preparation allocates bounded metadata and reserves all slot costs
         * before native creation. Backend-native requirement classifications remain authoritative.
         * No normal-frame waits or concurrent set reuse are permitted.
         * Expected memory-cost callback failures preserve their backend identity; documented
         * metadata allocation/length exceptions become capacity failures after rollback.
         * @throws std::bad_alloc Owned failure metadata cannot be allocated after rollback.
         * @throws std::length_error Owned failure metadata exceeds its representable capacity.
         */
        [[nodiscard]] Result<RenderGraphTransientResourcesHandle> PrepareTransientGraphResources(const RenderGraphLifetimePlan &plan,
                                                                                                 RenderMemoryScopeId scope);

        /**
         * @brief Releases one graph set while retaining backing needed by accepted GPU readers.
         * @param resources Exact unreleased frontend-issued set identity.
         * @return Success or typed malformed, foreign, stale, or active-frame failure.
         */
        [[nodiscard]] Result<void> ReleaseTransientGraphResources(RenderGraphTransientResourcesHandle resources);

    private:
        friend class RenderFrameScope;
        friend class Detail::RenderFrontendResourceAccess;
        friend class Detail::RenderExternalTextureAccess;
        friend class Detail::PreparedRenderGraphTemplate;

        class ConstructionKey {
            ConstructionKey() = default;
            friend class RenderFrontend;
        };

    public:
        RenderFrontend(std::unique_ptr<IRenderBackend> backend, RenderResourceOwnerId resourceOwner,
                       const RenderResourceUploadLimits &uploadLimits, std::unique_ptr<RenderMemoryBudget> memoryBudget,
                       const RenderFrontendMemoryConfig &memoryConfig, const RenderResourceRetirementLimits &retirementLimits,
                       ConstructionKey);

    private:
        /** @brief Plain exact resident facts that may safely cross an injected/native callback. */
        struct VfxResidentProof {
            RenderBufferHandle buffer;
            RenderPipelineHandle pipeline;
            std::uint64_t bufferInstance{};
            std::uint64_t pipelineInstance{};
            std::size_t byteSize{};
            std::uint64_t deviceIncarnation{};
        };

        /** @brief Revalidates owner, ready generations, native identity and immutable uniform facts after callbacks. */
        [[nodiscard]] Result<void> ValidateVfxResidents(const VfxResidentProof &proof) const;

        /** @brief Plain material and resource facts captured before a native size query can reenter. */
        struct VfxSlotPreparation {
            VfxResidentProof residents;
            std::size_t semanticBytes{};
            std::size_t boundBytes{};
        };

        /** @brief Validates material schema and snapshots exact native residents before any external callback. */
        [[nodiscard]] Result<VfxSlotPreparation> InspectVfxSlotMaterial(const ResidentMaterialBinding &material) const;
        /** @brief Reuses only the same live, nonpublishing slot claim for this exact buffer generation and material. */
        [[nodiscard]] Result<std::shared_ptr<PreparedVfxFrameSlot>> ExistingVfxFrameSlot(const ResidentMaterialBinding &material,
                                                                                         RenderBufferHandle buffer) const;
        /** @brief Revalidates callback-surviving facts before issuing and recording the sole generation claim. */
        [[nodiscard]] Result<std::shared_ptr<PreparedVfxFrameSlot>> IssueVfxFrameSlot(
            std::shared_ptr<const ResidentMaterialBinding> material, const VfxResidentProof &proof, std::size_t nativeBytes);
        /** @brief Checks owner-thread, revision, nonreentrancy and unique writer admission before staging mutation. */
        [[nodiscard]] Result<void> AdmitVfxFrameMutation(const std::shared_ptr<PreparedVfxFrameSlot> &slot, std::uint64_t revision,
                                                         const VfxDrawOrder *drawIssuer);
        /** @brief Publishes a staged slot only after native write evidence and postcallback resident validation. */
        [[nodiscard]] Result<void> PublishVfxSlot(const std::shared_ptr<PreparedVfxFrameSlot> &retained, const VfxResidentProof &proof,
                                                  std::uint64_t revision, std::uint32_t instanceCount,
                                                  const CancellationToken &cancellation);
        struct VfxDrawPreparation;
        /** @brief Captures issued draw-order ownership and bounded draw counts before any mutation or callback. */
        [[nodiscard]] Result<void> CaptureVfxDrawOrders(VfxDrawPreparation &preparation, const VfxGraphSchedule &schedule,
                                                        std::size_t bindingCount);
        /** @brief Copies and finite-validates borrowed bindings into reserved immutable-order storage before callbacks. */
        [[nodiscard]] Result<void> StageVfxDrawBindings(RenderGraphBuilder &builder, VfxDrawPreparation &preparation,
                                                        std::span<const VfxGraphDrawBinding> bindings);
        /** @brief Publishes each staged chunk through the canonical guarded frame-slot update authority. */
        [[nodiscard]] Result<void> PublishVfxDrawFrames(VfxDrawPreparation &preparation);
        /** @brief Revalidates graph, schedule and exact resident generations after all native callbacks. */
        [[nodiscard]] Result<void> ValidateVfxDrawPublication(RenderGraphBuilder &builder, const VfxGraphSchedule &schedule,
                                                              VfxDrawPreparation &preparation);
        /** @brief Registers exact read roles under the caller's append-only graph-usage rollback transaction. */
        [[nodiscard]] Result<void> AppendVfxDrawReadUsages(RenderGraphBuilder &builder, VfxDrawPreparation &preparation);
        /** @brief Releases publication admission and clears uncommitted prepared draws on every exit. */
        void ClearVfxDrawPreparation(VfxDrawPreparation &preparation, bool committed) noexcept;
        /** @brief Commits immutable draw capture after all validation and graph usage registration succeed. */
        void CommitVfxDrawPreparation(VfxGraphSchedule &schedule, VfxDrawPreparation &preparation);
        /** @brief Checks exact imported draw dependencies against the canonical resident generations. */
        [[nodiscard]] Result<void> ValidateVfxDrawResources(const RenderGraphBuilder &builder, const RenderGraphVfx &workload,
                                                            const VfxPreparedDraw &draw) const;
        /** @brief Clears stale draw records only when their canonical scheduling slot has no external immutable borrowers. */
        void ReleaseUnborrowedVfxDraws() noexcept;
        /** @brief Resolves VFX identities into preparation-owned scratch while its frame owner holds synchronous admission. */
        [[nodiscard]] Result<void> ResolveVfxGraphResources(std::span<const RenderGraphPassWorkload> workloads,
                                                            std::span<const RenderGraphMeshInstance> existingMeshes,
                                                            std::span<const RenderGraphMeshInstance> &meshes,
                                                            std::span<const RenderGraphTargetInstance> &targets);
        /** @brief Shares native publication logic with the exact synchronous canonical draw transaction. */
        [[nodiscard]] Result<void> UpdateVfxFrameInternal(const std::shared_ptr<PreparedVfxFrameSlot> &slot, std::uint64_t revision,
                                                          std::span<const VfxPackedDrawInstance> instances,
                                                          const CancellationToken &cancellation, const VfxDrawOrder *drawIssuer);
        /** @brief Resolves one ready, single-layer/mip/sample graph attachment in this frontend's resource namespace. */
        [[nodiscard]] const RenderTextureDescriptor *VfxAttachment(const RenderGraphBuilder &builder, RenderGraphResourceId resource) const;
        /** @brief Validates ready scene/workload attachment formats, identities and paired extents. */
        [[nodiscard]] VfxGraphSchedulingOutcome ValidateVfxAttachments(const RenderGraphBuilder &builder, const VfxGraphRequest &request,
                                                                       const VfxGraphSchedule &schedule) const;
        /** @brief Validates exact graph-local ready floating-color/depth recipe resources before append. */
        [[nodiscard]] VfxGraphSchedulingOutcome ValidateVfxRecipe(const RenderGraphBuilder &builder, const VfxGraphRequest &request,
                                                                  const VfxGraphSchedule &schedule) const;
        /** @brief Appends an already admitted contribution; caller owns atomic rollback of append-only records. */
        [[nodiscard]] Result<void> AppendVfxSchedule(RenderGraphBuilder &builder, VfxGraphSchedule &schedule,
                                                     const VfxGraphRequest &request) const;
        [[nodiscard]] bool IsLiveTarget(RenderTargetHandle target, FramebufferExtent extent) const noexcept;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderBufferHandle buffer) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderMeshHandle mesh) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderTextureViewHandle view) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderTargetHandle target) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderPipelineHandle pipeline) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderSamplerHandle sampler) const;
        [[nodiscard]] Result<void> ValidateMeshDependencies(const RenderMeshDescriptor &descriptor) const;
        [[nodiscard]] bool IsMeshBufferLayoutCompatible(const RenderMeshDescriptor &descriptor) const noexcept;
        [[nodiscard]] Result<void> ValidateTextureViewDependency(const RenderTextureViewDescriptor &descriptor) const;
        [[nodiscard]] Result<void> ValidateRenderTargetDependencies(const RenderTargetDescriptor &descriptor) const;
        [[nodiscard]] Result<void> ValidateRenderTargetAttachment(RenderTextureViewHandle handle, RenderTextureAspect requiredAspect,
                                                                  FramebufferExtent extent, std::uint32_t sampleCount) const;

        struct TargetRecord {
            FramebufferExtent extent{};
        };

        struct BufferRecord {
            std::uint32_t generation{0};
            RenderBufferDescriptor descriptor;
            std::weak_ptr<PreparedVfxFrameSlot> vfxSlot;
            bool vfxClaimed{false};   /**< This exact generation cannot restart native publication revisions. */
            bool vfxPreparing{false}; /**< Blocks same-buffer preparation reentry before any adapter callback. */
        };

        struct TextureRecord {
            std::uint32_t generation{0};
            RenderTextureDescriptor descriptor;
        };

        struct TextureViewRecord {
            std::uint32_t generation{0};
            RenderTextureViewDescriptor descriptor;
        };

        std::unique_ptr<IRenderBackend> backend_;
        std::unique_ptr<Detail::RenderSubmissionAllocator> submissionAllocator_;
        /** @brief Checks owner affinity before closing state; no mutable native work is admitted after close. */
        [[nodiscard]] Result<void> RequireOpenOwner() const;

        const std::thread::id preparationOwner_{std::this_thread::get_id()};
        bool closing_{false};
        bool drained_{false};
        std::unique_ptr<RenderMemoryBudget> memoryBudget_;
        std::shared_ptr<Detail::VfxSchedulingStorage> vfxScheduling_;
        std::shared_ptr<const Detail::VfxFrontendState> vfxFrontendState_;
        std::thread::id vfxSchedulingOwner_{std::this_thread::get_id()};
        RenderFrontendMemoryConfig memoryConfig_;
        friend class UiPaintFrameBufferPool;
        friend class UiPaintProgram;
        /** @brief Validate buffer creation at the existing owner boundary before any resource reservation. */
        [[nodiscard]] Result<void> AdmitUiPaintBufferRequest(const RenderBufferDescriptor &descriptor,
                                                             std::span<const std::byte> initialData) const;
        /** @brief Validate texture creation at the existing owner boundary before any resource reservation. */
        [[nodiscard]] Result<void> AdmitUiPaintTextureRequest(const RenderTextureDescriptor &descriptor,
                                                              std::span<const std::byte> initialData) const;
        /** @brief Admits a buffer with its nonallocating UI-owner retirement handoff before publication. */
        [[nodiscard]] Result<ResourceCreation<RenderBufferHandle>> CreateOwnedUiPaintBuffer(
            RenderMemoryScopeId scope, const RenderBufferDescriptor &descriptor, std::span<const std::byte> initialData,
            const std::shared_ptr<Detail::UiPaintOwnerRetirement> &owner);
        /** @brief Admits a texture with its nonallocating UI-owner retirement handoff before publication. */
        [[nodiscard]] Result<ResourceCreation<RenderTextureHandle>> CreateOwnedUiPaintTexture(
            RenderMemoryScopeId scope, const RenderTextureDescriptor &descriptor, std::span<const std::byte> initialData,
            const std::shared_ptr<Detail::UiPaintOwnerRetirement> &owner);
        /** @brief Tries immediate owner-boundary cleanup; deferred requests remain registry-owned. */
        void RetireOwnedUiPaintBuffer(RenderBufferHandle handle, ResourceOperationId operation) noexcept;
        /** @brief Tries immediate owner-boundary cleanup; deferred requests remain registry-owned. */
        void RetireOwnedUiPaintTexture(RenderTextureHandle handle, ResourceOperationId operation) noexcept;
        [[nodiscard]] bool RetireOwnedUiPaintResource(Detail::RenderResourceIdentity identity, ResourceOperationId operation) noexcept;
        std::shared_ptr<Detail::UiPaintFrontendState> uiPaintFrontendState_;
        std::unique_ptr<Detail::RenderResourceRegistry> resourceRegistry_;
        std::unique_ptr<Detail::RenderResourceUploadQueue> resourceUploadQueue_;
        std::unique_ptr<Detail::RenderGraphResourceLeasePool> graphResourceLeases_;
        std::unique_ptr<Detail::RenderGraphTransientResourcePool> graphTransientResources_;
        RenderGraphInspectionFeed inspectionFeed_;
        std::uint64_t inspectionRevision_{};
        RenderFrameScope *activeFrameScope_{nullptr};
        IStaticMeshPassExecutor *staticMeshPassExecutor_{nullptr};
        std::vector<TargetRecord> targets_{{}};
        std::vector<BufferRecord> buffers_{{}};
        std::vector<TextureRecord> textures_{{}};
        std::vector<TextureViewRecord> textureViews_{{}};
    };

}  // namespace Horo::Render
