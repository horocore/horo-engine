#include "Horo/Runtime/Save/SaveMigration.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <format>
#include <limits>
#include <new>
#include <ranges>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace Horo::Runtime {
    namespace {
        enum class VersionAdmission : std::uint8_t {
            Direct,
            Migration,
        };

        struct StepView final {
            SaveMigrationId id;
            SaveMigrationAxis axis{SaveMigrationAxis::ArchiveFormat};
            SaveMigrationStepKind kind{SaveMigrationStepKind::Sequential};
            std::uint32_t from{};
            std::uint32_t to{};
            std::optional<SaveParticipantId> participant;
            const SaveMigrationFn *migrate{};
            const std::vector<SaveMigrationId> *equivalentSequentialSteps{};
            std::uint64_t estimatedWork{};
        };

        [[nodiscard]] Error MigrationError(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool IsCanonicalMigrationIdentity(const std::string_view value) noexcept {
            if (value.empty() || value.size() > MaximumSaveMigrationIdentityBytes)
                return false;
            if (!((value.front() >= 'a' && value.front() <= 'z') || (value.front() >= '0' && value.front() <= '9')))
                return false;
            return std::ranges::all_of(value, [](const char character) {
                return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '.' ||
                       character == '_' || character == '-';
            });
        }

        [[nodiscard]] std::string AxisName(const SaveMigrationAxis axis) {
            switch (axis) {
                case SaveMigrationAxis::ArchiveFormat:
                    return "archive-format";
                case SaveMigrationAxis::SaveSchema:
                    return "save-schema";
                case SaveMigrationAxis::ParticipantSchema:
                    return "participant-schema";
                case SaveMigrationAxis::ProductCompatibility:
                    return "product-compatibility";
                case SaveMigrationAxis::Count:
                    break;
            }
            return "unknown-axis";
        }

        [[nodiscard]] std::string StepScope(const StepView &step) {
            if (step.participant)
                return std::format("{} participant={}", AxisName(step.axis), step.participant->Value());
            return AxisName(step.axis);
        }

        [[nodiscard]] StepView View(const SaveMigrationDefinition &definition) {
            return std::visit([](const auto &step) -> StepView {
                using Step = std::decay_t<decltype(step)>;
                if constexpr (std::is_same_v<Step, ArchiveMigrationStep>) {
                    return {.id = step.id,
                            .axis = SaveMigrationAxis::ArchiveFormat,
                            .kind = step.kind,
                            .from = step.from.Value(),
                            .to = step.to.Value(),
                            .participant = std::nullopt,
                            .migrate = &step.migrate,
                            .equivalentSequentialSteps = &step.equivalentSequentialSteps,
                            .estimatedWork = step.estimatedWork};
                } else if constexpr (std::is_same_v<Step, SaveSchemaMigrationStep>) {
                    return {.id = step.id,
                            .axis = SaveMigrationAxis::SaveSchema,
                            .kind = step.kind,
                            .from = step.from.Value(),
                            .to = step.to.Value(),
                            .participant = std::nullopt,
                            .migrate = &step.migrate,
                            .equivalentSequentialSteps = &step.equivalentSequentialSteps,
                            .estimatedWork = step.estimatedWork};
                } else {
                    return {.id = step.id,
                            .axis = SaveMigrationAxis::ParticipantSchema,
                            .kind = step.kind,
                            .from = step.from.Value(),
                            .to = step.to.Value(),
                            .participant = step.participant,
                            .migrate = &step.migrate,
                            .equivalentSequentialSteps = &step.equivalentSequentialSteps,
                            .estimatedWork = step.estimatedWork};
                }
            }, definition);
        }

        [[nodiscard]] bool SameParticipant(const std::optional<SaveParticipantId> &left,
                                           const std::optional<SaveParticipantId> &right) noexcept {
            return left == right;
        }

        [[nodiscard]] bool MatchesScope(const StepView &step, const SaveMigrationAxis axis,
                                        const std::optional<SaveParticipantId> &participant) noexcept {
            return step.axis == axis && SameParticipant(step.participant, participant);
        }

        [[nodiscard]] std::string EdgeKey(const StepView &step) {
            return std::format("{}|{}|{}|{}|{}", static_cast<unsigned>(step.axis),
                               step.participant ? step.participant->Value() : std::string_view{}, static_cast<unsigned>(step.kind),
                               step.from, step.to);
        }

        [[nodiscard]] bool StepOrder(const SaveMigrationDefinition &left, const SaveMigrationDefinition &right) {
            const StepView lhs = View(left);
            const StepView rhs = View(right);
            if (lhs.axis != rhs.axis)
                return lhs.axis < rhs.axis;
            if (lhs.participant != rhs.participant)
                return lhs.participant < rhs.participant;
            if (lhs.from != rhs.from)
                return lhs.from < rhs.from;
            if (lhs.to != rhs.to)
                return lhs.to < rhs.to;
            if (lhs.kind != rhs.kind)
                return lhs.kind < rhs.kind;
            return lhs.id.value < rhs.id.value;
        }

        [[nodiscard]] bool ValidLimits(const SaveMigrationLimits &limits) noexcept {
            return limits.maximumDefinitions != 0 && limits.maximumPlanSteps != 0 && limits.maximumParticipants != 0 &&
                   limits.maximumArchiveBytes != 0 && limits.maximumParticipantPayloadBytes != 0 && limits.maximumTotalPayloadBytes != 0 &&
                   limits.maximumParticipantPayloadBytes <= limits.maximumTotalPayloadBytes;
        }

        template <typename Tag> [[nodiscard]] bool IsValidSupport(const SaveVersionSupport<Tag> &support) noexcept {
            if (!support.direct.minimum.IsValid() || !support.direct.maximum.IsValid() || support.direct.minimum > support.direct.maximum)
                return false;
            if (!support.migrationSource)
                return true;
            const auto &migration = *support.migrationSource;
            if (!migration.minimum.IsValid() || !migration.maximum.IsValid() || migration.minimum > migration.maximum)
                return false;
            return migration.maximum < support.direct.minimum || support.direct.maximum < migration.minimum;
        }

        [[nodiscard]] bool ValidParticipantPolicy(const SaveCompatibilityPolicy &policy) noexcept {
            if (!std::ranges::is_sorted(policy.participants, {}, &SaveParticipantCompatibility::participant))
                return false;
            for (std::size_t index = 0; index < policy.participants.size(); ++index) {
                const auto &entry = policy.participants[index];
                if (!entry.participant.IsValid() || !IsValidSupport(entry.versions) ||
                    (index != 0 && policy.participants[index - 1].participant == entry.participant))
                    return false;
            }
            return true;
        }

        [[nodiscard]] Result<void> ValidateDefinition(const SaveMigrationDefinition &definition, const SaveMigrationLimits &limits) {
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
            if (step.kind == SaveMigrationStepKind::Sequential && !step.equivalentSequentialSteps->empty())
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationDefinitionInvalid,
                                   "Sequential migration definitions cannot carry checkpoint equivalence metadata."));
            if (step.kind == SaveMigrationStepKind::Checkpoint) {
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
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateCatalog(std::vector<SaveMigrationDefinition> &definitions, const SaveMigrationLimits &limits) {
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

        [[nodiscard]] Result<void> ValidateSupport(const SaveMigrationSupportDescriptor &support, const SaveMigrationLimits &limits) {
            if (!ValidLimits(limits) || !IsValidSupport(support.compatibility.archiveVersions) ||
                !IsValidSupport(support.compatibility.saveSchemaVersions) || !IsValidSupport(support.compatibility.productVersions) ||
                !ValidParticipantPolicy(support.compatibility) || support.compatibility.participants.size() > limits.maximumParticipants ||
                support.checkpoints.size() > limits.maximumDefinitions)
                return Result<void>::Failure(MigrationError(SaveErrors::MigrationDefinitionInvalid,
                                                            "Save migration support ranges or participant policy are invalid."));

            std::set<std::string> checkpointKeys;
            for (const SaveMigrationCheckpointDeclaration &checkpoint : support.checkpoints) {
                const bool participantAxis = checkpoint.axis == SaveMigrationAxis::ParticipantSchema;
                if (checkpoint.axis >= SaveMigrationAxis::Count || checkpoint.axis == SaveMigrationAxis::ProductCompatibility ||
                    !checkpoint.id.IsValid() || (checkpoint.participant && !checkpoint.participant->IsValid()) ||
                    participantAxis != checkpoint.participant.has_value() || checkpoint.equivalentSequentialSteps.empty() ||
                    checkpoint.equivalentSequentialSteps.size() > limits.maximumPlanSteps)
                    return Result<void>::Failure(MigrationError(SaveErrors::MigrationCheckpointInvalid,
                                                                "Save migration checkpoint declaration is incomplete or out of bounds."));
                std::string key =
                    std::format("{}|{}|{}", static_cast<unsigned>(checkpoint.axis),
                                checkpoint.participant ? checkpoint.participant->Value() : std::string_view{}, checkpoint.id.value);
                if (!checkpointKeys.emplace(std::move(key)).second)
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
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateState(const SaveMigrationState &state, const SaveMigrationLimits &limits) {
            if (!ValidLimits(limits) || !state.archiveFormatVersion.IsValid() || !state.saveSchemaVersion.IsValid() ||
                !state.productCompatibility.IsValid() || state.archiveBytes.size() > limits.maximumArchiveBytes ||
                state.participants.size() > limits.maximumParticipants ||
                !std::ranges::is_sorted(state.participants, {}, &SaveMigrationParticipantState::participant))
                return Result<void>::Failure(
                    MigrationError(SaveErrors::MigrationCandidateInvalid,
                                   "Detached save migration state has invalid versions, bounds, or participant order."));

            std::uint64_t totalPayloadBytes = 0;
            for (std::size_t index = 0; index < state.participants.size(); ++index) {
                const auto &participant = state.participants[index];
                if (!participant.participant.IsValid() || !participant.schemaVersion.IsValid() ||
                    (index != 0 && state.participants[index - 1].participant == participant.participant) ||
                    participant.payload.size() > limits.maximumParticipantPayloadBytes ||
                    participant.payload.size() > std::numeric_limits<std::uint64_t>::max() - totalPayloadBytes)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationCandidateInvalid,
                                       "Detached save migration state contains an invalid or duplicate participant."));
                totalPayloadBytes += participant.payload.size();
                if (totalPayloadBytes > limits.maximumTotalPayloadBytes)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationLimitExceeded,
                                       "Detached save migration state exceeds its aggregate participant payload limit."));
            }
            return Result<void>::Success();
        }

        template <typename Tag>
        [[nodiscard]] Result<VersionAdmission> ClassifyVersion(const SaveVersion<Tag> version, const SaveVersionSupport<Tag> &support,
                                                               const SaveMigrationAxis axis) {
            if (version > support.direct.maximum)
                return Result<VersionAdmission>::Failure(
                    MigrationError(SaveErrors::MigrationUnsupportedNewer,
                                   std::format("{} source version {} is newer than supported writer version {}.", AxisName(axis),
                                               version.Value(), support.direct.maximum.Value())));
            if (support.direct.Contains(version))
                return Result<VersionAdmission>::Success(VersionAdmission::Direct);
            if (support.migrationSource && support.migrationSource->Contains(version))
                return Result<VersionAdmission>::Success(VersionAdmission::Migration);
            return Result<VersionAdmission>::Failure(
                MigrationError(SaveErrors::MigrationSourceUnsupported,
                               std::format("{} source version {} is outside the declared direct and migration-source ranges.",
                                           AxisName(axis), version.Value())));
        }

        [[nodiscard]] const SaveParticipantCompatibility *FindPolicyParticipant(const SaveCompatibilityPolicy &policy,
                                                                                const SaveParticipantId &participant) noexcept {
            const auto found = std::ranges::lower_bound(policy.participants, participant, {}, &SaveParticipantCompatibility::participant);
            return found != policy.participants.end() && found->participant == participant ? &*found : nullptr;
        }

        [[nodiscard]] const SaveMigrationParticipantState *FindStateParticipant(const SaveMigrationState &state,
                                                                                const SaveParticipantId &participant) noexcept {
            const auto found = std::ranges::lower_bound(state.participants, participant, {}, &SaveMigrationParticipantState::participant);
            return found != state.participants.end() && found->participant == participant ? &*found : nullptr;
        }

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

        [[nodiscard]] Result<std::vector<const SaveMigrationDefinition *>> BuildSequentialRoute(
            const std::vector<SaveMigrationDefinition> &definitions, const SaveMigrationAxis axis,
            const std::optional<SaveParticipantId> &participant, const std::uint32_t source, const std::uint32_t target,
            const SaveMigrationLimits &limits) {
            std::vector<const SaveMigrationDefinition *> route;
            std::unordered_set<std::uint32_t> visited;
            visited.reserve(limits.maximumPlanSteps);
            std::uint32_t current = source;
            while (current != target) {
                if (!visited.emplace(current).second)
                    return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                        MigrationError(SaveErrors::MigrationCycle,
                                       std::format("{} migration route revisits version {}.", AxisName(axis), current)));
                std::vector<const SaveMigrationDefinition *> candidates;
                for (const SaveMigrationDefinition &definition : definitions) {
                    const StepView step = View(definition);
                    if (step.kind == SaveMigrationStepKind::Sequential && MatchesScope(step, axis, participant) && step.from == current &&
                        step.to <= target)
                        candidates.push_back(&definition);
                }
                if (candidates.empty())
                    return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                        MigrationError(SaveErrors::MigrationPathMissing,
                                       std::format("{} migration route has a gap at version {} before target {}.", AxisName(axis), current,
                                                   target)));
                if (candidates.size() != 1) {
                    std::string identities;
                    for (const SaveMigrationDefinition *candidate : candidates) {
                        if (!identities.empty())
                            identities += ", ";
                        identities += View(*candidate).id.value;
                    }
                    return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                        MigrationError(SaveErrors::MigrationAmbiguous,
                                       std::format("{} migration route has multiple next hops at version {}: {}.", AxisName(axis), current,
                                                   identities)));
                }
                route.push_back(candidates.front());
                current = View(*candidates.front()).to;
                if (route.size() > limits.maximumPlanSteps)
                    return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                        MigrationError(SaveErrors::MigrationLimitExceeded, "Save migration route exceeds its maximum step count."));
            }
            return Result<std::vector<const SaveMigrationDefinition *>>::Success(std::move(route));
        }

        [[nodiscard]] Result<std::vector<const SaveMigrationDefinition *>> BuildCheckpointRoute(
            const std::vector<SaveMigrationDefinition> &definitions, const SaveMigrationSupportDescriptor &support,
            const SaveMigrationAxis axis, const std::optional<SaveParticipantId> &participant, const std::uint32_t source,
            const std::uint32_t target, const SaveMigrationLimits &limits) {
            if (const auto validation = ValidateTargetCheckpointDeclarations(definitions, support, axis, participant, target);
                validation.HasError())
                return Result<std::vector<const SaveMigrationDefinition *>>::Failure(validation.ErrorValue());
            std::vector<const SaveMigrationCheckpointDeclaration *> matchingDeclarations;
            for (const SaveMigrationCheckpointDeclaration &declaration : support.checkpoints)
                if (declaration.axis == axis && SameParticipant(declaration.participant, participant))
                    matchingDeclarations.push_back(&declaration);
            if (matchingDeclarations.empty())
                return BuildSequentialRoute(definitions, axis, participant, source, target, limits);
            if (matchingDeclarations.size() != 1)
                return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                    MigrationError(SaveErrors::MigrationAmbiguous, "More than one checkpoint is declared for the same migration scope."));

            const SaveMigrationCheckpointDeclaration &declaration = *matchingDeclarations.front();
            std::vector<const SaveMigrationDefinition *> checkpoints;
            for (const SaveMigrationDefinition &definition : definitions) {
                const StepView step = View(definition);
                if (step.kind == SaveMigrationStepKind::Checkpoint && step.id == declaration.id && MatchesScope(step, axis, participant) &&
                    step.from == source && step.to == target)
                    checkpoints.push_back(&definition);
            }
            if (checkpoints.size() != 1)
                return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointInvalid,
                                   std::format("Declared checkpoint '{}' does not identify exactly one {} edge from {} to {}.",
                                               declaration.id.value, AxisName(axis), source, target)));
            const StepView checkpoint = View(*checkpoints.front());
            if (*checkpoint.equivalentSequentialSteps != declaration.equivalentSequentialSteps)
                return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                   std::format("Checkpoint '{}' and its release declaration carry different sequential equivalence routes.",
                                               checkpoint.id.value)));

            std::vector<const SaveMigrationDefinition *> route;
            std::uint32_t current = source;
            for (const SaveMigrationId &expected : declaration.equivalentSequentialSteps) {
                std::vector<const SaveMigrationDefinition *> byIdentity;
                for (const SaveMigrationDefinition &definition : definitions) {
                    const StepView step = View(definition);
                    if (step.id == expected)
                        byIdentity.push_back(&definition);
                }
                if (byIdentity.size() != 1) {
                    return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                        MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                       std::format("Checkpoint '{}' references sequential step '{}' which is not unique in the catalog.",
                                                   checkpoint.id.value, expected.value)));
                }
                const StepView sequential = View(*byIdentity.front());
                if (sequential.kind != SaveMigrationStepKind::Sequential || !MatchesScope(sequential, axis, participant) ||
                    sequential.from != current || sequential.to > target)
                    return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                        MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                       std::format("Checkpoint '{}' equivalence step '{}' does not continue from version {}.",
                                                   checkpoint.id.value, expected.value, current)));

                std::size_t candidateCount = 0;
                for (const SaveMigrationDefinition &definition : definitions) {
                    const StepView candidate = View(definition);
                    if (candidate.kind == SaveMigrationStepKind::Sequential && MatchesScope(candidate, axis, participant) &&
                        candidate.from == current && candidate.to <= target)
                        ++candidateCount;
                }
                if (candidateCount != 1)
                    return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                        MigrationError(SaveErrors::MigrationAmbiguous,
                                       std::format("Checkpoint '{}' cannot prove a deterministic sequential route at version {}.",
                                                   checkpoint.id.value, current)));
                route.push_back(byIdentity.front());
                current = sequential.to;
                if (route.size() > limits.maximumPlanSteps)
                    return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                        MigrationError(SaveErrors::MigrationLimitExceeded, "Checkpoint equivalence route exceeds its maximum step count."));
            }
            if (current != target)
                return Result<std::vector<const SaveMigrationDefinition *>>::Failure(
                    MigrationError(SaveErrors::MigrationCheckpointNotEquivalent,
                                   std::format("Checkpoint '{}' equivalence route ends at {} instead of target {}.", checkpoint.id.value,
                                               current, target)));
            return Result<std::vector<const SaveMigrationDefinition *>>::Success({checkpoints.front()});
        }

        [[nodiscard]] Result<void> AppendRoute(SaveMigrationPlan &plan, const std::vector<const SaveMigrationDefinition *> &route,
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

        [[nodiscard]] Result<void> ValidateTargetParticipants(const SaveMigrationCandidate &candidate, const SaveMigrationPlan &plan) {
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

        void AppendVersion(std::string &output, const std::uint32_t value) {
            output.push_back(static_cast<char>(value));
            output.push_back(static_cast<char>(value >> 8U));
            output.push_back(static_cast<char>(value >> 16U));
            output.push_back(static_cast<char>(value >> 24U));
        }

        void AppendCount(std::string &output, const std::uint64_t value) {
            for (std::size_t index = 0; index < sizeof(value); ++index)
                output.push_back(static_cast<char>(value >> (index * 8U)));
        }

        void AppendText(std::string &output, const std::string_view value) {
            AppendCount(output, value.size());
            output.append(value);
        }

        void AppendStep(std::string &output, const StepView &step) {
            output.push_back(static_cast<char>(step.axis));
            output.push_back(static_cast<char>(step.kind));
            AppendVersion(output, step.from);
            AppendVersion(output, step.to);
            AppendCount(output, step.estimatedWork);
            AppendText(output, step.id.value);
            AppendText(output, step.participant ? step.participant->Value() : std::string_view{});
            AppendCount(output, step.equivalentSequentialSteps->size());
            for (const SaveMigrationId &identity : *step.equivalentSequentialSteps)
                AppendText(output, identity.value);
        }

        [[nodiscard]] std::span<const std::byte> Bytes(const std::string &value) noexcept {
            return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
        }

        void AppendDigest(std::string &output, const Sha256Digest &digest) {
            for (const std::uint8_t byte : digest.bytes)
                output.push_back(static_cast<char>(byte));
        }

        [[nodiscard]] Sha256Digest CatalogIdentity(const std::vector<SaveMigrationDefinition> &definitions) {
            std::string canonical{"Horo.SaveMigrationCatalog.v1"};
            AppendCount(canonical, definitions.size());
            for (const SaveMigrationDefinition &definition : definitions)
                AppendStep(canonical, View(definition));
            return ComputeSha256(Bytes(canonical));
        }

        [[nodiscard]] Sha256Digest RouteIdentity(const SaveMigrationPlan &plan) {
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

        [[nodiscard]] Result<SaveMigrationPlan> PlanSnapshot(const SaveMigrationRegistrySnapshot &snapshot,
                                                             const SaveMigrationSource &source,
                                                             const SaveMigrationSupportDescriptor &support,
                                                             const SaveMigrationLimits &limits,
                                                             const std::vector<SaveMigrationDefinition> &definitions,
                                                             const Sha256Digest &catalogIdentity) {
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

            const auto addRoute = [&](const SaveMigrationAxis axis, const std::optional<SaveParticipantId> &participant,
                                      const std::uint32_t from, const std::uint32_t to) -> Result<void> {
                if (from == to)
                    return Result<void>::Success();
                auto route = BuildCheckpointRoute(definitions, support, axis, participant, from, to, limits);
                if (route.HasError())
                    return Result<void>::Failure(route.ErrorValue());
                return AppendRoute(plan, route.Value(), limits);
            };

            const auto archiveAdmission =
                ClassifyVersion(source.archiveFormatVersion, support.compatibility.archiveVersions, SaveMigrationAxis::ArchiveFormat);
            if (archiveAdmission.HasError())
                return Result<SaveMigrationPlan>::Failure(archiveAdmission.ErrorValue());
            if (archiveAdmission.Value() == VersionAdmission::Migration) {
                plan.targetArchiveFormat = support.compatibility.archiveVersions.direct.maximum;
                if (const auto result = addRoute(SaveMigrationAxis::ArchiveFormat, std::nullopt, source.archiveFormatVersion.Value(),
                                                 plan.targetArchiveFormat.Value());
                    result.HasError())
                    return Result<SaveMigrationPlan>::Failure(result.ErrorValue());
            }

            const auto schemaAdmission =
                ClassifyVersion(source.saveSchemaVersion, support.compatibility.saveSchemaVersions, SaveMigrationAxis::SaveSchema);
            if (schemaAdmission.HasError())
                return Result<SaveMigrationPlan>::Failure(schemaAdmission.ErrorValue());
            if (schemaAdmission.Value() == VersionAdmission::Migration) {
                plan.targetSaveSchema = support.compatibility.saveSchemaVersions.direct.maximum;
                if (const auto result = addRoute(SaveMigrationAxis::SaveSchema, std::nullopt, source.saveSchemaVersion.Value(),
                                                 plan.targetSaveSchema.Value());
                    result.HasError())
                    return Result<SaveMigrationPlan>::Failure(result.ErrorValue());
            }

            const auto productAdmission = ClassifyVersion(source.productCompatibility, support.compatibility.productVersions,
                                                          SaveMigrationAxis::ProductCompatibility);
            if (productAdmission.HasError())
                return Result<SaveMigrationPlan>::Failure(productAdmission.ErrorValue());
            if (productAdmission.Value() == VersionAdmission::Migration)
                plan.targetProductCompatibility = support.compatibility.productVersions.direct.maximum;

            plan.participantTargets.reserve(source.participants.size());
            for (const SaveMigrationParticipantState &sourceParticipant : source.participants) {
                const SaveParticipantCompatibility *policy = FindPolicyParticipant(support.compatibility, sourceParticipant.participant);
                if (policy == nullptr) {
                    if (sourceParticipant.required)
                        return Result<SaveMigrationPlan>::Failure(
                            MigrationError(SaveErrors::MigrationSourceUnsupported,
                                           "Required source participant is not declared by the target save composition: " +
                                               sourceParticipant.participant.Value()));
                    plan.participantTargets.push_back({.participant = sourceParticipant.participant,
                                                       .schemaVersion = sourceParticipant.schemaVersion,
                                                       .required = false});
                    continue;
                }
                const auto admission =
                    ClassifyVersion(sourceParticipant.schemaVersion, policy->versions, SaveMigrationAxis::ParticipantSchema);
                if (admission.HasError()) {
                    Error error = admission.ErrorValue();
                    error.message += " participant=" + sourceParticipant.participant.Value();
                    return Result<SaveMigrationPlan>::Failure(std::move(error));
                }
                const ParticipantSchemaVersion targetVersion =
                    admission.Value() == VersionAdmission::Migration ? policy->versions.direct.maximum : sourceParticipant.schemaVersion;
                plan.participantTargets.push_back(
                    {.participant = sourceParticipant.participant, .schemaVersion = targetVersion, .required = policy->required});
                plan.participantRequirementChanges |= sourceParticipant.required != policy->required;
                if (admission.Value() == VersionAdmission::Migration) {
                    auto route =
                        BuildCheckpointRoute(definitions, support, SaveMigrationAxis::ParticipantSchema, sourceParticipant.participant,
                                             sourceParticipant.schemaVersion.Value(), targetVersion.Value(), limits);
                    if (route.HasError())
                        return Result<SaveMigrationPlan>::Failure(route.ErrorValue());
                    if (const auto result = AppendRoute(plan, route.Value(), limits); result.HasError())
                        return Result<SaveMigrationPlan>::Failure(result.ErrorValue());
                }
            }
            for (const SaveParticipantCompatibility &policy : support.compatibility.participants) {
                if (FindStateParticipant(source, policy.participant) == nullptr && policy.required)
                    return Result<SaveMigrationPlan>::Failure(
                        MigrationError(SaveErrors::MigrationSourceUnsupported,
                                       "Target composition requires a participant absent from the source: " + policy.participant.Value()));
            }
            if (!std::ranges::is_sorted(plan.participantTargets, {}, &SaveMigrationParticipantTarget::participant))
                std::ranges::sort(plan.participantTargets, {}, &SaveMigrationParticipantTarget::participant);
            plan.routeIdentity = RouteIdentity(plan);
            return Result<SaveMigrationPlan>::Success(std::move(plan));
        }

        [[nodiscard]] std::string StepFailureContext(const StepView &step) {
            return std::format("Migration step '{}' ({}, {} -> {}) failed.", step.id.value, StepScope(step), step.from, step.to);
        }

        [[nodiscard]] Result<void> ValidateStepOutput(const SaveMigrationCandidate &before, const SaveMigrationCandidate &after,
                                                      const StepView &step, const SaveMigrationLimits &limits) {
            if (const auto validation = ValidateSaveMigrationState(after, limits); validation.HasError())
                return validation;
            if (step.axis == SaveMigrationAxis::ArchiveFormat) {
                if (after.archiveFormatVersion.Value() != step.to || after.saveSchemaVersion != before.saveSchemaVersion ||
                    after.productCompatibility != before.productCompatibility || after.participants != before.participants)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationCandidateInvalid,
                                       std::format("{} changed a version axis or participant state outside its archive-format ownership.",
                                                   StepFailureContext(step))));
            } else if (step.axis == SaveMigrationAxis::SaveSchema) {
                if (after.saveSchemaVersion.Value() != step.to || after.archiveFormatVersion != before.archiveFormatVersion ||
                    after.productCompatibility != before.productCompatibility || after.participants.size() != before.participants.size())
                    return Result<void>::Failure(MigrationError(SaveErrors::MigrationCandidateInvalid,
                                                                std::format("{} changed a version axis outside its save-schema ownership.",
                                                                            StepFailureContext(step))));
                for (std::size_t index = 0; index < before.participants.size(); ++index) {
                    const auto &beforeParticipant = before.participants[index];
                    const auto &afterParticipant = after.participants[index];
                    if (afterParticipant.participant != beforeParticipant.participant ||
                        afterParticipant.schemaVersion != beforeParticipant.schemaVersion ||
                        afterParticipant.required != beforeParticipant.required)
                        return Result<void>::Failure(
                            MigrationError(SaveErrors::MigrationCandidateInvalid,
                                           std::format("{} changed participant schema metadata outside participant migration ownership.",
                                                       StepFailureContext(step))));
                }
            } else {
                const auto *beforeParticipant = FindStateParticipant(before, *step.participant);
                const auto *afterParticipant = FindStateParticipant(after, *step.participant);
                if (beforeParticipant == nullptr || afterParticipant == nullptr || afterParticipant->schemaVersion.Value() != step.to ||
                    afterParticipant->required != beforeParticipant->required ||
                    after.archiveFormatVersion != before.archiveFormatVersion || after.saveSchemaVersion != before.saveSchemaVersion ||
                    after.productCompatibility != before.productCompatibility)
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationCandidateInvalid,
                                       std::format("{} changed an unrelated axis or lost its participant candidate.",
                                                   StepFailureContext(step))));
                if (before.participants.size() != after.participants.size())
                    return Result<void>::Failure(
                        MigrationError(SaveErrors::MigrationCandidateInvalid,
                                       std::format("{} changed participant composition outside its participant-local ownership.",
                                                   StepFailureContext(step))));
                for (const auto &beforeEntry : before.participants) {
                    const auto *afterEntry = FindStateParticipant(after, beforeEntry.participant);
                    if (afterEntry == nullptr || beforeEntry.participant != *step.participant && *afterEntry != beforeEntry)
                        return Result<void>::Failure(
                            MigrationError(SaveErrors::MigrationCandidateInvalid,
                                           std::format("{} modified an unrelated participant candidate.", StepFailureContext(step))));
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    Result<SaveMigrationId> SaveMigrationId::Parse(const std::string_view text) {
        if (!IsCanonicalMigrationIdentity(text))
            return Result<SaveMigrationId>::Failure(
                MigrationError(SaveErrors::MigrationDefinitionInvalid, "Save migration identity is not canonical."));
        try {
            return Result<SaveMigrationId>::Success({.value = std::string{text}});
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationId>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    bool SaveMigrationId::IsValid() const noexcept {
        return IsCanonicalMigrationIdentity(value);
    }

    bool SaveMigrationPlan::IsNoOp() const noexcept {
        return definitions.empty() && sourceArchiveFormat == targetArchiveFormat && sourceSaveSchema == targetSaveSchema &&
               sourceProductCompatibility == targetProductCompatibility && !participantRequirementChanges;
    }

    struct SaveMigrationRegistryDetail::SnapshotStorage final {
        std::vector<SaveMigrationDefinition> definitions;
        Sha256Digest identity;
    };

    SaveMigrationRegistrySnapshot::SaveMigrationRegistrySnapshot(
        const std::uint64_t generation, std::shared_ptr<const SaveMigrationRegistryDetail::SnapshotStorage> storage) noexcept
        : generation_(generation), storage_(std::move(storage)) {}

    bool SaveMigrationRegistrySnapshot::IsValid() const noexcept {
        return generation_ != 0 && storage_ != nullptr;
    }

    std::uint64_t SaveMigrationRegistrySnapshot::Generation() const noexcept {
        return generation_;
    }

    const Sha256Digest &SaveMigrationRegistrySnapshot::CatalogIdentity() const noexcept {
        static const Sha256Digest invalid{};
        return storage_ ? storage_->identity : invalid;
    }

    std::span<const SaveMigrationDefinition> SaveMigrationRegistrySnapshot::Definitions() const noexcept {
        return storage_ ? std::span<const SaveMigrationDefinition>{storage_->definitions} : std::span<const SaveMigrationDefinition>{};
    }

    Result<SaveMigrationPlan> SaveMigrationRegistrySnapshot::Plan(const SaveMigrationSource &source,
                                                                  const SaveMigrationSupportDescriptor &support,
                                                                  const SaveMigrationLimits &limits) const {
        if (!IsValid())
            return Result<SaveMigrationPlan>::Failure(
                MigrationError(SaveErrors::MigrationPlanInvalid, "Cannot plan from an invalid save migration registry snapshot."));
        try {
            return PlanSnapshot(*this, source, support, limits, storage_->definitions, storage_->identity);
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationPlan>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<SaveMigrationRegistry> SaveMigrationRegistry::Create(const std::span<const SaveMigrationDefinition> definitions,
                                                                const SaveMigrationLimits &limits) {
        if (!ValidLimits(limits) || definitions.size() > limits.maximumDefinitions)
            return Result<SaveMigrationRegistry>::Failure(MigrationError(SaveErrors::MigrationLimitExceeded));
        try {
            std::vector<SaveMigrationDefinition> ordered(definitions.begin(), definitions.end());
            if (const auto validation = ValidateCatalog(ordered, limits); validation.HasError())
                return Result<SaveMigrationRegistry>::Failure(validation.ErrorValue());
            return Result<SaveMigrationRegistry>::Success(SaveMigrationRegistry(std::move(ordered), limits));
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationRegistry>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<SaveMigrationRegistration> SaveMigrationRegistry::Register(const SaveMigrationDefinition &definition) {
        if (closed_)
            return Result<SaveMigrationRegistration>::Failure(MigrationError(SaveErrors::MigrationRegistryClosed));
        if (definitions_.size() >= limits_.maximumDefinitions)
            return Result<SaveMigrationRegistration>::Failure(MigrationError(SaveErrors::MigrationLimitExceeded));
        try {
            std::vector<SaveMigrationDefinition> candidate = definitions_;
            candidate.push_back(definition);
            if (const auto validation = ValidateCatalog(candidate, limits_); validation.HasError())
                return Result<SaveMigrationRegistration>::Failure(validation.ErrorValue());
            if (generation_ == std::numeric_limits<std::uint64_t>::max())
                return Result<SaveMigrationRegistration>::Failure(
                    MigrationError(SaveErrors::MigrationLimitExceeded, "Save migration registry generation is exhausted."));
            const SaveMigrationId registeredId = View(definition).id;
            definitions_ = std::move(candidate);
            ++generation_;
            return Result<SaveMigrationRegistration>::Success({.id = registeredId, .registryGeneration = generation_});
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationRegistration>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<bool> SaveMigrationRegistry::Unregister(const SaveMigrationId &id) {
        if (closed_)
            return Result<bool>::Failure(MigrationError(SaveErrors::MigrationRegistryClosed));
        if (!id.IsValid())
            return Result<bool>::Failure(
                MigrationError(SaveErrors::MigrationDefinitionInvalid, "Cannot unregister an invalid save migration identity."));
        const auto found = std::ranges::find_if(definitions_, [&id](const SaveMigrationDefinition &definition) {
            return View(definition).id == id;
        });
        if (found == definitions_.end())
            return Result<bool>::Success(false);
        try {
            std::vector<SaveMigrationDefinition> candidate = definitions_;
            const auto candidateFound = std::ranges::find_if(candidate, [&id](const SaveMigrationDefinition &definition) {
                return View(definition).id == id;
            });
            candidate.erase(candidateFound);
            if (const auto validation = ValidateCatalog(candidate, limits_); validation.HasError())
                return Result<bool>::Failure(validation.ErrorValue());
            if (generation_ == std::numeric_limits<std::uint64_t>::max())
                return Result<bool>::Failure(
                    MigrationError(SaveErrors::MigrationLimitExceeded, "Save migration registry generation is exhausted."));
            definitions_ = std::move(candidate);
            ++generation_;
            return Result<bool>::Success(true);
        } catch (const std::bad_alloc &) {
            return Result<bool>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<SaveMigrationRegistrySnapshot> SaveMigrationRegistry::Snapshot() const {
        try {
            auto storage = std::make_shared<SaveMigrationRegistryDetail::SnapshotStorage>();
            storage->definitions = definitions_;
            storage->identity = CatalogIdentity(storage->definitions);
            std::shared_ptr<const SaveMigrationRegistryDetail::SnapshotStorage> immutableStorage = std::move(storage);
            return Result<SaveMigrationRegistrySnapshot>::Success(SaveMigrationRegistrySnapshot(generation_, std::move(immutableStorage)));
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationRegistrySnapshot>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    void SaveMigrationRegistry::Close() noexcept {
        closed_ = true;
    }

    bool SaveMigrationRegistry::IsClosed() const noexcept {
        return closed_;
    }

    std::uint64_t SaveMigrationRegistry::Generation() const noexcept {
        return generation_;
    }

    Result<SaveMigrationCandidate> SaveMigrationExecutor::Migrate(const SaveMigrationSource &source, const SaveMigrationPlan &plan,
                                                                  const SaveMigrationLimits &limits) {
        try {
            if (!ValidLimits(limits) || plan.registryGeneration == 0 || plan.definitions.size() > limits.maximumPlanSteps ||
                plan.sourceArchiveFormat != source.archiveFormatVersion || plan.sourceSaveSchema != source.saveSchemaVersion ||
                plan.sourceProductCompatibility != source.productCompatibility || plan.routeIdentity != RouteIdentity(plan))
                return Result<SaveMigrationCandidate>::Failure(
                    MigrationError(SaveErrors::MigrationPlanInvalid, "Migration plan does not bind to its supplied source or limits."));
            if (const auto sourceValidation = ValidateState(source, limits); sourceValidation.HasError())
                return Result<SaveMigrationCandidate>::Failure(sourceValidation.ErrorValue());

            const SaveMigrationSource sourceBefore = source;
            SaveMigrationCandidate current = source;
            for (const SaveMigrationDefinition &definition : plan.definitions) {
                const StepView step = View(definition);
                if (const auto definitionValidation = ValidateDefinition(definition, limits); definitionValidation.HasError())
                    return Result<SaveMigrationCandidate>::Failure(
                        MigrationError(SaveErrors::MigrationPlanInvalid,
                                       std::format("Plan contains invalid migration definition '{}'.", step.id.value)));
                if (step.axis == SaveMigrationAxis::ArchiveFormat && current.archiveFormatVersion.Value() != step.from)
                    return Result<SaveMigrationCandidate>::Failure(
                        MigrationError(SaveErrors::MigrationPlanInvalid,
                                       std::format("Migration step '{}' expects archive version {}, current candidate is {}.",
                                                   step.id.value, step.from, current.archiveFormatVersion.Value())));
                if (step.axis == SaveMigrationAxis::SaveSchema && current.saveSchemaVersion.Value() != step.from)
                    return Result<SaveMigrationCandidate>::Failure(
                        MigrationError(SaveErrors::MigrationPlanInvalid,
                                       std::format("Migration step '{}' expects save schema {}, current candidate is {}.", step.id.value,
                                                   step.from, current.saveSchemaVersion.Value())));
                if (step.axis == SaveMigrationAxis::ParticipantSchema) {
                    const auto *participant = FindStateParticipant(current, *step.participant);
                    if (participant == nullptr || participant->schemaVersion.Value() != step.from)
                        return Result<SaveMigrationCandidate>::Failure(
                            MigrationError(SaveErrors::MigrationPlanInvalid,
                                           std::format("Migration step '{}' expects participant '{}' schema {}, but the candidate differs.",
                                                       step.id.value, step.participant->Value(), step.from)));
                }

                SaveMigrationStepContext context{.id = step.id, .axis = step.axis, .kind = step.kind, .participant = step.participant};
                Result<SaveMigrationCandidate> transformed =
                    Result<SaveMigrationCandidate>::Failure(MigrationError(SaveErrors::MigrationStepFailed));
                try {
                    transformed = (*step.migrate)(current, context);
                } catch (const std::bad_alloc &) {
                    return Result<SaveMigrationCandidate>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
                } catch (...) {
                    return Result<SaveMigrationCandidate>::Failure(
                        MigrationError(SaveErrors::MigrationStepFailed, StepFailureContext(step) + " Callback threw an exception."));
                }
                if (transformed.HasError()) {
                    Error wrapped = MigrationError(SaveErrors::MigrationStepFailed, StepFailureContext(step));
                    return Result<SaveMigrationCandidate>::Failure(WithCause(std::move(wrapped), transformed.ErrorValue()));
                }
                if (const auto outputValidation = ValidateStepOutput(current, transformed.Value(), step, limits);
                    outputValidation.HasError())
                    return Result<SaveMigrationCandidate>::Failure(outputValidation.ErrorValue());
                current = std::move(transformed).Value();
            }
            if (current.productCompatibility != plan.targetProductCompatibility)
                current.productCompatibility = plan.targetProductCompatibility;
            for (SaveMigrationParticipantState &participant : current.participants) {
                const auto target = std::ranges::find_if(plan.participantTargets, [&participant](const auto &candidate) {
                    return candidate.participant == participant.participant;
                });
                if (target != plan.participantTargets.end() && target->participant == participant.participant)
                    participant.required = target->required;
            }
            if (current.archiveFormatVersion != plan.targetArchiveFormat || current.saveSchemaVersion != plan.targetSaveSchema)
                return Result<SaveMigrationCandidate>::Failure(
                    MigrationError(SaveErrors::MigrationCandidateInvalid,
                                   "Migrated candidate did not reach every planned root target version."));
            if (const auto targetValidation = ValidateTargetParticipants(current, plan); targetValidation.HasError())
                return Result<SaveMigrationCandidate>::Failure(targetValidation.ErrorValue());
            if (const auto candidateValidation = ValidateState(current, limits); candidateValidation.HasError())
                return Result<SaveMigrationCandidate>::Failure(candidateValidation.ErrorValue());
            if (source != sourceBefore)
                return Result<SaveMigrationCandidate>::Failure(
                    MigrationError(SaveErrors::MigrationSourceMutated,
                                   "Migration callback altered the source state during detached execution."));
            return Result<SaveMigrationCandidate>::Success(std::move(current));
        } catch (const std::bad_alloc &) {
            return Result<SaveMigrationCandidate>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }

    Result<void> ValidateSaveMigrationState(const SaveMigrationState &state, const SaveMigrationLimits &limits) {
        try {
            return ValidateState(state, limits);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MigrationError(SaveErrors::MigrationAllocationFailed));
        }
    }
}  // namespace Horo::Runtime
