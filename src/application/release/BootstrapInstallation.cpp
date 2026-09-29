#include "Horo/Release/BootstrapInstallation.h"

#include "Horo/Release/BootstrapInstallationErrors.h"

#include <algorithm>
#include <fstream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Release {
    /** @copydoc DetectBootstrapBuildTarget */
    Result<BootstrapBuildTarget> DetectBootstrapBuildTarget() {
#if (!defined(_WIN32) && !defined(__APPLE__) && !defined(__linux__)) ||                                                                    \
    (!defined(_M_X64) && !defined(__x86_64__) && !defined(_M_ARM64) && !defined(__aarch64__))
        return Result<BootstrapBuildTarget>::Failure(MakeError(BootstrapInstallationErrors::InvalidLayout));
#else
#if defined(_WIN32)
        constexpr auto platform = DistributionPlatform::Windows;
#elif defined(__APPLE__)
        constexpr auto platform = DistributionPlatform::MacOS;
#elif defined(__linux__)
        constexpr auto platform = DistributionPlatform::Linux;
#endif
#if defined(_M_X64) || defined(__x86_64__)
        constexpr auto architecture = DistributionArchitecture::X64;
#elif defined(_M_ARM64) || defined(__aarch64__)
        constexpr auto architecture = DistributionArchitecture::Arm64;
#endif
        return Result<BootstrapBuildTarget>::Success({platform, architecture});
#endif
    }

    /** @copydoc CheckBootstrapBuildTarget */
    Result<void> CheckBootstrapBuildTarget(const DistributionArtifactIdentity &artifact) {
        auto native = DetectBootstrapBuildTarget();
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        if (artifact.platform != native.Value().platform || artifact.architecture != native.Value().architecture)
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PreflightFailed));
        return Result<void>::Success();
    }

    namespace {
        constexpr std::string_view PendingHeader = "horo-bootstrap-install-v1\n";
        constexpr std::string_view UninstallHeader = "horo-bootstrap-uninstall-v1\n";
        constexpr std::uintmax_t MaximumRecordBytes = 1024U;

        struct InstallPaths final {
            std::filesystem::path root;
            std::filesystem::path versions;
            std::filesystem::path active;
            std::filesystem::path prepared;
            std::filesystem::path pending;
            std::filesystem::path uninstallPending;
            std::filesystem::path lock;
        };

        /** @brief Keeps first-install transaction files separate from immutable version trees. */
        [[nodiscard]] InstallPaths Paths(const std::filesystem::path &root) {
            return {root,
                    root / "versions",
                    root / "active-version",
                    root / "active-version.prepared",
                    root / "bootstrap.pending",
                    root / "bootstrap-uninstall.pending",
                    root / ".activation.lock"};
        }

        /** @brief Rejects lexical aliases before accessing an installation root. */
        [[nodiscard]] bool CanonicalAbsolute(const std::filesystem::path &path) {
            if (!path.is_absolute() || path.filename().empty())
                return false;
            return std::ranges::none_of(path, [](const auto &part) {
                return part == "." || part == "..";
            });
        }

        /** @brief Does not follow links while checking for an existing transaction file. */
        [[nodiscard]] Result<bool> Exists(const std::filesystem::path &path) {
            std::error_code error;
            if (const auto status = std::filesystem::symlink_status(path, error);
                status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<bool>::Success(false);
            return error ? Result<bool>::Failure(MakeError(BootstrapInstallationErrors::InvalidLayout)) : Result<bool>::Success(true);
        }

        /** @brief Reads only one bounded, regular, single-link transaction record. */
        [[nodiscard]] Result<std::string> ReadRecord(const std::filesystem::path &path) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
                return Result<std::string>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
            if (std::filesystem::hard_link_count(path, error) != 1U || error)
                return Result<std::string>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0U || size > MaximumRecordBytes)
                return Result<std::string>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
            std::ifstream input(path, std::ios::binary);
            std::string record(static_cast<std::size_t>(size), '\0');
            input.read(record.data(), static_cast<std::streamsize>(record.size()));
            return input ? Result<std::string>::Success(std::move(record))
                         : Result<std::string>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
        }

        /** @brief Requires the authenticated version to occupy its declared immutable package-ID directory. */
        [[nodiscard]] bool ValidRequest(const BootstrapInstallationRequest &request, const InstallPaths &paths) {
            const auto &selection = request.candidate.package.selection;
            const auto &artifact = selection.artifact;
            const bool zip = selection.format == DistributionPackageFormat::ZipArchive;
            const bool linuxTar =
                selection.format == DistributionPackageFormat::TarGzip && artifact.platform == DistributionPlatform::Linux;
            if (const auto &id = artifact.package.value;
                !CanonicalAbsolute(paths.root) || request.healthTimeout.count() <= 0 || request.healthTimeout > std::chrono::minutes{5} ||
                !artifact.installation || artifact.artifactClass != DistributionArtifactClass::InstallableProduct ||
                !IsValidDistributionIdentity(id) || request.candidate.stageRoot != paths.versions / id || (!zip && !linuxTar) ||
                request.candidate.packageFile != paths.versions / (id + (zip ? ".zip" : ".tar.gz")))
                return false;
            if (const auto isDirectory = [](const std::filesystem::path &path) {
                std::error_code error;
                return std::filesystem::is_directory(std::filesystem::symlink_status(path, error)) && !error;
            }; !isDirectory(paths.root) || !isDirectory(paths.versions))
                return false;
            auto admitted = ValidateDistributionPackageSelection(artifact, selection.format);
            return admitted.HasValue() && admitted.Value() == selection;
        }

        /** @brief Removes a new pointer only if it still names this candidate. */
        [[nodiscard]] Result<void> RemoveCandidatePointer(const InstallPaths &paths, const std::string_view record,
                                                          NativeDurableFileSystem &files) {
            auto present = Exists(paths.active);
            if (present.HasError())
                return Result<void>::Failure(present.ErrorValue());
            if (!present.Value())
                return Result<void>::Success();
            if (auto active = ReadRecord(paths.active); active.HasError() || active.Value() != record)
                return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
            return files.RemoveDurable(paths.active);
        }

        /** @brief Leaves a journal whenever deactivation or integration cleanup cannot be proven. */
        [[nodiscard]] Result<void> UndoIncomplete(const BootstrapInstallationRequest &request, const InstallPaths &paths,
                                                  const std::string &record, NativeDurableFileSystem &files,
                                                  IBootstrapInstallationHost &host) {
            if (auto removed = RemoveCandidatePointer(paths, record, files); removed.HasError())
                return Result<void>::Failure(WrapError(BootstrapInstallationErrors::RecoveryFailed, removed.ErrorValue()));
            if (auto unregistered = host.Unregister(request); unregistered.HasError())
                return Result<void>::Failure(WrapError(BootstrapInstallationErrors::RecoveryFailed, unregistered.ErrorValue()));
            if (auto removed = files.RemoveDurable(paths.prepared); removed.HasError())
                return Result<void>::Failure(WrapError(BootstrapInstallationErrors::RecoveryFailed, removed.ErrorValue()));
            if (auto removed = files.RemoveDurable(paths.pending); removed.HasError())
                return Result<void>::Failure(WrapError(BootstrapInstallationErrors::RecoveryFailed, removed.ErrorValue()));
            return Result<void>::Success();
        }

        /** @brief Resolves a prior attempt to a non-active state before retry is allowed. */
        [[nodiscard]] Result<BootstrapInstallationOutcome> RecoverPending(const BootstrapInstallationRequest &request,
                                                                          const InstallPaths &paths, const std::string &record,
                                                                          NativeDurableFileSystem &files,
                                                                          IBootstrapInstallationHost &host) {
            if (auto pending = ReadRecord(paths.pending); pending.HasError() || pending.Value() != std::string{PendingHeader} + record)
                return Result<BootstrapInstallationOutcome>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
            if (auto undone = UndoIncomplete(request, paths, record, files, host); undone.HasError())
                return Result<BootstrapInstallationOutcome>::Failure(undone.ErrorValue());
            return Result<BootstrapInstallationOutcome>::Success(BootstrapInstallationOutcome::RecoveredIncomplete);
        }

        /** @brief Returns the original failure only after a complete rollback. */
        [[nodiscard]] Result<BootstrapInstallationOutcome> FailAndUndo(const BootstrapInstallationRequest &request,
                                                                       const InstallPaths &paths, const std::string &record,
                                                                       NativeDurableFileSystem &files, IBootstrapInstallationHost &host,
                                                                       Error failure) {
            if (auto undone = UndoIncomplete(request, paths, record, files, host); undone.HasError())
                return Result<BootstrapInstallationOutcome>::Failure(undone.ErrorValue());
            return Result<BootstrapInstallationOutcome>::Failure(std::move(failure));
        }

        /** @brief Binds a resumable removal to the same authenticated file inventory. */
        [[nodiscard]] Result<std::string> UninstallRecord(const BootstrapInstallationRequest &request, const std::string &activeRecord) {
            auto inventory = BuildCanonicalUpdateFileInventory(request.candidate.inventory, request.archiveLimits);
            if (inventory.HasError())
                return Result<std::string>::Failure(inventory.ErrorValue());
            const auto digest = ComputeSha256(std::as_bytes(std::span{inventory.Value()}));
            return Result<std::string>::Success(std::string{UninstallHeader} + activeRecord + FormatSha256(digest) + "\n");
        }

        /** @brief Rejects concurrent update or first-install journals before repair or removal. */
        [[nodiscard]] Result<void> NoOtherTransition(const InstallPaths &paths) {
            for (const auto &path : {paths.pending, paths.prepared, paths.root / "activation.pending", paths.root / "rollback.pending"}) {
                auto present = Exists(path);
                if (present.HasError() || present.Value())
                    return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
            }
            return Result<void>::Success();
        }

        /** @brief Proves that an active pointer still names exactly this package. */
        [[nodiscard]] Result<void> RequireActive(const InstallPaths &paths, const std::string_view record) {
            if (auto active = ReadRecord(paths.active); active.HasError() || active.Value() != record)
                return Result<void>::Failure(MakeError(BootstrapInstallationErrors::AlreadyInstalled));
            return Result<void>::Success();
        }

        /** @brief Checks common stopped-product and transaction prerequisites after the caller holds the installation lock. */
        [[nodiscard]] Result<ProductLaunchLease> ReadyForMaintenance(const InstallPaths &paths, IBootstrapInstallationHost &host,
                                                                     const NativeDurableFileSystem &files) {
            if (auto stopped = host.EnsureProductsStopped(paths.root); stopped.HasError())
                return Result<ProductLaunchLease>::Failure(stopped.ErrorValue());
            if (auto clear = NoOtherTransition(paths); clear.HasError())
                return Result<ProductLaunchLease>::Failure(clear.ErrorValue());
            return files.TryAcquireProductMaintenance(paths.root);
        }

        struct MaintenanceGuards final {
            ExclusiveFileLock transaction;
            ProductLaunchLease admission;
        };

        /** @brief Keeps both OS locks alive across one complete repair or uninstall transaction. */
        [[nodiscard]] Result<MaintenanceGuards> BeginMaintenance(const BootstrapInstallationRequest &request,
                                                                 NativeDurableFileSystem &files, IBootstrapInstallationHost &host,
                                                                 const std::string_view owner) {
            const auto paths = Paths(request.installationRoot);
            if (!ValidRequest(request, paths))
                return Result<MaintenanceGuards>::Failure(MakeError(BootstrapInstallationErrors::InvalidLayout));
            auto lock = files.TryAcquireExclusive(paths.lock, owner);
            if (lock.HasError())
                return Result<MaintenanceGuards>::Failure(lock.ErrorValue());
            auto admission = ReadyForMaintenance(paths, host, files);
            if (admission.HasError())
                return Result<MaintenanceGuards>::Failure(admission.ErrorValue());
            return Result<MaintenanceGuards>::Success({std::move(lock).Value(), std::move(admission).Value()});
        }

        /** @brief Completes a resumed uninstall only after its journal and active pointer match the candidate. */
        [[nodiscard]] Result<void> ResumeUninstall(const InstallPaths &paths, const std::string_view expected,
                                                   const std::string_view record, NativeDurableFileSystem &files) {
            if (auto saved = ReadRecord(paths.uninstallPending); saved.HasError() || saved.Value() != expected)
                return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
            auto active = Exists(paths.active);
            if (active.HasError())
                return Result<void>::Failure(active.ErrorValue());
            if (!active.Value())
                return Result<void>::Success();
            if (auto match = RequireActive(paths, record); match.HasError())
                return match;
            if (auto removed = files.RemoveDurable(paths.active); removed.HasError())
                return Result<void>::Failure(WrapError(BootstrapInstallationErrors::UninstallFailed, removed.ErrorValue()));
            return Result<void>::Success();
        }

        /** @brief Journals and deactivates an authenticated active installation before host cleanup. */
        [[nodiscard]] Result<void> BeginUninstall(const BootstrapInstallationRequest &request, const InstallPaths &paths,
                                                  const std::string_view record, const std::string_view expected,
                                                  NativeDurableFileSystem &files, const Security::ArtifactVerifier &verifier) {
            if (auto active = RequireActive(paths, record); active.HasError())
                return active;
            if (auto verified =
                    VerifyReadyUpdateStage(request.candidate.package, request.candidate.checkpoint, request.candidate.packageFile,
                                           request.candidate.stageRoot, request.candidate.inventory, request.archiveLimits, verifier);
                verified.HasError())
                return verified;
            if (auto written = files.AppendPrivateDurable(paths.uninstallPending, 0U, std::as_bytes(std::span{expected}));
                written.HasError())
                return written;
            if (auto removed = files.RemoveDurable(paths.active); removed.HasError())
                return Result<void>::Failure(WrapError(BootstrapInstallationErrors::UninstallFailed, removed.ErrorValue()));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc BootstrapVerifiedInstallation */
    Result<BootstrapInstallationOutcome> BootstrapVerifiedInstallation(const BootstrapInstallationRequest &request,
                                                                       NativeDurableFileSystem &files,
                                                                       const Security::ArtifactVerifier &verifier,
                                                                       IBootstrapInstallationHost &host) {
        const auto paths = Paths(request.installationRoot);
        if (!ValidRequest(request, paths))
            return Result<BootstrapInstallationOutcome>::Failure(MakeError(BootstrapInstallationErrors::InvalidLayout));
        auto lock = files.TryAcquireExclusive(paths.lock, "horo-bootstrap-installation");
        if (lock.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(lock.ErrorValue());
        if (auto stopped = host.EnsureProductsStopped(paths.root); stopped.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(stopped.ErrorValue());
        auto admission = files.TryAcquireProductMaintenance(paths.root);
        if (admission.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(admission.ErrorValue());
        [[maybe_unused]] ProductLaunchLease maintenance = std::move(admission).Value();
        if (auto verified =
                VerifyReadyUpdateStage(request.candidate.package, request.candidate.checkpoint, request.candidate.packageFile,
                                       request.candidate.stageRoot, request.candidate.inventory, request.archiveLimits, verifier);
            verified.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(verified.ErrorValue());
        auto encoded = EncodeActiveUpdateRecord(request.candidate.package);
        if (encoded.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(encoded.ErrorValue());
        const auto &record = encoded.Value();
        auto pending = Exists(paths.pending);
        if (pending.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(pending.ErrorValue());
        if (pending.Value())
            return RecoverPending(request, paths, record, files, host);
        if (auto uninstallPending = Exists(paths.uninstallPending); uninstallPending.HasError() || uninstallPending.Value())
            return Result<BootstrapInstallationOutcome>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
        auto active = Exists(paths.active);
        auto prepared = Exists(paths.prepared);
        if (active.HasError() || prepared.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(MakeError(BootstrapInstallationErrors::InvalidLayout));
        if (active.Value())
            return Result<BootstrapInstallationOutcome>::Failure(MakeError(BootstrapInstallationErrors::AlreadyInstalled));
        if (prepared.Value())
            return Result<BootstrapInstallationOutcome>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
        if (auto preflight = host.Preflight(request); preflight.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(
                WrapError(BootstrapInstallationErrors::PreflightFailed, preflight.ErrorValue()));
        const std::string journal = std::string{PendingHeader} + record;
        if (auto written = files.AppendPrivateDurable(paths.pending, 0U, std::as_bytes(std::span{journal})); written.HasError())
            return Result<BootstrapInstallationOutcome>::Failure(written.ErrorValue());
        if (auto registered = host.Register(request); registered.HasError())
            return FailAndUndo(request, paths, record, files, host,
                               WrapError(BootstrapInstallationErrors::IntegrationFailed, registered.ErrorValue()));
        if (auto written = files.AppendPrivateDurable(paths.prepared, 0U, std::as_bytes(std::span{record})); written.HasError())
            return FailAndUndo(request, paths, record, files, host, written.ErrorValue());
        if (auto switched = files.AtomicReplace(paths.prepared, paths.active); switched.HasError())
            return FailAndUndo(request, paths, record, files, host, switched.ErrorValue());
        if (auto health = host.ProbeStartupHealth(request.candidate.stageRoot, request.healthTimeout); health.HasError())
            return FailAndUndo(request, paths, record, files, host,
                               WrapError(BootstrapInstallationErrors::HealthFailed, health.ErrorValue()));
        if (auto current = ReadRecord(paths.active); current.HasError() || current.Value() != record)
            return Result<BootstrapInstallationOutcome>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
        if (auto removed = files.RemoveDurable(paths.pending); removed.HasError())
            return FailAndUndo(request, paths, record, files, host, removed.ErrorValue());
        return Result<BootstrapInstallationOutcome>::Success(BootstrapInstallationOutcome::Installed);
    }

    /** @copydoc RepairVerifiedInstallation */
    Result<void> RepairVerifiedInstallation(const BootstrapInstallationRequest &request, NativeDurableFileSystem &files,
                                            const Security::ArtifactVerifier &verifier, IBootstrapInstallationHost &host) {
        auto guards = BeginMaintenance(request, files, host, "horo-bootstrap-repair");
        if (guards.HasError())
            return Result<void>::Failure(guards.ErrorValue());
        [[maybe_unused]] MaintenanceGuards maintenance = std::move(guards).Value();
        const auto paths = Paths(request.installationRoot);
        if (auto uninstallPending = Exists(paths.uninstallPending); uninstallPending.HasError() || uninstallPending.Value())
            return Result<void>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
        auto encoded = EncodeActiveUpdateRecord(request.candidate.package);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        if (auto active = RequireActive(paths, encoded.Value()); active.HasError())
            return active;
        if (auto verified =
                VerifyReadyUpdateStage(request.candidate.package, request.candidate.checkpoint, request.candidate.packageFile,
                                       request.candidate.stageRoot, request.candidate.inventory, request.archiveLimits, verifier);
            verified.HasError())
            return Result<void>::Failure(WrapError(BootstrapInstallationErrors::RepairFailed, verified.ErrorValue()));
        if (auto registered = host.Register(request); registered.HasError())
            return Result<void>::Failure(WrapError(BootstrapInstallationErrors::RepairFailed, registered.ErrorValue()));
        if (auto healthy = host.ProbeStartupHealth(request.candidate.stageRoot, request.healthTimeout); healthy.HasError())
            return Result<void>::Failure(WrapError(BootstrapInstallationErrors::RepairFailed, healthy.ErrorValue()));
        return RequireActive(paths, encoded.Value());
    }

    /** @copydoc UninstallVerifiedInstallation */
    Result<void> UninstallVerifiedInstallation(const BootstrapInstallationRequest &request, NativeDurableFileSystem &files,
                                               const Security::ArtifactVerifier &verifier, IBootstrapInstallationHost &host) {
        auto guards = BeginMaintenance(request, files, host, "horo-bootstrap-uninstall");
        if (guards.HasError())
            return Result<void>::Failure(guards.ErrorValue());
        [[maybe_unused]] MaintenanceGuards maintenance = std::move(guards).Value();
        const auto paths = Paths(request.installationRoot);
        auto encoded = EncodeActiveUpdateRecord(request.candidate.package);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        auto expected = UninstallRecord(request, encoded.Value());
        if (expected.HasError())
            return Result<void>::Failure(expected.ErrorValue());
        auto pending = Exists(paths.uninstallPending);
        if (pending.HasError())
            return Result<void>::Failure(pending.ErrorValue());
        if (const auto deactivated = pending.Value() ? ResumeUninstall(paths, expected.Value(), encoded.Value(), files)
                                                     : BeginUninstall(request, paths, encoded.Value(), expected.Value(), files, verifier);
            deactivated.HasError())
            return deactivated;
        if (auto unregistered = host.Unregister(request); unregistered.HasError())
            return Result<void>::Failure(WrapError(BootstrapInstallationErrors::UninstallFailed, unregistered.ErrorValue()));
        if (auto removed = host.RemoveOwnedVersion(request); removed.HasError())
            return Result<void>::Failure(WrapError(BootstrapInstallationErrors::UninstallFailed, removed.ErrorValue()));
        if (auto removed = files.RemoveDurable(paths.uninstallPending); removed.HasError())
            return Result<void>::Failure(WrapError(BootstrapInstallationErrors::UninstallFailed, removed.ErrorValue()));
        return Result<void>::Success();
    }
}  // namespace Horo::Release
