#include "Horo/AI/EnvironmentQueryPhysics.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <utility>

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

        constexpr std::uint32_t ProbeHits = 1;

        /** @brief Forms one command without broadening the host's authorized selectors. */
        Physics::PhysicsQueryCommand ProbeCommand(const QueryPhysicsPublication &publication, Physics::PhysicsQueryGeometry geometry,
                                                  Physics::PhysicsQueryFilter filter) {
            filter.blockingOnly = true;
            return {.identity = publication.identity,
                    .expectedPublicationRevision = publication.revision,
                    .descriptor = {.world = publication.identity.world,
                                   .sceneGeneration = publication.sceneGeneration,
                                   .geometry = std::move(geometry),
                                   .filter = filter,
                                   .collection = Physics::PhysicsQueryCollection::Closest,
                                   .maximumHitCount = ProbeHits}};
        }

        /** @brief Validates the captured publication and translates one bounded blocking result. */
        Result<bool> IsClear(const Physics::PhysicsQueryBatchEntry &entry, const QueryPhysicsPublication &publication) {
            const auto &result = entry.completion.result;
            if (entry.completion.publicationRevision != publication.revision ||
                entry.completion.completedTick != publication.completedTick || result.broadphaseSnapshotGeneration == 0)
                return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
            const Physics::PhysicsQueryDescriptor bounds{.maximumHitCount = ProbeHits};
            if (const auto valid = Physics::ValidatePhysicsQueryResult(result, bounds); valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
            if (entry.hits.size() != result.hitCount ||
                (!entry.hits.empty() && entry.hits.front().response != Physics::PhysicsQueryResponse::Block))
                return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            return Result<bool>::Success(entry.hits.empty());
        }

        /** @brief Reduces only complete evidence from a single provider publication. */
        Result<std::optional<QueryTestSample>> TranslateCompletion(const Physics::PhysicsQueryBatchCompletion &completion,
                                                                   const QueryPhysicsPublication &publication,
                                                                   const std::size_t commandCount, const QueryStageId stage) {
            using Sample = Result<std::optional<QueryTestSample>>;
            if (commandCount == 0 || completion.entries.size() != commandCount)
                return Sample::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
            std::size_t clear = 0;
            for (const auto &entry : completion.entries) {
                auto result = IsClear(entry, publication);
                if (result.HasError())
                    return Sample::Failure(std::move(result).ErrorValue());
                clear += result.Value();
            }
            return Sample::Success(QueryTestSample{.stage = stage,
                                                   .filter = clear != 0,
                                                   .metric = static_cast<double>(clear) / static_cast<double>(commandCount)});
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
        if (!IsProbeGeometry(geometry))
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        const auto command = ProbeCommand(publication, std::move(geometry), filter);
        return Queue(capability, publication, stage, std::span{&command, 1});
    }

    /** @copydoc QueryPhysicsProbe::StartExposure */
    Result<bool> QueryPhysicsProbe::StartExposure(const Physics::PhysicsQueryEventCapability &capability,
                                                  const QueryPhysicsPublication publication, const QueryStageId stage,
                                                  const std::span<const Physics::PhysicsRayQuery> rays,
                                                  const Physics::PhysicsQueryFilter filter) {
        if (rays.empty())
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (rays.size() > MaximumExposureRays)
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryLimitExceeded));
        std::array<Physics::PhysicsQueryCommand, MaximumExposureRays> commands;
        for (std::size_t index = 0; index < rays.size(); ++index)
            commands[index] = ProbeCommand(publication, rays[index], filter);
        return Queue(capability, publication, stage, std::span{commands}.first(rays.size()));
    }

    /** @copydoc QueryPhysicsProbe::Queue */
    Result<bool> QueryPhysicsProbe::Queue(const Physics::PhysicsQueryEventCapability &capability, const QueryPhysicsPublication publication,
                                          const QueryStageId stage, const std::span<const Physics::PhysicsQueryCommand> commands) {
        if (pending_ || !stage.IsValid() || publication.completedTick == 0 || publication.revision == 0)
            return Result<bool>::Failure(MakeError(AIErrors::EnvironmentQueryContextInvalid));
        if (!SameAccess(publication.identity, capability.Identity()))
            return Result<bool>::Failure(MakeError(Physics::PhysicsErrors::CapabilityStale));
        const auto live = capability.ValidateQueryPublication(publication.sceneGeneration, publication.completedTick, publication.revision);
        if (live.HasError())
            return Result<bool>::Failure(live.ErrorValue());

        auto admitted = capability.SubmitBatch(commands);
        if (admitted.HasError()) {
            if (admitted.ErrorValue().code.Value() == Physics::PhysicsErrors::CapacityExceeded.code.Value())
                return Result<bool>::Success(false);
            return Result<bool>::Failure(admitted.ErrorValue());
        }
        capability_ = capability;
        publication_ = publication;
        stage_ = stage;
        commandCount_ = commands.size();
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
        const auto live = capability_.ValidateQueryPublication(current.sceneGeneration, current.completedTick, current.revision);
        if (live.HasError()) {
            Cancel();
            return ResultType::Failure(live.ErrorValue());
        }
        const auto completed = pending_->Poll();
        if (completed.HasValue() && !completed.Value())
            return ResultType::Success(std::nullopt);
        pending_.reset();
        capability_ = {};
        const auto commandCount = std::exchange(commandCount_, 0);
        if (completed.HasError())
            return ResultType::Failure(completed.ErrorValue());
        return TranslateCompletion(*completed.Value(), publication_, commandCount, stage_);
    }

    /** @copydoc QueryPhysicsProbe::Cancel */
    void QueryPhysicsProbe::Cancel() noexcept {
        if (pending_)
            (void)pending_->Cancel();
        pending_.reset();
        capability_ = {};
        commandCount_ = 0;
    }
}  // namespace Horo::AI
