#include "GameplayVfxContextInternal.h"

namespace Horo::Gameplay::VfxContextDetail {
    namespace {
        [[nodiscard]] GameplayVfxReceipt Receipt(const Detail::GameplayVfxSceneState &scene,
                                                 const Detail::GameplayVfxModuleState &moduleState) noexcept {
            return {scene.scene,
                    moduleState.grant.owner,
                    moduleState.grant.ownerGeneration,
                    {moduleState.grant.producer, moduleState.nextSequence}};
        }

        [[nodiscard]] GameplayVfxResolveOutcome Resolve(const Detail::GameplayVfxSceneState &scene,
                                                        const Detail::GameplayVfxModuleState &moduleState, const GameplayVfxEventKey key,
                                                        const GameplayVfxEventPayload &payload, const GameplayVfxReceipt &receipt,
                                                        const std::span<Vfx::VfxSpawnRequest> output) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            if (key.generation != scene.table->Generation())
                return {StaleBinding, 0};
            const auto &compiled = Detail::GameplayVfxBindingAccess::State(*scene.table);
            const auto *binding = VfxBindingDetail::FindDestination(compiled, key.destination);
            if (!binding)
                return {UnknownEvent, 0};
            if (!Allowed(moduleState, *binding))
                return {PermissionDenied, 0};
            return scene.table->Resolve(key, payload, Basis(scene, receipt), output);
        }
    }  // namespace

    GameplayVfxSubmission Stage(Detail::GameplayVfxSceneState &scene, Detail::GameplayVfxModuleState &moduleState,
                                const GameplayVfxEventKey key, const GameplayVfxEventPayload &payload,
                                std::shared_ptr<const Detail::GameplayVfxScopeLifetime> lifetime) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (!scene.sourceOpen)
            return {.status = PermissionDenied};
        if (scene.sealPrepared)
            return {.status = Reentrant};
        if (moduleState.nextSequence == std::numeric_limits<std::uint64_t>::max())
            return {.status = ResourceBudgetExceeded};
        const auto receipt = Receipt(scene, moduleState);
        std::array<Vfx::VfxSpawnRequest, Vfx::VfxSpawnHardLimits::Layers> scratch;
        const auto resolved = Resolve(scene, moduleState, key, payload, receipt, scratch);
        if (resolved.status != Accepted)
            return {.status = resolved.status};
        if (scene.pendingCount + scene.resultCount >= scene.pending.size())
            return {.status = EventQueueFull};
        scene.pending[scene.pendingCount++] = {scene.table, key, payload, receipt, scene.tick, false, std::move(lifetime)};
        ++moduleState.nextSequence;
        return {Accepted, receipt};
    }

    GameplayVfxSubmission EmitCommitted(Detail::GameplayVfxSceneState &scene, Detail::GameplayVfxModuleState &moduleState,
                                        const GameplayVfxEventKey key, const GameplayVfxEventPayload &payload,
                                        const std::uint64_t tick) noexcept {
        if (!scene.sourceOpen || tick != scene.tick || scene.sealPrepared)
            return {.status = Vfx::VfxSpawnStatus::OutOfOrder};
        const auto staged = Stage(scene, moduleState, key, payload);
        if (staged.status == Vfx::VfxSpawnStatus::Accepted)
            scene.pending[scene.pendingCount - 1].committed = true;
        return staged;
    }
}  // namespace Horo::Gameplay::VfxContextDetail

namespace Horo::Gameplay::Detail {
    namespace {
        /** @brief Blocks reentrant client commands throughout the real source commit and World update. */
        struct SourceScope final {
            explicit SourceScope(GameplayVfxSceneState &value) : scene(value) {
                scene.busy = true;
            }

            SourceScope(const SourceScope &) = delete;
            SourceScope &operator=(const SourceScope &) = delete;

            ~SourceScope() {
                scene.busy = false;
            }

            GameplayVfxSceneState &scene;
        };

        [[nodiscard]] Vfx::VfxSpawnStatus Current(const GameplayVfxSceneLease &lease) noexcept {
            return lease.state ? VfxContextDetail::Status(*lease.state) : Vfx::VfxSpawnStatus::StaleScene;
        }

        /** @brief Keeps attempted, sealed and actual World cutoffs monotonic while permitting an unsealed same-tick retry. */
        bool NewSourceTick(const GameplayVfxSceneState &scene, const std::uint64_t tick) noexcept {
            return tick != 0 && tick >= scene.tick && tick > scene.lastCommittedTick && tick > scene.lastSealedTick;
        }

        /** @brief Resolves only after the source owner confirms its actual successful Scene tick. */
        void SubmitPending(GameplayVfxSceneState &scene, const GameplayVfxPending &pending, GameplayVfxSourceRecord &record,
                           GameplayVfxTickResult &report) noexcept {
            record = {.receipt = pending.receipt};
            if (pending.lifetime && VfxContextDetail::ScopeRevoked(pending.lifetime)) {
                record.result.status = Vfx::VfxSpawnStatus::Cancelled;
                ++report.rejected;
                return;
            }
            std::array<Vfx::VfxSpawnRequest, Vfx::VfxSpawnHardLimits::Layers> scratch;
            auto basis = VfxContextDetail::Basis(scene, pending.receipt);
            basis.tick = pending.tick;
            const auto resolved = pending.table->Resolve(pending.key, pending.payload, basis, scratch);
            if (resolved.status != Vfx::VfxSpawnStatus::Accepted) {
                record.result.status = resolved.status;
                ++report.rejected;
                return;
            }
            const auto accepted = scene.world->TryEnqueueSpawns(std::span{scratch.data(), resolved.layers});
            record.result.status = accepted.status;
            record.result.pending = accepted.status == Vfx::VfxSpawnStatus::Accepted;
            if (record.result.pending)
                ++report.submitted;
            else
                ++report.rejected;
        }

        /** @brief Copies actual terminal layers, without equating source staging or queue admission with spawning. */
        void CollectResult(const GameplayVfxSceneState &scene, GameplayVfxSourceRecord &record) noexcept {
            using enum Horo::Vfx::VfxSpawnStatus;
            if (!record.result.pending)
                return;
            for (const auto &outcome : scene.world->SpawnResults()) {
                if (outcome.occurrence != record.receipt.occurrence)
                    continue;
                if (record.result.layerCount == record.result.layers.size())
                    break;
                record.result.layers[record.result.layerCount++] = outcome;
                if (outcome.status != Spawned)
                    record.result.status = outcome.status;
            }
            if (record.result.layerCount != 0) {
                record.result.pending = false;
                if (record.result.status == Accepted)
                    record.result.status = Spawned;
            }
        }

        /** @brief Completes only this cutoff; later sealed semantic commands retain their captured immutable policy. */
        void CompleteCutoff(GameplayVfxSceneState &scene, const GameplayVfxPending &pending, const bool sourceCommitted,
                            GameplayVfxTickResult &report) noexcept {
            auto &record = scene.results[scene.resultCount++];
            if (!sourceCommitted && !pending.committed) {
                record = {.receipt = pending.receipt, .result = {.status = Vfx::VfxSpawnStatus::Cancelled}};
                ++report.rejected;
                return;
            }
            SubmitPending(scene, pending, record, report);
        }

        /** @brief Stable compaction preserves source order without allocating or resolving another cutoff. */
        void CompleteSealedPending(GameplayVfxSceneState &scene, const std::uint64_t tick, const bool sourceCommitted,
                                   GameplayVfxTickResult &report) noexcept {
            std::size_t retained{};
            for (std::size_t index = 0; index < scene.pendingCount; ++index) {
                auto &pending = scene.pending[index];
                if (pending.tick <= tick)
                    CompleteCutoff(scene, pending, sourceCommitted, report);
                else {
                    if (retained != index)
                        scene.pending[retained] = std::move(pending);
                    ++retained;
                }
            }
            for (std::size_t index = retained; index < scene.pendingCount; ++index) {
                scene.pending[index].table.reset();
                scene.pending[index].lifetime.reset();
            }
            scene.pendingCount = retained;
        }

        void ClearPending(GameplayVfxSceneState &scene) noexcept {
            for (std::size_t index = 0; index < scene.pendingCount; ++index) {
                scene.pending[index].table.reset();
                scene.pending[index].lifetime.reset();
            }
            scene.pendingCount = 0;
        }

        /** @brief Cancels exactly the failed attachment's unsealed commands and keeps the existing aggregate result credits. */
        void AbortCallbackPending(GameplayVfxSceneState &scene, const std::shared_ptr<const GameplayVfxScopeLifetime> &lifetime) noexcept {
            std::size_t retained{};
            for (std::size_t index = 0; index < scene.pendingCount; ++index) {
                auto &pending = scene.pending[index];
                if (!pending.committed && pending.tick == scene.tick && pending.lifetime == lifetime) {
                    scene.results[scene.resultCount++] = {.receipt = pending.receipt, .result = {.status = Vfx::VfxSpawnStatus::Cancelled}};
                } else {
                    if (retained != index)
                        scene.pending[retained] = std::move(pending);
                    ++retained;
                }
            }
            for (std::size_t index = retained; index < scene.pendingCount; ++index) {
                scene.pending[index].table.reset();
                scene.pending[index].lifetime.reset();
            }
            scene.pendingCount = retained;
        }

        /** @brief Aborts only this failed source transaction; independently committed sources retain their original tick/sequence. */
        void AbortPending(GameplayVfxSceneState &scene) noexcept {
            std::size_t retained{};
            for (std::size_t index = 0; index < scene.pendingCount; ++index) {
                auto &pending = scene.pending[index];
                if (pending.committed || pending.tick != scene.tick) {
                    if (retained != index)
                        scene.pending[retained] = std::move(pending);
                    ++retained;
                } else {
                    scene.results[scene.resultCount++] = {.receipt = pending.receipt, .result = {.status = Vfx::VfxSpawnStatus::Cancelled}};
                }
            }
            for (std::size_t index = retained; index < scene.pendingCount; ++index) {
                scene.pending[index].table.reset();
                scene.pending[index].lifetime.reset();
            }
            scene.pendingCount = retained;
        }
    }  // namespace

    Vfx::VfxSpawnStatus GameplayVfxHostAccess::BeginTick(const GameplayVfxSceneLease &lease, const std::uint64_t tick,
                                                         const float deltaSeconds) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (const auto status = Current(lease); status != Accepted)
            return status;
        auto &scene = *lease.state;
        if (scene.sourceOpen)
            return Reentrant;
        if (!NewSourceTick(scene, tick))
            return OutOfOrder;
        if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0F || deltaSeconds > scene.maximumDelta)
            return InvalidRequest;
        if (scene.sealCount == scene.sealed.size())
            return EventQueueFull;
        scene.tick = tick;
        scene.delta = deltaSeconds;
        scene.sourceOpen = true;
        scene.sealPrepared = false;
        scene.resultCount = 0;
        if (scene.sealCount == 0)
            scene.world->ClearSpawnResults();
        return Accepted;
    }

    GameplayVfxTickResult GameplayVfxHostAccess::CommitTick(const GameplayVfxSceneLease &lease, const std::uint64_t tick) noexcept {
        if (const auto status = Current(lease); status != Vfx::VfxSpawnStatus::Accepted)
            return {.status = status};
        auto &scene = *lease.state;
        if (!scene.sourceOpen || tick != scene.tick || scene.sealCount != 0 || scene.sealPrepared)
            return {.status = Vfx::VfxSpawnStatus::OutOfOrder};
        const SourceScope scope{scene};
        GameplayVfxTickResult report{.status = Vfx::VfxSpawnStatus::Accepted};
        for (std::size_t index = 0; index < scene.pendingCount; ++index)
            SubmitPending(scene, scene.pending[index], scene.results[scene.resultCount++], report);
        ClearPending(scene);
        scene.sourceOpen = false;
        report.world = scene.world->UpdateSpawns(tick, scene.delta);
        scene.lastCommittedTick = tick;
        report.status = report.world.status;
        for (auto &result : std::span{scene.results.data(), scene.resultCount})
            CollectResult(scene, result);
        return report;
    }

    GameplayVfxTickResult GameplayVfxHostAccess::CommitSealedTick(const GameplayVfxSceneLease &lease, const std::uint64_t tick,
                                                                  const bool sourceCommitted) noexcept {
        if (const auto status = Current(lease); status != Vfx::VfxSpawnStatus::Accepted)
            return {.status = status};
        auto &scene = *lease.state;
        if (scene.sourceOpen || scene.sealCount == 0 || scene.sealed[scene.sealHead].tick != tick)
            return {.status = Vfx::VfxSpawnStatus::OutOfOrder};
        const SourceScope scope{scene};
        const auto firstResult = scene.resultCount;
        GameplayVfxTickResult report{.status = Vfx::VfxSpawnStatus::Accepted};
        scene.world->ClearSpawnResults();
        CompleteSealedPending(scene, tick, sourceCommitted, report);
        report.world = scene.world->UpdateSpawns(tick, scene.sealed[scene.sealHead].delta);
        scene.lastCommittedTick = tick;
        report.status = report.world.status;
        for (std::size_t index = firstResult; index < scene.resultCount; ++index)
            CollectResult(scene, scene.results[index]);
        scene.sealed[scene.sealHead] = {};
        scene.sealHead = (scene.sealHead + 1) % scene.sealed.size();
        --scene.sealCount;
        return report;
    }

    /** @copydoc GameplayVfxHostAccess::AbortCallback */
    void GameplayVfxHostAccess::AbortCallback(const std::shared_ptr<const GameplayVfxContext> &client) noexcept {
        if (!client || !client->state_ || !client->state_->callback)
            return;
        std::shared_ptr<GameplayVfxSceneState> scene;
        if (VfxContextDetail::ClientStatus(*client->state_, scene) != Vfx::VfxSpawnStatus::Accepted)
            return;
        if (!scene->sourceOpen)
            return;
        AbortCallbackPending(*scene, client->state_->lifetime);
    }

    void GameplayVfxHostAccess::AbortTick(const GameplayVfxSceneLease &lease, const std::uint64_t tick) noexcept {
        if (Current(lease) != Vfx::VfxSpawnStatus::Accepted)
            return;
        auto &scene = *lease.state;
        if (!scene.sourceOpen || tick != scene.tick)
            return;
        AbortPending(scene);
        scene.sourceOpen = false;
        scene.sealPrepared = false;
    }
}  // namespace Horo::Gameplay::Detail
