#include "DecisionCompilationBuilder.h"

#include <algorithm>
#include <new>

namespace Horo::AI::Detail {
    namespace {
        template <typename T> Result<T> Invalid(const ErrorCodeDescriptor &code = AIErrors::DecisionAssetSchemaInvalid) {
            return Result<T>::Failure(MakeError(code));
        }

        struct Builder final {
            const DecisionCompilationEnvironment &environment;
            const DecisionCompilationLimits &limits;
            DecisionClosureAdmission admission;
            DecisionCompilationCandidate candidate;
            std::vector<LoweredDecisionGraph> lowered;
            std::vector<DecisionAssetDescriptor> catalog;
            std::vector<DecisionNodeDescriptor> descriptors;

            Result<void> CaptureSources(DecisionGraphAssetId root, std::span<const DecisionGraphSource> sources) {
                if (sources.empty() || sources.size() > limits.maximumAssets || sources.size() > 128)
                    return Invalid<void>(AIErrors::DecisionAssetLimitExceeded);
                candidate.sources.assign(sources.begin(), sources.end());
                std::ranges::sort(candidate.sources, {}, &DecisionGraphSource::asset);
                if (std::ranges::adjacent_find(candidate.sources, {}, &DecisionGraphSource::asset) != candidate.sources.end())
                    return Invalid<void>(AIErrors::DecisionAssetSubtreeAmbiguous);
                const auto found = std::ranges::find(candidate.sources, root, &DecisionGraphSource::asset);
                if (found == candidate.sources.end())
                    return Invalid<void>(AIErrors::DecisionAssetSubtreeMissing);
                candidate.root = static_cast<std::size_t>(found - candidate.sources.begin());
                candidate.graphs.reserve(sources.size());
                return LowerSources();
            }

            Result<void> LowerSources() {
                std::size_t total{};
                for (const auto &source : candidate.sources) {
                    auto result = LowerDecisionGraph(source);
                    if (result.HasError())
                        return Result<void>::Failure(result.ErrorValue());
                    if (result.Value().metadata.nodes.size() > limits.maximumNodes - std::min(total, limits.maximumNodes))
                        return Invalid<void>(AIErrors::DecisionAssetLimitExceeded);
                    total += result.Value().metadata.nodes.size();
                    catalog.push_back(result.Value().metadata);
                    lowered.push_back(std::move(result).Value());
                }
                for (const auto &contract : environment.nodes->Contracts())
                    descriptors.push_back(contract.descriptor);
                return Result<void>::Success();
            }

            Result<void> CompileGraph(std::size_t index) {
                const auto &source = lowered[index];
                auto metadata = DecisionAssetCompiler::Compile(source.metadata, catalog, descriptors, environment.schemas);
                if (metadata.HasError())
                    return Result<void>::Failure(metadata.ErrorValue());
                for (const auto &diagnostic : metadata.Value().validation.Diagnostics()) {
                    if (candidate.diagnostics.size() == DecisionAssetValidationHardLimits::Diagnostics)
                        return Invalid<void>(AIErrors::DecisionAssetLimitExceeded);
                    candidate.diagnostics.push_back(diagnostic);
                }
                if (!metadata.Value().plan)
                    return Result<void>::Success();
                if (auto providers = ValidateDecisionProviders(source, environment); providers.HasError())
                    return providers;
                DecisionCompiledGraph graph{.asset = source.metadata.asset,
                                            .schemaVersion = source.metadata.schemaVersion,
                                            .executionTask = candidate.sources[index].executionTask,
                                            .bindings = metadata.Value().plan,
                                            .providerProperties = source.properties};
                if (auto executable = CompileFamily(graph, source); executable.HasError())
                    return executable;
                candidate.graphs.push_back(std::move(graph));
                return Result<void>::Success();
            }

            Result<void> CompileFamily(DecisionCompiledGraph &graph, const LoweredDecisionGraph &source) const {
                if (source.utility) {
                    auto plan = CookedUtilityPlan::CompileClosure(admission, graph.bindings, *source.utility);
                    if (plan.HasError())
                        return Result<void>::Failure(plan.ErrorValue());
                    graph.execution = std::move(plan).Value();
                } else if (source.stateMachine) {
                    auto plan = CookedStateMachinePlan::CompileClosure(admission, graph.bindings, *source.stateMachine);
                    if (plan.HasError())
                        return Result<void>::Failure(plan.ErrorValue());
                    graph.execution = std::move(plan).Value();
                } else {
                    auto plan =
                        BehaviorTreeExecutionPlan::CompileClosure(admission, graph.bindings, source.root, source.controls, source.services);
                    if (plan.HasError())
                        return Result<void>::Failure(plan.ErrorValue());
                    graph.execution = std::move(plan).Value();
                }
                return Result<void>::Success();
            }

            Result<void> ResolveCalls() {
                const auto schema = candidate.graphs[candidate.root].bindings->BlackboardSchema();
                for (std::size_t index = 0; index < candidate.graphs.size(); ++index) {
                    auto &graph = candidate.graphs[index];
                    if (graph.bindings->BlackboardSchema() != schema)
                        return Invalid<void>(AIErrors::DecisionAssetSchemaIncompatible);
                    graph.calls.reserve(lowered[index].calls.size());
                    for (const auto &[node, call] : lowered[index].calls) {
                        const auto callee = std::ranges::find(candidate.graphs, call.asset, &DecisionCompiledGraph::asset);
                        if (callee == candidate.graphs.end())
                            return Invalid<void>(AIErrors::DecisionAssetSubtreeMissing);
                        graph.calls.push_back({node, static_cast<std::size_t>(callee - candidate.graphs.begin())});
                    }
                    std::ranges::sort(graph.calls, {}, &DecisionCallSite::node);
                }
                if (auto reachable = RejectUnreachable(); reachable.HasError())
                    return reachable;
                return MeasureGraphs();
            }

            Result<void> MeasureGraphs() {
                for (auto &graph : candidate.graphs) {
                    auto storage = MeasureDecisionGraph(graph, environment);
                    if (storage.HasError())
                        return Result<void>::Failure(storage.ErrorValue());
                    graph.localStorage = storage.Value();
                }
                return Result<void>::Success();
            }

            Result<void> RejectUnreachable() const {
                std::vector<bool> visited(candidate.graphs.size());
                std::vector<std::size_t> pending{candidate.root};
                visited[candidate.root] = true;
                for (std::size_t cursor = 0; cursor < pending.size(); ++cursor) {
                    for (const auto &call : candidate.graphs[pending[cursor]].calls) {
                        if (visited[call.calleePlan])
                            continue;
                        visited[call.calleePlan] = true;
                        pending.push_back(call.calleePlan);
                    }
                }
                if (pending.size() != candidate.graphs.size())
                    return Invalid<void>(AIErrors::BehaviorTreeTopologyInvalid);
                return Result<void>::Success();
            }

            Result<DecisionCompilationCandidate> Run(DecisionGraphAssetId root, std::span<const DecisionGraphSource> sources) {
                if (auto captured = CaptureSources(root, sources); captured.HasError())
                    return Result<DecisionCompilationCandidate>::Failure(captured.ErrorValue());
                for (std::size_t index = 0; index < lowered.size(); ++index)
                    if (auto compiled = CompileGraph(index); compiled.HasError())
                        return Result<DecisionCompilationCandidate>::Failure(compiled.ErrorValue());
                if (candidate.diagnostics.empty()) {
                    if (auto resolved = ResolveCalls(); resolved.HasError())
                        return Result<DecisionCompilationCandidate>::Failure(resolved.ErrorValue());
                }
                return Result<DecisionCompilationCandidate>::Success(std::move(candidate));
            }
        };
    }  // namespace

    Result<DecisionCompilationCandidate> BuildDecisionCompilation(DecisionGraphAssetId root, std::span<const DecisionGraphSource> sources,
                                                                  const DecisionCompilationEnvironment &environment,
                                                                  const DecisionCompilationLimits &limits,
                                                                  DecisionClosureAdmission admission) {
        if (!environment.nodes || !environment.nodes->CanUse())
            return Invalid<DecisionCompilationCandidate>(AIErrors::RuntimeUnavailable);
        return Builder{environment, limits, admission}.Run(root, sources);
    }
}  // namespace Horo::AI::Detail
