#include "VfxWorldSpawnInternal.h"

#include <new>
#include <stdexcept>

namespace Horo::Vfx::SpawnDetail {
    namespace {
        template <typename Value> [[nodiscard]] Result<Value> Reject(const ErrorCodeDescriptor &code) {
            return Result<Value>::Failure(MakeError(code));
        }

        /** @brief Reserves a conservative bounded kernel envelope; actual allocation is checked before publication. */
        [[nodiscard]] std::uint64_t KernelReservation(const VfxSpawnAssetPreparation &source) noexcept {
            const auto particles = source.particles->Data().maximumParticles;
            return 256U * 1024U + static_cast<std::uint64_t>(particles) * 2048U +
                   static_cast<std::uint64_t>(source.simulation.sceneDepth.samples.size()) * sizeof(CpuParticleDepthSample);
        }

        /** @brief Rejects unprepared native/collision/mesh paths explicitly rather than selecting a substitute. */
        [[nodiscard]] bool SupportedRecipe(const VfxSpawnAssetPreparation &source) noexcept {
            const auto &data = source.particles->Data();
            return data.simulationPreference == SimulationPreference::RequireCPU &&
                   data.collisionMode != ParticleCollisionMode::PhysicsWorld && data.collisionMode != ParticleCollisionMode::SceneDepth &&
                   data.renderMode == ParticleRenderMode::Billboard && data.sortMode == ParticleSortMode::None &&
                   source.simulation.stageObserver == nullptr && source.materialBlend == Render::MaterialAlphaMode::Additive;
        }

        /** @brief Validates positive explicit effect lifetime and bounded per-asset demand. */
        [[nodiscard]] bool ValidAsset(const VfxSpawnAssetPreparation &source) noexcept {
            return source.asset.IsValid() && source.particles && source.material.IsValid() && source.maximumInstances != 0 &&
                   source.requiredReserve <= source.maximumInstances && std::isfinite(source.durationSeconds) &&
                   source.durationSeconds > 0.0F && source.durationSeconds <= 60.0F;
        }

        /** @brief Copies only genuine declared GameplayInput scalar channels into the public recipe schema. */
        [[nodiscard]] Result<void> CopySchema(Detail::VfxSpawnAsset &asset, const VfxSpawnAssetPreparation &source) {
            if (source.simulation.payloadChannels.size() > CpuParticleSimulationHardLimits::PayloadChannels)
                return Reject<void>(VfxErrors::SpawnPreparationInvalid);
            asset.info.asset = source.asset;
            asset.info.parameterSchema = source.parameterSchema;
            asset.info.maximumInstances = source.maximumInstances;
            asset.info.requiredReserve = source.requiredReserve;
            asset.info.requiredGameplay = source.simulation.requiredGameplay;
            for (const auto &channel : source.simulation.payloadChannels) {
                if (channel.classification != CpuParticlePayloadClass::GameplayInput)
                    continue;
                if (asset.info.parameterCount == VfxSpawnHardLimits::Parameters)
                    return Reject<void>(VfxErrors::SpawnPreparationInvalid);
                asset.info.parameters[asset.info.parameterCount++] = {channel.channel, channel.minimum, channel.maximum};
            }
            if ((asset.info.parameterCount == 0) != (source.parameterSchema == 0))
                return Reject<void>(VfxErrors::SpawnPreparationInvalid);
            return Result<void>::Success();
        }

        /** @brief Materializes a disjoint range of genuine canonical simulators before making any recipe visible. */
        [[nodiscard]] Result<void> PrepareInstances(Detail::VfxSpawnAsset &asset, const VfxSpawnAssetPreparation &source,
                                                    const VfxIdentityScope scene, const std::uint64_t perInstanceBytes) {
            asset.instances.reserve(source.maximumInstances);
            for (std::uint32_t index = 0; index < source.maximumInstances; ++index) {
                auto info = source.simulation;
                info.activation = {scene, asset.firstSlot + index, 1};
                info.buffer = {scene, asset.firstSlot + index, 1};
                info.maximumBufferBytes = static_cast<std::size_t>(perInstanceBytes / 2U);
                auto simulation = CpuParticleSimulator::Create(*source.particles, info);
                if (simulation.HasError())
                    return Result<void>::Failure(simulation.ErrorValue());
                if (simulation.Value().PreparedBytes() > perInstanceBytes)
                    return Reject<void>(VfxErrors::SpawnPreparationBudgetExceeded);
                asset.instances.emplace_back(std::move(simulation).Value(), info.activation);
            }
            return Result<void>::Success();
        }

        /** @brief Charges one pool and its entire pre-materialized payload to the remaining scene envelope. */
        [[nodiscard]] Result<EffectInstancePool> PreparePool(const VfxSpawnAssetPreparation &source, const VfxIdentityScope scene,
                                                             const std::uint32_t firstSlot, const std::uint64_t remainingBytes) {
            const EffectPoolDescriptor descriptor{scene, source.maximumInstances, 1, KernelReservation(source), firstSlot};
            const EffectPoolBudget budget{source.maximumInstances, source.maximumInstances, remainingBytes, source.requiredReserve, 0};
            auto pool = EffectInstancePool::Prepare(descriptor, budget, {});
            if (pool.HasError())
                return pool;
            if (pool.Value().Statistics().plan.capacity != source.maximumInstances)
                return Reject<EffectInstancePool>(VfxErrors::SpawnPreparationBudgetExceeded);
            return pool;
        }

        /** @brief Prepares one immutable asset slot range; caller publishes it only after this succeeds. */
        [[nodiscard]] Result<Detail::VfxSpawnAsset> PrepareAsset(const VfxSpawnAssetPreparation &source, const VfxIdentityScope scene,
                                                                 const std::uint32_t firstSlot, const std::uint64_t remainingBytes,
                                                                 const float maximumDeltaSeconds) {
            if (!ValidAsset(source))
                return Reject<Detail::VfxSpawnAsset>(VfxErrors::SpawnPreparationInvalid);
            if (source.initialBurst > source.simulation.maximumBurstParticles ||
                source.simulation.maximumDeltaSeconds < maximumDeltaSeconds)
                return Reject<Detail::VfxSpawnAsset>(VfxErrors::SpawnPreparationInvalid);
            if (!SupportedRecipe(source))
                return Reject<Detail::VfxSpawnAsset>(VfxErrors::SpawnPreparationUnsupported);
            auto pool = PreparePool(source, scene, firstSlot, remainingBytes);
            if (pool.HasError())
                return Result<Detail::VfxSpawnAsset>::Failure(pool.ErrorValue());
            Detail::VfxSpawnAsset asset{std::move(pool).Value()};
            asset.firstSlot = firstSlot;
            asset.initialBurst = source.initialBurst;
            asset.durationSeconds = source.durationSeconds;
            asset.material = source.material;
            asset.emitter = source.particles->Data().emitter;
            asset.requiredGameplay = source.simulation.requiredGameplay;
            if (auto copied = CopySchema(asset, source); copied.HasError())
                return Result<Detail::VfxSpawnAsset>::Failure(copied.ErrorValue());
            if (auto ready = PrepareInstances(asset, source, scene, KernelReservation(source)); ready.HasError())
                return Result<Detail::VfxSpawnAsset>::Failure(ready.ErrorValue());
            return Result<Detail::VfxSpawnAsset>::Success(std::move(asset));
        }

        /** @brief Rejects aggregate demand before allocating any simulator, including checked disjoint identity ranges. */
        [[nodiscard]] bool FitsAggregate(const VfxSpawnAssetPreparation &source, const VfxSpawnBudget budget, const std::uint64_t instances,
                                         const std::uint64_t particles) noexcept {
            if (!ValidAsset(source))
                return false;
            return instances + source.maximumInstances <= budget.maximumInstances &&
                   particles + static_cast<std::uint64_t>(source.maximumInstances) * source.particles->Data().maximumParticles <=
                       budget.maximumParticles;
        }

        /** @brief Preallocates queue/results/authority records and charges actual vector capacities. */
        [[nodiscard]] std::unique_ptr<Detail::VfxWorldSpawnState> PrepareStorage(const VfxSpawnBudget budget,
                                                                                 const std::size_t assetCount) {
            auto state = std::make_unique<Detail::VfxWorldSpawnState>();
            state->budget = budget;
            state->pending.resize(budget.maximumPending);
            state->results.resize(budget.maximumResults);
            state->owners.resize(budget.maximumOwners);
            state->producers.resize(budget.maximumProducers);
            state->assets.reserve(assetCount);
            state->preparedBytes =
                sizeof(*state) + state->pending.capacity() * sizeof(VfxSpawnRequest) + state->results.capacity() * sizeof(VfxSpawnOutcome) +
                state->owners.capacity() * sizeof(Detail::VfxSpawnOwner) + state->producers.capacity() * sizeof(Detail::VfxSpawnProducer) +
                state->assets.capacity() * sizeof(Detail::VfxSpawnAsset);
            return state;
        }

        /** @brief Checks one finite aggregate resource envelope before materialization. */
        [[nodiscard]] bool ValidCounts(const VfxSpawnBudget budget) noexcept {
            return budget.maximumInstances > 0 && budget.maximumInstances <= 256 && budget.maximumParticles > 0 &&
                   budget.maximumParticles <= 16'384;
        }

        [[nodiscard]] bool ValidResources(const VfxSpawnBudget budget) noexcept {
            return ValidCounts(budget) && budget.maximumBytes > 0 && budget.maximumBytes <= CpuParticleBufferHardLimits::Bytes &&
                   std::isfinite(budget.maximumDeltaSeconds) && budget.maximumDeltaSeconds > 0.0F && budget.maximumDeltaSeconds <= 60.0F;
        }

        /** @brief Cold candidate construction; every failure destroys only unpublished ownership. */
        [[nodiscard]] Result<std::unique_ptr<Detail::VfxWorldSpawnState>> PrepareCandidate(
            const VfxIdentityScope scene, const std::span<const VfxSpawnAssetPreparation> assets, const VfxSpawnBudget budget) {
            auto state = PrepareStorage(budget, assets.size());
            std::uint64_t instances{}, particles{};
            for (const auto &source : assets) {
                if (!ValidAsset(source))
                    return Reject<std::unique_ptr<Detail::VfxWorldSpawnState>>(VfxErrors::SpawnPreparationInvalid);
                if (!FitsAggregate(source, budget, instances, particles) || state->preparedBytes >= budget.maximumBytes)
                    return Reject<std::unique_ptr<Detail::VfxWorldSpawnState>>(VfxErrors::SpawnPreparationBudgetExceeded);
                if (FindAsset(*state, source.asset))
                    return Reject<std::unique_ptr<Detail::VfxWorldSpawnState>>(VfxErrors::SpawnPreparationInvalid);
                auto asset = PrepareAsset(source, scene, static_cast<std::uint32_t>(instances), budget.maximumBytes - state->preparedBytes,
                                          budget.maximumDeltaSeconds);
                if (asset.HasError())
                    return Result<std::unique_ptr<Detail::VfxWorldSpawnState>>::Failure(asset.ErrorValue());
                state->preparedBytes += asset.Value().pool.Statistics().plan.reservedBytes;
                state->preparedBytes += asset.Value().instances.capacity() * sizeof(Detail::VfxSpawnInstance);
                if (state->preparedBytes > budget.maximumBytes)
                    return Reject<std::unique_ptr<Detail::VfxWorldSpawnState>>(VfxErrors::SpawnPreparationBudgetExceeded);
                instances += source.maximumInstances;
                particles += static_cast<std::uint64_t>(source.maximumInstances) * source.particles->Data().maximumParticles;
                state->assets.push_back(std::move(asset).Value());
            }
            return Result<std::unique_ptr<Detail::VfxWorldSpawnState>>::Success(std::move(state));
        }
    }  // namespace

    VfxSpawnStatus ValidateBudget(const VfxSpawnBudget budget) noexcept {
        const bool queues = budget.maximumPending > 0 && budget.maximumPending <= VfxSpawnHardLimits::Pending &&
                            budget.maximumResults > 0 && budget.maximumResults <= VfxSpawnHardLimits::Results;
        if (!queues)
            return VfxSpawnStatus::InvalidRequest;
        const bool owners = budget.maximumOwners > 0 && budget.maximumOwners <= VfxSpawnHardLimits::Producers &&
                            budget.maximumProducers > 0 && budget.maximumProducers <= VfxSpawnHardLimits::Producers;
        return owners ? VfxSpawnStatus::Accepted : VfxSpawnStatus::InvalidRequest;
    }

    Result<std::unique_ptr<Detail::VfxWorldSpawnState>> Prepare(const VfxIdentityScope scene,
                                                                const std::span<const VfxSpawnAssetPreparation> assets,
                                                                const VfxSpawnBudget budget) {
        if (ValidateBudget(budget) != VfxSpawnStatus::Accepted || assets.empty() || assets.size() > VfxSpawnHardLimits::Effects)
            return Reject<std::unique_ptr<Detail::VfxWorldSpawnState>>(VfxErrors::SpawnPreparationInvalid);
        if (!ValidResources(budget))
            return Reject<std::unique_ptr<Detail::VfxWorldSpawnState>>(VfxErrors::SpawnPreparationInvalid);
        try {
            return PrepareCandidate(scene, assets, budget);
        } catch (const std::bad_alloc &) {
            return Reject<std::unique_ptr<Detail::VfxWorldSpawnState>>(VfxErrors::SpawnPreparationAllocationFailed);
        } catch (const std::length_error &) {
            return Reject<std::unique_ptr<Detail::VfxWorldSpawnState>>(VfxErrors::SpawnPreparationAllocationFailed);
        }
    }
}  // namespace Horo::Vfx::SpawnDetail
