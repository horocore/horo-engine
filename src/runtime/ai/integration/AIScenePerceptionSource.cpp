#include "Horo/AI/AIScenePerceptionSource.h"

#include "Horo/AI/AIErrors.h"

#include <limits>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Checks the current structural revision rather than trusting a retained entity slot. */
        [[nodiscard]] bool SceneSourceAlive(void *context, const PerceptionSourceRef &source) {
            const auto &scene = *static_cast<Runtime::RuntimeScene *>(context);
            const auto view = scene.View();
            if (!source.IsValid() || view.RuntimeId().value != source.sceneIncarnation)
                return false;
            const Runtime::EntityRef entity{.runtime = view.RuntimeId(),
                                            .entity = Runtime::EntityId{.index = source.slot, .generation = source.generation}};
            return view.Get(entity).HasValue();
        }

        /** @brief Owns the matching debug and paired blackboard values before any publication. */
        struct ProjectionCandidate final {
            PerceptionPublicationSnapshot snapshot;
            BlackboardCollectionValue sources{.elementKind = BlackboardValueKind::EntityReference};
            BlackboardCollectionValue positions{.elementKind = BlackboardValueKind::WorldCoordinate};
        };

        /** @brief Filters remembered subject/provenance and bounds the complete paired projection before writes begin. */
        [[nodiscard]] Result<ProjectionCandidate> BuildProjection(const Runtime::RuntimeSceneView &scene,
                                                                  const PerceptionSpatialSnapshot &filtering,
                                                                  const PerceptionMemorySnapshot &memory,
                                                                  const PerceptionPublicationSnapshot &identity) {
            ProjectionCandidate next{.snapshot = identity};
            for (std::size_t index = 0; index < memory.count; ++index) {
                const auto &record = memory.entries[index];
                const auto admits = [&](const PerceptionSourceRef source) {
                    return filtering.Allows(identity.listener, {scene.RuntimeId(), {source.slot, source.generation}}, record.key.sense);
                };
                if (!admits(record.key.source) || !admits(record.provenance))
                    continue;
                if (next.snapshot.memory.count == MaximumBlackboardCollectionElements)
                    return Result<ProjectionCandidate>::Failure(MakeError(AIErrors::PerceptionDescriptorLimitExceeded));
                const auto offset = next.snapshot.memory.count++;
                next.snapshot.memory.entries[offset] = record;
                next.sources.elements[next.sources.count++] =
                    BlackboardStoredEntityReference{record.key.source.sceneIncarnation, record.key.source.slot,
                                                    record.key.source.generation};
                next.positions.elements[next.positions.count++] = record.lastKnownPosition;
            }
            return Result<ProjectionCandidate>::Success(std::move(next));
        }
    }  // namespace

    /** @copydoc ProjectPerceptionSource */
    PerceptionSourceRef ProjectPerceptionSource(const Runtime::EntityRef entity) noexcept {
        return {.sceneIncarnation = entity.runtime.value, .slot = entity.entity.index, .generation = entity.entity.generation};
    }

    /** @copydoc PerceptionSceneLiveness */
    PerceptionSourceLiveness PerceptionSceneLiveness(Runtime::RuntimeScene &scene) noexcept {
        return {.context = &scene, .isAlive = SceneSourceAlive};
    }

    /** @copydoc PerceptionPublication::PublishAtBlackboardSync */
    Result<PerceptionPublicationSnapshot> PerceptionPublication::PublishAtBlackboardSync(
        Runtime::RuntimeScene &scene, const PerceptionSpatialBroadphase &filtering, const Runtime::EntityRef listener,
        AIPerceptionMemory &memory, BlackboardInstance &blackboard, const PerceptionBlackboardProjection projection,
        const std::uint64_t simulationTick) {
        using PublicationResult = Result<PerceptionPublicationSnapshot>;
        const auto spatial = filtering.Current();
        const auto view = scene.View();
        if (!spatial || !view.IsCurrent() || spatial->Scene() != view.RuntimeId() || listener.runtime != view.RuntimeId() ||
            view.Get(listener).HasError() || memory.SceneIncarnation() != view.RuntimeId().value ||
            blackboard.Binding().agent != memory.Agent() || !projection.sources.IsValid() || !projection.positions.IsValid() ||
            projection.sources == projection.positions)
            return PublicationResult::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        if (current_ && (current_->agent != memory.Agent() || current_->listener != listener || binding_ != blackboard.Binding() ||
                         projection_.sources != projection.sources || projection_.positions != projection.positions ||
                         simulationTick < current_->simulationTick || current_->revision == std::numeric_limits<std::uint64_t>::max()))
            return PublicationResult::Failure(MakeError(AIErrors::PerceptionSpatialInvalid));
        if (current_ && filtering_ == spatial && simulationTick == current_->simulationTick)
            return Current(view, filtering, blackboard);
        const auto remembered = memory.Snapshot(simulationTick, PerceptionSceneLiveness(scene));
        if (remembered.HasError())
            return PublicationResult::Failure(remembered.ErrorValue());
        const PerceptionPublicationSnapshot identity{.agent = memory.Agent(),
                                                     .listener = listener,
                                                     .revision = current_ ? current_->revision + 1 : 1,
                                                     .spatialRevision = spatial->Revision(),
                                                     .simulationTick = simulationTick};
        auto projected = BuildProjection(view, *spatial, remembered.Value(), identity);
        if (projected.HasError())
            return PublicationResult::Failure(projected.ErrorValue());
        auto next = std::move(projected).Value();
        auto createdBatch = blackboard.BeginWriteBatch();
        if (createdBatch.HasError())
            return PublicationResult::Failure(createdBatch.ErrorValue());
        auto batch = std::move(createdBatch).Value();
        if (const auto staged = batch.Stage({projection.sources, BlackboardValue{next.sources}, spatial->ReadAuthorization()});
            staged.HasError())
            return PublicationResult::Failure(staged.ErrorValue());
        if (const auto staged = batch.Stage({projection.positions, BlackboardValue{next.positions}, spatial->ReadAuthorization()});
            staged.HasError())
            return PublicationResult::Failure(staged.ErrorValue());
        const auto committed = blackboard.CommitAtBlackboardSync(std::move(batch));
        if (committed.HasError())
            return PublicationResult::Failure(committed.ErrorValue());
        next.snapshot.blackboardRevision = committed.Value().revision;
        filtering_ = spatial;
        binding_ = blackboard.Binding();
        projection_ = projection;
        current_ = next.snapshot;
        return PublicationResult::Success(next.snapshot);
    }

    /** @copydoc PerceptionPublication::Current */
    Result<PerceptionPublicationSnapshot> PerceptionPublication::Current(const Runtime::RuntimeSceneView &scene,
                                                                         const PerceptionSpatialBroadphase &filtering,
                                                                         const BlackboardInstance &blackboard) const {
        if (!current_ || filtering.Current() != filtering_ || !scene.IsCurrent() || scene.RuntimeId() != current_->listener.runtime ||
            scene.Get(current_->listener).HasError() || !blackboard.IsActive() || blackboard.Binding() != binding_)
            return Result<PerceptionPublicationSnapshot>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        const auto board = blackboard.Snapshot();
        if (board.HasError() || board.Value().Revision().HasError() || board.Value().Revision().Value() != current_->blackboardRevision)
            return Result<PerceptionPublicationSnapshot>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        for (std::size_t index = 0; index < current_->memory.count; ++index) {
            const auto &record = current_->memory.entries[index];
            if (scene.Get({scene.RuntimeId(), {record.key.source.slot, record.key.source.generation}}).HasError() ||
                scene.Get({scene.RuntimeId(), {record.provenance.slot, record.provenance.generation}}).HasError())
                return Result<PerceptionPublicationSnapshot>::Failure(MakeError(AIErrors::PerceptionSpatialStale));
        }
        return Result<PerceptionPublicationSnapshot>::Success(*current_);
    }

    /** @copydoc PerceptionPublication::Reset */
    void PerceptionPublication::Reset() noexcept {
        current_.reset();
        filtering_.reset();
    }
}  // namespace Horo::AI
