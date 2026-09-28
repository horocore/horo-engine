#include "Horo/Release/UpdateZipStagingJob.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <system_error>
#include <utility>

namespace Horo::Release {
    /** @copydoc PrepareZipUpdateStageHttps */
    Result<std::filesystem::path> PrepareZipUpdateStageHttps(const UpdateZipStagingRequest &request, NativeDurableFileSystem &files,
                                                             const Security::ArtifactVerifier &verifier, CancellationToken cancellation,
                                                             const UpdateDownloadProgress &progress) {
        const auto &[package, paths, stageRoot, downloadLimits, archiveLimits, policy] = request;
        const auto invalid = [] {
            return Result<std::filesystem::path>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        };
        if (package.selection.format != DistributionPackageFormat::ZipArchive || !paths.partialFile.is_absolute() ||
            !paths.checkpointFile.is_absolute() || !stageRoot.is_absolute() ||
            paths.partialFile.parent_path() != paths.checkpointFile.parent_path() ||
            paths.partialFile.parent_path() != stageRoot.parent_path() || paths.partialFile == paths.checkpointFile ||
            paths.partialFile == stageRoot || paths.checkpointFile == stageRoot)
            return invalid();
        for (const auto &path : {paths.partialFile, paths.checkpointFile, stageRoot}) {
            if (path.filename().empty())
                return invalid();
            for (const auto &component : path) {
                if (component == "." || component == "..")
                    return invalid();
            }
        }
        auto ready = stageRoot;
        ready += ".ready";
        auto prepared = ready;
        prepared += ".prepared";
        if (paths.partialFile == ready || paths.partialFile == prepared || paths.checkpointFile == ready ||
            paths.checkpointFile == prepared)
            return invalid();
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(stageRoot.parent_path(), error)) || error)
            return invalid();
        if (const auto stageStatus = std::filesystem::symlink_status(stageRoot, error);
            stageStatus.type() != std::filesystem::file_type::not_found || (error && error != std::errc::no_such_file_or_directory))
            return invalid();

        auto checkpoint = DownloadUpdatePackageHttps({package, paths, downloadLimits, policy}, files, verifier, cancellation, progress);
        if (checkpoint.HasError())
            return Result<std::filesystem::path>::Failure(checkpoint.ErrorValue());
        return StageVerifiedZipUpdate({package, checkpoint.Value(), paths.partialFile, stageRoot, archiveLimits}, files, verifier,
                                      cancellation);
    }
}  // namespace Horo::Release
