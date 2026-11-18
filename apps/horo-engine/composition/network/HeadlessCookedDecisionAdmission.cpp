#include "HeadlessCookedDecisionAdmission.h"

#include "CommittedSceneAssets.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AISceneActivation.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <algorithm>
#include <map>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using Catalog = std::vector<AI::AiControllerDescriptor>;
        using Plan = std::shared_ptr<const AI::CookedDecisionPlan>;

        struct ControllerSelection final {
            Assets::AssetId artifact;
            const AI::AiControllerComponent *authored{};
        };

        using Selections = std::map<AI::ControllerTypeId, ControllerSelection>;
        using Descriptors = std::map<AI::ControllerTypeId, AI::AiControllerDescriptor>;

        /** @brief The decision/schema contract is shared by callers of one controller; enable/capability flags are per agent. */
        [[nodiscard]] bool MatchesDecision(const AI::AiControllerComponent &left, const AI::AiControllerComponent &right) noexcept {
            return left.decisionAsset == right.decisionAsset && left.decisionKind == right.decisionKind &&
                   left.blackboardSchema == right.blackboardSchema;
        }

        /** @brief Resolves explicit project asset selections against actual Scene declarations before decoding artifacts. */
        Result<Selections> SelectControllers(const Runtime::RuntimeSceneDefinition &scene,
                                             const std::span<const HeadlessCookedDecisionBinding> bindings,
                                             const CancellationToken &cancellation) {
            Selections selections;
            for (const auto &binding : bindings) {
                if (!binding.controller.IsValid() || !binding.artifact.IsValid())
                    return Result<Selections>::Failure(MakeError(AI::AIErrors::ControllerDescriptorMissing,
                                                                 "Cooked AI selection requires exact controller and asset identities."));
                if (!selections.emplace(binding.controller, ControllerSelection{binding.artifact}).second)
                    return Result<Selections>::Failure(
                        MakeError(AI::AIErrors::DescriptorConflict, "A controller has multiple cooked asset selections."));
            }
            if (!selections.empty()) {
                for (const auto &entity : scene.Entities()) {
                    if (cancellation.IsCancellationRequested())
                        return Result<Selections>::Failure(JobCancelled().ErrorValue());
                    const auto &controller = entity.components.aiController;
                    if (!controller)
                        continue;
                    const auto found = selections.find(controller->controller);
                    if (found == selections.end())
                        continue;
                    auto &selected = found->second.authored;
                    if (selected && !MatchesDecision(*selected, *controller))
                        return Result<Selections>::Failure(MakeError(AI::AIErrors::DescriptorConflict,
                                                                     "One selected controller has conflicting Scene decision bindings."));
                    selected = &*controller;
                }
            }
            return Result<Selections>::Success(std::move(selections));
        }

        /** @brief Uses an explicit descriptor or the exact authored binding; does not manufacture optional provider metadata. */
        Result<AI::AiControllerDescriptor *> ResolveDescriptor(Descriptors &descriptors, const AI::ControllerTypeId identity,
                                                               const AI::AiControllerComponent *authored) {
            using Resolved = Result<AI::AiControllerDescriptor *>;
            if (const auto existing = descriptors.find(identity); existing != descriptors.end()) {
                auto &descriptor = existing->second;
                if (descriptor.cookedPlan)
                    return Resolved::Failure(MakeError(AI::AIErrors::DescriptorConflict));
                if (authored && (descriptor.decisionAsset != authored->decisionAsset || descriptor.decisionKind != authored->decisionKind))
                    return Resolved::Failure(MakeError(AI::AIErrors::ControllerDescriptorIncompatible));
                return Resolved::Success(&descriptor);
            }
            if (!authored)
                return Resolved::Failure(MakeError(AI::AIErrors::ControllerDescriptorMissing,
                                                   "A cooked controller selection needs a Scene declaration or an explicit descriptor."));
            if (descriptors.size() >= AI::MaximumAiSceneAgents)
                return Resolved::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
            auto [inserted, _] = descriptors.emplace(identity, AI::AiControllerDescriptor{.controller = identity,
                                                                                          .decisionAsset = authored->decisionAsset,
                                                                                          .decisionKind = authored->decisionKind});
            return Resolved::Success(&inserted->second);
        }

        /** @brief Materializes typed schema definitions from the exact committed generation when none were supplied explicitly. */
        Result<AI::DecisionCompilationEnvironment> ResolveEnvironment(const HeadlessCookedDecisionInput &input,
                                                                      const CancellationToken &cancellation) {
            using Environment = Result<AI::DecisionCompilationEnvironment>;
            auto environment = input.environment;
            if (!environment.schemas.empty() || input.bindings.empty())
                return Environment::Success(std::move(environment));

            for (const auto &artifact : input.committed.Artifacts()) {
                if (cancellation.IsCancellationRequested())
                    return Environment::Failure(JobCancelled().ErrorValue());
                if (artifact.type.Value() != AI::BlackboardSchemaAssetType)
                    continue;
                if (environment.schemas.size() >= AI::DecisionAssetValidationHardLimits::Schemas)
                    return Environment::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
                auto schema = AI::BlackboardSchemaCodec::Decode(artifact.payload);
                if (schema.HasError())
                    return Environment::Failure(schema.ErrorValue());
                environment.schemas.emplace_back(std::make_shared<const AI::BlackboardSchema>(std::move(schema).Value()));
            }
            if (environment.schemas.empty())
                return Environment::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaMissing));
            const auto identity = [](const auto &schema) {
                return std::pair{schema->Identity(), schema->Version()};
            };
            std::ranges::sort(environment.schemas, {}, identity);
            if (std::ranges::adjacent_find(environment.schemas, {}, identity) != environment.schemas.end())
                return Environment::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaAmbiguous));
            return Environment::Success(std::move(environment));
        }

        /** @brief Decodes a previously authenticated artifact against the live captured registry and exact declared family. */
        [[nodiscard]] Result<Plan> DecodePlan(const AI::DecisionCompilationEnvironment &environment, AI::DecisionCodecLimits limits,
                                              const Assets::AssetCookArtifact &artifact) {
            auto compiled = AI::DecisionPlanCodec::DecodeCooked(artifact.payload, environment, limits);
            if (compiled.HasError())
                return Result<Plan>::Failure(compiled.ErrorValue());
            if (!compiled.Value().plan) {
                auto error = MakeError(AI::AIErrors::ControllerDescriptorIncompatible);
                for (const auto &finding : compiled.Value().validation.Diagnostics())
                    error.diagnostics.emplace_back(DiagnosticCode{std::string{finding.code.Value()}}, finding.severity, finding.message,
                                                   finding.source, finding.path);
                return Result<Plan>::Failure(std::move(error));
            }
            return Result<Plan>::Success(std::move(compiled).Value().plan);
        }

        /** @brief Retains one plan per full artifact identity and bounds aggregate compiled storage. */
        [[nodiscard]] Result<Plan> AcquirePlan(const AI::DecisionCompilationEnvironment &environment, AI::DecisionCodecLimits limits,
                                               const Assets::AssetCookArtifact &artifact, std::map<Assets::AssetId, Plan> &plans,
                                               std::uint64_t &remaining) {
            if (const auto found = plans.find(artifact.id); found != plans.end())
                return Result<Plan>::Success(found->second);
            auto decoded = DecodePlan(environment, limits, artifact);
            if (decoded.HasError())
                return decoded;
            const auto bytes = decoded.Value()->Storage().compiledBytes;
            if (bytes > remaining)
                return Result<Plan>::Failure(
                    MakeError(AI::AIErrors::AgentCapacityExceeded, "Cooked AI startup exceeded its compiled storage budget."));
            remaining -= bytes;
            plans.emplace(artifact.id, decoded.Value());
            return decoded;
        }

        /** @brief Replaces projected binding metadata only after matching the existing declared root and exact schema. */
        [[nodiscard]] Result<void> BindPlan(AI::AiControllerDescriptor &descriptor, Plan plan, const AI::AiControllerComponent *authored) {
            const auto graphs = plan->Graphs();
            const auto root = std::ranges::find(graphs, plan->Root(), &AI::DecisionCompiledGraph::asset);
            if (plan->Root() != descriptor.decisionAsset || root == graphs.end() || !root->bindings ||
                (descriptor.blackboardSchema && root->bindings->BlackboardSchema() != descriptor.blackboardSchema) ||
                root->bindings->Kind() != descriptor.decisionKind)
                return Result<void>::Failure(MakeError(AI::AIErrors::ControllerDescriptorIncompatible,
                                                       "Cooked AI artifact does not match the declared root, family, or schema."));
            const auto schema = root->bindings->BlackboardSchema();
            if (authored && (!schema || schema->Identity() != authored->blackboardSchema))
                return Result<void>::Failure(MakeError(AI::AIErrors::ControllerDescriptorIncompatible,
                                                       "Cooked AI schema does not match the selected Scene controller."));
            descriptor.blackboardSchema = schema;
            descriptor.decisionPlan = root->bindings;
            descriptor.cookedPlan = std::move(plan);
            return AI::ValidateAiControllerDescriptor(descriptor);
        }

        /** @brief Captures independent public descriptors; all validation occurs before the app constructs its Scene participant. */
        [[nodiscard]] Result<Catalog> Admit(const std::span<const AI::AiControllerDescriptor> catalog,
                                            const Runtime::RuntimeSceneDefinition &scene, const HeadlessCookedDecisionInput &input,
                                            const CancellationToken &cancellation) {
            Descriptors descriptors;
            for (const auto &descriptor : catalog)
                if (!descriptors.emplace(descriptor.controller, descriptor).second)
                    return Result<Catalog>::Failure(MakeError(AI::AIErrors::DescriptorConflict));

            const auto selected = SelectControllers(scene, input.bindings, cancellation);
            if (selected.HasError())
                return Result<Catalog>::Failure(selected.ErrorValue());
            const auto environment = ResolveEnvironment(input, cancellation);
            if (environment.HasError())
                return Result<Catalog>::Failure(environment.ErrorValue());
            std::map<Assets::AssetId, Plan> plans;
            auto remaining = input.limits.compilation.maximumCoreBytes;
            for (const auto &[identity, binding] : selected.Value()) {
                if (cancellation.IsCancellationRequested())
                    return Result<Catalog>::Failure(JobCancelled().ErrorValue());
                auto descriptor = ResolveDescriptor(descriptors, identity, binding.authored);
                if (descriptor.HasError())
                    return Result<Catalog>::Failure(descriptor.ErrorValue());
                const auto *artifact = input.committed.Find(binding.artifact);
                if (!artifact)
                    return Result<Catalog>::Failure(
                        MakeError(AI::AIErrors::ControllerDescriptorMissing, "Selected cooked AI artifact is absent."));
                if (artifact->type.Value() != AI::DecisionAssetType(descriptor.Value()->decisionKind))
                    return Result<Catalog>::Failure(
                        MakeError(AI::AIErrors::ControllerDescriptorIncompatible, "Selected cooked AI artifact has the wrong family."));
                auto plan = AcquirePlan(environment.Value(), input.limits, *artifact, plans, remaining);
                if (plan.HasError())
                    return Result<Catalog>::Failure(plan.ErrorValue());
                if (auto bound = BindPlan(*descriptor.Value(), std::move(plan).Value(), binding.authored); bound.HasError())
                    return Result<Catalog>::Failure(bound.ErrorValue());
            }
            if (cancellation.IsCancellationRequested())
                return Result<Catalog>::Failure(JobCancelled().ErrorValue());

            Catalog admitted;
            admitted.reserve(descriptors.size());
            for (auto &[identity, descriptor] : descriptors)
                admitted.push_back(std::move(descriptor));
            return Result<Catalog>::Success(std::move(admitted));
        }
    }  // namespace

    /** @copydoc AdmitHeadlessCookedControllers */
    Result<Catalog> AdmitHeadlessCookedControllers(const std::span<const AI::AiControllerDescriptor> catalog,
                                                   const Runtime::RuntimeSceneDefinition &scene, const HeadlessCookedDecisionInput &input,
                                                   const CancellationToken &cancellation) {
        if (catalog.size() > AI::MaximumAiSceneAgents || input.bindings.size() > AI::MaximumAiSceneAgents)
            return Result<Catalog>::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
        try {
            return Admit(catalog, scene, input, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<Catalog>::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
        }
    }
}  // namespace Horo::Application::Internal
