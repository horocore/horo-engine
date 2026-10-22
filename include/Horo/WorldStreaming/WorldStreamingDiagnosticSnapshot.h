#pragma once

/**
 * @file WorldStreamingDiagnosticSnapshot.h
 * @brief Immutable bounded diagnostic projection of World Streaming authority facts.
 */

#include "Horo/WorldStreaming/StreamingBudgetModel.h"
#include "Horo/WorldStreaming/StreamingCellState.h"
#include "Horo/WorldStreaming/StreamingDesiredState.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Tag separating diagnostic revisions from authority and policy revisions. */
        struct WorldStreamingDiagnosticRevisionTag;
        /** @brief Tag separating structured event identities from snapshot revisions. */
        struct StreamingDiagnosticEventIdTag;
        /** @brief Tag separating event order from event and operation identities. */
        struct StreamingDiagnosticEventSequenceTag;
    }  // namespace Detail

    /** @brief Monotonic revision of one immutable diagnostic projection. */
    using WorldStreamingDiagnosticRevision =
        Foundation::Detail::NonZeroId64<Detail::WorldStreamingDiagnosticRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Stable identity for one structured streaming decision record. */
    using StreamingDiagnosticEventId =
        Foundation::Detail::NonZeroId64<Detail::StreamingDiagnosticEventIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Monotonic owner-supplied ordering fact for structured streaming decisions. */
    using StreamingDiagnosticEventSequence =
        Foundation::Detail::NonZeroId64<Detail::StreamingDiagnosticEventSequenceTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Closed event families emitted by the streaming authority. */
    enum class StreamingDiagnosticEventCategory : std::uint8_t {
        CellLifecycle,
        Admission,
        Rollback
    };

    /** @brief Event importance independent of the operation's typed result. */
    enum class StreamingDiagnosticEventSeverity : std::uint8_t {
        Info,
        Warning,
        Error
    };

    /** @brief Closed scheduler decision vocabulary; not a scheduler command or result. */
    enum class StreamingDiagnosticAdmissionDecision : std::uint8_t {
        Accepted,
        Deferred,
        Rejected
    };

    /** @brief Typed bounded metadata keys safe for diagnostics and external projection. */
    enum class StreamingDiagnosticContextKey : std::uint8_t {
        RequestedCapacityUnits,
        ReservedCapacityUnits,
        QueueDepth,
        BudgetRevision,
    };

    /** @brief One bounded numeric diagnostic context field; native/provider strings are intentionally excluded. */
    struct StreamingDiagnosticContextField final {
        StreamingDiagnosticContextKey key{}; /**< Stable machine-readable field key. */
        std::uint64_t value{};               /**< Safe unsigned numeric evidence. */
        [[nodiscard]] constexpr auto operator<=>(const StreamingDiagnosticContextField &) const noexcept = default;
    };

    /** @brief One proposed canonical residency transition. */
    struct StreamingDiagnosticCellTransition final {
        StreamingCellState previous{}; /**< Residency fact before the decision. */
        StreamingCellState current{};  /**< Residency fact after the decision. */
        [[nodiscard]] constexpr auto operator<=>(const StreamingDiagnosticCellTransition &) const noexcept = default;
    };

    /** @brief Immutable authority-authored lifecycle, admission or rollback evidence. */
    struct StreamingDiagnosticDecisionEvent final {
        static constexpr std::size_t MaximumContextFields = 4;

        StreamingDiagnosticEventId id;                                               /**< Stable event identity. */
        StreamingDiagnosticEventSequence sequence;                                   /**< Canonical order within the owner lifetime. */
        StreamingRuntimeCompositionRevision ownerRevision;                           /**< Exact authority composition revision. */
        WorldStreamingDiagnosticRevision snapshotRevision;                           /**< Exact projection revision that owns this row. */
        StreamingCellOperationHandle operation;                                      /**< Exact cell-attempt and operation identity. */
        StreamingDiagnosticEventCategory category{};                                 /**< Lifecycle, admission or rollback family. */
        StreamingDiagnosticEventSeverity severity{};                                 /**< Importance separate from typed outcome. */
        std::optional<StreamingDiagnosticCellTransition> transition;                 /**< State change when required by the category. */
        std::optional<StreamingDiagnosticAdmissionDecision> admission;               /**< Admission decision when applicable. */
        StreamingCellOperationOutcome outcome{};                                     /**< Typed result evidence; None is not failure. */
        std::array<StreamingDiagnosticContextField, MaximumContextFields> context{}; /**< Inline bounded safe metadata. */
        std::uint8_t contextCount{};                                                 /**< Number of valid leading context entries. */
    };

    /** @brief Controls whether structured events are retained in this projection. */
    enum class StreamingDiagnosticInstrumentation : std::uint8_t {
        Enabled,
        Disabled
    };

    /** @brief Mandatory caller ceilings for owned diagnostic rows. */
    struct WorldStreamingDiagnosticLimits final {
        static constexpr std::uint32_t MaximumSources = 4096;
        static constexpr std::uint32_t MaximumCells = StreamingCellStateLedgerConfig::MaximumTrackedAttempts;
        static constexpr std::uint32_t MaximumFailures = StreamingCellStateLedgerConfig::MaximumTrackedAttempts;
        static constexpr std::uint32_t MaximumEvents = StreamingCellStateLedgerConfig::MaximumTrackedAttempts * 4;

        std::uint32_t sources{};             /**< Maximum source rows copied into one snapshot. */
        std::uint32_t cells{};               /**< Maximum current or retiring cell-attempt rows. */
        std::uint32_t failures{};            /**< Maximum terminal or pending-retirement failure rows. */
        std::uint32_t events{MaximumEvents}; /**< Maximum structured decision rows. */

        /** @brief Validates positive implementation-bounded row ceilings. @return True when every ceiling is usable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Immutable queue pressure facts read from the authority-owned scheduler ledger. */
    struct StreamingDiagnosticQueueSnapshot final {
        StreamingSchedulerLedgerId owner;         /**< Exact scheduler-ledger owner lifetime. */
        StreamingSchedulerAdmissionLimits limits; /**< Immutable admitted queue/capacity ceilings. */
        StreamingSchedulerAdmissionState state{}; /**< Accepting, draining or closed admission state. */
        std::uint32_t reservedOperations{};       /**< Operations still retained by the ledger. */
        std::uint64_t reservedCapacityUnits{};    /**< Generic capacity still charged. */
    };

    /** @brief Typed terminal or pending-retirement cause for one exact cell operation. */
    struct StreamingDiagnosticFailureRecord final {
        StreamingCellOperationHandle operation; /**< Exact operation and cell-attempt fence. */
        StreamingCellOperationOutcome reason{}; /**< Cancelled, Failed, Replaced or Shutdown. */

        [[nodiscard]] constexpr auto operator<=>(const StreamingDiagnosticFailureRecord &) const noexcept = default;
    };

    /** @brief Borrowed authority-safe-point facts copied transactionally by snapshot creation. */
    struct WorldStreamingDiagnosticSnapshotInput final {
        StreamingRuntimeOwnerToken owner;                  /**< Exact mounted authority lifetime. */
        WorldStreamingDiagnosticRevision revision;         /**< Non-zero immutable snapshot revision. */
        StreamingRuntimeCompositionRevision ownerRevision; /**< Exact runtime-composition revision captured at the safe point. */
        WorldStreamingRuntimeCompositionState lifecycle{}; /**< Current composition lifecycle fact. */
        StreamingDiagnosticInstrumentation instrumentation{StreamingDiagnosticInstrumentation::Enabled}; /**< Event retention policy. */
        WorldStreamingDiagnosticLimits limits;  /**< Mandatory owned-row ceilings. */
        StreamingDiagnosticQueueSnapshot queue; /**< Scheduler queue pressure at the same safe point. */
    };

    /** @brief One borrowed, bounded authority-safe-point row set copied atomically into a diagnostic snapshot. */
    struct WorldStreamingDiagnosticRows final {
        std::span<const StreamingSourceDesiredState> sources;       /**< Current source desired-state rows. */
        std::span<const StreamingCellStateRecord> cells;            /**< Current and retiring cell-attempt rows. */
        std::span<const StreamingDiagnosticFailureRecord> failures; /**< Typed causes correlated to @ref cells. */
        std::span<const StreamingDiagnosticDecisionEvent> events;   /**< Structured decisions for this revision. */
    };

    /**
     * @brief Owned immutable, deterministic and bounded World Streaming diagnostic snapshot.
     * @details Creation copies authority-safe-point facts only. The value performs no discovery, logging,
     *          serialization, runtime mutation or independent residency/budget accounting.
     */
    class WorldStreamingDiagnosticSnapshot final {
    public:
        /**
         * @brief Validates and owns one complete diagnostic projection.
         * @param input Owner, revision, lifecycle, queue and mandatory capacity facts.
         * @param policy Current immutable multidimensional budget policy.
         * @param sample Current usage sample for exactly @p policy.
         * @param rows Current source, cell, failure and structured decision rows.
         * @return Canonically ordered snapshot, or a typed invalid, stale, unsupported, conflict or capacity failure.
         * @post Failure publishes no partial snapshot and leaves every input unchanged.
         */
        [[nodiscard]] static Result<WorldStreamingDiagnosticSnapshot> Create(const WorldStreamingDiagnosticSnapshotInput &input,
                                                                             const StreamingBudgetPolicy &policy,
                                                                             const StreamingBudgetSample &sample,
                                                                             const WorldStreamingDiagnosticRows &rows);

        /** @brief Captures actual state when this owner has no budget-measurement capability.
         * @param input Actual owner, revision, lifecycle, queue and bounded row ceilings.
         * @param rows Actual source/residency/failure/decision rows, validated identically to Create.
         * @return Canonical snapshot with explicitly unavailable budget policy/sample, or typed failure.
         * @details Absence is never converted to zero usage or a fabricated policy. */
        [[nodiscard]] static Result<WorldStreamingDiagnosticSnapshot> CreateStateOnly(const WorldStreamingDiagnosticSnapshotInput &input,
                                                                                      const WorldStreamingDiagnosticRows &rows);

        /**
         * @brief Validates a complete successor while preserving the prior immutable snapshot on failure.
         * @param previous Currently published snapshot.
         * @param input Complete successor authority facts with a strictly newer projection revision.
         * @param policy Current immutable budget policy. @param sample Matching usage sample.
         * @param rows Complete successor rows; events are ignored without allocation when instrumentation is disabled.
         * @return Complete successor, or a typed failure without modifying @p previous.
         */
        [[nodiscard]] static Result<WorldStreamingDiagnosticSnapshot> Replace(const WorldStreamingDiagnosticSnapshot &previous,
                                                                              const WorldStreamingDiagnosticSnapshotInput &input,
                                                                              const StreamingBudgetPolicy &policy,
                                                                              const StreamingBudgetSample &sample,
                                                                              const WorldStreamingDiagnosticRows &rows);

        /** @brief Returns the exact mounted authority lifetime. @return Immutable owner token. */
        [[nodiscard]] const StreamingRuntimeOwnerToken &Owner() const noexcept;
        /** @brief Returns this immutable projection revision. @return Non-zero revision. */
        [[nodiscard]] WorldStreamingDiagnosticRevision Revision() const noexcept;
        /** @brief Returns the exact captured runtime-composition revision. @return Non-zero owner revision. */
        [[nodiscard]] StreamingRuntimeCompositionRevision OwnerRevision() const noexcept;
        /** @brief Returns the captured authority lifecycle. @return Active, cancelling, draining or closed. */
        [[nodiscard]] WorldStreamingRuntimeCompositionState Lifecycle() const noexcept;
        /** @brief Returns captured scheduler queue pressure. @return Immutable queue facts. */
        [[nodiscard]] const StreamingDiagnosticQueueSnapshot &Queue() const noexcept;
        /** @brief Returns the captured multidimensional budget policy. @return Immutable owned policy, or null when this owner cannot
         * supply budget observations. */
        [[nodiscard]] const StreamingBudgetPolicy *BudgetPolicy() const noexcept;
        /** @brief Returns the captured multidimensional usage sample. @return Immutable owned sample, or null together with an unavailable
         * budget policy. */
        [[nodiscard]] const StreamingBudgetSample *BudgetSample() const noexcept;
        /** @brief Returns source rows in stable source-identity order. @return Borrow valid for this value's lifetime. */
        [[nodiscard]] std::span<const StreamingSourceDesiredState> Sources() const noexcept;
        /** @brief Returns cell attempts in canonical cell/generation order. @return Borrow valid for this value's lifetime. */
        [[nodiscard]] std::span<const StreamingCellStateRecord> Cells() const noexcept;
        /** @brief Returns failures in canonical cell/generation/operation order. @return Borrow valid for this value's lifetime. */
        [[nodiscard]] std::span<const StreamingDiagnosticFailureRecord> Failures() const noexcept;
        /** @brief Returns decisions in stable sequence order. @return Empty when instrumentation was disabled. */
        [[nodiscard]] std::span<const StreamingDiagnosticDecisionEvent> Events() const noexcept;

    private:
        /** @brief Shares canonical row validation for measured-budget and explicitly state-only captures. */
        [[nodiscard]] static Result<WorldStreamingDiagnosticSnapshot> Capture(const WorldStreamingDiagnosticSnapshotInput &input,
                                                                              std::optional<StreamingBudgetPolicy> policy,
                                                                              std::optional<StreamingBudgetSample> sample,
                                                                              const WorldStreamingDiagnosticRows &rows);
        /** @brief Owns an already validated and canonically ordered projection. */
        WorldStreamingDiagnosticSnapshot(WorldStreamingDiagnosticSnapshotInput input, std::optional<StreamingBudgetPolicy> policy,
                                         std::optional<StreamingBudgetSample> sample, std::vector<StreamingSourceDesiredState> sources,
                                         std::vector<StreamingCellStateRecord> cells,
                                         std::vector<StreamingDiagnosticFailureRecord> failures,
                                         std::vector<StreamingDiagnosticDecisionEvent> events) noexcept;

        WorldStreamingDiagnosticSnapshotInput input_;
        std::optional<StreamingBudgetPolicy> policy_;
        std::optional<StreamingBudgetSample> sample_;
        std::vector<StreamingSourceDesiredState> sources_;
        std::vector<StreamingCellStateRecord> cells_;
        std::vector<StreamingDiagnosticFailureRecord> failures_;
        std::vector<StreamingDiagnosticDecisionEvent> events_;
    };
}  // namespace Horo::WorldStreaming
