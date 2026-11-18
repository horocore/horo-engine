#pragma once

/** @file PrefabCliHost.h @brief Typed terminal adapter for the existing prefab cook operation. */
#include "Horo/Cli/CliDispatcher.h"

namespace Horo::Application::Internal {
    /** @brief Describe the shared headless project cook command for registry admission and help. @return Command descriptor. */
    [[nodiscard]] Cli::CliCommandDescriptor DescribePrefabCookCommand();

    /** @brief Bind the real native prefab use case without starting work. @return Adapter and its exact declared grant. */
    [[nodiscard]] Cli::CliCommandAdapterRegistration MakePrefabCookCommandAdapter();
}  // namespace Horo::Application::Internal
