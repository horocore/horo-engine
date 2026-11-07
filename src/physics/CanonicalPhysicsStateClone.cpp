#include "CanonicalPhysicsStateCloneInternal.h"
#include "CanonicalPhysicsStateConstraintReplay.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Copies only complete retained immutable/Horo metadata before any
         * candidate publication. */
        void CopyMetadata(const CanonicalWorld &source, CanonicalWorld &candidate) {
            candidate.scene.shapes = source.scene.shapes;
            candidate.scene.disabledJointCollisionPairs = source.scene.disabledJointCollisionPairs;
            candidate.scene.nextConstraintSlot = source.scene.nextConstraintSlot;
            candidate.nextResourceSlot = source.nextResourceSlot;
            candidate.continuousCollision = source.continuousCollision;
            candidate.query.nextFixtureGeneration = source.query.nextFixtureGeneration;
            candidate.query.querySchemaGeneration = source.query.querySchemaGeneration;
            candidate.query.nativeFixtureIndices = source.query.nativeFixtureIndices;
            candidate.native.system->SetPhysicsSettings(source.native.system->GetPhysicsSettings());
        }
    }  // namespace

    /** @copydoc CloneCanonicalPhysicsStateTopology */
    Result<CanonicalWorldHandle> CloneCanonicalPhysicsStateTopology(const CanonicalWorldHandle handle,
                                                                    const PhysicsWorldSettings &settings) {
        const auto *source = static_cast<const CanonicalWorld *>(handle.value);
        if (!source || !source->native.system || !source->pendingBodyBatch.expired() || source->contactRoute.load())
            return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (auto profile = ValidateCanonicalConstraintCreationProfile(*source); profile.HasError())
            return Result<CanonicalWorldHandle>::Failure(profile.ErrorValue());
        auto created = CreateCanonicalWorld({&source->owner}, settings, CanonicalFailurePoint::None, source->simulation.binding);
        if (created.HasError())
            return created;
        std::unique_ptr<CanonicalWorld> candidate{static_cast<CanonicalWorld *>(created.Value().value)};
        CopyMetadata(*source, *candidate);
        if (auto bodies = CloneCanonicalStateBodies(*source, *candidate); bodies.HasError())
            return Result<CanonicalWorldHandle>::Failure(bodies.ErrorValue());
        if (auto joints = CloneCanonicalStateJoints(*source, *candidate); joints.HasError())
            return Result<CanonicalWorldHandle>::Failure(joints.ErrorValue());
        RebuildCanonicalSceneBodyIndices(*candidate);
        return Result<CanonicalWorldHandle>::Success({candidate.release()});
    }
}  // namespace Horo::Physics::Detail
