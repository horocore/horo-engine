#pragma once

#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    /** @brief One actual retained shape identity compiled from resident records and
     * body geometry references, never a serialized native pointer. */
    struct CanonicalPhysicsStateGeometry final {
        ShapeHandle handle;
        JPH::RefConst<JPH::Shape> shape;
        Sha256Digest recipe;
        std::size_t nativeRequestedBytes{};
    };

    /** @brief Cold complete handle-sorted inventory; duplicate references must
     * prove the same actual immutable geometry. */
    [[nodiscard]] Result<std::vector<CanonicalPhysicsStateGeometry>> CollectCanonicalPhysicsStateGeometry(const CanonicalWorld &world,
                                                                                                          std::size_t maximumPayloadBytes);
    /** @brief One reservation per unique Horo geometry identity; retained child
     * DAGs may conservatively overlap. */
    [[nodiscard]] Result<std::size_t> CanonicalPhysicsStateGeometryReservation(std::span<const CanonicalPhysicsStateGeometry> geometry);
}  // namespace Horo::Physics::Detail
