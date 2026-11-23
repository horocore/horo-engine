#include "CanonicalSolver.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>

#if defined(JPH_DOUBLE_PRECISION) || defined(JPH_USE_DX12) || defined(JPH_USE_VK) || defined(JPH_USE_MTL) || defined(JPH_USE_CPU_COMPUTE)
#error "CanonicalV1 requires float CPU-only Physics without native compute backends"
#endif

static_assert(JPH_OBJECT_LAYER_BITS == 16);

namespace Horo::Physics::Detail {
    /** @copydoc IsCanonicalSolverBuildCompatible */
    bool IsCanonicalSolverBuildCompatible() noexcept {
        return JPH::VerifyJoltVersionID();
    }
}  // namespace Horo::Physics::Detail
