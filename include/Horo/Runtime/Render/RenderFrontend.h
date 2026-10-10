#pragma once

#include "Horo/Runtime/Render/RenderGraphInspection.h"

/**
 * @file RenderFrontend.h
 * @brief Host-facing owner of one selected and initialized renderer backend.
 */

#include "Horo/Runtime/Render/RenderBackendRegistry.h"
#include "Horo/Runtime/Render/RenderGraphTransientResources.h"
#include "Horo/Runtime/Render/RenderMemoryBudget.h"
#include "Horo/Runtime/Render/RenderParallelWork.h"

#include <memory>
#include <span>
#include <vector>

namespace Horo::Render {
    class RenderFrontend;
    class CompiledRenderGraphExecution;
    class RenderGraphLifetimePlan;
    struct RenderGraphPassWorkload;
    struct UiRenderSubmission;
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
        class RenderResourceRegistry;
        class RenderResourceUploadQueue;
        class RenderFrontendResourceAccess;
        class RenderGraphResourceLeasePool;
        class RenderGraphTransientResourcePool;
        class RenderParallelWorkState;
        class RenderParallelGraphWorkState;
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

        /** @brief Reports whether every completion and destruction bound is finite and non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumSubmissionPins > 0 && maximumTrackedQueues > 0 && maximumCompletionsPerDrain > 0 &&
                   maximumRetirementsPerDrain > 0;
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

        /**
         * @brief Executes the ordered pass sequence for this frame exactly once.
         * @param orderedPasses Non-owning pass sequence valid for this call.
         * @return Success, a typed invalid-stage error, the original backend failure,
         * or a translated backend exception.
         */
        [[nodiscard]] Result<void> Execute(std::span<const RenderPassDescriptor> orderedPasses);

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
         * @return Success, a typed invalid-stage error, the original backend failure,
         * or a translated backend exception.
         */
        [[nodiscard]] Result<void> Present();

        /** @brief Explicitly aborts the owned frame; safe to call repeatedly. */
        void Cancel() noexcept;

    private:
        friend class RenderFrontend;

        RenderFrameScope(RenderFrontend &owner, IRenderBackend &backend, FrameToken frame) noexcept;
        /** @brief Common graph admission preserving one authoritative native completion lease. */
        [[nodiscard]] Result<void> ExecuteGraphInternal(const CompiledRenderGraphExecution &graph,
                                                        std::span<const RenderGraphPassWorkload> workloads, UiRenderSubmission *ui,
                                                        RenderGraphTransientResourcesHandle transientResources = {});
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
        bool executed_{false};
        std::unique_ptr<Detail::RenderParallelWorkState> parallelWork_;
        std::unique_ptr<Detail::RenderParallelGraphWorkState> parallelGraphWork_;
    };

    /**
     * @brief Owns the initialized renderer backend for one host lifetime.
     *
     * Construction performs explicit registry selection and backend initialization.
     * Destruction deterministically shuts the backend down before releasing it.
     * All methods and destruction must run serially on one host-declared render-capable
     * thread. The frontend and its frame scope are not thread-safe.
     */
    class RenderFrontend final {  // NOSONAR(cpp:S1448) Cohesive public facade for one renderer frontend lifetime.
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

        /** @brief Shuts down and releases the owned backend. */
        ~RenderFrontend();

        RenderFrontend(const RenderFrontend &) = delete;
        RenderFrontend &operator=(const RenderFrontend &) = delete;
        RenderFrontend(RenderFrontend &&) = delete;
        RenderFrontend &operator=(RenderFrontend &&) = delete;

        /**
         * @brief Returns the immutable capability snapshot of the initialized backend.
         * @return Reference valid until frontend destruction; safe to query during an active frame.
         */
        [[nodiscard]] const RenderBackendCapabilities &Capabilities() const noexcept;

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
        [[nodiscard]] bool IsLiveTarget(RenderTargetHandle target, FramebufferExtent extent) const noexcept;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderBufferHandle buffer) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderMeshHandle mesh) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderTextureViewHandle view) const;
        [[nodiscard]] Result<std::uint64_t> BackendInstance(RenderTargetHandle target) const;
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
        std::unique_ptr<RenderMemoryBudget> memoryBudget_;
        RenderFrontendMemoryConfig memoryConfig_;
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
