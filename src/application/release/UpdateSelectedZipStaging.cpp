#include "Horo/Release/UpdateDeltaStaging.h"
#include "Horo/Release/UpdateTransferCheckpointStore.h"
#include "Horo/Release/UpdateTransferErrors.h"
#include "Horo/Release/UpdateZipStagingJob.h"

#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Requires an absent private destination before either delivery route mutates it. */
        [[nodiscard]] bool Absent(const std::filesystem::path &path) {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            return status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);
        }

        /** @brief Keeps all host-supplied private names absolute and free of lexical aliases. */
        [[nodiscard]] bool NormalizedPrivatePath(const std::filesystem::path &path) {
            return path.is_absolute() && !path.filename().empty() && path.lexically_normal() == path;
        }

        /** @brief Checks the full route's names before either route can mutate the stage. */
        [[nodiscard]] bool ValidFullPaths(const SelectedZipStagingRequest &request) {
            const auto &full = request.candidates.full;
            const auto &id = full.selection.artifact.package.value;
            return full.selection.format == DistributionPackageFormat::ZipArchive && IsValidDistributionIdentity(id) &&
                   NormalizedPrivatePath(request.stageRoot) && NormalizedPrivatePath(request.fullPaths.partialFile) &&
                   NormalizedPrivatePath(request.fullPaths.checkpointFile) && request.stageRoot.filename() == id &&
                   request.fullPaths.partialFile.filename() == id + ".zip" &&
                   request.fullPaths.checkpointFile.filename() == id + ".checkpoint" &&
                   request.stageRoot.parent_path() == request.fullPaths.partialFile.parent_path() &&
                   request.stageRoot.parent_path() == request.fullPaths.checkpointFile.parent_path() && Absent(request.stageRoot);
        }

        /** @brief Checks the optional delta route's private names and signed full-package binding. */
        [[nodiscard]] bool ValidDeltaPaths(const SelectedZipStagingRequest &request) {
            const auto &delta = *request.candidates.delta;
            const auto &id = delta.package.selection.artifact.package.value;
            return delta.package.selection.format == DistributionPackageFormat::DeltaZipArchive &&
                   delta.fullPackage == request.candidates.full.selection.artifact.package && IsValidDistributionIdentity(id) &&
                   id != request.candidates.full.selection.artifact.package.value && NormalizedPrivatePath(request.deltaStageRoot) &&
                   NormalizedPrivatePath(request.deltaPaths.partialFile) && NormalizedPrivatePath(request.deltaPaths.checkpointFile) &&
                   request.deltaStageRoot.filename() == id && request.deltaPaths.partialFile.filename() == id + ".zip" &&
                   request.deltaPaths.checkpointFile.filename() == id + ".checkpoint" &&
                   request.deltaStageRoot.parent_path() == request.stageRoot.parent_path() &&
                   request.deltaPaths.partialFile.parent_path() == request.stageRoot.parent_path() &&
                   request.deltaPaths.checkpointFile.parent_path() == request.stageRoot.parent_path() && Absent(request.deltaStageRoot);
        }

        /** @brief A failed local reproduction must leave no new output authority before full fallback. */
        [[nodiscard]] bool SafeToFallback(const SelectedZipStagingRequest &request, const bool packageWasAbsent,
                                          const bool checkpointWasAbsent) {
            auto ready = request.stageRoot;
            ready += ".ready";
            auto prepared = ready;
            prepared += ".prepared";
            const auto temporary = request.stageRoot.parent_path() / (request.stageRoot.filename().string() + ".repack");
            return Absent(ready) && Absent(prepared) && Absent(temporary) && (!packageWasAbsent || Absent(request.fullPaths.partialFile)) &&
                   (!checkpointWasAbsent || Absent(request.fullPaths.checkpointFile));
        }

        /** @brief Removes a tree this worker created; uncertainty stops fallback before full staging. */
        [[nodiscard]] Result<void> RemoveOwnedStage(const std::filesystem::path &stageRoot, NativeDurableFileSystem &files) {
            std::error_code error;
            std::filesystem::remove_all(stageRoot, error);
            if (error)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
            return files.SyncDirectory(stageRoot.parent_path());
        }

        /** @brief Performs the one optional delta attempt without publishing partial activation authority. */
        [[nodiscard]] Result<RepackedDeltaZipResult> TryDelta(const SelectedZipStagingRequest &request, NativeDurableFileSystem &files,
                                                              const Security::ArtifactVerifier &verifier,
                                                              const CancellationToken cancellation,
                                                              const UpdateDownloadProgress &progress) {
            const auto &delta = *request.candidates.delta;
            auto downloaded = DownloadUpdatePackageHttps({delta.package, request.deltaPaths, request.downloadLimits, request.policy}, files,
                                                         verifier, cancellation, progress);
            if (downloaded.HasError())
                return Result<RepackedDeltaZipResult>::Failure(downloaded.ErrorValue());
            auto patch = StageVerifiedDeltaZipUpdate({delta.package, downloaded.Value(), request.deltaPaths.partialFile,
                                                      request.deltaStageRoot, request.archiveLimits},
                                                     files, verifier, cancellation);
            if (patch.HasError())
                return Result<RepackedDeltaZipResult>::Failure(patch.ErrorValue());
            auto plan = PlanUpdateFileDelta(request.baseInventory, request.targetInventory, patch.Value(), delta.baseInventoryDigest,
                                            delta.targetInventoryDigest, delta.deltaInventoryDigest, request.archiveLimits);
            if (plan.HasError())
                return Result<RepackedDeltaZipResult>::Failure(plan.ErrorValue());
            if (auto reconstructed = ReconstructUpdateFileDeltaStage(plan.Value(), request.baseRoot, request.deltaStageRoot,
                                                                     request.stageRoot, request.archiveLimits, files, cancellation);
                reconstructed.HasError())
                return Result<RepackedDeltaZipResult>::Failure(reconstructed.ErrorValue());
            return RepackVerifiedDeltaAsFullZip({request.candidates.full, plan.Value(), request.stageRoot, request.fullPaths.partialFile,
                                                 request.fullPaths.checkpointFile, request.archiveLimits},
                                                files, verifier, cancellation);
        }
    }  // namespace

    /** @copydoc PrepareSelectedZipUpdateStageHttps */
    Result<SelectedZipStagingResult> PrepareSelectedZipUpdateStageHttps(const SelectedZipStagingRequest &request,
                                                                        NativeDurableFileSystem &files,
                                                                        const Security::ArtifactVerifier &verifier,
                                                                        const CancellationToken cancellation,
                                                                        const UpdateDownloadProgress &progress) {
        const auto &full = request.candidates.full;
        const auto invalid = [] {
            return Result<SelectedZipStagingResult>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        };
        if (!ValidFullPaths(request))
            return invalid();
        if (request.candidates.delta) {
            if (!ValidDeltaPaths(request))
                return invalid();
            const bool packageWasAbsent = Absent(request.fullPaths.partialFile);
            const bool checkpointWasAbsent = Absent(request.fullPaths.checkpointFile);
            auto repacked = TryDelta(request, files, verifier, cancellation, progress);
            if (repacked.HasValue()) {
                auto result = std::move(repacked).Value();
                return Result<SelectedZipStagingResult>::Success({std::move(result.checkpoint), std::move(result.readyMarker), true});
            }
            if (cancellation.IsCancellationRequested())
                return Result<SelectedZipStagingResult>::Failure(repacked.ErrorValue());
            if (!Absent(request.stageRoot)) {
                if (auto removed = RemoveOwnedStage(request.stageRoot, files); removed.HasError())
                    return Result<SelectedZipStagingResult>::Failure(removed.ErrorValue());
            }
            if (!Absent(request.deltaStageRoot)) {
                if (auto removed = RemoveOwnedStage(request.deltaStageRoot, files); removed.HasError())
                    return Result<SelectedZipStagingResult>::Failure(removed.ErrorValue());
            }
            if (!SafeToFallback(request, packageWasAbsent, checkpointWasAbsent))
                return invalid();
        }
        auto ready = PrepareZipUpdateStageHttps({full, request.fullPaths, request.stageRoot, request.downloadLimits, request.archiveLimits,
                                                 request.policy},
                                                files, verifier, cancellation, progress);
        if (ready.HasError())
            return Result<SelectedZipStagingResult>::Failure(ready.ErrorValue());
        auto checkpoint = LoadUpdateTransferCheckpoint(request.fullPaths.partialFile, request.fullPaths.checkpointFile);
        if (checkpoint.HasError() || !checkpoint.Value())
            return invalid();
        auto loaded = std::move(checkpoint).Value();
        return Result<SelectedZipStagingResult>::Success({std::move(*loaded), std::move(ready).Value(), false});
    }
}  // namespace Horo::Release
