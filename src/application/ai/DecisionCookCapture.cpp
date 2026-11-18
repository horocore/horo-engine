#include "DecisionCookState.h"

#include <algorithm>
#include <array>

namespace Horo::Application::DecisionCookDetail {
    namespace {
        void RebaseRequirements(std::vector<AI::DecisionBlackboardRequirement> &requirements, const SourceLocation &location) {
            for (auto &requirement : requirements)
                requirement.source = location;
        }

        void RebaseAction(std::optional<AI::StateMachineProviderAction> &action, const SourceLocation &location) {
            if (!action)
                return;
            action->binding.source = location;
            RebaseRequirements(action->binding.requirements, location);
        }

        Result<void> RebaseTree(AI::DecisionGraphSource &source, const SourceLocation &location) {
            const auto &tree = std::get<std::shared_ptr<const AI::BehaviorTreeAsset>>(source.body);
            auto data = tree->Data();
            for (auto &node : data.nodes) {
                node.source = location;
                RebaseRequirements(node.requirements, location);
            }
            auto captured = AI::BehaviorTreeAsset::Capture(std::move(data));
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            source.body = std::make_shared<const AI::BehaviorTreeAsset>(std::move(captured).Value());
            return Result<void>::Success();
        }

        void RebaseMachine(AI::StateMachineAssetDescriptor &machine, const SourceLocation &location) {
            for (auto &state : machine.states) {
                state.source = location;
                RebaseAction(state.entryProvider, location);
                RebaseAction(state.updateProvider, location);
                RebaseAction(state.exitProvider, location);
            }
            for (auto &transition : machine.transitions) {
                transition.location = location;
                RebaseRequirements(transition.requirements, location);
            }
        }

        /** @brief Rebase the typed Utility root and provider diagnostics to the authoritative source path. */
        void RebaseUtility(AI::UtilityDecisionSource &utility, const SourceLocation &location) {
            RebaseRequirements(utility.requirements, location);
            for (auto &option : utility.options) {
                option.provider.source = location;
                RebaseRequirements(option.provider.requirements, location);
            }
        }

    }  // namespace

    /** @copydoc DecodeRegisteredDecisionGraphSource */
    Result<AI::DecisionGraphSource> DecodeRegisteredDecisionGraphSource(const Assets::AssetCookPinnedSource &file,
                                                                        AI::DecisionCompilationLimits limits) {
        AI::DecisionGraphAssetId root;
        auto decoded = AI::DecisionPlanCodec::DecodeSource(file.bytes, root, {.compilation = limits});
        if (decoded.HasError())
            return Result<AI::DecisionGraphSource>::Failure(decoded.ErrorValue());
        if (decoded.Value().size() != 1 || decoded.Value().front().asset != root)
            return Result<AI::DecisionGraphSource>::Failure(MakeError(DecisionCookErrors::Invalid));
        auto graphs = std::move(decoded).Value();
        auto source = std::move(graphs.front());
        constexpr std::array types{DecisionBehaviorTreeSourceType, DecisionStateMachineSourceType, DecisionUtilitySourceType};
        const bool tree = source.body.index() == 0;
        if (file.record.type.Value() != types[source.body.index()])
            return Result<AI::DecisionGraphSource>::Failure(MakeError(DecisionCookErrors::Invalid));
        source.location = {std::string{file.record.sourcePath.String()}, 0, 0};
        if (tree) {
            if (auto rebased = RebaseTree(source, source.location); rebased.HasError())
                return Result<AI::DecisionGraphSource>::Failure(rebased.ErrorValue());
        } else if (auto *machine = std::get_if<AI::StateMachineAssetDescriptor>(&source.body))
            RebaseMachine(*machine, source.location);
        else
            RebaseUtility(std::get<AI::UtilityDecisionSource>(source.body), source.location);
        return Result<AI::DecisionGraphSource>::Success(std::move(source));
    }

    namespace {
        bool SameRecord(const Assets::AssetRecord &left, const Assets::AssetRecord &right) {
            return left.id == right.id && left.type == right.type && left.sourcePath.String() == right.sourcePath.String() &&
                   left.metadataPath.String() == right.metadataPath.String();
        }

        Result<void> CaptureRecord(State &state, const Assets::AssetRecord &record, const CancellationToken &cancellation) {
            if (state.inputs.size() == state.limits.maximumAssets)
                return Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
            const auto *file = state.files->Find(record.id);
            if (!file || !SameRecord(record, file->record))
                return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
            auto source = DecodeRegisteredDecisionGraphSource(*file, state.limits);
            if (source.HasError())
                return Result<void>::Failure(source.ErrorValue());
            state.inputs.emplace_back(record.id, record.type, file->sourceDigest, std::move(source).Value());
            return Result<void>::Success();
        }

        /** @brief Captures a real registry-backed schema and stages its canonical immutable cooked payload. */
        Result<void> CaptureSchema(State &state, const Assets::AssetRecord &record, const CancellationToken &cancellation) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
            if (state.environment.schemas.size() >= AI::DecisionAssetValidationHardLimits::Schemas)
                return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
            const auto *file = state.files->Find(record.id);
            if (!file || !SameRecord(record, file->record))
                return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
            auto decoded = AI::BlackboardSchemaSourceCodec::Decode(file->bytes);
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            auto schema = std::make_shared<const AI::BlackboardSchema>(std::move(decoded).Value().schema);
            if (std::ranges::any_of(state.environment.schemas, [&schema](const auto &existing) {
                return existing && existing->Identity() == schema->Identity() && existing->Version() == schema->Version();
            }))
                return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaAmbiguous));
            auto payload = AI::BlackboardSchemaCodec::Encode(*schema);
            if (payload.HasError())
                return Result<void>::Failure(payload.ErrorValue());
            if (payload.Value().size() > state.maximumPreparedBytes - state.preparedSchemaBytes)
                return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
            state.preparedSchemaBytes += payload.Value().size();
            state.schemas.push_back({schema->Identity(), schema->Version(), record.id});
            state.environment.schemas.push_back(std::move(schema));
            state.prepared.push_back({record.id, record.type, file->sourceDigest, std::move(payload).Value(), {}});
            return Result<void>::Success();
        }

    }  // namespace

    Result<void> Verify(const State &state, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
        if (!state.inputs.empty() && (!state.environment.nodes || !state.environment.nodes->CanUse()))
            return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
        return state.files->VerifyUnchanged(cancellation);
    }

    Result<void> Capture(State &state, const Assets::AssetRegistrySnapshot &registry, const CancellationToken &cancellation) {
        if (registry.Records().size() > 16384)
            return Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
        for (const auto &record : registry.Records()) {
            if (record.type.Value() == AI::BlackboardSchemaAssetType) {
                if (auto captured = CaptureSchema(state, record, cancellation); captured.HasError())
                    return captured;
                continue;
            }
            if (record.type.Value() != DecisionBehaviorTreeSourceType && record.type.Value() != DecisionStateMachineSourceType &&
                record.type.Value() != DecisionUtilitySourceType)
                continue;
            if (auto captured = CaptureRecord(state, record, cancellation); captured.HasError())
                return captured;
        }
        if (state.inputs.empty() && state.schemas.empty())
            return Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
        std::ranges::sort(state.inputs, {}, [](const auto &input) {
            return input.graph.asset;
        });
        if (std::ranges::adjacent_find(state.inputs, {}, [](const auto &input) {
            return input.graph.asset;
        }) != state.inputs.end())
            return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetSubtreeAmbiguous));
        return Verify(state, cancellation);
    }
}  // namespace Horo::Application::DecisionCookDetail
