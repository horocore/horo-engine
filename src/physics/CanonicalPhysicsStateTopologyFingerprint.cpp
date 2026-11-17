#include "CanonicalPhysicsStateBackendInternal.h"
#include "CanonicalPhysicsStateTopologyHash.h"

namespace Horo::Physics::Detail {
    namespace {
        bool SameInventory(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                           const CanonicalPhysicsStateTopologyMap &map) noexcept {
            return world.scene.bodies.size() == map.sceneBodies && world.query.fixtures.size() == map.queryBodies &&
                   world.scene.shapes.size() == map.sceneShapes && world.scene.constraints.size() == map.sceneJoints &&
                   map.bodies.size() == bindings.stableBodies.size() && map.joints.size() == bindings.stableJoints.size() &&
                   world.native.system->GetNumBodies() == map.bodies.size();
        }

        Result<void> Shapes(StateDetail::StateHash &hash, const CanonicalWorld &world, const CanonicalPhysicsStateTopologyMap &map) {
            hash.Word(map.shapeRecords.size());
            for (const auto &index : map.shapeRecords) {
                if (index.index >= world.scene.shapes.size())
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
                const auto &record = world.scene.shapes[index.index];
                if (record.handle != index.handle || !record.shape)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
                hash.Word(index.id);
                hash.Digest(record.recipe);
            }
            return Result<void>::Success();
        }

        Result<void> Bodies(StateDetail::StateHash &hash, const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                            const CanonicalPhysicsStateTopologyMap &map) {
            hash.Word(map.bodies.size());
            for (std::size_t index = 0; index < map.bodies.size(); ++index) {
                const auto &record = map.bodies[index];
                hash.Word(record.fixture);
                auto body = record.fixture ? HashStateFixtureBody(hash, world, bindings.stableBodies[index], record, map)
                                           : HashStateSceneBody(hash, world, bindings.stableBodies[index], record, map);
                if (body.HasError())
                    return body;
            }
            return Result<void>::Success();
        }

        Result<void> Joints(StateDetail::StateHash &hash, const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                            const CanonicalPhysicsStateTopologyMap &map) {
            hash.Word(map.joints.size());
            for (std::size_t index = 0; index < map.joints.size(); ++index)
                if (auto joint = HashStateJoint(hash, world, bindings.stableJoints[index], map.joints[index], bindings); joint.HasError())
                    return joint;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CanonicalPhysicsStateTopologyFingerprint */
    Result<Sha256Digest> CanonicalPhysicsStateTopologyFingerprint(const CanonicalWorld &world,
                                                                  const CanonicalPhysicsStateBindings &bindings,
                                                                  const CanonicalPhysicsStateTopologyMap &map) {
        using Outcome = Result<Sha256Digest>;
        if (!world.native.system || !SameInventory(world, bindings, map))
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityStale));
        StateDetail::StateHash hash{"Horo.Physics.State.Topology.v1"};
        if (auto shapes = Shapes(hash, world, map); shapes.HasError())
            return Outcome::Failure(shapes.ErrorValue());
        if (auto bodies = Bodies(hash, world, bindings, map); bodies.HasError())
            return Outcome::Failure(bodies.ErrorValue());
        if (auto joints = Joints(hash, world, bindings, map); joints.HasError())
            return Outcome::Failure(joints.ErrorValue());
        return Outcome::Success(hash.Finish());
    }
}  // namespace Horo::Physics::Detail
