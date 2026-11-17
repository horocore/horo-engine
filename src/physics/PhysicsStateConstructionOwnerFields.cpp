#include "PhysicsStateConstructionManifestCodec.h"

namespace Horo::Physics::StateDetail {
    /** @copydoc ConstructionOwnerFields */
    void ConstructionOwnerFields(Wire &wire, PhysicsStatePersistentOwner &owner) noexcept {
        for (auto &byte : owner.entity)
            wire.Integer(byte);
        wire.Integer(owner.generation);
        auto role = static_cast<std::uint8_t>(owner.role);
        wire.Integer(role);
        owner.role = static_cast<PhysicsStateOwnerRole>(role);
        wire.Integer(owner.slot);
        wire.Integer(owner.object);
    }
}  // namespace Horo::Physics::StateDetail
