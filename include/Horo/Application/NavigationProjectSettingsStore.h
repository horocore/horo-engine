#pragma once
/** @file NavigationProjectSettingsStore.h
 * @brief Explicit host-owned per-file durable publication of navigation project policy.
 */
#include "Horo/Application/NavigationProjectSettings.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "Horo/Foundation/Platform.h"

#include <filesystem>
#include <memory>

namespace Horo::Application {
    /** @brief Owned policy plus exact file bytes used for external-conflict admission. */
    struct NavigationSettingsFileSnapshot final {
        std::shared_ptr<const Application::NavigationProjectSettings> settings;
        Assets::AssetSourceIdentity identity;
    };

    /** @brief Irreversible publication and durability are separate; committed with an error is not rollback. */
    struct NavigationSettingsPublication final {
        NavigationSettingsFileSnapshot snapshot;
        std::optional<Error> durabilityError;
    };

    /** @brief Borrows a host filesystem and an authorized fixed policy path; never discovers a project or provider.
     * @details The host protects the canonical parent namespace. Source/cook/definition files are never changed by this store.
     */
    class NavigationProjectSettingsStore final {
    public:
        /** @brief Binds the durable owner and exact project path.
         * @param files Host filesystem, outlives this store. @param projectRoot Explicit authorized canonical root. @param path Absolute
         * normalized project-owned .horo/navigation.json.
         */
        NavigationProjectSettingsStore(DurableFileSystem &files, std::filesystem::path projectRoot, std::filesystem::path path);
        /** @brief Captures bounded source policy and its exact digest. @return Validated policy or original parse/I/O failure. */
        [[nodiscard]] Result<NavigationSettingsFileSnapshot> Load() const;
        /** @brief Atomically publishes one policy after a locked exact-byte/revision conflict check.
         * @param expected Prior Load receipt, or absence requiring an absent destination. @param candidate Validated new policy.
         * @return Committed receipt (possibly durabilityError) or precommit failure preserving prior bytes.
         * @pre New revision is exactly previous+1 (or1 on creation); no revision wraps.
         */
        [[nodiscard]] Result<NavigationSettingsPublication> Save(const std::optional<NavigationSettingsFileSnapshot> &expected,
                                                                 const Application::NavigationProjectSettings &candidate);

    private:
        DurableFileSystem &files_;
        std::filesystem::path root_;
        std::filesystem::path path_;
    };
}  // namespace Horo::Application
