#include "Horo/Extensions/ExtensionErrors.h"
#include "ModuleScriptPackage.h"

namespace Horo::Application::Internal {
    Result<std::shared_ptr<const Extensions::ScriptModuleProgram>> MakeModuleScriptProgram(
        const std::filesystem::path &root, std::shared_ptr<const AuthenticatedExtensionScript> packet) {
        using Output = Result<std::shared_ptr<const Extensions::ScriptModuleProgram>>;
        if (!packet || !packet->trustOptions.moduleOperation)
            return Output::Failure(MakeError(Extensions::ExtensionErrors::ScriptInvocationInvalid));
        Extensions::ScriptModuleProgramInput input{.runtime = packet->trustOptions.runtime,
                                                   .package = packet->manifest.id,
                                                   .moduleId = packet->trustOptions.moduleId,
                                                   .root = root,
                                                   .source = packet->trustOptions.script,
                                                   .currentTrust = std::make_shared<ExtensionScriptArtifactGate>(root, packet)};
        input.artifacts.reserve(packet->artifacts.size());
        for (const auto &artifact : packet->artifacts) {
            if (!artifact.evidence)
                return Output::Failure(MakeError(Extensions::ExtensionErrors::CapabilityUnavailable));
            input.artifacts.emplace_back(artifact.relative, std::shared_ptr<const std::vector<std::byte>>{packet, &artifact.bytes},
                                         *artifact.evidence);
        }
        return Extensions::ScriptModuleProgram::Capture(std::move(input));
    }

    Result<std::shared_ptr<const Extensions::ScriptModuleProgram>> CaptureModuleScriptProgram(const ExtensionScriptOptions &options,
                                                                                              bool nativeConsent, bool scriptConsent,
                                                                                              const CancellationToken &cancellation) {
        using Output = Result<std::shared_ptr<const Extensions::ScriptModuleProgram>>;
        if (!nativeConsent || !scriptConsent || !options.moduleOperation)
            return Output::Failure(MakeError(Extensions::ExtensionErrors::CapabilityUnavailable));
        auto packet = AuthenticateModuleScriptPackage(options, cancellation, Extensions::ExtensionErrors::ScriptInvocationCancelled);
        if (packet.HasError())
            return Output::Failure(packet.ErrorValue());
        return MakeModuleScriptProgram(options.packageRoot, std::move(packet).Value());
    }
}  // namespace Horo::Application::Internal
