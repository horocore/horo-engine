#include "HeadlessCookedDecisionAdmission.h"

#include "CommittedSceneAssets.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/AISceneActivation.h"
#include "Horo/Foundation/JobSystem.h"

#include <algorithm>
#include <map>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using Catalog = std::vector<AI::AiControllerDescriptor>;
        using Plan = std::shared_ptr<const AI::CookedDecisionPlan>;

        /** @brief Decodes a previously authenticated artifact against the live captured registry and exact declared family. */
        [[nodiscard]] Result<Plan> DecodePlan(const HeadlessCookedDecisionInput &input, const Assets::AssetCookArtifact &artifact) {
            auto compiled = AI::DecisionPlanCodec::DecodeCooked(artifact.payload, input.environment, input.limits);
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
        [[nodiscard]] Result<Plan> AcquirePlan(const HeadlessCookedDecisionInput &input, const Assets::AssetCookArtifact &artifact,
                                               std::map<Assets::AssetId, Plan> &plans, std::uint64_t &remaining) {
            if (const auto found = plans.find(artifact.id); found != plans.end())
                return Result<Plan>::Success(found->second);
            auto decoded = DecodePlan(input, artifact);
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
        [[nodiscard]] Result<void> BindPlan(AI::AiControllerDescriptor &descriptor, Plan plan) {
            const auto graphs = plan->Graphs();
            const auto root = std::ranges::find(graphs, plan->Root(), &AI::DecisionCompiledGraph::asset);
            if (plan->Root() != descriptor.decisionAsset || root == graphs.end() || !root->bindings ||
                root->bindings->BlackboardSchema() != descriptor.blackboardSchema || root->bindings->Kind() != descriptor.decisionKind)
                return Result<void>::Failure(MakeError(AI::AIErrors::ControllerDescriptorIncompatible,
                                                       "Cooked AI artifact does not match the declared root, family, or schema."));
            descriptor.decisionPlan = root->bindings;
            descriptor.cookedPlan = std::move(plan);
            return AI::ValidateAiControllerDescriptor(descriptor);
        }

        /** @brief Captures independent public descriptors; all validation occurs before the app constructs its Scene participant. */
        [[nodiscard]] Result<Catalog> Admit(const std::span<const AI::AiControllerDescriptor> catalog,
                                            const HeadlessCookedDecisionInput &input, const CancellationToken &cancellation) {
            Catalog admitted(catalog.begin(), catalog.end());
            std::ranges::sort(admitted, {}, &AI::AiControllerDescriptor::controller);
            if (std::ranges::adjacent_find(admitted, {}, &AI::AiControllerDescriptor::controller) != admitted.end())
                return Result<Catalog>::Failure(MakeError(AI::AIErrors::DescriptorConflict));

            std::map<Assets::AssetId, Plan> plans;
            auto remaining = input.limits.compilation.maximumCoreBytes;
            for (const auto &binding : input.bindings) {
                if (cancellation.IsCancellationRequested())
                    return Result<Catalog>::Failure(JobCancelled().ErrorValue());
                const auto descriptor = std::ranges::lower_bound(admitted, binding.controller, {}, &AI::AiControllerDescriptor::controller);
                if (!binding.controller.IsValid() || descriptor == admitted.end() || descriptor->controller != binding.controller)
                    return Result<Catalog>::Failure(MakeError(AI::AIErrors::ControllerDescriptorMissing));
                if (descriptor->cookedPlan)
                    return Result<Catalog>::Failure(MakeError(AI::AIErrors::DescriptorConflict));
                const auto *artifact = input.committed.Find(binding.artifact);
                if (!artifact)
                    return Result<Catalog>::Failure(
                        MakeError(AI::AIErrors::ControllerDescriptorMissing, "Selected cooked AI artifact is absent."));
                if (artifact->type.Value() != AI::DecisionAssetType(descriptor->decisionKind))
                    return Result<Catalog>::Failure(
                        MakeError(AI::AIErrors::ControllerDescriptorIncompatible, "Selected cooked AI artifact has the wrong family."));
                auto plan = AcquirePlan(input, *artifact, plans, remaining);
                if (plan.HasError())
                    return Result<Catalog>::Failure(plan.ErrorValue());
                if (auto bound = BindPlan(*descriptor, std::move(plan).Value()); bound.HasError())
                    return Result<Catalog>::Failure(bound.ErrorValue());
            }
            if (cancellation.IsCancellationRequested())
                return Result<Catalog>::Failure(JobCancelled().ErrorValue());
            return Result<Catalog>::Success(std::move(admitted));
        }
    }  // namespace

    /** @copydoc AdmitHeadlessCookedControllers */
    Result<Catalog> AdmitHeadlessCookedControllers(const std::span<const AI::AiControllerDescriptor> catalog,
                                                   const HeadlessCookedDecisionInput &input, const CancellationToken &cancellation) {
        if (catalog.size() > AI::MaximumAiSceneAgents || input.bindings.size() > catalog.size())
            return Result<Catalog>::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
        try {
            return Admit(catalog, input, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<Catalog>::Failure(MakeError(AI::AIErrors::AgentCapacityExceeded));
        }
    }
}  // namespace Horo::Application::Internal
