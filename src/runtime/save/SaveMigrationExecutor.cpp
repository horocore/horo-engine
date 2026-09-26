#include "SaveMigrationInternal.h"

#include <new>

namespace Horo::Runtime {
    namespace {
        using namespace SaveMigrationDetail;

        [[nodiscard]] Result<void> ChargeWork(std::uint64_t &used, const std::uint64_t amount, const SaveMigrationLimits &limits) {
            if (used > limits.maximumCumulativeWorkBytes || amount > limits.maximumCumulativeWorkBytes - used) {
                Error error = MigrationError(SaveErrors::MigrationLimitExceeded, "Migration cumulative work budget exceeded.");
                error.diagnostics.push_back({DiagnosticCode{"save.migration.limit.cumulative_work"},
                                             DiagnosticSeverity::Error,
                                             "Migration cumulative work budget exceeded.",
                                             {"migration", 0, 0}});
                return Result<void>::Failure(std::move(error));
            }
            used += amount;
            return Result<void>::Success();
        }

        [[nodiscard]] std::uint64_t CandidateBytes(const SaveMigrationState &state) {
            std::uint64_t bytes = state.archiveBytes.size();
            for (const auto &participant : state.participants)
                bytes += participant.payload.size();
            return bytes;
        }

        struct ParticipantBaseline final {
            SaveParticipantId participant;
            ParticipantSchemaVersion schemaVersion;
            bool required{true};
            Sha256Digest payloadDigest;
        };

        struct StepBaseline final {
            ArchiveFormatVersion archiveFormatVersion;
            SaveSchemaVersion saveSchemaVersion;
            ProductSaveCompatibilityVersion productCompatibility;
            Sha256Digest archiveDigest;
            std::vector<ParticipantBaseline> participants;
        };

        [[nodiscard]] Result<StepBaseline> CaptureStepBaseline(const SaveMigrationCandidate &candidate, const SaveMigrationAxis axis) {
            StepBaseline baseline{.archiveFormatVersion = candidate.archiveFormatVersion,
                                  .saveSchemaVersion = candidate.saveSchemaVersion,
                                  .productCompatibility = candidate.productCompatibility,
                                  .archiveDigest = axis == SaveMigrationAxis::ParticipantSchema
                                                       ? ComputeSha256(std::span<const std::byte>{candidate.archiveBytes})
                                                       : Sha256Digest{}};
            baseline.participants.reserve(candidate.participants.size());
            for (const SaveMigrationParticipantState &participant : candidate.participants)
                baseline.participants.push_back(
                    {.participant = participant.participant,
                     .schemaVersion = participant.schemaVersion,
                     .required = participant.required,
                     .payloadDigest = axis == SaveMigrationAxis::ArchiveFormat || axis == SaveMigrationAxis::ParticipantSchema
                                          ? ComputeSha256(std::span<const std::byte>{participant.payload})
                                          : Sha256Digest{}});
            return Result<StepBaseline>::Success(std::move(baseline));
        }

        [[nodiscard]] const ParticipantBaseline *FindBaselineParticipant(const StepBaseline &baseline,
                                                                         const SaveParticipantId &participant) noexcept {
            const auto found = std::ranges::lower_bound(baseline.participants, participant, {}, &ParticipantBaseline::participant);
            return found != baseline.participants.end() && found->participant == participant ? &*found : nullptr;
        }

        [[nodiscard]] bool MatchesBaseline(const ParticipantBaseline &baseline, const SaveMigrationParticipantState &candidate,
                                           const bool includePayload) {
            return baseline.participant == candidate.participant && baseline.schemaVersion == candidate.schemaVersion &&
                   baseline.required == candidate.required &&
                   (!includePayload || baseline.payloadDigest == ComputeSha256(std::span<const std::byte>{candidate.payload}));
        }

        [[nodiscard]] std::string StepFailureContext(const StepView &step) {
            return std::format("Migration step '{}' ({}, {} -> {}) failed.", step.id.value, StepScope(step), step.from, step.to);
        }

        [[nodiscard]] Result<void> InvalidStepOutput(const StepView &step, std::string message) {
            return Result<void>::Failure(
                MigrationError(SaveErrors::MigrationCandidateInvalid, std::format("{} {}", StepFailureContext(step), std::move(message))));
        }

        [[nodiscard]] Result<void> ValidateArchiveStepOutput(const StepBaseline &before, const SaveMigrationCandidate &after,
                                                             const StepView &step) {
            if (after.archiveFormatVersion.Value() != step.to || after.saveSchemaVersion != before.saveSchemaVersion ||
                after.productCompatibility != before.productCompatibility || after.participants.size() != before.participants.size())
                return InvalidStepOutput(step, "changed a version axis or participant composition outside archive-format ownership.");
            for (std::size_t index = 0; index < before.participants.size(); ++index)
                if (!MatchesBaseline(before.participants[index], after.participants[index], true))
                    return InvalidStepOutput(step, "changed participant state outside archive-format ownership.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateSaveSchemaStepOutput(const StepBaseline &before, const SaveMigrationCandidate &after,
                                                                const StepView &step) {
            if (after.saveSchemaVersion.Value() != step.to || after.archiveFormatVersion != before.archiveFormatVersion ||
                after.productCompatibility != before.productCompatibility || after.participants.size() != before.participants.size())
                return InvalidStepOutput(step, "changed a version axis or participant composition outside save-schema ownership.");
            for (std::size_t index = 0; index < before.participants.size(); ++index) {
                if (!MatchesBaseline(before.participants[index], after.participants[index], false))
                    return InvalidStepOutput(step, "changed participant schema metadata outside save-schema ownership.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateUnrelatedParticipants(const StepBaseline &before, const SaveMigrationCandidate &after,
                                                                 const SaveParticipantId &target, const StepView &step) {
            for (const ParticipantBaseline &beforeEntry : before.participants) {
                if (beforeEntry.participant == target)
                    continue;
                const auto *afterEntry = FindStateParticipant(after, beforeEntry.participant);
                if (afterEntry == nullptr || !MatchesBaseline(beforeEntry, *afterEntry, true))
                    return InvalidStepOutput(step, "modified an unrelated participant candidate.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateParticipantStepOutput(const StepBaseline &before, const SaveMigrationCandidate &after,
                                                                 const StepView &step) {
            const auto *beforeParticipant = FindBaselineParticipant(before, *step.participant);
            const auto *afterParticipant = FindStateParticipant(after, *step.participant);
            if (beforeParticipant == nullptr || afterParticipant == nullptr || afterParticipant->schemaVersion.Value() != step.to ||
                afterParticipant->required != beforeParticipant->required || after.archiveFormatVersion != before.archiveFormatVersion ||
                after.saveSchemaVersion != before.saveSchemaVersion || after.productCompatibility != before.productCompatibility ||
                after.participants.size() != before.participants.size() ||
                ComputeSha256(std::span<const std::byte>{after.archiveBytes}) != before.archiveDigest)
                return InvalidStepOutput(step, "changed an unrelated axis, archive payload, or participant composition.");
            return ValidateUnrelatedParticipants(before, after, *step.participant, step);
        }

        [[nodiscard]] Result<void> ValidateStepOutput(const StepBaseline &before, const SaveMigrationCandidate &after,
                                                      const StepView &step) {
            switch (step.axis) {
                case SaveMigrationAxis::ArchiveFormat:
                    return ValidateArchiveStepOutput(before, after, step);
                case SaveMigrationAxis::SaveSchema:
                    return ValidateSaveSchemaStepOutput(before, after, step);
                case SaveMigrationAxis::ParticipantSchema:
                    return ValidateParticipantStepOutput(before, after, step);
                case SaveMigrationAxis::ProductCompatibility:
                case SaveMigrationAxis::Count:
                    break;
            }
            return InvalidStepOutput(step, "used an unsupported migration axis.");
        }

        [[nodiscard]] Result<void> ValidateStepInput(const SaveMigrationCandidate &candidate, const StepView &step) {
            if (step.axis == SaveMigrationAxis::ArchiveFormat && candidate.archiveFormatVersion.Value() != step.from)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationPlanInvalid,
                                   std::format("Migration step '{}' expects archive version {}, current candidate is {}.", step.id.value,
                                               step.from, candidate.archiveFormatVersion.Value())));
            if (step.axis == SaveMigrationAxis::SaveSchema && candidate.saveSchemaVersion.Value() != step.from)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationPlanInvalid,
                                   std::format("Migration step '{}' expects save schema {}, current candidate is {}.", step.id.value,
                                               step.from, candidate.saveSchemaVersion.Value())));
            if (step.axis == SaveMigrationAxis::ParticipantSchema) {
                const auto *participant = FindStateParticipant(candidate, *step.participant);
                if (participant == nullptr || participant->schemaVersion.Value() != step.from)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationPlanInvalid,
                                       std::format("Migration step '{}' expects participant '{}' schema {}, but the candidate differs.",
                                                   step.id.value, step.participant->Value(), step.from)));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<SaveMigrationCandidate> InvokeMigration(SaveMigrationCandidate candidate, const StepView &step,
                                                                     const SaveMigrationLimits &limits, const std::uint64_t workUsed) {
            const SaveMigrationStepContext context{.id = step.id,
                                                   .axis = step.axis,
                                                   .kind = step.kind,
                                                   .participant = step.participant,
                                                   .remainingWorkBytes = limits.maximumCumulativeWorkBytes - workUsed,
                                                   .maximumArchiveBytes = limits.maximumArchiveBytes,
                                                   .maximumParticipantPayloadBytes = limits.maximumParticipantPayloadBytes,
                                                   .maximumTotalPayloadBytes = limits.maximumTotalPayloadBytes};
            try {
                return (*step.migrate)(std::move(candidate), context);
            } catch (const std::bad_alloc &) {
                return Result<SaveMigrationCandidate>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
            } catch (...) {
                return Result<SaveMigrationCandidate>::Failure(
                    MigrationError(SaveErrors::MigrationStepFailed, StepFailureContext(step) + " Callback threw an exception."));
            }
        }

        [[nodiscard]] Result<SaveMigrationCandidate> ExecuteStep(SaveMigrationCandidate candidate,
                                                                 const SaveMigrationDefinition &definition,
                                                                 const SaveMigrationLimits &limits, std::uint64_t &workUsed) {
            const StepView step = View(definition);
            if (const auto definitionValidation = ValidateDefinition(definition, limits); definitionValidation.HasError())
                return Result<SaveMigrationCandidate>::Failure(
                    MigrationError(SaveErrors::MigrationPlanInvalid,
                                   std::format("Plan contains invalid migration definition '{}'.", step.id.value)));
            if (const auto inputValidation = ValidateStepInput(candidate, step); inputValidation.HasError())
                return Result<SaveMigrationCandidate>::Failure(inputValidation.ErrorValue());
            if (auto charged = ChargeWork(workUsed, CandidateBytes(candidate), limits); charged.HasError())
                return Result<SaveMigrationCandidate>::Failure(charged.ErrorValue());
            if (auto charged = ChargeWork(workUsed, step.estimatedWork, limits); charged.HasError())
                return Result<SaveMigrationCandidate>::Failure(charged.ErrorValue());
            const auto baseline = CaptureStepBaseline(candidate, step.axis);
            if (baseline.HasError())
                return Result<SaveMigrationCandidate>::Failure(baseline.ErrorValue());
            auto transformed = InvokeMigration(std::move(candidate), step, limits, workUsed);
            if (transformed.HasError()) {
                Error wrapped = MigrationError(SaveErrors::MigrationStepFailed, StepFailureContext(step));
                return Result<SaveMigrationCandidate>::Failure(WithCause(std::move(wrapped), transformed.ErrorValue()));
            }
            SaveMigrationCandidate output = std::move(transformed).Value();
            if (const auto bounds = ValidateState(output, limits); bounds.HasError())
                return Result<SaveMigrationCandidate>::Failure(bounds.ErrorValue());
            if (auto charged = ChargeWork(workUsed, CandidateBytes(output), limits); charged.HasError())
                return Result<SaveMigrationCandidate>::Failure(charged.ErrorValue());
            if (const auto outputValidation = ValidateStepOutput(baseline.Value(), output, step); outputValidation.HasError())
                return Result<SaveMigrationCandidate>::Failure(outputValidation.ErrorValue());
            return Result<SaveMigrationCandidate>::Success(std::move(output));
        }

        void ApplyTargetComposition(SaveMigrationCandidate &candidate, const SaveMigrationPlan &plan) {
            if (candidate.productCompatibility != plan.targetProductCompatibility)
                candidate.productCompatibility = plan.targetProductCompatibility;
            for (SaveMigrationParticipantState &participant : candidate.participants) {
                const auto target = std::ranges::find_if(plan.participantTargets, [&participant](const auto &candidateTarget) {
                    return candidateTarget.participant == participant.participant;
                });
                if (target != plan.participantTargets.end())
                    participant.required = target->required;
            }
        }

        [[nodiscard]] Result<void> ValidateFinalCandidate(const SaveMigrationCandidate &candidate, const SaveMigrationPlan &plan,
                                                          const SaveMigrationLimits &limits) {
            if (candidate.archiveFormatVersion != plan.targetArchiveFormat || candidate.saveSchemaVersion != plan.targetSaveSchema)
                return Result<void>::Failure(MigrationError(SaveErrors::MigrationCandidateInvalid,
                                                            "Migrated candidate did not reach every planned root target version."));
            if (const auto targetValidation = ValidateTargetParticipants(candidate, plan); targetValidation.HasError())
                return targetValidation;
            return ValidateState(candidate, limits);
        }

        [[nodiscard]] Result<void> ValidatePlanBinding(const SaveMigrationSource &source, const SaveMigrationPlan &plan,
                                                       const SaveMigrationLimits &limits) {
            if (!ValidLimits(limits) || plan.registryGeneration == 0 || plan.definitions.size() > limits.maximumPlanSteps ||
                plan.sourceArchiveFormat != source.archiveFormatVersion || plan.sourceSaveSchema != source.saveSchemaVersion ||
                plan.sourceProductCompatibility != source.productCompatibility || plan.routeIdentity != RouteIdentity(plan))
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationPlanInvalid, "Migration plan does not bind to its supplied source or limits."));
            return ValidateState(source, limits);
        }
    }  // namespace

    Result<SaveMigrationCandidate> SaveMigrationExecutor::Migrate(const SaveMigrationSource &source, const SaveMigrationPlan &plan,
                                                                  const SaveMigrationLimits &limits) {
        try {
            if (const auto binding = ValidatePlanBinding(source, plan, limits); binding.HasError())
                return Result<SaveMigrationCandidate>::Failure(binding.ErrorValue());
            std::uint64_t workUsed = 0;
            if (auto charged = ChargeWork(workUsed, CandidateBytes(source), limits); charged.HasError())
                return Result<SaveMigrationCandidate>::Failure(charged.ErrorValue());
            SaveMigrationCandidate current = source;
            for (const SaveMigrationDefinition &definition : plan.definitions) {
                auto transformed = ExecuteStep(std::move(current), definition, limits, workUsed);
                if (transformed.HasError())
                    return Result<SaveMigrationCandidate>::Failure(transformed.ErrorValue());
                current = std::move(transformed).Value();
            }
            ApplyTargetComposition(current, plan);
            if (const auto finalValidation = ValidateFinalCandidate(current, plan, limits); finalValidation.HasError())
                return Result<SaveMigrationCandidate>::Failure(finalValidation.ErrorValue());
            return Result<SaveMigrationCandidate>::Success(std::move(current));
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationCandidate>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
