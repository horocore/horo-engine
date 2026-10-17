#pragma once

/** @file ProjectCollisionSettingsStore.h
 * @brief Host-owned revision-fenced durable publication of the sole Project collision document.
 */

#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Editor/ProjectMutation.h"

namespace Horo::Editor {
    /** @brief Exact project-contained reference source captured by the explicit impact reviewer. */
    struct CollisionSourceFence final {
        std::filesystem::path path;
        Assets::AssetSourceIdentity identity;
    };

    /** @brief Complete reviewed source inventory and exact physical source identities, borrowed only during Save.
     * @details The owning Assets scanner rechecks the whole inventory under the Project mutation lease. The span
     * remains alive for this synchronous nonreentrant call; it never lends document or runtime mutation authority. */
    struct CollisionPublicationSourceFence final {
        Assets::AssetRegistrySnapshot inventory;
        std::span<const CollisionSourceFence> sources;
    };

    /** @brief Replacement result distinguishes published bytes from uncertain directory durability. */
    struct ProjectCollisionPublication final {
        Application::ProjectCollisionFileSnapshot snapshot;
        std::optional<Error> durabilityError;
    };

    /** @brief Explicit owner-thread persistence adapter; no editor state or active world is discovered. */
    class ProjectCollisionSettingsStore final {
    public:
        /** @brief Borrows durable operations and shared project mutation admission for one authorized project.
         * @param files Host-owned durable filesystem. @param mutations Shared host project mutation coordinator.
         * @param projectRoot Canonical absolute authorized root. @param projectId Exact loaded Project metadata identity.
         * @param source Root-bound physical inventory owner, alive until this store is destroyed.
         */
        ProjectCollisionSettingsStore(DurableFileSystem &files, ProjectMutationCoordinator &mutations, std::filesystem::path projectRoot,
                                      std::string projectId, const Assets::IAssetRegistrySource &source);
        /** @brief Reads the exact single authority without migration or runtime defaults.
         * @return Complete immutable file snapshot or an actionable typed read/validation failure.
         */
        [[nodiscard]] Result<Application::ProjectCollisionFileSnapshot> Load() const;
        /** @brief Publishes one fully validated revision under the shared project and source publication leases.
         * @param expected Last accepted complete source, or absence only for explicit new-project creation.
         * @param candidate Complete next revision belonging to this exact project.
         * @param reviewed Complete reviewed inventory and owner sources, rechecked under the shared project mutation lease.
         * Omission is for low-level explicit source creation/publication; the semantic editor session always supplies this fence.
         * @return Published snapshot and possible durability uncertainty, or zero-publication rejection.
         * @post Active worlds retain their prior immutable schema; this operation does not activate a generation.
         */
        [[nodiscard]] Result<ProjectCollisionPublication> Save(
            const std::optional<Application::ProjectCollisionFileSnapshot> &expected,
            const Application::ProjectCollisionSettings &candidate,
            const std::optional<CollisionPublicationSourceFence> &reviewed = std::nullopt) const;

    private:
        DurableFileSystem &files_;
        ProjectMutationCoordinator &mutations_;
        std::filesystem::path root_;
        std::string projectId_;
        const Assets::IAssetRegistrySource &source_;
    };
}  // namespace Horo::Editor
