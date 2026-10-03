#include "Horo/Release/UpdateTransferCheckpointStore.h"
#include "Horo/Release/UpdateTransferErrors.h"
#include "Horo/Release/UpdateZipPackageProducer.h"
#include "Horo/Release/UpdateZipStagingJob.h"

#include <system_error>
#include <utility>
#include <vector>

namespace Horo::Release {
    namespace {
        /** @brief Accepts only absent output names under the reconstructed stage's private parent. */
        [[nodiscard]] bool ValidOutput(const std::filesystem::path &path, const std::filesystem::path &parent) {
            if (!path.is_absolute() || path.parent_path() != parent || path.filename().empty() || path.lexically_normal() != path)
                return false;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            return status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);
        }

        /** @brief Removes only destinations proven absent on entry, retaining an error if cleanup is uncertain. */
        [[nodiscard]] Result<void> ClearOwnedOutputs(const std::filesystem::path &temporary, const std::filesystem::path &stageRoot,
                                                     const std::filesystem::path &packageFile, const std::filesystem::path &checkpointFile,
                                                     NativeDurableFileSystem &files) {
            auto ready = stageRoot;
            ready += ".ready";
            auto prepared = ready;
            prepared += ".prepared";
            if (auto removed = files.RemoveDurable(prepared); removed.HasError())
                return removed;
            if (auto removed = files.RemoveDurable(ready); removed.HasError())
                return removed;
            if (auto removed = files.RemoveDurable(checkpointFile); removed.HasError())
                return removed;
            if (auto removed = files.RemoveDurable(packageFile); removed.HasError())
                return removed;
            std::error_code error;
            std::filesystem::remove_all(temporary, error);
            if (error)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
            return files.SyncDirectory(packageFile.parent_path());
        }

        /** @brief Gives locally reproduced signed bytes the same complete checkpoint shape as offline import. */
        [[nodiscard]] UpdateTransferCheckpoint CompleteCheckpoint(const UpdatePackageRecord &package) {
            return {package.digest, package.size, package.size, package.url, package.url, {}};
        }

        /** @brief Checks ownership of every output before the transaction can remove it. */
        [[nodiscard]] bool ValidRepackPaths(const RepackedDeltaZipRequest &request, const std::filesystem::path &temporary) {
            const auto &id = request.fullPackage.selection.artifact.package.value;
            const auto &parent = request.stageRoot.parent_path();
            auto ready = request.stageRoot;
            ready += ".ready";
            auto prepared = ready;
            prepared += ".prepared";
            return request.fullPackage.selection.format == DistributionPackageFormat::ZipArchive && IsValidDistributionIdentity(id) &&
                   request.stageRoot.is_absolute() && request.stageRoot.filename() == id &&
                   request.stageRoot.lexically_normal() == request.stageRoot && request.packageFile.filename() == id + ".zip" &&
                   request.checkpointFile.filename() == id + ".checkpoint" && ValidOutput(request.packageFile, parent) &&
                   ValidOutput(request.checkpointFile, parent) && ValidOutput(temporary, parent) && ValidOutput(ready, parent) &&
                   ValidOutput(prepared, parent);
        }

        /** @brief Converts the signed target plan into the producer's source inventory. */
        [[nodiscard]] Result<ReleasePreSignInventory> TargetInventory(const UpdateFileDeltaPlan &plan) {
            std::vector<ReleaseArtifactRecord> artifacts;
            artifacts.reserve(plan.targetFiles.size());
            for (const auto &file : plan.targetFiles)
                artifacts.emplace_back(file.target.path, ReleaseArtifactRole::Binary, file.target.size, file.target.digest);
            return ReleasePreSignInventory::Create(ReleaseCandidateId{1U}, std::move(artifacts));
        }

        /** @brief Returns the verified target list used by the existing ready-stage contract. */
        [[nodiscard]] std::vector<UpdateStagedFile> TargetFiles(const UpdateFileDeltaPlan &plan) {
            std::vector<UpdateStagedFile> target;
            target.reserve(plan.targetFiles.size());
            for (const auto &file : plan.targetFiles)
                target.push_back(file.target);
            return target;
        }

        /** @brief Preserves signed executable and entrypoint intent when reproducing the full ZIP. */
        struct LaunchFiles final {
            std::string entrypoint;
            std::vector<std::string> executablePaths;
        };

        /** @brief Extracts the validated target inventory's product launch metadata. */
        [[nodiscard]] LaunchFiles TargetLaunchFiles(const UpdateFileDeltaPlan &plan) {
            LaunchFiles launch;
            for (const auto &file : plan.targetFiles) {
                if (file.target.mode == UpdateFileMode::Executable)
                    launch.executablePaths.push_back(file.target.path);
                if (file.target.role == UpdateFileRole::Entrypoint)
                    launch.entrypoint = file.target.path;
            }
            return launch;
        }
    }  // namespace

    /** @copydoc RepackVerifiedDeltaAsFullZip */
    Result<RepackedDeltaZipResult> RepackVerifiedDeltaAsFullZip(const RepackedDeltaZipRequest &request, NativeDurableFileSystem &files,
                                                                const Security::ArtifactVerifier &verifier,
                                                                CancellationToken cancellation) {
        const auto &[full, plan, stageRoot, packageFile, checkpointFile, limits] = request;
        const auto mismatch = [] {
            return Result<RepackedDeltaZipResult>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        };
        const auto parent = stageRoot.parent_path();
        const auto temporary = parent / (stageRoot.filename().string() + ".repack");
        if (!ValidRepackPaths(request, temporary))
            return mismatch();
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(stageRoot, error)) || error)
            return mismatch();
        if (cancellation.IsCancellationRequested())
            return Result<RepackedDeltaZipResult>::Failure(MakeError(UpdateTransferErrors::Cancelled));
        if (auto verified = VerifyUpdateFileDeltaStage(stageRoot, plan, limits); verified.HasError())
            return Result<RepackedDeltaZipResult>::Failure(verified.ErrorValue());
        auto available = files.AvailableBytes(parent);
        if (available.HasError())
            return Result<RepackedDeltaZipResult>::Failure(available.ErrorValue());
        if (limits.reserveBytes > available.Value() || full.size > (available.Value() - limits.reserveBytes) / 2U)
            return Result<RepackedDeltaZipResult>::Failure(MakeError(UpdateTransferErrors::InsufficientSpace));

        auto inventory = TargetInventory(plan);
        if (inventory.HasError())
            return Result<RepackedDeltaZipResult>::Failure(inventory.ErrorValue());
        if (!std::filesystem::create_directory(temporary, error) || error)
            return mismatch();
        const auto fail = [&](const Error &reason) {
            if (auto cleared = ClearOwnedOutputs(temporary, stageRoot, packageFile, checkpointFile, files); cleared.HasError())
                return Result<RepackedDeltaZipResult>::Failure(cleared.ErrorValue());
            return Result<RepackedDeltaZipResult>::Failure(reason);
        };
        UpdateZipPackageProducer producer{limits};
        auto launch = TargetLaunchFiles(plan);
        auto produced = producer.Produce(
            {full.selection, inventory.Value(), stageRoot, temporary, std::move(launch.entrypoint), std::move(launch.executablePaths)});
        if (produced.HasError())
            return fail(produced.ErrorValue());
        if (produced.Value().files.size() != 1U || produced.Value().files.front().size != full.size ||
            produced.Value().files.front().digest != full.digest)
            return fail(MakeError(UpdateTransferErrors::StageMismatch));
        const auto temporaryPackage = temporary / "update.zip";
        const auto checkpoint = CompleteCheckpoint(full);
        if (auto verified = VerifyCompletedUpdateTransfer(full, checkpoint, temporaryPackage, verifier); verified.HasError())
            return fail(verified.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return fail(MakeError(UpdateTransferErrors::Cancelled));
        if (auto copied = files.CopyDurable(temporaryPackage, packageFile); copied.HasError())
            return fail(copied.ErrorValue());
        if (auto saved = SaveUpdateTransferCheckpoint(files, packageFile, checkpointFile, checkpoint); saved.HasError())
            return fail(saved.ErrorValue());
        if (auto removed = files.RemoveDurable(temporaryPackage); removed.HasError())
            return fail(removed.ErrorValue());
        std::filesystem::remove(temporary, error);
        if (error)
            return fail(MakeError(UpdateTransferErrors::StageMismatch));
        if (auto synced = files.SyncDirectory(parent); synced.HasError())
            return fail(synced.ErrorValue());
        const auto target = TargetFiles(plan);
        auto published =
            PublishVerifiedUpdateStage({full, checkpoint, packageFile, stageRoot, target, limits}, files, verifier, cancellation);
        if (published.HasError())
            return fail(published.ErrorValue());
        return Result<RepackedDeltaZipResult>::Success({checkpoint, std::move(published).Value()});
    }
}  // namespace Horo::Release
