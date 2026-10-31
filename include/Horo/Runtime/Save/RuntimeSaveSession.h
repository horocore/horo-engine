#pragma once
/** @file RuntimeSaveSession.h
 * @brief Explicit runtime capture, detached work and safe-point restore orchestration.
 */
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Save/SaveCaptureBarrier.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"

namespace Horo::Runtime {
    /** @brief One operation-owned worker adapter; no live runtime or editor document may be borrowed.
     * @details Prepare receives only detached immutable state and must perform every fallible prepublication step.
     * Commit publishes that preparation transactionally after the operation's cancellation gate. Success certifies
     * publication; an error conservatively reports Unknown publication. Archive/storage policy belongs to this adapter.
     * Both methods run on the selected JobSystem lane and must not call session-owner APIs.
     * The adapter and all its leases may outlive the session. Last-reference destruction can run on a worker
     * or owner thread; teardown must be thread-safe and must not borrow thread-affine runtime owners.
     */
    class IRuntimeSaveWriter {
    public:
        virtual ~IRuntimeSaveWriter() = default;
        /** @brief Prepare detached state. @param snapshot Owned immutable capture. @param cancellation Worker cancellation.
         * @return Success or original typed preparation failure; no publication may occur here. */
        [[nodiscard]] virtual Result<void> Prepare(const RuntimeSaveSnapshot &snapshot, const CancellationToken &cancellation) = 0;
        /** @brief Publish prepared state once. @return Success or original failure with potentially unknown publication. */
        [[nodiscard]] virtual Result<void> Commit() = 0;
    };

    /** @brief Operation-owned detached restore staging; rollback is owned by the existing restore transaction.
     * @details Receipts, resolver and all leases may outlive the session. Before owner-side preparation, their
     * destruction must be safe on worker or owner threads; no thread-affine runtime retirement may be assumed.
     */
    struct RuntimeSaveRestoreStaging final {
        std::vector<std::unique_ptr<IStagedRestoreParticipant>> participants;
        std::unique_ptr<IStagedRestoreReferenceResolver> references;
    };

    /** @brief Worker-side input adapter shared by packaged, headless and Editor hosts.
     * @details Own all archive/data/module leases; never borrow mutable world state. Stage constructs inactive receipts;
     * their Decode/Validate/Instantiate/Apply/Fixup and publication run later on the owner safe point.
     * Source and lease destruction may occur on either the worker or owner thread after session retirement;
     * they must own their lifetimes and support thread-safe teardown.
     */
    class IRuntimeSaveRestoreSource {
    public:
        virtual ~IRuntimeSaveRestoreSource() = default;
        /** @brief Resolve detached input into bounded staging. @param context Exact target generations and operation.
         * @param participants Pinned canonical registry. @param cancellation Cooperative worker cancellation.
         * @return Receipts/resolver or original typed failure. */
        [[nodiscard]] virtual Result<RuntimeSaveRestoreStaging> Stage(const StagedRestoreContext &context,
                                                                      const SaveParticipantRegistrySnapshot &participants,
                                                                      const CancellationToken &cancellation) = 0;
    };

    namespace RuntimeSaveSessionDetail {
        struct Work;
    }

    /** @brief Copyable thread-safe progress, cancellation and exactly-once completion observation capability. */
    class RuntimeSaveRequest final {
    public:
        /** @brief Read the existing canonical operation handle. @return Owned polling/completion capability. */
        [[nodiscard]] SaveOperationHandle Operation() const noexcept;
        /** @brief Cancel both lifecycle and detached work without waiting. @return Canonical cancellation disposition. */
        [[nodiscard]] SaveCancellationRequestResult RequestCancellation() const noexcept;

    private:
        friend class RuntimeSaveSession;
        explicit RuntimeSaveRequest(std::shared_ptr<RuntimeSaveSessionDetail::Work> work) noexcept;
        std::shared_ptr<RuntimeSaveSessionDetail::Work> work_;
    };

    /** @brief One owner-thread runtime/scene/registry incarnation, independent of editor persistence.
     * @details At most one operation is retained until Acknowledge, including settled terminal work. Requests return
     * before capture/I/O; polling/cancellation/observation are thread-safe through RuntimeSaveRequest. Submission,
     * pumping, retirement and acknowledgement require the construction thread. The host owns the supplied actual
     * capture barrier, registers all four domains and every capture owner, and publishes genuine mutation/epoch
     * evidence. No readiness, content-install proof or Scene/world authority is fabricated by this service.
     * Worker closures own all request inputs and registry/adapter leases, never this session. Retire closes admission
     * without waiting; wait for IsSettled before releasing external module/runtime owners. Destruction retires safely,
     * while outstanding detached work retains its owned leases. Recompose after Scene/module generation replacement.
     */
    class RuntimeSaveSession final {
    public:
        /** @brief Compose against explicit runtime owners. @param jobs Scheduler outliving this session.
         * @param barrier Actual host mutation/epoch authority outliving this session.
         * @param participants Actual pinned current registry snapshot. @param generation Exact active incarnation.
         * @param limits Qualified capture bounds. @param workerLane Explicit CPU or dedicated I/O lane.
         * @return Session or typed generation/configuration/allocation error. */
        [[nodiscard]] static Result<std::unique_ptr<RuntimeSaveSession>> Create(JobSystem &jobs, SaveCaptureBarrier &barrier,
                                                                                SaveParticipantRegistrySnapshot participants,
                                                                                SaveRuntimeGeneration generation,
                                                                                RuntimeSaveCaptureLimits limits = {},
                                                                                JobResource workerLane = JobResource::Cpu);
        /** @brief Retire on the construction thread; detached closures retain their owned leases. */
        ~RuntimeSaveSession();
        RuntimeSaveSession(const RuntimeSaveSession &) = delete;
        RuntimeSaveSession &operator=(const RuntimeSaveSession &) = delete;
        /** @brief Admit capture and detached publication without callbacks or waiting.
         * @param operation Fresh host-issued OperationStore descriptor of kind Save. @param writer Owned publication adapter.
         * @return Thread-safe request or typed busy/identity/lifecycle failure. */
        [[nodiscard]] Result<RuntimeSaveRequest> Save(SaveOperationDescriptor operation, std::shared_ptr<IRuntimeSaveWriter> writer);
        /** @brief Admit detached input staging and owner-safe-point restore.
         * @param operation Fresh host-issued descriptor of kind Load. @param source Owned detached staging adapter.
         * @return Thread-safe request or typed scheduler/admission failure. */
        [[nodiscard]] Result<RuntimeSaveRequest> Load(SaveOperationDescriptor operation, std::shared_ptr<IRuntimeSaveRestoreSource> source);
        /** @brief Pump capture and restore only under the host's exclusive lifecycle boundary.
         * @param phase Must be CommitDeferredLifecycleChanges. @param current Actual current runtime/Scene/registry generations.
         * @param provenance Genuine current capture epoch/source evidence, required for pending Save, ignored for Load.
         * @return Success for pending/completed progress or original typed capture/restore/lifecycle error.
         * @details Wrong phase changes no state; stale generation retires pending work before any callback.
         */
        [[nodiscard]] Result<void> CommitAtSafePoint(RuntimePhase phase, SaveRuntimeGeneration current,
                                                     const RuntimeSaveCaptureProvenance &provenance = {});
        /** @brief Close admission and cancel precommit work without joining. @return Owner/reentrancy failure or success. */
        [[nodiscard]] Result<void> Retire();
        /** @brief Check whether detached work released its execution lane. @return Owner-thread settlement fact. */
        [[nodiscard]] bool IsSettled() const;
        /** @brief Release the one terminal settled record. @return Success or typed busy/lifecycle error. */
        [[nodiscard]] Result<void> Acknowledge();

    private:
        RuntimeSaveSession(JobSystem &jobs, SaveCaptureBarrier &barrier, SaveParticipantRegistrySnapshot participants,
                           SaveRuntimeGeneration generation, RuntimeSaveCaptureLimits limits, JobResource workerLane,
                           std::shared_ptr<SaveSafePointCoordinator> safePoints) noexcept;
        [[nodiscard]] Result<void> ValidateOwner() const;
        [[nodiscard]] Result<std::shared_ptr<RuntimeSaveSessionDetail::Work>> Admit(SaveOperationDescriptor operation);
        [[nodiscard]] Result<void> Dispatch(const std::shared_ptr<RuntimeSaveSessionDetail::Work> &work);
        [[nodiscard]] Result<void> Pump(const RuntimeSaveCaptureProvenance &provenance);
        /** @brief Retire beneath an already held owner transition fence, including stale safe-point admission. */
        [[nodiscard]] Result<void> RetireInternal();
        void ReleaseBarrier() noexcept;
        JobSystem &jobs_;
        SaveCaptureBarrier &barrier_;
        SaveParticipantRegistrySnapshot participants_;
        SaveRuntimeGeneration generation_;
        RuntimeSaveCaptureLimits limits_;
        JobResource workerLane_;
        std::shared_ptr<SaveSafePointCoordinator> safePoints_;
        std::shared_ptr<RuntimeSaveSessionDetail::Work> work_;
        std::thread::id owner_;
        bool closed_{};
        bool mutating_{};
        bool barrierPending_{};
    };
}  // namespace Horo::Runtime
