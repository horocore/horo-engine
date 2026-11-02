#pragma once

#include "ExtensionScriptCliHost.h"
#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "Horo/Security/ArtifactSignature.h"

namespace Horo::Application::Internal {
    struct ExtensionScriptOptions final {
        std::filesystem::path packageRoot, hostTrust, temporaryRoot, script;
        std::string package, module, import, publisher;
    };

    struct ExtensionScriptArtifact final {
        std::filesystem::path relative;
        std::vector<std::byte> bytes;
        Security::DetachedSignatureEnvelope signature;
    };

    struct AuthenticatedExtensionScript final {
        ExtensionScriptOptions trustOptions;
        Extensions::ExtensionManifest manifest;
        std::shared_ptr<const Security::ArtifactVerifier> verifier;
        std::vector<ExtensionScriptArtifact> artifacts;
        std::string source;
        std::string service;
    };

    /** @brief Exact immutable artifact gate; native load verifies the captured signed bytes again from owned staging. */
    class ExtensionScriptArtifactGate final : public Security::NativeArtifactGate {
    public:
        ExtensionScriptArtifactGate(std::filesystem::path root, std::shared_ptr<const AuthenticatedExtensionScript> packet);
        Result<Security::VerifiedArtifactEvidence> Verify(const std::filesystem::path &path) const override;

    private:
        std::filesystem::path root_;
        std::shared_ptr<const AuthenticatedExtensionScript> packet_;
    };

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
}  // namespace Horo::Application::Internal
