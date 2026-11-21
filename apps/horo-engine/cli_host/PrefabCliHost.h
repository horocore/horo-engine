#pragma once

/** @file PrefabCliHost.h @brief Typed terminal adapters for prefab cooking and migration planning. */
#include "Horo/Cli/CliDispatcher.h"

namespace Horo::Application::Internal {
    /** @brief Describe the shared headless project cook command for registry admission and help. @return Command descriptor. */
    [[nodiscard]] Cli::CliCommandDescriptor DescribePrefabCookCommand();

    /** @brief Bind the real native prefab use case without starting work. @return Adapter and its exact declared grant. */
    [[nodiscard]] Cli::CliCommandAdapterRegistration MakePrefabCookCommandAdapter();

    /** @brief Describe the explicit-project prefab migration plan command. @return Command descriptor. */
    [[nodiscard]] Cli::CliCommandDescriptor DescribePrefabMigrationPlanCommand();

    /** @brief Bind the read-only project preflight command. @return Adapter and its exact declared grant. */
    [[nodiscard]] Cli::CliCommandAdapterRegistration MakePrefabMigrationPlanCommandAdapter();
}  // namespace Horo::Application::Internal
