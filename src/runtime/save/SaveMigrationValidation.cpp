#include "SaveMigrationInternal.h"

#include <limits>
#include <new>
#include <set>
#include <unordered_set>

namespace Horo::Runtime::SaveMigrationDetail {
    namespace {
        [[nodiscard]] Result<void> ValidateCheckpointMetadata(const StepView &step, const SaveMigrationLimits &limits) {
            if (step.kind == SaveMigrationStepKind::Sequential && !step.equivalentSequentialSteps->empty())
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationDefinitionInvalid,
                                   "Sequential migration definitions cannot carry checkpoint equivalence metadata."));
            if (step.kind != SaveMigrationStepKind::Checkpoint)
                return Result<void>::Success();
            if (step.equivalentSequentialSteps->empty() || step.equivalentSequentialSteps->size() > limits.maximumPlanSteps)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointInvalid,
                                   std::format("Checkpoint '{}' must declare a bounded non-empty sequential equivalence route.",
                                               step.id.value)));
            std::unordered_set<std::string> identities;
            identities.reserve(step.equivalentSequentialSteps->size());
            for (const SaveMigrationId &identity : *step.equivalentSequentialSteps) {
                if (!identity.IsValid() || !identities.emplace(identity.value).second)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationCheckpointInvalid,
                                       std::format("Checkpoint '{}' contains an invalid or duplicate equivalent step identity.",
                                                   step.id.value)));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateParticipantPolicyEntry(const SaveParticipantCompatibility &entry, const std::size_t index,
                                                                  const SaveCompatibilityPolicy &policy) {
            if (!entry.participant.IsValid() || !IsValidSupport(entry.versions) ||
                (index != 0 && policy.participants[index - 1].participant == entry.participant))
                return Result<void>::Failure(MigrationError(SaveErrors::MigrationDefinitionInvalid,
                                                            "Save migration participant policy contains an invalid or duplicate entry."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCheckpointDeclaration(const SaveMigrationCheckpointDeclaration &checkpoint,
                                                                 std::set<std::string> &checkpointKeys, const SaveMigrationLimits &limits) {
            const bool participantAxis = checkpoint.axis == SaveMigrationAxis::ParticipantSchema;
            if (checkpoint.axis >= SaveMigrationAxis::Count || checkpoint.axis == SaveMigrationAxis::ProductCompatibility ||
                !checkpoint.id.IsValid() || (checkpoint.participant && !checkpoint.participant->IsValid()) ||
                participantAxis != checkpoint.participant.has_value() || checkpoint.equivalentSequentialSteps.empty() ||
                checkpoint.equivalentSequentialSteps.size() > limits.maximumPlanSteps)
                return Result<void>::Failure(MigrationError(SaveErrors::MigrationCheckpointInvalid,
                                                            "Save migration checkpoint declaration is incomplete or out of bounds."));

            const std::string key =
                std::format("{}|{}|{}", static_cast<unsigned>(checkpoint.axis),
                            checkpoint.participant ? checkpoint.participant->Value() : std::string_view{}, checkpoint.id.value);
            if (!checkpointKeys.emplace(key).second)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointInvalid, "Save migration checkpoint declarations must be unique."));

            std::unordered_set<std::string> routeIds;
            routeIds.reserve(checkpoint.equivalentSequentialSteps.size());
            for (const SaveMigrationId &identity : checkpoint.equivalentSequentialSteps) {
                if (!identity.IsValid() || !routeIds.emplace(identity.value).second)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationCheckpointInvalid,
                                       "Save migration checkpoint equivalence contains an invalid or duplicate step."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateParticipantState(const SaveMigrationParticipantState &participant, const std::size_t index,
                                                            const SaveMigrationState &state, const SaveMigrationLimits &limits,
                                                            std::uint64_t &totalPayloadBytes) {
            if (!participant.participant.IsValid() || !participant.schemaVersion.IsValid() ||
                (index != 0 && state.participants[index - 1].participant == participant.participant) ||
                participant.payload.size() > limits.maximumParticipantPayloadBytes ||
                participant.payload.size() > std::numeric_limits<std::uint64_t>::max() - totalPayloadBytes)
                return Result<void>::Failure(MigrationError(SaveErrors::MigrationCandidateInvalid,
                                                            "Detached save migration state contains an invalid or duplicate participant."));
            totalPayloadBytes += participant.payload.size();
            if (totalPayloadBytes > limits.maximumTotalPayloadBytes)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationLimitExceeded,
                                   "Detached save migration state exceeds its aggregate participant payload limit."));
            return Result<void>::Success();
        }
    }  // namespace

    Result<void> ValidateDefinition(const SaveMigrationDefinition &definition, const SaveMigrationLimits &limits) {
        const StepView step = View(definition);
        if (step.axis >= SaveMigrationAxis::Count || step.kind >= SaveMigrationStepKind::Count || !step.id.IsValid() || !step.migrate ||
            step.estimatedWork == 0 || step.from == 0 || step.to == 0)
            return Result<void>::Failure(MigrationError(SaveErrors::MigrationDefinitionInvalid,
                                                        "Migration definition has an invalid identity, range, callback, or kind."));
        if (step.from >= step.to)
            return Result<void>::Failure(
                MigrationError(SaveErrors::MigrationBackwardEdge,
                               std::format("Migration '{}' on {} must advance from {} to a newer version, got {} -> {}.", step.id.value,
                                           StepScope(step), step.from, step.from, step.to)));
        if (step.participant && !step.participant->IsValid())
            return Result<void>::Failure(MigrationError(SaveErrors::MigrationDefinitionInvalid,
                                                        "Participant migration definition has an invalid participant identity."));
        return ValidateCheckpointMetadata(step, limits);
    }

    Result<void> ValidateCatalog(std::vector<SaveMigrationDefinition> &definitions, const SaveMigrationLimits &limits) {
        if (!ValidLimits(limits) || definitions.size() > limits.maximumDefinitions)
            return Result<void>::Failure(MigrationError(SaveErrors::MigrationLimitExceeded,
                                                        "Save migration catalog exceeds its configured definition or state limits."));
        std::unordered_set<std::string> identities;
        std::unordered_set<std::string> edges;
        identities.reserve(definitions.size());
        edges.reserve(definitions.size());
        for (const SaveMigrationDefinition &definition : definitions) {
            if (const auto validation = ValidateDefinition(definition, limits); validation.HasError())
                return validation;
            const StepView step = View(definition);
            if (!identities.emplace(step.id.value).second)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationDuplicateIdentity, "Duplicate save migration identity: " + step.id.value));
            if (!edges.emplace(EdgeKey(step)).second)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationDuplicateEdge, "Duplicate save migration edge: " + EdgeKey(step)));
        }
        std::ranges::sort(definitions, StepOrder);
        return Result<void>::Success();
    }

    Result<void> ValidateSupport(const SaveMigrationSupportDescriptor &support, const SaveMigrationLimits &limits) {
        if (!ValidLimits(limits) || !IsValidSupport(support.compatibility.archiveVersions) ||
            !IsValidSupport(support.compatibility.saveSchemaVersions) || !IsValidSupport(support.compatibility.productVersions) ||
            !ValidParticipantPolicy(support.compatibility) || support.compatibility.participants.size() > limits.maximumParticipants ||
            support.checkpoints.size() > limits.maximumDefinitions)
            return Result<void>::Failure(
                MigrationError(SaveErrors::MigrationDefinitionInvalid, "Save migration support ranges or participant policy are invalid."));
        for (std::size_t index = 0; index < support.compatibility.participants.size(); ++index) {
            if (const auto validation =
                    ValidateParticipantPolicyEntry(support.compatibility.participants[index], index, support.compatibility);
                validation.HasError())
                return validation;
        }
        std::set<std::string> checkpointKeys;
        for (const SaveMigrationCheckpointDeclaration &checkpoint : support.checkpoints) {
            if (const auto validation = ValidateCheckpointDeclaration(checkpoint, checkpointKeys, limits); validation.HasError())
                return validation;
        }
        return Result<void>::Success();
    }

    Result<void> ValidateState(const SaveMigrationState &state, const SaveMigrationLimits &limits) {
        if (!ValidLimits(limits) || !state.archiveFormatVersion.IsValid() || !state.saveSchemaVersion.IsValid() ||
            !state.productCompatibility.IsValid() || state.archiveBytes.size() > limits.maximumArchiveBytes ||
            state.participants.size() > limits.maximumParticipants ||
            !std::ranges::is_sorted(state.participants, {}, &SaveMigrationParticipantState::participant))
            return Result<void>::Failure(
                MigrationError(SaveErrors::MigrationCandidateInvalid,
                               "Detached save migration state has invalid versions, bounds, or participant order."));

        std::uint64_t totalPayloadBytes = 0;
        for (std::size_t index = 0; index < state.participants.size(); ++index) {
            if (const auto validation = ValidateParticipantState(state.participants[index], index, state, limits, totalPayloadBytes);
                validation.HasError())
                return validation;
        }
        return Result<void>::Success();
    }

    Result<void> ValidateTargetParticipants(const SaveMigrationCandidate &candidate, const SaveMigrationPlan &plan) {
        if (candidate.participants.size() != plan.participantTargets.size())
            return Result<void>::Failure(
                MigrationError(SaveErrors::MigrationPlanInvalid, "Migration plan participant targets do not cover the candidate."));
        if (!std::ranges::is_sorted(plan.participantTargets, {}, &SaveMigrationParticipantTarget::participant))
            return Result<void>::Failure(
                MigrationError(SaveErrors::MigrationPlanInvalid, "Migration plan participant targets are not in canonical order."));
        for (std::size_t index = 0; index < plan.participantTargets.size(); ++index) {
            const auto &target = plan.participantTargets[index];
            if (!target.participant.IsValid() || !target.schemaVersion.IsValid() ||
                (index != 0 && plan.participantTargets[index - 1].participant == target.participant))
                return Result<void>::Failure(MigrationError(SaveErrors::MigrationPlanInvalid,
                                                            "Migration plan contains an invalid or duplicate participant target."));
            const auto *actual = FindStateParticipant(candidate, target.participant);
            if (actual == nullptr || actual->schemaVersion != target.schemaVersion || actual->required != target.required)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationCandidateInvalid,
                                   std::format("Migrated participant '{}' does not match its planned target schema or required policy.",
                                               target.participant.Value())));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Runtime::SaveMigrationDetail

namespace Horo::Runtime {
    Result<void> ValidateSaveMigrationState(const SaveMigrationState &state, const SaveMigrationLimits &limits) {
        try {
            return SaveMigrationDetail::ValidateState(state, limits);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(SaveMigrationDetail::MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
