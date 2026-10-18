#ifndef HORO_EDITOR_PREFAB_HIERARCHY_COMMAND_H
#define HORO_EDITOR_PREFAB_HIERARCHY_COMMAND_H
/** @file PrefabHierarchyCommand.h
 * @brief Typed revision-fenced hierarchy intents; panels never receive document mutation authority.
 */
#include "editor/screens/workspace/prefab/PrefabHierarchyProjection.h"

namespace Horo::Editor {
    /** @brief Closed workspace actions supported for source-owned prefab rows. */
    enum class PrefabHierarchyAction : std::uint8_t {
        Select,
        OpenSource,
        LocateInstance,
        DuplicateInstance,
        DeleteInstance,
        ReparentInstance
    };

    /** @brief Exact presentation evidence copied at gesture time and checked by the document owner. */
    struct PrefabHierarchyCommand final {
        Assets::AssetId documentSession;
        std::uint64_t publication{};
        std::uint64_t documentRevision{};
        PrefabHierarchyTarget target;
        PrefabHierarchyAction action{PrefabHierarchyAction::Select};
        std::optional<SceneSource::SceneObjectId> destinationParent; /**< Only ordinary authored scene parents are representable. */
    };

    /**
     * @brief Checks a gesture against the exact current immutable projection before any owner mutation or source navigation.
     * @param command Captured typed intent. @param current Current owner publication, never a panel-provided replacement.
     * @return Borrowed exact target entry or typed stale/boundary failure. No document or selection is changed.
     * @details Root placement deletion/reparenting uses existing SceneDocument prefab commands. Expanded members cannot
     * become ordinary SceneObject mutation targets; their owning source must be edited instead.
     */
    [[nodiscard]] Result<const PrefabHierarchyEntry *> ValidatePrefabHierarchyCommand(const PrefabHierarchyCommand &command,
                                                                                      const PrefabHierarchyProjection &current);
}  // namespace Horo::Editor

#endif
