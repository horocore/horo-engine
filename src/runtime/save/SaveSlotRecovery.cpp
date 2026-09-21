#include "Horo/Runtime/Save/SaveSlotRecovery.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <new>
#include <string>
#include <utility>

namespace Horo::Runtime {
    namespace {
        [[nodiscard]] Error Invalid(std::string message) {
            return MakeError(SaveErrors::SlotRecoveryInvalid, std::move(message));
        }

        [[nodiscard]] Error LimitExceeded(std::string message) {
            return MakeError(SaveErrors::SlotRecoveryLimitExceeded, std::move(message));
        }

        [[nodiscard]] bool IsKnown(const SaveSlotRecoveryTrigger value) noexcept {
            return value < SaveSlotRecoveryTrigger::Count;
        }

        [[nodiscard]] bool IsKnown(const SaveSlotRecoveryAutomaticPolicy value) noexcept {
            return value < SaveSlotRecoveryAutomaticPolicy::Count;
        }

        [[nodiscard]] bool IsIncompatibleArchiveError(const Error &error) noexcept {
            const auto &code = error.code.Value();
            return code == SaveErrors::VersionUnsupportedNewer.code.Value() || code == SaveErrors::ArchiveCodecUnsupported.code.Value() ||
                   code == SaveErrors::ArchiveExtensionInvalid.code.Value() ||
                   code == SaveErrors::ArchiveIntegrityAlgorithmUnsupported.code.Value();
        }

        [[nodiscard]] SaveSlotRecoveryValidation Classified(const SaveSlotRecoveryValidationState state, Error diagnostic) {
            return {.state = state, .requiresMigration = false, .compatibility = std::nullopt, .diagnostic = std::move(diagnostic)};
        }

        [[nodiscard]] Result<SaveSlotRecoveryValidation> Corrupt(std::string message) {
            return Result<SaveSlotRecoveryValidation>::Success(
                Classified(SaveSlotRecoveryValidationState::Corrupt, Invalid(std::move(message))));
        }

        [[nodiscard]] Result<SaveSlotRecoveryValidation> ClassifyArchiveError(Error error) {
            return Result<SaveSlotRecoveryValidation>::Success(Classified(IsIncompatibleArchiveError(error)
                                                                              ? SaveSlotRecoveryValidationState::Incompatible
                                                                              : SaveSlotRecoveryValidationState::Corrupt,
                                                                          std::move(error)));
        }

        [[nodiscard]] bool MatchesCatalog(const ValidatedSaveArchive &archive, const SaveSlotCatalogEntry &metadata) noexcept {
            const auto &header = archive.Header();
            const auto &manifest = archive.Manifest();
            const auto &integrity = archive.Integrity();
            return header.slot == metadata.publication.slot && header.slotGeneration == metadata.publication.generation &&
                   header.baseScene == metadata.publication.baseScene &&
                   header.productCompatibility == metadata.publication.productCompatibility &&
                   header.capturedAtUnixMilliseconds == metadata.publication.savedAtUnixMilliseconds &&
                   header.playTimeNanoseconds == metadata.publication.playTimeNanoseconds &&
                   header.projectBuildId == metadata.publication.projectBuildId &&
                   manifest.saveSchemaVersion == metadata.publication.saveSchema &&
                   manifest.canonicalState == metadata.publication.canonicalState &&
                   integrity.archiveContent == metadata.publication.archiveContent;
        }

        [[nodiscard]] bool Newer(const SaveSlotRecoveryArtifact &left, const SaveSlotRecoveryArtifact &right) noexcept {
            return left.retentionSequence > right.retentionSequence;
        }

        [[nodiscard]] Result<void> ValidatePolicy(const SaveSlotRecoveryPolicy &policy) {
            if (policy.maximumBackups == 0 || policy.maximumBackups > MaximumSaveSlotRecoveryBackups || policy.maximumQuarantined == 0 ||
                policy.maximumQuarantined > MaximumSaveSlotRecoveryQuarantined ||
                policy.maximumObservedArtifacts < policy.maximumBackups + policy.maximumQuarantined ||
                policy.maximumObservedArtifacts > MaximumSaveSlotRecoveryObservedArtifacts || !IsKnown(policy.automaticPromotion))
                return Result<void>::Failure(MakeError(SaveErrors::SlotRecoveryInvalid));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequest(const SaveSlotRecoveryRequest &request, const SaveSlotRecoveryPolicy &policy) {
            if (auto valid = ValidatePolicy(policy); valid.HasError())
                return valid;
            if (!request.slot.IsValid() || !IsKnown(request.trigger))
                return Result<void>::Failure(Invalid("Save-slot recovery request has an invalid slot or trigger."));

            const std::size_t currentCount = request.current.has_value() ? 1U : 0U;
            if (request.backups.size() > policy.maximumObservedArtifacts - currentCount)
                return Result<void>::Failure(LimitExceeded("Save-slot recovery observations exceed the configured bound."));
            const std::size_t currentAndBackups = currentCount + request.backups.size();
            if (request.quarantined.size() > policy.maximumObservedArtifacts - currentAndBackups)
                return Result<void>::Failure(LimitExceeded("Save-slot recovery observations exceed the configured bound."));

            try {
                std::vector<std::uint64_t> sequences;
                sequences.reserve(currentCount + request.backups.size() + request.quarantined.size());
                if (request.current) {
                    if (request.current->retentionSequence == 0)
                        return Result<void>::Failure(Invalid("Current recovery evidence has no retention sequence."));
                    sequences.push_back(request.current->retentionSequence);
                }
                for (const auto &artifact : request.backups) {
                    if (artifact.retentionSequence == 0)
                        return Result<void>::Failure(Invalid("A recovery backup has no retention sequence."));
                    sequences.push_back(artifact.retentionSequence);
                }
                for (const auto &artifact : request.quarantined) {
                    if (artifact.retentionSequence == 0)
                        return Result<void>::Failure(Invalid("A quarantined recovery artifact has no retention sequence."));
                    sequences.push_back(artifact.retentionSequence);
                }
                std::ranges::sort(sequences);
                if (std::ranges::adjacent_find(sequences) != sequences.end())
                    return Result<void>::Failure(Invalid("Recovery retention sequences must be unique for deterministic cleanup."));
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(SaveErrors::SlotRecoveryAllocationFailed));
            }

            return Result<void>::Success();
        }

        [[nodiscard]] bool AutomaticAllowed(const SaveSlotRecoveryAutomaticPolicy policy, const SaveSlotRecoveryTrigger trigger) noexcept {
            using enum SaveSlotRecoveryAutomaticPolicy;
            if (policy == CorruptOrInterrupted)
                return trigger == SaveSlotRecoveryTrigger::CorruptCurrent || trigger == SaveSlotRecoveryTrigger::InterruptedPublication;
            return policy == CorruptCurrent && trigger == SaveSlotRecoveryTrigger::CorruptCurrent;
        }

        [[nodiscard]] SaveSlotRecoveryDecisionReason NoPromotionReason(const SaveSlotRecoveryPlan &plan) noexcept {
            if (plan.currentValidation && plan.currentValidation->state == SaveSlotRecoveryValidationState::Valid)
                return SaveSlotRecoveryDecisionReason::CurrentValid;
            return SaveSlotRecoveryDecisionReason::NoValidBackup;
        }

        struct QuarantineEntry final {
            SaveSlotRecoveryArtifact artifact;
            bool protectedEvidence{};
        };

        [[nodiscard]] Result<void> AddQuarantineCandidate(SaveSlotRecoveryPlan &plan, SaveSlotRecoveryCandidate candidate) {
            try {
                plan.quarantine.push_back(std::move(candidate));
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(MakeError(SaveErrors::SlotRecoveryAllocationFailed));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<SaveSlotRecoveryValidation> AdmitValidation(Result<SaveSlotRecoveryValidation> result) {
            if (result.HasError())
                return Result<SaveSlotRecoveryValidation>::Failure(std::move(result).ErrorValue());
            auto validation = std::move(result).Value();
            if (validation.state >= SaveSlotRecoveryValidationState::Count)
                return Result<SaveSlotRecoveryValidation>::Failure(Invalid("Recovery validator returned an unknown validation state."));
            return Result<SaveSlotRecoveryValidation>::Success(std::move(validation));
        }

        [[nodiscard]] Result<void> ValidateCurrent(const SaveSlotRecoveryRequest &request, const ISaveSlotRecoveryValidator &validator,
                                                   SaveSlotRecoveryPlan &plan) {
            if (!request.current) {
                if (request.trigger != SaveSlotRecoveryTrigger::InterruptedPublication)
                    return Result<void>::Failure(
                        Invalid("Corrupt or incompatible recovery requires the original current artifact as evidence."));
                return Result<void>::Success();
            }

            auto currentValidation = AdmitValidation(validator.Validate(*request.current, request.slot));
            if (currentValidation.HasError())
                return Result<void>::Failure(std::move(currentValidation).ErrorValue());
            plan.currentValidation = std::move(currentValidation).Value();
            if (request.trigger == SaveSlotRecoveryTrigger::CorruptCurrent &&
                plan.currentValidation->state != SaveSlotRecoveryValidationState::Corrupt)
                return Result<void>::Failure(Invalid("Recovery trigger says the current archive is corrupt, but validation disagrees."));
            if (request.trigger == SaveSlotRecoveryTrigger::IncompatibleCurrent &&
                plan.currentValidation->state != SaveSlotRecoveryValidationState::Incompatible)
                return Result<void>::Failure(
                    Invalid("Recovery trigger says the current archive is incompatible, but validation disagrees."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<std::vector<std::size_t>> InspectBackups(const SaveSlotRecoveryRequest &request,
                                                                      const ISaveSlotRecoveryValidator &validator,
                                                                      SaveSlotRecoveryPlan &plan) {
            plan.inspectedBackups.reserve(request.backups.size());
            for (const auto &artifact : request.backups) {
                auto validation = AdmitValidation(validator.Validate(artifact, request.slot));
                if (validation.HasError())
                    return Result<std::vector<std::size_t>>::Failure(std::move(validation).ErrorValue());
                plan.inspectedBackups.push_back({.artifact = artifact, .validation = std::move(validation).Value()});
            }
            std::ranges::sort(plan.inspectedBackups, [](const SaveSlotRecoveryCandidate &left, const SaveSlotRecoveryCandidate &right) {
                return Newer(left.artifact, right.artifact);
            });

            std::vector<std::size_t> validIndices;
            validIndices.reserve(plan.inspectedBackups.size());
            for (std::size_t index = 0; index < plan.inspectedBackups.size(); ++index) {
                const auto &candidate = plan.inspectedBackups[index];
                if (candidate.validation.state == SaveSlotRecoveryValidationState::Valid)
                    validIndices.push_back(index);
                else {
                    if (auto added = AddQuarantineCandidate(plan, candidate); added.HasError())
                        return Result<std::vector<std::size_t>>::Failure(std::move(added).ErrorValue());
                }
            }
            return Result<std::vector<std::size_t>>::Success(std::move(validIndices));
        }

        [[nodiscard]] std::vector<QuarantineEntry> MakeQuarantinePool(const SaveSlotRecoveryRequest &request,
                                                                      const SaveSlotRecoveryPlan &plan) {
            std::vector<QuarantineEntry> quarantinePool;
            quarantinePool.reserve(request.quarantined.size() + plan.quarantine.size() + 1U);
            if (plan.currentValidation && plan.currentValidation->state != SaveSlotRecoveryValidationState::Valid)
                quarantinePool.push_back({*request.current, true});
            for (const auto &candidate : plan.quarantine)
                quarantinePool.push_back({candidate.artifact, false});
            for (const auto &artifact : request.quarantined)
                quarantinePool.push_back({artifact, false});
            return quarantinePool;
        }

        void RetainBackups(SaveSlotRecoveryPlan &plan, const std::vector<std::size_t> &validIndices, const SaveSlotRecoveryPolicy &policy) {
            const std::size_t retainedBackupCount = std::min(policy.maximumBackups, validIndices.size());
            plan.retainedBackups.reserve(retainedBackupCount);
            for (std::size_t index = 0; index < validIndices.size(); ++index) {
                const auto &artifact = plan.inspectedBackups[validIndices[index]].artifact;
                if (index < retainedBackupCount)
                    plan.retainedBackups.push_back(artifact);
                else
                    plan.cleanup.push_back({.kind = SaveSlotRecoveryArtifactKind::Backup, .artifact = artifact});
            }
        }

        [[nodiscard]] Result<void> RetainQuarantine(SaveSlotRecoveryPlan &plan, std::vector<QuarantineEntry> quarantinePool,
                                                    const SaveSlotRecoveryPolicy &policy) {
            std::ranges::sort(quarantinePool, [](const QuarantineEntry &left, const QuarantineEntry &right) {
                return Newer(left.artifact, right.artifact);
            });
            std::size_t protectedCount = 0;
            for (const auto &entry : quarantinePool)
                protectedCount += entry.protectedEvidence ? 1U : 0U;
            if (protectedCount > policy.maximumQuarantined)
                return Result<void>::Failure(
                    LimitExceeded("Recovery retention cannot preserve the current evidence within the quarantine bound."));

            plan.retainedQuarantine.reserve(std::min(policy.maximumQuarantined, quarantinePool.size()));
            std::size_t retainedQuarantineCount = 0;
            for (const auto &entry : quarantinePool) {
                if (!entry.protectedEvidence)
                    continue;
                plan.retainedQuarantine.push_back(entry.artifact);
                ++retainedQuarantineCount;
            }
            for (const auto &entry : quarantinePool) {
                if (entry.protectedEvidence || retainedQuarantineCount >= policy.maximumQuarantined) {
                    if (!entry.protectedEvidence)
                        plan.cleanup.push_back({.kind = SaveSlotRecoveryArtifactKind::Quarantine, .artifact = entry.artifact});
                    continue;
                }
                plan.retainedQuarantine.push_back(entry.artifact);
                ++retainedQuarantineCount;
            }
            std::ranges::sort(plan.retainedQuarantine, [](const SaveSlotRecoveryArtifact &left, const SaveSlotRecoveryArtifact &right) {
                return Newer(left, right);
            });
            return Result<void>::Success();
        }

        void SelectDecision(SaveSlotRecoveryPlan &plan, const SaveSlotRecoveryRequest &request, const SaveSlotRecoveryPolicy &policy,
                            const std::vector<std::size_t> &validIndices) {
            const bool currentValid = plan.currentValidation && plan.currentValidation->state == SaveSlotRecoveryValidationState::Valid;
            if (currentValid) {
                plan.cleanup.clear();
                plan.decision = SaveSlotRecoveryDecision::NoRecovery;
                plan.decisionReason = SaveSlotRecoveryDecisionReason::CurrentValid;
                return;
            }
            if (validIndices.empty()) {
                plan.cleanup.clear();
                plan.decision = SaveSlotRecoveryDecision::NoRecovery;
                plan.decisionReason = NoPromotionReason(plan);
                return;
            }

            plan.promotion = plan.inspectedBackups[validIndices.front()];
            if (request.trigger == SaveSlotRecoveryTrigger::IncompatibleCurrent) {
                plan.decision = SaveSlotRecoveryDecision::UserConfirmationRequired;
                plan.decisionReason = SaveSlotRecoveryDecisionReason::IncompatibleCurrentRequiresConfirmation;
            } else if (AutomaticAllowed(policy.automaticPromotion, request.trigger)) {
                plan.decision = SaveSlotRecoveryDecision::AutomaticPromotion;
                plan.decisionReason = SaveSlotRecoveryDecisionReason::AutomaticPolicy;
            } else {
                plan.decision = SaveSlotRecoveryDecision::UserConfirmationRequired;
                plan.decisionReason = request.trigger == SaveSlotRecoveryTrigger::InterruptedPublication
                                          ? SaveSlotRecoveryDecisionReason::InterruptedPublicationRequiresConfirmation
                                          : SaveSlotRecoveryDecisionReason::AutomaticPolicyDisabled;
            }
        }
    }  // namespace

    /** @copydoc SaveArchiveRecoveryValidator::SaveArchiveRecoveryValidator */
    SaveArchiveRecoveryValidator::SaveArchiveRecoveryValidator(SaveArchiveReader reader, SaveCompatibilityPolicy compatibility)
        : reader_(std::move(reader)), compatibility_(std::move(compatibility)) {}

    /** @copydoc ISaveSlotRecoveryValidator::Validate */
    Result<SaveSlotRecoveryValidation> SaveArchiveRecoveryValidator::Validate(const SaveSlotRecoveryArtifact &artifact,
                                                                              const SaveGameSlotId expectedSlot) const {
        if (!expectedSlot.IsValid() || artifact.retentionSequence == 0)
            return Result<SaveSlotRecoveryValidation>::Failure(Invalid("Recovery validation input has an invalid slot or sequence."));
        if (!artifact.metadata)
            return Corrupt("Recovery artifact has no trusted catalog metadata.");
        if (auto metadata = ValidateSaveSlotPublicationMetadata(artifact.metadata->publication); metadata.HasError())
            return Result<SaveSlotRecoveryValidation>::Success(Classified(SaveSlotRecoveryValidationState::Corrupt, metadata.ErrorValue()));
        if (artifact.metadata->publication.slot != expectedSlot)
            return Result<SaveSlotRecoveryValidation>::Success(
                Classified(SaveSlotRecoveryValidationState::Incompatible,
                           Invalid("Recovery artifact addresses a different logical slot.")));
        if (!artifact.archive.bytes || artifact.archive.bytes->empty())
            return Corrupt("Recovery artifact has no complete archive bytes.");

        try {
            auto admitted = reader_.Read(artifact.archive.bytes);
            if (admitted.HasError())
                return ClassifyArchiveError(admitted.ErrorValue());
            if (!MatchesCatalog(admitted.Value(), *artifact.metadata))
                return Result<SaveSlotRecoveryValidation>::Success(
                    Classified(SaveSlotRecoveryValidationState::Corrupt,
                               Invalid("Recovery archive identity or catalog integrity evidence does not match.")));

            const SaveCompatibilityDecision compatibility =
                EvaluateSaveCompatibility(admitted.Value().Preamble().archiveFormatVersion, admitted.Value().Header(),
                                          admitted.Value().Manifest(), compatibility_);
            if (compatibility.disposition == SaveCompatibilityDisposition::Rejected) {
                SaveSlotRecoveryValidation validation{.state = SaveSlotRecoveryValidationState::Incompatible,
                                                      .requiresMigration = false,
                                                      .compatibility = compatibility,
                                                      .diagnostic =
                                                          Invalid("Recovery archive is incompatible with the active save policy.")};
                return Result<SaveSlotRecoveryValidation>::Success(std::move(validation));
            }

            SaveSlotRecoveryValidation validation{.state = SaveSlotRecoveryValidationState::Valid,
                                                  .requiresMigration =
                                                      compatibility.disposition == SaveCompatibilityDisposition::MigrationRequired,
                                                  .compatibility = compatibility,
                                                  .diagnostic = std::nullopt};
            return Result<SaveSlotRecoveryValidation>::Success(std::move(validation));
        } catch (const std::bad_alloc &) {
            return Result<SaveSlotRecoveryValidation>::Failure(MakeError(SaveErrors::SlotRecoveryAllocationFailed));
        }
    }

    /** @copydoc SaveSlotRecoveryPlan::HasPromotion */
    bool SaveSlotRecoveryPlan::HasPromotion() const noexcept {
        return promotion.has_value() &&
               (decision == SaveSlotRecoveryDecision::AutomaticPromotion || decision == SaveSlotRecoveryDecision::UserConfirmationRequired);
    }

    /** @copydoc SaveSlotRecoveryPlan::RequiresUserConfirmation */
    bool SaveSlotRecoveryPlan::RequiresUserConfirmation() const noexcept {
        return decision == SaveSlotRecoveryDecision::UserConfirmationRequired;
    }

    /** @copydoc SaveSlotRecoveryPlanner::SaveSlotRecoveryPlanner */
    SaveSlotRecoveryPlanner::SaveSlotRecoveryPlanner(const ISaveSlotRecoveryValidator &validator, SaveSlotRecoveryPolicy policy) noexcept
        : validator_(&validator), policy_(policy) {}

    /** @copydoc SaveSlotRecoveryPlanner::Build */
    Result<SaveSlotRecoveryPlan> SaveSlotRecoveryPlanner::Build(const SaveSlotRecoveryRequest &request) const {
        if (validator_ == nullptr)
            return Result<SaveSlotRecoveryPlan>::Failure(MakeError(SaveErrors::SlotRecoveryInvalid));
        if (auto valid = ValidateRequest(request, policy_); valid.HasError())
            return Result<SaveSlotRecoveryPlan>::Failure(valid.ErrorValue());

        try {
            SaveSlotRecoveryPlan plan;
            if (auto current = ValidateCurrent(request, *validator_, plan); current.HasError())
                return Result<SaveSlotRecoveryPlan>::Failure(std::move(current).ErrorValue());

            auto validIndicesResult = InspectBackups(request, *validator_, plan);
            if (validIndicesResult.HasError())
                return Result<SaveSlotRecoveryPlan>::Failure(std::move(validIndicesResult).ErrorValue());
            auto validIndices = std::move(validIndicesResult).Value();

            RetainBackups(plan, validIndices, policy_);

            if (auto retained = RetainQuarantine(plan, MakeQuarantinePool(request, plan), policy_); retained.HasError())
                return Result<SaveSlotRecoveryPlan>::Failure(std::move(retained).ErrorValue());

            SelectDecision(plan, request, policy_, validIndices);
            return Result<SaveSlotRecoveryPlan>::Success(std::move(plan));
        } catch (const std::bad_alloc &) {
            return Result<SaveSlotRecoveryPlan>::Failure(MakeError(SaveErrors::SlotRecoveryAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
