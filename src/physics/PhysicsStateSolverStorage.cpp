#include "PhysicsStateSolverStorage.h"

#include "PhysicsStateValidationInternal.h"

#include <limits>
#include <new>

namespace Horo::Physics::StateDetail {
    namespace {
        /** @brief Checks all cold payload products by subtraction without trusting a
         * caller's host size width. */
        bool Charge(std::size_t count, std::size_t size, std::size_t &remaining) noexcept {
            if (count > remaining / size)
                return false;
            remaining -= count * size;
            return true;
        }

        /** @brief No vector allocation begins until the complete minimum retained
         * payload fits. */
        bool CanPrepare(const PhysicsStateLimits &limits) noexcept {
            if (!ValidStateLimits(limits))
                return false;
            std::size_t bytes = limits.maximumBytes;
            if (!Charge(1, sizeof(SolverStorage), bytes))
                return false;
            return Charge(limits.maximumBodies, sizeof(PhysicsStateBody) + sizeof(PhysicsStateObjectId) + 1, bytes) &&
                   Charge(limits.maximumConstraints, sizeof(PhysicsStateConstraint), bytes) &&
                   Charge(limits.maximumPairs, sizeof(PhysicsStateBodyPair), bytes) &&
                   Charge(limits.maximumManifolds, sizeof(PhysicsStateManifold), bytes) &&
                   Charge(limits.maximumCcdManifolds, sizeof(PhysicsStateManifoldKey), bytes);
        }
    }  // namespace

    /** @copydoc SolverStorage::RetainedPayloadBytes */
    std::size_t SolverStorage::RetainedPayloadBytes() const noexcept {
        return sizeof(*this) + bodies.capacity() * sizeof(PhysicsStateBody) + activeBodyOrder.capacity() * sizeof(PhysicsStateObjectId) +
               constraints.capacity() * sizeof(PhysicsStateConstraint) + pairs.capacity() * sizeof(PhysicsStateBodyPair) +
               manifolds.capacity() * sizeof(PhysicsStateManifold) + ccdManifolds.capacity() * sizeof(PhysicsStateManifoldKey) +
               activeBodyScratch.capacity();
    }

    /** @copydoc PrepareSolverStorage */
    Result<std::unique_ptr<SolverStorage>> PrepareSolverStorage(const PhysicsStateLimits &limits) {
        if (!CanPrepare(limits))
            return Result<std::unique_ptr<SolverStorage>>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        try {
            auto storage = std::make_unique<SolverStorage>();
            storage->limits = limits;
            storage->bodies.resize(limits.maximumBodies);
            storage->activeBodyOrder.resize(limits.maximumBodies);
            storage->constraints.resize(limits.maximumConstraints);
            storage->pairs.resize(limits.maximumPairs);
            storage->manifolds.resize(limits.maximumManifolds);
            storage->ccdManifolds.resize(limits.maximumCcdManifolds);
            storage->activeBodyScratch.resize(limits.maximumBodies);
            if (storage->RetainedPayloadBytes() > limits.maximumBytes)
                return Result<std::unique_ptr<SolverStorage>>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Result<std::unique_ptr<SolverStorage>>::Success(std::move(storage));
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<SolverStorage>>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Physics::StateDetail
