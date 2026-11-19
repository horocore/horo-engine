#include "DecisionCookState.h"

#include <algorithm>
#include <array>

namespace Horo::Application::DecisionCookDetail {
    namespace {
        template <typename T> Result<T> Invalid(const ErrorCodeDescriptor &error = DecisionCookErrors::Invalid) {
            return Result<T>::Failure(MakeError(error));
        }

        std::vector<AI::DecisionGraphAssetId> Calls(const AI::DecisionGraphSource &source) {
            std::vector<AI::DecisionGraphAssetId> calls;
            if (const auto *tree = std::get_if<std::shared_ptr<const AI::BehaviorTreeAsset>>(&source.body)) {
                for (const auto &node : (*tree)->Nodes()) {
                    if (node.call)
                        calls.push_back(node.call->asset);
                    if (node.nestedSubtree) {
                        auto id = AI::DecisionGraphAssetId::Create(node.nestedSubtree->graph.Value());
                        if (id.HasValue())
                            calls.push_back(id.Value());
                    }
                }
            } else if (const auto *machine = std::get_if<AI::StateMachineAssetDescriptor>(&source.body)) {
                for (const auto &state : machine->states)
                    if (state.call)
                        calls.push_back(state.call->asset);
            }
            return calls;
        }

        /** @brief Resolves a semantic graph identity in the canonical sorted capture. */
        std::optional<std::size_t> FindGraph(const State &state, AI::DecisionGraphAssetId id) {
            const auto found = std::ranges::lower_bound(state.inputs, id, {}, [](const auto &input) {
                return input.graph.asset;
            });
            if (found == state.inputs.end() || found->graph.asset != id)
                return {};
            return static_cast<std::size_t>(found - state.inputs.begin());
        }

        Result<std::vector<std::size_t>> Closure(const State &state, std::size_t root) {
            std::vector closure{root};
            for (std::size_t cursor = 0; cursor < closure.size(); ++cursor) {
                for (const auto id : Calls(state.inputs[closure[cursor]].graph)) {
                    const auto found = FindGraph(state, id);
                    if (!found)
                        return Invalid<std::vector<std::size_t>>(AI::AIErrors::DecisionAssetSubtreeMissing);
                    if (std::ranges::find(closure, *found) == closure.end())
                        closure.push_back(*found);
                    if (closure.size() > state.limits.maximumAssets)
                        return Invalid<std::vector<std::size_t>>(AI::AIErrors::DecisionAssetLimitExceeded);
                }
            }
            return Result<std::vector<std::size_t>>::Success(std::move(closure));
        }

        Error CompilationError(const AI::DecisionAssetValidationReport &report) {
            auto error = MakeError(DecisionCookErrors::Invalid);
            const auto diagnostics = report.Diagnostics();
            if (diagnostics.empty())
                return error;
            error.code = diagnostics[0].code;
            error.domain = ErrorDomainId{"horo.ai"};
            error.message = diagnostics[0].message;
            for (const auto &diagnostic : diagnostics)
                error.diagnostics.emplace_back(DiagnosticCode{std::string{diagnostic.code.Value()}}, diagnostic.severity,
                                               diagnostic.message, diagnostic.source, diagnostic.path);
            return error;
        }

        /** @brief Resolves the actual registered query and validates its supported output against a compiled node binding. */
        Result<Assets::AssetId> QueryDependency(const State &state, const AI::BehaviorTreeQueryOutput &output,
                                                std::span<const AI::DecisionPlanBlackboardBinding> keys) {
            using Dependency = Result<Assets::AssetId>;
            const auto query = std::ranges::lower_bound(state.queries, output.query, {}, &QueryArtifact::query);
            if (query == state.queries.end() || query->query != output.query)
                return Dependency::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
            const auto &result = query->result;
            if (result.id != output.result || result.version != output.resultVersion || result.itemType != output.item)
                return Dependency::Failure(MakeError(AI::AIErrors::EnvironmentQueryVersionIncompatible));
            if (query->source) {
                const auto plan = AI::EnvironmentQueryPlan::Compile(*query->source, *state.querySchema);
                if (plan.HasError())
                    return Dependency::Failure(plan.ErrorValue());
            }
            const auto key = std::ranges::find(keys, output.key, &AI::DecisionPlanBlackboardBinding::key);
            if (key == keys.end())
                return Dependency::Failure(MakeError(AI::AIErrors::BlackboardValueTypeMismatch));
            const bool acceptsPoint = query->kind == AI::QueryItemKind::Point && key->kind == AI::BlackboardValueKind::WorldCoordinate;
            const bool acceptsActor = query->kind == AI::QueryItemKind::Actor && key->kind == AI::BlackboardValueKind::EntityReference;
            if (!acceptsPoint && !acceptsActor)
                return Dependency::Failure(MakeError(AI::AIErrors::BlackboardValueTypeMismatch));
            return Dependency::Success(query->asset);
        }

        /** @brief Adds typed query references, including those in nested graphs, to the existing asset dependency set. */
        Result<void> AppendQueryDependencies(const State &state, const AI::DecisionCompiledGraph &graph,
                                             std::vector<Assets::AssetId> &dependencies) {
            for (const auto &config : graph.providerProperties) {
                for (const auto &property : config.properties) {
                    const auto *output = std::get_if<AI::BehaviorTreeQueryOutput>(&property.value);
                    if (!output)
                        continue;
                    const auto dependency = QueryDependency(state, *output, graph.bindings->BindingsForNode(config.node));
                    if (dependency.HasError())
                        return Result<void>::Failure(dependency.ErrorValue());
                    dependencies.push_back(dependency.Value());
                }
            }
            return Result<void>::Success();
        }

        Result<Prepared> Prepare(const State &state, std::size_t index) {
            auto closure = Closure(state, index);
            if (closure.HasError())
                return Result<Prepared>::Failure(closure.ErrorValue());
            std::vector<AI::DecisionGraphSource> sources;
            std::vector<Assets::AssetId> dependencies;
            for (const auto member : closure.Value()) {
                sources.push_back(state.inputs[member].graph);
                if (member != index)
                    dependencies.push_back(state.inputs[member].asset);
            }
            const auto &input = state.inputs[index];
            auto compiled = AI::DecisionGraphCompiler::Compile(input.graph.asset, sources, state.environment, state.limits);
            if (compiled.HasError())
                return Result<Prepared>::Failure(compiled.ErrorValue());
            if (!compiled.Value().plan)
                return Result<Prepared>::Failure(CompilationError(compiled.Value().validation));
            auto payload = AI::DecisionPlanCodec::EncodeCooked(*compiled.Value().plan, {.compilation = state.limits});
            if (payload.HasError())
                return Result<Prepared>::Failure(payload.ErrorValue());
            for (const auto &graph : compiled.Value().plan->Graphs()) {
                const auto schema = graph.bindings->BlackboardSchema();
                const auto found = std::ranges::find_if(state.schemas, [&schema](const auto &record) {
                    return record.identity == schema->Identity() && record.version == schema->Version();
                });
                if (found != state.schemas.end())
                    dependencies.push_back(found->asset);
                if (auto attached = AppendQueryDependencies(state, graph, dependencies); attached.HasError())
                    return Result<Prepared>::Failure(attached.ErrorValue());
            }
            std::ranges::sort(dependencies);
            const auto duplicates = std::ranges::unique(dependencies);
            dependencies.erase(duplicates.begin(), duplicates.end());
            return Result<Prepared>::Success(
                {input.asset, input.type, input.sourceDigest, std::move(payload).Value(), std::move(dependencies)});
        }

        void Word(Sha256Builder &builder, std::uint64_t value) {
            std::array<std::byte, 8> bytes{};
            for (auto &byte : bytes) {
                byte = static_cast<std::byte>(value & 255U);
                value >>= 8U;
            }
            static_cast<void>(builder.Update(bytes));
        }

        Sha256Digest Settings(const State &state) {
            Sha256Builder builder;
            constexpr std::string_view domain = "horo.ai.cook.v1";
            static_cast<void>(builder.Update(std::as_bytes(std::span{domain.data(), domain.size()})));
            Word(builder, state.target.Value().size());
            static_cast<void>(builder.Update(std::as_bytes(std::span{state.target.Value()})));
            Word(builder, state.prepared.size());
            for (const auto &prepared : state.prepared) {
                static_cast<void>(builder.Update(std::as_bytes(std::span{prepared.asset.Bytes()})));
                const auto digest = ComputeSha256(std::as_bytes(std::span{prepared.payload}));
                static_cast<void>(builder.Update(std::as_bytes(std::span{digest.bytes})));
            }
            return builder.Finalize();
        }

        /** @brief Follows reverse call edges from every graph admitting the selected schema version. */
        std::vector<std::size_t> AffectedRoots(const State &state, AI::BlackboardSchemaId schema, std::uint32_t version) {
            std::vector<std::vector<std::size_t>> callers(state.inputs.size());
            std::vector<std::size_t> affected;
            std::vector<bool> visited(state.inputs.size());
            for (std::size_t index = 0; index < state.inputs.size(); ++index) {
                const auto &graph = state.inputs[index].graph;
                if (graph.blackboard == schema && graph.blackboardVersion.Contains(version)) {
                    affected.push_back(index);
                    visited[index] = true;
                }
                for (const auto id : Calls(graph)) {
                    if (const auto target = FindGraph(state, id))
                        callers[*target].push_back(index);
                }
            }
            for (std::size_t current = 0; current < affected.size(); ++current) {
                for (const auto caller : callers[affected[current]]) {
                    if (visited[caller])
                        continue;
                    visited[caller] = true;
                    affected.push_back(caller);
                }
            }
            std::ranges::sort(affected);
            return affected;
        }
    }  // namespace

    Result<void> PrepareRoots(State &state, const CancellationToken &cancellation) {
        auto bytes = state.preparedDependencyBytes;
        for (std::size_t index = 0; index < state.inputs.size(); ++index) {
            if (cancellation.IsCancellationRequested())
                return Invalid<void>(DecisionCookErrors::Cancelled);
            auto prepared = Prepare(state, index);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (prepared.Value().payload.size() > state.maximumPreparedBytes - bytes)
                return Invalid<void>(AI::AIErrors::DecisionAssetLimitExceeded);
            bytes += prepared.Value().payload.size();
            state.prepared.push_back(std::move(prepared).Value());
        }
        std::ranges::sort(state.prepared, {}, &Prepared::asset);
        state.settings = Settings(state);
        return Verify(state, cancellation);
    }

    /** @copydoc ValidateSchemaRoots */
    Result<std::size_t> ValidateSchemaRoots(const State &state, AI::BlackboardSchemaId schema, std::uint32_t version,
                                            const CancellationToken &cancellation) {
        using Count = Result<std::size_t>;
        const auto roots = AffectedRoots(state, schema, version);
        for (const auto index : roots) {
            if (cancellation.IsCancellationRequested())
                return Invalid<std::size_t>(DecisionCookErrors::Cancelled);
            if (const auto prepared = Prepare(state, index); prepared.HasError())
                return Count::Failure(prepared.ErrorValue());
        }
        if (auto current = roots.empty() ? state.files->VerifyUnchanged(cancellation) : Verify(state, cancellation); current.HasError())
            return Count::Failure(current.ErrorValue());
        return Count::Success(roots.size());
    }
}  // namespace Horo::Application::DecisionCookDetail
