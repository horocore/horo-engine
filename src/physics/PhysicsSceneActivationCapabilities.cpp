#include "Horo/Physics/PhysicsErrors.h"
#include "PhysicsSceneActivationInternal.h"

#include <algorithm>

namespace Horo::Physics::Detail {
    namespace {
        [[nodiscard]] Result<void> RequireSceneCapability(const PhysicsRuntime &runtime, const PhysicsCapability capability) {
            if (runtime.Capability(capability) == PhysicsCapabilitySupport::Available)
                return Result<void>::Success();
            return Result<void>::Failure(
                MakeError(PhysicsErrors::CapabilityUnavailable, "The selected Physics composition lacks a required scene capability."));
        }

        [[nodiscard]] bool DefinitionHasActiveBodies(const Runtime::RuntimeSceneDefinition &definition) noexcept {
            return std::ranges::any_of(definition.Entities(), [](const Runtime::RuntimeEntityDefinition &entity) {
                return entity.components.rigidBody.has_value() && entity.components.rigidBody->enabled;
            });
        }

        [[nodiscard]] bool DefinitionHasActiveConstraints(const Runtime::RuntimeSceneDefinition &definition) noexcept {
            return std::ranges::any_of(definition.Entities(), [](const Runtime::RuntimeEntityDefinition &entity) {
                return std::ranges::any_of(entity.components.physicsConstraints, [](const Runtime::PhysicsConstraintComponent &constraint) {
                    return constraint.enabled;
                });
            });
        }
    }  // namespace

    /** @copydoc RequirePhysicsSceneCapabilities */
    Result<void> RequirePhysicsSceneCapabilities(const PhysicsRuntime &runtime, const Runtime::RuntimeSceneDefinition &definition) {
        using enum PhysicsCapability;
        if (DefinitionHasActiveBodies(definition)) {
            if (const Result<void> shapes = RequireSceneCapability(runtime, ImmutableShapes); shapes.HasError())
                return shapes;
            if (const Result<void> bodies = RequireSceneCapability(runtime, RigidBodies); bodies.HasError())
                return bodies;
        }
        if (DefinitionHasActiveConstraints(definition))
            return RequireSceneCapability(runtime, Constraints);
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
