#include "Horo/Release/LinuxPortableBootstrapHost.h"

#include "Horo/Release/BootstrapInstallationErrors.h"
#include "PortableBootstrapOwnedFiles.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <sys/utsname.h>
#include <system_error>
#include <unistd.h>

namespace Horo::Release {
    namespace {
        /** @brief Rejects foreign paths before platform methods can touch the installation tree. */
        [[nodiscard]] bool Admitted(const BootstrapInstallationRequest &request) {
            const auto &selection = request.candidate.package.selection;
            const auto &id = selection.artifact.package.value;
            const auto versions = request.installationRoot / "versions";
            if (std::ranges::any_of(request.installationRoot, [](const auto &part) {
                return part == "." || part == "..";
            }))
                return false;
            if (std::error_code error;
                !std::filesystem::is_directory(std::filesystem::symlink_status(request.installationRoot, error)) || error)
                return false;
            if (std::error_code error; !std::filesystem::is_directory(std::filesystem::symlink_status(versions, error)) || error)
                return false;
            auto validated = ValidateDistributionPackageSelection(selection.artifact, selection.format);
            return validated.HasValue() && validated.Value() == selection && selection.format == DistributionPackageFormat::TarGzip &&
                   selection.artifact.platform == DistributionPlatform::Linux &&
                   selection.capabilities.installLayout == DistributionInstallLayout::Portable &&
                   selection.capabilities.desktopIntegration == DistributionCapability::Unsupported &&
                   selection.capabilities.fileAssociations == DistributionCapability::Unsupported && IsValidDistributionIdentity(id) &&
                   request.installationRoot.is_absolute() && request.candidate.stageRoot == versions / id &&
                   request.candidate.packageFile == versions / (id + ".tar.gz");
        }

        /** @brief Parses only the two leading numeric Linux kernel components. */
        [[nodiscard]] bool KernelAtLeast(const std::string_view release, const LinuxPortableBootstrapPolicy policy) {
            const auto dot = release.find('.');
            if (dot == std::string_view::npos)
                return false;
            const auto minorEnd = release.find('.', dot + 1U);
            const auto majorText = release.substr(0U, dot);
            const auto minorText = release.substr(dot + 1U, minorEnd == std::string_view::npos ? minorEnd : minorEnd - dot - 1U);
            unsigned major{};
            unsigned minor{};
            const auto [majorEnd, majorError] = std::from_chars(majorText.data(), majorText.data() + majorText.size(), major);
            const auto [minorParsed, minorError] = std::from_chars(minorText.data(), minorText.data() + minorText.size(), minor);
            if (majorError != std::errc{} || minorError != std::errc{} || majorEnd != majorText.data() + majorText.size() ||
                minorParsed != minorText.data() + minorText.size())
                return false;
            return major > policy.minimumKernelMajor || (major == policy.minimumKernelMajor && minor >= policy.minimumKernelMinor);
        }

    }  // namespace

    /** @copydoc LinuxPortableBootstrapHost::LinuxPortableBootstrapHost */
    LinuxPortableBootstrapHost::LinuxPortableBootstrapHost(NativeDurableFileSystem &files, const Security::ArtifactVerifier &verifier,
                                                           IUpdateActivationHost &processes,
                                                           const LinuxPortableBootstrapPolicy policy) noexcept
        : files_(files), verifier_(verifier), processes_(processes), policy_(policy) {}

    /** @copydoc LinuxPortableBootstrapHost::Preflight */
    Result<void> LinuxPortableBootstrapHost::Preflight(const BootstrapInstallationRequest &request) {
        if (!Admitted(request) || CheckBootstrapBuildTarget(request.candidate.package.selection.artifact).HasError())
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PreflightFailed));
        utsname system{};
        if (uname(&system) != 0 || std::string_view{system.sysname} != "Linux" || !KernelAtLeast(system.release, policy_))
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PreflightFailed));
        if (const auto expectedMachine =
                request.candidate.package.selection.artifact.architecture == DistributionArchitecture::X64 ? "x86_64" : "aarch64";
            std::string_view{system.machine} != expectedMachine || access(request.installationRoot.c_str(), W_OK | X_OK) != 0 ||
            access(request.candidate.stageRoot.parent_path().c_str(), W_OK | X_OK) != 0)
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PreflightFailed));
        if (auto available = files_.AvailableBytes(request.installationRoot);
            available.HasError() || available.Value() < policy_.minimumFreeBytes)
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PreflightFailed));
        return Result<void>::Success();
    }

    /** @copydoc LinuxPortableBootstrapHost::Register */
    Result<void> LinuxPortableBootstrapHost::Register(const BootstrapInstallationRequest &request) {
        return Admitted(request) ? Result<void>::Success()
                                 : Result<void>::Failure(MakeError(BootstrapInstallationErrors::IntegrationFailed));
    }

    /** @copydoc LinuxPortableBootstrapHost::Unregister */
    Result<void> LinuxPortableBootstrapHost::Unregister(const BootstrapInstallationRequest &request) {
        return Admitted(request) ? Result<void>::Success() : Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
    }

    /** @copydoc LinuxPortableBootstrapHost::RemoveOwnedVersion */
    Result<void> LinuxPortableBootstrapHost::RemoveOwnedVersion(const BootstrapInstallationRequest &request) {
        if (!Admitted(request))
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
        return RemoveVerifiedPortableVersion(request, files_, verifier_);
    }

    /** @copydoc LinuxPortableBootstrapHost::EnsureProductsStopped */
    Result<void> LinuxPortableBootstrapHost::EnsureProductsStopped(const std::filesystem::path &installationRoot) {
        return processes_.EnsureProductsStopped(installationRoot);
    }

    /** @copydoc LinuxPortableBootstrapHost::ProbeStartupHealth */
    Result<void> LinuxPortableBootstrapHost::ProbeStartupHealth(const std::filesystem::path &versionRoot,
                                                                const std::chrono::seconds timeout) {
        return processes_.ProbeStartupHealth(versionRoot, timeout);
    }
}  // namespace Horo::Release
