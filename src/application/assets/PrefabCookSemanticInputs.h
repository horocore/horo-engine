#pragma once
/** @file PrefabCookSemanticInputs.h @brief Dependency-local immutable cook input selection. */
#include "Horo/Application/PrefabSceneCookHost.h"
#include "Horo/Prefab/PrefabDependencyGraph.h"
#include "Horo/Prefab/PrefabSourceResolver.h"

namespace Horo::Application::PrefabCookDetail {
    /** @brief Complete supported root input closure, computed before any output work is scheduled. */
    struct SemanticCookInputs final {
        std::vector<Assets::AssetId> sources;
        Sha256Digest producerSettings;
    };

    /** @brief Captures source/nested/variant/resource and used-schema semantics from actual pinned inputs.
     * @param inputs Whole immutable host capture, retained separately for final freshness validation.
     * @param graph Validated prefab graph from the same registry publication.
     * @param resolver Immutable parsed prefab documents from that graph.
     * @param root Artifact whose complete effective dependencies are selected.
     * @param schemas Immutable validating component/behavior metadata, or absent for schema-free sources.
     * @param catalog Explicit resource producers, each required to declare complete source dependencies.
     * @param target Selected output target. @param maximumInputs Finite closure/work ceiling.
     * @param cancellation Cooperative stop checked during traversal.
     * @return Canonical source selection and provider/schema commitment, or typed missing/cycle/unsupported/budget failure.
     */
    [[nodiscard]] Result<SemanticCookInputs> CaptureSemanticCookInputs(
        const Assets::AssetCookInputSnapshot &inputs, const Prefab::PrefabDependencyGraphSnapshot &graph,
        const Prefab::PrefabSourceResolverSnapshot &resolver, Assets::AssetId root,
        const std::shared_ptr<const PrefabCookSchemaContext> &schemas, const Assets::CookerCatalogSnapshot &catalog,
        const AssetCookTargetId &target, std::size_t maximumInputs, const CancellationToken &cancellation,
        std::span<const Assets::AssetId> sceneResources = {});
}  // namespace Horo::Application::PrefabCookDetail
