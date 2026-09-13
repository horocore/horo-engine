#pragma once

/**
 * @file SaveSlotCommitTransaction.h
 * @brief Recoverable atomic publication of one immutable save-slot generation.
 */

#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveStorageAdapter.h"

#include <optional>

namespace Horo::Runtime {
    /** @brief Durable recovery phase for one slot-generation publication. */
    enum class SaveSlotCommitPhase : std::uint8_t {
        Preparing,
        Prepared,
        Publishing,
        Published,
    };

    /** @brief Complete typed journal record persisted before any generation artifact is created. */
    struct SaveSlotCommitJournal final {
        OperationId operation{};                                   /**< Non-zero owning operation identity. */
        SaveStorageAddress address;                                /**< Exact namespace binding and logical slot. */
        std::optional<SaveSlotCatalogEntry> previous;              /**< Previously visible generation, when any. */
        SaveSlotCatalogEntry candidate;                            /**< New immutable generation and its catalog metadata. */
        SaveSlotCommitPhase phase{SaveSlotCommitPhase::Preparing}; /**< Last durably acknowledged phase. */
    };

    /** @brief Durable evidence observed while replaying a commit journal. */
    struct SaveSlotCommitObservation final {
        std::optional<SaveSlotCatalogEntry> published; /**< Generation currently selected by the atomic catalog. */
        bool candidatePrepared{};                      /**< Whether the complete hidden candidate generation exists. */
    };

    /** @brief Successful recovery action, suitable for startup diagnostics. */
    enum class SaveSlotRecoveryAction : std::uint8_t {
        None,
        DiscardedUnpublished,
        PublishedCandidate,
        FinalizedPublished,
    };

    /** @brief Successful commit result after the candidate is known durably visible. */
    struct SaveSlotCommitResult final {
        bool cleanupDeferred{}; /**< True when journal removal failed after durable publication. */
    };

    /**
     * @brief Qualified backend seam for generation-addressed storage and one atomic catalog visibility gate.
     *
     * Implementations map typed values to private physical storage. Prepared generations are immutable and
     * invisible to readers until PublishGeneration atomically and durably selects the candidate metadata.
     * PublishGeneration retains the previous generation and is idempotent when the candidate is already
     * selected. DiscardPrepared and RemoveJournal are idempotent and may remove only the exact operation-owned
     * unpublished candidate or journal respectively.
     */
    class ISaveSlotCommitStore {
    public:
        virtual ~ISaveSlotCommitStore() = default;

        /** @brief Loads the bounded journal for one leased slot. @param address Exact slot address.
         * @return Journal, absence, or a preserved storage failure. */
        [[nodiscard]] virtual Result<std::optional<SaveSlotCommitJournal>> LoadJournal(const SaveStorageAddress &address) = 0;
        /** @brief Atomically writes and durably synchronizes the complete journal. @param journal Record to persist.
         * @return Success or a preserved storage failure. */
        [[nodiscard]] virtual Result<void> StoreJournal(const SaveSlotCommitJournal &journal) = 0;
        /** @brief Writes and validates the hidden immutable candidate. @param journal Preparing journal.
         * @param archive Finalized immutable archive bytes. @return Success only after candidate durability. */
        [[nodiscard]] virtual Result<void> PrepareGeneration(const SaveSlotCommitJournal &journal, const ImmutableSaveArchive &archive) = 0;
        /** @brief Observes current catalog selection and candidate existence. @param journal Valid journal.
         * @return Durable evidence or a preserved storage failure. */
        [[nodiscard]] virtual Result<SaveSlotCommitObservation> Observe(const SaveSlotCommitJournal &journal) = 0;
        /** @brief Atomically selects the complete candidate in the catalog. @param journal Publishing journal.
         * @return Success after catalog durability; failure has unknown publication outcome. */
        [[nodiscard]] virtual Result<void> PublishGeneration(const SaveSlotCommitJournal &journal) = 0;
        /** @brief Removes only an unpublished operation-owned candidate. @param journal Preparing or prepared journal.
         * @return Idempotent success or a preserved storage failure. */
        [[nodiscard]] virtual Result<void> DiscardPrepared(const SaveSlotCommitJournal &journal) = 0;
        /** @brief Removes only the exact operation-owned journal. @param journal Record to remove.
         * @return Idempotent success or a preserved storage failure. */
        [[nodiscard]] virtual Result<void> RemoveJournal(const SaveSlotCommitJournal &journal) = 0;
    };

    /** @brief Coordinates crash-safe slot publication while a caller holds the namespace and per-slot lease. */
    class SaveSlotCommitTransaction final {
    public:
        /** @brief Binds a qualified store that outlives this coordinator. @param store Backend storage authority. */
        explicit SaveSlotCommitTransaction(ISaveSlotCommitStore &store) noexcept;

        /**
         * @brief Publishes one finalized archive and matching metadata as a new durable generation.
         * @param operation Non-zero operation identity.
         * @param address Exact leased namespace and logical slot.
         * @param previous Previously published entry, when overwriting.
         * @param candidate New catalog entry matching address and archive.
         * @param archive Complete finalized immutable archive.
         * @return Durable success, or a typed failure. A publish-stage failure is outcome-unknown and requires Recover.
         */
        [[nodiscard]] Result<SaveSlotCommitResult> Execute(OperationId operation, SaveStorageAddress address,
                                                           std::optional<SaveSlotCatalogEntry> previous, SaveSlotCatalogEntry candidate,
                                                           ImmutableSaveArchive archive);

        /**
         * @brief Replays the durable journal for one leased slot to a stable old-or-new state.
         * @param address Exact namespace and slot whose journal should be recovered.
         * @return Idempotent action, or a typed fail-closed recovery error.
         */
        [[nodiscard]] Result<SaveSlotRecoveryAction> Recover(const SaveStorageAddress &address);

    private:
        ISaveSlotCommitStore *store_{};
    };
}  // namespace Horo::Runtime
