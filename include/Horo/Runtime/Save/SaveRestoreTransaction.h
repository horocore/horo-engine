#pragma once

/**
 * @file SaveRestoreTransaction.h
 * @brief Deterministic staged restore preparation, rollback, and aggregate activation.
 */

#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveParticipantRegistry.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Runtime {
    /**
     * @brief Qualified ceiling for participant phase events retained by one restore.
     * @details Seven slots per participant cover five fallible preparation phases plus terminal activation and rollback evidence;
     *          three aggregate slots cover planning, readiness, and a non-participant failure.
     */
    inline constexpr std::size_t MaximumStagedRestoreTraceEvents = MaximumSaveParticipantCount * 7U + 3U;

    /** @brief Ordered preparation and terminal phases of one staged restore. */
    enum class StagedRestorePhase : std::uint8_t {
        Decode,          /**< Decode bounded detached participant input. */
        Validate,        /**< Validate decoded semantic state. */
        Plan,            /**< Select the pinned deterministic restore dependency plan. */
        Instantiate,     /**< Create inactive participant candidates. */
        ApplyState,      /**< Apply canonical values to inactive candidates. */
        FixupReferences, /**< Resolve stable references inside inactive candidates. */
        ReadyToActivate, /**< All fallible preparation completed. */
        Activate,        /**< Publish no-fail ownership transfers. */
        Rollback,        /**< Retire unpublished staging in reverse plan order. */
    };

    /** @brief Outcome recorded for one deterministic restore phase event. */
    enum class StagedRestoreEventOutcome : std::uint8_t {
        Succeeded,
        Failed,
    };

    /** @brief Exclusive ownership state of one restore transaction. */
    enum class StagedRestoreTransactionState : std::uint8_t {
        Created,
        Preparing,
        ReadyToActivate,
        Activated,
        Failed,
        RolledBack,
    };

    /** @brief Stable participant/schema/scope identity declared by staged input. */
    struct StagedRestoreParticipantRequirement final {
        SaveParticipantId participant;                                  /**< Registered canonical owner. */
        ParticipantSchemaVersion schemaVersion;                         /**< Exact decoded participant schema. */
        SaveParticipantScope scope{SaveParticipantScope::RuntimeScene}; /**< Exact authority scope. */

        [[nodiscard]] auto operator<=>(const StagedRestoreParticipantRequirement &) const noexcept = default;
    };

    /** @brief Immutable operation and runtime generation evidence supplied to every phase. */
    struct StagedRestoreContext final {
        OperationId operation{};            /**< Application-owned load operation identity. */
        std::uint64_t registryGeneration{}; /**< Pinned participant registry generation. */
        std::uint64_t sessionGeneration{};  /**< Non-zero owning runtime-session generation. */
        std::uint64_t sceneIncarnation{};   /**< Non-zero active Scene incarnation expected at activation. */
        std::size_t maximumParticipants{};  /**< Positive bounded staged participant count. */

        [[nodiscard]] constexpr auto operator<=>(const StagedRestoreContext &) const noexcept = default;
    };

    /** @brief Current owner generations revalidated immediately before aggregate publication. */
    struct StagedRestoreActivationEvidence final {
        std::uint64_t registryGeneration{}; /**< Current participant registry generation. */
        std::uint64_t sessionGeneration{};  /**< Current owning runtime-session generation. */
        std::uint64_t sceneIncarnation{};   /**< Current active Scene incarnation. */

        [[nodiscard]] constexpr auto operator<=>(const StagedRestoreActivationEvidence &) const noexcept = default;
    };

    /** @brief One allocation-free trace event; participantIndex addresses Participants(), or is absent. */
    struct StagedRestoreTraceEvent final {
        std::uint64_t sequence{};                                                /**< Contiguous one-based event sequence. */
        StagedRestorePhase phase{StagedRestorePhase::Decode};                    /**< Exact phase that completed or failed. */
        StagedRestoreEventOutcome outcome{StagedRestoreEventOutcome::Succeeded}; /**< Phase disposition. */
        std::size_t participantIndex{}; /**< Index in Participants() when hasParticipant is true. */
        bool hasParticipant{};          /**< Whether this event addresses one participant. */

        [[nodiscard]] constexpr auto operator<=>(const StagedRestoreTraceEvent &) const noexcept = default;
    };

    /** @brief Opaque immutable participant candidate exposed only to declared restore dependencies. */
    class ICanonicalRestorePreparedState {
    public:
        virtual ~ICanonicalRestorePreparedState() = default;

    protected:
        ICanonicalRestorePreparedState() = default;
    };

    /** @brief Read-only lookup that exposes only already prepared declared restore dependencies. */
    class ICanonicalRestoreDependencyLookup {
    public:
        virtual ~ICanonicalRestoreDependencyLookup() = default;

        /** @brief Finds one declared prepared dependency. @param participant Stable dependency identity.
         * @return Opaque immutable candidate, or null for an absent/undeclared dependency.
         */
        [[nodiscard]] virtual const ICanonicalRestorePreparedState *Find(const SaveParticipantId &participant) const noexcept = 0;

    protected:
        ICanonicalRestoreDependencyLookup() = default;
    };

    /**
     * @brief Operation-owned inactive staging receipt for one canonical restore participant.
     * @details Every fallible method mutates only private candidate state. PublishPrepared is the sole live-state transfer and must
     *          not allocate, wait, invoke observers, or fail. RollbackPrepared is idempotent and never touches active state.
     */
    class IStagedRestoreParticipant {
    public:
        virtual ~IStagedRestoreParticipant() = default;
        IStagedRestoreParticipant(const IStagedRestoreParticipant &) = delete;
        IStagedRestoreParticipant &operator=(const IStagedRestoreParticipant &) = delete;

        /** @brief Returns exact registry-facing participant evidence. @return Immutable requirement. */
        [[nodiscard]] virtual const StagedRestoreParticipantRequirement &Requirement() const noexcept = 0;
        /** @brief Decodes bounded detached input into private staging. @param context Immutable operation provenance.
         * @return Success or a typed decode failure.
         */
        [[nodiscard]] virtual Result<void> Decode(const StagedRestoreContext &context) = 0;
        /** @brief Validates decoded semantics without touching active state. @param context Immutable operation provenance.
         * @return Success or a typed semantic validation failure.
         */
        [[nodiscard]] virtual Result<void> Validate(const StagedRestoreContext &context) = 0;
        /** @brief Instantiates an inactive private candidate. @param context Immutable operation provenance.
         * @return Success or a typed prerequisite/resource failure.
         */
        [[nodiscard]] virtual Result<void> Instantiate(const StagedRestoreContext &context) = 0;
        /** @brief Returns the instantiated immutable dependency projection. @return Non-null after successful Instantiate. */
        [[nodiscard]] virtual const ICanonicalRestorePreparedState *PreparedState() const noexcept = 0;
        /** @brief Applies canonical values to the inactive candidate. @param dependencies Earlier declared restore dependencies.
         * @return Success or a typed state-application failure.
         */
        [[nodiscard]] virtual Result<void> ApplyState(const ICanonicalRestoreDependencyLookup &dependencies) = 0;
        /** @brief Resolves stable references inside the inactive candidate. @param dependencies Earlier declared dependencies.
         * @return Success or a typed reference-resolution failure.
         */
        [[nodiscard]] virtual Result<void> FixupReferences(const ICanonicalRestoreDependencyLookup &dependencies) = 0;
        /** @brief Publishes the fully prepared candidate through a bounded no-fail ownership transfer. */
        virtual void PublishPrepared() noexcept = 0;
        /** @brief Releases inactive candidate state; repeated calls must be harmless. */
        virtual void RollbackPrepared() noexcept = 0;

    protected:
        IStagedRestoreParticipant() = default;
    };

    /** @brief Move-only aggregate that stages every restore owner and publishes exactly once. */
    class StagedRestoreTransaction final {
    public:
        /** @brief Rolls back unpublished candidate ownership. */
        ~StagedRestoreTransaction();
        StagedRestoreTransaction(const StagedRestoreTransaction &) = delete;
        StagedRestoreTransaction &operator=(const StagedRestoreTransaction &) = delete;
        /** @brief Transfers unique operation and candidate ownership. @param other Source transaction. */
        StagedRestoreTransaction(StagedRestoreTransaction &&other) noexcept;
        /** @brief Rolls back current ownership before accepting the source transaction. @param other Source transaction.
         * @return This transaction.
         */
        StagedRestoreTransaction &operator=(StagedRestoreTransaction &&other) noexcept;

        /**
         * @brief Validates and adopts detached participant staging without invoking participant code.
         * @param context Exact operation, registry, session, Scene, and capacity evidence.
         * @param operation Sole producer for a nonterminal Load operation.
         * @param participants Pinned registry snapshot providing the canonical restore plan and adapter leases.
         * @param staged Unique detached participant staging receipts.
         * @return Created transaction, or typed context, operation, participant, capacity, or allocation failure.
         * @post Every supplied staged receipt is rolled back when creation fails.
         */
        [[nodiscard]] static Result<StagedRestoreTransaction> Create(StagedRestoreContext context, SaveOperationController operation,
                                                                     SaveParticipantRegistrySnapshot participants,
                                                                     std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged);

        /**
         * @brief Runs decode, validation, planning, instantiation, state application, and reference fix-up deterministically.
         * @return Success at ReadyToActivate, or the original typed participant/cancellation/contract failure.
         * @post Failure rolls every receipt back in reverse restore-plan order and terminalizes the load operation once.
         */
        [[nodiscard]] Result<void> Prepare();

        /**
         * @brief Publishes the complete prepared bundle through no-fail ownership transfers.
         * @param evidence Exact current registry, session, and active Scene generations.
         * @return Success, or a typed stale, cancellation, operation, or lifecycle failure before publication.
         * @pre Called only under the host's exclusive lifecycle commit boundary; no reader may observe intermediate transfers.
         */
        [[nodiscard]] Result<void> Activate(StagedRestoreActivationEvidence evidence);

        /** @brief Fails and rolls back an unpublished transaction with the supplied typed cause. @param error Terminal cause. */
        void Rollback(Error error);

        /** @brief Returns current unique ownership state. @return Transaction state. */
        [[nodiscard]] StagedRestoreTransactionState State() const noexcept;
        /** @brief Returns immutable operation/runtime generation evidence. @return Borrowed context. */
        [[nodiscard]] const StagedRestoreContext &Context() const noexcept;
        /** @brief Returns staged requirements in stable participant-identity order. @return Borrowed immutable view. */
        [[nodiscard]] std::span<const StagedRestoreParticipantRequirement> Participants() const noexcept;
        /** @brief Returns deterministic phase events emitted so far. @return Borrowed immutable trace. */
        [[nodiscard]] std::span<const StagedRestoreTraceEvent> Trace() const noexcept;
        /** @brief Returns the consumer operation handle retained by the transaction. @return Copyable handle. */
        [[nodiscard]] SaveOperationHandle Operation() const noexcept;

    private:
        StagedRestoreTransaction(StagedRestoreContext context, SaveOperationController operation,
                                 SaveParticipantRegistrySnapshot participants,
                                 std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged,
                                 std::vector<StagedRestoreParticipantRequirement> requirements, std::vector<std::size_t> restorePlan,
                                 std::vector<StagedRestoreTraceEvent> trace) noexcept;

        void Record(StagedRestorePhase phase, StagedRestoreEventOutcome outcome) noexcept;
        void Record(StagedRestorePhase phase, StagedRestoreEventOutcome outcome, std::size_t participantIndex) noexcept;
        void RollbackCandidates() noexcept;
        [[nodiscard]] Result<void> PublishPreparationProgress(std::uint64_t completedUnits, std::uint64_t totalUnits,
                                                              StagedRestorePhase phase, std::size_t participantIndex);
        [[nodiscard]] Result<void> RunPreparationStep(StagedRestorePhase phase, std::size_t participantIndex, std::size_t visiblePlanLength,
                                                      std::uint64_t &completedUnits, std::uint64_t totalUnits);
        [[nodiscard]] Result<void> RunIdentityPhase(StagedRestorePhase phase, std::uint64_t &completedUnits, std::uint64_t totalUnits);
        [[nodiscard]] Result<void> RunRestorePlanPhase(StagedRestorePhase phase, std::uint64_t &completedUnits, std::uint64_t totalUnits);
        [[nodiscard]] Result<void> EnterReadyToActivate();
        [[nodiscard]] Result<void> FailUnpublished(Error error);
        [[nodiscard]] Result<void> FailPreparation(Error error, StagedRestorePhase phase, std::size_t participantIndex);

        StagedRestoreContext context_;
        SaveOperationController operation_;
        SaveParticipantRegistrySnapshot participants_;
        std::vector<std::unique_ptr<IStagedRestoreParticipant>> staged_;
        std::vector<StagedRestoreParticipantRequirement> requirements_;
        std::vector<std::size_t> restorePlan_;
        std::vector<StagedRestoreTraceEvent> trace_;
        StagedRestoreTransactionState state_{StagedRestoreTransactionState::RolledBack};
    };
}  // namespace Horo::Runtime
