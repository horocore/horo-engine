#include "CanonicalPhysicsStateTopology.h"
#include "PhysicsStateValidationInternal.h"

#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Pinned native leaf families have no residual subshape bits;
         * unsupported future shapes fail closed. */
        bool Leaf(const JPH::Shape &shape, JPH::SubShapeID key) noexcept {
            switch (shape.GetSubType()) {
                case JPH::EShapeSubType::Box:
                case JPH::EShapeSubType::Sphere:
                case JPH::EShapeSubType::Capsule:
                case JPH::EShapeSubType::Plane:
                    return key.IsEmpty();
                default:
                    return false;
            }
        }

        /** @brief Validates every compound path before calling any native method that
         * asserts on an invalid index. */
        bool Subshape(const JPH::Shape *shape, std::uint32_t code, std::size_t maximumDepth) noexcept {
            JPH::SubShapeID key;
            key.SetValue(code);
            for (std::size_t depth = 0; depth <= maximumDepth; ++depth) {
                if (shape->GetSubType() == JPH::EShapeSubType::RotatedTranslated) {
                    shape = static_cast<const JPH::RotatedTranslatedShape &>(*shape).GetInnerShape();
                    continue;
                }
                if (shape->GetType() != JPH::EShapeType::Compound)
                    return Leaf(*shape, key);
                const auto &compound = static_cast<const JPH::CompoundShape &>(*shape);
                if (!compound.IsSubShapeIDValid(key))
                    return false;
                JPH::SubShapeID remaining;
                const auto index = compound.GetSubShapeIndexFromID(key, remaining);
                shape = compound.GetSubShape(index).mShape.GetPtr();
                key = remaining;
            }
            return false;
        }

        std::size_t SceneShapeBytes(const CanonicalWorld &world, JPH::BodyID id) noexcept {
            if (id.GetIndex() >= world.scene.nativeBodyIndices.size())
                return 0;
            const auto index = world.scene.nativeBodyIndices[id.GetIndex()];
            if (index >= world.scene.bodies.size())
                return 0;
            const auto &record = world.scene.bodies[index];
            return record.nativeBody == id ? record.shapeNativeRequestedBytes : 0;
        }

        std::size_t QueryShapeBytes(const CanonicalWorld &world, JPH::BodyID id) noexcept {
            if (id.GetIndex() >= world.query.nativeFixtureIndices.size())
                return 0;
            const auto index = world.query.nativeFixtureIndices[id.GetIndex()];
            if (index >= world.query.fixtures.size())
                return 0;
            const auto &record = world.query.fixtures[index];
            return record.nativeBody == id ? record.nativeRequestedBytes : 0;
        }

        /** @brief Every native path node was an actual Shape allocation of at least sizeof(Shape).
         * The creation observer+retained-child reservation bounds traversal even after child table records retire.
         * An identity single-child optimization adds no node; a translated wrapper is an observed allocation.
         */
        std::size_t ShapeNodeBound(const CanonicalWorld &world, JPH::BodyID id) noexcept {
            const auto scene = SceneShapeBytes(world, id);
            const auto bytes = scene == 0 ? QueryShapeBytes(world, id) : scene;
            return bytes / sizeof(JPH::Shape);
        }

        /** @brief Uses actual current shape evidence; matching a numeric subshape code
         * alone never proves compatibility. */
        bool Endpoint(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings, PhysicsStateObjectId id,
                      std::uint32_t subshape) noexcept {
            const auto *binding = FindStableStateBody(bindings, id);
            if (!binding)
                return false;
            const auto *body = world.native.system->GetBodyLockInterfaceNoLock().TryGetBody(JPH::BodyID{binding->nativeId});
            return body && Subshape(body->GetShape(), subshape, ShapeNodeBound(world, JPH::BodyID{binding->nativeId}));
        }

        /** @brief Pinned AddBodyPair/AddContactConstraint/CCD creation all retain lower
         * native BodyID first. */
        bool NativePairOrder(const CanonicalPhysicsStateBindings &bindings, PhysicsStateObjectId first,
                             PhysicsStateObjectId second) noexcept {
            const auto *a = FindStableStateBody(bindings, first);
            const auto *b = FindStableStateBody(bindings, second);
            return a && b && a->nativeId < b->nativeId;
        }

        /** @brief Both manifold endpoints must belong to the exact retained immutable
         * topology. */
        bool Key(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings, const PhysicsStateManifoldKey &key) noexcept {
            return NativePairOrder(bindings, key.first, key.second) && Endpoint(world, bindings, key.first, key.firstSubshape) &&
                   Endpoint(world, bindings, key.second, key.secondSubshape);
        }

        /** @brief Bounds apply to the authored body origin, not a compound's possibly
         * offset center of mass. */
        bool OriginFits(const CanonicalWorld &world, const CanonicalPhysicsStateBodyBinding &binding, const PhysicsStateBody &value,
                        float extent) noexcept {
            const auto *body = world.native.system->GetBodyLockInterfaceNoLock().TryGetBody(JPH::BodyID{binding.nativeId});
            if (!body || (body->GetMotionPropertiesUnchecked() != nullptr) != value.hasMotion)
                return false;
            const JPH::Vec3 center{value.centerOfMass[0], value.centerOfMass[1], value.centerOfMass[2]};
            const JPH::Quat rotation{value.rotation[0], value.rotation[1], value.rotation[2], value.rotation[3]};
            const auto origin = center - rotation * body->GetShape()->GetCenterOfMass();
            const std::array components{origin.GetX(), origin.GetY(), origin.GetZ()};
            return std::ranges::all_of(components, [extent](float component) {
                return std::isfinite(component) && std::abs(component) <= extent;
            });
        }

        /** @brief Stable identities and native motion storage presence are complete,
         * not a subset restore. */
        bool Bodies(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings, const PhysicsStateSolverView &state,
                    const PhysicsWorldSettings &settings) noexcept {
            if (state.bodies.size() != bindings.stableBodies.size() || state.bodies.size() != world.native.system->GetNumBodies())
                return false;
            const auto extent = settings.Values().bounds.localHalfExtentMeters;
            for (std::size_t index = 0; index < state.bodies.size(); ++index) {
                const auto &body = state.bodies[index];
                const auto &binding = bindings.stableBodies[index];
                if (body.id != binding.id || body.hasMotion != binding.hasMotion)
                    return false;
                if (!OriginFits(world, binding, body, extent))
                    return false;
            }
            return true;
        }

        /** @brief Immutable settings own gravity and the only admitted positive
         * previous native step duration. */
        bool Global(const PhysicsStateSolverView &state, const PhysicsWorldSettings &settings) noexcept {
            const auto &values = settings.Values().world;
            const std::array gravity{values.gravity.x, values.gravity.y, values.gravity.z};
            const auto delta = static_cast<float>(values.fixedDeltaSeconds);
            return state.gravity == gravity && (state.previousDelta == 0 || state.previousDelta == delta);
        }

        /** @brief Every native contact section uses the same retained endpoint/shape
         * authority. */
        Result<void> ContactKeys(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                 const PhysicsStateSolverView &state) {
            for (const auto &pair : state.pairs)
                if (!NativePairOrder(bindings, pair.first, pair.second))
                    return StateDetail::Invalid("Physics state pair orientation disagrees "
                                                "with its retained solver topology.");
            for (const auto &manifold : state.manifolds)
                if (!Key(world, bindings, manifold.key))
                    return StateDetail::Invalid("Physics state manifold subshape does not "
                                                "exist in retained shape topology.");
            for (const auto &key : state.ccdManifolds)
                if (!Key(world, bindings, key))
                    return StateDetail::Invalid("Physics state CCD subshape does not exist "
                                                "in retained shape topology.");
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateCanonicalPhysicsStateTopology */
    Result<void> ValidateCanonicalPhysicsStateTopology(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                                       const PhysicsStateSolverView &state, const PhysicsWorldSettings &settings) {
        if (!Global(state, settings))
            return StateDetail::Invalid("Physics state global solver values disagree "
                                        "with immutable world settings.");
        if (!Bodies(world, bindings, state, settings) || state.constraints.size() != bindings.stableJoints.size())
            return StateDetail::Invalid("Physics state bodies or constraints disagree "
                                        "with exact retained topology.");
        for (std::size_t index = 0; index < state.constraints.size(); ++index) {
            const auto &binding = bindings.stableJoints[index];
            if (state.constraints[index].id != binding.id || state.constraints[index].kind != binding.kind)
                return StateDetail::Invalid("Physics state joint family or stable "
                                            "identity does not match retained topology.");
        }
        return ContactKeys(world, bindings, state);
    }
}  // namespace Horo::Physics::Detail
