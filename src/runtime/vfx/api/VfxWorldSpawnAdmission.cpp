#include "VfxWorldSpawnInternal.h"

#include <tuple>

namespace Horo::Vfx::SpawnDetail {
    namespace {
        /** @brief Confirms every layer belongs to one captured semantic resolution, independent of chosen effect assets. */
        [[nodiscard]] bool SameResolution(const VfxSpawnRequest &left, const VfxSpawnRequest &right) noexcept {
            return std::tie(left.scene, left.owner, left.ownerGeneration, left.tick, left.occurrence, left.bindingGeneration,
                            left.destination, left.requirement) == std::tie(right.scene, right.owner, right.ownerGeneration, right.tick,
                                                                            right.occurrence, right.bindingGeneration, right.destination,
                                                                            right.requirement);
        }

        /** @brief Checks exact admitted ownership without interpreting any semantic event. */
        [[nodiscard]] VfxSpawnStatus OwnerStatus(const Detail::VfxWorldSpawnState &state, const VfxSpawnRequest &request) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            const auto found = std::ranges::find(state.owners, request.owner, &Detail::VfxSpawnOwner::owner);
            if (found == state.owners.end() || found->generation != request.ownerGeneration)
                return StaleOwner;
            return found->active ? Accepted : Cancelled;
        }

        /** @brief Rejects mesh placement that the existing sizeXY/local-Z-rotation representation cannot express. */
        [[nodiscard]] bool SupportedPlacement(const Detail::VfxSpawnAsset &asset, const VfxSpawnRequest &request) noexcept {
            if (asset.topology != Render::VfxPrimitiveTopology::InstancedMesh)
                return true;
            const auto &rotation = request.transform.orientation;
            return rotation.x == 0.F && rotation.y == 0.F && rotation.z == 0.F && request.transform.scale.z * request.scale == 1.F;
        }

        /** @brief Validates all representation and prepared schema facts before queue or dedup state changes. */
        [[nodiscard]] VfxSpawnStatus ValidateOne(Detail::VfxWorldSpawnState &state, const VfxIdentityScope scene,
                                                 const VfxSpawnRequest &request) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            if (const auto valid = ValidateVfxSpawnRequest(request); valid != Accepted)
                return valid;
            if (request.scene != scene)
                return StaleScene;
            if (const auto valid = OwnerStatus(state, request); valid != Accepted)
                return valid;
            const auto asset = FindAsset(state, request.effectAsset);
            if (!asset)
                return MissingAsset;
            if (request.requirement == VfxRequirementClass::GameplayRequired &&
                (!asset->requiredGameplay || asset->info.requiredReserve == 0))
                return PermissionDenied;
            if (!SupportedPlacement(*asset, request))
                return UnsupportedRecipe;
            return ParametersMatch(request.parameters, asset->info) ? Accepted : ParameterSchemaMismatch;
        }

        [[nodiscard]] VfxSpawnStatus ValidateBatch(Detail::VfxWorldSpawnState &state, const VfxIdentityScope scene,
                                                   const std::span<const VfxSpawnRequest> requests) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            for (std::size_t layer = 0; layer < requests.size(); ++layer) {
                const auto &request = requests[layer];
                if (!SameResolution(requests.front(), request) || request.layerOrdinal != layer)
                    return InvalidRequest;
                if (const auto valid = ValidateOne(state, scene, request); valid != Accepted)
                    return valid;
            }
            return Accepted;
        }

        /** @brief Admission reserves both queue storage and one eventual terminal result per layer. */
        [[nodiscard]] VfxSpawnStatus Credits(const Detail::VfxWorldSpawnState &state, const std::size_t layers) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            if (state.pendingCount + layers > state.pending.size())
                return EventQueueFull;
            if (state.resultCount + state.pendingCount + layers > state.results.size())
                return ResultQueueFull;
            return Accepted;
        }

        /** @brief Finds a permanent source watermark or unused prepared producer record without publishing it. */
        [[nodiscard]] VfxSpawnStatus ResolveProducer(Detail::VfxWorldSpawnState &state, const VfxSpawnRequest &request,
                                                     Detail::VfxSpawnProducer *&output) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            for (auto &producer : state.producers) {
                if (producer.producer != request.occurrence.producer)
                    continue;
                if (producer.owner != request.owner)
                    return PermissionDenied;
                if (producer.sequence >= request.occurrence.sequence)
                    return Duplicate;
                output = &producer;
                return Accepted;
            }
            const auto free = std::ranges::find(state.producers, std::uint64_t{}, &Detail::VfxSpawnProducer::producer);
            if (free == state.producers.end())
                return ResourceBudgetExceeded;
            output = &*free;
            return Accepted;
        }
    }  // namespace

    VfxSpawnOutcome Enqueue(Detail::VfxWorldSpawnState &state, const VfxIdentityScope scene,
                            const std::span<const VfxSpawnRequest> requests) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (requests.empty() || requests.size() > VfxSpawnHardLimits::Layers)
            return {.status = InvalidRequest};
        const auto &first = requests.front();
        if (const auto valid = ValidateBatch(state, scene, requests); valid != Accepted)
            return MakeVfxSpawnOutcome(first, valid);
        Detail::VfxSpawnProducer *producer{};
        if (const auto valid = ResolveProducer(state, first, producer); valid != Accepted)
            return MakeVfxSpawnOutcome(first, valid);
        if (first.tick <= state.tick)
            return MakeVfxSpawnOutcome(first, OutOfOrder);
        if (const auto valid = Credits(state, requests.size()); valid != Accepted)
            return MakeVfxSpawnOutcome(first, valid);
        for (const auto &request : requests)
            state.pending[state.pendingCount++] = request;
        *producer = {first.owner, first.occurrence.producer, first.occurrence.sequence};
        return MakeVfxSpawnOutcome(first, Accepted);
    }
}  // namespace Horo::Vfx::SpawnDetail
