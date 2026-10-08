#pragma once

/** @file SaveEventTriggers.h
 * @brief Allowlisted, bounded event autosave admission and transition-save decisions.
 */
#include "Horo/Runtime/Save/SaveAutosaveScheduler.h"
#include "Horo/Runtime/Save/SaveSlotIndex.h"

#include <array>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    /** @brief Product-issued trigger identity; zero is reserved. */
    struct SaveTriggerId final {
        std::uint64_t value{};
        [[nodiscard]] constexpr auto operator<=>(const SaveTriggerId &) const noexcept = default;
    };
    /** @brief Exact payload contract and capture side registered by the host. */
    enum class SaveTriggerKind : std::uint8_t {
        Gameplay,
        Milestone,
        Project,
        BeforeTransition,
        AfterTransition
    };
    /** @brief Explicit transition disposition after save failure or cancellation. */
    enum class SaveTransitionFailurePolicy : std::uint8_t {
        Continue,
        Block
    };
    /** @brief Transition owner decision; Wait never blocks the calling thread. */
    enum class SaveTransitionDecision : std::uint8_t {
        NotApplicable,
        Wait,
        Continue,
        Block
    };

    /** @brief Product milestone identity, independent of storage names. */
    struct SaveMilestonePayload final {
        std::uint64_t milestone{};
        [[nodiscard]] constexpr auto operator<=>(const SaveMilestonePayload &) const noexcept = default;
    };

    /** @brief Fixed-capacity project payload; schema and byte count are allowlisted. Unused bytes must be zero. */
    struct SaveProjectTriggerPayload final {
        std::uint32_t schema{};
        std::uint8_t size{};
        std::array<std::byte, 32> bytes{};
        [[nodiscard]] constexpr auto operator<=>(const SaveProjectTriggerPayload &) const noexcept = default;
    };

    /** @brief Host-issued transition identity and exact source/destination incarnations. */
    struct SaveTransitionPayload final {
        std::uint64_t transition{};
        SaveRuntimeGeneration source;
        SaveRuntimeGeneration destination;
        [[nodiscard]] constexpr auto operator<=>(const SaveTransitionPayload &) const noexcept = default;
    };

    /** @brief Payloads contain no paths, target slots or storage capabilities. */
    using SaveTriggerPayload = std::variant<std::monostate, SaveMilestonePayload, SaveProjectTriggerPayload, SaveTransitionPayload>;

    /** @brief Stable async correlation; sequence is nonzero and monotonic per registered trigger/session. */
    struct SaveTriggerCorrelation final {
        SaveTriggerId trigger;
        std::uint64_t sequence{};
        [[nodiscard]] constexpr auto operator<=>(const SaveTriggerCorrelation &) const noexcept = default;
    };

    /** @brief Owned gameplay intent copied before returning to its publisher. */
    struct SaveTriggerEvent final {
        SaveTriggerCorrelation correlation;
        SaveRuntimeGeneration generation;
        SaveTriggerPayload payload;
        [[nodiscard]] constexpr auto operator<=>(const SaveTriggerEvent &) const noexcept = default;
    };

    /** @brief Trusted registration; target selection belongs to host catalog/product policy, never event publishers. */
    struct SaveTriggerRegistration final {
        SaveTriggerId id;
        SaveTriggerKind kind{SaveTriggerKind::Gameplay};
        SavePolicyMode mode{SavePolicyMode::Auto};
        SaveArbiterAddress target;
        SaveTransitionFailurePolicy failure{SaveTransitionFailurePolicy::Continue};
        std::uint32_t projectSchema{};      /**< Required only for Project. */
        std::uint8_t minimumPayloadBytes{}; /**< Project lower bound; maximum is always 32. */
    };

    /** @brief Trusted owner-thread facts, borrowed for the dispatcher's lifetime. */
    struct SaveTriggerHostState final {
        SaveNamespaceBindingSnapshot binding;
        SaveSlotIndex catalog;
        SaveRuntimeGeneration generation;
        SaveAutosaveActivity activity{SaveAutosaveActivity::Active};
        std::uint64_t monotonicMilliseconds{};
        bool authorized{};
    };

    /** @brief Pollable bounded receipt; coalesced publishers receive the original effective correlation and handle. */
    struct SaveTriggerReceipt final {
        SaveTriggerEvent event;
        SaveNamespaceAccessRequest access;
        SaveTriggerKind kind{SaveTriggerKind::Gameplay};
        SaveTransitionFailurePolicy failure{SaveTransitionFailurePolicy::Continue};
        SaveOperationHandle operation;
        bool pending{true};
        std::optional<Error> error; /**< Original admission/fence error, if no producer could run. */
    };

    /** @brief Immutable admission evidence forwarded with the existing asynchronous handle to host capture/storage. */
    struct SaveTriggerHandoff final {
        SaveTriggerReceipt receipt;
        SaveNamespaceAccessRequest access;
        std::uint64_t catalogRevision{};
        SaveArbiterAddress target;
        std::optional<SlotGenerationId> expectedGeneration; /**< Required storage CAS precondition; empty means create. */
    };

    /** @brief Session-owned allowlist with one pending intent and one admitted operation, borrowing existing authorities.
     * Valid Submit performs bounded scalar work and never reads storage or retains live runtime objects.
     * Registrations/receipts are allocated once at Create, with a hard limit of 64. Dependencies outlive this object; close on the owner
     * before destruction. Host owns capture barrier, worker pipeline, operation/coordinator acknowledgement and durable ring rotation.
     */
    class SaveEventTriggers final {
    public:
        SaveEventTriggers(const SaveEventTriggers &) = delete;
        SaveEventTriggers &operator=(const SaveEventTriggers &) = delete;
        /** @brief Validates and copies immutable registrations on the owner thread.
         * @param registrations Nonempty unique trigger allowlist, at most 64.
         * @param policy Cooked product policy; only enabled Auto/Checkpoint modes without UI confirmation are allowed.
         * @param host Trusted facts; borrowed, never modified.
         * @param arbiter Sole session operation authority. @param safePoints Sole generation/capture fence authority.
         * @return Dispatcher or typed configuration/allocation error.
         */
        [[nodiscard]] static Result<std::unique_ptr<SaveEventTriggers>> Create(std::span<const SaveTriggerRegistration> registrations,
                                                                               CookedSaveProjectPolicy policy,
                                                                               const SaveTriggerHostState &host,
                                                                               SaveOperationArbiter &arbiter,
                                                                               SaveSafePointCoordinator &safePoints);
        /** @brief Retains one intent; equivalent rapid events reuse the original correlation without adding work.
         * @param event Exact registered payload and current capture incarnation.
         * @return Effective receipt, or invalid/stale/busy/eligibility error. Distinct busy intents are explicitly rejected.
         * @post A repeated sequence with different data is rejected; older sequences never replay saves.
         */
        [[nodiscard]] Result<SaveTriggerReceipt> Submit(const SaveTriggerEvent &event);
        /** @brief Admits pending intent only at the actual lifecycle commit safe point, idle and after product cooldown.
         * @param phase Actual runtime phase. @param operation Fresh host-issued Save descriptor, used only on admission.
         * @return Empty while busy/cooling down, one correlated handoff, or original typed failure.
         * @post Capture is fenced by the existing safe-point coordinator for the exact source (before) or destination (after).
         */
        [[nodiscard]] Result<std::optional<SaveTriggerHandoff>> CommitAtSafePoint(RuntimePhase phase, SaveOperationDescriptor operation);
        /** @brief Rechecks target/CAS and trusted context under the host capture/publication lease.
         * @param handoff Exact issued evidence. @return Success or stale/denied error; no mutation or I/O.
         */
        [[nodiscard]] Result<void> Revalidate(const SaveTriggerHandoff &handoff) const;
        /** @brief Polls the retained effective correlation, including async terminal state through its handle.
         * @param correlation Effective identity returned by Submit. @return Receipt or stale identity error.
         */
        [[nodiscard]] Result<SaveTriggerReceipt> Receipt(SaveTriggerCorrelation correlation) const;
        /** @brief Closes admission and drops pending intent with a visible cancellation cause; admitted work stays host-owned.
         * @return Success or thread-affinity error. Idempotent.
         */
        [[nodiscard]] Result<void> BeginShutdown();

    private:
        struct Record final {
            SaveTriggerRegistration registration;
            std::optional<SaveTriggerReceipt> receipt;
            std::uint64_t highestSequence{};
        };

        /** @brief Factory-issued capability that prevents unvalidated construction. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class SaveEventTriggers;
        };

    public:
        /** @brief Factory-only construction after allowlist and policy validation.
         * @param policy Validated immutable product policy. @param host Borrowed trusted facts.
         * @param arbiter Borrowed session arbiter. @param safePoints Borrowed capture fence coordinator.
         * @param records Validated bounded registration records. @param key Factory-issued construction capability.
         */
        SaveEventTriggers(CookedSaveProjectPolicy policy, const SaveTriggerHostState &host, SaveOperationArbiter &arbiter,
                          SaveSafePointCoordinator &safePoints, std::vector<Record> records, ConstructionKey key);

    private:
        /** @brief Validates owner affinity and open admission. */
        [[nodiscard]] Result<void> ValidateOwner() const;
        /** @brief Checks host authority, activity, namespace and capture incarnation. */
        [[nodiscard]] Result<void> ValidateContext(const Record &record, const SaveTriggerEvent &event) const;
        /** @brief Resolves only a registered target, validating catalog kind, capacity and exact generation. */
        [[nodiscard]] Result<SaveTriggerHandoff> Resolve(const Record &record) const;
        /** @brief Retains the original failure for transition decisions and releases the single pending slot. */
        void RejectPending(Error error);
        /** @brief Reserves arbiter and safe-point records together and returns exact correlation. */
        [[nodiscard]] Result<SaveTriggerHandoff> Admit(Record &record, SaveTriggerHandoff handoff, SaveOperationDescriptor operation);
        CookedSaveProjectPolicy policy_;
        const SaveTriggerHostState *host_;
        SaveOperationArbiter *arbiter_;
        SaveSafePointCoordinator *safePoints_;
        std::vector<Record> records_;
        std::optional<std::size_t> pending_;
        SaveOperationHandle active_;
        std::array<std::uint64_t, SaveProjectPolicy::ModeCount> admittedAt_{};
        std::array<bool, SaveProjectPolicy::ModeCount> admitted_{};
        std::thread::id owner_;
        std::uint64_t sessionRuntime_;
        bool closed_{};
    };

    /** @brief Derives explicit transition behavior from one correlated pending or terminal receipt.
     * Before-transition owners await durable completion before applying the scene change. After-transition owners
     * hold transition finalization (the destination is already active); Block never implies implicit rollback.
     * @param receipt Exact effective receipt. @return Wait while pending/running, Continue after durable success;
     * configured Continue/Block after failure/cancellation/unknown publication, or NotApplicable for other events.
     */
    [[nodiscard]] SaveTransitionDecision DecideSaveTransition(const SaveTriggerReceipt &receipt);
}  // namespace Horo::Runtime
