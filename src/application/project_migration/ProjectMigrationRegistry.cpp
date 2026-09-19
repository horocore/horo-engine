#include "../project/ProjectErrors.h"
#include "Horo/Application/ProjectMigration.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/TransparentString.h"

#include <algorithm>
#include <format>
#include <set>
#include <unordered_set>

namespace Horo::Application {
    namespace {
        using Horo::TransparentStringHash;
        using Horo::TransparentStringSet;

        [[nodiscard]] Error MigrationError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool Before(const ContractBaselineVersion &left, const ContractBaselineVersion &right) noexcept {
            return CompareHoroVersions(left.value, right.value) == std::strong_ordering::less;
        }

        [[nodiscard]] bool Same(const ContractBaselineVersion &left, const ContractBaselineVersion &right) noexcept {
            return CompareHoroVersions(left.value, right.value) == std::strong_ordering::equal;
        }

        [[nodiscard]] std::string EdgeKey(const ProjectMigrationDefinition &definition) {
            return std::format("{}:{}:{}", static_cast<int>(definition.kind), FormatHoroVersion(definition.from.value),
                               FormatHoroVersion(definition.to.value));
        }

        [[nodiscard]] bool HasProvider(const ProjectMigrationSupportDescriptor &support, const MigrationProviderId &provider) {
            return std::ranges::any_of(support.availableProviders, [&provider](const MigrationProviderId &available) {
                return available == provider;
            });
        }

        [[nodiscard]] Result<std::vector<const ProjectMigrationDefinition *>> CollectDeclaredCheckpoints(
            const ProjectMigrationSupportDescriptor &support, const std::vector<ProjectMigrationDefinition> &definitions,
            const ContractBaselineVersion &source) {
            for (const ProjectMigrationDefinition &definition : definitions) {
                if (definition.kind != ProjectMigrationDefinitionKind::Checkpoint || !Same(definition.to, support.target))
                    continue;
                const bool declared =
                    std::ranges::any_of(support.checkpoints, [&definition](const MigrationCheckpointDeclaration &declaration) {
                    return declaration.id == definition.id && Same(declaration.source, definition.from) &&
                           Same(declaration.target, definition.to);
                });
                if (!declared)
                    return Result<std::vector<const ProjectMigrationDefinition *>>::Failure(
                        MigrationError(ProjectErrors::MigrationCatalogInvalid,
                                       "Catalog checkpoint is not declared by the target support descriptor: " + definition.id.value));
            }

            std::vector<const ProjectMigrationDefinition *> declaredCheckpoints;
            for (const MigrationCheckpointDeclaration &declaration : support.checkpoints) {
                if (!Same(declaration.source, source) || !Same(declaration.target, support.target))
                    continue;
                for (const ProjectMigrationDefinition &definition : definitions)
                    if (definition.kind == ProjectMigrationDefinitionKind::Checkpoint && definition.id == declaration.id &&
                        Same(definition.from, source) && Same(definition.to, support.target))
                        declaredCheckpoints.push_back(&definition);
            }
            if (declaredCheckpoints.size() > 1)
                return Result<std::vector<const ProjectMigrationDefinition *>>::Failure(
                    MigrationError(ProjectErrors::MigrationAmbiguous,
                                   "More than one declared checkpoint matches this exact source and target."));
            return Result<std::vector<const ProjectMigrationDefinition *>>::Success(std::move(declaredCheckpoints));
        }

        [[nodiscard]] Result<void> BuildSequentialChain(const ProjectMigrationSupportDescriptor &support,
                                                        const std::vector<ProjectMigrationDefinition> &definitions,
                                                        const ContractBaselineVersion &source, PersistentContractHash currentContract,
                                                        ProjectMigrationPlan &plan) {
            ContractBaselineVersion current = source;
            std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> visited;
            while (!Same(current, support.target)) {
                if (!visited.emplace(FormatHoroVersion(current.value)).second)
                    return Result<void>::Failure(
                        MigrationError(ProjectErrors::MigrationCycle, "Sequential migration chain contains a cycle."));
                std::vector<const ProjectMigrationDefinition *> candidates;
                for (const ProjectMigrationDefinition &definition : definitions) {
                    if (definition.kind == ProjectMigrationDefinitionKind::Sequential && Same(definition.from, current) &&
                        !Before(support.target, definition.to)) {
                        candidates.push_back(&definition);
                    }
                }
                if (candidates.empty())
                    return Result<void>::Failure(MigrationError(ProjectErrors::MigrationPathMissing,
                                                                "Sequential migration catalog has a gap before the target baseline."));
                if (candidates.size() != 1)
                    return Result<void>::Failure(MigrationError(ProjectErrors::MigrationAmbiguous,
                                                                "Sequential migration catalog offers multiple canonical next hops."));
                const ProjectMigrationDefinition &next = *candidates.front();
                if (next.sourceContract != currentContract)
                    return Result<void>::Failure(MigrationError(ProjectErrors::MigrationContractMismatch,
                                                                "Sequential migration source contract does not match the preceding hop."));
                plan.definitions.push_back(next);
                current = next.to;
                currentContract = next.targetContract;
            }
            if (currentContract != support.targetContract)
                return Result<void>::Failure(MigrationError(ProjectErrors::MigrationContractMismatch,
                                                            "Sequential migration target contract does not match the support descriptor."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidatePlanProvidersAndComputeWeight(ProjectMigrationPlan &plan,
                                                                         const ProjectMigrationSupportDescriptor &support) {
            for (const ProjectMigrationDefinition &definition : plan.definitions) {
                for (const MigrationProviderId &provider : definition.requiredProviders) {
                    if (!HasProvider(support, provider))
                        return Result<void>::Failure(MigrationError(ProjectErrors::MigrationProviderMissing,
                                                                    "Required migration provider is unavailable: " + provider.value));
                }
                for (const ProjectMigrationNode &node : definition.pipeline->Nodes()) {
                    if (node.documentStage)
                        plan.estimatedWeight += node.documentStage->Describe().estimatedWeight;
                    else if (node.stage)
                        plan.estimatedWeight += node.stage->Describe().estimatedWeight;
                    else if (node.validator)
                        plan.estimatedWeight += node.validator->Describe().estimatedWeight;
                }
            }
            if (plan.targetValidator)
                plan.estimatedWeight += plan.targetValidator->Describe().estimatedWeight;
            return Result<void>::Success();
        }
    }  // namespace

    ProjectMigrationPipelineBuilder ProjectMigrationPipelineBuilder::Begin(StableMigrationId definitionId) {
        return ProjectMigrationPipelineBuilder(std::move(definitionId));
    }

    ProjectMigrationPipelineBuilder &ProjectMigrationPipelineBuilder::AddForEach(
        MigrationDocumentQuery query, std::shared_ptr<const IProjectMigrationDocumentStage> stage) {
        nodes_.push_back(
            ProjectMigrationNode{.kind = ProjectMigrationNodeKind::ForEach, .query = std::move(query), .documentStage = std::move(stage)});
        return *this;
    }

    ProjectMigrationPipelineBuilder &ProjectMigrationPipelineBuilder::AddThen(std::shared_ptr<const IProjectMigrationStage> stage) {
        nodes_.push_back(ProjectMigrationNode{.kind = ProjectMigrationNodeKind::Then, .stage = std::move(stage)});
        return *this;
    }

    ProjectMigrationPipelineBuilder &ProjectMigrationPipelineBuilder::AddValidator(
        std::shared_ptr<const IProjectMigrationValidator> validator) {
        nodes_.push_back(ProjectMigrationNode{.kind = ProjectMigrationNodeKind::Validate, .validator = std::move(validator)});
        return *this;
    }

    Result<std::shared_ptr<const ProjectMigrationPipeline>> ProjectMigrationPipelineBuilder::Build() && {
        if (definitionId_.value.empty() || nodes_.empty())
            return Result<std::shared_ptr<const ProjectMigrationPipeline>>::Failure(
                MigrationError(ProjectErrors::MigrationPipelineInvalid,
                               "Migration pipeline requires a definition identity and at least one node."));

        std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> stageIds;
        bool hasValidator = false;
        for (std::size_t index = 0; index < nodes_.size(); ++index) {
            const ProjectMigrationNode &node = nodes_[index];
            MigrationStageDescriptor descriptor;
            if (node.kind == ProjectMigrationNodeKind::ForEach && node.documentStage)
                descriptor = node.documentStage->Describe();
            else if (node.kind == ProjectMigrationNodeKind::Then && node.stage)
                descriptor = node.stage->Describe();
            else if (node.kind == ProjectMigrationNodeKind::Validate && node.validator) {
                descriptor = node.validator->Describe();
                hasValidator = true;
                if (index + 1 != nodes_.size())
                    return Result<std::shared_ptr<const ProjectMigrationPipeline>>::Failure(
                        MigrationError(ProjectErrors::MigrationPipelineInvalid,
                                       "Validate must be the terminal node of one definition pipeline."));
            } else
                return Result<std::shared_ptr<const ProjectMigrationPipeline>>::Failure(
                    MigrationError(ProjectErrors::MigrationPipelineInvalid, "Migration pipeline contains a node without its typed stage."));

            if (descriptor.id.value.empty() || !stageIds.emplace(descriptor.id.value).second)
                return Result<std::shared_ptr<const ProjectMigrationPipeline>>::Failure(
                    MigrationError(ProjectErrors::MigrationPipelineInvalid,
                                   "Migration stage identities must be non-empty and unique within a definition."));
        }
        if (!hasValidator)
            return Result<std::shared_ptr<const ProjectMigrationPipeline>>::Failure(
                MigrationError(ProjectErrors::MigrationPipelineInvalid,
                               "Every migration definition requires a terminal validation barrier."));

        return Result<std::shared_ptr<const ProjectMigrationPipeline>>::Success(
            std::shared_ptr<const ProjectMigrationPipeline>(new ProjectMigrationPipeline(std::move(nodes_))));  // NOSONAR(cpp:S5950)
    }

    Result<ProjectMigrationRegistry> ProjectMigrationRegistry::Create(const std::span<const ProjectMigrationDefinition> definitions) {
        std::vector<ProjectMigrationDefinition> ordered(definitions.begin(), definitions.end());
        std::ranges::sort(ordered, [](const ProjectMigrationDefinition &left, const ProjectMigrationDefinition &right) {
            if (const auto targetOrder = CompareHoroVersions(left.to.value, right.to.value); targetOrder != std::strong_ordering::equal)
                return targetOrder == std::strong_ordering::less;
            if (const auto sourceOrder = CompareHoroVersions(left.from.value, right.from.value); sourceOrder != std::strong_ordering::equal)
                return sourceOrder == std::strong_ordering::less;
            if (left.kind != right.kind)
                return left.kind < right.kind;
            return left.id.value < right.id.value;
        });

        std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> identities;
        std::unordered_set<std::string, TransparentStringHash, std::equal_to<>> edges;
        for (const ProjectMigrationDefinition &definition : ordered) {
            if (definition.id.value.empty() || !definition.pipeline)
                return Result<ProjectMigrationRegistry>::Failure(
                    MigrationError(ProjectErrors::MigrationCatalogInvalid,
                                   "Every migration definition needs an identity and validated pipeline."));
            if (!identities.emplace(definition.id.value).second)
                return Result<ProjectMigrationRegistry>::Failure(
                    MigrationError(ProjectErrors::MigrationDuplicateIdentity, "Duplicate migration identity: " + definition.id.value));
            if (!edges.emplace(EdgeKey(definition)).second)
                return Result<ProjectMigrationRegistry>::Failure(
                    MigrationError(ProjectErrors::MigrationDuplicateEdge, "Duplicate migration edge: " + EdgeKey(definition)));
            if (!Before(definition.from, definition.to))
                return Result<ProjectMigrationRegistry>::Failure(
                    MigrationError(ProjectErrors::MigrationBackwardEdge, "Migration edges must move to a newer exact contract baseline."));
        }
        return Result<ProjectMigrationRegistry>::Success(ProjectMigrationRegistry(std::move(ordered)));
    }

    Result<ProjectMigrationPlan> ProjectMigrationRegistry::Plan(const ContractBaselineVersion &source,
                                                                const PersistentContractHash &sourceContract,
                                                                const ProjectMigrationSupportDescriptor &support) const {
        const std::string sourceText = FormatHoroVersion(source.value);
        const std::string targetText = FormatHoroVersion(support.target.value);
        LOG_DEBUG("application.project_migration.plan", "Planning migration source=%s target=%s catalog_definitions=%zu.",
                  sourceText.c_str(), targetText.c_str(), definitions_.size());
        if (Before(source, support.minimumMigratable) || Before(support.target, source))
            return Result<ProjectMigrationPlan>::Failure(
                MigrationError(ProjectErrors::MigrationPathMissing, "Source baseline is outside the target release support horizon."));
        if (!Same(source, support.target) && !support.targetValidator)
            return Result<ProjectMigrationPlan>::Failure(
                MigrationError(ProjectErrors::MigrationPipelineInvalid,
                               "Migration support descriptor requires a final target-contract validator."));

        ProjectMigrationPlan plan{.source = source, .target = support.target, .targetValidator = support.targetValidator};
        if (Same(source, support.target)) {
            if (sourceContract != support.targetContract)
                return Result<ProjectMigrationPlan>::Failure(
                    MigrationError(ProjectErrors::MigrationContractMismatch,
                                   "Source baseline contract does not match the target contract."));
            LOG_INFO("application.project_migration.plan", "Migration plan is a no-op source=%s target=%s.", sourceText.c_str(),
                     targetText.c_str());
            return Result<ProjectMigrationPlan>::Success(std::move(plan));
        }

        auto checkpointResult = CollectDeclaredCheckpoints(support, definitions_, source);
        if (checkpointResult.HasError())
            return Result<ProjectMigrationPlan>::Failure(checkpointResult.ErrorValue());

        if (const auto &declaredCheckpoints = checkpointResult.Value(); declaredCheckpoints.size() == 1) {
            const ProjectMigrationDefinition &checkpoint = *declaredCheckpoints.front();
            if (checkpoint.sourceContract != sourceContract || checkpoint.targetContract != support.targetContract)
                return Result<ProjectMigrationPlan>::Failure(
                    MigrationError(ProjectErrors::MigrationContractMismatch,
                                   "Declared checkpoint does not bind the expected source and target contracts."));
            plan.definitions.push_back(checkpoint);
        } else {
            auto chainResult = BuildSequentialChain(support, definitions_, source, sourceContract, plan);
            if (chainResult.HasError())
                return Result<ProjectMigrationPlan>::Failure(chainResult.ErrorValue());
        }

        if (const auto weightResult = ValidatePlanProvidersAndComputeWeight(plan, support); weightResult.HasError())
            return Result<ProjectMigrationPlan>::Failure(weightResult.ErrorValue());

        for (const ProjectMigrationDefinition &definition : plan.definitions)
            LOG_INFO("application.project_migration.plan", "Selected migration definition=%s.", definition.id.value.c_str());
        LOG_INFO("application.project_migration.plan", "Migration plan ready source=%s target=%s.", sourceText.c_str(), targetText.c_str());
        LOG_DEBUG("application.project_migration.plan", "Migration plan aggregate definitions=%zu estimated_weight=%llu.",
                  plan.definitions.size(), static_cast<unsigned long long>(plan.estimatedWeight));
        return Result<ProjectMigrationPlan>::Success(std::move(plan));
    }
}  // namespace Horo::Application
