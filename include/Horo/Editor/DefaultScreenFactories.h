#pragma once

#include "Horo/Application/PackageApplication.h"

#include <functional>

namespace Horo::Editor {

    class ScreenRegistry;

    /**
     * @file DefaultScreenFactories.h
     * @brief Registration helpers for default application GUI screens.
     */
    void RegisterWelcomeScreen(ScreenRegistry &registry);
    void RegisterProjectCreationScreen(ScreenRegistry &registry);
    void RegisterProjectLoadingScreen(ScreenRegistry &registry);

    /** @brief Explicit project application access supplied by the executable root, never discovered by a screen.
     * @details Acquire may report preparation pending; Pump receives the actual play lifecycle boundary. Retire cancels
     * admission, while the executable retains pending session owners through its independent shutdown drain.
     */
    struct PackageWorkspaceAccess final {
        std::function<Result<std::shared_ptr<Application::PackageApplication>>(const std::filesystem::path &)> acquire;
        std::function<Result<void>(const std::filesystem::path &, Packages::PackageActivationBoundary)> pump;
        std::function<Result<void>(const std::filesystem::path &)> retire;
    };

    /** @brief Registers the workspace with explicit optional package access; missing access remains unavailable.
     * @param registry Screen factory registry. @param packages Application-root package capability. */
    void RegisterEditorWorkspaceScreen(ScreenRegistry &registry, PackageWorkspaceAccess packages = {});

}  // namespace Horo::Editor
