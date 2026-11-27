#include "DecisionTreeSubtreeCatalog.h"

#include "../../AuthoredAiSourceInventory.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Application/DecisionAssetCookHost.h"

#include <algorithm>
#include <map>
#include <set>

namespace Horo::Editor {
    namespace {
        /** @brief Enumerates references from the two existing Behavior Tree source encodings. */
        Result<std::vector<AI::DecisionGraphAssetId>> CallTargets(const AI::DecisionGraphSource &source) {
            using Targets = Result<std::vector<AI::DecisionGraphAssetId>>;
            std::vector<AI::DecisionGraphAssetId> calls;
            const auto *tree = std::get_if<std::shared_ptr<const AI::BehaviorTreeAsset>>(&source.body);
            if (!tree || !*tree)
                return Targets::Success({});
            for (const auto &node : (*tree)->Nodes()) {
                if (node.call) {
                    if (node.call->kind != AI::DecisionPlanKind::BehaviorTree)
                        return Targets::Failure(MakeError(AI::AIErrors::DecisionAssetSubtreeIncompatible));
                    calls.push_back(node.call->asset);
                }
                if (node.nestedSubtree) {
                    const auto id = AI::DecisionGraphAssetId::Create(node.nestedSubtree->graph.Value());
                    if (id.HasError())
                        return Targets::Failure(id.ErrorValue());
                    calls.push_back(id.Value());
                }
            }
            return Targets::Success(std::move(calls));
        }
    }  // namespace

    /** @copydoc HasDecisionTreeCalls */
    bool HasDecisionTreeCalls(const AI::DecisionGraphSource &source) {
        const auto *tree = std::get_if<std::shared_ptr<const AI::BehaviorTreeAsset>>(&source.body);
        return tree && *tree && std::ranges::any_of((*tree)->Nodes(), [](const auto &node) {
            return node.call || node.nestedSubtree;
        });
    }

    /** @copydoc CaptureDecisionTreeClosure */
    Result<DecisionTreeClosure> CaptureDecisionTreeClosure(const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry,
                                                           const AI::DecisionGraphSource &candidate,
                                                           const std::optional<BehaviorTreeSubtreeReview> &review) {
        using Closure = Result<DecisionTreeClosure>;
        DecisionTreeClosure result{{candidate}, std::nullopt};
        if (!HasDecisionTreeCalls(candidate))
            return Closure::Success(std::move(result));
        auto pinned =
            AuthoredAiSources::Capture(root, registry, Application::DecisionBehaviorTreeSourceType, AI::DecisionCodecLimits{}.maximumBytes);
        if (pinned.HasError())
            return Closure::Failure(pinned.ErrorValue());
        if (review) {
            const auto *reviewed = pinned.Value().Find(review->asset);
            if (!reviewed || reviewed->sourceDigest != review->digest || registry.Revision().value != review->registryRevision)
                return Closure::Failure(MakeError(Application::DecisionCookErrors::Stale));
        }

        std::map<AI::DecisionGraphAssetId, AI::DecisionGraphSource> catalog;
        for (const auto &file : pinned.Value().Sources()) {
            auto decoded = Application::DecodeRegisteredDecisionGraphSource(file);
            if (decoded.HasError())
                return Closure::Failure(decoded.ErrorValue());
            const auto id = decoded.Value().asset;
            if (review && file.record.id == review->asset && (id != review->graph || decoded.Value().schemaVersion != review->graphVersion))
                return Closure::Failure(MakeError(Application::DecisionCookErrors::Stale));
            if (!catalog.try_emplace(id, std::move(decoded).Value()).second)
                return Closure::Failure(MakeError(AI::AIErrors::DecisionAssetSubtreeAmbiguous));
        }

        constexpr AI::DecisionCompilationLimits limits{};
        std::set<AI::DecisionGraphAssetId> queued{candidate.asset};
        for (std::size_t index = 0; index < result.graphs.size(); ++index) {
            const auto calls = CallTargets(result.graphs[index]);
            if (calls.HasError())
                return Closure::Failure(calls.ErrorValue());
            for (const auto id : calls.Value()) {
                if (!queued.insert(id).second)
                    continue;  // Preserve the actual reference; the compiler validates cycles.
                const auto found = catalog.find(id);
                if (found == catalog.end())
                    return Closure::Failure(MakeError(AI::AIErrors::DecisionAssetSubtreeMissing));
                if (result.graphs.size() == limits.maximumAssets)
                    return Closure::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
                result.graphs.push_back(found->second);
            }
        }
        result.inputs.emplace(std::move(pinned).Value());
        return Closure::Success(std::move(result));
    }
}  // namespace Horo::Editor
