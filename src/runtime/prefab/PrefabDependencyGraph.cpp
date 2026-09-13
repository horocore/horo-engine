#include "Horo/Prefab/PrefabDependencyGraph.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string_view>
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

        /** @brief Checks every occurrence of one semantic edge against its captured target revision. */
        [[nodiscard]] bool AuthoredRevisionsMatch(const PrefabDocumentData &document, const Assets::AssetId &target,
                                                  const PrefabDependencyKind kind, const PrefabSourceRevision &targetRevision) noexcept {
            if (!document.composition)
                return false;
            const PrefabComposition &composition = *document.composition;
            if (kind == PrefabDependencyKind::VariantParent)
                return composition.variantAuthoredAgainst && *composition.variantAuthoredAgainst == targetRevision;
            if (kind == PrefabDependencyKind::NestedPrefab) {
                bool found = false;
                for (const NestedPrefabPlacement &placement : composition.nestedPlacements) {
                    if (placement.sourcePrefab.Asset() == target) {
                        found = true;
                        if (placement.authoredAgainst != targetRevision)
                            return false;
                    }
                }
                return found;
            }
            return false;
        }

        /** @brief Charges work before inserting a graph-owned item. */
        [[nodiscard]] Result<void> Charge(PrefabExpansionBudget &budget) {
            return budget.Consume(1);
        }

        /** @brief Adds one registry-backed node if it is not already captured. */
        [[nodiscard]] Result<void> CaptureNode(NodeMap &nodes, const Assets::AssetRecord &record,
                                               std::optional<PrefabSourceRevision> revision, PrefabExpansionBudget &budget) {
            const auto existing = nodes.find(record.id);
            if (existing != nodes.end()) {
                if (revision)
                    existing->second.sourceRevision = std::move(revision);
                return Result<void>::Success();
            }
            if (const auto charged = Charge(budget); charged.HasError())
                return charged;
            nodes.emplace(record.id, PrefabDependencyNode{record.id, record.type, std::move(revision)});
            return Result<void>::Success();
        }
    }  // namespace

    PrefabDependencyGraphSnapshot::PrefabDependencyGraphSnapshot(const Assets::AssetRegistryRevision registryRevision,
                                                                 std::vector<PrefabDependencyNode> nodes,
                                                                 std::vector<PrefabDependencyEdge> edges,
                                                                 std::vector<PrefabDependencyEdge> reverseEdges) noexcept
        : registryRevision_(registryRevision), nodes_(std::move(nodes)), edges_(std::move(edges)), reverseEdges_(std::move(reverseEdges)) {}

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
        std::set<Assets::AssetId> visited;
        std::vector<Assets::AssetId> pending;
        pending.reserve(nodes_.size());
        for (const Assets::AssetId root : roots) {
            if (FindNode(root) == nullptr)
                return Result<std::vector<Assets::AssetId>>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            if (visited.insert(root).second)
                pending.push_back(root);
        }

        for (std::size_t index = 0; index < pending.size(); ++index) {
            for (const PrefabDependencyEdge &edge : DirectDependencies(pending[index])) {
                if (visited.insert(edge.targetAsset).second)
                    pending.push_back(edge.targetAsset);
            }
        }

        for (const Assets::AssetId root : roots)
            visited.erase(root);
        return Result<std::vector<Assets::AssetId>>::Success({visited.begin(), visited.end()});
    }

    /** @copydoc PrefabDependencyGraphSnapshot::InvalidatedPrefabs */
    std::vector<Assets::AssetId> PrefabDependencyGraphSnapshot::InvalidatedPrefabs(
        const std::span<const Assets::AssetId> changedAssets) const {
        std::set<Assets::AssetId> visited;
        std::set<Assets::AssetId> affected;
        std::vector<Assets::AssetId> pending;
        pending.reserve(nodes_.size());
        for (const Assets::AssetId changed : changedAssets) {
            if (const PrefabDependencyNode *node = FindNode(changed); node != nullptr && visited.insert(changed).second) {
                pending.push_back(changed);
                if (node->sourceRevision)
                    affected.insert(changed);
            }
        }

        for (std::size_t index = 0; index < pending.size(); ++index) {
            const Assets::AssetId target = pending[index];
            const auto first = std::ranges::lower_bound(reverseEdges_, target, {}, &PrefabDependencyEdge::targetAsset);
            const auto last = std::ranges::upper_bound(first, reverseEdges_.end(), target, {}, &PrefabDependencyEdge::targetAsset);
            for (auto edge = first; edge != last; ++edge) {
                affected.insert(edge->sourcePrefab);
                if (visited.insert(edge->sourcePrefab).second)
                    pending.push_back(edge->sourcePrefab);
            }
        }
        return {affected.begin(), affected.end()};
    }

    /** @copydoc BuildPrefabDependencyGraph */
    Result<PrefabDependencyGraphSnapshot> BuildPrefabDependencyGraph(const Assets::AssetRegistrySnapshot &registry,
                                                                     std::vector<PrefabDependencySource> sources,
                                                                     const PrefabLimitProfile &limits) {
        PrefabExpansionBudget budget{limits};
        SourceMap sourceById;
        NodeMap nodes;

        for (const PrefabDependencySource &source : sources) {
            const PrefabDocumentData &document = source.document.Data();
            if (document.referencedAssets.size() > limits.Policy().maximumReferencedAssets)
                return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::ReferenceCountExceeded));
            if (document.composition && document.composition->nestedPlacements.size() > limits.Policy().maximumDirectNestedPlacements)
                return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::NestedPlacementCountExceeded));
            const Assets::AssetId id = document.assetId;
            if (!RevisionMatchesDocument(source) || !sourceById.emplace(id, std::addressof(source)).second)
                return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::DependencyGraphInvalid));
            const Assets::AssetRecord *record = registry.Find(id);
            if (record == nullptr)
                return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            if (record->type.Value() != PrefabAssetType)
                return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::DependencyTypeMismatch));
            if (const auto captured = CaptureNode(nodes, *record, source.sourceRevision, budget); captured.HasError())
                return Result<PrefabDependencyGraphSnapshot>::Failure(captured.ErrorValue());
        }

        std::vector<PrefabDependencyEdge> edges;
        for (const PrefabDependencySource &source : sources) {
            const PrefabDocumentData &document = source.document.Data();
            for (const Assets::AssetId target : document.referencedAssets) {
                const Assets::AssetRecord *record = registry.Find(target);
                if (record == nullptr)
                    return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::DependencyUnavailable));

                const PrefabDependencyKind kind = DependencyKind(document, target);
                const bool semanticPrefab = kind != PrefabDependencyKind::Resource;
                if (semanticPrefab && record->type.Value() != PrefabAssetType)
                    return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::DependencyTypeMismatch));

                const auto targetSource = sourceById.find(target);
                if (record->type.Value() == PrefabAssetType && targetSource == sourceById.end())
                    return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::DependencyUnavailable));
                if (semanticPrefab && (targetSource == sourceById.end() ||
                                       !AuthoredRevisionsMatch(document, target, kind, targetSource->second->sourceRevision)))
                    return Result<PrefabDependencyGraphSnapshot>::Failure(MakeError(PrefabErrors::DependencyRevisionMismatch));

                if (const auto captured = CaptureNode(nodes, *record, std::nullopt, budget); captured.HasError())
                    return Result<PrefabDependencyGraphSnapshot>::Failure(captured.ErrorValue());
                if (const auto charged = Charge(budget); charged.HasError())
                    return Result<PrefabDependencyGraphSnapshot>::Failure(charged.ErrorValue());
                edges.push_back({document.assetId, target, kind});
            }
        }

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
            PrefabDependencyGraphSnapshot{registry.Revision(), std::move(canonicalNodes), std::move(edges), std::move(reverseEdges)});
    }
}  // namespace Horo::Prefab
