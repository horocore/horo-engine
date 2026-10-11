#pragma once

/** @file ProjectReadFiles.h
 * @brief Root-bound read-only filesystem capability, independent of application and protocol models.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Platform {
    /** @brief Stable bounded filesystem failures; no native path or operating-system exception payload. */
    namespace ProjectReadErrors {
        extern const ErrorCodeDescriptor Invalid;
        extern const ErrorCodeDescriptor UnsafePath;
        extern const ErrorCodeDescriptor ReadFailed;
        extern const ErrorCodeDescriptor Capacity;
        extern const ErrorCodeDescriptor Stale;
        extern const ErrorCodeDescriptor Unavailable;
        extern const ErrorCodeDescriptor Cancelled;
        extern const ErrorCodeDescriptor Deadline;
    }  // namespace ProjectReadErrors

    /** @brief Finite read-only budgets; callers may reduce these hard limits. */
    struct ProjectReadLimits final {
        std::size_t maximumEntries{4096};
        std::size_t maximumFileBytes{1U << 20U};
        std::size_t maximumManifestBytes{1U << 20U};
        std::chrono::milliseconds maximumDuration{1000};
    };

    /** @brief Exact root-authority fence and borrowed synchronous stop view. */
    struct ProjectReadContext final {
        std::string projectIdentity;
        std::uint64_t projectGeneration{};
        CancellationToken cancellation;
        std::chrono::steady_clock::time_point deadline;
        std::function<bool()> authorityStopped; /**< Never retained by a query. */
    };

    /** @brief One contained, portable relative file spelling. */
    struct ProjectReadEntry final {
        std::string path;
    };

    /** @brief Owned bounded manifest; the revision identifies its exact sorted relative entries. */
    struct ProjectReadManifest final {
        std::string projectIdentity;
        std::uint64_t projectGeneration{};
        std::string revision;
        std::vector<ProjectReadEntry> entries;
    };

    /** @brief Owned captured bytes with an exact content revision; encoding validation belongs to the caller. */
    struct ProjectReadTextSnapshot final {
        std::string projectIdentity;
        std::uint64_t projectGeneration{};
        std::string path;
        std::string revision;
        std::shared_ptr<const std::string> text;
    };

    /** @brief Validates portable relative UTF-8 path segments without normalization or I/O.
     * @param path Candidate spelling. @param allowRoot Whether the empty root is allowed.
     * @return True for nontraversing, non-device portable paths.
     */
    [[nodiscard]] bool IsSafeProjectReadPath(std::string_view path, bool allowRoot = false);
    /** @brief Checks cancellation, live authority and the cooperative monotonic deadline.
     * @param context Per-call stop view. @return Success or typed stop failure.
     */
    [[nodiscard]] Result<void> CheckProjectReadContext(const ProjectReadContext &context);

    /** @brief Root-identity owner whose descendant reads reject links, reparses and special files before data access.
     * Every operation stays relative to retained native directory identities. Implementations perform
     * bounded enumeration/reads, retain no call context, publish no caches and mutate no filesystem state.
     * Host composition explicitly supplies native or restricted implementations; callers discover none.
     */
    class IProjectReadFiles {
    public:
        virtual ~IProjectReadFiles() = default;
        /** @brief Captures a bounded relative manifest rooted at an admitted directory prefix.
         * @param prefix Relative directory or empty root. @param context Authority/stop fence.
         * @param limits Finite policy. @return Owned manifest or typed failure.
         */
        [[nodiscard]] virtual Result<ProjectReadManifest> Files(std::string_view prefix, const ProjectReadContext &context,
                                                                const ProjectReadLimits &limits) const = 0;
        /** @brief Reads through retained directory identities, never a path-validated later reopen.
         * @param path Relative regular file. @param context Authority/stop fence.
         * @param limits Finite policy. @return Owned bytes/revision or typed failure.
         */
        [[nodiscard]] virtual Result<ProjectReadTextSnapshot> Text(std::string_view path, const ProjectReadContext &context,
                                                                   const ProjectReadLimits &limits) const = 0;
    };
}  // namespace Horo::Platform
