#include "CanonicalPhysicsStateTopologyMap.h"

namespace Horo::Physics::Detail {
    namespace {
        using JointIndex = std::pair<ConstraintHandle, std::size_t>;

        std::vector<CanonicalPhysicsStateBodyRecordIndex> BodyInventory(const CanonicalWorld &world) {
            std::vector<CanonicalPhysicsStateBodyRecordIndex> result;
            result.reserve(world.scene.bodies.size() + world.query.fixtures.size());
            for (std::size_t index = 0; index < world.scene.bodies.size(); ++index)
                result.emplace_back(world.scene.bodies[index].handle, index, false);
            for (std::size_t index = 0; index < world.query.fixtures.size(); ++index)
                result.emplace_back(world.query.fixtures[index].fixture.body, index, true);
            std::ranges::sort(result, {}, &CanonicalPhysicsStateBodyRecordIndex::handle);
            return result;
        }

        Result<void> Bodies(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                            CanonicalPhysicsStateTopologyMap &map) {
            const auto inventory = BodyInventory(world);
            if (inventory.size() != bindings.stableBodies.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            map.bodies.reserve(inventory.size());
            for (const auto &binding : bindings.stableBodies) {
                const auto found = std::ranges::lower_bound(inventory, binding.body, {}, &CanonicalPhysicsStateBodyRecordIndex::handle);
                if (found == inventory.end() || found->handle != binding.body)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                map.bodies.push_back(*found);
            }
            return Result<void>::Success();
        }

        Result<void> Joints(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                            CanonicalPhysicsStateTopologyMap &map) {
            std::vector<JointIndex> inventory;
            inventory.reserve(world.scene.constraints.size());
            for (std::size_t index = 0; index < world.scene.constraints.size(); ++index)
                inventory.emplace_back(world.scene.constraints[index].handle, index);
            std::ranges::sort(inventory, {}, &JointIndex::first);
            if (inventory.size() != bindings.stableJoints.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            map.joints.reserve(inventory.size());
            for (const auto &binding : bindings.stableJoints) {
                const auto found = std::ranges::lower_bound(inventory, binding.constraint, {}, &JointIndex::first);
                if (found == inventory.end() || found->first != binding.constraint)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
                map.joints.push_back(found->second);
            }
            return Result<void>::Success();
        }

        Result<void> Shapes(const CanonicalWorld &world, CanonicalPhysicsStateTopologyMap &map) {
            map.shapeRecords.reserve(world.scene.shapes.size());
            for (std::size_t index = 0; index < world.scene.shapes.size(); ++index) {
                const auto handle = world.scene.shapes[index].handle;
                const auto id = FindStateShapeId(map, handle);
                if (id == 0)
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
                map.shapeRecords.emplace_back(id, handle, index);
            }
            std::ranges::sort(map.shapeRecords, {}, &CanonicalPhysicsStateShapeRecordIndex::id);
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PrepareCanonicalPhysicsStateTopologyMap */
    Result<CanonicalPhysicsStateTopologyMap> PrepareCanonicalPhysicsStateTopologyMap(const CanonicalWorld &world,
                                                                                     const CanonicalPhysicsStateBindings &bindings,
                                                                                     std::span<const PhysicsStateShapeBinding> shapes) {
        using Outcome = Result<CanonicalPhysicsStateTopologyMap>;
        CanonicalPhysicsStateTopologyMap map;
        if (auto bodies = Bodies(world, bindings, map); bodies.HasError())
            return Outcome::Failure(bodies.ErrorValue());
        if (auto joints = Joints(world, bindings, map); joints.HasError())
            return Outcome::Failure(joints.ErrorValue());
        map.shapesByHandle.assign(shapes.begin(), shapes.end());
        std::ranges::sort(map.shapesByHandle, {}, &PhysicsStateShapeBinding::shape);
        if (auto shapesMapped = Shapes(world, map); shapesMapped.HasError())
            return Outcome::Failure(shapesMapped.ErrorValue());
        map.sceneBodies = world.scene.bodies.size();
        map.queryBodies = world.query.fixtures.size();
        map.sceneShapes = world.scene.shapes.size();
        map.sceneJoints = world.scene.constraints.size();
        return Outcome::Success(std::move(map));
    }

    /** @copydoc FindStateShapeId */
    PhysicsStateObjectId FindStateShapeId(const CanonicalPhysicsStateTopologyMap &map, ShapeHandle shape) noexcept {
        const auto found = std::ranges::lower_bound(map.shapesByHandle, shape, {}, &PhysicsStateShapeBinding::shape);
        return found != map.shapesByHandle.end() && found->shape == shape ? found->id : 0;
    }

    /** @copydoc CanonicalPhysicsStateTopologyMap::RetainedPayloadBytes */
    std::size_t CanonicalPhysicsStateTopologyMap::RetainedPayloadBytes() const noexcept {
        // sizeof(map) lives in workspace Impl and is charged there exactly once.
        return bodies.capacity() * sizeof(CanonicalPhysicsStateBodyRecordIndex) + joints.capacity() * sizeof(std::size_t) +
               shapesByHandle.capacity() * sizeof(PhysicsStateShapeBinding) +
               shapeRecords.capacity() * sizeof(CanonicalPhysicsStateShapeRecordIndex);
    }
}  // namespace Horo::Physics::Detail
