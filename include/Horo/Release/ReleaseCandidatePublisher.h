#pragma once

/**
 * @file ReleaseCandidatePublisher.h
 * @brief Private candidate staging and atomic no-replace promotion boundary.
 */

#include "Horo/Foundation/Platform.h"
#include "Horo/Release/ReleaseArtifactManifest.h"

#include <filesystem>

namespace Horo::Release {
    /** @brief One private stage and its reserved final destination on the same output volume. */
    class ReleaseStagingArea final {
    public:
        /** @brief Returns the private directory owned by the candidate. @return Staging path. */
        [[nodiscard]] const std::filesystem::path &StageRoot() const noexcept;
        /** @brief Returns the final versioned path. @return Destination path. */
        [[nodiscard]] const std::filesystem::path &FinalRoot() const noexcept;
        /** @brief Returns the owner identity. @return Candidate ID. */
        [[nodiscard]] ReleaseCandidateId Candidate() const noexcept;

    private:
        friend class NativeReleaseCandidatePublisher;
        ReleaseStagingArea(std::filesystem::path stageRoot, std::filesystem::path finalRoot, ReleaseCandidateId candidate);

        std::filesystem::path stageRoot_;
        std::filesystem::path finalRoot_;
        ReleaseCandidateId candidate_;
    };

    /** @brief Native filesystem adapter for explicit candidate staging and collision-safe promotion. */
    class NativeReleaseCandidatePublisher final {
    public:
        /** @brief Uses a host-owned durable filesystem; the caller retains its lifetime. @param files Host filesystem boundary. */
        explicit NativeReleaseCandidatePublisher(DurableFileSystem &files) noexcept;

        /**
         * @brief Creates a private stage for one preflighted candidate without touching the final path.
         * @param plan Frozen release plan with canonical output root and target identity.
         * @param candidate Nonzero candidate identity.
         * @return New stage, or an explicit collision/I/O failure; existing stages are preserved for recovery.
         */
        [[nodiscard]] Result<ReleaseStagingArea> Begin(const ReleaseExecutionPlan &plan, ReleaseCandidateId candidate) const;

        /**
         * @brief Writes final metadata, verifies the complete quiescent tree, and atomically promotes without replacement.
         * @param plan The same frozen plan that created @p stage.
         * @param stage Private stage returned by Begin, after all byte-changing workers have stopped.
         * @param manifest Final post-sign manifest for the exact candidate and plan identities.
         * @return Success after promotion or an idempotent retry that verifies the same final candidate;
         *         a conflicting or changed final candidate is never replaced.
         */
        [[nodiscard]] Result<void> Promote(const ReleaseExecutionPlan &plan, const ReleaseStagingArea &stage,
                                           const ReleaseArtifactManifest &manifest);

    private:
        DurableFileSystem &files_;
    };
}  // namespace Horo::Release
