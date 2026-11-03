#include "CanonicalPhysicsStateBackendInternal.h"
#include "PhysicsStateHashInternal.h"

namespace Horo::Physics::Detail {
    /** @brief STATE-only semantic identity. Existing live SurfaceMaterial fingerprint and Matches are deliberately unchanged. */
    Sha256Digest CanonicalPhysicsStateSurfaceFingerprint(const PhysicsSurfaceWorldBinding &binding) noexcept {
        StateDetail::StateHash hash{"Horo.Physics.State.Surface.v2"};
        hash.Word(static_cast<bool>(binding.catalog));
        if (binding.catalog) {
            hash.Word(binding.catalog->Revision());
            hash.Digest(binding.catalog->Fingerprint());
        }
        hash.Word(binding.defaultSurface.has_value());
        if (binding.defaultSurface)
            hash.Id(*binding.defaultSurface);
        return hash.Finish();
    }
}  // namespace Horo::Physics::Detail
