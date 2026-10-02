#pragma once

/**
 * @file BootstrapInstallation.h
 * @brief First-install transaction for an authenticated immutable product version.
 */

#include "Horo/Release/UpdateActivation.h"

namespace Horo::Release {
    /** @brief Operating-system and CPU target of the running installer binary. */
    struct BootstrapBuildTarget final {
        DistributionPlatform platform;
        DistributionArchitecture architecture;
        bool operator==(const BootstrapBuildTarget &) const noexcept = default;
    };

    /**
     * @brief Identifies the running installer's compiled OS and CPU target.
     * @return Build target or failure for an unsupported target.
     */
    [[nodiscard]] Result<BootstrapBuildTarget> DetectBootstrapBuildTarget();

    /**
     * @brief Rejects a package targeting a different OS or CPU than this installer binary.
     * @param artifact Authenticated package identity.
     * @return Success only for the exact supported native target.
     */
    [[nodiscard]] Result<void> CheckBootstrapBuildTarget(const DistributionArtifactIdentity &artifact);

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
         * @note Native hosts call CheckBootstrapBuildTarget and separately probe the actual OS and hardware before installation.
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
        /**
         * @brief Idempotently removes only files owned by this authenticated package after deactivation.
         * @param request Exact package, inventory, and immutable version paths admitted by the installer.
         * @return Success after owned files are durably absent; failure keeps the uninstall journal for retry.
         * @note This must preserve projects, settings, caches, logs, credentials, and shared components.
         */
        [[nodiscard]] virtual Result<void> RemoveOwnedVersion(const BootstrapInstallationRequest &request) = 0;
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

    /**
     * @brief Reauthenticates an active installation and restores its idempotent OS registration.
     * @param request Exact installed package and immutable version evidence.
     * @param files Native durable filesystem owning the installation lock.
     * @param verifier Trusted publisher signature verifier.
     * @param host Product-exit gate, integration repair, and bounded startup probe.
     * @return Success only if the existing active version is authenticated and healthy.
     */
    [[nodiscard]] Result<void> RepairVerifiedInstallation(const BootstrapInstallationRequest &request, NativeDurableFileSystem &files,
                                                          const Security::ArtifactVerifier &verifier, IBootstrapInstallationHost &host);

    /**
     * @brief Deactivates and removes one authenticated bootstrap version without touching user data.
     * @param request Exact installed package and immutable version evidence.
     * @param files Native durable filesystem owning the installation lock.
     * @param verifier Trusted publisher signature verifier.
     * @param host Product-exit gate and idempotent integration/owned-file removal.
     * @return Success when activation, integration, and exact owned files are absent.
     * @note Interrupted removal retains a journal and can be retried with the same request.
     */
    [[nodiscard]] Result<void> UninstallVerifiedInstallation(const BootstrapInstallationRequest &request, NativeDurableFileSystem &files,
                                                             const Security::ArtifactVerifier &verifier, IBootstrapInstallationHost &host);
}  // namespace Horo::Release
