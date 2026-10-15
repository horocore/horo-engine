#include "PhysicsWorldInternal.h"

namespace Horo::Physics {
    /** @copydoc PhysicsWorld::Query */
    Result<PhysicsQueryResult> PhysicsWorld::Query(const PhysicsQueryDescriptor &descriptor, const std::span<PhysicsQueryHit> hits) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (const Result<void> valid = ValidatePhysicsQueryDescriptor(descriptor, impl_->identity, impl_->querySceneGeneration);
            valid.HasError())
            return Result<PhysicsQueryResult>::Failure(valid.ErrorValue());
        if (descriptor.bodyPoseFrame) {
            const auto published = impl_->publication.Snapshot();
            const auto &frame = *descriptor.bodyPoseFrame;
            if (published.publicationRevision != frame.sourcePublicationRevision ||
                published.completedTick == std::numeric_limits<std::uint64_t>::max() || frame.consumingTick != published.completedTick + 1)
                return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        }
        if (hits.size() > MaximumPhysicsQueryHits)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        return Detail::ExecuteCanonicalQuery(impl_->native, descriptor, hits);
    }

}  // namespace Horo::Physics
