#pragma once
#include "Horo/Runtime/Save/SaveParticipantRegistry.h"

namespace Horo::Physics::Detail {
    /** @brief Archive record metadata for the SAME versioned Physics codec. These are schema constants, never live owner identities. */
    inline constexpr std::size_t MaximumPhysicsStateSaveRecordBytes = 8U << 20U;
    [[nodiscard]] Runtime::SaveParticipantId PhysicsStateSaveParticipant();
    [[nodiscard]] Runtime::SaveRecordId PhysicsStateSaveRecord();
    [[nodiscard]] Runtime::CanonicalStateParticipantDescriptor PhysicsStateSaveDescriptor();
}  // namespace Horo::Physics::Detail
