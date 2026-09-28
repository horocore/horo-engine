#pragma once

/**
 * @file BootstrapInstallation.h
 * @brief First-install transaction for an authenticated immutable product version.
 */

#include "Horo/Release/UpdateActivation.h"

namespace Horo::Release {
    /** @brief Exact first-install inputs; the candidate tree is private and quiescent for the call. */
    struct BootstrapInstallationRequest final {
        std::filesystem::path installationRoot;
        UpdateActivationVersion candidate;
        UpdateArchiveLimits archiveLimits;
        std::chrono::seconds healthTimeout{30};
    };

    /** @brief Host policy and operating-system integration executed under the installation lock. */
    class IBootstrapInstallationHost : public IUpdateActivationHost {
    public:
        /**
         * @brief Checks the actual OS, architecture, minimum OS, permission, disk, destination, and package conflicts.
         * @param request Immutable selected package and destination.
         * @return Success only when this host can install the exact package without replacing another product.
         */
        [[nodiscard]] virtual Result<void> Preflight(const BootstrapInstallationRequest &request) = 0;
        /**
         * @brief Idempotently registers only package-policy-approved operating-system integration.
         * @param request Authenticated product and version layout.
         * @return Success after integration is durable; failure may be followed by Unregister.
         */
        [[nodiscard]] virtual Result<void> Register(const BootstrapInstallationRequest &request) = 0;
        /**
         * @brief Idempotently removes this installation's integration after failure or interrupted first install.
         * @param request Exact product and destination whose registration may have begun.
         * @return Success only after no registration points to this candidate.
         */
        [[nodiscard]] virtual Result<void> Unregister(const BootstrapInstallationRequest &request) = 0;
    };

    /** @brief Initial activation succeeded, or an interrupted attempt was safely undone. */
    enum class BootstrapInstallationOutcome : std::uint8_t {
        Installed,
        RecoveredIncomplete
    };

    /**
     * @brief Authenticates an already-staged package and activates the first installation transactionally.
     * @param request Protected destination and verified candidate evidence.
     * @param files Native durable file service owning the exclusive installation lock.
     * @param verifier Publisher signature verifier.
     * @param host Platform preflight, integration, product-exit gate, and bounded first-launch probe.
     * @return Installed or recovered-incomplete outcome; a failed cleanup retains the journal for later recovery.
     * @note The host must keep the installation root and candidate tree private and quiescent through this call.
     */
    [[nodiscard]] Result<BootstrapInstallationOutcome> BootstrapVerifiedInstallation(const BootstrapInstallationRequest &request,
                                                                                     NativeDurableFileSystem &files,
                                                                                     const Security::ArtifactVerifier &verifier,
                                                                                     IBootstrapInstallationHost &host);
}  // namespace Horo::Release
