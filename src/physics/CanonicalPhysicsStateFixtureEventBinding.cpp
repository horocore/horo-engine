#include "CanonicalPhysicsStateEventBindingInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        template <class T>
        bool Metadata(const T &actual, const PhysicsStateEventEndpoint &endpoint, const std::optional<PhysicsEventMaterial> &material,
                      const std::optional<PhysicsSurfaceEvidence> &surface) noexcept {
            return actual.layer == endpoint.layer && actual.profile == endpoint.profile && actual.surface == surface &&
                   SameStateMaterial(actual.material, material);
        }

        bool Contributor(const PhysicsQueryFixtureDescriptor &descriptor, const PhysicsStateEventEndpoint &endpoint,
                         const std::optional<PhysicsEventMaterial> &material,
                         const std::optional<PhysicsSurfaceEvidence> &surface) noexcept {
            if (endpoint.collider != 0)
                return false;
            if (const auto *compound = std::get_if<PhysicsCompoundShapeDescriptor>(&descriptor.shape)) {
                for (const auto &child : compound->children)
                    if (endpoint.subshape == child.subshape && Metadata(child, endpoint, material, surface))
                        return true;
                return false;
            }
            return descriptor.subshape == endpoint.subshape && Metadata(descriptor, endpoint, material, surface);
        }
    }  // namespace

    /** @copydoc BindStateFixtureEvent */
    Result<PhysicsEventEndpoint> BindStateFixtureEvent(const CanonicalWorld &world, const CanonicalQueryFixtureRecord &body,
                                                       const PhysicsStateEventEndpoint &endpoint,
                                                       const std::optional<PhysicsEventMaterial> &material,
                                                       const std::optional<PhysicsSurfaceEvidence> &surface) {
        if (!Contributor(body.descriptor, endpoint, material, surface))
            return Result<PhysicsEventEndpoint>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return Result<PhysicsEventEndpoint>::Success({body.fixture.body, body.fixture.shape, endpoint.subshape, endpoint.layer,
                                                      endpoint.profile, world.query.querySchemaGeneration, 0});
    }
}  // namespace Horo::Physics::Detail
