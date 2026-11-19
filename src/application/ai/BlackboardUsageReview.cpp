#include "Horo/Application/BlackboardUsageReview.h"

#include "BlackboardUsageSceneSources.h"
#include "DecisionCookState.h"
#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/Editor/ProjectMutation.h"

#include <algorithm>
#include <array>

namespace Horo::Application {
    namespace {
        constexpr AI::DecisionCompilationLimits GraphLimits;
        constexpr std::size_t MaximumDecisionSources = AI::DecisionAssetValidationHardLimits::Schemas + GraphLimits.maximumAssets;
        constexpr std::size_t MaximumQuerySources = 256;
        constexpr std::size_t MaximumScenePrefabSources = 256;
        using BlackboardUsageSources::AddGap;
        using BlackboardUsageSources::MaximumFindings;
        using BlackboardUsageSources::OwnerKind;
        constexpr auto MaximumSourceBytes = std::max(AI::DecisionCodecLimits{}.maximumBytes, AI::EnvironmentQuerySourceCodec::MaximumBytes);
        constexpr std::uint64_t MaximumCapturedBytes = 64ULL * 1024ULL * 1024ULL;

        /** @brief Checks the engine's typed Decision families through the canonical asset-type identities. */
        bool IsDecision(std::string_view type) {
            return type == DecisionBehaviorTreeSourceType || type == DecisionStateMachineSourceType || type == DecisionUtilitySourceType;
        }

        /** @brief Holds the decision and query source closure separately from Scene and Prefab sources. */
        struct SourceSelection final {
            std::vector<Assets::AssetId> ai;
            std::vector<Assets::AssetId> hierarchy;
            std::size_t queryCount{};
            std::size_t decisionCount{};

            /** @brief Admits one registered AI source within its family budget. */
            Result<void> AddAi(Assets::AssetId asset, bool query) {
                auto &count = query ? queryCount : decisionCount;
                if (const auto limit = query ? MaximumQuerySources : MaximumDecisionSources; count == limit)
                    return Result<void>::Failure(
                        MakeError(query ? AI::AIErrors::EnvironmentQueryLimitExceeded : AI::AIErrors::DecisionAssetLimitExceeded));
                ai.push_back(asset);
                ++count;
                return Result<void>::Success();
            }
        };

        /** @brief Selects typed current registry sources and records any hierarchy inputs beyond the review budget. */
        Result<SourceSelection> SelectSources(const Assets::AssetRegistrySnapshot &registry, BlackboardUsageReview &review,
                                              const CancellationToken &cancellation) {
            using Selection = Result<SourceSelection>;
            SourceSelection selected;
            for (const auto &record : registry.Records()) {
                if (cancellation.IsCancellationRequested())
                    return Selection::Failure(MakeError(DecisionCookErrors::Cancelled));
                const auto &type = record.type.Value();
                const bool query = type == AI::EnvironmentQuerySourceAssetType;
                if (query || type == AI::BlackboardSchemaAssetType || IsDecision(type)) {
                    if (auto added = selected.AddAi(record.id, query); added.HasError())
                        return Selection::Failure(added.ErrorValue());
                } else if (const auto kind = OwnerKind(type)) {
                    if (selected.hierarchy.size() < MaximumScenePrefabSources)
                        selected.hierarchy.push_back(record.id);
                    else if (auto gap = AddGap(review, {*kind, record.id, {}, MakeError(AI::AIErrors::DecisionAssetLimitExceeded)});
                             gap.HasError())
                        return Selection::Failure(gap.ErrorValue());
                }
            }
            return Selection::Success(std::move(selected));
        }

        /** @brief Retains the admitted structural source batch or explicit gaps without discarding Decision evidence. */
        Result<std::optional<Assets::AssetCookInputSnapshot>> CaptureHierarchy(const std::filesystem::path &root,
                                                                               const Assets::AssetRegistrySnapshot &registry,
                                                                               std::span<const Assets::AssetId> selected,
                                                                               BlackboardUsageReview &review,
                                                                               const CancellationToken &cancellation) {
            using Capture = Result<std::optional<Assets::AssetCookInputSnapshot>>;
            if (selected.empty())
                return Capture::Success(std::nullopt);
            const Assets::AssetCookLimits limits{.maximumSourceBytes = MaximumSourceBytes, .maximumAssets = MaximumScenePrefabSources};
            auto captured =
                Assets::AssetCookInputSnapshot::CaptureSelected(root, registry, selected, limits, MaximumCapturedBytes, cancellation);
            if (captured.HasValue())
                return Capture::Success(std::move(captured).Value());
            if (cancellation.IsCancellationRequested())
                return Capture::Failure(MakeError(DecisionCookErrors::Cancelled));
            for (const auto asset : selected) {
                // Every identity comes from the same captured registry's typed owner selection.
                const auto kind = OwnerKind(registry.Find(asset)->type.Value());
                if (auto gap = AddGap(review, {*kind, asset, {}, captured.ErrorValue()}); gap.HasError())
                    return Capture::Failure(gap.ErrorValue());
            }
            return Capture::Success(std::nullopt);
        }

        /** @brief Appends exact source and canonical sidecar identity commitments to the review. */
        void AppendStamp(BlackboardUsageReview &review, const Assets::AssetCookPinnedSource &file) {
            review.sources.push_back({file.record.id, file.sourceDigest, file.metadataDigest});
        }

        /** @brief Validates exact selected durable identity and rejects ambiguous semantic schema ownership. */
        Result<AI::BlackboardSchema> CaptureSchema(const Assets::AssetCookInputSnapshot &capture, BlackboardUsageSelection selection) {
            using Schema = Result<AI::BlackboardSchema>;
            std::optional<AI::BlackboardSchema> found;
            for (const auto &file : capture.Sources()) {
                if (file.record.type.Value() != AI::BlackboardSchemaAssetType)
                    continue;
                auto decoded = AI::BlackboardSchemaSourceCodec::Decode(file.bytes);
                if (decoded.HasError())
                    return Schema::Failure(decoded.ErrorValue());
                const auto &schema = decoded.Value().schema;
                const bool selectedSchema = schema.Identity() == selection.schema && schema.Version() == selection.version;
                if (file.record.id != selection.schemaAsset) {
                    if (selectedSchema)
                        return Schema::Failure(MakeError(AI::AIErrors::DecisionAssetSchemaAmbiguous));
                    continue;
                }
                if (!selectedSchema || file.sourceDigest != selection.source)
                    return Schema::Failure(MakeError(DecisionCookErrors::Stale));
                found.emplace(std::move(decoded).Value().schema);
            }
            return found ? Schema::Success(std::move(*found)) : Schema::Failure(MakeError(DecisionCookErrors::Invalid));
        }

        /** @brief Validates the selected key and optional explicit destination against the same captured schema. */
        Result<void> CheckSelection(const AI::BlackboardSchema &schema, const BlackboardUsageSelection &selection) {
            const auto keys = schema.Keys();
            const auto original = std::ranges::find(keys, selection.key, &AI::BlackboardKeyDescriptor::key);
            if (original == keys.end())
                return Result<void>::Failure(MakeError(DecisionCookErrors::Stale));
            if (!selection.replacement)
                return Result<void>::Success();
            const auto replacement = std::ranges::find(keys, *selection.replacement, &AI::BlackboardKeyDescriptor::key);
            if (replacement == keys.end() || !AI::IsCompatibleBlackboardReplacement(*original, *replacement))
                return Result<void>::Failure(MakeError(AI::AIErrors::BlackboardValueTypeMismatch));
            return Result<void>::Success();
        }

        /** @brief Authored query result contract with the exact registered owner in the active snapshot. */
        struct QuerySource final {
            AI::QueryId id;
            AI::QueryResultSchema result;
            const Assets::AssetRecord *record;
        };

        /** @brief Indexes semantic query identities without compiling or executing contributed stages. */
        Result<std::vector<QuerySource>> IndexQueries(BlackboardUsageReview &review, const Assets::AssetCookInputSnapshot &capture,
                                                      const CancellationToken &cancellation) {
            using Index = Result<std::vector<QuerySource>>;
            std::vector<QuerySource> queries;
            for (const auto &file : capture.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Index::Failure(MakeError(DecisionCookErrors::Cancelled));
                if (file.record.type.Value() != AI::EnvironmentQuerySourceAssetType)
                    continue;
                if (const auto query = AI::EnvironmentQuerySourceCodec::Decode(file.bytes); query.HasValue()) {
                    queries.emplace_back(query.Value().Id(), query.Value().ResultSchema(), &file.record);
                } else if (auto gap =
                               AddGap(review, {BlackboardUsageGapKind::EnvironmentQueryBindings, file.record.id, {}, query.ErrorValue()});
                           gap.HasError()) {
                    return Index::Failure(gap.ErrorValue());
                }
            }
            std::ranges::sort(queries, {}, &QuerySource::id);
            return Index::Success(std::move(queries));
        }

        /** @brief Resolves a binding only when one registered asset provides its exact authored result contract. */
        Result<const Assets::AssetRecord *> ResolveQuery(std::span<const QuerySource> queries, const AI::BehaviorTreeQueryOutput &output) {
            using Resolved = Result<const Assets::AssetRecord *>;
            const auto matches = std::ranges::equal_range(queries, output.query, {}, &QuerySource::id);
            if (matches.empty())
                return Resolved::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
            if (matches.size() != 1)
                return Resolved::Failure(MakeError(AI::AIErrors::EnvironmentQueryIdentityConflict));
            const auto &match = *matches.begin();
            if (match.result.id != output.result || match.result.version != output.resultVersion || match.result.itemType != output.item)
                return Resolved::Failure(MakeError(AI::AIErrors::EnvironmentQueryVersionIncompatible));
            return Resolved::Success(match.record);
        }

        /** @brief Retains the typed authoring occurrence even when its target needs repair. */
        Result<void> AppendQueryUsage(BlackboardUsageReview &review, const Assets::AssetRecord &decision,
                                      const AI::DecisionBlackboardQueryUse &use, std::span<const QuerySource> queries) {
            BlackboardQueryUsage usage{decision.id, std::string{decision.sourcePath.String()}, use};
            if (const auto target = ResolveQuery(queries, use.output); target.HasValue()) {
                usage.queryAsset = target.Value()->id;
                usage.queryPath = target.Value()->sourcePath.String();
            } else if (auto gap =
                           AddGap(review, {BlackboardUsageGapKind::EnvironmentQueryBindings, decision.id, use.node, target.ErrorValue()});
                       gap.HasError()) {
                return gap;
            }
            review.queryUsages.push_back(std::move(usage));
            return Result<void>::Success();
        }

        /** @brief Binds the selected key's authored outputs against the indexed source closure. */
        Result<void> AppendQueryUsages(BlackboardUsageReview &review, const Assets::AssetRecord &decision,
                                       std::span<const AI::DecisionBlackboardQueryUse> uses, std::span<const QuerySource> queries) {
            if (uses.size() > MaximumFindings - review.queryUsages.size())
                return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
            for (const auto &use : uses)
                if (auto appended = AppendQueryUsage(review, decision, use, queries); appended.HasError())
                    return appended;
            return Result<void>::Success();
        }

        /** @brief Stages bounded replacement bytes and the exact source commitment for one registered graph. */
        Result<void> AppendRetarget(BlackboardUsageReview &review, const Assets::AssetCookPinnedSource &file,
                                    AI::DecisionGraphSource &graph, const AI::BlackboardSchema &schema,
                                    const AI::DecisionBlackboardProviderMetadata *providers, std::uint64_t &availableBytes) {
            if (!review.selection.replacement)
                return Result<void>::Success();
            const auto rewritten =
                AI::RetargetDecisionBlackboardKey(graph, schema, review.selection.key, *review.selection.replacement, providers);
            if (rewritten.HasError())
                return AddGap(review, {BlackboardUsageGapKind::DecisionRetarget, file.record.id, {}, rewritten.ErrorValue()});
            if (!rewritten.Value())
                return Result<void>::Success();
            auto encoded = AI::DecisionPlanCodec::EncodeSource(std::span{&graph, 1}, graph.asset);
            if (encoded.HasError())
                return AddGap(review, {BlackboardUsageGapKind::DecisionRetarget, file.record.id, {}, encoded.ErrorValue()});
            if (encoded.Value().size() > availableBytes)
                return Result<void>::Failure(MakeError(AI::AIErrors::DecisionAssetLimitExceeded));
            availableBytes -= encoded.Value().size();
            review.decisionRetargets.push_back({{file.record.id, file.sourceDigest, file.metadataDigest}, std::move(encoded).Value()});
            return Result<void>::Success();
        }

        /** @brief Projects one admitted source while preserving exact unresolved or malformed source identities. */
        Result<void> InspectFile(BlackboardUsageReview &review, const Assets::AssetCookPinnedSource &file,
                                 const AI::BlackboardSchema &schema, const AI::DecisionBlackboardProviderMetadata *providers,
                                 std::span<const QuerySource> queries, std::uint64_t &availableBytes) {
            const auto unresolved = [&](const Error &error) {
                return AddGap(review, {BlackboardUsageGapKind::DecisionSource, file.record.id, {}, error});
            };
            auto graph = DecodeRegisteredDecisionGraphSource(file, GraphLimits);
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
            if (auto appended = AppendQueryUsages(review, file.record, inspected.Value().queryOutputs, queries); appended.HasError())
                return appended;
            auto candidate = std::move(graph).Value();
            return AppendRetarget(review, file, candidate, schema, providers, availableBytes);
        }

        /** @brief Checks the durable key and inspects Decision sources against the same registered query capture. */
        Result<void> InspectAiSources(BlackboardUsageReview &review, const Assets::AssetCookInputSnapshot &capture,
                                      const AI::DecisionBlackboardProviderMetadata *providers, const CancellationToken &cancellation) {
            const auto schema = CaptureSchema(capture, review.selection);
            if (schema.HasError())
                return Result<void>::Failure(schema.ErrorValue());
            if (auto checked = CheckSelection(schema.Value(), review.selection); checked.HasError())
                return checked;
            const auto queries = IndexQueries(review, capture, cancellation);
            if (queries.HasError())
                return Result<void>::Failure(queries.ErrorValue());
            auto availableBytes = MaximumCapturedBytes;
            for (const auto &file : capture.Sources()) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(DecisionCookErrors::Cancelled));
                AppendStamp(review, file);
                if (!IsDecision(file.record.type.Value()))
                    continue;
                if (auto inspected = InspectFile(review, file, schema.Value(), providers, queries.Value(), availableBytes);
                    inspected.HasError())
                    return inspected;
            }
            return Result<void>::Success();
        }

        /** @brief Adds Scene and Prefab evidence and verifies the separately bounded hierarchy capture. */
        Result<void> InspectHierarchy(BlackboardUsageReview &review, const std::filesystem::path &root,
                                      const Assets::AssetCookInputSnapshot &owners, const CancellationToken &cancellation) {
            if (auto inspected = BlackboardUsageSources::Inspect(review, root, owners, cancellation); inspected.HasError())
                return inspected;
            review.sources.reserve(review.sources.size() + owners.Sources().size());
            for (const auto &file : owners.Sources())
                AppendStamp(review, file);
            if (auto verified = owners.VerifyUnchanged(cancellation); verified.HasError())
                return verified;
            const std::array scopes{review.closure, owners.ClosureDigest()};
            review.closure = ComputeSha256(std::as_bytes(std::span{scopes}));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CaptureBlackboardUsageReview */
    Result<BlackboardUsageReview> CaptureBlackboardUsageReview(const std::filesystem::path &root, const Assets::AssetRegistry &registry,
                                                               const std::optional<Assets::AssetRegistryRevision> completeInventory,
                                                               const Editor::ProjectMutationCoordinator &mutations,
                                                               const BlackboardUsageSelection selection,
                                                               std::shared_ptr<const AI::DecisionBlackboardProviderMetadata> providers,
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
        BlackboardUsageReview review{.selection = selection, .registry = pinned.Revision(), .providers = providers};
        if (completeInventory != pinned.Revision())
            review.gaps.push_back({BlackboardUsageGapKind::RegistryInventory});
        review.gaps.push_back({BlackboardUsageGapKind::PerceptionPolicy});
        const auto selected = SelectSources(pinned, review, cancellation);
        if (selected.HasError())
            return Review::Failure(selected.ErrorValue());
        const Assets::AssetCookLimits limits{.maximumSourceBytes = MaximumSourceBytes,
                                             .maximumAssets = MaximumDecisionSources + MaximumQuerySources};
        const auto capture =
            Assets::AssetCookInputSnapshot::CaptureSelected(root, pinned, selected.Value().ai, limits, MaximumCapturedBytes, cancellation);
        if (capture.HasError())
            return Review::Failure(capture.ErrorValue());
        review.closure = capture.Value().ClosureDigest();
        review.sources.reserve(capture.Value().Sources().size());
        if (auto inspected = InspectAiSources(review, capture.Value(), providers.get(), cancellation); inspected.HasError())
            return Review::Failure(inspected.ErrorValue());
        const auto hierarchy = CaptureHierarchy(root, pinned, selected.Value().hierarchy, review, cancellation);
        if (hierarchy.HasError())
            return Review::Failure(hierarchy.ErrorValue());
        if (const auto &owners = hierarchy.Value()) {
            if (auto inspected = InspectHierarchy(review, root, *owners, cancellation); inspected.HasError())
                return Review::Failure(inspected.ErrorValue());
        }
        std::ranges::sort(review.sources, {}, &BlackboardUsageSourceStamp::asset);
        if (const auto verified = capture.Value().VerifyUnchanged(cancellation); verified.HasError())
            return Review::Failure(verified.ErrorValue());
        if (registry.Snapshot().Revision() != review.registry)
            return Review::Failure(MakeError(DecisionCookErrors::Stale));
        return Review::Success(std::move(review));
    }
}  // namespace Horo::Application
