#pragma once

/**
 * @file SaveSlotRecovery.h
 * @brief Bounded last-known-good save-slot validation and recovery policy.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveArchiveReader.h"
#include "Horo/Runtime/Save/SaveStorageAdapter.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Runtime {
    inline constexpr std::size_t MaximumSaveSlotRecoveryBackups = 32;
    inline constexpr std::size_t MaximumSaveSlotRecoveryQuarantined = 32;
    inline constexpr std::size_t MaximumSaveSlotRecoveryObservedArtifacts = 128;

    /** @brief Reason why the current slot needs a last-known-good lookup. */
    enum class SaveSlotRecoveryTrigger : std::uint8_t {
        CorruptCurrent,
        IncompatibleCurrent,
        InterruptedPublication,
        Count,
    };

    /** @brief Validation state exposed for one recovery artifact. */
    enum class SaveSlotRecoveryValidationState : std::uint8_t {
        Valid,
        Corrupt,
        Incompatible,
        Count,
    };

    /** @brief Product policy controlling when a validated backup may be promoted automatically. */
    enum class SaveSlotRecoveryAutomaticPolicy : std::uint8_t {
        Disabled,
        CorruptCurrent,
        CorruptOrInterrupted,
        Count,
    };

    /** @brief Typed recovery presentation decision; UI owns confirmation and localized copy. */
    enum class SaveSlotRecoveryDecision : std::uint8_t {
        NoRecovery,
        AutomaticPromotion,
        UserConfirmationRequired,
        Count,
    };

    /** @brief Stable explanation for a recovery decision. */
    enum class SaveSlotRecoveryDecisionReason : std::uint8_t {
        CurrentValid,
        NoValidBackup,
        AutomaticPolicy,
        AutomaticPolicyDisabled,
        IncompatibleCurrentRequiresConfirmation,
        InterruptedPublicationRequiresConfirmation,
        Count,
    };

    /** @brief Physical recovery collection containing an artifact before cleanup. */
    enum class SaveSlotRecoveryArtifactKind : std::uint8_t {
        Backup,
        Quarantine,
        Count,
    };

    /**
     * @brief Bounded product policy for recovery copies and evidence retention.
     *
     * `retentionSequence` on each artifact is supplied by the storage authority. It is the
     * only ordering input used for retention; archive timestamps and generation bytes are not
     * treated as causality.
     */
    struct SaveSlotRecoveryPolicy final {
        std::size_t maximumBackups{3};
        std::size_t maximumQuarantined{8};
        std::size_t maximumObservedArtifacts{MaximumSaveSlotRecoveryObservedArtifacts};
        SaveSlotRecoveryAutomaticPolicy automaticPromotion{SaveSlotRecoveryAutomaticPolicy::CorruptCurrent};
    };

    /**
     * @brief One storage-owned recovery artifact with optional trusted catalog metadata.
     *
     * Missing metadata or bytes are retained as evidence and classified as corrupt, but can
     * never become a promotion candidate. The archive is immutable and owned by the caller's
     * storage lease; the planner copies only the shared ownership token.
     */
    struct SaveSlotRecoveryArtifact final {
        std::optional<SaveSlotCatalogEntry> metadata;
        ImmutableSaveArchive archive;
        std::uint64_t retentionSequence{};
    };

    /** @brief Bounded validation evidence retained with a recovery artifact. */
    struct SaveSlotRecoveryValidation final {
        SaveSlotRecoveryValidationState state{SaveSlotRecoveryValidationState::Corrupt};
        bool requiresMigration{};
        std::optional<SaveCompatibilityDecision> compatibility;
        std::optional<Error> diagnostic;
    };

    /**
     * @brief Injected validation seam for recovery artifacts.
     *
     * Implementations must validate framing/integrity, slot identity, and compatibility without
     * mutating the active slot. Corrupt and incompatible input is returned as a successful typed
     * classification so callers can quarantine and explain it rather than collapsing it into a
     * missing backup.
     */
    class ISaveSlotRecoveryValidator {
    public:
        virtual ~ISaveSlotRecoveryValidator() = default;

        /**
         * @brief Validates one artifact for a requested logical slot.
         * @param artifact Immutable artifact and storage ordering evidence.
         * @param expectedSlot Requested logical slot identity.
         * @return Valid, corrupt, or incompatible classification; malformed validator configuration may fail.
         */
        [[nodiscard]] virtual Result<SaveSlotRecoveryValidation> Validate(const SaveSlotRecoveryArtifact &artifact,
                                                                          SaveGameSlotId expectedSlot) const = 0;
    };

    /**
     * @brief Production validator using the bounded archive reader and current compatibility policy.
     *
     * Archive admission errors that indicate malformed or contradictory bytes are classified as
     * Corrupt. Unsupported format/features and rejected compatibility ranges are classified as
     * Incompatible. Neither result is offered for promotion.
     */
    class SaveArchiveRecoveryValidator final : public ISaveSlotRecoveryValidator {
    public:
        /**
         * @brief Binds immutable archive and compatibility admission policy.
         * @param reader Bounded archive reader.
         * @param compatibility Current product compatibility policy.
         */
        SaveArchiveRecoveryValidator(SaveArchiveReader reader, SaveCompatibilityPolicy compatibility);

        /** @copydoc ISaveSlotRecoveryValidator::Validate */
        [[nodiscard]] Result<SaveSlotRecoveryValidation> Validate(const SaveSlotRecoveryArtifact &artifact,
                                                                  SaveGameSlotId expectedSlot) const override;

    private:
        SaveArchiveReader reader_;
        SaveCompatibilityPolicy compatibility_;
    };

    /** @brief Recovery artifact together with its validation evidence. */
    struct SaveSlotRecoveryCandidate final {
        SaveSlotRecoveryArtifact artifact;
        SaveSlotRecoveryValidation validation;
    };

    /** @brief One deterministic cleanup operation deferred until promotion/publication succeeds. */
    struct SaveSlotRecoveryCleanup final {
        SaveSlotRecoveryArtifactKind kind{SaveSlotRecoveryArtifactKind::Backup};
        SaveSlotRecoveryArtifact artifact;
    };

    /** @brief Detached recovery input collected under one namespace/slot lease. */
    struct SaveSlotRecoveryRequest final {
        SaveGameSlotId slot;
        SaveSlotRecoveryTrigger trigger{SaveSlotRecoveryTrigger::CorruptCurrent};
        std::optional<SaveSlotRecoveryArtifact> current;
        std::span<const SaveSlotRecoveryArtifact> backups;
        std::span<const SaveSlotRecoveryArtifact> quarantined;
    };

    /**
     * @brief Complete deterministic recovery decision and deferred retention work.
     *
     * `promotion` is an immutable input for the existing slot commit transaction. The plan does
     * not publish, rename, quarantine, or delete anything; callers apply cleanup only after that
     * transaction reports durable success. The current invalid artifact is always protected from
     * cleanup while this plan is being applied.
     */
    struct SaveSlotRecoveryPlan final {
        std::optional<SaveSlotRecoveryValidation> currentValidation;
        std::vector<SaveSlotRecoveryCandidate> inspectedBackups;
        std::vector<SaveSlotRecoveryCandidate> quarantine;
        std::vector<SaveSlotRecoveryArtifact> retainedBackups;
        std::vector<SaveSlotRecoveryArtifact> retainedQuarantine;
        std::vector<SaveSlotRecoveryCleanup> cleanup;
        std::optional<SaveSlotRecoveryCandidate> promotion;
        SaveSlotRecoveryDecision decision{SaveSlotRecoveryDecision::NoRecovery};
        SaveSlotRecoveryDecisionReason decisionReason{SaveSlotRecoveryDecisionReason::NoValidBackup};

        /** @brief Reports whether a validated artifact is ready for the commit transaction. @return True when promotion exists. */
        [[nodiscard]] bool HasPromotion() const noexcept;
        /** @brief Reports whether the host must obtain explicit user confirmation. @return True for confirmation-required decisions. */
        [[nodiscard]] bool RequiresUserConfirmation() const noexcept;
    };

    /**
     * @brief Builds a bounded last-known-good plan without mutating storage or live runtime state.
     *
     * Backups are sorted by descending trusted retention sequence, independent of scan order.
     * Invalid backups are moved to quarantine evidence, while only Valid backups can be selected.
     */
    class SaveSlotRecoveryPlanner final {
    public:
        /**
         * @brief Binds a validator and immutable retention policy.
         * @param validator Artifact validator that outlives the planner.
         * @param policy Bounded retention and promotion policy.
         */
        SaveSlotRecoveryPlanner(const ISaveSlotRecoveryValidator &validator, SaveSlotRecoveryPolicy policy) noexcept;

        /**
         * @brief Validates artifacts, selects one candidate, and computes deferred cleanup.
         * @param request Detached artifacts captured under the exact slot lease.
         * @return Deterministic plan or a typed validation/limit/allocation error.
         */
        [[nodiscard]] Result<SaveSlotRecoveryPlan> Build(const SaveSlotRecoveryRequest &request) const;

    private:
        const ISaveSlotRecoveryValidator *validator_{};
        SaveSlotRecoveryPolicy policy_;
    };
}  // namespace Horo::Runtime
