#pragma once

#include "Horo/Application/PackageApplication.h"

#include <compare>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace Horo::AI {
    class DecisionNodeGeneration;
}

namespace Horo::Editor {

    class ScreenRegistry;

    /**
     * @file DefaultScreenFactories.h
     * @brief Registration helpers for default application GUI screens.
     */
    void RegisterWelcomeScreen(ScreenRegistry &registry);
    void RegisterProjectCreationScreen(ScreenRegistry &registry);
    void RegisterProjectLoadingScreen(ScreenRegistry &registry);

    /** @brief Exact application project binding and already published package role composition. */
    struct PackageWorkspaceDecisionCatalog final {
        std::uint64_t project{};
        std::uint64_t activation{};
        [[nodiscard]] auto operator<=>(const PackageWorkspaceDecisionCatalog &) const = default;
    };

    /** @brief Explicit project application access supplied by the executable root, never discovered by a screen.
     * @details Acquire may report preparation pending; Pump receives the actual play lifecycle boundary. Retire cancels
     * admission, while the executable retains pending session owners through its independent shutdown drain.
     */
    struct PackageWorkspaceAccess final {
        std::function<Result<std::shared_ptr<Application::PackageApplication>>(const std::filesystem::path &)> acquire;
        std::function<Result<void>(const std::filesystem::path &, Packages::PackageActivationBoundary)> pump;
        std::function<Result<void>(const std::filesystem::path &)> retire;
        std::function<bool(const std::filesystem::path &)> activationTrustRequired; /**< Read-only evidence for the exact project. */
        std::function<std::optional<PackageWorkspaceDecisionCatalog>(const std::filesystem::path &)> decisionCatalog;
        std::function<Result<std::vector<std::shared_ptr<const AI::DecisionNodeGeneration>>>(const std::filesystem::path &,
                                                                                             const PackageWorkspaceDecisionCatalog &)>
            acquireDecisionNodes;
        bool gameplayModuleOwner{}; /**< The executable supplied a gameplay host. Its execution approval remains separately enforced. */
    };

    /** @brief Registers the workspace with explicit optional package access; missing access remains unavailable.
     * @param registry Screen factory registry. @param packages Application-root package capability. */
    void RegisterEditorWorkspaceScreen(ScreenRegistry &registry, PackageWorkspaceAccess packages = {});

}  // namespace Horo::Editor
