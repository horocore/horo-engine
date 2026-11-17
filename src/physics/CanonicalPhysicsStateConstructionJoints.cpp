#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateConstructionExport.h"

namespace Horo::Physics::Detail {
    namespace {
        Result<void> Endpoints(const CanonicalSceneConstraintRecord &record, const CanonicalPhysicsStateBindings &bindings,
                               PhysicsStateConstructionJoint &out) {
            const auto *first = FindNativeStateBody(bindings, record.firstBody.GetIndexAndSequenceNumber());
            if (!first || first->body != record.first || record.descriptor.first.body != record.first)
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            out.first = first->id;
            out.firstLocalFrame = record.descriptor.first.localFrame;
            if (const auto *anchor = std::get_if<PhysicsBodyAnchor>(&record.descriptor.second)) {
                const auto *second = FindNativeStateBody(bindings, record.secondBody.GetIndexAndSequenceNumber());
                if (!second || second->body != record.second || anchor->body != record.second)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
                out.second = second->id;
                out.secondLocalOrWorldFrame = anchor->localFrame;
            } else {
                if (record.second.IsValid() || !record.secondBody.IsInvalid())
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
                out.secondLocalOrWorldFrame = std::get<PhysicsWorldAnchor>(record.descriptor.second).frame;
            }
            return Result<void>::Success();
        }

        Result<void> Joint(const CanonicalSceneConstraintRecord &record, const CanonicalPhysicsStateJointBinding &binding,
                           const CanonicalPhysicsStateBindings &bindings, CanonicalConstructionRows &rows) {
            if (record.handle != binding.constraint || !record.constraint || rows.joints >= rows.storage.joints.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            PhysicsStateConstructionJoint out{.id = binding.id,
                                              .parameters = record.descriptor.parameters,
                                              .collisionPolicy = record.collisionPolicy,
                                              .firstConstructorFrame = record.creation.firstFrame,
                                              .secondConstructorFrame = record.creation.secondFrame,
                                              .firstCom = {record.creation.first.centerOfMass, record.creation.first.rotation},
                                              .secondCom = {record.creation.second.centerOfMass, record.creation.second.rotation},
                                              .floatingPoint = record.creation.floatingPoint};
            if (auto endpoints = Endpoints(record, bindings, out); endpoints.HasError())
                return endpoints;
            rows.storage.joints[rows.joints++] = out;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ExportCanonicalConstructionJoints */
    Result<void> ExportCanonicalConstructionJoints(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                                   const CanonicalPhysicsStateTopologyMap &map, CanonicalConstructionRows &rows) {
        if (map.joints.size() != bindings.stableJoints.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        for (std::size_t i = 0; i < map.joints.size(); ++i) {
            if (map.joints[i] >= world.scene.constraints.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            if (auto copied = Joint(world.scene.constraints[map.joints[i]], bindings.stableJoints[i], bindings, rows); copied.HasError())
                return copied;
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
