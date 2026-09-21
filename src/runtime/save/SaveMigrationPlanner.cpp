#include "SaveMigrationInternal.h"

#include <algorithm>
#include <new>
#include <unordered_set>

namespace Horo::Runtime::SaveMigrationDetail {
    namespace {
        using Route = std::vector<const SaveMigrationDefinition *>;

        [[nodiscard]] bool DeclarationMatches(const SaveMigrationCheckpointDeclaration &declaration, const StepView &step) noexcept {
            return declaration.id == step.id && declaration.axis == step.axis && SameParticipant(declaration.participant, step.participant);
        }

        [[nodiscard]] Result<void> ValidateTargetCheckpointDeclarations(const std::vector<SaveMigrationDefinition> &definitions,
                                                                        const SaveMigrationSupportDescriptor &support,
                                                                        const SaveMigrationAxis axis,
                                                                        const std::optional<SaveParticipantId> &participant,
                                                                        const std::uint32_t target) {
            for (const SaveMigrationDefinition &definition : definitions) {
                const StepView step = View(definition);
                if (step.kind != SaveMigrationStepKind::Checkpoint || !MatchesScope(step, axis, participant) || step.to != target)
                    continue;
                if (!std::ranges::any_of(support.checkpoints, [&step](const auto &declaration) {
                    return DeclarationMatches(declaration, step);
                }))
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationCheckpointInvalid, std::format("Checkpoint '{}' reaches {} version {} but is "
                                                                                           "not declared by the target support descriptor.",
                                                                                           step.id.value, StepScope(step), target)));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Route SequentialCandidates(const std::vector<SaveMigrationDefinition> &definitions, const SaveMigrationAxis axis,
                                                 const std::optional<SaveParticipantId> &participant, const std::uint32_t current,
                                                 const std::uint32_t target) {
            Route candidates;
            for (const SaveMigrationDefinition &definition : definitions) {
                const StepView step = View(definition);
                if (step.kind == SaveMigrationStepKind::Sequential && MatchesScope(step, axis, participant) && step.from == current &&
                    step.to <= target)
                    candidates.push_back(&definition);
            }
            return candidates;
        }

        [[nodiscard]] Result<Route> BuildSequentialRoute(const std::vector<SaveMigrationDefinition> &definitions,
                                                         const SaveMigrationAxis axis, const std::optional<SaveParticipantId> &participant,
                                                         const std::uint32_t source, const std::uint32_t target,
                                                         const SaveMigrationLimits &limits) {
            Route route;
            std::unordered_set<std::uint32_t> visited;
            visited.reserve(limits.maximumPlanSteps);
            std::uint32_t current = source;
            while (current != target) {
                if (!visited.emplace(current).second)
                    return Result<Route>::Failure(
                        MigrationError(SaveErrors::MigrationCycle,
                                       std::format("{} migration route revisits version {}.", AxisName(axis), current)));
                const Route candidates = SequentialCandidates(definitions, axis, participant, current, target);
                if (candidates.empty())
                    return Result<Route>::Failure(MigrationError(SaveErrors::MigrationPathMissing,
                                                                 std::format("{} migration route has a gap at version {} before target {}.",
                                                                             AxisName(axis), current, target)));
                if (candidates.size() != 1) {
                    std::string identities;
                    for (const SaveMigrationDefinition *candidate : candidates) {
                        if (!identities.empty())
                            identities += ", ";
                        identities += View(*candidate).id.value;
                    }
                    return Result<Route>::Failure(MigrationError(SaveErrors::MigrationAmbiguous,
                                                                 std::format("{} migration route has multiple next hops at version {}: {}.",
                                                                             AxisName(axis), current, identities)));
                }
                route.push_back(candidates.front());
                current = View(*candidates.front()).to;
                if (route.size() > limits.maximumPlanSteps)
                    return Result<Route>::Failure(
                        MigrationError(SaveErrors::MigrationLimitExceeded, "Save migration route exceeds its maximum step count."));
            }
            return Result<Route>::Success(std::move(route));
        }

        [[nodiscard]] std::vector<const SaveMigrationCheckpointDeclaration *> MatchingDeclarations(
            const SaveMigrationSupportDescriptor &support, const SaveMigrationAxis axis,
            const std::optional<SaveParticipantId> &participant) {
            std::vector<const SaveMigrationCheckpointDeclaration *> matching;
            for (const SaveMigrationCheckpointDeclaration &declaration : support.checkpoints)
                if (declaration.axis == axis && SameParticipant(declaration.participant, participant))
                    matching.push_back(&declaration);
            return matching;
        }

        [[nodiscard]] Result<const SaveMigrationDefinition *> FindCheckpointDefinition(
            const std::vector<SaveMigrationDefinition> &definitions, const SaveMigrationCheckpointDeclaration &declaration,
            const SaveMigrationAxis axis, const std::optional<SaveParticipantId> &participant, const std::uint32_t source,
            const std::uint32_t target) {
            const SaveMigrationDefinition *found = nullptr;
            for (const SaveMigrationDefinition &definition : definitions) {
                const StepView step = View(definition);
                if (step.kind == SaveMigrationStepKind::Checkpoint && step.id == declaration.id && MatchesScope(step, axis, participant) &&
                    step.from == source && step.to == target) {
                    if (found != nullptr)
                        return Result<const SaveMigrationDefinition *>::Failure(
                            MigrationError(SaveErrors::MigrationCheckpointInvalid,
                                           std::format("Declared checkpoint '{}' identifies more than one {} edge from {} to {}.",
                                                       declaration.id.value, AxisName(axis), source, target)));
                    found = &definition;
                }
            }
            if (found == nullptr)
                return Result<const SaveMigrationDefinition *>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointInvalid,
                                   std::format("Declared checkpoint '{}' does not identify exactly one {} edge from {} to {}.",
                                               declaration.id.value, AxisName(axis), source, target)));
            return Result<const SaveMigrationDefinition *>::Success(found);
        }

        [[nodiscard]] Result<const SaveMigrationDefinition *> FindUniqueStepById(const std::vector<SaveMigrationDefinition> &definitions,
                                                                                 const SaveMigrationId &identity,
                                                                                 const SaveMigrationId &checkpointId) {
            const SaveMigrationDefinition *found = nullptr;
            for (const SaveMigrationDefinition &definition : definitions) {
                if (View(definition).id != identity)
                    continue;
                if (found != nullptr)
                    return Result<const SaveMigrationDefinition *>::Failure(
                        MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                       std::format("Checkpoint '{}' references sequential step '{}' which is not unique in the catalog.",
                                                   checkpointId.value, identity.value)));
                found = &definition;
            }
            if (found == nullptr)
                return Result<const SaveMigrationDefinition *>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                   std::format("Checkpoint '{}' references missing sequential step '{}'.", checkpointId.value,
                                               identity.value)));
            return Result<const SaveMigrationDefinition *>::Success(found);
        }

        [[nodiscard]] Result<Route> ValidateCheckpointEquivalence(const std::vector<SaveMigrationDefinition> &definitions,
                                                                  const SaveMigrationCheckpointDeclaration &declaration,
                                                                  const StepView &checkpoint, const SaveMigrationAxis axis,
                                                                  const std::optional<SaveParticipantId> &participant,
                                                                  const std::uint32_t source, const std::uint32_t target,
                                                                  const SaveMigrationLimits &limits) {
            if (*checkpoint.equivalentSequentialSteps != declaration.equivalentSequentialSteps)
                return Result<Route>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                   std::format("Checkpoint '{}' and its release declaration carry different sequential equivalence routes.",
                                               checkpoint.id.value)));

            Route route;
            std::uint32_t current = source;
            for (const SaveMigrationId &expected : declaration.equivalentSequentialSteps) {
                const auto stepResult = FindUniqueStepById(definitions, expected, checkpoint.id);
                if (stepResult.HasError())
                    return Result<Route>::Failure(stepResult.ErrorValue());
                const SaveMigrationDefinition *definition = stepResult.Value();
                const StepView sequential = View(*definition);
                if (sequential.kind != SaveMigrationStepKind::Sequential || !MatchesScope(sequential, axis, participant) ||
                    sequential.from != current || sequential.to > target)
                    return Result<Route>::Failure(
                        MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                       std::format("Checkpoint '{}' equivalence step '{}' does not continue from version {}.",
                                                   checkpoint.id.value, expected.value, current)));
                if (SequentialCandidates(definitions, axis, participant, current, target).size() != 1)
                    return Result<Route>::Failure(
                        MigrationError(SaveErrors::MigrationAmbiguous,
                                       std::format("Checkpoint '{}' cannot prove a deterministic sequential route at version {}.",
                                                   checkpoint.id.value, current)));
                route.push_back(definition);
                current = sequential.to;
                if (route.size() > limits.maximumPlanSteps)
                    return Result<Route>::Failure(
                        MigrationError(SaveErrors::MigrationLimitExceeded, "Checkpoint equivalence route exceeds its maximum step count."));
            }
            if (current != target)
                return Result<Route>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                   std::format("Checkpoint '{}' equivalence route ends at {} instead of target {}.", checkpoint.id.value,
                                               current, target)));
            return Result<Route>::Success(std::move(route));
        }

        [[nodiscard]] Result<Route> BuildDeclaredCheckpointRoute(const std::vector<SaveMigrationDefinition> &definitions,
                                                                 const SaveMigrationCheckpointDeclaration &declaration,
                                                                 const SaveMigrationAxis axis,
                                                                 const std::optional<SaveParticipantId> &participant,
                                                                 const std::uint32_t source, const std::uint32_t target,
                                                                 const SaveMigrationLimits &limits) {
            const auto checkpointResult = FindCheckpointDefinition(definitions, declaration, axis, participant, source, target);
            if (checkpointResult.HasError())
                return Result<Route>::Failure(checkpointResult.ErrorValue());
            const StepView checkpoint = View(*checkpointResult.Value());
            const auto equivalence =
                ValidateCheckpointEquivalence(definitions, declaration, checkpoint, axis, participant, source, target, limits);
            if (equivalence.HasError())
                return Result<Route>::Failure(equivalence.ErrorValue());
            return Result<Route>::Success({checkpointResult.Value()});
        }

        [[nodiscard]] Result<void> AppendRootRoute(SaveMigrationPlan &plan, const std::vector<SaveMigrationDefinition> &definitions,
                                                   const SaveMigrationSupportDescriptor &support, const SaveMigrationAxis axis,
                                                   const std::optional<SaveParticipantId> &participant, const std::uint32_t source,
                                                   const std::uint32_t target, const SaveMigrationLimits &limits) {
            if (source == target)
                return Result<void>::Success();
            const auto route = BuildCheckpointRoute(definitions, support, axis, participant, source, target, limits);
            if (route.HasError())
                return Result<void>::Failure(route.ErrorValue());
            return AppendRoute(plan, route.Value(), limits);
        }

        [[nodiscard]] Result<void> PlanArchive(const SaveMigrationSource &source, const SaveMigrationSupportDescriptor &support,
                                               const std::vector<SaveMigrationDefinition> &definitions, SaveMigrationPlan &plan,
                                               const SaveMigrationLimits &limits) {
            const auto admission =
                ClassifyVersion(source.archiveFormatVersion, support.compatibility.archiveVersions, SaveMigrationAxis::ArchiveFormat);
            if (admission.HasError())
                return Result<void>::Failure(admission.ErrorValue());
            if (admission.Value() != VersionAdmission::Migration)
                return Result<void>::Success();
            plan.targetArchiveFormat = support.compatibility.archiveVersions.direct.maximum;
            return AppendRootRoute(plan, definitions, support, SaveMigrationAxis::ArchiveFormat, std::nullopt,
                                   source.archiveFormatVersion.Value(), plan.targetArchiveFormat.Value(), limits);
        }

        [[nodiscard]] Result<void> PlanSaveSchema(const SaveMigrationSource &source, const SaveMigrationSupportDescriptor &support,
                                                  const std::vector<SaveMigrationDefinition> &definitions, SaveMigrationPlan &plan,
                                                  const SaveMigrationLimits &limits) {
            const auto admission =
                ClassifyVersion(source.saveSchemaVersion, support.compatibility.saveSchemaVersions, SaveMigrationAxis::SaveSchema);
            if (admission.HasError())
                return Result<void>::Failure(admission.ErrorValue());
            if (admission.Value() != VersionAdmission::Migration)
                return Result<void>::Success();
            plan.targetSaveSchema = support.compatibility.saveSchemaVersions.direct.maximum;
            return AppendRootRoute(plan, definitions, support, SaveMigrationAxis::SaveSchema, std::nullopt,
                                   source.saveSchemaVersion.Value(), plan.targetSaveSchema.Value(), limits);
        }

        [[nodiscard]] Result<void> PlanProduct(const SaveMigrationSource &source, const SaveMigrationSupportDescriptor &support,
                                               SaveMigrationPlan &plan) {
            const auto admission = ClassifyVersion(source.productCompatibility, support.compatibility.productVersions,
                                                   SaveMigrationAxis::ProductCompatibility);
            if (admission.HasError())
                return Result<void>::Failure(admission.ErrorValue());
            if (admission.Value() == VersionAdmission::Migration)
                plan.targetProductCompatibility = support.compatibility.productVersions.direct.maximum;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> PlanParticipant(const SaveMigrationParticipantState &sourceParticipant,
                                                   const SaveCompatibilityPolicy &compatibility,
                                                   const SaveMigrationSupportDescriptor &support,
                                                   const std::vector<SaveMigrationDefinition> &definitions, SaveMigrationPlan &plan,
                                                   const SaveMigrationLimits &limits) {
            const SaveParticipantCompatibility *policy = FindPolicyParticipant(compatibility, sourceParticipant.participant);
            if (policy == nullptr) {
                if (sourceParticipant.required)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationSourceUnsupported,
                                       "Required source participant is not declared by the target save composition: " +
                                           sourceParticipant.participant.Value()));
                plan.participantTargets.push_back(
                    {.participant = sourceParticipant.participant, .schemaVersion = sourceParticipant.schemaVersion, .required = false});
                return Result<void>::Success();
            }

            const auto admission = ClassifyVersion(sourceParticipant.schemaVersion, policy->versions, SaveMigrationAxis::ParticipantSchema);
            if (admission.HasError()) {
                Error error = admission.ErrorValue();
                error.message += " participant=" + sourceParticipant.participant.Value();
                return Result<void>::Failure(std::move(error));
            }
            const ParticipantSchemaVersion targetVersion =
                admission.Value() == VersionAdmission::Migration ? policy->versions.direct.maximum : sourceParticipant.schemaVersion;
            plan.participantTargets.push_back(
                {.participant = sourceParticipant.participant, .schemaVersion = targetVersion, .required = policy->required});
            plan.participantRequirementChanges |= sourceParticipant.required != policy->required;
            return AppendRootRoute(plan, definitions, support, SaveMigrationAxis::ParticipantSchema, sourceParticipant.participant,
                                   sourceParticipant.schemaVersion.Value(), targetVersion.Value(), limits);
        }

        [[nodiscard]] Result<void> ValidateRequiredParticipants(const SaveMigrationSource &source,
                                                                const SaveMigrationSupportDescriptor &support) {
            for (const SaveParticipantCompatibility &policy : support.compatibility.participants) {
                if (FindStateParticipant(source, policy.participant) == nullptr && policy.required)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationSourceUnsupported,
                                       "Target composition requires a participant absent from the source: " + policy.participant.Value()));
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<std::vector<const SaveMigrationDefinition *>> BuildCheckpointRoute(const std::vector<SaveMigrationDefinition> &definitions,
                                                                              const SaveMigrationSupportDescriptor &support,
                                                                              const SaveMigrationAxis axis,
                                                                              const std::optional<SaveParticipantId> &participant,
                                                                              const std::uint32_t source, const std::uint32_t target,
                                                                              const SaveMigrationLimits &limits) {
        if (const auto validation = ValidateTargetCheckpointDeclarations(definitions, support, axis, participant, target);
            validation.HasError())
            return Result<std::vector<const SaveMigrationDefinition *>>::Failure(validation.ErrorValue());
        const auto matching = MatchingDeclarations(support, axis, participant);
        if (matching.empty())
            return BuildSequentialRoute(definitions, axis, participant, source, target, limits);
        if (matching.size() != 1)
            return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                MigrationError(SaveErrors::MigrationAmbiguous, "More than one checkpoint is declared for the same migration scope."));
        return BuildDeclaredCheckpointRoute(definitions, *matching.front(), axis, participant, source, target, limits);
    }

    Result<void> AppendRoute(SaveMigrationPlan &plan, const std::vector<const SaveMigrationDefinition *> &route,
                             const SaveMigrationLimits &limits) {
        if (route.size() > limits.maximumPlanSteps - plan.definitions.size())
            return Result<void>::Failure(
                MigrationError(SaveErrors::MigrationLimitExceeded, "Combined save migration plan exceeds its maximum step count."));
        for (const SaveMigrationDefinition *definition : route) {
            plan.definitions.push_back(*definition);
            const std::uint64_t weight = View(*definition).estimatedWork;
            if (weight > std::numeric_limits<std::uint64_t>::max() - plan.estimatedWork)
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationLimitExceeded, "Save migration estimated work overflows its bounded counter."));
            plan.estimatedWork += weight;
        }
        return Result<void>::Success();
    }

    Sha256Digest CatalogIdentity(const std::vector<SaveMigrationDefinition> &definitions) {
        std::string canonical{"Horo.SaveMigrationCatalog.v1"};
        AppendCount(canonical, definitions.size());
        for (const SaveMigrationDefinition &definition : definitions)
            AppendStep(canonical, View(definition));
        return ComputeSha256(Bytes(canonical));
    }

    Sha256Digest RouteIdentity(const SaveMigrationPlan &plan) {
        std::string canonical{"Horo.SaveMigrationRoute.v1"};
        AppendVersion(canonical, plan.sourceArchiveFormat.Value());
        AppendVersion(canonical, plan.sourceSaveSchema.Value());
        AppendVersion(canonical, plan.sourceProductCompatibility.Value());
        AppendVersion(canonical, plan.targetArchiveFormat.Value());
        AppendVersion(canonical, plan.targetSaveSchema.Value());
        AppendVersion(canonical, plan.targetProductCompatibility.Value());
        AppendCount(canonical, plan.registryGeneration);
        AppendDigest(canonical, plan.registryIdentity);
        AppendCount(canonical, plan.definitions.size());
        for (const SaveMigrationDefinition &definition : plan.definitions)
            AppendStep(canonical, View(definition));
        AppendCount(canonical, plan.participantTargets.size());
        canonical.push_back(plan.participantRequirementChanges ? '\x01' : '\x00');
        for (const auto &target : plan.participantTargets) {
            AppendText(canonical, target.participant.Value());
            AppendVersion(canonical, target.schemaVersion.Value());
            canonical.push_back(target.required ? '\x01' : '\x00');
        }
        return ComputeSha256(Bytes(canonical));
    }

    Result<SaveMigrationPlan> PlanSnapshot(const SaveMigrationRegistrySnapshot &snapshot, const SaveMigrationSource &source,
                                           const SaveMigrationSupportDescriptor &support, const SaveMigrationLimits &limits,
                                           const std::vector<SaveMigrationDefinition> &definitions, const Sha256Digest &catalogIdentity) {
        if (const auto sourceValidation = ValidateState(source, limits); sourceValidation.HasError())
            return Result<SaveMigrationPlan>::Failure(sourceValidation.ErrorValue());
        if (const auto supportValidation = ValidateSupport(support, limits); supportValidation.HasError())
            return Result<SaveMigrationPlan>::Failure(supportValidation.ErrorValue());

        SaveMigrationPlan plan{.sourceArchiveFormat = source.archiveFormatVersion,
                               .sourceSaveSchema = source.saveSchemaVersion,
                               .sourceProductCompatibility = source.productCompatibility,
                               .targetArchiveFormat = source.archiveFormatVersion,
                               .targetSaveSchema = source.saveSchemaVersion,
                               .targetProductCompatibility = source.productCompatibility,
                               .registryGeneration = snapshot.Generation(),
                               .registryIdentity = catalogIdentity};
        if (const auto result = PlanArchive(source, support, definitions, plan, limits); result.HasError())
            return Result<SaveMigrationPlan>::Failure(result.ErrorValue());
        if (const auto result = PlanSaveSchema(source, support, definitions, plan, limits); result.HasError())
            return Result<SaveMigrationPlan>::Failure(result.ErrorValue());
        if (const auto result = PlanProduct(source, support, plan); result.HasError())
            return Result<SaveMigrationPlan>::Failure(result.ErrorValue());

        plan.participantTargets.reserve(source.participants.size());
        for (const SaveMigrationParticipantState &sourceParticipant : source.participants) {
            if (const auto result = PlanParticipant(sourceParticipant, support.compatibility, support, definitions, plan, limits);
                result.HasError())
                return Result<SaveMigrationPlan>::Failure(result.ErrorValue());
        }
        if (const auto result = ValidateRequiredParticipants(source, support); result.HasError())
            return Result<SaveMigrationPlan>::Failure(result.ErrorValue());
        plan.routeIdentity = RouteIdentity(plan);
        return Result<SaveMigrationPlan>::Success(std::move(plan));
    }
}  // namespace Horo::Runtime::SaveMigrationDetail
