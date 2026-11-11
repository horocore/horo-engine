#include "VfxWorldSpawnInternal.h"

#include <tuple>

namespace Horo::Vfx::SpawnDetail {
    namespace {
        /** @brief Freezes canonical source/layer order independently of worker arrival and asset lookup order. */
        [[nodiscard]] bool RequestLess(const VfxSpawnRequest &left, const VfxSpawnRequest &right) noexcept {
            return std::tie(left.tick, left.occurrence.producer, left.occurrence.sequence, left.destination, left.layerOrdinal) <
                   std::tie(right.tick, right.occurrence.producer, right.occurrence.sequence, right.destination, right.layerOrdinal);
        }

        /** @brief Rechecks retirement at consumption; revocation never requires mutating already accepted commands. */
        [[nodiscard]] bool OwnerActive(const Detail::VfxWorldSpawnState &state, const VfxSpawnRequest &request) noexcept {
            const auto owner = std::ranges::find(state.owners, request.owner, &Detail::VfxSpawnOwner::owner);
            return owner != state.owners.end() && owner->active && owner->generation == request.ownerGeneration;
        }

        /** @brief Applies only prevalidated declared GameplayInput scalars before the kernel cutoff. */
        [[nodiscard]] bool ApplyParameters(Detail::VfxSpawnInstance &instance) noexcept {
            for (const auto &value : instance.request.parameters.Values())
                if (instance.simulation.SubmitGameplayInput(value.channel, value.value).HasError())
                    return false;
            return true;
        }

        /** @brief Binds a genuinely pool-issued activation to its preallocated same-slot CPU kernel. */
        [[nodiscard]] VfxSpawnOutcome Activate(Detail::VfxSpawnAsset &asset, const VfxSpawnRequest &request) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            const auto admitted = asset.pool.Play({request.owner, request.ownerGeneration, request.tick, request.requirement});
            if (admitted.status != EffectPoolStatus::Admitted)
                return MakeVfxSpawnOutcome(request, EffectCapacityExceeded);
            auto &instance = asset.instances[admitted.instance.slot - asset.firstSlot];
            const bool same = instance.instance == admitted.instance;
            const bool reset = same || instance.simulation.Reset(admitted.instance) == CpuParticleResetStatus::Complete;
            instance.instance = admitted.instance;
            if (!reset) {
                Retire(asset, instance);
                return MakeVfxSpawnOutcome(request, SimulationFailed);
            }
            instance.request = request;
            instance.elapsed = 0.0F;
            instance.firstStep = true;
            instance.live = true;
            if (!ApplyParameters(instance)) {
                Retire(asset, instance);
                return MakeVfxSpawnOutcome(request, ParameterSchemaMismatch);
            }
            return MakeVfxSpawnOutcome(request, Spawned, admitted.instance);
        }

        /** @brief Emits exactly one reserved terminal admission result for each consumed request. */
        [[nodiscard]] VfxSpawnOutcome Consume(Detail::VfxWorldSpawnState &state, const VfxSpawnRequest &request) noexcept {
            if (!OwnerActive(state, request))
                return MakeVfxSpawnOutcome(request, VfxSpawnStatus::Cancelled);
            auto *asset = FindAsset(state, request.effectAsset);
            if (!asset)
                return MakeVfxSpawnOutcome(request, VfxSpawnStatus::MissingAsset);
            return Activate(*asset, request);
        }

        /** @brief Preserves the first exact failure while retaining bounded aggregate counts. */
        void RecordFailure(VfxSpawnUpdate &report, const VfxSpawnOutcome &outcome) noexcept {
            if (outcome.status == VfxSpawnStatus::Spawned)
                return;
            if (report.firstFailure.status == VfxSpawnStatus::Empty)
                report.firstFailure = outcome;
            report.status = outcome.status;
        }

        /** @brief Executes the same prepared canonical stages while returning exact static failure evidence. */
        [[nodiscard]] CpuParticleAdvanceOutcome AdvanceKernel(CpuParticleSimulator &simulation,
                                                              const CpuParticleSimulationStep step) noexcept {
            return simulation.AdvancePrepared(step);
        }

        /** @brief Corrects an initial activation result before the same owner cutoff publishes terminal evidence. */
        void RecordKernelFailure(Detail::VfxWorldSpawnState &state, const Detail::VfxSpawnInstance &instance,
                                 const ErrorCodeDescriptor *descriptor, VfxSpawnUpdate &report) noexcept {
            auto failure = MakeVfxSpawnOutcome(instance.request, VfxSpawnStatus::SimulationFailed, instance.instance);
            failure.kernelFailure = descriptor;
            RecordFailure(report, failure);
            if (!instance.firstStep)
                return;
            for (auto &result : std::span{state.results.data(), state.resultCount})
                if (result.instance == instance.instance)
                    result = failure;
        }

        /** @brief Advances the one canonical CPU owner; no proxy or extraction-driven simulation is involved. */
        void AdvanceInstance(Detail::VfxWorldSpawnState &state, Detail::VfxSpawnAsset &asset, Detail::VfxSpawnInstance &instance,
                             const float deltaSeconds, VfxSpawnUpdate &report) noexcept {
            const CpuParticleSimulationStep step{deltaSeconds, instance.firstStep ? asset.initialBurst : 0,
                                                 instance.simulation.Statistics().steps, false};
            const auto advanced = AdvanceKernel(instance.simulation, step);
            if (advanced.status != CpuParticleAdvanceStatus::Complete) {
                RecordKernelFailure(state, instance, advanced.failure, report);
                Retire(asset, instance);
                ++report.retired;
                return;
            }
            instance.firstStep = false;
            instance.elapsed += deltaSeconds;
            if (instance.elapsed >= asset.durationSeconds) {
                Retire(asset, instance);
                ++report.retired;
                return;
            }
            ++report.live;
        }

        /** @brief Commits the frozen prefix and leaves future-tick requests in the original bounded storage. */
        void Drain(Detail::VfxWorldSpawnState &state, const std::uint64_t tick, VfxSpawnUpdate &report) noexcept {
            std::sort(state.pending.begin(), state.pending.begin() + state.pendingCount, RequestLess);
            std::uint32_t consumed{};
            while (consumed < state.pendingCount && state.pending[consumed].tick <= tick) {
                const auto outcome = Consume(state, state.pending[consumed]);
                state.results[state.resultCount++] = outcome;
                RecordFailure(report, outcome);
                ++consumed;
            }
            std::move(state.pending.begin() + consumed, state.pending.begin() + state.pendingCount, state.pending.begin());
            state.pendingCount -= consumed;
            report.consumed = consumed;
        }

        /** @brief Busy admission closes reentrant submission for the entire owner update. */
        struct UpdateScope final {
            explicit UpdateScope(bool &value) : busy(value) {
                busy = true;
            }

            UpdateScope(const UpdateScope &) = delete;
            UpdateScope &operator=(const UpdateScope &) = delete;

            ~UpdateScope() {
                busy = false;
            }

            bool &busy;
        };
    }  // namespace

    void Retire(Detail::VfxSpawnAsset &asset, Detail::VfxSpawnInstance &instance) noexcept {
        instance.live = false;
        static_cast<void>(asset.pool.Stop(instance.instance));
        static_cast<void>(asset.pool.CompleteRetirement(instance.instance));
    }

    VfxSpawnUpdate Update(Detail::VfxWorldSpawnState &state, const std::uint64_t tick, const float deltaSeconds) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (tick == 0 || tick <= state.tick)
            return {.status = OutOfOrder};
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F || deltaSeconds > state.budget.maximumDeltaSeconds)
            return {.status = InvalidRequest};
        const UpdateScope scope{state.busy};
        VfxSpawnUpdate report{.status = Accepted};
        state.tick = tick;
        Drain(state, tick, report);
        for (auto &asset : state.assets)
            for (auto &instance : asset.instances)
                if (instance.live)
                    AdvanceInstance(state, asset, instance, deltaSeconds, report);
        return report;
    }
}  // namespace Horo::Vfx::SpawnDetail
