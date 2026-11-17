#include "ExtensionScriptCliInternal.h"
#include "Horo/Cli/CliErrors.h"

namespace Horo::Application::Internal {
    Result<std::shared_ptr<const AuthenticatedExtensionScript>> AuthenticateExtensionScript(const ExtensionScriptOptions &options,
                                                                                            const CancellationToken &cancellation) {
        return AuthenticateModuleScriptPackage(options, cancellation, Cli::CliErrors::ExecutionCancelled);
    }

    Result<std::shared_ptr<const Security::ArtifactVerifier>> RevalidateExtensionScript(const AuthenticatedExtensionScript &packet) {
        return RevalidateModuleScriptPackage(packet);
    }

    Result<std::vector<std::byte>> ReadExtensionScriptFile(const std::filesystem::path &path, std::size_t maximum) {
        return ReadModuleScriptFile(path, maximum);
    }

    Result<std::filesystem::path> ExtensionScriptRelative(const std::filesystem::path &root, const std::filesystem::path &relative) {
        return ModuleScriptRelative(root, relative);
    }
}  // namespace Horo::Application::Internal
