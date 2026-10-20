#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/Platform.h"

namespace Horo {

    /** @copydoc DurableFileSystem::ReadFileBounded */
    Result<std::vector<std::uint8_t>> DurableFileSystem::ReadFileBounded(const std::filesystem::path &, std::size_t,
                                                                         const CancellationToken &) const {
        return Result<std::vector<std::uint8_t>>::Failure(MakeError(FileStorageErrors::IoFailed, "File reading is not granted."));
    }

    /** @copydoc DurableFileSystem::InspectEntry */
    Result<std::optional<FileEntryInfo>> DurableFileSystem::InspectEntry(const std::filesystem::path &) const {
        return Result<std::optional<FileEntryInfo>>::Failure(MakeError(FileStorageErrors::IoFailed, "File inspection is not granted."));
    }

    /** @copydoc DurableFileSystem::ListDirectory */
    Result<std::vector<FileDirectoryEntry>> DurableFileSystem::ListDirectory(const std::filesystem::path &, std::size_t,
                                                                             const CancellationToken &) const {
        return Result<std::vector<FileDirectoryEntry>>::Failure(
            MakeError(FileStorageErrors::IoFailed, "Directory listing is not granted."));
    }

}  // namespace Horo
