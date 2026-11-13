#ifndef HORO_EDITOR_PREFAB_HIERARCHY_PROJECTION_H
#define HORO_EDITOR_PREFAB_HIERARCHY_PROJECTION_H

/** @file PrefabHierarchyProjection.h
 * @brief Workspace-owned immutable prefab hierarchy presentation and selection provenance.
 */
#include "Horo/Prefab/PrefabSceneIdentityRemap.h"
#include "Horo/Scene/SceneRuntimeConversion.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {
    /** @brief Stable placement or exact nested member; never a runtime entity or panel row ordinal. */
    struct PrefabHierarchyTarget final {
        Prefab::PrefabInstanceId instance;
        std::optional<Prefab::ExpandedPrefabObjectKey> member; /**< Empty selects the containing placement. */
        [[nodiscard]] auto operator<=>(const PrefabHierarchyTarget &) const noexcept = default;
    };

    /** @brief Read-only source-qualified row; no component mutation authority is carried. */
    struct PrefabValueDiffPresentation final {
        std::string target;
        std::string source;
        std::string effective;
        std::optional<std::string> schemaDefault;
        const char *originKey{}; /**< Closed localized vocabulary chosen from canonical origin evidence during preparation. */
        const char *stateKey{};  /**< Null for resolved values; preserved conflict/orphan/opaque state otherwise. */
        bool changed{};
        bool sourcePresent{true};
        bool effectivePresent{true};
    };

    /** @brief Read-only source-qualified row; no component mutation authority is carried. */
    struct PrefabHierarchyEntry final {
        PrefabHierarchyTarget target;
        std::optional<PrefabHierarchyTarget> parent;
        Assets::AssetId source;
        std::string name;
        std::string sourceLabel; /**< Cached stable identity text; Draw never formats or loads source metadata. */
        bool hasOverrides{};
        std::optional<Error> failure;
        std::optional<Prefab::PrefabSceneObjectId> sceneObject; /**< Canonical collision-checked remap, never a panel alias. */
        std::vector<PrefabValueDiffPresentation> valueDiff;
        bool valueDiffIncomplete{};
        std::optional<Error> valueInspectionFailure;
    };

    /** @brief Complete detached projection pinned to its source resolution; published only after preparation succeeds. */
    struct PrefabHierarchyProjection final {
        Assets::AssetId documentSession;
        std::uint64_t publication{};
        std::uint64_t documentRevision{};
        std::shared_ptr<const SceneSource::ScenePrefabProjection> sources;
        std::vector<PrefabHierarchyEntry> entries;

        /** @brief Finds exact provenance without allocation or source I/O. @param target Stable selection. @return Borrowed row or null. */
        [[nodiscard]] const PrefabHierarchyEntry *Find(const PrefabHierarchyTarget &target) const noexcept;
    };

    /**
     * @brief Prepares presentation from the canonical resolver output without re-parsing source or mutating a document.
     * @param session Host-issued workspace incarnation. @param publication Never-reused owner publication generation within that workspace.
     * @param revision Exact containing document revision.
     * @param sources Complete canonical placement projection, retained immutably by the result.
     * @param maximumRows Explicit workspace presentation budget including repairable placement roots.
     * @param authored Existing scene identities reserved before any prefab remapping. @param limits Canonical expansion policy.
     * @return Complete projection, or typed invalid/capacity failure; no partial rows escape.
     * @details Explicit document/source refresh only. Draw and selection polling borrow the retained result.
     */
    [[nodiscard]] Result<PrefabHierarchyProjection> BuildPrefabHierarchyProjection(
        Assets::AssetId session, std::uint64_t publication, std::uint64_t revision,
        std::shared_ptr<const SceneSource::ScenePrefabProjection> sources, std::size_t maximumRows,
        std::span<const SceneSource::SceneObjectSnapshot> authored, const Prefab::PrefabLimitProfile &limits);
}  // namespace Horo::Editor

#endif
