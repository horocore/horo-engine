#pragma once

#include "Horo/Foundation/Platform.h"
#include "ImmutableFileNative.h"
#include "PublicationLockState.h"

namespace Horo::Platform::PublicationFileNative {
    using ProjectReadNative::Handle;

    /** @brief Returns no-follow metadata relative to a retained directory; only absence yields nullopt. */
    [[nodiscard]] Result<std::optional<FileEntryInfo>> Inspect(const Handle &parent, std::string_view name);
    /** @brief Completes a bounded immediate inventory using an independent retained cursor. */
    [[nodiscard]] Result<std::vector<FileDirectoryEntry>> List(const Handle &directory, std::size_t maximumEntries,
                                                               const CancellationToken &cancellation);
    /** @brief Exclusively creates a regular private leaf and sets created immediately after native creation. */
    [[nodiscard]] Result<Handle> Create(const Handle &parent, std::string_view name, bool &created, std::filesystem::perms permissions);
    /** @brief Creates one directory without granting traversal or changing an existing object. */
    [[nodiscard]] Result<bool> CreateDirectory(const Handle &parent, std::string_view name);
    /** @brief Returns storage capacity from a retained native object rather than its former spelling. */
    [[nodiscard]] Result<std::uint64_t> Available(const Handle &directory);
    /** @brief Applies the platform's strongest supported file-data flush before directory durability. */
    [[nodiscard]] Result<void> FlushFile(const Handle &file);
    /** @brief Acquires native writer exclusion and transfers its handle, exact path and parent lifetime to a lease state. */
    [[nodiscard]] Result<std::unique_ptr<ExclusiveFileLock::State>> Lock(Handle parent, std::string name, std::filesystem::path logicalPath,
                                                                         std::string_view ownerMetadata);
    /** @brief Moves a no-follow regular file/directory between retained parents; records successful native commit before sync. */
    [[nodiscard]] Result<void> Replace(const Handle &sourceParent, std::string_view source, const Handle &destinationParent,
                                       std::string_view destination, AtomicFileReplacementReceipt &receipt);
    /** @brief Removes one admitted regular file or empty directory; never recurses or follows a redirect. */
    [[nodiscard]] Result<void> Remove(const Handle &parent, std::string_view name);
}  // namespace Horo::Platform::PublicationFileNative
