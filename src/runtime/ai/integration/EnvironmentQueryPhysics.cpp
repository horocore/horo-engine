#include "Horo/AI/EnvironmentQueryPhysics.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/Physics/PhysicsErrors.h"

namespace Horo::AI {
    namespace {
        /** @brief Compares the exact revocable Physics access identity. */
        bool SameAccess(const Physics::PhysicsQueryEventIdentity &left, const Physics::PhysicsQueryEventIdentity &right) noexcept {
            return left.world == right.world && left.capabilityGeneration == right.capabilityGeneration;
        }

        /** @brief Restricts tactical probes to binary visibility and occupancy geometry. */
        bool IsProbeGeometry(const Physics::PhysicsQueryGeometry &geometry) noexcept {
            return std::holds_alternative<Physics::PhysicsRayQuery>(geometry) ||
                   std::holds_alternative<Physics::PhysicsOverlapQuery>(geometry) ||
                   std::holds_alternative<Physics::PhysicsCapsuleOverlapQuery>(geometry);
        }

        /** @brief Validates the exact publication and bounded blocking evidence from Physics. */
        Result<std::optional<QueryTestSample>> TranslateCompletion(const Physics::PhysicsQueryBatchCompletion &completion,
                                                                   const QueryPhysicsPublication &publication,
                                                                   const Physics::PhysicsQueryDescriptor &descriptor,
                                                                   const QueryStageId stage) {
            if (completion.entries.size() != 1)
                return Result<std::optional<QueryTestSample>>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            const auto &entry = completion.entries.front();
            const auto &result = entry.completion.result;
            if (entry.completion.publicationRevision != publication.revision ||
                entry.completion.completedTick != publication.completedTick || result.broadphaseSnapshotGeneration == 0)
                return Result<std::optional<QueryTestSample>>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
            if (const auto valid = Physics::ValidatePhysicsQueryResult(result, descriptor); valid.HasError())
                return Result<std::optional<QueryTestSample>>::Failure(valid.ErrorValue());
            if (entry.hits.size() != result.hitCount ||
                (!entry.hits.empty() && entry.hits.front().response != Physics::PhysicsQueryResponse::Block))
                return Result<std::optional<QueryTestSample>>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            const bool clear = entry.hits.empty();
            return Result<std::optional<QueryTestSample>>::Success(
                QueryTestSample{.stage = stage, .filter = clear, .metric = clear ? 1.0 : 0.0});
        }
    }  // namespace

    /** @copydoc QueryPhysicsProbe::~QueryPhysicsProbe */
    QueryPhysicsProbe::~QueryPhysicsProbe() {
        Cancel();
    }

    /** @copydoc QueryPhysicsProbe::Start */
    Result<bool> QueryPhysicsProbe::Start(const Physics::PhysicsQueryEventCapability &capability, const QueryPhysicsPublication publication,
                                          const QueryStageId stage, Physics::PhysicsQueryGeometry geometry,
                                          Physics::PhysicsQueryFilter filter) {
        if (pending_ || !stage.IsValid() || publication.completedTick == 0 || publication.revision == 0)
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (!IsProbeGeometry(geometry))
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (!SameAccess(publication.identity, capability.Identity()))
            return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::CapabilityStale));

        filter.blockingOnly = true;
        const Physics::PhysicsQueryCommand command{.identity = publication.identity,
                                                   .expectedPublicationRevision = publication.revision,
                                                   .descriptor = {.world = publication.identity.world,
                                                                  .sceneGeneration = publication.sceneGeneration,
                                                                  .geometry = std::move(geometry),
                                                                  .filter = filter,
                                                                  .collection = Physics::PhysicsQueryCollection::Closest,
                                                                  .maximumHitCount = 1}};
        auto admitted = capability.SubmitBatch(std::span{&command, 1});
        if (admitted.HasError()) {
            if (admitted.ErrorValue().code.Value() == Physics::PhysicsErrors::CapacityExceeded.code.Value())
                return Result<bool>::Success(false);
            return Result<bool>::Failure(admitted.ErrorValue());
        }
        publication_ = publication;
        stage_ = stage;
        descriptor_ = command.descriptor;
        pending_.emplace(std::move(admitted).Value());
        return Result<bool>::Success(true);
    }

    /** @copydoc QueryPhysicsProbe::Poll */
    Result<std::optional<QueryTestSample>> QueryPhysicsProbe::Poll(const QueryPhysicsPublication &current) {
        using ResultType = Result<std::optional<QueryTestSample>>;
        if (!pending_)
            return ResultType::Success(std::nullopt);
        if (!SameAccess(current.identity, publication_.identity) || current.sceneGeneration != publication_.sceneGeneration ||
            current.revision != publication_.revision || current.completedTick != publication_.completedTick) {
            Cancel();
            return ResultType::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        }
        const auto completed = pending_->Poll();
        if (completed.HasError()) {
            pending_.reset();
            return ResultType::Failure(completed.ErrorValue());
        }
        if (!completed.Value())
            return ResultType::Success(std::nullopt);
        pending_.reset();
        return TranslateCompletion(*completed.Value(), publication_, descriptor_, stage_);
    }

    /** @copydoc QueryPhysicsProbe::Cancel */
    void QueryPhysicsProbe::Cancel() noexcept {
        if (pending_)
            (void)pending_->Cancel();
        pending_.reset();
    }
}  // namespace Horo::AI
