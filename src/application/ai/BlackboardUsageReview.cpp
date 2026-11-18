#include "Horo/Application/BlackboardUsageReview.h"

#include "DecisionCookState.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Editor/ProjectMutation.h"

#include <algorithm>

namespace Horo::Application {
    namespace {
        constexpr AI::DecisionCompilationLimits GraphLimits;
        constexpr std::size_t MaximumSources = AI::DecisionAssetValidationHardLimits::Schemas + GraphLimits.maximumAssets;
        constexpr std::size_t MaximumFindings = AI::DecisionAssetValidationHardLimits::Diagnostics;
        constexpr std::size_t MaximumSourceBytes = AI::DecisionCodecLimits{}.maximumBytes;
        constexpr std::uint64_t MaximumCapturedBytes = 64ULL * 1024ULL * 1024ULL;

        /** @brief Checks the engine's typed Decision families through the canonical asset-type identities. */
        bool IsDecision(std::string_view type) {
            return type == DecisionBehaviorTreeSourceType || type == DecisionStateMachineSourceType || type == DecisionUtilitySourceType;
        }

        /** @brief Captures every selected schema/Decision locator and reports known Scene and Prefab inventory gaps. */
        Result<std::vector<Assets::AssetId>> SelectSources(const Assets::AssetRegistrySnapshot &registry, BlackboardUsageReview &review,
                                                           const CancellationToken &cancellation) {
            using Selection = Result<std::vector<Assets::AssetId>>;
            std::vector<Assets::AssetId> selected;
            for (const auto &record : registry.Records()) {
                if (cancellation.IsCancellationRequested())
                    return Selection::Failure(MakeError(DecisionCookErrors::Cancelled));
                const auto &type = record.type.Value();
                if (type == AI::BlackboardSchemaAssetType || IsDecision(type)) {
                    if (selected.size() == MaximumSources)
                        return Selection::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
                    selected.push_back(record.id);
                } else if (type == "core.scene" || type == "core.prefab") {
                    if (review.gaps.size() == MaximumFindings)
                        return Selection::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
                    const auto owner = type == "core.scene" ? BlackboardUsageGapKind::SceneSource : BlackboardUsageGapKind::PrefabSource;
                    review.gaps.push_back({owner, record.id});
                }
            }
            return Selection::Success(std::move(selected));
        }

        /** @brief Validates exact selected durable identity and rejects ambiguous semantic schema ownership. */
        Result<void> CheckSchema(const Assets::AssetCookInputSnapshot &capture, BlackboardUsageSelection selection) {
            bool found = false;
            for (const auto &file : capture.Sources()) {
                if (file.record.type.Value() != AI::BlackboardSchemaAssetType)
                    continue;
                const auto decoded = AI::BlackboardSchemaSourceCodec::Decode(file.bytes);
                if (decoded.HasError())
                    return Result<void>::Failure(decoded.ErrorValue());
                const auto &schema = decoded.Value().schema;
                const bool selectedSchema = schema.Identity() == selection.schema && schema.Version() == selection.version;
                if (file.record.id != selection.schemaAsset) {
                    if (selectedSchema)
                        return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaAmbiguous));
                    continue;
                }
                found = selectedSchema && file.sourceDigest == selection.source &&
                        std::ranges::binary_search(schema.Keys(), selection.key, {}, &AI::BlackboardKeyDescriptor::key);
                if (!found)
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
            }
            return found ? Result<void>::Success() : Result<void>::Failure(MakeError(DecisionCookErrors::Invalid));
        }

        /** @brief Projects one admitted source while preserving exact unresolved or malformed source identities. */
        Result<void> InspectFile(BlackboardUsageReview &review, const Assets::AssetCookPinnedSource &file,
                                 const AI::DecisionNodeGeneration *providers) {
            const auto unresolved = [&](const Error &error) {
                if (review.gaps.size() == MaximumFindings)
                    return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
                review.gaps.push_back({BlackboardUsageGapKind::DecisionSource, file.record.id, {}, error});
                return Result<void>::Success();
            };
            const auto graph = DecisionCookDetail::DecodeRegisteredDecisionGraphSource(file, GraphLimits);
            if (graph.HasError())
                return unresolved(graph.ErrorValue());
            if (graph.Value().blackboard != review.selection.schema || !graph.Value().blackboardVersion.Contains(review.selection.version))
                return Result<void>::Success();
            auto inspected = AI::InspectDecisionBlackboardUses(graph.Value(), review.selection.key, providers);
            if (inspected.HasError())
                return unresolved(inspected.ErrorValue());
            if (inspected.Value().uses.size() > MaximumFindings - review.uses.size() ||
                inspected.Value().unresolvedProviders.size() > MaximumFindings - review.gaps.size())
                return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
            for (auto &use : inspected.Value().uses)
                review.uses.push_back({file.record.id, graph.Value().asset, std::string{file.record.sourcePath.String()}, std::move(use)});
            for (const auto node : inspected.Value().unresolvedProviders)
                review.gaps.push_back({BlackboardUsageGapKind::DecisionProvider, file.record.id, node});
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CaptureBlackboardUsageReview */
    Result<BlackboardUsageReview> CaptureBlackboardUsageReview(const std::filesystem::path &root, const Assets::AssetRegistry &registry,
                                                               const std::optional<Assets::AssetRegistryRevision> completeInventory,
                                                               const Editor::ProjectMutationCoordinator &mutations,
                                                               const BlackboardUsageSelection selection,
                                                               std::shared_ptr<const AI::DecisionNodeGeneration> providers,
                                                               const CancellationToken &cancellation) {
        using Review = Result<BlackboardUsageReview>;
        if (!selection.schemaAsset.IsValid() || !selection.schema.IsValid() || !selection.key.IsValid() || selection.version == 0)
            return Review::Failure(MakeError(DecisionCookErrors::Invalid));
        if (cancellation.IsCancellationRequested())
            return Review::Failure(MakeError(DecisionCookErrors::Cancelled));
        const auto lease = mutations.TryAcquire({root, Editor::ProjectMutationOwner::Asset, "blackboard-usage-review"});
        if (lease.HasError())
            return Review::Failure(lease.ErrorValue());

        const auto pinned = registry.Snapshot();
        if (pinned.Revision().value == 0)
            return Review::Failure(MakeError(DecisionCookErrors::Stale));
        if (providers && !providers->CanUse())
            return Review::Failure(MakeError(DecisionCookErrors::Stale));
        BlackboardUsageReview review{.selection = selection, .registry = pinned.Revision(), .providers = providers};
        if (completeInventory != pinned.Revision())
            review.gaps.push_back({BlackboardUsageGapKind::RegistryInventory});
        review.gaps.push_back({BlackboardUsageGapKind::EnvironmentQueryBindings});
        review.gaps.push_back({BlackboardUsageGapKind::PerceptionPolicy});
        const auto selected = SelectSources(pinned, review, cancellation);
        if (selected.HasError())
            return Review::Failure(selected.ErrorValue());
        const Assets::AssetCookLimits limits{.maximumSourceBytes = MaximumSourceBytes, .maximumAssets = MaximumSources};
        const auto capture =
            Assets::AssetCookInputSnapshot::CaptureSelected(root, pinned, selected.Value(), limits, MaximumCapturedBytes, cancellation);
        if (capture.HasError())
            return Review::Failure(capture.ErrorValue());
        if (const auto checked = CheckSchema(capture.Value(), selection); checked.HasError())
            return Review::Failure(checked.ErrorValue());
        review.closure = capture.Value().ClosureDigest();
        review.sources.reserve(capture.Value().Sources().size());
        for (const auto &file : capture.Value().Sources()) {
            if (cancellation.IsCancellationRequested())
                return Review::Failure(MakeError(DecisionCookErrors::Cancelled));
            review.sources.push_back({file.record.id, file.sourceDigest, file.metadataDigest});
            if (IsDecision(file.record.type.Value())) {
                if (const auto inspected = InspectFile(review, file, providers.get()); inspected.HasError())
                    return Review::Failure(inspected.ErrorValue());
            }
        }
        if (const auto verified = capture.Value().VerifyUnchanged(cancellation); verified.HasError())
            return Review::Failure(verified.ErrorValue());
        if (registry.Snapshot().Revision() != review.registry || (providers && !providers->CanUse()))
            return Review::Failure(MakeError(DecisionCookErrors::Stale));
        return Review::Success(std::move(review));
    }
}  // namespace Horo::Application
