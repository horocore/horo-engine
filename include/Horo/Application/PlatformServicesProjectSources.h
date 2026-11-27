#pragma once

/** @file PlatformServicesProjectSources.h
 * @brief Shared bounded project-source capture and exact revision fences for every Platform Services host.
 */

#include "Horo/Application/PlatformServicesAuthoring.h"
#include "Horo/Application/PlatformServicesDefinitionCodec.h"
#include "Horo/Assets/AssetSourcePublication.h"

#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace Horo::Application {
    struct PlatformServicesProjectPolicyPublication;

    /** @brief Immutable-for-an-operation host policy inputs; source text never chooses these values. */
    struct PlatformServicesProjectValidationContext final {
        PlatformServices::PlatformServicesHostProfile profile{PlatformServices::PlatformServicesHostProfile::InteractiveDevelopment};
        std::vector<PlatformServices::PlatformProviderModuleContribution> contributions;
        std::vector<ModuleId> trustedModules;
        PlatformServices::PlatformProjectConfigurationLimits policyLimits{.maximumProjectIdBytes =
                                                                              PlatformServices::PlatformServicesMaximumProjectIdBytes,
                                                                          .maximumProviderKeyBytes = 1024,
                                                                          .maximumModuleIdBytes = 1024};
        PlatformServices::AchievementDefinitionRegistryLimits achievementLimits{.maximumDefinitions = 4096,
                                                                                .maximumLocalizationKeyBytes = 512};
        PlatformServices::PlatformDefinitionRegistryLimits definitionLimits{.maximumDefinitions = 4096,
                                                                            .maximumLocalizationKeyBytes = 512,
                                                                            .maximumPresenceDetailBytes = 4096};
    };

    /** @brief Owned coherent immutable source/model generation; creation is restricted to complete validation. */
    class PlatformServicesProjectSourceSnapshot final {
    public:
        /** @brief Returns the admitted project root. @return Borrowed canonical authorized root for this snapshot's lifetime. */
        [[nodiscard]] const std::filesystem::path &ProjectRoot() const noexcept {
            return root_;
        }

        /** @brief Returns the exact root byte witness. @return Immutable existence, byte count and SHA-256 evidence. */
        [[nodiscard]] const Assets::AssetSourceIdentity &RootIdentity() const noexcept {
            return sources_.front();
        }

        /** @brief Returns the domain-separated six-source digest. @return Immutable raw-source generation, independent of host profile. */
        [[nodiscard]] const Sha256Digest &SourceFingerprint() const noexcept {
            return fingerprint_;
        }

        /** @brief Returns the captured identity namespace. @return Owned immutable ledger reference. */
        [[nodiscard]] const PlatformServices::PlatformStableIdRegistry &StableIds() const noexcept {
            return ids_;
        }

        /** @brief Returns captured achievement semantics. @return Owned immutable complete definitions. */
        [[nodiscard]] const PlatformServices::AchievementDefinitionRegistry &Achievements() const noexcept {
            return achievements_;
        }

        /** @brief Returns captured stat semantics. @return Owned immutable complete definitions. */
        [[nodiscard]] const PlatformServices::StatDefinitionRegistry &Stats() const noexcept {
            return stats_;
        }

        /** @brief Returns captured leaderboard semantics. @return Owned immutable cross-reference-validated definitions. */
        [[nodiscard]] const PlatformServices::LeaderboardDefinitionRegistry &Leaderboards() const noexcept {
            return leaderboards_;
        }

        /** @brief Returns captured presence semantics. @return Owned immutable complete definitions. */
        [[nodiscard]] const PlatformServices::PresenceDefinitionRegistry &Presence() const noexcept {
            return presence_;
        }

        /** @brief Returns policy resolved against actual trusted inert metadata. @return Immutable host-profile-specific policy. */
        [[nodiscard]] const PlatformServices::PlatformProjectConfiguration &Configuration() const noexcept {
            return configuration_;
        }

    private:
        friend Result<PlatformServicesProjectSourceSnapshot> CapturePlatformServicesProjectSources(
            const std::filesystem::path &, const ProjectCompatibilitySnapshot &, const PlatformServicesProjectValidationContext &,
            CancellationToken);
        friend Result<void> RevalidatePlatformServicesProjectSources(const PlatformServicesProjectSourceSnapshot &, CancellationToken);
        friend Result<PlatformServicesProjectPolicyPublication> PublishPlatformServicesProjectPolicy(
            const PlatformServicesProjectSourceSnapshot &, const PlatformServices::PlatformProjectConfiguration &,
            const ExclusiveFileLock &, DurableFileSystem &, std::function<Result<void>()>, CancellationToken);
        /** @brief Owns already validated models and exact source witnesses; no source buffer or ambient authority is retained. */
        PlatformServicesProjectSourceSnapshot(std::filesystem::path root, std::array<Assets::AssetSourceIdentity, 6> sources,
                                              PlatformServices::PlatformStableIdRegistry ids,
                                              PlatformServices::AchievementDefinitionRegistry achievements,
                                              PlatformServices::StatDefinitionRegistry stats,
                                              PlatformServices::LeaderboardDefinitionRegistry leaderboards,
                                              PlatformServices::PresenceDefinitionRegistry presence,
                                              PlatformServices::PlatformProjectConfiguration configuration);
        std::filesystem::path root_;
        std::array<Assets::AssetSourceIdentity, 6> sources_;
        Sha256Digest fingerprint_;
        PlatformServices::PlatformStableIdRegistry ids_;
        PlatformServices::AchievementDefinitionRegistry achievements_;
        PlatformServices::StatDefinitionRegistry stats_;
        PlatformServices::LeaderboardDefinitionRegistry leaderboards_;
        PlatformServices::PresenceDefinitionRegistry presence_;
        PlatformServices::PlatformProjectConfiguration configuration_;
    };

    /** @brief Complete accepted source generation and tracked root-policy publication, including durability uncertainty. */
    struct PlatformServicesProjectPolicyPublication final {
        std::shared_ptr<const PlatformServicesProjectSourceSnapshot> sources;
        Assets::AssetSourcePublication publication;
    };

    /** @brief Publishes one typed portable policy edit without changing identity or service definitions.
     * @param sources Complete accepted six-source generation from this exact opening.
     * @param configuration Replacement validated against this host's actual provider/trust context, with the same project and profile.
     * @param lease Exact native asset-source publication lease for the existing `.horo/project.json`.
     * @param files Borrowed durable filesystem authority retained through return.
     * @param checkAvailable Required bounded nonreentrant host revocation check; never inferred from retained CPU models.
     * @param cancellation Cooperative operation/shutdown cancellation.
     * @return Preallocated complete source generation and durable/reconciliation receipt, or typed precommit failure.
     * @pre The caller retains the shared project mutation lease and excludes noncooperating namespace writers.
     * @details All six source identities and host availability are checked again after staging immediately before replacement.
     * Identity, salt and unrelated root values are preserved. Live operations keep their old captures; ProjectReopen policy
     * does not activate providers. Failure before replacement preserves the old root; reconciliation retains the new root.
     * Every next-generation model and result owner is prepared before the irreversible boundary.
     */
    [[nodiscard]] Result<PlatformServicesProjectPolicyPublication> PublishPlatformServicesProjectPolicy(
        const PlatformServicesProjectSourceSnapshot &sources, const PlatformServices::PlatformProjectConfiguration &configuration,
        const ExclusiveFileLock &lease, DurableFileSystem &files, std::function<Result<void>()> checkAvailable,
        CancellationToken cancellation = {});

    /**
     * @brief Captures and validates root, ledger and four service documents through the shared typed validators.
     * @param projectRoot Canonical absolute host-authorized root, admitted by base project compatibility first.
     * @param admission Current/compatible base admission with no pending marker update, metadata and exact root fingerprint from the same
     * read.
     * @param context Host-owned profile, actual inert contributions, trust set and finite limits; never discovered from ambient state.
     * @param cancellation Cooperative load-time cancellation checked throughout capture and before return.
     * @return Complete immutable generation or original typed read, shape, identity, semantic, trust or selection error.
     * @details Root is bounded to 1 MiB and each sidecar to 4 MiB. All six identities are rechecked after decoding.
     * No cache, native provider instance, source repair or runtime activation is published. Exact provider selection never falls back.
     * Revalidation is a bounded load/activation operation, not a rendering-frame operation. Cooperating source writers must use the
     * shared mutation authority; the byte fence does not make noncooperating filesystem writes transactional.
     */
    [[nodiscard]] Result<PlatformServicesProjectSourceSnapshot> CapturePlatformServicesProjectSources(
        const std::filesystem::path &projectRoot, const ProjectCompatibilitySnapshot &admission,
        const PlatformServicesProjectValidationContext &context, CancellationToken cancellation = {});

    /**
     * @brief Rechecks the six exact source identities before publication or activation.
     * @param snapshot Complete owned captured generation, including its root authority.
     * @param cancellation Cooperative load-time cancellation.
     * @return Success, original typed read/cancellation failure, or source conflict after any byte-level edit/deletion.
     * @pre The host still grants read/publication authority for snapshot.ProjectRoot(); a retained CPU snapshot is not an access grant.
     * @details Reads retain the same finite limits as capture. This does not publish state or reselect a provider.
     */
    [[nodiscard]] Result<void> RevalidatePlatformServicesProjectSources(const PlatformServicesProjectSourceSnapshot &snapshot,
                                                                        CancellationToken cancellation = {});
}  // namespace Horo::Application
