#pragma once

/**
 * @file UpdateRetention.h
 * @brief Deterministic disk-budget plan that protects active and last-known-good versions.
 */

#include "Horo/Release/UpdateActivation.h"

#include <cstdint>
#include <span>
#include <vector>

namespace Horo::Release {
    /** @brief Installed-version role established from trusted active and rollback records. */
    enum class UpdateRetentionRole : std::uint8_t {
        Active,
        LastKnownGood,
        Obsolete
    };

    /** @brief Exact measured storage for one host-owned immutable installed version. */
    struct UpdateRetentionVersion final {
        DistributionPackageId package;
        std::uint64_t occupiedBytes{};
        std::uint64_t lastUsedGeneration{};
        UpdateRetentionRole role{UpdateRetentionRole::Obsolete};
    };

    /** @brief Obsolete versions to remove in order, and occupied bytes after their removal. */
    struct UpdateRetentionPlan final {
        std::vector<DistributionPackageId> remove;
        std::uint64_t remainingBytes{};
    };

    /** @brief Trusted snapshot row and authenticated ownership evidence for one installed version. */
    struct UpdateRetentionCandidate final {
        UpdateRetentionVersion version;
        UpdateActivationVersion evidence;
    };

    /** @brief Complete locked cleanup request for one versioned installation. */
    struct UpdateRetentionCleanupRequest final {
        std::filesystem::path installationRoot;
        std::span<const UpdateRetentionCandidate> candidates;
        UpdateArchiveLimits archiveLimits;
        std::uint64_t maximumBytes{};
    };

    /**
     * @brief Selects oldest obsolete versions until the measured installation fits its disk budget.
     * @param versions Complete host-owned installed-version snapshot with one active and one distinct last-known-good version.
     * @param maximumBytes Maximum measured bytes retained across these immutable versions.
     * @return Deterministic deletion plan; failure if protected versions exceed budget or the snapshot is malformed.
     * @note The caller must recheck active/rollback records under the installation lock before applying this plan.
     */
    [[nodiscard]] Result<UpdateRetentionPlan> PlanUpdateRetention(std::span<const UpdateRetentionVersion> versions,
                                                                  std::uint64_t maximumBytes);

    /**
     * @brief Rechecks active and rollback pins under the installation lock and removes only authenticated obsolete files.
     * @param request Complete trusted version snapshot and signed file evidence, held quiescent by the host.
     * @param files Native durable filesystem that owns the shared installation lock.
     * @param verifier Trusted publisher signature verifier.
     * @param host Host gate that prevents concurrent product use of this installation.
     * @return Cleanup plan applied, or failure retaining a durable marker for an interrupted deletion.
     * @note Repeating an interrupted cleanup with the same authenticated evidence resumes it safely.
     */
    [[nodiscard]] Result<UpdateRetentionPlan> ApplyUpdateRetention(const UpdateRetentionCleanupRequest &request,
                                                                   NativeDurableFileSystem &files,
                                                                   const Security::ArtifactVerifier &verifier, IUpdateActivationHost &host);
}  // namespace Horo::Release
