#include "Horo/Release/UpdateZipStagingJob.h"

#include "Horo/Release/UpdateTransferErrors.h"
#include "UpdateStageFileOperations.h"

#include <utility>

namespace Horo::Release {
    /** @copydoc PrepareZipUpdateStageHttps */
    Result<std::filesystem::path> PrepareZipUpdateStageHttps(const UpdateZipStagingRequest &request, NativeDurableFileSystem &files,
                                                             const Security::ArtifactVerifier &verifier, CancellationToken cancellation,
                                                             const UpdateDownloadProgress &progress) {
        const auto &[package, paths, stageRoot, downloadLimits, archiveLimits, policy] = request;
        if (package.selection.format != DistributionPackageFormat::ZipArchive ||
            !Detail::ValidStageDownloadPaths(paths.partialFile, paths.checkpointFile, stageRoot))
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::StageMismatch));

        auto checkpoint = DownloadUpdatePackageHttps({package, paths, downloadLimits, policy}, files, verifier, cancellation, progress);
        if (checkpoint.HasError())
            return Result<std::filesystem::path>::Failure(checkpoint.ErrorValue());
        return StageVerifiedZipUpdate({package, checkpoint.Value(), paths.partialFile, stageRoot, archiveLimits}, files, verifier,
                                      cancellation);
    }
}  // namespace Horo::Release
