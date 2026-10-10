#pragma once

/** @file SaveGameplayCheckpoint.h
 * @brief Immutable gameplay checkpoints and validated death/retry restore transactions.
 */

#include "Horo/Runtime/Save/SaveCaptureSnapshot.h"
#include "Horo/Runtime/Save/SaveRestoreTransaction.h"
#include "Horo/Runtime/Save/SaveSlotMetadata.h"

#include <optional>
#include <thread>

namespace Horo::Runtime {
    /** @brief Persistence lifetime; transient rollback points never imply a committed slot. */
    enum class GameplayCheckpointLifetime : std::uint8_t {
        Transient,
        Durable
    };

    /** @brief Exact project/world baseline compatibility, independent from display text and runtime handles. */
    struct GameplayCheckpointBaseline final {
        SaveProjectId project;
        SaveWorldId world;
        SaveBaseSceneId baseScene;
        ProductSaveCompatibilityVersion compatibility;
        CanonicalStateHash baseline;
        [[nodiscard]] auto operator<=>(const GameplayCheckpointBaseline &) const noexcept = default;
    };

    /** @brief Inert project declaration identifying canonical checkpoint payload ownership. */
    struct GameplayCheckpointPayloadDescriptor final {
        SaveParticipantId participant;
        SaveRecordId record;
        ParticipantSchemaVersion schema;
        [[nodiscard]] auto operator<=>(const GameplayCheckpointPayloadDescriptor &) const noexcept = default;
    };

    /** @brief Stable restart semantics supplied to the project participant before staging.
     * Spawn anchor and context are persistent project record identities, never localized names.
     * The project checkpoint participant must encode these facts in its canonical record for durable recovery.
     */
    struct GameplayCheckpointMetadata final {
        SaveCheckpointId checkpoint;
        GameplayCheckpointBaseline baseline;
        SaveRecordId spawnAnchor;
        SaveRecordId restartContext;
        GameplayCheckpointPayloadDescriptor payload;
        SaveSlotDisplayMetadata display;
    };

    /** @brief Immutable detached checkpoint; copies share immutable canonical payload leases. */
    class GameplayCheckpoint final {
    public:
        /** @brief Captures every registered participant at the caller's exclusive owner safe point.
         * @param metadata Stable restart facts and advisory display copy.
         * @param provenance Exact coherent source evidence.
         * @param sessionGeneration Non-zero owning runtime session generation.
         * @param participants Pinned current participant registry.
         * @param limits Explicit capture allocation bounds.
         * @return Transient checkpoint or typed metadata, descriptor, capture or allocation failure.
         * @pre The host holds the same capture barrier used by ordinary saves.
         */
        [[nodiscard]] static Result<GameplayCheckpoint> Capture(GameplayCheckpointMetadata metadata,
                                                                const RuntimeSaveCaptureProvenance &provenance,
                                                                std::uint64_t sessionGeneration,
                                                                SaveParticipantRegistrySnapshot participants,
                                                                const RuntimeSaveCaptureLimits &limits = {});
        /** @brief Reopens durable metadata obtained from a verified committed checkpoint archive.
         * @param metadata Decoded project checkpoint participant metadata.
         * @param publication Trusted exact committed slot publication.
         * @return Durable checkpoint or typed metadata/publication mismatch.
         * @pre Host verified archive integrity and decoded metadata from its canonical project record.
         */
        [[nodiscard]] static Result<GameplayCheckpoint> OpenDurable(GameplayCheckpointMetadata metadata,
                                                                    SaveSlotPublicationMetadata publication);
        /** @brief Promotes this capture after its exact state was durably committed.
         * @param publication Trusted publication of Snapshot(), supplied by the successful slot commit.
         * @return Durable checkpoint or typed mismatch. No disk write occurs here.
         * @pre Host proves publication was produced from this exact immutable capture.
         */
        [[nodiscard]] Result<GameplayCheckpoint> CommitDurable(SaveSlotPublicationMetadata publication) const;
        /** @brief Returns immutable restart metadata. @return Borrowed checkpoint metadata. */
        [[nodiscard]] const GameplayCheckpointMetadata &Metadata() const noexcept;
        /** @brief Returns the explicit checkpoint lifetime. @return Transient or durable. */
        [[nodiscard]] GameplayCheckpointLifetime Lifetime() const noexcept;
        /** @brief Returns detached canonical state for transient restore/encoding. @return Snapshot or null for durable. */
        [[nodiscard]] const RuntimeSaveSnapshot *Snapshot() const noexcept;
        /** @brief Returns exact committed source for durable restore. @return Publication or null for transient. */
        [[nodiscard]] const SaveSlotPublicationMetadata *Publication() const noexcept;

    private:
        /** @brief Adopts already validated immutable checkpoint ownership.
         * @param metadata Stable checkpoint facts.
         * @param snapshot Detached transient capture, or empty for durable.
         * @param publication Exact durable source, or absent for transient.
         * @param sessionGeneration Transient source session, zero for durable.
         */
        GameplayCheckpoint(GameplayCheckpointMetadata metadata, RuntimeSaveSnapshot snapshot,
                           std::optional<SaveSlotPublicationMetadata> publication, std::uint64_t sessionGeneration) noexcept;
        GameplayCheckpointMetadata metadata_;
        friend class GameplayCheckpointController;
        RuntimeSaveSnapshot snapshot_;
        std::uint64_t sessionGeneration_{};
        std::optional<SaveSlotPublicationMetadata> publication_;
    };

    /** @brief Detached ordinary load staging including the aggregate reference resolver. */
    struct GameplayCheckpointRestoreStaging final {
        std::vector<std::unique_ptr<IStagedRestoreParticipant>> participants;
        std::unique_ptr<IStagedRestoreReferenceResolver> references;
    };

    /** @brief Trusted slot-load composition also used by checkpoint restart.
     * For durable sources, pin the exact namespace/slot generation, verify archive integrity,
     * compatibility and canonical metadata equality with checkpoint.Metadata() before staging.
     * For transient sources, stage only immutable Snapshot() records. No live mutation is allowed.
     * Include the declared project payload owner in the returned receipts; apply spawn/restart
     * semantics in that owner's private candidate, never in a callback after publication.
     */
    class IGameplayCheckpointRestoreSource {
    public:
        virtual ~IGameplayCheckpointRestoreSource() = default;
        /** @brief Produces detached receipts using the ordinary slot-load pipeline.
         * @param checkpoint Exact immutable source and restart context.
         * @param context Current load operation and runtime generations.
         * @param participants Current pinned restore registry.
         * @return All canonical receipts or the original typed archive/decode/staging failure.
         */
        [[nodiscard]] virtual Result<GameplayCheckpointRestoreStaging> Stage(const GameplayCheckpoint &checkpoint,
                                                                             const StagedRestoreContext &context,
                                                                             const SaveParticipantRegistrySnapshot &participants) = 0;
    };

    /** @brief Owner-thread active retry point; selection never restores live state.
     * No global checkpoint registry or implicit disk I/O. The host retains durable metadata in
     * its project participant and retires this owner before shutting down the runtime session.
     */
    class GameplayCheckpointController final {
    public:
        /** @brief Binds active checkpoint selection to the current owner thread. */
        GameplayCheckpointController() noexcept;
        GameplayCheckpointController(const GameplayCheckpointController &) = delete;
        GameplayCheckpointController &operator=(const GameplayCheckpointController &) = delete;
        /** @brief Selects a checkpoint only if its baseline and payload fit the current host.
         * @param checkpoint Immutable capture or verified durable source.
         * @param baseline Current trusted baseline.
         * @param participants Current participant registry.
         * @return Success or typed incompatible/stale/owner failure; failure preserves previous selection.
         */
        [[nodiscard]] Result<void> Activate(GameplayCheckpoint checkpoint, const GameplayCheckpointBaseline &baseline,
                                            const SaveParticipantRegistrySnapshot &participants);
        /** @brief Prepares the selected retry point through the same transaction as ordinary slot load.
         * @param baseline Current trusted baseline revalidated before staging.
         * @param context Load operation and runtime generation evidence, copied before source callbacks.
         * @param operation Sole producer for an admitted nonterminal Load operation.
         * @param participants Current pinned restore registry.
         * @param source Trusted slot-load staging composition.
         * @return ReadyToActivate transaction or typed failure; no live state changes here.
         * The host must call transaction.Activate with fresh evidence under its exclusive commit boundary.
         */
        [[nodiscard]] Result<StagedRestoreTransaction> Restart(const GameplayCheckpointBaseline &baseline,
                                                               const StagedRestoreContext &context, SaveOperationController operation,
                                                               SaveParticipantRegistrySnapshot participants,
                                                               IGameplayCheckpointRestoreSource &source) const;
        /** @brief Returns the selected retry point on the owner thread. @return Checkpoint or null when absent/wrong thread. */
        [[nodiscard]] const GameplayCheckpoint *Active() const noexcept;
        /** @brief Clears selection on the owner thread. @return Success or thread affinity failure. */
        [[nodiscard]] Result<void> Clear();

    private:
        /** @brief Validates selected source ownership and transient lifetime before project code runs. */
        [[nodiscard]] Result<void> ValidateRestart(const GameplayCheckpointBaseline &baseline, const StagedRestoreContext &context,
                                                   const SaveParticipantRegistrySnapshot &participants) const;
        std::thread::id owner_;
        std::optional<GameplayCheckpoint> active_;
    };
}  // namespace Horo::Runtime
