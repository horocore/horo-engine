#pragma once

/**
 * @file ZipPortableBootstrapHost.h
 * @brief Native Windows and macOS portable ZIP bootstrap policy.
 */

#include "Horo/Release/BootstrapInstallation.h"

namespace Horo::Release {
#if defined(_WIN32) || defined(__APPLE__)
    /** @brief Native OS minimum and free space retained for transaction metadata. */
    struct ZipPortableBootstrapPolicy final {
        unsigned minimumOsMajor{};
        unsigned minimumOsMinor{};
        std::uint64_t minimumFreeBytes{4096U};
    };

    /** @brief Portable ZIP host with caller-owned process coordination. */
    class ZipPortableBootstrapHost final : public IBootstrapInstallationHost {
    public:
        /**
         * @brief Binds durable files, trusted verification, and product process coordination.
         * @param files Durable filesystem used by the installation transaction.
         * @param verifier Trusted publisher signature verifier.
         * @param processes Product stop and bounded launch-health implementation.
         * @param policy Minimum native OS and free-space policy.
         */
        ZipPortableBootstrapHost(NativeDurableFileSystem &files, const Security::ArtifactVerifier &verifier,
                                 IUpdateActivationHost &processes, ZipPortableBootstrapPolicy policy) noexcept;

        /** @copydoc IUpdateActivationHost::EnsureProductsStopped */
        [[nodiscard]] Result<void> EnsureProductsStopped(const std::filesystem::path &installationRoot) override;
        /** @copydoc IUpdateActivationHost::ProbeStartupHealth */
        [[nodiscard]] Result<void> ProbeStartupHealth(const std::filesystem::path &versionRoot, std::chrono::seconds timeout) override;

        /** @copydoc IBootstrapInstallationHost::Preflight */
        [[nodiscard]] Result<void> Preflight(const BootstrapInstallationRequest &request) override;
        /** @copydoc IBootstrapInstallationHost::Register */
        [[nodiscard]] Result<void> Register(const BootstrapInstallationRequest &request) override;
        /** @copydoc IBootstrapInstallationHost::Unregister */
        [[nodiscard]] Result<void> Unregister(const BootstrapInstallationRequest &request) override;
        /** @copydoc IBootstrapInstallationHost::RemoveOwnedVersion */
        [[nodiscard]] Result<void> RemoveOwnedVersion(const BootstrapInstallationRequest &request) override;

    private:
        NativeDurableFileSystem &files_;
        const Security::ArtifactVerifier &verifier_;
        IUpdateActivationHost &processes_;
        ZipPortableBootstrapPolicy policy_;
    };
#endif
}  // namespace Horo::Release
