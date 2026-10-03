#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Resolves an exact resident handle at the joined owner-thread safe point. */
        auto FindResidentBody(CanonicalWorld &world, const BodyHandle body) {
            return std::ranges::find_if(world.scene.bodies, [body](const auto &record) {
                return record.handle == body;
            });
        }
    }  // namespace

    /** @copydoc CanonicalSceneEntity */
    std::uint64_t CanonicalSceneEntity(const CanonicalWorldHandle world, const BodyHandle body) noexcept {
        if (world.value == nullptr)
            return 0;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto found = FindResidentBody(canonical, body);
        return found == canonical.scene.bodies.end() ? 0 : found->sceneEntity;
    }

    /** @copydoc SetCanonicalSceneEntity */
    void SetCanonicalSceneEntity(const CanonicalWorldHandle world, const BodyHandle body, const std::uint64_t sceneEntity) noexcept {
        if (world.value == nullptr)
            return;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (const auto found = FindResidentBody(canonical, body); found != canonical.scene.bodies.end())
            found->sceneEntity = sceneEntity;
    }

    /** @copydoc FindCanonicalNonFiniteBody */
    std::optional<CanonicalNonFiniteBody> FindCanonicalNonFiniteBody(const CanonicalWorldHandle world, const bool postStep,
                                                                     std::size_t &cursor) noexcept {
        if (world.value == nullptr)
            return std::nullopt;
        const auto &canonical = *static_cast<const CanonicalWorld *>(world.value);
        for (; cursor < canonical.scene.bodies.size(); ++cursor) {
            const CanonicalSceneBodyRecord &record = canonical.scene.bodies[cursor];
            JPH::BodyLockRead lock(canonical.native.system->GetBodyLockInterfaceNoLock(), record.nativeBody);
            if (!lock.Succeeded())
                return CanonicalNonFiniteBody{record.handle, record.sceneEntity, false};
            const JPH::Body &body = lock.GetBody();
            const auto position = body.GetPosition();
            const auto rotation = body.GetRotation();
            const auto linear = body.GetLinearVelocity();
            const auto angular = body.GetAngularVelocity();
            const auto &bounds = body.GetWorldSpaceBounds();
            std::array components{position.GetX(),    position.GetY(),    position.GetZ(),    rotation.GetX(),    rotation.GetY(),
                                  rotation.GetZ(),    rotation.GetW(),    linear.GetX(),      linear.GetY(),      linear.GetZ(),
                                  angular.GetX(),     angular.GetY(),     angular.GetZ(),     bounds.mMin.GetX(), bounds.mMin.GetY(),
                                  bounds.mMin.GetZ(), bounds.mMax.GetX(), bounds.mMax.GetY(), bounds.mMax.GetZ()};
            // Inject into the copied readback, never into Jolt state before a solver call.
            if (record.injectedStateForTesting.has_value() && record.injectPostStepForTesting == postStep)
                components[record.injectedComponentForTesting] = *record.injectedStateForTesting;
            const bool finite = std::ranges::all_of(components, [](const float component) {
                return std::isfinite(component);
            });
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
        const auto found = FindResidentBody(canonical, body);
        if (found == canonical.scene.bodies.end())
            return;
        for (std::size_t index = 0; index < canonical.scene.constraints.size();) {
            const auto &constraint = canonical.scene.constraints[index];
            if (constraint.first != body && constraint.second != body) {
                ++index;
                continue;
            }
            const ConstraintHandle retired = constraint.handle;
            (void)DestroyCanonicalSceneConstraint(world, retired);
            sink.Retire(retired);
        }
        auto &interface = canonical.native.system->GetBodyInterface();
        interface.RemoveBody(found->nativeBody);
        interface.DestroyBody(found->nativeBody);
        canonical.scene.bodies.erase(found);
        ++canonical.query.querySchemaGeneration;
        sink.Retire(body);
    }

    /** @copydoc InjectCanonicalNonFiniteBodyForTesting */
    bool InjectCanonicalNonFiniteBodyForTesting(const CanonicalWorldHandle world, const BodyHandle body, const float value,
                                                const std::uint8_t component, const bool postStep) noexcept {
        if (world.value == nullptr || component >= 19)
            return false;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto found = FindResidentBody(canonical, body);
        if (found == canonical.scene.bodies.end())
            return false;
        found->injectedStateForTesting = value;
        found->injectedComponentForTesting = component;
        found->injectPostStepForTesting = postStep;
        return true;
    }
}  // namespace Horo::Physics::Detail
