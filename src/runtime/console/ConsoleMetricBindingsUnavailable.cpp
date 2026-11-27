#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleTelemetryBindings.h"

namespace Horo::Debug {
    /** @copydoc BindConsoleTelemetryCommands */
    Result<std::vector<CommandBinding>> BindConsoleTelemetryCommands(std::shared_ptr<const CommandRegistry> registry) {
        if (!registry || registry->Profile() != CompiledConsoleProfile())
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        return Result<std::vector<CommandBinding>>::Success({});
    }
}  // namespace Horo::Debug
