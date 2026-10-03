#pragma once

/**
 * @file LinuxPortableBootstrapHost.h
 * @brief Linux tar.gz bootstrap policy and exact owned-file removal adapter.
 */

#include "Horo/Release/BootstrapInstallation.h"

namespace Horo::Release {
#if defined(__linux__)
    /** @brief Minimum supported Linux kernel and free space retained for transaction metadata. */
    struct LinuxPortableBootstrapPolicy final {
        unsigned minimumKernelMajor{};
        unsigned minimumKernelMinor{};
        std::uint64_t minimumFreeBytes{4096U};
    };

    /** @brief Linux portable archive host with caller-owned process coordination.
     * @note The bootstrap transaction holds the installation lock and removal journal; the caller keeps the private tree quiescent.
     */
    class LinuxPortableBootstrapHost final : public IBootstrapInstallationHost {
    public:
        /**
         * @brief Binds the native file service and a process coordinator retained by the composition root.
         * @param files Durable filesystem used by the installation transaction.
         * @param verifier Trusted signature verifier used again before owned-file removal.
         * @param processes Process stop and bounded launch-health implementation.
         * @param policy Minimum Linux kernel and free-space policy.
         */
        LinuxPortableBootstrapHost(NativeDurableFileSystem &files, const Security::ArtifactVerifier &verifier,
                                   IUpdateActivationHost &processes, LinuxPortableBootstrapPolicy policy) noexcept;

        /** @copydoc IBootstrapInstallationHost::Preflight */
        [[nodiscard]] Result<void> Preflight(const BootstrapInstallationRequest &request) override;
        /** @copydoc IBootstrapInstallationHost::Register */
        [[nodiscard]] Result<void> Register(const BootstrapInstallationRequest &request) override;
        /** @copydoc IBootstrapInstallationHost::Unregister */
        [[nodiscard]] Result<void> Unregister(const BootstrapInstallationRequest &request) override;
        /** @copydoc IBootstrapInstallationHost::RemoveOwnedVersion */
        [[nodiscard]] Result<void> RemoveOwnedVersion(const BootstrapInstallationRequest &request) override;
        /** @copydoc IUpdateActivationHost::EnsureProductsStopped */
        [[nodiscard]] Result<void> EnsureProductsStopped(const std::filesystem::path &installationRoot) override;
        /** @copydoc IUpdateActivationHost::ProbeStartupHealth */
        [[nodiscard]] Result<void> ProbeStartupHealth(const std::filesystem::path &versionRoot, std::chrono::seconds timeout) override;

    private:
        NativeDurableFileSystem &files_;
        const Security::ArtifactVerifier &verifier_;
        IUpdateActivationHost &processes_;
        LinuxPortableBootstrapPolicy policy_;
    };
#endif
}  // namespace Horo::Release
