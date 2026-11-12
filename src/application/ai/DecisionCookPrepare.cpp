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
            } else {
                for (const auto &state : std::get<AI::StateMachineAssetDescriptor>(source.body).states)
                    if (state.call)
                        calls.push_back(state.call->asset);
            }
            return calls;
        }

        Result<std::vector<std::size_t>> Closure(const State &state, std::size_t root) {
            std::vector<std::size_t> closure{root};
            for (std::size_t cursor = 0; cursor < closure.size(); ++cursor) {
                for (const auto id : Calls(state.inputs[closure[cursor]].graph)) {
                    const auto found = std::ranges::lower_bound(state.inputs, id, {}, [](const auto &input) {
                        return input.graph.asset;
                    });
                    if (found == state.inputs.end() || found->graph.asset != id)
                        return Invalid<std::vector<std::size_t>>(AI::AIErrors::DecisionAssetSubtreeMissing);
                    if (const auto index = static_cast<std::size_t>(found - state.inputs.begin());
                        std::ranges::find(closure, index) == closure.end())
                        closure.push_back(index);
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
            std::ranges::sort(dependencies);
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
    }  // namespace

    Result<void> PrepareRoots(State &state, const CancellationToken &cancellation) {
        std::uint64_t bytes{};
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
}  // namespace Horo::Application::DecisionCookDetail
