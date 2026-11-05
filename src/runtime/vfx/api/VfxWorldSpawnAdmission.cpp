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
            const auto found = std::ranges::find(state.owners, request.owner, &Detail::VfxSpawnOwner::owner);
            if (found == state.owners.end() || found->generation != request.ownerGeneration)
                return VfxSpawnStatus::StaleOwner;
            return found->active ? VfxSpawnStatus::Accepted : VfxSpawnStatus::Cancelled;
        }

        /** @brief Validates all representation and prepared schema facts before queue or dedup state changes. */
        [[nodiscard]] VfxSpawnStatus ValidateOne(Detail::VfxWorldSpawnState &state, const VfxIdentityScope scene,
                                                 const VfxSpawnRequest &request) noexcept {
            if (const auto valid = ValidateVfxSpawnRequest(request); valid != VfxSpawnStatus::Accepted)
                return valid;
            if (request.scene != scene)
                return VfxSpawnStatus::StaleScene;
            if (const auto valid = OwnerStatus(state, request); valid != VfxSpawnStatus::Accepted)
                return valid;
            const auto asset = FindAsset(state, request.effectAsset);
            if (!asset)
                return VfxSpawnStatus::MissingAsset;
            if (request.requirement == VfxRequirementClass::GameplayRequired &&
                (!asset->requiredGameplay || asset->info.requiredReserve == 0))
                return VfxSpawnStatus::PermissionDenied;
            return ParametersMatch(request.parameters, asset->info) ? VfxSpawnStatus::Accepted : VfxSpawnStatus::ParameterSchemaMismatch;
        }

        [[nodiscard]] VfxSpawnStatus ValidateBatch(Detail::VfxWorldSpawnState &state, const VfxIdentityScope scene,
                                                   const std::span<const VfxSpawnRequest> requests) noexcept {
            for (std::size_t layer = 0; layer < requests.size(); ++layer) {
                const auto &request = requests[layer];
                if (!SameResolution(requests.front(), request) || request.layerOrdinal != layer)
                    return VfxSpawnStatus::InvalidRequest;
                if (const auto valid = ValidateOne(state, scene, request); valid != VfxSpawnStatus::Accepted)
                    return valid;
            }
            return VfxSpawnStatus::Accepted;
        }

        /** @brief Admission reserves both queue storage and one eventual terminal result per layer. */
        [[nodiscard]] VfxSpawnStatus Credits(const Detail::VfxWorldSpawnState &state, const std::size_t layers) noexcept {
            if (state.pendingCount + layers > state.pending.size())
                return VfxSpawnStatus::EventQueueFull;
            if (state.resultCount + state.pendingCount + layers > state.results.size())
                return VfxSpawnStatus::ResultQueueFull;
            return VfxSpawnStatus::Accepted;
        }

        /** @brief Finds a permanent source watermark or unused prepared producer record without publishing it. */
        [[nodiscard]] VfxSpawnStatus ResolveProducer(Detail::VfxWorldSpawnState &state, const VfxSpawnRequest &request,
                                                     Detail::VfxSpawnProducer *&output) noexcept {
            for (auto &producer : state.producers) {
                if (producer.producer != request.occurrence.producer)
                    continue;
                if (producer.owner != request.owner)
                    return VfxSpawnStatus::PermissionDenied;
                if (producer.sequence >= request.occurrence.sequence)
                    return VfxSpawnStatus::Duplicate;
                output = &producer;
                return VfxSpawnStatus::Accepted;
            }
            const auto free = std::ranges::find(state.producers, std::uint64_t{}, &Detail::VfxSpawnProducer::producer);
            if (free == state.producers.end())
                return VfxSpawnStatus::ResourceBudgetExceeded;
            output = &*free;
            return VfxSpawnStatus::Accepted;
        }
    }  // namespace

    VfxSpawnOutcome Enqueue(Detail::VfxWorldSpawnState &state, const VfxIdentityScope scene,
                            const std::span<const VfxSpawnRequest> requests) noexcept {
        if (requests.empty() || requests.size() > VfxSpawnHardLimits::Layers)
            return {.status = VfxSpawnStatus::InvalidRequest};
        const auto &first = requests.front();
        if (const auto valid = ValidateBatch(state, scene, requests); valid != VfxSpawnStatus::Accepted)
            return MakeVfxSpawnOutcome(first, valid);
        Detail::VfxSpawnProducer *producer{};
        if (const auto valid = ResolveProducer(state, first, producer); valid != VfxSpawnStatus::Accepted)
            return MakeVfxSpawnOutcome(first, valid);
        if (first.tick <= state.tick)
            return MakeVfxSpawnOutcome(first, VfxSpawnStatus::OutOfOrder);
        if (const auto valid = Credits(state, requests.size()); valid != VfxSpawnStatus::Accepted)
            return MakeVfxSpawnOutcome(first, valid);
        for (const auto &request : requests)
            state.pending[state.pendingCount++] = request;
        *producer = {first.owner, first.occurrence.producer, first.occurrence.sequence};
        return MakeVfxSpawnOutcome(first, VfxSpawnStatus::Accepted);
    }
}  // namespace Horo::Vfx::SpawnDetail
