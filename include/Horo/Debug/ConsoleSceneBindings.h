#pragma once

/** @file ConsoleSceneBindings.h @brief Explicit read-only console bindings over the existing Scene service authority. */
#include "Horo/Debug/CommandExecutor.h"

namespace Horo::Runtime {
    class RuntimeSceneService;
}

namespace Horo::Debug {
    /** @brief Finite inspection work; a huge/sparse world cannot turn one command into an unbounded owner-frame scan. */
    struct ConsoleSceneInspectionLimits final {
        std::size_t maximumVisitedSlots{4096};
        std::size_t maximumReportedEntities{256};
    };

    /** @brief Binds only admitted canonical scene inspection commands to an explicitly supplied owning service.
     * @param registry Immutable composed command inventory.
     * @param scene Existing process/application Scene service; shared lifetime is retained through pending requests.
     * @param limits Finite slot/row budgets; output uses the registry's existing byte budget.
     * @return Owned read-only command bindings, or typed composition failure. No Scene query occurs during binding.
     * @pre Construct on the Scene owner thread and use that same owner in CommandExecutor.
     * @note DevelopmentOnly implementation is omitted outside Editor/Development. Other profiles contribute no handlers.
     */
    [[nodiscard]] Result<std::vector<CommandBinding>> BindConsoleSceneCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                               std::shared_ptr<Runtime::RuntimeSceneService> scene,
                                                                               ConsoleSceneInspectionLimits limits = {});
}  // namespace Horo::Debug
