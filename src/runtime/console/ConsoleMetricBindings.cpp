#include "ConsoleMetricCommandInternal.h"
#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleTelemetryBindings.h"

#if !defined(HORO_DEBUG_COMMANDS_ENABLED) || defined(HORO_PROFILE_SHIPPING)
#error "Console metric diagnostics must be omitted from Shipping artifacts."
#endif

namespace Horo::Debug {
    /** @copydoc BindConsoleTelemetryCommands */
    Result<std::vector<CommandBinding>> BindConsoleTelemetryCommands(std::shared_ptr<const CommandRegistry> registry) {
        if (!registry || registry->Profile() != CompiledConsoleProfile())
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        return Internal::BindMetricCommand(std::move(registry));
    }
}  // namespace Horo::Debug
