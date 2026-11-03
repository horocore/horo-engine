#pragma once
#include "Horo/Extensions/ScriptModuleRuntime.h"
#include "Horo/Security/ArtifactSignature.h"

#include <filesystem>

namespace Horo::Extensions {
    struct ScriptModuleCapturedArtifact final {
        std::filesystem::path relative;
        std::shared_ptr<const std::vector<std::byte>> bytes;
        Security::VerifiedArtifactEvidence evidence;

        ScriptModuleCapturedArtifact(std::filesystem::path path, std::shared_ptr<const std::vector<std::byte>> data,
                                     Security::VerifiedArtifactEvidence proof)
            : relative(std::move(path)), bytes(std::move(data)), evidence(std::move(proof)) {}
    };

    struct ScriptModuleProgramInput final {
        ScriptModuleRuntimeKind runtime{ScriptModuleRuntimeKind::Lua54};
        std::string package, module;
        std::filesystem::path root, source;
        std::shared_ptr<const Security::NativeArtifactGate> currentTrust;
        std::vector<ScriptModuleCapturedArtifact> artifacts;
    };

    /** Bounded immutable signed capture for an explicit module operation. No adapter/VM supplies its fields. */
    class ScriptModuleProgram final {
    public:
        static Result<std::shared_ptr<const ScriptModuleProgram>> Capture(ScriptModuleProgramInput);
        [[nodiscard]] Result<void> VerifyCurrent() const;

        [[nodiscard]] const ExtensionManifest &Manifest() const noexcept {
            return manifest_;
        }

        [[nodiscard]] const ExtensionModuleManifest &Module() const noexcept {
            return *module_;
        }

        [[nodiscard]] std::string_view Source() const noexcept {
            return source_;
        }

        [[nodiscard]] const std::string &ScriptId() const noexcept {
            return scriptId_;
        }

        [[nodiscard]] ScriptModuleRuntimeKind Runtime() const noexcept {
            return input_.runtime;
        }

        [[nodiscard]] std::span<const ScriptModuleCapturedArtifact> Artifacts() const noexcept {
            return input_.artifacts;
        }

        [[nodiscard]] bool RequestsReference() const noexcept;

    private:
        ScriptModuleProgram(const ScriptModuleProgram &) = delete;
        ScriptModuleProgram &operator=(const ScriptModuleProgram &) = delete;

        explicit ScriptModuleProgram(ScriptModuleProgramInput input) : input_(std::move(input)) {}

        Result<void> DecodeCapture();
        Result<void> SelectModule();
        ScriptModuleProgramInput input_;
        ExtensionManifest manifest_;
        const ExtensionModuleManifest *module_{};
        std::string source_, scriptId_;
    };

    [[nodiscard]] Result<void> ValidateScriptModuleSelectedDeclaration(const ExtensionModuleManifest &);
    [[nodiscard]] bool SameScriptModuleEvidence(const Security::VerifiedArtifactEvidence &,
                                                const Security::VerifiedArtifactEvidence &) noexcept;
    [[nodiscard]] Result<void> ValidateScriptModuleActiveCapture(const class ModuleScriptHostAuthority &, const ScriptModuleProgram &);
}  // namespace Horo::Extensions
