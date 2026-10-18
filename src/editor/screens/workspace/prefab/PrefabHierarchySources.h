#ifndef HORO_EDITOR_PREFAB_HIERARCHY_SOURCES_H
#define HORO_EDITOR_PREFAB_HIERARCHY_SOURCES_H
/** @file PrefabHierarchySources.h
 * @brief Explicit bounded source capture for one workspace prefab projection refresh.
 */
#include "Horo/Assets/AssetCookInputSnapshot.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Prefab/PrefabSourceResolver.h"

#include <functional>

namespace Horo::Editor {
    /** @brief Complete selected source bytes pinned alongside their canonical resolver for explicit freshness checks. */
    struct PrefabHierarchySources final {
        Prefab::PrefabSourceResolverSnapshot resolver;
        Assets::AssetCookInputSnapshot inputs;
    };

    /** @brief Prepared sources plus the host lease retained until immediate owner publication. */
    struct PreparedPrefabHierarchySources final {
        PrefabHierarchySources sources;
        ProjectMutationLease lease;
    };

    /** @brief Explicit host-owned loader; no application discovery or filesystem work is allowed in Draw. */
    using PrefabHierarchySourceCapture =
        std::function<Result<PreparedPrefabHierarchySources>(const Assets::AssetRegistrySnapshot &, std::span<const Assets::AssetId>,
                                                             const Prefab::PrefabLimitProfile &, const CancellationToken &)>;

    /**
     * @brief Captures only the requested prefab source closure using the canonical asset pair reader and resolver.
     * @param root Explicit authorized project root. @param registry Original pinned authoritative asset publication.
     * @param roots Requested placement source identities. @param limits Canonical project prefab policy.
     * @param version Exact admitted project version. @param cancellation Cooperative stop checked before every bounded capture and before
     * publication.
     * @return Complete resolver and selected capture, or typed read/identity/parser/capacity/stale/cancel failure.
     * @pre The host retains its project lease through this call and owner publication; queries and Draw never call it.
     * @details At most 256 sources and 64 MiB source/sidecar storage are admitted, retaining the canonical 32 MiB per-source
     * ceiling. This bounds a presentation refresh without narrowing valid individual Prefab source files. Containment
     * and observed digest checks match AssetCookInputSnapshot; no OS-atomic snapshot or retained-directory claim is made.
     */
    [[nodiscard]] Result<PrefabHierarchySources> CapturePrefabHierarchySources(
        const std::filesystem::path &root, const Assets::AssetRegistrySnapshot &registry, std::span<const Assets::AssetId> roots,
        const Prefab::PrefabLimitProfile &limits, const Application::HoroVersion &version, const CancellationToken &cancellation = {});
}  // namespace Horo::Editor

#endif
