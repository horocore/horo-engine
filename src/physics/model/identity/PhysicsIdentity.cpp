#include "Horo/Physics/PhysicsIdentity.h"

namespace Horo::Physics {
    /** @copydoc PhysicsWorldId::Create */
    Result<PhysicsWorldId> PhysicsWorldId::Create(const std::uint64_t value) {
        if (value == 0)
            return Result<PhysicsWorldId>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        return Result<PhysicsWorldId>::Success(PhysicsWorldId{value});
    }
}  // namespace Horo::Physics
