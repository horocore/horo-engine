#include "Horo/Release/UpdateArchiveIndex.h"

#include "Horo/Release/ReleaseArtifactManifest.h"
#include "Horo/Release/UpdateTransferErrors.h"

#include <algorithm>
#include <map>
#include <string>

namespace Horo::Release {
    namespace {
        /** @brief Rejects a file used as a parent and any duplicate or case-colliding identity. */
        [[nodiscard]] bool InsertPath(std::map<std::string, UpdateArchiveEntryKind, std::less<>> &paths, const std::string &collisionKey,
                                      const UpdateArchiveEntryKind kind) {
            if (paths.contains(collisionKey))
                return false;
            std::size_t separator = collisionKey.find('/');
            while (separator != std::string::npos) {
                if (const auto parent = paths.find(collisionKey.substr(0U, separator));
                    parent != paths.end() && parent->second != UpdateArchiveEntryKind::Directory)
                    return false;
                separator = collisionKey.find('/', separator + 1U);
            }
            if (kind == UpdateArchiveEntryKind::File) {
                const auto child = paths.lower_bound(collisionKey + '/');
                if (child != paths.end() && child->first.starts_with(collisionKey + '/'))
                    return false;
            }
            paths.try_emplace(collisionKey, kind);
            return true;
        }
    }  // namespace

    /** @copydoc ValidateUpdateArchiveIndex */
    Result<void> ValidateUpdateArchiveIndex(const std::span<const UpdateArchiveEntry> entries, const UpdateArchiveLimits &limits) {
        if (limits.maximumEntries == 0U || limits.maximumFileBytes == 0U || limits.maximumExpandedBytes == 0U || entries.empty() ||
            entries.size() > limits.maximumEntries)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));

        std::map<std::string, UpdateArchiveEntryKind, std::less<>> paths;
        std::uint64_t expandedBytes = 0U;
        bool hasFile = false;
        for (const auto &entry : entries) {
            if (entry.kind != UpdateArchiveEntryKind::File && entry.kind != UpdateArchiveEntryKind::Directory)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            if (!IsValidReleaseArtifactPath(entry.path))
                return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            std::string collisionKey = entry.path;
            std::ranges::transform(collisionKey, collisionKey.begin(), [](const char character) {
                return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
            });
            if (!InsertPath(paths, collisionKey, entry.kind))
                return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
            if (entry.kind == UpdateArchiveEntryKind::Directory) {
                if (entry.expandedBytes != 0U)
                    return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
                continue;
            }
            hasFile = true;
            if (entry.expandedBytes > limits.maximumFileBytes || entry.expandedBytes > limits.maximumExpandedBytes - expandedBytes)
                return Result<void>::Failure(MakeError(UpdateTransferErrors::ArchiveResourceLimit));
            expandedBytes += entry.expandedBytes;
        }
        if (!hasFile)
            return Result<void>::Failure(MakeError(UpdateTransferErrors::InvalidArchive));
        return Result<void>::Success();
    }
}  // namespace Horo::Release
