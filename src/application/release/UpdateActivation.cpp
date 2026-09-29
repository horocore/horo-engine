#include "Horo/Release/UpdateActivation.h"

#include "Horo/Release/UpdateActivationErrors.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        constexpr std::string_view ActiveHeader = "horo-active-version-v1\n";
        constexpr std::string_view PendingHeader = "horo-update-activation-v2\n";
        constexpr std::string_view PinPresent = "pin-present\n";
        constexpr std::string_view PinAbsent = "pin-absent\n";
        constexpr std::uintmax_t MaximumRecordBytes = 1024U;

        struct ActivationPaths final {
            std::filesystem::path root;
            std::filesystem::path versions;
            std::filesystem::path active;
            std::filesystem::path prepared;
            std::filesystem::path lastKnownGood;
            std::filesystem::path lastKnownGoodPrepared;
            std::filesystem::path pending;
            std::filesystem::path lock;
        };

        /** @brief Keeps all mutable transaction files within one durable installation root. */
        [[nodiscard]] ActivationPaths Paths(const std::filesystem::path &root) {
            return {root,
                    root / "versions",
                    root / "active-version",
                    root / "active-version.prepared",
                    root / "last-known-good-version",
                    root / "last-known-good-version.prepared",
                    root / "activation.pending",
                    root / ".activation.lock"};
        }

        /** @brief Rejects lexical aliases before any native filesystem operation. */
        [[nodiscard]] bool CanonicalAbsolute(const std::filesystem::path &path) {
            if (!path.is_absolute() || path.filename().empty())
                return false;
            return std::ranges::none_of(path, [](const auto &part) {
                return part == "." || part == "..";
            });
        }

        /** @brief Checks that one signed package belongs to its exact version directory. */
        [[nodiscard]] bool VersionPathsMatch(const UpdateActivationVersion &version, const ActivationPaths &paths) {
            const auto &selection = version.package.selection;
            const auto &id = selection.artifact.package.value;
            const bool zip = selection.format == DistributionPackageFormat::ZipArchive;
            const bool linuxTar =
                selection.format == DistributionPackageFormat::TarGzip && selection.artifact.platform == DistributionPlatform::Linux;
            return IsValidDistributionIdentity(id) && (zip || linuxTar) && version.stageRoot == paths.versions / id &&
                   version.packageFile == paths.versions / (id + (zip ? ".zip" : ".tar.gz"));
        }

        /** @brief Requires two versions of one installable product and one protected layout. */
        [[nodiscard]] bool ValidRequest(const UpdateActivationRequest &request, const ActivationPaths &paths) {
            const auto &current = request.current.package.selection.artifact;
            const auto &staged = request.staged.package.selection.artifact;
            if (!CanonicalAbsolute(paths.root) || request.healthTimeout.count() <= 0 || request.healthTimeout > std::chrono::minutes{5} ||
                current.package == staged.package || !current.installation || current.installation != staged.installation ||
                current.product != staged.product || current.platform != staged.platform || current.architecture != staged.architecture ||
                current.artifactClass != DistributionArtifactClass::InstallableProduct ||
                staged.artifactClass != DistributionArtifactClass::InstallableProduct ||
                request.current.package.selection.format == DistributionPackageFormat::DeltaZipArchive ||
                request.staged.package.selection.format == DistributionPackageFormat::DeltaZipArchive ||
                !VersionPathsMatch(request.current, paths) || !VersionPathsMatch(request.staged, paths) ||
                request.current.packageFile == request.staged.stageRoot || request.staged.packageFile == request.current.stageRoot)
                return false;
            if (const auto isDirectory = [](const std::filesystem::path &path) {
                std::error_code error;
                return std::filesystem::is_directory(std::filesystem::symlink_status(path, error)) && !error;
            }; !isDirectory(paths.root) || !isDirectory(paths.versions))
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
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) || error)
                return Result<std::string>::Failure(MakeError(invalid));
            if (std::filesystem::hard_link_count(path, error) != 1U || error)
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
            if (const auto status = std::filesystem::symlink_status(path, error);
                status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                return Result<bool>::Success(false);
            return error ? Result<bool>::Failure(MakeError(UpdateActivationErrors::PendingMismatch)) : Result<bool>::Success(true);
        }

        /** @brief Admits only a canonical pointer record before preserving an existing rollback pin. */
        [[nodiscard]] bool ValidPointerRecord(const std::string_view record) {
            if (!record.starts_with(ActiveHeader))
                return false;
            const auto body = record.substr(ActiveHeader.size());
            const auto separator = body.find('\n');
            if (separator == std::string_view::npos || !IsValidDistributionIdentity(body.substr(0, separator)))
                return false;
            const auto digest = body.substr(separator + 1U);
            if (digest.size() != 72U || !digest.starts_with("sha256:") || digest.back() != '\n')
                return false;
            return std::ranges::all_of(digest.substr(7U, 64U), [](const char digit) {
                return (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
            });
        }

        /** @brief Preserves the prior rollback pin in the journal before activation can replace it. */
        [[nodiscard]] Result<std::optional<std::string>> ReadPriorPin(const ActivationPaths &paths) {
            if (auto prepared = PendingExists(paths.lastKnownGoodPrepared); prepared.HasError() || prepared.Value())
                return Result<std::optional<std::string>>::Failure(MakeError(UpdateActivationErrors::PendingMismatch));
            auto present = PendingExists(paths.lastKnownGood);
            if (present.HasError())
                return Result<std::optional<std::string>>::Failure(present.ErrorValue());
            if (!present.Value())
                return Result<std::optional<std::string>>::Success(std::nullopt);
            auto record = ReadRecord(paths.lastKnownGood, UpdateActivationErrors::PendingMismatch);
            if (record.HasError() || !ValidPointerRecord(record.Value()))
                return Result<std::optional<std::string>>::Failure(MakeError(UpdateActivationErrors::PendingMismatch));
            return Result<std::optional<std::string>>::Success(std::move(record).Value());
        }

        /** @brief Rejects a journal that cannot prove its exact previous pin state. */
        [[nodiscard]] Result<std::optional<std::string>> DecodePriorPin(const std::string_view journal, const std::string &previous,
                                                                        const std::string &target) {
            const std::string prefix = std::string{PendingHeader} + previous + target;
            if (!journal.starts_with(prefix))
                return Result<std::optional<std::string>>::Failure(MakeError(UpdateActivationErrors::PendingMismatch));
            const std::string_view suffix{journal.data() + prefix.size(), journal.size() - prefix.size()};
            if (suffix == PinAbsent)
                return Result<std::optional<std::string>>::Success(std::nullopt);
            if (!suffix.starts_with(PinPresent) || !ValidPointerRecord(suffix.substr(PinPresent.size())))
                return Result<std::optional<std::string>>::Failure(MakeError(UpdateActivationErrors::PendingMismatch));
            return Result<std::optional<std::string>>::Success(std::string{suffix.substr(PinPresent.size())});
        }

        /** @brief Atomically publishes a rollback pin under the shared activation lock. */
        [[nodiscard]] Result<void> ReplacePin(const ActivationPaths &paths, const std::string &record, NativeDurableFileSystem &files) {
            if (auto removed = files.RemoveDurable(paths.lastKnownGoodPrepared); removed.HasError())
                return removed;
            if (auto written = files.AppendPrivateDurable(paths.lastKnownGoodPrepared, 0U, std::as_bytes(std::span{record}));
                written.HasError())
                return written;
            return files.AtomicReplace(paths.lastKnownGoodPrepared, paths.lastKnownGood);
        }

        /** @brief Restores the old rollback pin, or its proven absence, before clearing the journal. */
        [[nodiscard]] Result<void> RestorePriorPin(const ActivationPaths &paths, const std::optional<std::string> &priorPin,
                                                   NativeDurableFileSystem &files) {
            if (priorPin)
                return ReplacePin(paths, *priorPin, files);
            if (auto removed = files.RemoveDurable(paths.lastKnownGoodPrepared); removed.HasError())
                return removed;
            return files.RemoveDurable(paths.lastKnownGood);
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
                                                   const std::optional<std::string> &priorPin, NativeDurableFileSystem &files) {
            if (auto replaced = ReplaceActive(paths, previous, files); replaced.HasError())
                return Result<void>::Failure(WrapError(UpdateActivationErrors::RollbackFailed, replaced.ErrorValue()));
            if (auto restored = RestorePriorPin(paths, priorPin, files); restored.HasError())
                return Result<void>::Failure(WrapError(UpdateActivationErrors::RollbackFailed, restored.ErrorValue()));
            return files.RemoveDurable(paths.pending);
        }

        /** @brief Resolves an interrupted transaction to its verified previous version. */
        [[nodiscard]] Result<UpdateActivationOutcome> RecoverPending(const ActivationPaths &paths, const std::string &previous,
                                                                     const std::string &target, NativeDurableFileSystem &files) {
            auto pending = ReadRecord(paths.pending, UpdateActivationErrors::PendingMismatch);
            auto active = ReadRecord(paths.active, UpdateActivationErrors::CurrentMismatch);
            if (pending.HasError() || active.HasError())
                return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateActivationErrors::PendingMismatch));
            auto priorPin = DecodePriorPin(pending.Value(), previous, target);
            if (priorPin.HasError() || (active.Value() != target && active.Value() != previous))
                return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateActivationErrors::PendingMismatch));
            if (auto restored = RestorePrevious(paths, previous, priorPin.Value(), files); restored.HasError())
                return Result<UpdateActivationOutcome>::Failure(restored.ErrorValue());
            return Result<UpdateActivationOutcome>::Success(UpdateActivationOutcome::RecoveredPrevious);
        }

        /** @brief Publishes the rollback record before changing the active pointer. */
        [[nodiscard]] Result<void> StartTransaction(const ActivationPaths &paths, const std::string &previous, const std::string &target,
                                                    const std::optional<std::string> &priorPin, NativeDurableFileSystem &files) {
            const std::string pending =
                std::string{PendingHeader} + previous + target + (priorPin ? std::string{PinPresent} + *priorPin : std::string{PinAbsent});
            return files.AppendPrivateDurable(paths.pending, 0U, std::as_bytes(std::span{pending}));
        }

        /** @brief Runs the probe and clears the journal only after a healthy new version. */
        [[nodiscard]] Result<UpdateActivationOutcome> FinishActivation(const UpdateActivationRequest &request, const ActivationPaths &paths,
                                                                       const std::string &previous,
                                                                       const std::optional<std::string> &priorPin,
                                                                       NativeDurableFileSystem &files, IUpdateActivationHost &host) {
            if (auto health = host.ProbeStartupHealth(request.staged.stageRoot, request.healthTimeout); health.HasError()) {
                if (auto restored = RestorePrevious(paths, previous, priorPin, files); restored.HasError())
                    return Result<UpdateActivationOutcome>::Failure(restored.ErrorValue());
                return Result<UpdateActivationOutcome>::Failure(WrapError(UpdateActivationErrors::HealthFailed, health.ErrorValue()));
            }
            if (auto pinned = ReplacePin(paths, previous, files); pinned.HasError()) {
                if (auto restored = RestorePrevious(paths, previous, priorPin, files); restored.HasError())
                    return Result<UpdateActivationOutcome>::Failure(restored.ErrorValue());
                return Result<UpdateActivationOutcome>::Failure(pinned.ErrorValue());
            }
            if (auto removed = files.RemoveDurable(paths.pending); removed.HasError()) {
                if (auto restored = RestorePrevious(paths, previous, priorPin, files); restored.HasError())
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
        auto admission = files.TryAcquireProductMaintenance(paths.root);
        if (admission.HasError())
            return Result<UpdateActivationOutcome>::Failure(admission.ErrorValue());
        [[maybe_unused]] ProductLaunchLease maintenance = std::move(admission).Value();
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
        if (auto active = ReadRecord(paths.active, UpdateActivationErrors::CurrentMismatch);
            active.HasError() || active.Value() != previous.Value())
            return Result<UpdateActivationOutcome>::Failure(MakeError(UpdateActivationErrors::CurrentMismatch));
        auto priorPin = ReadPriorPin(paths);
        if (priorPin.HasError())
            return Result<UpdateActivationOutcome>::Failure(priorPin.ErrorValue());
        if (auto started = StartTransaction(paths, previous.Value(), target.Value(), priorPin.Value(), files); started.HasError())
            return Result<UpdateActivationOutcome>::Failure(started.ErrorValue());
        if (auto switched = ReplaceActive(paths, target.Value(), files); switched.HasError())
            return Result<UpdateActivationOutcome>::Failure(switched.ErrorValue());
        return FinishActivation(request, paths, previous.Value(), priorPin.Value(), files, host);
    }
}  // namespace Horo::Release
