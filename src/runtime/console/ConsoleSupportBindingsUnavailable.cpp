#include "Horo/Debug/ConsoleProduct.h"
#include "Horo/Debug/ConsoleSupportBindings.h"

namespace Horo::Debug {
    /** @copydoc BindConsoleSupportBundleCommands */
    Result<std::vector<CommandBinding>> BindConsoleSupportBundleCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                         std::shared_ptr<IConsoleSupportBundleRequests>) {
        if (!registry || registry->Profile() != CompiledConsoleProfile())
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        return Result<std::vector<CommandBinding>>::Success({});
    }
}  // namespace Horo::Debug
