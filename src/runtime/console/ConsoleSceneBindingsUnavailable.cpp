#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleSceneBindings.h"

namespace Horo::Debug {
    /** @copydoc BindConsoleSceneCommands */
    Result<std::vector<CommandBinding>> BindConsoleSceneCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                 std::shared_ptr<Runtime::RuntimeSceneService>,
                                                                 ConsoleSceneInspectionLimits limits) {
        if (!registry || registry->Profile() != CompiledConsoleProfile() || limits.maximumVisitedSlots == 0 ||
            limits.maximumVisitedSlots > 16384 || limits.maximumReportedEntities == 0 || limits.maximumReportedEntities > 512)
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        return Result<std::vector<CommandBinding>>::Success({});
    }
}  // namespace Horo::Debug
