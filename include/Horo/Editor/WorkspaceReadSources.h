#pragma once

/** @file WorkspaceReadSources.h
 * @brief Host-composed workspace read authorities without native backend selection in the GUI.
 */
#include "Horo/Assets/AssetId.h"
#include "Horo/Assets/AssetRegistrySource.h"
#include "Horo/Platform/ProjectReadFiles.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace Horo::Editor {
    /** @brief Complete owned source capabilities for one never-reused workspace incarnation, both bound to generation one. */
    struct WorkspaceReadSources final {
        Assets::AssetId owner;
        std::shared_ptr<const Assets::IAssetRegistrySource> registry;
        std::shared_ptr<const Platform::IProjectReadFiles> world;
    };

    /** @brief Application-owned selection of native or restricted readers; capture performs no registration or ambient discovery. */
    struct WorkspaceReadSourceFactory final {
        /** @brief Captures both readers under a fresh owner for the exact admitted absolute project root.
         * @param root Host-authorized project root. @return Complete retained source authorities or original typed failure. */
        using Capture = std::function<Result<WorkspaceReadSources>(const std::filesystem::path &root)>;
        Capture capture; /**< Synchronous project-open callback; returns owned readers and retains no GUI stack references. */
    };

    /** @brief Admits one complete host capture; missing factories never fall back to a native backend.
     * @param factory Explicit borrowed host capability, or null when unavailable. @param root Absolute admitted project root.
     * @return Complete owned pair or original capture failure; unavailable/malformed composition returns a typed read failure.
     * @details The factory must issue a never-reused owner and bind both readers to that identity at generation one.
     * Capture may perform bounded project-open I/O; presentation draw paths never call it.
     */
    [[nodiscard]] Result<WorkspaceReadSources> CaptureWorkspaceReadSources(const WorkspaceReadSourceFactory *factory,
                                                                           const std::filesystem::path &root);
}  // namespace Horo::Editor
