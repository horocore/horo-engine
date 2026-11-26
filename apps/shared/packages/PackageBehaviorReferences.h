#pragma once

#include "Horo/Packages/GameplayPackageModuleHost.h"
#include "Horo/Platform/ProjectReadFiles.h"

#include <optional>

namespace Horo::Assets {
    class IAssetRegistrySource;
    struct AssetRegistryCandidate;
}

namespace Horo::Packages {
    class PackageRemovalLease;
}

namespace Horo::Application::Internal {
    inline constexpr std::size_t MaximumRemovalCoverageDiagnostics = 16'384;
    /** @brief Bounded saved-source failures retained alongside confirmed reference findings. */
    class PackageSourceReviewFailures final {
    public:
        /** @brief Prevents a partial asset inventory from authorizing removal while retaining confirmed matches. */
        void MarkInventoryIncomplete();

        /** @brief Reports whether saved sources or their inventory could not be fully reviewed. */
        [[nodiscard]] bool Incomplete() const noexcept {
            return first_.has_value();
        }

        /** @brief Records a canonical project-relative source failure while keeping the first typed cause.
         * @param path Path from the admitted asset registry. @param failure Exact failure for that source.
         * @return Success to continue the scan or a resource-limit error.
         */
        [[nodiscard]] Result<void> Record(std::string_view path, Error failure);

        /** @brief Releases the first failure with the combined source and reference diagnostics.
         * @param references Confirmed references from successfully decoded sources.
         * @return Failure with combined findings, or the original references on a complete scan.
         */
        [[nodiscard]] Result<std::vector<Diagnostic>> Finish(std::vector<Diagnostic> references) &&;

    private:
        std::optional<Error> first_;
    };

    /** @brief Inert evidence from one open project session. Unobserved modules remain unknown.
     * @details Retains observed identities and dependency edges across activations of the exact artifact and role. */
    struct PackageRemovalRegistrationEvidence final {
        std::vector<Packages::GameplayPackageModuleHost::RegistrationInventory> registrations;

        /** @brief Adds one exact role observation without discarding previously observed references.
         * @param observed Owned metadata from the published activation.
         * @return Success or a typed evidence limit error. Failure preserves the prior observations.
         */
        [[nodiscard]] Result<void> Observe(Packages::GameplayPackageModuleHost::RegistrationInventory observed);
    };

    /** @brief Borrowed project inputs for the saved-reference checks within the mutation lease. */
    struct PackageRemovalSources final {
        const Assets::IAssetRegistrySource &assets;
        const Platform::IProjectReadFiles &files;
        Platform::ProjectReadContext context;
        std::span<const Packages::GameplayPackageModuleHost::RegistrationInventory> registrations;
        std::shared_ptr<const Packages::PackageRemovalLease> live; /**< Owner-issued native reservation and sealed archive evidence. */
        const Packages::StagedPackageRestoreRecord *offline{}; /**< Read-only staging evidence for an asset-only withdrawal. */
        bool requireGameplayOwner{}; /**< Executable departures must be fully owned by the gameplay lifecycle. */
        const Error *registrationFailure{}; /**< A failed capture invalidates live role evidence. */
    };

    /** @brief Reports when a locked contribution requires a live owner for removal.
     * @param package Exact departing lock entry.
     * @return A blocker for unsupported contributions or no lock-level blocker for an asset-only entry.
     * @details The caller must still verify the actual archive before an offline mutation.
     */
    [[nodiscard]] std::optional<Diagnostic> OfflineRemovalContributionBlocker(const Packages::LockedPackage &package);

    /** @brief Reports sealed archive members that cannot depart without a live lifecycle owner.
     * @param blockers Bounded diagnostics accumulated in lock and archive inventory order.
     * @param locked Exact departing package identity.
     * @param archive Cache or staging archive validated against the lock entry.
     * @return Success or a typed diagnostic limit failure.
     */
    [[nodiscard]] Result<void> AppendOfflineRemovalArchiveBlockers(std::vector<Diagnostic> &blockers,
                                                                    const Packages::LockedPackage &locked,
                                                                    const Packages::ValidatedPackageArchive &archive);

    /** @brief Requires exact observations for departing runtime roles and for retained roles that could consume a departing service. */
    [[nodiscard]] Result<void> CheckRemovalRegistrationCoverage(const PackageRemovalSources &sources,
                                                                const Packages::ValidatedPackageLockfileV1 &lock,
                                                                const Packages::PackageRemovalGraphPreview &removal);

    /** @brief Finds retained native registrations that use services or component types from exact departing artifacts.
     * @param sources Owned-registration observations bound to the current project.
     * @param lock Current validated lock used to filter stale observations.
     * @param removal Exact withdrawal effects from that lock.
     * @return Bounded dependency diagnostics or a reference-limit error.
     * @details Only supplied, observed package roles are examined. Registrations leaving together cannot block removal.
     */
    [[nodiscard]] Result<std::vector<Diagnostic>> InspectPackageNativeReferences(const PackageRemovalSources &sources,
                                                                                 const Packages::ValidatedPackageLockfileV1 &lock,
                                                                                 const Packages::PackageRemovalGraphPreview &removal);

    /** @brief Finds authored Decision nodes and saved sources requiring custom sense review.
     * @param sources Owned registration observations and root-bound saved source.
     * @param lock Exact current lock used to attribute registrations.
     * @param removal Validated withdrawal plan.
     * @param inventory Exact admitted saved inventory shared with the importer review.
     * @return Bounded diagnostics, or a typed failure if registered graphs cannot be read completely.
     */
    [[nodiscard]] Result<std::vector<Diagnostic>> InspectPackageDecisionReferences(const PackageRemovalSources &sources,
                                                                                   const Packages::ValidatedPackageLockfileV1 &lock,
                                                                                   const Packages::PackageRemovalGraphPreview &removal,
                                                                                   const Assets::AssetRegistryCandidate &inventory);

    /** @brief One artifact-bound rename claim with names interpreted by Gameplay. */
    struct PackageBehaviorMigration final {
        Packages::HoroPackageId package;
        Gameplay::BehaviorTypeId previous;
        Gameplay::BehaviorTypeId candidate;
    };

    /** @brief Reports saved Scene and Prefab attachments whose type matches a declared rename.
     * @param assets Same-project canonical asset registry source. @param files Root-bound source reader.
     * @param context Original opening, cancellation and deadline.
     * @param claims Bounded artifact-bound claims with typed behavior names. @param diagnosticLimit Output limit.
     * @return Advisory attachment locations and exact source revisions. Incomplete inventories return a typed error
     * with advisory locations found in readable sources.
     * @details The matches are possible impacts. Only the live package lifecycle can certify provider ownership.
     */
    [[nodiscard]] Result<std::vector<Diagnostic>> InspectPackageBehaviorMigrations(
        const Assets::IAssetRegistrySource &assets, const Platform::IProjectReadFiles &files, Platform::ProjectReadContext context,
        std::span<const PackageBehaviorMigration> claims, std::size_t diagnosticLimit);

    /** @brief Reads canonical saved Scene and Prefab sources for components, behaviors and potential custom sense dependencies.
     * @param sources Same-project registry, root-bound read authority, and registration evidence.
     * @param lock Current validated lock. @param removal Exact withdrawal effects from that lock.
     * @param inventory Exact admitted saved inventory shared with the importer review.
     * @return Confirmed component and behavior matches, unresolved sense findings, or a typed failure.
     * @details Matches known registrations. It does not certify registration coverage for packages without supplied evidence,
     * inspect unsaved documents or grant removal approval. Call off the UI draw lane.
     */
    [[nodiscard]] Result<std::vector<Diagnostic>> InspectPackageScenePrefabReferences(
        const PackageRemovalSources &sources,
        const Packages::ValidatedPackageLockfileV1 &lock, const Packages::PackageRemovalGraphPreview &removal,
        const Assets::AssetRegistryCandidate &inventory);
}  // namespace Horo::Application::Internal
