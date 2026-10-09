#include "Horo/Runtime/Save/SaveGameplayCheckpoint.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <utility>

namespace Horo::Runtime {
    namespace {
        /** @brief Checks exact project/world baseline identity without runtime or presentation state. */
        [[nodiscard]] bool ValidBaseline(const GameplayCheckpointBaseline &baseline) noexcept {
            return baseline.project.IsValid() && baseline.world.IsValid() && baseline.baseScene.IsValid() &&
                   baseline.compatibility.IsValid();
        }

        /** @brief Checks stable restart metadata without assigning authority to display text. */
        [[nodiscard]] Result<void> ValidateMetadata(const GameplayCheckpointMetadata &metadata) {
            if (!metadata.checkpoint.IsValid() || !ValidBaseline(metadata.baseline) || !metadata.spawnAnchor.IsValid() ||
                !metadata.restartContext.IsValid() || !metadata.payload.participant.IsValid() || !metadata.payload.record.IsValid() ||
                !metadata.payload.schema.IsValid())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid, "Checkpoint restart metadata is invalid."));
            return Result<void>::Success();
        }

        /** @brief Requires an exact, mandatory canonical project record with capture and restore roles. */
        [[nodiscard]] Result<void> ValidatePayload(const GameplayCheckpointPayloadDescriptor &payload,
                                                   const SaveParticipantRegistrySnapshot &participants) {
            const auto *binding = participants.Find(payload.participant);
            const auto roles = SaveParticipantRole::Capture | SaveParticipantRole::Restore;
            if (!binding || binding->Descriptor().schemaVersion != payload.schema || !binding->Descriptor().required ||
                binding->Descriptor().roles != roles ||
                std::ranges::find(binding->Descriptor().ownedRecords, payload.record) == binding->Descriptor().ownedRecords.end())
                return Result<void>::Failure(MakeError(SaveErrors::RestoreParticipantInvalid,
                                                       "Checkpoint payload must have an exact required capture/restore owner."));
            return Result<void>::Success();
        }

        /** @brief Rejects incompatible baseline and stale transient registry before adapter invocation. */
        [[nodiscard]] Result<void> ValidateCheckpoint(const GameplayCheckpoint &checkpoint, const GameplayCheckpointBaseline &baseline,
                                                      const SaveParticipantRegistrySnapshot &participants) {
            if (checkpoint.Metadata().baseline != baseline)
                return Result<void>::Failure(MakeError(SaveErrors::CommandIncompatible, "Checkpoint world baseline changed."));
            if (const auto *snapshot = checkpoint.Snapshot();
                snapshot && snapshot->Provenance().registryGeneration != participants.Generation())
                return Result<void>::Failure(MakeError(SaveErrors::CommandStale, "Transient checkpoint registry generation changed."));
            return ValidatePayload(checkpoint.Metadata().payload, participants);
        }

        /** @brief Rejects malformed or already spent load authority before any source code runs. */
        [[nodiscard]] Result<void> ValidateOperation(const SaveOperationController &operation, const StagedRestoreContext &context,
                                                     const SaveParticipantRegistrySnapshot &participants) {
            const auto state = operation.Handle().Snapshot();
            if (!state || state->IsTerminal() || state->kind != SaveOperationKind::Load || state->operation != context.operation ||
                context.registryGeneration != participants.Generation() || context.sessionGeneration == 0 ||
                context.sceneIncarnation == 0 || context.maximumParticipants == 0 ||
                context.maximumParticipants > MaximumSaveParticipantCount)
                return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
            return Result<void>::Success();
        }

        /** @brief Adopts detached source receipts, forwards aggregate references and prepares without publication. */
        [[nodiscard]] Result<StagedRestoreTransaction> PrepareStaging(StagedRestoreContext context, SaveOperationController operation,
                                                                      SaveParticipantRegistrySnapshot participants,
                                                                      GameplayCheckpointRestoreStaging staging) {
            auto created =
                StagedRestoreTransaction::Create(context, std::move(operation), std::move(participants), std::move(staging.participants));
            if (created.HasError())
                return created;
            auto transaction = std::move(created).Value();
            if (staging.references) {
                const auto resolver = transaction.SetReferenceResolver(std::move(staging.references));
                if (resolver.HasError()) {
                    transaction.Rollback(resolver.ErrorValue());
                    return Result<StagedRestoreTransaction>::Failure(resolver.ErrorValue());
                }
            }
            if (const auto prepared = transaction.Prepare(); prepared.HasError())
                return Result<StagedRestoreTransaction>::Failure(prepared.ErrorValue());
            return Result<StagedRestoreTransaction>::Success(std::move(transaction));
        }
    }  // namespace

    /** @copydoc GameplayCheckpoint::GameplayCheckpoint */
    GameplayCheckpoint::GameplayCheckpoint(GameplayCheckpointMetadata metadata, RuntimeSaveSnapshot snapshot,
                                           std::optional<SaveSlotPublicationMetadata> publication,
                                           const std::uint64_t sessionGeneration) noexcept
        : metadata_(std::move(metadata)), snapshot_(std::move(snapshot)), sessionGeneration_(sessionGeneration),
          publication_(std::move(publication)) {}

    /** @copydoc GameplayCheckpoint::Capture */
    Result<GameplayCheckpoint> GameplayCheckpoint::Capture(GameplayCheckpointMetadata metadata, RuntimeSaveCaptureProvenance provenance,
                                                           const std::uint64_t sessionGeneration,
                                                           SaveParticipantRegistrySnapshot participants, RuntimeSaveCaptureLimits limits) {
        if (const auto result = ValidateMetadata(metadata); result.HasError())
            return Result<GameplayCheckpoint>::Failure(result.ErrorValue());
        if (const auto result = ValidatePayload(metadata.payload, participants); result.HasError())
            return Result<GameplayCheckpoint>::Failure(result.ErrorValue());
        if (sessionGeneration == 0)
            return Result<GameplayCheckpoint>::Failure(MakeError(SaveErrors::RestoreContextInvalid));
        if (ValidateSaveSlotDisplayMetadata(metadata.display).HasError())
            metadata.display = {};
        auto created = RuntimeSaveCaptureBuilder::Create(provenance, std::move(participants), limits);
        if (created.HasError())
            return Result<GameplayCheckpoint>::Failure(created.ErrorValue());
        auto builder = std::move(created).Value();
        if (const auto result = builder.CaptureParticipants(); result.HasError())
            return Result<GameplayCheckpoint>::Failure(result.ErrorValue());
        auto snapshot = builder.Seal();
        if (snapshot.HasError())
            return Result<GameplayCheckpoint>::Failure(snapshot.ErrorValue());
        const auto records = snapshot.Value().Records();
        if (std::ranges::none_of(records, [&metadata](const auto &record) {
            return record.Record().participant == metadata.payload.participant && record.Record().record == metadata.payload.record;
        }))
            return Result<GameplayCheckpoint>::Failure(
                MakeError(SaveErrors::RestoreParticipantIncomplete, "Capture omitted the declared checkpoint payload record."));
        return Result<GameplayCheckpoint>::Success(
            GameplayCheckpoint{std::move(metadata), std::move(snapshot).Value(), std::nullopt, sessionGeneration});
    }

    /** @copydoc GameplayCheckpoint::OpenDurable */
    Result<GameplayCheckpoint> GameplayCheckpoint::OpenDurable(GameplayCheckpointMetadata metadata,
                                                               SaveSlotPublicationMetadata publication) {
        if (const auto result = ValidateMetadata(metadata); result.HasError())
            return Result<GameplayCheckpoint>::Failure(result.ErrorValue());
        if (const auto result = ValidateSaveSlotPublicationMetadata(publication); result.HasError())
            return Result<GameplayCheckpoint>::Failure(result.ErrorValue());
        if (ValidateSaveSlotDisplayMetadata(metadata.display).HasError())
            metadata.display = {};
        if (publication.kind != SaveSlotKind::Checkpoint || publication.checkpoint != metadata.checkpoint ||
            publication.baseScene != metadata.baseline.baseScene || publication.productCompatibility != metadata.baseline.compatibility)
            return Result<GameplayCheckpoint>::Failure(
                MakeError(SaveErrors::CommandIncompatible, "Durable checkpoint metadata does not match its publication."));
        return Result<GameplayCheckpoint>::Success(GameplayCheckpoint{std::move(metadata), {}, std::move(publication), 0});
    }

    /** @copydoc GameplayCheckpoint::CommitDurable */
    Result<GameplayCheckpoint> GameplayCheckpoint::CommitDurable(SaveSlotPublicationMetadata publication) const {
        if (!snapshot_.IsValid())
            return Result<GameplayCheckpoint>::Failure(
                MakeError(SaveErrors::RestoreTransitionInvalid, "Only a detached capture can be promoted to durable."));
        return OpenDurable(metadata_, std::move(publication));
    }

    /** @copydoc GameplayCheckpoint::Metadata */
    const GameplayCheckpointMetadata &GameplayCheckpoint::Metadata() const noexcept {
        return metadata_;
    }

    /** @copydoc GameplayCheckpoint::Lifetime */
    GameplayCheckpointLifetime GameplayCheckpoint::Lifetime() const noexcept {
        return publication_ ? GameplayCheckpointLifetime::Durable : GameplayCheckpointLifetime::Transient;
    }

    /** @copydoc GameplayCheckpoint::Snapshot */
    const RuntimeSaveSnapshot *GameplayCheckpoint::Snapshot() const noexcept {
        return snapshot_.IsValid() ? &snapshot_ : nullptr;
    }

    /** @copydoc GameplayCheckpoint::Publication */
    const SaveSlotPublicationMetadata *GameplayCheckpoint::Publication() const noexcept {
        return publication_ ? &*publication_ : nullptr;
    }

    /** @copydoc GameplayCheckpointController::GameplayCheckpointController */
    GameplayCheckpointController::GameplayCheckpointController() noexcept : owner_(std::this_thread::get_id()) {}

    /** @copydoc GameplayCheckpointController::Activate */
    Result<void> GameplayCheckpointController::Activate(GameplayCheckpoint checkpoint, const GameplayCheckpointBaseline &baseline,
                                                        const SaveParticipantRegistrySnapshot &participants) {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        if (const auto result = ValidateCheckpoint(checkpoint, baseline, participants); result.HasError())
            return result;
        active_ = std::move(checkpoint);
        return Result<void>::Success();
    }

    /** @copydoc GameplayCheckpointController::ValidateRestart */
    Result<void> GameplayCheckpointController::ValidateRestart(const GameplayCheckpointBaseline &baseline,
                                                               const StagedRestoreContext &context,
                                                               const SaveParticipantRegistrySnapshot &participants) const {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        if (!active_)
            return Result<void>::Failure(MakeError(SaveErrors::RestoreContextInvalid, "No gameplay checkpoint is active."));
        if (const auto result = ValidateCheckpoint(*active_, baseline, participants); result.HasError())
            return Result<void>::Failure(result.ErrorValue());
        if (const auto *snapshot = active_->Snapshot(); snapshot && (snapshot->Provenance().sceneIncarnation != context.sceneIncarnation ||
                                                                     active_->sessionGeneration_ != context.sessionGeneration))
            return Result<void>::Failure(MakeError(SaveErrors::CommandStale, "Transient checkpoint scene incarnation changed."));
        return Result<void>::Success();
    }

    /** @copydoc GameplayCheckpointController::Restart */
    Result<StagedRestoreTransaction> GameplayCheckpointController::Restart(const GameplayCheckpointBaseline &baseline,
                                                                           StagedRestoreContext context, SaveOperationController operation,
                                                                           SaveParticipantRegistrySnapshot participants,
                                                                           IGameplayCheckpointRestoreSource &source) const {
        const auto fail = [&operation](Error error) {
            if (operation.Handle().IsValid()) {
                const auto failed = operation.Fail(error, SaveOperationCommitOutcome::NotCommitted);
                (void)failed;
            }
            return Result<StagedRestoreTransaction>::Failure(std::move(error));
        };
        if (const auto selected = ValidateRestart(baseline, context, participants); selected.HasError())
            return fail(selected.ErrorValue());
        if (const auto validated = ValidateOperation(operation, context, participants); validated.HasError())
            return fail(validated.ErrorValue());
        if (operation.ObserveCancellation() == SaveCancellationObservation::Cancelled)
            return fail(MakeError(SaveErrors::OperationCancelled));
        try {
            // Pin selection across project code that may synchronously change the active retry point.
            const auto checkpoint = *active_;
            auto staged = source.Stage(checkpoint, context, participants);
            if (staged.HasError())
                return fail(staged.ErrorValue());
            return PrepareStaging(context, std::move(operation), std::move(participants), std::move(staged).Value());
        } catch (const std::bad_alloc &) {
            return fail(MakeError(SaveErrors::RestoreAllocationFailed));
        } catch (...) {
            return fail(MakeError(SaveErrors::RestoreAdapterContractInvalid, "Checkpoint staging source threw."));
        }
    }

    /** @copydoc GameplayCheckpointController::Active */
    const GameplayCheckpoint *GameplayCheckpointController::Active() const noexcept {
        return owner_ == std::this_thread::get_id() && active_ ? &*active_ : nullptr;
    }

    /** @copydoc GameplayCheckpointController::Clear */
    Result<void> GameplayCheckpointController::Clear() {
        if (owner_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(SaveErrors::ThreadAffinityViolation));
        active_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime
