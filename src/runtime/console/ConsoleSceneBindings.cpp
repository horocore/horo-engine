#include "Horo/Debug/ConsoleSceneBindings.h"

#include "ConsoleSceneCommandInternal.h"
#include "Horo/Debug/ConsoleProduct.h"

#if !defined(HORO_PROFILE_EDITOR) && !defined(HORO_PROFILE_DEVELOPMENT)
#error "Development scene inspection must be omitted from other product artifacts."
#endif

namespace Horo::Debug {
    /** @copydoc BindConsoleSceneCommands */
    Result<std::vector<CommandBinding>> BindConsoleSceneCommands(std::shared_ptr<const CommandRegistry> registry,
                                                                 std::shared_ptr<Runtime::RuntimeSceneService> scene,
                                                                 ConsoleSceneInspectionLimits limits) {
        if (!registry || registry->Profile() != CompiledConsoleProfile())
            return Result<std::vector<CommandBinding>>::Failure(MakeCommandError(CommandError::InvalidDescriptor));
        auto source = std::make_shared<Internal::ConsoleSceneInspection>();
        source->source = std::move(scene);
        return Internal::BindSceneInspectionCommands(std::move(registry), std::move(source), limits);
    }
}  // namespace Horo::Debug
