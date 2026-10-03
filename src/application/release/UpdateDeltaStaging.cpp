#include "Horo/Release/UpdateDeltaStaging.h"

#include "Horo/Release/UpdateTransferErrors.h"
#include "UpdateStageFileOperations.h"

#include <cstdint>
#include <system_error>
#include <utility>
#include <vector>

namespace Horo::Release {
    namespace {
        /** @brief Requires a stable absolute spelling for a host-owned staging root. */
        [[nodiscard]] bool NormalizedRoot(const std::filesystem::path &path) {
            return path.is_absolute() && !path.filename().empty() && path.lexically_normal() == path;
        }

        /** @brief Requires absolute normalized roots and prevents a stage from aliasing either source. */
        [[nodiscard]] bool ValidRoots(const std::filesystem::path &base, const std::filesystem::path &delta,
                                      const std::filesystem::path &stage) {
            if (!NormalizedRoot(base) || !NormalizedRoot(delta) || !NormalizedRoot(stage))
                return false;
            std::error_code error;
            const auto realBase = std::filesystem::canonical(base, error);
            if (error)
                return false;
            const auto realDelta = std::filesystem::canonical(delta, error);
            if (error)
                return false;
            const auto realStageParent = std::filesystem::canonical(stage.parent_path(), error);
            if (error)
                return false;
            if (const bool sameSources = std::filesystem::equivalent(realBase, realDelta, error); error || sameSources)
                return false;
            const auto aliasesStageParent = [&](const std::filesystem::path &source) {
                for (auto parent = realStageParent;; parent = parent.parent_path()) {
                    if (const bool aliases = std::filesystem::equivalent(source, parent, error); error || aliases)
                        return true;
                    if (parent == parent.root_path())
                        return false;
                }
            };
            return !aliasesStageParent(realBase) && !aliasesStageParent(realDelta);
        }

        /** @brief Removes only the previously absent destination created by this operation. */
        struct StageCleanup final {
            explicit StageCleanup(std::filesystem::path stageRoot) : root(std::move(stageRoot)) {}

            StageCleanup(const StageCleanup &) = delete;
            StageCleanup &operator=(const StageCleanup &) = delete;
            StageCleanup(StageCleanup &&) = delete;
            StageCleanup &operator=(StageCleanup &&) = delete;

            std::filesystem::path root;
            bool active{true};

            ~StageCleanup() {
                if (active) {
                    std::error_code ignored;
                    std::filesystem::remove_all(root, ignored);
                }
            }
        };

        /** @brief Preflights full target capacity while retaining the host's recovery reserve. */
        [[nodiscard]] Result<void> CheckCapacity(const UpdateFileDeltaPlan &plan, const std::filesystem::path &parent,
                                                 const UpdateArchiveLimits &limits, const NativeDurableFileSystem &files) {
            std::uint64_t total{};
            for (const auto &file : plan.targetFiles)
                total += file.target.size;
            auto available = files.AvailableBytes(parent);
            if (available.HasError())
                return Result<void>::Failure(available.ErrorValue());
            if (limits.reserveBytes > available.Value() || total > available.Value() - limits.reserveBytes)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::InsufficientSpace));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ReconstructUpdateFileDeltaStage */
    Result<void> ReconstructUpdateFileDeltaStage(const UpdateFileDeltaPlan &plan, const std::filesystem::path &baseRoot,
                                                 const std::filesystem::path &deltaRoot, const std::filesystem::path &stageRoot,
                                                 const UpdateArchiveLimits &limits, NativeDurableFileSystem &files,
                                                 CancellationToken cancellation) {
        const auto failed = [](const ErrorCodeDescriptor &code) {
            return Result<void>::Failure(MakeError(code));
        };
        if (!ValidRoots(baseRoot, deltaRoot, stageRoot))
            return failed(UpdateTransferErrors::StageMismatch);
        std::vector<UpdateStagedFile> target;
        target.reserve(plan.targetFiles.size());
        for (const auto &file : plan.targetFiles)
            target.push_back(file.target);
        auto canonical = PlanUpdateFileDelta(plan.baseFiles, target, plan.deltaFiles, plan.baseInventoryDigest, plan.targetInventoryDigest,
                                             plan.deltaInventoryDigest, limits);
        if (canonical.HasError())
            return Result<void>::Failure(canonical.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return failed(UpdateTransferErrors::Cancelled);
        if (auto verified = VerifyUpdateStagedTree(baseRoot, canonical.Value().baseFiles, limits); verified.HasError())
            return verified;
        if (auto verified = VerifyUpdateStagedTree(deltaRoot, canonical.Value().deltaFiles, limits); verified.HasError())
            return verified;
        if (auto capacity = CheckCapacity(canonical.Value(), stageRoot.parent_path(), limits, files); capacity.HasError())
            return capacity;
        if (std::error_code error; !std::filesystem::is_directory(std::filesystem::symlink_status(stageRoot.parent_path(), error)) || error)
            return failed(UpdateTransferErrors::StageMismatch);
        if (std::error_code error; !std::filesystem::create_directory(stageRoot, error) || error)
            return failed(UpdateTransferErrors::StageMismatch);
        StageCleanup cleanup{stageRoot};
        if (auto synced = files.SyncDirectory(stageRoot.parent_path()); synced.HasError())
            return synced;
        for (const auto &file : canonical.Value().targetFiles) {
            if (cancellation.IsCancellationRequested())
                return failed(UpdateTransferErrors::Cancelled);
            const auto &source = file.source == UpdateDeltaFileSource::VerifiedBase ? baseRoot : deltaRoot;
            if (auto copied = files.CopyDurable(source / file.target.path, stageRoot / file.target.path); copied.HasError())
                return copied;
        }
        if (cancellation.IsCancellationRequested())
            return failed(UpdateTransferErrors::Cancelled);
        if (auto verified = VerifyUpdateFileDeltaStage(stageRoot, canonical.Value(), limits); verified.HasError())
            return verified;
        if (auto synced = Detail::SyncStageDirectories(stageRoot, files); synced.HasError())
            return synced;
        cleanup.active = false;
        return Result<void>::Success();
    }
}  // namespace Horo::Release
