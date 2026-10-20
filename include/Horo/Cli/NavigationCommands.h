#pragma once

/** @file NavigationCommands.h @brief Navigation CLI adapters over the shared project bake application. */
#include "Horo/Application/NavigationBakeApplication.h"
#include "Horo/Cli/CliDispatcher.h"

namespace Horo::Cli {
    /** @brief Invocation-bound project acquisition, with trust, containment and deadline retained by the returned owner. */
    using NavigationCommandApplication =
        std::function<Result<std::shared_ptr<Application::NavigationBakeApplication>>(const CliCommandRequest &, CliExecutionContext &)>;

    /** @brief Returns inert navigation bake metadata for discovery and CI use.
     * @return One command supporting full-project, selected-surface and validate-only execution.
     */
    [[nodiscard]] CliCommandDescriptor DescribeNavigationBakeCommand();
    /** @brief Creates a thin adapter; no capture, provider discovery or project mutation occurs.
     * @param application Explicit host acquisition using the admitted project option and invocation stop policy.
     * @return Registration requiring the exact horo.navigation.bake capability.
     * The adapter observes terminal state before returning, including cancellation and presentation failures.
     */
    [[nodiscard]] CliCommandAdapterRegistration MakeNavigationBakeCommandAdapter(NavigationCommandApplication application);
}  // namespace Horo::Cli
