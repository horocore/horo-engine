#pragma once

/** @file NativeProjectIoOwners.h
 * @brief Explicit native composition of project I/O capabilities sharing one retained root identity.
 */
#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Platform/ProjectReadFiles.h"

namespace Horo::Platform {
    /** @brief Independently owned capabilities derived from the same admitted native project directory.
     * @details Construction creates no output and grants no trust, generation activation or executable-package authority.
     * Each owner retains its native identity independently; callers may release or retain them separately.
     */
    struct NativeProjectIoOwners final {
        std::shared_ptr<const IProjectReadFiles> source;
        std::shared_ptr<const ImmutableFileStorage> cache;
        std::shared_ptr<DurableFileSystem> publication;
    };

    /** @brief Opens one project root once and derives source, immutable-cache and durable-publication owners from it.
     * @param authorizedRoot Existing absolute normalized directory admitted without following ancestor redirects.
     * @param projectIdentity Exact nonempty UTF-8 authority identity, at most 256 bytes.
     * @param projectGeneration Nonzero authority generation, checked by source queries.
     * @param cacheNamespace Fixed portable relative derived-cache namespace.
     * @param publicationNamespace Fixed portable relative publication namespace, lexically disjoint from cacheNamespace.
     * @return Complete owned capability set or a typed failure, without creating output or reopening the root pathname.
     * @details This load/tooling operation is synchronous. Hosts own trust, cancellation, compatibility and source-freshness
     * fences; path inspection before this call is not proof of the retained project's metadata. Publication calls use logical
     * absolute paths under the authorized spelling, while all native operations stay on the originally admitted directory.
     */
    [[nodiscard]] Result<NativeProjectIoOwners> CreateNativeProjectIoOwners(const std::filesystem::path &authorizedRoot,
                                                                            std::string projectIdentity, std::uint64_t projectGeneration,
                                                                            std::string_view cacheNamespace,
                                                                            std::string_view publicationNamespace);
}  // namespace Horo::Platform
