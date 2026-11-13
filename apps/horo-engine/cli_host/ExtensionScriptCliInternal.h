#pragma once

#include "ExtensionScriptCliHost.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "Horo/Security/ArtifactSignature.h"
#include "ModuleScriptPackage.h"

namespace Horo::Extensions {
    class ScriptModuleHostServices;
}

namespace Horo::Application::Internal {
    [[nodiscard]] Result<ExtensionScriptOptions> ParseExtensionScriptOptions(const Cli::CliCommandRequest &request);
    [[nodiscard]] Result<std::shared_ptr<const AuthenticatedExtensionScript>> AuthenticateExtensionScript(
        const ExtensionScriptOptions &options, const CancellationToken &cancellation);
    /** @brief Reads current independent policy/time and verifies immutable captured artifacts before each admission. */
    [[nodiscard]] Result<std::shared_ptr<const Security::ArtifactVerifier>> RevalidateExtensionScript(
        const AuthenticatedExtensionScript &packet);
    [[nodiscard]] Result<std::filesystem::path> StageExtensionScript(const ExtensionScriptOptions &options,
                                                                     const AuthenticatedExtensionScript &packet);
    [[nodiscard]] Result<std::vector<std::byte>> ReadExtensionScriptFile(const std::filesystem::path &path, std::size_t maximum);
    [[nodiscard]] Result<std::filesystem::path> ExtensionScriptRelative(const std::filesystem::path &root,
                                                                        const std::filesystem::path &relative);
    [[nodiscard]] Result<Cli::CliCommandResult> ExecuteCapturedModuleProgram(Extensions::ScriptModuleHostServices &,
                                                                             std::shared_ptr<const Extensions::ScriptModuleProgram>,
                                                                             Extensions::ScriptCapabilityScopeLease project,
                                                                             Extensions::ScriptCapabilityScopeLease operation,
                                                                             Cli::CliExecutionContext &, bool refreshReference = false);
}  // namespace Horo::Application::Internal
