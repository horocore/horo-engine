#pragma once

/** @file ConsoleMetricCommandInternal.h @brief Non-installed binding shared by explicit Editor and non-shipping product composition. */
#include "Horo/Debug/CommandExecutor.h"

namespace Horo::Debug::Internal {
    /** @brief Validates admitted metric metadata and binds the existing process diagnostic query without starting/querying it.
     * @param registry Existing immutable non-shipping inventory; the public product facade additionally checks compiled-profile affinity.
     * @return Owned binding or typed composition failure; a missing admitted command contributes nothing.
     * Construction captures the actual owner identity. No metric producer, store or backend is created.
     */
    [[nodiscard]] Result<std::vector<CommandBinding>> BindMetricCommand(std::shared_ptr<const CommandRegistry> registry);
}  // namespace Horo::Debug::Internal
