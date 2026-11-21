#pragma once

/** @file NativePrefabCookComposition.h @brief Headless host composition of the canonical prefab cook. */

#include "Horo/Assets/AssetCookService.h"

#include <chrono>

namespace Horo::Application::Internal {
    /** @brief An explicitly authorized project, target and runtime roots for a joined synchronous cook. */
    struct NativePrefabCookInvocation final {
        std::filesystem::path project;
        AssetCookTargetId target;
        std::vector<Assets::AssetId> runtimeRoots;
        std::chrono::steady_clock::time_point deadline{std::chrono::steady_clock::time_point::max()};
        BuildOutputStore *output{};
        OperationStore *operations{};
        Assets::AssetCookProgressSink progress;
        std::optional<Assets::AssetRecord> *failedSource{}; /**< Reset on entry; filled only with a captured source record. */
    };

    /** @brief Cook one project through PrefabSceneCookHost using admitted headless built-in contributions.
     * @param invocation Exact project directory, target and runtime roots. The caller retains all projection owners.
     * @param cancellation Cooperative token shared by inventory, preparation and publication.
     * @return Canonical generation report or typed failure. No migration or package activation occurs.
     */
    [[nodiscard]] Result<Assets::AssetCookReport> CookNativePrefabProject(const NativePrefabCookInvocation &invocation,
                                                                          const CancellationToken &cancellation);
}  // namespace Horo::Application::Internal
