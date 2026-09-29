#include "Horo/Release/UpdateTarGzipStagingJob.h"

#include "Horo/Release/UpdateTransferErrors.h"
#include "UpdateStageFileOperations.h"

namespace Horo::Release {
    /** @copydoc PrepareTarGzipUpdateStageHttps */
    Result<std::filesystem::path> PrepareTarGzipUpdateStageHttps(const UpdateTarGzipStagingRequest &request, NativeDurableFileSystem &files,
                                                                 const Security::ArtifactVerifier &verifier,
                                                                 const CancellationToken cancellation,
                                                                 const UpdateDownloadProgress &progress) {
        const auto &[package, paths, stageRoot, downloadLimits, archiveLimits, policy] = request;
        if (package.selection.format != DistributionPackageFormat::TarGzip ||
            !Detail::ValidStageDownloadPaths(paths.partialFile, paths.checkpointFile, stageRoot))
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::StageMismatch));

        auto checkpoint = DownloadUpdatePackageHttps({package, paths, downloadLimits, policy}, files, verifier, cancellation, progress);
        if (checkpoint.HasError())
            return Result<std::filesystem::path>::Failure(checkpoint.ErrorValue());
        return StageVerifiedTarGzipUpdate({package, checkpoint.Value(), paths.partialFile, stageRoot, archiveLimits}, files, verifier,
                                          cancellation);
    }
}  // namespace Horo::Release
