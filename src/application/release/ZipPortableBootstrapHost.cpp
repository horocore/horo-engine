#include "Horo/Release/ZipPortableBootstrapHost.h"

#include "Horo/Release/BootstrapInstallationErrors.h"
#include "PortableBootstrapOwnedFiles.h"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace Horo::Release {
#if defined(_WIN32) || defined(__APPLE__)
    namespace {
        /** @brief Requires a native portable ZIP and exact immutable version paths. */
        [[nodiscard]] bool Admitted(const BootstrapInstallationRequest &request) {
            const auto &selection = request.candidate.package.selection;
            const auto &id = selection.artifact.package.value;
            const auto versions = request.installationRoot / "versions";
            if (!request.installationRoot.is_absolute() || std::ranges::any_of(request.installationRoot, [](const auto &part) {
                return part == "." || part == "..";
            }))
                return false;
            if (std::error_code error;
                !std::filesystem::is_directory(std::filesystem::symlink_status(request.installationRoot, error)) || error)
                return false;
            if (std::error_code error; !std::filesystem::is_directory(std::filesystem::symlink_status(versions, error)) || error)
                return false;
            auto validated = ValidateDistributionPackageSelection(selection.artifact, selection.format);
            return validated.HasValue() && validated.Value() == selection && selection.format == DistributionPackageFormat::ZipArchive &&
                   selection.capabilities.installLayout == DistributionInstallLayout::Portable &&
                   selection.capabilities.desktopIntegration == DistributionCapability::Unsupported &&
                   selection.capabilities.fileAssociations == DistributionCapability::Unsupported && IsValidDistributionIdentity(id) &&
                   request.candidate.stageRoot == versions / id && request.candidate.packageFile == versions / (id + ".zip");
        }

#if defined(_WIN32)
        /** @brief Queries the real OS version, independent of executable compatibility manifests. */
        [[nodiscard]] bool NativeOsAtLeast(const ZipPortableBootstrapPolicy policy) {
            const auto module = GetModuleHandleW(L"ntdll.dll");
            if (!module)
                return false;
            using VersionFunction = LONG(WINAPI *)(OSVERSIONINFOW *);
            const auto versionFunction = reinterpret_cast<VersionFunction>(GetProcAddress(module, "RtlGetVersion"));
            if (!versionFunction)
                return false;
            OSVERSIONINFOW version{};
            version.dwOSVersionInfoSize = sizeof(version);
            if (versionFunction(&version) != 0)
                return false;
            return version.dwMajorVersion > policy.minimumOsMajor ||
                   (version.dwMajorVersion == policy.minimumOsMajor && version.dwMinorVersion >= policy.minimumOsMinor);
        }

        /** @brief Checks native hardware and write access to both existing installation directories. */
        [[nodiscard]] bool NativeTargetAndWritable(const BootstrapInstallationRequest &request) {
            SYSTEM_INFO system{};
            GetNativeSystemInfo(&system);
            const auto expected = request.candidate.package.selection.artifact.architecture == DistributionArchitecture::X64
                                      ? PROCESSOR_ARCHITECTURE_AMD64
                                      : PROCESSOR_ARCHITECTURE_ARM64;
            if (system.wProcessorArchitecture != expected)
                return false;
            for (const auto &directory : {request.installationRoot, request.installationRoot / "versions"}) {
                const auto handle = CreateFileW(directory.c_str(), FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                                FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                if (handle == INVALID_HANDLE_VALUE)
                    return false;
                CloseHandle(handle);
            }
            return true;
        }
#else
        /** @brief Parses a leading decimal OS version component. */
        [[nodiscard]] bool ParseVersionComponent(const std::string_view text, unsigned &value) {
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
            return error == std::errc{} && end == text.data() + text.size();
        }

        /** @brief Checks macOS version from the kernel and the running machine architecture. */
        [[nodiscard]] bool NativeOsAtLeast(const ZipPortableBootstrapPolicy policy) {
            char version[64]{};
            std::size_t size = sizeof(version);
            if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0 || size == 0U || size > sizeof(version) ||
                version[size - 1U] != '\0')
                return false;
            const std::string_view text{version, size - 1U};
            const auto majorEnd = text.find('.');
            if (majorEnd == std::string_view::npos)
                return false;
            const auto minorEnd = text.find('.', majorEnd + 1U);
            unsigned major{};
            unsigned minor{};
            const auto minorText = text.substr(majorEnd + 1U, minorEnd == std::string_view::npos ? minorEnd : minorEnd - majorEnd - 1U);
            if (!ParseVersionComponent(text.substr(0U, majorEnd), major) || !ParseVersionComponent(minorText, minor))
                return false;
            return major > policy.minimumOsMajor || (major == policy.minimumOsMajor && minor >= policy.minimumOsMinor);
        }

        /** @brief Checks the running macOS kernel, CPU target, and actual directory access. */
        [[nodiscard]] bool NativeTargetAndWritable(const BootstrapInstallationRequest &request) {
            utsname system{};
            if (uname(&system) != 0 || std::string_view{system.sysname} != "Darwin")
                return false;
            const auto expected =
                request.candidate.package.selection.artifact.architecture == DistributionArchitecture::X64 ? "x86_64" : "arm64";
            return std::string_view{system.machine} == expected && access(request.installationRoot.c_str(), W_OK | X_OK) == 0 &&
                   access((request.installationRoot / "versions").c_str(), W_OK | X_OK) == 0;
        }
#endif
    }  // namespace

    /** @copydoc ZipPortableBootstrapHost::ZipPortableBootstrapHost */
    ZipPortableBootstrapHost::ZipPortableBootstrapHost(NativeDurableFileSystem &files, const Security::ArtifactVerifier &verifier,
                                                       IUpdateActivationHost &processes, const ZipPortableBootstrapPolicy policy) noexcept
        : files_(files), verifier_(verifier), processes_(processes), policy_(policy) {}

    /** @copydoc ZipPortableBootstrapHost::Preflight */
    Result<void> ZipPortableBootstrapHost::Preflight(const BootstrapInstallationRequest &request) {
        if (!Admitted(request) || CheckBootstrapBuildTarget(request.candidate.package.selection.artifact).HasError() ||
            !NativeOsAtLeast(policy_) || !NativeTargetAndWritable(request))
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PreflightFailed));
        if (auto available = files_.AvailableBytes(request.installationRoot);
            available.HasError() || available.Value() < policy_.minimumFreeBytes)
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PreflightFailed));
        return Result<void>::Success();
    }

    /** @copydoc ZipPortableBootstrapHost::Register */
    Result<void> ZipPortableBootstrapHost::Register(const BootstrapInstallationRequest &request) {
        return Admitted(request) ? Result<void>::Success()
                                 : Result<void>::Failure(MakeError(BootstrapInstallationErrors::IntegrationFailed));
    }

    /** @copydoc ZipPortableBootstrapHost::Unregister */
    Result<void> ZipPortableBootstrapHost::Unregister(const BootstrapInstallationRequest &request) {
        return Admitted(request) ? Result<void>::Success() : Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
    }

    /** @copydoc ZipPortableBootstrapHost::RemoveOwnedVersion */
    Result<void> ZipPortableBootstrapHost::RemoveOwnedVersion(const BootstrapInstallationRequest &request) {
        if (!Admitted(request))
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
        return RemoveVerifiedPortableVersion(request, files_, verifier_);
    }

    /** @copydoc ZipPortableBootstrapHost::EnsureProductsStopped */
    Result<void> ZipPortableBootstrapHost::EnsureProductsStopped(const std::filesystem::path &installationRoot) {
        return processes_.EnsureProductsStopped(installationRoot);
    }

    /** @copydoc ZipPortableBootstrapHost::ProbeStartupHealth */
    Result<void> ZipPortableBootstrapHost::ProbeStartupHealth(const std::filesystem::path &versionRoot,
                                                              const std::chrono::seconds timeout) {
        return processes_.ProbeStartupHealth(versionRoot, timeout);
    }
#endif
}  // namespace Horo::Release
