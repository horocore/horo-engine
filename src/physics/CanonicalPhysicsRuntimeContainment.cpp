#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    /** @copydoc FindCanonicalNonFiniteBody */
    std::optional<CanonicalNonFiniteBody> FindCanonicalNonFiniteBody(const CanonicalWorldHandle world) noexcept {
        if (world.value == nullptr)
            return std::nullopt;
        const auto &canonical = *static_cast<const CanonicalWorld *>(world.value);
        for (const CanonicalSceneBodyRecord &record : canonical.scene.bodies) {
            if (record.injectedStateForTesting.has_value() && !std::isfinite(*record.injectedStateForTesting))
                return CanonicalNonFiniteBody{record.handle, record.sceneEntity};
            JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), record.nativeBody);
            if (!lock.Succeeded())
                return CanonicalNonFiniteBody{record.handle, record.sceneEntity, false};
            const JPH::Body &body = lock.GetBody();
            const auto position = body.GetPosition();
            const auto rotation = body.GetRotation();
            const auto linear = body.GetLinearVelocity();
            const auto angular = body.GetAngularVelocity();
            const auto bounds = body.GetWorldSpaceBounds().GetSize();
            const bool finite = std::isfinite(position.GetX()) && std::isfinite(position.GetY()) && std::isfinite(position.GetZ()) &&
                                std::isfinite(rotation.GetX()) && std::isfinite(rotation.GetY()) && std::isfinite(rotation.GetZ()) &&
                                std::isfinite(rotation.GetW()) && std::isfinite(linear.GetX()) && std::isfinite(linear.GetY()) &&
                                std::isfinite(linear.GetZ()) && std::isfinite(angular.GetX()) && std::isfinite(angular.GetY()) &&
                                std::isfinite(angular.GetZ()) && std::isfinite(bounds.GetX()) && std::isfinite(bounds.GetY()) &&
                                std::isfinite(bounds.GetZ());
            if (!finite)
                return CanonicalNonFiniteBody{record.handle, record.sceneEntity};
        }
        return std::nullopt;
    }

    /** @copydoc CanonicalQueryFixtureUsesBodyHandle */
    bool CanonicalQueryFixtureUsesBodyHandle(const CanonicalWorldHandle world, const BodyHandle body) noexcept {
        if (world.value == nullptr)
            return false;
        const auto &fixtures = static_cast<const CanonicalWorld *>(world.value)->query.fixtures;
        return std::ranges::any_of(fixtures, [body](const auto &fixture) {
            return fixture.fixture.body == body;
        });
    }

    /** @copydoc QuarantineCanonicalSceneBody */
    void QuarantineCanonicalSceneBody(const CanonicalWorldHandle world, const BodyHandle body,
                                      const CanonicalRetirementSink &sink) noexcept {
        if (world.value == nullptr)
            return;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto found = std::ranges::find_if(canonical.scene.bodies, [body](const auto &record) {
            return record.handle == body;
        });
        if (found == canonical.scene.bodies.end())
            return;
        std::erase_if(canonical.scene.constraints, [&canonical, body, &sink](const CanonicalSceneConstraintRecord &constraint) {
            if (constraint.first != body && constraint.second != body)
                return false;
            canonical.native.system->RemoveConstraint(constraint.constraint.GetPtr());
            if (sink.constraint)
                sink.constraint(constraint.handle);
            return true;
        });
        auto &interface = canonical.native.system->GetBodyInterface();
        interface.RemoveBody(found->nativeBody);
        interface.DestroyBody(found->nativeBody);
        canonical.scene.bodies.erase(found);
        ++canonical.query.querySchemaGeneration;
        if (sink.body)
            sink.body(body);
    }

    /** @copydoc InjectCanonicalNonFiniteBodyForTesting */
    bool InjectCanonicalNonFiniteBodyForTesting(const CanonicalWorldHandle world, const BodyHandle body, const float value) noexcept {
        if (world.value == nullptr)
            return false;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto found = std::ranges::find_if(canonical.scene.bodies, [body](const auto &record) {
            return record.handle == body;
        });
        if (found == canonical.scene.bodies.end())
            return false;
        found->injectedStateForTesting = value;
        return true;
    }
}  // namespace Horo::Physics::Detail
