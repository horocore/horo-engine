#pragma once

#include "Horo/Physics/PhysicsCollisionSchema.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/SurfaceMaterial.h"

#include <limits>

namespace Horo::Physics::Detail {
    /** @brief Read-only same-domain accounting of actual immutable schema
     * capacities; no mutable or new publication authority. */
    struct PhysicsStateSchemaAccounting final {
        /** @brief Charges each immutable owner once per retained resource record;
         * shared records may conservatively reserve it again. */
        [[nodiscard]] static Result<std::size_t> Collision(const NormalizedCollisionSchema *schema);
        /** @brief Includes actual vector capacity and heap string payload, excluding
         * inline string bytes already in object sizeof. */
        [[nodiscard]] static Result<std::size_t> Surfaces(const SurfaceCatalogSnapshot *catalog);
    };
}  // namespace Horo::Physics::Detail
