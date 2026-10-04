#pragma once

#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    /** @brief Detached native storage; the weak world registration lets teardown invalidate retained candidates safely. */
    struct CanonicalSceneBodyBatchState final {
        ~CanonicalSceneBodyBatchState() {
            Abort();
        }

        void Abort() noexcept {
            if (world == nullptr)
                return;
            auto &bodies = world->native.system->GetBodyInterface();
            if (broadphasePrepared)
                bodies.AddBodiesAbort(nativeBodies.data(), static_cast<int>(nativeBodies.size()), addState);
            constraints.clear();
            nativeConstraints.clear();
            for (const auto &record : records)
                bodies.DestroyBody(record.nativeBody);
            shapes.clear();
            world->pendingBodyBatch.reset();
            world = nullptr;
        }

        CanonicalWorld *world{};
        PhysicsWorldId owner;
        std::vector<BodyHandle> retiredBodies;
        std::vector<ShapeHandle> retiredShapes;
        std::vector<ConstraintHandle> retiredConstraints;
        std::vector<JPH::BodyID> retiredNativeBodies;
        std::vector<JPH::Constraint *> retiredNativeConstraints;
        bool retirementPrepared{};
        bool retirementFailed{};
        std::vector<CanonicalSceneShapeRecord> shapes;
        std::vector<ShapeHandle> shapeHandles;
        std::vector<CanonicalSceneConstraintRecord> constraints;
        std::vector<ConstraintHandle> constraintHandles;
        std::vector<JPH::Constraint *> nativeConstraints;
        std::vector<std::uint64_t> collisionPairs;
        bool constraintsPrepared{};
        bool constraintsFailed{};
        std::vector<CanonicalSceneBodyRecord> records;
        std::vector<BodyHandle> handles;
        std::vector<JPH::BodyID> nativeBodies;
        std::vector<JPH::BodyID> awakeBodies;
        JPH::BodyInterface::AddState addState{};
        bool broadphasePrepared{};
    };

}  // namespace Horo::Physics::Detail
