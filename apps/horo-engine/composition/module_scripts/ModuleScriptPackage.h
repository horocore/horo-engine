#pragma once
#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "Horo/Extensions/ScriptModuleRuntime.h"
#include "Horo/Security/ArtifactSignature.h"
#include "ScriptModuleProgram.h"

#include <filesystem>

namespace Horo::Application::Internal {
    struct ExtensionScriptOptions final {
        std::filesystem::path packageRoot;
        std::filesystem::path hostTrust;
        std::filesystem::path temporaryRoot;
        std::filesystem::path script;
        std::string package;
        std::string module;
        std::string import;
        std::string publisher;
        bool moduleOperation{};
        bool refreshReference{};
        Extensions::ScriptModuleRuntimeKind runtime{Extensions::ScriptModuleRuntimeKind::Lua54};
    };

    struct ExtensionScriptArtifact final {
        std::filesystem::path relative;
        std::vector<std::byte> bytes;
        Security::DetachedSignatureEnvelope signature;
        std::optional<Security::VerifiedArtifactEvidence> evidence;
    };

    struct AuthenticatedExtensionScript final {
        ExtensionScriptOptions trustOptions;
        Extensions::ExtensionManifest manifest;
        std::shared_ptr<const Security::ArtifactVerifier> verifier;
        std::vector<ExtensionScriptArtifact> artifacts;
        std::string source;
        std::string service;
    };

    class ExtensionScriptArtifactGate final : public Security::NativeArtifactGate {
    public:
        ExtensionScriptArtifactGate(std::filesystem::path, std::shared_ptr<const AuthenticatedExtensionScript>);
        Result<Security::VerifiedArtifactEvidence> Verify(const std::filesystem::path &) const override;

    private:
        std::filesystem::path root_;
        std::shared_ptr<const AuthenticatedExtensionScript> packet_;
    };

    [[nodiscard]] Result<std::shared_ptr<const AuthenticatedExtensionScript>> AuthenticateModuleScriptPackage(
        const ExtensionScriptOptions &, const CancellationToken &, const ErrorCodeDescriptor &cancellationError);
    [[nodiscard]] Result<std::shared_ptr<const Security::ArtifactVerifier>> RevalidateModuleScriptPackage(
        const AuthenticatedExtensionScript &);
    [[nodiscard]] Result<std::filesystem::path> StageModuleScriptPackage(const ExtensionScriptOptions &,
                                                                         const AuthenticatedExtensionScript &);
    [[nodiscard]] Result<std::vector<std::byte>> ReadModuleScriptFile(const std::filesystem::path &, std::size_t);
    [[nodiscard]] Result<std::filesystem::path> ModuleScriptRelative(const std::filesystem::path &, const std::filesystem::path &);
    [[nodiscard]] Result<std::shared_ptr<const Extensions::ScriptModuleProgram>> MakeModuleScriptProgram(
        const std::filesystem::path &root, std::shared_ptr<const AuthenticatedExtensionScript>);
    /** Explicit product admission; source/native consent and signatures are independent requirements. */
    [[nodiscard]] Result<std::shared_ptr<const Extensions::ScriptModuleProgram>> CaptureModuleScriptProgram(const ExtensionScriptOptions &,
                                                                                                            bool nativeConsent,
                                                                                                            bool scriptConsent,
                                                                                                            const CancellationToken &);
}  // namespace Horo::Application::Internal
