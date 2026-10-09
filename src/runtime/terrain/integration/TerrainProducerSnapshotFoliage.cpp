#include "TerrainProducerSnapshotInternal.h"

namespace Horo::Terrain::ProducerDetail {
    namespace {
        /** @brief Resolves an exact pinned definition and rejects ambiguous same-type or stale revision evidence. */
        Result<const FoliageTypeDefinitionData *> Definition(const TerrainProducerSnapshotRequest &request,
                                                             const CookedFoliageCluster &cluster, Budget &budget,
                                                             const CancellationToken &cancellation) {
            const FoliageTypeDefinitionData *found{};
            for (const auto &definition : request.definitions) {
                auto step = budget.Step(request, cancellation);
                if (!step.HasValue())
                    return Result<const FoliageTypeDefinitionData *>::Failure(step.ErrorValue());
                if (definition.Data().type == cluster.type) {
                    if (found || definition.Data().revision != cluster.definitionRevision)
                        return Result<const FoliageTypeDefinitionData *>::Failure(Failure(request, TerrainProducerErrors::Stale));
                    found = &definition.Data();
                }
            }
            if (!found)
                return Result<const FoliageTypeDefinitionData *>::Failure(Failure(request, TerrainProducerErrors::Invalid));
            return Result<const FoliageTypeDefinitionData *>::Success(found);
        }

        /** @brief Projects only exact authored consumer-relevant primitives, retaining visual-only membership during admission. */
        bool Relevant(const FoliageTypeDefinitionData &definition, const TerrainProducerConsumer consumer) noexcept {
            return definition.collision.shape != FoliageCollisionShape::None &&
                   (consumer == TerrainProducerConsumer::Collision || definition.collision.blocksNavigation);
        }

        /** @brief Validates integer placement and instance identity without consumer-native conversion. */
        bool ValidInstance(const CookedFoliageInstance &instance, const CookedFoliageCluster &cluster,
                           const FoliageTypeDefinitionData &definition) noexcept {
            return instance.id.IsValid() && instance.cluster == cluster.id && instance.type == cluster.type &&
                   instance.scalePermille >= definition.minimumScalePermille && instance.scalePermille <= definition.maximumScalePermille &&
                   instance.yawMilliDegrees < 360'000 && instance.slopeMilliDegrees <= 90'000 && instance.normalYPermille > 0;
        }

        /** @brief Examines every selected instance, including visual-only instances, and rejects duplicates/out-of-order IDs. */
        Result<void> ValidateInstances(const TerrainProducerSnapshotRequest &request, const CookedFoliageCluster &cluster,
                                       const FoliageTypeDefinitionData &definition, Budget &budget, const CancellationToken &cancellation) {
            FoliageInstanceId previous{};
            for (const auto &instance : cluster.instances) {
                auto step = budget.Step(request, cancellation);
                if (!step.HasValue())
                    return step;
                if (!ValidInstance(instance, cluster, definition) || (previous.IsValid() && previous >= instance.id))
                    return Result<void>::Failure(Failure(request, TerrainProducerErrors::Invalid));
                previous = instance.id;
            }
            return Result<void>::Success();
        }

        /** @brief Selects only the exact manifest requirement for the already validated consumer. */
        TerrainPayloadRequirement Requirement(const TerrainProducerSnapshotRequest &request) noexcept {
            const auto requirements = request.manifest->FoliageRequirements();
            return request.header.consumer == TerrainProducerConsumer::Collision ? requirements.collision : requirements.navigation;
        }

        /** @brief Admits one complete cluster and exact definition before deciding its projected contribution count. */
        Result<std::uint64_t> AdmitCluster(const TerrainProducerSnapshotRequest &request, const CookedFoliageCluster &cluster,
                                           Budget &budget, const CancellationToken &cancellation) {
            if (cluster.tile.dataset != request.header.terrain.dataset || cluster.capability != request.header.revision.capability)
                return Result<std::uint64_t>::Failure(Failure(request, TerrainProducerErrors::Stale));
            if (!Charge(budget.instances, cluster.instances.size(), budget.limits.maximumInstances))
                return Result<std::uint64_t>::Failure(Failure(request, TerrainProducerErrors::Limit));
            auto verified = VerifyPayload(request, cluster.payload, cluster.digest, budget, cancellation);
            if (!verified.HasValue())
                return Result<std::uint64_t>::Failure(verified.ErrorValue());
            auto definition = Definition(request, cluster, budget, cancellation);
            if (!definition.HasValue())
                return Result<std::uint64_t>::Failure(definition.ErrorValue());
            const bool relevant = Relevant(*definition.Value(), request.header.consumer);
            if (relevant && Requirement(request) == TerrainPayloadRequirement::NotRequested)
                return Result<std::uint64_t>::Failure(Failure(request, TerrainProducerErrors::Unavailable));
            auto instances = ValidateInstances(request, cluster, *definition.Value(), budget, cancellation);
            if (!instances.HasValue())
                return Result<std::uint64_t>::Failure(instances.ErrorValue());
            return Result<std::uint64_t>::Success(relevant ? cluster.instances.size() : 0);
        }
    }  // namespace

    /** @copydoc AdmitFoliage */
    Result<std::uint64_t> AdmitFoliage(const TerrainProducerSnapshotRequest &request,
                                       const std::span<const CookedFoliageCluster *const> clusters, Budget &budget,
                                       const CancellationToken &cancellation) {
        std::uint64_t outputCount{};
        for (const auto *cluster : clusters) {
            auto admitted = AdmitCluster(request, *cluster, budget, cancellation);
            if (!admitted.HasValue())
                return admitted;
            outputCount += admitted.Value();
        }
        if (!budget.Bytes(outputCount * sizeof(TerrainProducerFoliage)))
            return Result<std::uint64_t>::Failure(Failure(request, TerrainProducerErrors::Limit));
        return Result<std::uint64_t>::Success(outputCount);
    }

    /** @copydoc CopyFoliage */
    Result<void> CopyFoliage(const TerrainProducerSnapshotRequest &request, const std::span<const CookedFoliageCluster *const> clusters,
                             std::vector<TerrainProducerFoliage> &output, Budget &budget, const CancellationToken &cancellation) {
        for (const auto *cluster : clusters) {
            auto definition = Definition(request, *cluster, budget, cancellation);
            if (!definition.HasValue())
                return Result<void>::Failure(definition.ErrorValue());
            if (!Relevant(*definition.Value(), request.header.consumer))
                continue;
            for (const auto &instance : cluster->instances) {
                auto step = budget.Step(request, cancellation);
                if (!step.HasValue())
                    return step;
                output.push_back({cluster->tile, cluster->id, cluster->type, cluster->sourceRevision, cluster->placementRevision,
                                  cluster->definitionRevision, cluster->digest, cluster->placementFingerprint, cluster->placementDigest,
                                  cluster->geometryDigest, cluster->profileFingerprint, cluster->bounds, cluster->geometryRadiusMillimeters,
                                  instance, definition.Value()->collision, definition.Value()->placement.alignment});
            }
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain::ProducerDetail
