#pragma once

#include "Horo/Packages/GameplayPackageModuleHost.h"
#include "Horo/Platform/ProjectReadFiles.h"

namespace Horo::Assets {
    class IAssetRegistrySource;
}

namespace Horo::Application::Internal {
    /** @brief Inert evidence from one open project session. Unobserved modules remain unknown.
     * @details Retains previously observed behavior IDs and the latest observed service descriptors per exact package artifact and role. */
    struct PackageRemovalRegistrationEvidence final {
        std::vector<Packages::GameplayPackageModuleHost::RegistrationInventory> registrations;
    };

    /** @brief Borrowed project inputs for the saved-reference checks within the mutation lease. */
    struct PackageRemovalSources final {
        const Assets::IAssetRegistrySource &assets;
        const Platform::IProjectReadFiles &files;
        Platform::ProjectReadContext context;
        std::span<const Packages::GameplayPackageModuleHost::RegistrationInventory> registrations;
        std::shared_ptr<const Packages::VerifiedPackageInstallRecord> install; /**< Sealed archive evidence for the current lock. */
    };

    /** @brief Requires observed registrations for every departing declared runtime role in the exact sealed archives. */
    [[nodiscard]] Result<void> CheckRemovalRegistrationCoverage(const PackageRemovalSources &sources,
                                                                const Packages::ValidatedPackageLockfileV1 &lock,
                                                                const Packages::PackageRemovalGraphPreview &removal);

    /** @brief A saved Scene or Prefab attachment using an exactly attributed package behavior. */
    struct PackageBehaviorReference final {
        Packages::HoroPackageId package;
        Gameplay::BehaviorTypeId type;
        std::string assetPath;
        std::uint64_t object;
        Gameplay::BehaviorInstanceId instance;
    };

    /** @brief Reads the canonical saved Scene and Prefab sources for the supplied package behavior identities.
     * @param assets Canonical registry source bound to the same project as files and context.
     * @param files Root-bound source read authority. @param context Original opening, cancellation and deadline.
     * @param registrations Owned registration evidence from the package gameplay owner.
     * @param lock Current validated lock. @param removal Exact withdrawal effects from that lock.
     * @return Owned reference list or a diagnostic for stale ownership, unreadable or unsupported source.
     * @details Matches known registrations. It does not certify registration coverage for packages without supplied evidence,
     * inspect unsaved documents or grant removal approval. Call off the UI draw lane.
     */
    [[nodiscard]] Result<std::vector<PackageBehaviorReference>> InspectPackageBehaviorReferences(
        const Assets::IAssetRegistrySource &assets, const Platform::IProjectReadFiles &files, Platform::ProjectReadContext context,
        std::span<const Packages::GameplayPackageModuleHost::RegistrationInventory> registrations,
        const Packages::ValidatedPackageLockfileV1 &lock, const Packages::PackageRemovalGraphPreview &removal);
}  // namespace Horo::Application::Internal
