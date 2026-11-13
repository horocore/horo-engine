#include "ExtensionScriptCliInternal.h"

namespace Horo::Application::Internal {
    Result<std::filesystem::path> StageExtensionScript(const ExtensionScriptOptions &options, const AuthenticatedExtensionScript &packet) {
        return StageModuleScriptPackage(options, packet);
    }
}  // namespace Horo::Application::Internal
