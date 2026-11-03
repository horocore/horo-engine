#pragma once

#include "Horo/Physics/PhysicsStateSnapshot.h"

#include <memory>

namespace Horo::Physics::StateDetail {
    /** @brief One private unpublished codec/capture slot. Published immutable slots
     * are never passed as a decode destination. */
    struct SolverStorage final {
        PhysicsStateLimits limits;
        std::vector<PhysicsStateBody> bodies;
        std::vector<PhysicsStateObjectId> activeBodyOrder;
        std::vector<PhysicsStateConstraint> constraints;
        std::vector<PhysicsStateBodyPair> pairs;
        std::vector<PhysicsStateManifold> manifolds;
        std::vector<PhysicsStateManifoldKey> ccdManifolds;
        std::vector<std::uint8_t> activeBodyScratch;
        PhysicsStateSolverView used;

        /** @brief Captures actual vector payload capacities after cold allocation;
         * excludes allocator/control-block overhead. */
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;
    };

    /** @brief Cold slot allocation is transactional; typed budget failure occurs
     * before any native call. */
    [[nodiscard]] Result<std::unique_ptr<SolverStorage>> PrepareSolverStorage(const PhysicsStateLimits &limits);
    /** @brief Writes one validated complete solver section; failure before writes
     * preserves caller output. */
    [[nodiscard]] Result<std::size_t> EncodeSolverState(const PhysicsStateSolverView &state, const PhysicsStateLimits &limits,
                                                        std::span<std::uint8_t> scratch, std::span<std::byte> output);
    /** @brief Decodes into caller's private staging slot; full validation precedes
     * any possible native mutation. */
    [[nodiscard]] Result<PhysicsStateSolverView> DecodeSolverState(std::span<const std::byte> input, SolverStorage &staging);
}  // namespace Horo::Physics::StateDetail
