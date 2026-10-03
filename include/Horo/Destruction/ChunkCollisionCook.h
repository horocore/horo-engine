#pragma once

/** @file ChunkCollisionCook.h
 * @brief Exact DFR chunk projection into Physics-owned convex and compound artifacts.
 */

#include "Horo/Destruction/ChunkCollisionArtifact.h"
#include "Horo/Destruction/ChunkMeshCook.h"

namespace Horo::Destruction {
    /** @brief Explicit collision authority; render material assignments never select Physics slots. */
    struct ChunkCollisionMaterial final {
        DestructionChunkId chunk;
        Physics::PhysicsMaterialSlotId slot;
    };

    /** @brief Captured input generation, exact Physics profile and finite aggregate cook budget. */
    struct ChunkCollisionCookRequest final {
        FractureArtifactContentIdentity content;
        Sha256Digest meshIntegrityDigest;
        Physics::PhysicsShapeCookTargetDigest target;
        Physics::PhysicsConvexHullCookSettings convex;
        Physics::PhysicsCompoundCookLimits compound;
        DestructionLimits limits;
        std::span<const ChunkCollisionMaterial> materials;
    };

    /**
     * @brief Cooks exact neutral regions through public Physics contracts, never solver-native APIs.
     * @param mesh Sealed immutable mesh artifact retaining neutral collision regions.
     * @param request Exact captured content/profile, explicit material slots and finite limits.
     * @param cancellation Cooperative token checked during bounded validation and between Physics calls.
     * @return Complete immutable dependency closure or original typed failure, publishing nothing.
     * @pre Offline/Assets cook operation only. Callers serialize owner publication separately.
     */
    [[nodiscard]] Result<std::shared_ptr<const ChunkCollisionArtifactSet>> CookChunkCollision(const ChunkMeshArtifact &mesh,
                                                                                              const ChunkCollisionCookRequest &request,
                                                                                              const CancellationToken &cancellation = {});

    /** @brief Single-thread publication owner; old snapshots and Physics leases survive replacement/shutdown. */
    class ChunkCollisionCookOwner final {
    public:
        ChunkCollisionCookOwner() = default;

        /** @brief Cancels remaining detached tokens before the publication owner disappears. */
        ~ChunkCollisionCookOwner();

        ChunkCollisionCookOwner(const ChunkCollisionCookOwner &) = delete;
        ChunkCollisionCookOwner &operator=(const ChunkCollisionCookOwner &) = delete;

        /** @brief Current non-wrapping generation. @return Nonzero owner revision. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;

        /** @brief Token retained by detached work. @return Current generation cancellation. */
        [[nodiscard]] CancellationToken Token() const noexcept;

        /** @brief Last completely accepted dependency closure. @return Immutable snapshot or null. */
        [[nodiscard]] std::shared_ptr<const ChunkCollisionArtifactSet> Snapshot() const noexcept;

        /**
         * @brief Accepts a complete candidate only for the current content, mesh, target and owner generation.
         * @param candidate Detached immutable cook result.
         * @param expectedRevision Captured owner generation.
         * @param current Current upstream content/profile captured by the Assets owner.
         * @return Success or typed failure retaining the previous snapshot.
         */
        [[nodiscard]] Result<void> Accept(std::shared_ptr<const ChunkCollisionArtifactSet> candidate, std::uint64_t expectedRevision,
                                          const ChunkCollisionCookRequest &current);
        /** @brief Cancels old work and advances generation, retaining the last result. @return Success or typed failure. */
        [[nodiscard]] Result<void> Invalidate();
        /** @brief Idempotently closes admission and cancels pending work, retaining reader snapshots. */
        void Shutdown() noexcept;

    private:
        std::shared_ptr<const ChunkCollisionArtifactSet> current_;
        CancellationSource cancellation_;
        std::uint64_t revision_{1};
        bool shutdown_{};
    };
}  // namespace Horo::Destruction
