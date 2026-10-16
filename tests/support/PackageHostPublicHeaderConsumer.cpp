#include "Horo/Packages/GameplayPackageModuleHost.h"
#include "Horo/Packages/NativePackageModuleHost.h"
#include "Horo/Packages/PackageModuleHost.h"
#include "Horo/Packages/ScriptPackageModuleHost.h"

#include <type_traits>

static_assert(!std::is_default_constructible_v<Horo::Packages::VerifiedPackageModule>);
static_assert(!std::is_copy_constructible_v<Horo::Packages::PackageModuleRetirementQueue>);

namespace Horo::Packages::Tests {
    /** @brief Compiles actual application composition using only the owning target's staged public surface. */
    Result<std::unique_ptr<PackageLifecycleService>> ComposeHybridPackageHost(PackageInstallService &install,
                                                                              const IPackageExtensionTrust &trust,
                                                                              PackageLifecycleConfiguration configuration,
                                                                              std::vector<PackageScriptRegistryRealm> realms,
                                                                              std::vector<GameplayPackageModuleBinding> gameplayBindings) {
        auto native = std::make_shared<NativePackageModuleHost>(configuration);
        auto script = std::make_shared<ScriptPackageModuleHost>(std::move(realms), native);
        auto gameplay = std::make_shared<GameplayPackageModuleHost>(GameplayPackageHostConfiguration{configuration.temporaryRoot,
                                                                                                     configuration.artifactGate},
                                                                    std::move(gameplayBindings));
        configuration.moduleHosts = {std::move(native), std::move(script), std::move(gameplay)};
        return PackageLifecycleService::Create(install, trust, std::move(configuration));
    }
}  // namespace Horo::Packages::Tests
