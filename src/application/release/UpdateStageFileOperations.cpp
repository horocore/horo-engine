#include "UpdateStageFileOperations.h"

#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <system_error>
#include <vector>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Horo::Release::Detail {
    /** @copydoc ValidStagePaths */
    bool ValidStagePaths(const std::filesystem::path &packageFile, const std::filesystem::path &stageRoot) {
        if (!packageFile.is_absolute() || !stageRoot.is_absolute() || packageFile.parent_path() != stageRoot.parent_path() ||
            packageFile == stageRoot || packageFile.filename().empty() || stageRoot.filename().empty())
            return false;
        for (const auto &path : {packageFile, stageRoot}) {
            for (const auto &part : path) {
                if (part == "." || part == "..")
                    return false;
            }
        }
        std::error_code error;
        if (!std::filesystem::is_directory(std::filesystem::symlink_status(stageRoot.parent_path(), error)) || error)
            return false;
        const auto stage = std::filesystem::symlink_status(stageRoot, error);
        return stage.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory);
    }

    /** @copydoc ValidStageDownloadPaths */
    bool ValidStageDownloadPaths(const std::filesystem::path &packageFile, const std::filesystem::path &checkpointFile,
                                 const std::filesystem::path &stageRoot) {
        if (!ValidStagePaths(packageFile, stageRoot) || !ValidStagePaths(checkpointFile, stageRoot) || packageFile == checkpointFile)
            return false;
        auto ready = stageRoot;
        ready += ".ready";
        auto prepared = ready;
        prepared += ".prepared";
        return packageFile != ready && packageFile != prepared && checkpointFile != ready && checkpointFile != prepared;
    }

    /** @copydoc SyncStageDirectories */
    Result<void> SyncStageDirectories(const std::filesystem::path &root, NativeDurableFileSystem &files) {
        std::error_code error;
        std::vector<std::filesystem::path> directories{root};
        std::filesystem::recursive_directory_iterator entry(root, std::filesystem::directory_options::none, error);
        const std::filesystem::recursive_directory_iterator end;
        while (entry != end && !error) {
            if (entry->is_directory(error) && !error)
                directories.emplace_back(entry->path());
            if (!error)
                entry.increment(error);
        }
        if (error)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        std::ranges::sort(directories, [](const auto &left, const auto &right) {
            return left.native().size() > right.native().size();
        });
        for (const auto &directory : directories) {
            if (auto synced = files.SyncDirectory(directory); synced.HasError())
                return synced;
        }
        return files.SyncDirectory(root.parent_path());
    }

    /** @copydoc ApplyAuthenticatedFileMode */
    Result<void> ApplyAuthenticatedFileMode(const std::filesystem::path &path, const UpdateFileMode mode) {
        if (mode != UpdateFileMode::Regular && mode != UpdateFileMode::Executable)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
#if defined(_WIN32)
        static_cast<void>(path);
        return Result<void>::Success();
#else
        const int descriptor = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (descriptor < 0)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));

        struct DescriptorGuard final {
            int value;
            DescriptorGuard(const DescriptorGuard &) = delete;
            DescriptorGuard &operator=(const DescriptorGuard &) = delete;
            DescriptorGuard(DescriptorGuard &&) = delete;
            DescriptorGuard &operator=(DescriptorGuard &&) = delete;

            explicit DescriptorGuard(const int descriptorValue) : value(descriptorValue) {}

            ~DescriptorGuard() {
                close(value);
            }
        };

        DescriptorGuard guard{descriptor};
        if (struct stat metadata{}; fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_nlink != 1 ||
                                    fchmod(descriptor, mode == UpdateFileMode::Executable ? 0755 : 0644) != 0 || fsync(descriptor) != 0)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::StageMismatch));
        return Result<void>::Success();
#endif
    }
}  // namespace Horo::Release::Detail
