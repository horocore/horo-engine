#pragma once
#include "CanonicalPhysicsRuntimeInternal.h"
#include "Horo/Foundation/CancellationToken.h"
#include "PhysicsStateConstructionStorage.h"

namespace Horo::Physics::Detail {
    /** @brief Cold detached shape construction; no Horo/native publication until exact roles are admitted by the receiving owner.
     * Temporary native refs are private and destroyed on that same owner lane. Recipe storage is the SAME Runtime pool.
     */
    struct CanonicalRestartShapes final {
        std::vector<JPH::Ref<JPH::Shape>> native;
        std::vector<std::size_t> requestedBytes;
        std::vector<StateDetail::ConstructionRecipeLease> recipes;
        std::vector<Sha256Digest> digests;
        [[nodiscard]] std::size_t HoroPayloadBytes() const noexcept;
    };

    [[nodiscard]] Result<CanonicalRestartShapes> PrepareCanonicalRestartShapes(CanonicalRuntime &, const PhysicsStateShapeRecipes &,
                                                                               const CancellationToken &);
}  // namespace Horo::Physics::Detail
