#include "ModuleScriptHostBridgeInternal.h"

#include <algorithm>
#include <tuple>

namespace Horo::Extensions {
    namespace {
        bool Abi(const auto &a, const auto &b) {
            if (a.has_value() != b.has_value())
                return false;
            return !a || std::tie(a->major, a->minimumMinor) == std::tie(b->major, b->minimumMinor);
        }

        bool Sources(const ExtensionModuleManifest &a, const ExtensionModuleManifest &b) {
            if (a.entry != b.entry || a.entries.size() != b.entries.size())
                return false;
            return std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(), [](const auto &x, const auto &y) {
                return std::tie(x.platform, x.architecture, x.buildProfile, x.entry) ==
                       std::tie(y.platform, y.architecture, y.buildProfile, y.entry);
            });
        }

        bool Exports(const ExtensionModuleManifest &a, const ExtensionModuleManifest &b) {
            if (a.exports.size() != b.exports.size())
                return false;
            return std::equal(a.exports.begin(), a.exports.end(), b.exports.begin(), [](const auto &x, const auto &y) {
                return std::tie(x.id, x.contract, x.version) == std::tie(y.id, y.contract, y.version);
            });
        }

        bool Imports(const ExtensionModuleManifest &a, const ExtensionModuleManifest &b) {
            if (a.imports.size() != b.imports.size())
                return false;
            return std::equal(a.imports.begin(), a.imports.end(), b.imports.begin(), [](const auto &x, const auto &y) {
                return std::tie(x.id, x.service, x.contract, x.minimumVersion, x.required) ==
                       std::tie(y.id, y.service, y.contract, y.minimumVersion, y.required);
            });
        }

        bool Same(const ExtensionModuleManifest &a, const ExtensionModuleManifest &b) {
            const bool identity = std::tie(a.id, a.version, a.kind, a.roles, a.dependencies, a.requiredCapabilities) ==
                                  std::tie(b.id, b.version, b.kind, b.roles, b.dependencies, b.requiredCapabilities);
            return identity && Abi(a.abi, b.abi) && Sources(a, b) && Exports(a, b) && Imports(a, b);
        }

        Result<std::shared_ptr<NativeServiceModule>> Active(const NativeExtensionServiceState &host, std::string_view package,
                                                            const ExtensionModuleManifest &declaration) {
            using Output = Result<std::shared_ptr<NativeServiceModule>>;
            for (const auto &weak : host.modules) {
                auto module = weak.lock();
                if (!module)
                    continue;
                if (module->extension != package || module->declaration.id != declaration.id)
                    continue;
                if (!module->committed || module->revoked || !Same(module->declaration, declaration))
                    break;
                return Output::Success(std::move(module));
            }
            return Output::Failure(MakeError(ExtensionErrors::ScriptInvocationUnavailable,
                                             "The exact signed native module declaration is not currently active."));
        }

        Result<void> Evidence(const NativeServiceModule &module, std::span<const ModuleScriptArtifactEvidence> artifacts) {
            const auto code = module.moduleCode.lock();
            if (!code || !code->loaded || !code->artifactEvidence)
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::ScriptInvocationProviderRevoked, "Actual native load evidence is absent or retired."));
            const auto found =
                std::ranges::find(artifacts, std::filesystem::path{module.declaration.entry}, &ModuleScriptArtifactEvidence::relative);
            if (found == artifacts.end() || !(code->artifactEvidence->ArtifactDigest() == found->evidence.ArtifactDigest() &&
                                              code->artifactEvidence->PublisherId() == found->evidence.PublisherId() &&
                                              code->artifactEvidence->KeyId() == found->evidence.KeyId()))
                return Result<void>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable,
                                                       "Captured native bytes/publisher/key differ from the actual activated library."));
            return Result<void>::Success();
        }

        bool PresentationOnly(const ExtensionModuleManifest &module) {
            return module.roles.size() == 1 && module.roles.front() == ExtensionModuleRole::EditorPresentation;
        }
    }  // namespace

    Result<void> ModuleScriptHostAuthority::ValidateActiveArtifacts(const ExtensionManifest &manifest,
                                                                    std::span<const ModuleScriptArtifactEvidence> artifacts) const {
        auto lane = RequireOpenOwner();
        if (lane.HasError())
            return lane;
        for (const auto &declaration : manifest.modules) {
            if (PresentationOnly(declaration))
                continue;
            auto active = Active(*impl_->state, manifest.id, declaration);
            if (active.HasError())
                return Result<void>::Failure(active.ErrorValue());
            auto evidence = Evidence(*active.Value(), artifacts);
            if (evidence.HasError())
                return evidence;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Extensions
