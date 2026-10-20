#pragma once

/** @file ImmutableFileStorage.h
 * @brief Host-owned bounded immutable file storage independent of native paths and asset/cache policy.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/ModuleDescriptor.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Horo {
    /** @brief Stable storage failures without native paths or operating-system error text. */
    namespace FileStorageErrors {
        extern const ErrorCodeDescriptor InvalidPath;
        extern const ErrorCodeDescriptor UnsafeObject;
        extern const ErrorCodeDescriptor Capacity;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Conflict;
        extern const ErrorCodeDescriptor IoFailed;
    }  // namespace FileStorageErrors

    /** @brief Returns inert metadata for explicit host error registration.
     * @return Independently owned descriptor containing process-lifetime immutable error definitions.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor ImmutableFileStorageErrorDomain();

    /** @brief Root-identity capability for bounded immutable files in an explicitly granted namespace.
     * @details Hosts compose the implementation and retain it through joined work. Relative names use
     * portable UTF-8 components: no absolute paths, traversal, device names, redirects or special files.
     * Native implementations resolve each component against retained directory identities rather than
     * checking a path and reopening it later. The capability grants no authored-project mutation,
     * generation selection, enumeration or deletion API. Cache keys, envelope verification, resource
     * budgets and publication remain with their existing owners.
     *
     * Calls may run concurrently. Each read has an independent cursor, and immutable publication never
     * replaces existing bytes. Cancellation is cooperative between bounded native transfers; an OS
     * call already in progress cannot be forcibly interrupted. Callers must join calls before teardown.
     */
    class ImmutableFileStorage {
    public:
        static constexpr std::size_t MaximumEntryBytes = 1024U * 1024U * 1024U; /**< Hard per-entry ceiling; callers may reduce it. */

        virtual ~ImmutableFileStorage() = default;

        /** @brief Loads one regular single-link file through the retained namespace.
         * @param path Portable relative path, at most 4096 UTF-8 bytes.
         * @param maximumBytes Positive admission bound no greater than MaximumEntryBytes.
         * @param cancellation Borrowed cooperative stop token; not retained.
         * @return Complete owned bytes, nullopt only for an absent component, or a typed safety/I/O/stop failure.
         * @post No directories or files are created. Mutation observed during the read is a failure, never a partial hit.
         */
        [[nodiscard]] virtual Result<std::optional<std::vector<std::uint8_t>>> Load(std::string_view path, std::size_t maximumBytes,
                                                                                    const CancellationToken &cancellation) const = 0;

        /** @brief Publishes complete immutable bytes without replacing any existing entry.
         * @param path Portable relative path, at most 4096 UTF-8 bytes; missing parents may be created inside the retained namespace.
         * @param bytes Complete entry, including empty content, within MaximumEntryBytes.
         * @param cancellation Borrowed cooperative stop token; not retained.
         * @return Durable success for a new entry or identical existing bytes; Conflict for different existing bytes,
         * or typed safety/I/O/stop failure. No failure permits falling back to an ambient filesystem.
         * @details Private preparation and no-replace publication are atomic with respect to entry visibility.
         * Cancellation before commit publishes no entry. A post-commit durability error can leave a complete
         * immutable entry; retry verifies it normally. This is cache storage, not a generation commit receipt.
         */
        [[nodiscard]] virtual Result<void> Store(std::string_view path, std::span<const std::uint8_t> bytes,
                                                 const CancellationToken &cancellation) const = 0;
    };
}  // namespace Horo
