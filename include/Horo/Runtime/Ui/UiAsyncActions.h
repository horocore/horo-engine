#pragma once

/** @file UiAsyncActions.h
 * @brief Bounded owner-thread asynchronous action state and callback-free completion leases.
 */

#include "Horo/Runtime/Ui/UiActions.h"

namespace Horo::Runtime::Ui {
    /** @brief Closed lifecycle of an admitted asynchronous action. */
    enum class UiAsyncActionState : std::uint8_t {
        Pending,
        Completed,
        Failed,
        Cancelled
    };

    /** @brief Monotonic progress within an explicitly numbered phase. */
    struct UiAsyncActionProgress final {
        std::uint32_t phase{};    /**< Increasing phase ordinal; zero denotes initial preparation. */
        std::uint16_t permille{}; /**< Progress in [0, 1000]; zero when indeterminate. */
        bool determinate{};
        [[nodiscard]] bool operator==(const UiAsyncActionProgress &) const noexcept = default;
    };

    /** @brief Owned immutable projection; contains no widget, tree, callback or execution owner. */
    struct UiAsyncActionSnapshot final {
        UiActionSource source;
        UiActionRequestId request;
        UiActionOperationId operation;
        UiAsyncActionState state{UiAsyncActionState::Pending};
        UiAsyncActionProgress progress;
        UiActionPayload payload;
        std::shared_ptr<const Error> error; /**< Original bounded typed failure, including causes. */
        UiActionCancellationReason cancellation{UiActionCancellationReason::Count};

        /** @brief Reports whether this projection blocks duplicate activation. @return True only while pending. */
        [[nodiscard]] bool Busy() const noexcept {
            return state == UiAsyncActionState::Pending;
        }
    };

    namespace UiAsyncActionDetail {
        struct Record;
    }

    /** @brief Full correlation fence; operation sequence alone is never authority across routers. */
    struct UiAsyncActionKey final {
        UiActionSource source;
        UiActionRequestId request;
        UiActionOperationId operation;
        [[nodiscard]] bool operator==(const UiAsyncActionKey &) const noexcept = default;
    };

    /**
     * @brief Lifetime-safe worker observer of one cancellation request.
     * @details Only IsCancellationRequested may run on workers. Retaining this token pins the slot,
     * so a cancelled operation can never observe a later operation's cancellation state.
     */
    class UiAsyncActionCancellation final {
    public:
        UiAsyncActionCancellation() = default;
        /** @brief Reads the lock-free cooperative signal. @return True after cancellation or for an empty token. */
        [[nodiscard]] bool IsCancellationRequested() const noexcept;

    private:
        friend class UiAsyncActionProducer;
        explicit UiAsyncActionCancellation(std::shared_ptr<const UiAsyncActionDetail::Record> record) noexcept;
        std::shared_ptr<const UiAsyncActionDetail::Record> record_;
    };

    /**
     * @brief Move-only owner-thread completion lease for one exact operation generation.
     * @details A provider owns its jobs/task group independently of UI. Workers receive only Cancellation();
     * immutable prepared progress/results are handed to this lease at the existing owner-command cutoff.
     * No worker mutates this lease or UI storage. Dropping an unfinished lease cancels once. A lease may
     * outlive its router, route, tree or widget, retaining only bounded operation storage.
     */
    class UiAsyncActionProducer final {
    public:
        ~UiAsyncActionProducer();
        UiAsyncActionProducer(UiAsyncActionProducer &&) noexcept;
        UiAsyncActionProducer &operator=(UiAsyncActionProducer &&) noexcept;
        UiAsyncActionProducer(const UiAsyncActionProducer &) = delete;
        UiAsyncActionProducer &operator=(const UiAsyncActionProducer &) = delete;
        /** @brief Returns exact operation correlation. @return Invalid after move. */
        [[nodiscard]] UiAsyncActionKey Key() const noexcept;
        /** @brief Creates a worker-safe observer without allocation. @return Slot-pinning cancellation token. */
        [[nodiscard]] UiAsyncActionCancellation Cancellation() const noexcept;
        /** @brief Publishes monotonic bounded progress at an owner safe point. @param progress Next phase/progress. @return Typed error for
         * stale/invalid progress. */
        [[nodiscard]] Result<void> PublishProgress(UiAsyncActionProgress progress);
        /** @brief Completes once at an owner safe point. @param payload Bounded terminal values. @return Success or typed stale/payload
         * failure. */
        [[nodiscard]] Result<void> Complete(UiActionPayload payload = {});
        /**
         * @brief Publishes an original immutable typed failure once, without copying its strings.
         * @param error Provider-owned immutable error prepared outside frame-hot code; at most eight error nodes
         * including the outer error, sixteen diagnostics per node and 4096 total text bytes are admitted.
         * @return Success or typed stale/invalid-error failure; rejected input leaves the operation pending.
         */
        [[nodiscard]] Result<void> Fail(std::shared_ptr<const Error> error);

    private:
        friend class UiAsyncActionStore;
        explicit UiAsyncActionProducer(std::shared_ptr<UiAsyncActionDetail::Record> record) noexcept;
        /** @brief Borrows mutable operation state. @return Null after move; unavailable on const leases. */
        [[nodiscard]] UiAsyncActionDetail::Record *StateRecord() noexcept;
        /** @brief Borrows read-only operation state. @return Null after move. */
        [[nodiscard]] const UiAsyncActionDetail::Record *StateRecord() const noexcept;
        void Abandon() noexcept;
        std::shared_ptr<UiAsyncActionDetail::Record> record_;
    };

    /**
     * @brief Router-owned preallocated asynchronous state, mutated only by its owner thread.
     * @details Creation allocates all slots. Successful admission, projection, progress, completion and
     * cancellation allocate nothing, poll no jobs, perform no I/O and wait on nothing. Only the cancellation
     * signal crosses threads; it protects cooperative worker observation until the last token retires.
     * Terminal records remain queryable until Release. Pinned slots apply backpressure instead of reuse.
     * A moved-from store has no authority: operations return ActionLifecycleUnavailable and retirement is harmless.
     */
    class UiAsyncActionStore final {
    public:
        /** @brief Reserves finite operation storage. @param owner Exact router owner. @param capacity Bound in [1,
         * MaximumUiActionCommands]. @return Store or typed failure. */
        [[nodiscard]] static Result<UiAsyncActionStore> Create(const UiActionOwnerContext &owner, std::uint32_t capacity);
        ~UiAsyncActionStore();
        UiAsyncActionStore(UiAsyncActionStore &&) noexcept;
        UiAsyncActionStore &operator=(UiAsyncActionStore &&) noexcept;
        UiAsyncActionStore(const UiAsyncActionStore &) = delete;
        UiAsyncActionStore &operator=(const UiAsyncActionStore &) = delete;
        /** @brief Admits one frozen router request. @param request Typed exact source. @return Lease or stale/busy/capacity failure. */
        [[nodiscard]] Result<UiAsyncActionProducer> Start(const UiActionRequest &request);
        /** @brief Copies exact operation state. @param operation Identity to query. @return Projection or stale failure. */
        [[nodiscard]] Result<UiAsyncActionSnapshot> Snapshot(const UiAsyncActionKey &operation) const;
        /** @brief Projects the latest action for an exact control source. @param source Current source. @return Optional projection or
         * stale-source failure. */
        [[nodiscard]] Result<std::optional<UiAsyncActionSnapshot>> Project(const UiActionSource &source) const;
        /** @brief Cancels a pending operation once; terminal results remain unchanged. @param operation Exact identity. @param reason
         * Closed cancellation reason. @return Success or typed invalid/stale failure. */
        [[nodiscard]] Result<void> Cancel(const UiAsyncActionKey &operation, UiActionCancellationReason reason);
        /** @brief Releases terminal retention; outstanding leases still pin the slot. @param operation Exact identity. @return Success or
         * typed busy/stale failure. */
        [[nodiscard]] Result<void> Release(const UiAsyncActionKey &operation);
        /** @brief Idempotently closes admission and cancels every pending operation. @param reason First lifecycle cancellation reason
         * wins. */
        void Retire(UiActionCancellationReason reason) noexcept;

    private:
        struct Storage;
        friend class UiActionRouter;
        /** @brief Checks all requests, completion/cancellation pins and error storage drained before a source generation is reused. */
        [[nodiscard]] bool CanPrepareReplacementSource() const noexcept;
        /** @brief Reinitializes only an inactive, fully drained preallocated source; caller has checked admission. */
        void PrepareReplacementSource(const UiActionOwnerContext &owner, std::uint64_t previousRequest) noexcept;
        /** @brief Releases drained terminal error pins at explicit application quiescence. @return Number of released records. */
        [[nodiscard]] std::size_t DrainReplacementSource() noexcept;
        explicit UiAsyncActionStore(std::unique_ptr<Storage> storage) noexcept;
        /** @brief Borrows mutable owner state. @return Null after move; unavailable on const stores. */
        [[nodiscard]] Storage *StateStorage() noexcept;
        /** @brief Borrows read-only owner state. @return Null after move. */
        [[nodiscard]] const Storage *StateStorage() const noexcept;
        std::unique_ptr<Storage> storage_;
    };

    /** @brief Borrowed scheduling boundary; execution remains in provider-owned structured jobs. */
    class UiAsyncActionHandler {
    public:
        virtual ~UiAsyncActionHandler() = default;
        /** @brief Schedules bounded work without waiting. @param request Frozen typed command, borrowed for this call. @param producer
         * Owned completion lease to retain outside widgets. @return Submission success or original typed rejection. */
        [[nodiscard]] virtual Result<void> Start(const UiActionRequest &request, UiAsyncActionProducer producer) = 0;
    };
}  // namespace Horo::Runtime::Ui
