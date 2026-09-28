#include "Horo/Release/LinuxPortableBootstrapHost.h"

#include "Horo/Release/BootstrapInstallationErrors.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <sys/utsname.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

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

        /** @brief Looks at a path without following a symlink or mistaking an error for absence. */
        [[nodiscard]] Result<bool> Present(const std::filesystem::path &path) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error);
                status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<bool>::Success(false);
            return error ? Result<bool>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed)) : Result<bool>::Success(true);
        }

        /** @brief Validates the exact regular single-link bytes before unlinking a declared file. */
        [[nodiscard]] bool MatchesFile(const std::filesystem::path &path, const UpdateStagedFile &file) {
            if (std::error_code error; !std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
                return false;
            if (std::error_code error; std::filesystem::hard_link_count(path, error) != 1U || error)
                return false;
            if (std::error_code error; std::filesystem::file_size(path, error) != file.size || error)
                return false;
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            Sha256Builder digest;
            std::array<char, 64U * 1024U> buffer{};
            std::uint64_t total = 0U;
            while (total < file.size) {
                const auto count = static_cast<std::streamsize>(std::min<std::uint64_t>(buffer.size(), file.size - total));
                input.read(buffer.data(), count);
                const auto read = input.gcount();
                if (read <= 0 || !digest.Update(std::as_bytes(std::span{buffer}.first(static_cast<std::size_t>(read)))))
                    return false;
                total += static_cast<std::uint64_t>(read);
            }
            return input.peek() == std::char_traits<char>::eof() && digest.Finalize() == file.digest;
        }

        /** @brief Proves every remaining stage entry is a declared file or its parent directory. */
        [[nodiscard]] Result<std::vector<std::filesystem::path>> CheckRemaining(const BootstrapInstallationRequest &request) {
            const auto invalid = [] {
                return Result<std::vector<std::filesystem::path>>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
            };
            if (std::error_code error;
                !std::filesystem::is_directory(std::filesystem::symlink_status(request.candidate.stageRoot, error)) || error)
                return invalid();
            std::error_code error;
            std::map<std::string, const UpdateStagedFile *, std::less<>> files;
            std::set<std::string, std::less<>> parents;
            for (const auto &file : request.candidate.inventory) {
                if (!files.try_emplace(file.path, &file).second)
                    return invalid();
                std::size_t separator = file.path.find('/');
                while (separator != std::string::npos) {
                    parents.emplace(file.path.substr(0U, separator));
                    separator = file.path.find('/', separator + 1U);
                }
            }
            std::vector<std::filesystem::path> directories;
            for (std::filesystem::recursive_directory_iterator
                     entry(request.candidate.stageRoot, std::filesystem::directory_options::none, error),
                 end;
                 entry != end && !error; entry.increment(error)) {
                const auto relative = entry->path().lexically_relative(request.candidate.stageRoot).generic_string();
                const auto status = entry->symlink_status(error);
                if (error)
                    return invalid();
                if (std::filesystem::is_directory(status)) {
                    if (!parents.contains(relative))
                        return invalid();
                    directories.push_back(entry->path());
                } else if (const auto found = files.find(relative); !std::filesystem::is_regular_file(status) || found == files.end() ||
                                                                    !MatchesFile(entry->path(), *found->second)) {
                    return invalid();
                }
            }
            if (error)
                return invalid();
            std::ranges::sort(directories, [](const auto &left, const auto &right) {
                return left.native().size() > right.native().size();
            });
            return Result<std::vector<std::filesystem::path>>::Success(std::move(directories));
        }

        /** @brief Removes a now-empty directory and syncs its parent without touching adjacent data. */
        [[nodiscard]] Result<void> RemoveDirectory(const std::filesystem::path &path, NativeDurableFileSystem &files) {
            if (std::error_code error; !std::filesystem::remove(path, error) || error)
                return Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
            return files.SyncDirectory(path.parent_path());
        }

        /** @brief Rechecks authenticated owned artifacts before any uninstall deletion. */
        [[nodiscard]] bool VerifiedOwnedArtifacts(const BootstrapInstallationRequest &request, const std::filesystem::path &marker,
                                                  const Security::ArtifactVerifier &verifier) {
            if (auto present = Present(marker);
                present.HasError() ||
                (present.Value() &&
                 VerifyReadyUpdateStage(request.candidate.package, request.candidate.checkpoint, request.candidate.packageFile,
                                        request.candidate.stageRoot, request.candidate.inventory, request.archiveLimits, verifier)
                     .HasError()))
                return false;
            if (auto present = Present(request.candidate.packageFile);
                present.HasError() ||
                (present.Value() && VerifyCompletedUpdateTransfer(request.candidate.package, request.candidate.checkpoint,
                                                                  request.candidate.packageFile, verifier)
                                        .HasError()))
                return false;
            return true;
        }

        /** @brief Removes only the already checked stage tree and its ready marker. */
        [[nodiscard]] Result<void> RemoveOwnedStage(const BootstrapInstallationRequest &request, const std::filesystem::path &marker,
                                                    const bool stagePresent, const std::span<const std::filesystem::path> directories,
                                                    NativeDurableFileSystem &files) {
            const auto failed = [] {
                return Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
            };
            if (auto present = Present(marker); present.HasError() || (present.Value() && files.RemoveDurable(marker).HasError()))
                return failed();
            if (!stagePresent)
                return Result<void>::Success();
            for (const auto &file : request.candidate.inventory) {
                const auto path = request.candidate.stageRoot / std::filesystem::path(file.path);
                if (auto present = Present(path); present.HasError() || (present.Value() && files.RemoveDurable(path).HasError()))
                    return failed();
            }
            for (const auto &directory : directories) {
                if (auto removed = RemoveDirectory(directory, files); removed.HasError())
                    return removed;
            }
            return RemoveDirectory(request.candidate.stageRoot, files);
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
        const auto failed = [] {
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::UninstallFailed));
        };
        if (!Admitted(request))
            return failed();
        auto marker = request.candidate.stageRoot;
        marker += ".ready";
        if (!VerifiedOwnedArtifacts(request, marker, verifier_))
            return failed();
        auto stagePresent = Present(request.candidate.stageRoot);
        if (stagePresent.HasError())
            return failed();
        std::vector<std::filesystem::path> directories;
        if (stagePresent.Value()) {
            auto remaining = CheckRemaining(request);
            if (remaining.HasError())
                return failed();
            directories = std::move(remaining).Value();
        }
        if (auto removed = RemoveOwnedStage(request, marker, stagePresent.Value(), directories, files_); removed.HasError())
            return removed;
        if (auto present = Present(request.candidate.packageFile); present.HasError())
            return failed();
        else if (present.Value() && files_.RemoveDurable(request.candidate.packageFile).HasError())
            return failed();
        return Result<void>::Success();
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
