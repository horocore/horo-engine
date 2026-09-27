#include "Horo/Release/BootstrapInstallation.h"

#include "Horo/Release/BootstrapInstallationErrors.h"

#include <fstream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::string_view PendingHeader = "horo-bootstrap-install-v1\n";
        constexpr std::uintmax_t MaximumRecordBytes = 1024U;

        struct InstallPaths final {
            std::filesystem::path root;
            std::filesystem::path versions;
            std::filesystem::path active;
            std::filesystem::path prepared;
            std::filesystem::path pending;
            std::filesystem::path lock;
        };

        /** @brief Keeps first-install transaction files separate from immutable version trees. */
        [[nodiscard]] InstallPaths Paths(const std::filesystem::path &root) {
            return {root,
                    root / "versions",
                    root / "active-version",
                    root / "active-version.prepared",
                    root / "bootstrap.pending",
                    root / ".activation.lock"};
        }

        /** @brief Rejects lexical aliases before accessing an installation root. */
        [[nodiscard]] bool CanonicalAbsolute(const std::filesystem::path &path) {
            if (!path.is_absolute() || path.filename().empty())
                return false;
            for (const auto &part : path) {
                if (part == "." || part == "..")
                    return false;
            }
            return true;
        }

        /** @brief Does not follow links while checking for an existing transaction file. */
        [[nodiscard]] Result<bool> Exists(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<bool>::Success(false);
            return error ? Result<bool>::Failure(MakeError(BootstrapInstallationErrors::InvalidLayout)) : Result<bool>::Success(true);
        }

        /** @brief Reads only one bounded, regular, single-link transaction record. */
        [[nodiscard]] Result<std::string> ReadRecord(const std::filesystem::path &path) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error ||
                std::filesystem::hard_link_count(path, error) != 1U || error)
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
            const auto &id = artifact.package.value;
            if (!CanonicalAbsolute(paths.root) || request.healthTimeout.count() <= 0 || request.healthTimeout > std::chrono::minutes{5} ||
                !artifact.installation || artifact.artifactClass != DistributionArtifactClass::InstallableProduct ||
                !IsValidDistributionIdentity(id) || request.candidate.stageRoot != paths.versions / id ||
                request.candidate.packageFile != paths.versions / (id + ".zip") ||
                selection.format != DistributionPackageFormat::ZipArchive)
                return false;
            std::error_code error;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(paths.root, error)) || error ||
                !std::filesystem::is_directory(std::filesystem::symlink_status(paths.versions, error)) || error)
                return false;
            auto admitted = ValidateDistributionPackageSelection(artifact, selection.format);
            return admitted.HasValue() && admitted.Value() == selection;
        }

        /** @brief Removes a new pointer only if it still names this candidate. */
        [[nodiscard]] Result<void> RemoveCandidatePointer(const InstallPaths &paths, const std::string &record,
                                                          NativeDurableFileSystem &files) {
            auto present = Exists(paths.active);
            if (present.HasError())
                return Result<void>::Failure(present.ErrorValue());
            if (!present.Value())
                return Result<void>::Success();
            auto active = ReadRecord(paths.active);
            if (active.HasError() || active.Value() != record)
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
            auto pending = ReadRecord(paths.pending);
            if (pending.HasError() || pending.Value() != std::string{PendingHeader} + record)
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
        auto current = ReadRecord(paths.active);
        if (current.HasError() || current.Value() != record)
            return Result<BootstrapInstallationOutcome>::Failure(MakeError(BootstrapInstallationErrors::PendingMismatch));
        if (auto removed = files.RemoveDurable(paths.pending); removed.HasError())
            return FailAndUndo(request, paths, record, files, host, removed.ErrorValue());
        return Result<BootstrapInstallationOutcome>::Success(BootstrapInstallationOutcome::Installed);
    }
}  // namespace Horo::Release
