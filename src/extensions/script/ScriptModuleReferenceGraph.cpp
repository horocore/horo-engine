#include "Horo/Extensions/ExtensionServiceAbi.h"
#include "ScriptModuleReferenceInstallationInternal.h"

#include <algorithm>

namespace Horo::Extensions {
    namespace {
        constexpr ExtensionHostPlatform Platform() noexcept {
#if defined(_WIN32)
            return ExtensionHostPlatform::Windows;
#elif defined(__APPLE__)
            return ExtensionHostPlatform::MacOS;
#else
            return ExtensionHostPlatform::Linux;
#endif
        }

        constexpr ExtensionHostArchitecture Architecture() noexcept {
#if defined(__aarch64__) || defined(_M_ARM64)
            return ExtensionHostArchitecture::Arm64;
#else
            return ExtensionHostArchitecture::X86_64;
#endif
        }

        constexpr ExtensionBuildProfile BuildProfile() noexcept {
#if defined(NDEBUG)
            return ExtensionBuildProfile::Release;
#else
            return ExtensionBuildProfile::Debug;
#endif
        }
    }  // namespace

    Result<ExtensionModulePlan> ScriptModuleReferenceGraph(ScriptCapabilityProfile profile) {
        ExtensionManifest declaration;
        declaration.id = "horo.engine.script-reference";
        declaration.modules = {{.id = "horo.script.reference.provider",
                                .version = "1.0.0",
                                .kind = "builtin",
                                .roles = {ExtensionModuleRole::BackendCapability},
                                .exports = {{.id = "horo.script.reference", .contract = "horo.script.reference.v1", .version = "1.0.0"}}},
                               {.id = "horo.script.reference.consumer",
                                .version = "1.0.0",
                                .kind = "builtin",
                                .roles = {ExtensionModuleRole::BackendCapability},
                                .imports = {{.id = "reference",
                                             .service = "horo.script.reference",
                                             .contract = "horo.script.reference.v1",
                                             .minimumVersion = "1.0.0",
                                             .required = true}}}};
        const ExtensionHostEnvironment environment{.profile = profile == ScriptCapabilityProfile::Gameplay
                                                                  ? ExtensionHostProfile::Interactive
                                                                  : ExtensionHostProfile::Headless,
                                                   .platform = Platform(),
                                                   .architecture = Architecture(),
                                                   .buildProfile = BuildProfile(),
                                                   .engineVersion = "0.1.0",
                                                   .abiMajor = HORO_EXTENSION_ABI_VERSION,
                                                   .abiMinor = HORO_EXTENSION_ABI_MINOR_VERSION};
        return ResolveExtensionModules(declaration, environment);
    }
}  // namespace Horo::Extensions
