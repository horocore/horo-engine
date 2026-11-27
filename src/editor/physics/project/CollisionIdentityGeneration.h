#pragma once
#include "Horo/Physics/PhysicsCollisionSchema.h"

#include <array>

namespace Horo::Editor {
    /** @brief Creates fresh inert UUIDv4 bytes for an explicit authoring action; never reserves native identity.
     * @param existing Optional current project definitions used to reject cross-type reuse. @return New bytes or typed capacity failure.
     */
    [[nodiscard]] Result<std::array<std::uint8_t, 16>> AllocateCollisionIdentityBytes(const Physics::NormalizedCollisionSchema *existing);
}  // namespace Horo::Editor
