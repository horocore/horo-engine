#include "Horo/Prefab/PrefabDependencyGraph.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string_view>
#include <tuple>
#include <utility>

namespace Horo::Prefab {
    namespace {
        constexpr std::string_view PrefabAssetType = "core.prefab";

        using SourceMap = std::map<Assets::AssetId, const PrefabDependencySource *>;
        using NodeMap = std::map<Assets::AssetId, PrefabDependencyNode>;

        /** @brief Verifies that a source revision uses the document's canonical project version. */
        [[nodiscard]] bool RevisionMatchesDocument(const PrefabDependencySource &source) {
            const Application::HoroVersion &version = source.sourceRevision.projectVersion;
            const auto parsed = Application::ParseHoroVersion(Application::FormatHoroVersion(version));
            return parsed.HasValue() && parsed.Value() == version && version == source.document.Data().projectVersion;
        }

        /** @brief Classifies one already declared dependency according to composition semantics. */
        [[nodiscard]] PrefabDependencyKind DependencyKind(const PrefabDocumentData &document, const Assets::AssetId &target) noexcept {
            if (!document.composition)
                return PrefabDependencyKind::Resource;
            const PrefabComposition &composition = *document.composition;
            if (composition.variantParent && composition.variantParent->Asset() == target)
                return PrefabDependencyKind::VariantParent;
            if (std::ranges::any_of(composition.nestedPlacements, [&target](const NestedPrefabPlacement &placement) {
                return placement.sourcePrefab.Asset() == target;
            })) {
                return PrefabDependencyKind::NestedPrefab;
            }
            return PrefabDependencyKind::Resource;
        }

        /** @brief Charges work before inserting a graph-owned item. */
        [[nodiscard]] Result<void> Charge(PrefabExpansionBudget &budget) {
            return budget.Consume(1);
        }

        /** @brief Adds one registry-backed node if it is not already captured. */
        [[nodiscard]] Result<void> CaptureNode(NodeMap &nodes, const Assets::AssetRecord &record,
                                               std::optional<PrefabSourceRevision> revision, PrefabExpansionBudget &budget) {
            if (const auto existing = nodes.find(record.id); existing != nodes.end()) {
                if (revision)
                    existing->second.sourceRevision = std::move(revision);
                return Result<void>::Success();
            }
            if (const auto charged = Charge(budget); charged.HasError())
                return charged;
            nodes.try_emplace(record.id, record.id, record.type, std::move(revision));
            return Result<void>::Success();
        }

        [[nodiscard]] bool InsertSorted(std::vector<Assets::AssetId> &values, const Assets::AssetId value) {
            const auto position = std::ranges::lower_bound(values, value);
            if (position != values.end() && *position == value)
                return false;
            values.insert(position, value);
            return true;
        }

        /** @brief Canonicalizes borrowed source identity order before node validation can select a failure. */
        std::vector<const PrefabDependencySource *> OrderedSources(const std::vector<PrefabDependencySource> &sources) {
            std::vector<const PrefabDependencySource *> ordered;
            ordered.reserve(sources.size());
            for (const auto &source : sources)
                ordered.push_back(&source);
            std::ranges::sort(ordered, {}, [](const auto *source) {
                return source->document.Data().assetId;
            });
            return ordered;
        }

        /** @brief Admits source identities in stable order before collecting any semantic edge. */
        [[nodiscard]] Result<void> ValidateAndCaptureNodes(const Assets::AssetRegistrySnapshot &registry,
                                                           const std::vector<PrefabDependencySource> &sources,
                                                           const PrefabLimitProfile &limits, PrefabExpansionBudget &budget,
                                                           SourceMap &sourceById, NodeMap &nodes) {
            for (const auto *entry : OrderedSources(sources)) {
                const auto &source = *entry;
                const PrefabDocumentData &document = source.document.Data();
                if (document.referencedAssets.size() > limits.Policy().maximumReferencedAssets)
                    return Result<void>::Failure(MakeError(PrefabErrors::ReferenceCountExceeded));
                if (document.composition && document.composition->nestedPlacements.size() > limits.Policy().maximumDirectNestedPlacements)
                    return Result<void>::Failure(MakeError(PrefabErrors::NestedPlacementCountExceeded));
                const Assets::AssetId id = document.assetId;
                if (!RevisionMatchesDocument(source) || !sourceById.try_emplace(id, std::addressof(source)).second)
                    return Result<void>::Failure(MakeError(PrefabErrors::DependencyGraphInvalid));
                const Assets::AssetRecord *record = registry.Find(id);
                if (record == nullptr)
                    return Result<void>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
                if (record->type.Value() != PrefabAssetType)
                    return Result<void>::Failure(MakeError(PrefabErrors::DependencyTypeMismatch));
                if (const auto captured = CaptureNode(nodes, *record, source.sourceRevision, budget); captured.HasError())
                    return captured;
            }
            return Result<void>::Success();
        }

        /** @brief One bounded graph capture; policy never changes which registry/source generation supplies a target. */
        struct EdgeCapture final {
            const Assets::AssetRegistrySnapshot &registry;
            const SourceMap &sources;
            NodeMap &nodes;
            PrefabExpansionBudget &budget;
            PrefabSourceRevisionPolicy policy;
            std::vector<PrefabDependencyRevisionSkew> &skews;
        };

        /** @brief Retains both revisions only for explicitly admitted authoring rebases. */
        Result<void> AppendRevisionSkew(EdgeCapture &capture, const PrefabDocumentData &owner, Assets::AssetId target,
                                        PrefabDependencyKind kind, std::optional<LocalObjectId> placement,
                                        const PrefabSourceRevision &authored, const PrefabSourceRevision &actual) {
            if (authored == actual)
                return Result<void>::Success();
            if (capture.policy == PrefabSourceRevisionPolicy::RequireExact)
                return Result<void>::Failure(MakeError(PrefabErrors::DependencyRevisionMismatch));
            if (auto charged = Charge(capture.budget); charged.HasError())
                return charged;
            capture.skews.push_back({owner.assetId, target, kind, placement, authored, actual});
            return Result<void>::Success();
        }

        /** @brief Checks each stable occurrence independently; repeated asset references cannot hide an older placement revision. */
        Result<void> CaptureEdgeRevisions(EdgeCapture &capture, const PrefabDocumentData &document, Assets::AssetId target,
                                          PrefabDependencyKind kind, const PrefabSourceRevision &actual) {
            const auto &composition = *document.composition;
            if (kind == PrefabDependencyKind::VariantParent)
                return AppendRevisionSkew(capture, document, target, kind, {}, *composition.variantAuthoredAgainst, actual);
            for (const auto &placement : composition.nestedPlacements)
                if (placement.sourcePrefab.Asset() == target)
                    if (auto accepted = AppendRevisionSkew(capture, document, target, kind, placement.placementLocalId,
                                                           placement.authoredAgainst, actual);
                        accepted.HasError())
                        return accepted;
            return Result<void>::Success();
        }

        /** @brief Admits availability, role and revisions before capturing any edge or bounded node reference. */
        Result<PrefabDependencyEdge> CaptureEdge(EdgeCapture &capture, const PrefabDocumentData &document, Assets::AssetId target) {
            const auto *record = capture.registry.Find(target);
            if (!record)
                return Result<PrefabDependencyEdge>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            const auto kind = DependencyKind(document, target);
            if (kind != PrefabDependencyKind::Resource && record->type.Value() != PrefabAssetType)
                return Result<PrefabDependencyEdge>::Failure(MakeError(PrefabErrors::DependencyTypeMismatch));
            const auto source = capture.sources.find(target);
            if (record->type.Value() == PrefabAssetType && source == capture.sources.end())
                return Result<PrefabDependencyEdge>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            if (kind != PrefabDependencyKind::Resource)
                if (auto revisions = CaptureEdgeRevisions(capture, document, target, kind, source->second->sourceRevision);
                    revisions.HasError())
                    return Result<PrefabDependencyEdge>::Failure(revisions.ErrorValue());
            if (auto node = CaptureNode(capture.nodes, *record, std::nullopt, capture.budget); node.HasError())
                return Result<PrefabDependencyEdge>::Failure(node.ErrorValue());
            if (auto charged = Charge(capture.budget); charged.HasError())
                return Result<PrefabDependencyEdge>::Failure(charged.ErrorValue());
            return Result<PrefabDependencyEdge>::Success({document.assetId, target, kind});
        }

        /** @brief Canonicalizes only the bounded source view so serialized dependency order cannot choose an error. */
        Result<void> CaptureSourceEdges(EdgeCapture &capture, const PrefabDocumentData &document,
                                        std::vector<PrefabDependencyEdge> &edges) {
            auto references = document.referencedAssets;
            std::ranges::sort(references);
            for (const auto target : references) {
                auto edge = CaptureEdge(capture, document, target);
                if (edge.HasError())
                    return Result<void>::Failure(edge.ErrorValue());
                edges.push_back(std::move(edge).Value());
            }
            return Result<void>::Success();
        }

        /** @brief Uses canonical owner/target identity order for output and diagnostics under both explicit capture modes. */
        [[nodiscard]] Result<std::vector<PrefabDependencyEdge>> ProcessEdges(EdgeCapture &capture) {
            std::vector<PrefabDependencyEdge> edges;
            for (const auto &[asset, source] : capture.sources) {
                static_cast<void>(asset);
                if (auto captured = CaptureSourceEdges(capture, source->document.Data(), edges); captured.HasError())
                    return Result<std::vector<PrefabDependencyEdge>>::Failure(captured.ErrorValue());
            }
            std::ranges::sort(capture.skews, [](const auto &a, const auto &b) {
                return std::tie(a.owner, a.target, a.kind, a.placement) < std::tie(b.owner, b.target, b.kind, b.placement);
            });
            return Result<std::vector<PrefabDependencyEdge>>::Success(std::move(edges));
        }
    }  // namespace

    /** @brief Publishes only fully admitted canonical graph storage and its explicit authoring-capture evidence. */
    PrefabDependencyGraphSnapshot::PrefabDependencyGraphSnapshot(const Assets::AssetRegistryRevision registryRevision,
                                                                 std::vector<PrefabDependencyNode> nodes,
                                                                 std::vector<PrefabDependencyEdge> edges,
                                                                 std::vector<PrefabDependencyEdge> reverseEdges,
                                                                 PrefabSourceRevisionPolicy policy,
                                                                 std::vector<PrefabDependencyRevisionSkew> skews) noexcept
        : registryRevision_(registryRevision), nodes_(std::move(nodes)), edges_(std::move(edges)), reverseEdges_(std::move(reverseEdges)),
          revisionPolicy_(policy), revisionSkews_(std::move(skews)) {}

    /** @copydoc PrefabDependencyGraphSnapshot::RevisionPolicy */
    PrefabSourceRevisionPolicy PrefabDependencyGraphSnapshot::RevisionPolicy() const noexcept {
        return revisionPolicy_;
    }

    /** @copydoc PrefabDependencyGraphSnapshot::RevisionSkews */
    std::span<const PrefabDependencyRevisionSkew> PrefabDependencyGraphSnapshot::RevisionSkews() const noexcept {
        return revisionSkews_;
    }

    /** @copydoc PrefabDependencyGraphSnapshot::RegistryRevision */
    Assets::AssetRegistryRevision PrefabDependencyGraphSnapshot::RegistryRevision() const noexcept {
        return registryRevision_;
    }

    /** @copydoc PrefabDependencyGraphSnapshot::Nodes */
    std::span<const PrefabDependencyNode> PrefabDependencyGraphSnapshot::Nodes() const noexcept {
        return nodes_;
    }

    /** @copydoc PrefabDependencyGraphSnapshot::Edges */
    std::span<const PrefabDependencyEdge> PrefabDependencyGraphSnapshot::Edges() const noexcept {
        return edges_;
    }

    /** @copydoc PrefabDependencyGraphSnapshot::FindNode */
    const PrefabDependencyNode *PrefabDependencyGraphSnapshot::FindNode(const Assets::AssetId assetId) const noexcept {
        const auto found = std::ranges::lower_bound(nodes_, assetId, {}, &PrefabDependencyNode::assetId);
        return found != nodes_.end() && found->assetId == assetId ? std::to_address(found) : nullptr;
    }

    /** @copydoc PrefabDependencyGraphSnapshot::DirectDependencies */
    std::span<const PrefabDependencyEdge> PrefabDependencyGraphSnapshot::DirectDependencies(const Assets::AssetId prefabId) const noexcept {
        const auto first = std::ranges::lower_bound(edges_, prefabId, {}, &PrefabDependencyEdge::sourcePrefab);
        const auto last = std::ranges::upper_bound(first, edges_.end(), prefabId, {}, &PrefabDependencyEdge::sourcePrefab);
        return {first, last};
    }

    /** @copydoc PrefabDependencyGraphSnapshot::DependencyClosure */
    Result<std::vector<Assets::AssetId>> PrefabDependencyGraphSnapshot::DependencyClosure(
        const std::span<const Assets::AssetId> roots) const {
        std::vector<Assets::AssetId> visited;
        std::vector<Assets::AssetId> pending;
        visited.reserve(nodes_.size());
        pending.reserve(nodes_.size());
        for (const Assets::AssetId root : roots) {
            if (FindNode(root) == nullptr)
                return Result<std::vector<Assets::AssetId>>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            if (InsertSorted(visited, root))
                pending.push_back(root);
        }

        std::size_t index = 0;
        while (index < pending.size()) {
            for (const PrefabDependencyEdge &edge : DirectDependencies(pending[index])) {
                if (InsertSorted(visited, edge.targetAsset))
                    pending.push_back(edge.targetAsset);
            }
            ++index;
        }

        for (const Assets::AssetId root : roots)
            if (const auto found = std::ranges::lower_bound(visited, root); found != visited.end() && *found == root)
                visited.erase(found);
        return Result<std::vector<Assets::AssetId>>::Success(std::move(visited));
    }

    /** @copydoc PrefabDependencyGraphSnapshot::InvalidatedPrefabs */
    std::vector<Assets::AssetId> PrefabDependencyGraphSnapshot::InvalidatedPrefabs(
        const std::span<const Assets::AssetId> changedAssets) const {
        std::vector<Assets::AssetId> visited;
        std::vector<Assets::AssetId> affected;
        std::vector<Assets::AssetId> pending;
        visited.reserve(nodes_.size());
        affected.reserve(nodes_.size());
        pending.reserve(nodes_.size());
        for (const Assets::AssetId changed : changedAssets) {
            if (const PrefabDependencyNode *node = FindNode(changed); node != nullptr && InsertSorted(visited, changed)) {
                pending.push_back(changed);
                if (node->sourceRevision)
                    static_cast<void>(InsertSorted(affected, changed));
            }
        }

        std::size_t index = 0;
        while (index < pending.size()) {
            const Assets::AssetId target = pending[index];
            const auto first = std::ranges::lower_bound(reverseEdges_, target, {}, &PrefabDependencyEdge::targetAsset);
            const auto last = std::ranges::upper_bound(first, reverseEdges_.end(), target, {}, &PrefabDependencyEdge::targetAsset);
            for (auto edge = first; edge != last; ++edge) {
                static_cast<void>(InsertSorted(affected, edge->sourcePrefab));
                if (InsertSorted(visited, edge->sourcePrefab))
                    pending.push_back(edge->sourcePrefab);
            }
            ++index;
        }
        return affected;
    }

    /** @copydoc BuildPrefabDependencyGraph */
    Result<PrefabDependencyGraphSnapshot> BuildPrefabDependencyGraph(const Assets::AssetRegistrySnapshot &registry,
                                                                     const std::vector<PrefabDependencySource> &sources,
                                                                     const PrefabLimitProfile &limits, const PrefabMetricHandles &metrics,
                                                                     PrefabSourceRevisionPolicy revisionPolicy) {
        PrefabMetricOperation observation{metrics, PrefabMetricPhase::Graph};
        auto result = [&]() -> Result<PrefabDependencyGraphSnapshot> {
            if (revisionPolicy > PrefabSourceRevisionPolicy::PreserveLayerRebase)
                return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::DependencyGraphInvalid));
            PrefabExpansionBudget budget{limits};
            SourceMap sourceById;
            NodeMap nodes;

            if (auto validated = ValidateAndCaptureNodes(registry, sources, limits, budget, sourceById, nodes); validated.HasError())
                return Result<PrefabDependencyGraphSnapshot>::Failure(std::move(validated).ErrorValue());
            std::vector<PrefabDependencyRevisionSkew> skews;
            EdgeCapture capture{registry, sourceById, nodes, budget, revisionPolicy, skews};
            auto processedEdges = ProcessEdges(capture);
            if (processedEdges.HasError())
                return Result<PrefabDependencyGraphSnapshot>::Failure(std::move(processedEdges).ErrorValue());
            std::vector<PrefabDependencyEdge> edges = std::move(processedEdges).Value();

            std::ranges::sort(edges);
            edges.erase(std::ranges::unique(edges).begin(), edges.end());
            std::vector<PrefabDependencyEdge> reverseEdges = edges;
            std::ranges::sort(reverseEdges, [](const PrefabDependencyEdge &left, const PrefabDependencyEdge &right) {
                if (left.targetAsset != right.targetAsset)
                    return left.targetAsset < right.targetAsset;
                if (left.sourcePrefab != right.sourcePrefab)
                    return left.sourcePrefab < right.sourcePrefab;
                return left.kind < right.kind;
            });

            std::vector<PrefabDependencyNode> canonicalNodes;
            canonicalNodes.reserve(nodes.size());
            for (auto &[id, node] : nodes) {
                static_cast<void>(id);
                canonicalNodes.push_back(std::move(node));
            }
            return Result<PrefabDependencyGraphSnapshot>::Success(
                PrefabDependencyGraphSnapshot{registry.Revision(), std::move(canonicalNodes), std::move(edges), std::move(reverseEdges),
                                              revisionPolicy, std::move(skews)});
        }();
        if (result.HasError())
            observation.Fail(result.ErrorValue());
        else {
            observation.Observe(PrefabMetricItem::GraphNodes, result.Value().Nodes().size());
            observation.Observe(PrefabMetricItem::GraphEdges, result.Value().Edges().size());
            observation.Complete();
        }
        return result;
    }
}  // namespace Horo::Prefab
