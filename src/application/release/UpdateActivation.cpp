#include "Horo/Release/UpdateActivation.h"

#include "Horo/Release/UpdateActivationErrors.h"

#include <fstream>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::string_view ActiveHeader = "horo-active-version-v1\n";
        constexpr std::string_view PendingHeader = "horo-update-activation-v1\n";
        constexpr std::uintmax_t MaximumRecordBytes = 1024U;

        struct ActivationPaths final {
            std::filesystem::path root;
            std::filesystem::path versions;
            std::filesystem::path active;
            std::filesystem::path prepared;
            std::filesystem::path pending;
            std::filesystem::path lock;
        };

        /** @brief Keeps all mutable transaction files within one durable installation root. */
        [[nodiscard]] ActivationPaths Paths(const std::filesystem::path &root) {
            return {root,
                    root / "versions",
                    root / "active-version",
                    root / "active-version.prepared",
                    root / "activation.pending",
                    root / ".activation.lock"};
        }

        /** @brief Rejects lexical aliases before any native filesystem operation. */
        [[nodiscard]] bool CanonicalAbsolute(const std::filesystem::path &path) {
            if (!path.is_absolute() || path.filename().empty())
                return false;
            for (const auto &part : path) {
                if (part == "." || part == "..")
                    return false;
            }
            return true;
        }

        /** @brief Checks that one signed package belongs to its exact version directory. */
        [[nodiscard]] bool VersionPathsMatch(const UpdateActivationVersion &version, const ActivationPaths &paths) {
            const auto &id = version.package.selection.artifact.package.value;
            return IsValidDistributionIdentity(id) && version.stageRoot == paths.versions / id &&
                   version.packageFile == paths.versions / (id + ".zip");
        }

        /** @brief Requires two versions of one installable product and one protected layout. */
        [[nodiscard]] bool ValidRequest(const UpdateActivationRequest &request, const ActivationPaths &paths) {
            const auto &current = request.current.package.selection.artifact;
            const auto &staged = request.staged.package.selection.artifact;
            if (!CanonicalAbsolute(paths.root) || request.healthTimeout.count() <= 0 || request.healthTimeout > std::chrono::minutes{5} ||
                current.package == staged.package || !current.installation || current.installation != staged.installation ||
                current.product != staged.product || current.platform != staged.platform || current.architecture != staged.architecture ||
                current.artifactClass != DistributionArtifactClass::InstallableProduct ||
                staged.artifactClass != DistributionArtifactClass::InstallableProduct || !VersionPathsMatch(request.current, paths) ||
                !VersionPathsMatch(request.staged, paths) || request.current.packageFile == request.staged.stageRoot ||
                request.staged.packageFile == request.current.stageRoot)
                return false;
            std::error_code error;
            if (!std::filesystem::is_directory(std::filesystem::symlink_status(paths.root, error)) || error ||
                !std::filesystem::is_directory(std::filesystem::symlink_status(paths.versions, error)) || error)
                return false;
            auto currentSelection = ValidateDistributionPackageSelection(current, request.current.package.selection.format);
            auto stagedSelection = ValidateDistributionPackageSelection(staged, request.staged.package.selection.format);
            return currentSelection.HasValue() && stagedSelection.HasValue() &&
                   currentSelection.Value() == request.current.package.selection &&
                   stagedSelection.Value() == request.staged.package.selection &&
                   currentSelection.Value().capabilities.updates != DistributionCapability::Unsupported &&
                   stagedSelection.Value().capabilities.updates != DistributionCapability::Unsupported;
        }

        /** @brief Reads only a single-link, bounded, regular transaction record. */
        [[nodiscard]] Result<std::string> ReadRecord(const std::filesystem::path &path, const ErrorCodeDescriptor &invalid) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error ||
                std::filesystem::hard_link_count(path, error) != 1U || error)
                return Result<std::string>::Failure(MakeError(invalid));
            const auto size = std::filesystem::file_size(path, error);
            if (error || size == 0U || size > MaximumRecordBytes)
                return Result<std::string>::Failure(MakeError(invalid));
            std::ifstream input(path, std::ios::binary);
            std::string record(static_cast<std::size_t>(size), '\0');
            input.read(record.data(), static_cast<std::streamsize>(record.size()));
            return input ? Result<std::string>::Success(std::move(record)) : Result<std::string>::Failure(MakeError(invalid));
        }

        /** @brief Detects a pending transaction without following a stale symlink. */
        [[nodiscard]] Result<bool> PendingExists(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<bool>::Success(false);
            return error ? Result<bool>::Failure(MakeError(UpdateActivationErrors::PendingMismatch)) : Result<bool>::Success(true);
        }

        /** @brief Writes one pointer through an absent prepared file and a durable atomic replacement. */
        [[nodiscard]] Result<void> ReplaceActive(const ActivationPaths &paths, const std::string &record, NativeDurableFileSystem &files) {
            if (auto removed = files.RemoveDurable(paths.prepared); removed.HasError())
                return removed;
            if (auto written = files.AppendPrivateDurable(paths.prepared, 0U, std::as_bytes(std::span{record})); written.HasError())
                return written;
            return files.AtomicReplace(paths.prepared, paths.active);
        }

        /** @brief Returns to the previous pointer, retaining the journal if replacement is uncertain. */
        [[nodiscard]] Result<void> RestorePrevious(const ActivationPaths &paths, const std::string &previous,
                                                   NativeDurableFileSystem &files) {
            if (auto replaced = ReplaceActive(paths, previous, files); replaced.HasError())
                return Result<void>::Failure(WrapError(UpdateActivationErrors::RollbackFailed, replaced.ErrorValue()));
            return files.RemoveDurable(paths.pending);
        }

        /** @brief Resolves an interrupted transaction to its verified previous version. */
        [[nodiscard]] Result<UpdateActivationOutcome> RecoverPending(const ActivationPaths &paths, const std::string &previous,
                                                                     const std::string &target, NativeDurableFileSystem &files) {
            const std::string expected = std::string{PendingHeader} + previous + target;
            auto pending = ReadRecord(paths.pending, UpdateActivationErrors::PendingMismatch);
            auto active = ReadRecord(paths.active, UpdateActivationErrors::CurrentMismatch);
            if (pending.HasError() || active.HasError() || pending.Value() != expected)
                return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateActivationErrors::PendingMismatch));
            if (active.Value() == target) {
                if (auto restored = RestorePrevious(paths, previous, files); restored.HasError())
                    return Result<UpdateActivationOutcome>::Failure(restored.ErrorValue());
            } else if (active.Value() == previous) {
                if (auto removed = files.RemoveDurable(paths.pending); removed.HasError())
                    return Result<UpdateActivationOutcome>::Failure(removed.ErrorValue());
            } else {
                return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateActivationErrors::PendingMismatch));
            }
            return Result<UpdateActivationOutcome>::Success(UpdateActivationOutcome::RecoveredPrevious);
        }

        /** @brief Publishes the rollback record before changing the active pointer. */
        [[nodiscard]] Result<void> StartTransaction(const ActivationPaths &paths, const std::string &previous, const std::string &target,
                                                    NativeDurableFileSystem &files) {
            const std::string pending = std::string{PendingHeader} + previous + target;
            return files.AppendPrivateDurable(paths.pending, 0U, std::as_bytes(std::span{pending}));
        }

        /** @brief Runs the probe and clears the journal only after a healthy new version. */
        [[nodiscard]] Result<UpdateActivationOutcome> FinishActivation(const UpdateActivationRequest &request, const ActivationPaths &paths,
                                                                       const std::string &previous, NativeDurableFileSystem &files,
                                                                       IUpdateActivationHost &host) {
            auto health = host.ProbeStartupHealth(request.staged.stageRoot, request.healthTimeout);
            if (health.HasError()) {
                if (auto restored = RestorePrevious(paths, previous, files); restored.HasError())
                    return Result<UpdateActivationOutcome>::Failure(restored.ErrorValue());
                return Result<UpdateActivationOutcome>::Failure(WrapError(UpdateActivationErrors::HealthFailed, health.ErrorValue()));
            }
            if (auto removed = files.RemoveDurable(paths.pending); removed.HasError()) {
                if (auto restored = RestorePrevious(paths, previous, files); restored.HasError())
                    return Result<UpdateActivationOutcome>::Failure(restored.ErrorValue());
                return Result<UpdateActivationOutcome>::Failure(removed.ErrorValue());
            }
            return Result<UpdateActivationOutcome>::Success(UpdateActivationOutcome::Activated);
        }
    }  // namespace

    /** @copydoc EncodeActiveUpdateRecord */
    Result<std::string> EncodeActiveUpdateRecord(const UpdatePackageRecord &package) {
        const auto &id = package.selection.artifact.package.value;
        if (!IsValidDistributionIdentity(id))
            return Result<std::string>::Failure(MakeError(UpdateActivationErrors::InvalidLayout));
        return Result<std::string>::Success(std::string{ActiveHeader} + id + '\n' + FormatSha256(package.digest) + '\n');
    }

    /** @copydoc ActivateVerifiedUpdate */
    Result<UpdateActivationOutcome> ActivateVerifiedUpdate(const UpdateActivationRequest &request, NativeDurableFileSystem &files,
                                                           const Security::ArtifactVerifier &verifier, IUpdateActivationHost &host) {
        const auto paths = Paths(request.installationRoot);
        if (!ValidRequest(request, paths))
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateActivationErrors::InvalidLayout));
        auto lock = files.TryAcquireExclusive(paths.lock, "horo-update-activation");
        if (lock.HasError())
            return Result<UpdateActivationOutcome>::Failure(lock.ErrorValue());
        if (auto stopped = host.EnsureProductsStopped(paths.root); stopped.HasError())
            return Result<UpdateActivationOutcome>::Failure(stopped.ErrorValue());
        for (const auto *version : {&request.current, &request.staged}) {
            if (auto verified = VerifyReadyUpdateStage(version->package, version->checkpoint, version->packageFile, version->stageRoot,
                                                       version->inventory, request.archiveLimits, verifier);
                verified.HasError())
                return Result<UpdateActivationOutcome>::Failure(verified.ErrorValue());
        }
        auto previous = EncodeActiveUpdateRecord(request.current.package);
        auto target = EncodeActiveUpdateRecord(request.staged.package);
        if (previous.HasError() || target.HasError())
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateActivationErrors::InvalidLayout));
        auto pending = PendingExists(paths.pending);
        if (pending.HasError())
            return Result<UpdateActivationOutcome>::Failure(pending.ErrorValue());
        if (pending.Value())
            return RecoverPending(paths, previous.Value(), target.Value(), files);
        auto active = ReadRecord(paths.active, UpdateActivationErrors::CurrentMismatch);
        if (active.HasError() || active.Value() != previous.Value())
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateActivationErrors::CurrentMismatch));
        if (auto started = StartTransaction(paths, previous.Value(), target.Value(), files); started.HasError())
            return Result<UpdateActivationOutcome>::Failure(started.ErrorValue());
        if (auto switched = ReplaceActive(paths, target.Value(), files); switched.HasError())
            return Result<UpdateActivationOutcome>::Failure(switched.ErrorValue());
        return FinishActivation(request, paths, previous.Value(), files, host);
    }
}  // namespace Horo::Release
