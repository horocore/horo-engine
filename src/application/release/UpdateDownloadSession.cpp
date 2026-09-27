#include "Horo/Release/UpdateDownloadSession.h"

#include "Horo/Release/UpdateTransferCheckpointStore.h"
#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <system_error>
#include <utility>

namespace Horo::Release {
    namespace {
        /** @brief Rejects lexical traversal through a host-owned private path. */
        [[nodiscard]] bool NoTraversal(const std::filesystem::path &path) {
            return std::ranges::none_of(path, [](const auto &part) {
                return part == "." || part == "..";
            });
        }

        /** @brief Requires two distinct absolute files directly below one existing private directory. */
        [[nodiscard]] bool ValidPaths(const UpdateDownloadPaths &paths) {
            if (!paths.partialFile.is_absolute() || !paths.checkpointFile.is_absolute() || !NoTraversal(paths.partialFile) ||
                !NoTraversal(paths.checkpointFile) || paths.partialFile == paths.checkpointFile ||
                paths.partialFile.parent_path() != paths.checkpointFile.parent_path() || paths.partialFile.filename().empty() ||
                paths.checkpointFile.filename().empty() || paths.partialFile.filename() == "." || paths.partialFile.filename() == ".." ||
                paths.checkpointFile.filename() == "." || paths.checkpointFile.filename() == "..")
                return false;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(paths.partialFile.parent_path(), error);
            return !error && std::filesystem::is_directory(status);
        }

        /** @brief Retains the private filesystem reserve while accounting for already durable bytes. */
        [[nodiscard]] bool FitsRemainingSpace(const UpdatePackageRecord &package, const UpdateDownloadLimits &limits,
                                              const std::uint64_t durableBytes, const std::uint64_t availableBytes) {
            return package.size > 0U && package.size <= limits.maximumPackageBytes && durableBytes < package.size &&
                   limits.reserveBytes <= availableBytes && package.size - durableBytes <= availableBytes - limits.reserveBytes;
        }
    }  // namespace

    UpdateDownloadSession::UpdateDownloadSession(UpdatePackageRecord package, UpdateTransferPlan plan, UpdateDownloadPaths paths,
                                                 NativeDurableFileSystem &files, CancellationToken cancellation)
        : package_(std::move(package)), plan_(std::move(plan)), paths_(std::move(paths)), files_(&files),
          cancellation_(std::move(cancellation)) {}

    /** @copydoc UpdateDownloadSession::Begin */
    Result<UpdateDownloadSession> UpdateDownloadSession::Begin(const UpdatePackageRecord &package, const UpdateTransferResponse &response,
                                                               UpdateDownloadPaths paths, const UpdateDownloadLimits &limits,
                                                               NativeDurableFileSystem &files, CancellationToken cancellation) {
        if (cancellation.IsCancellationRequested())
            return Result<UpdateDownloadSession>::Failure(MakeError(UpdateTransferErrors::Cancelled));
        if (!ValidPaths(paths))
            return Result<UpdateDownloadSession>::Failure(MakeError(UpdateTransferErrors::InvalidCheckpoint));
        auto loaded = LoadUpdateTransferCheckpoint(paths.partialFile, paths.checkpointFile);
        if (loaded.HasError())
            return Result<UpdateDownloadSession>::Failure(loaded.ErrorValue());
        auto plan = PlanUpdateTransfer(package, response, loaded.Value());
        if (plan.HasError())
            return Result<UpdateDownloadSession>::Failure(plan.ErrorValue());
        auto available = files.AvailableBytes(paths.partialFile.parent_path());
        if (available.HasError())
            return Result<UpdateDownloadSession>::Failure(available.ErrorValue());
        if (!FitsRemainingSpace(package, limits, plan.Value().writeOffset, available.Value()))
            return Result<UpdateDownloadSession>::Failure(MakeError(UpdateTransferErrors::InsufficientSpace));
        return Result<UpdateDownloadSession>::Success(
            UpdateDownloadSession{package, std::move(plan).Value(), std::move(paths), files, std::move(cancellation)});
    }

    /** @copydoc UpdateDownloadSession::Append */
    Result<UpdateTransferCheckpoint> UpdateDownloadSession::Append(const std::span<const std::byte> bytes) {
        const auto invalid = [] {
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        };
        if (failed_ || finished_ || bytes.empty() || receivedBytes_ > plan_.responseBytes ||
            bytes.size() > plan_.responseBytes - receivedBytes_) {
            failed_ = true;
            return invalid();
        }
        if (cancellation_.IsCancellationRequested()) {
            failed_ = true;
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::Cancelled));
        }
        const auto nextBytes = receivedBytes_ + bytes.size();
        auto checkpoint = AdvanceUpdateTransfer(plan_, nextBytes);
        if (checkpoint.HasError()) {
            failed_ = true;
            return Result<UpdateTransferCheckpoint>::Failure(checkpoint.ErrorValue());
        }
        if (auto written = files_->AppendPrivateDurable(paths_.partialFile, plan_.writeOffset + receivedBytes_, bytes);
            written.HasError()) {
            failed_ = true;
            return Result<UpdateTransferCheckpoint>::Failure(written.ErrorValue());
        }
        if (auto saved = SaveUpdateTransferCheckpoint(*files_, paths_.partialFile, paths_.checkpointFile, checkpoint.Value());
            saved.HasError()) {
            failed_ = true;
            return Result<UpdateTransferCheckpoint>::Failure(saved.ErrorValue());
        }
        receivedBytes_ = nextBytes;
        return checkpoint;
    }

    /** @copydoc UpdateDownloadSession::Finish */
    Result<UpdateTransferCheckpoint> UpdateDownloadSession::Finish(const Security::ArtifactVerifier &verifier) {
        if (failed_ || finished_ || receivedBytes_ != plan_.responseBytes)
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::InvalidResponse));
        if (cancellation_.IsCancellationRequested()) {
            failed_ = true;
            return Result<UpdateTransferCheckpoint>::Failure(MakeError(UpdateTransferErrors::Cancelled));
        }
        auto checkpoint = AdvanceUpdateTransfer(plan_, receivedBytes_);
        if (checkpoint.HasError()) {
            failed_ = true;
            return Result<UpdateTransferCheckpoint>::Failure(checkpoint.ErrorValue());
        }
        if (checkpoint.Value().durableBytes == package_.size) {
            auto verified = VerifyCompletedUpdateTransfer(package_, checkpoint.Value(), paths_.partialFile, verifier);
            if (verified.HasError()) {
                failed_ = true;
                return Result<UpdateTransferCheckpoint>::Failure(verified.ErrorValue());
            }
        }
        finished_ = true;
        return checkpoint;
    }
}  // namespace Horo::Release
