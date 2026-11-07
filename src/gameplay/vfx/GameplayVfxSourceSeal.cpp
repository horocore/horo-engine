#include "GameplayVfxContextInternal.h"

#include <exception>

namespace Horo::Gameplay::Detail {
    /** @copydoc GameplayVfxHostAccess::PrepareSealTick */
    Vfx::VfxSpawnStatus GameplayVfxHostAccess::PrepareSealTick(const GameplayVfxSceneLease &lease, const std::uint64_t tick) noexcept {
        if (!lease.state)
            return Vfx::VfxSpawnStatus::StaleScene;
        auto &scene = *lease.state;
        if (const auto status = VfxContextDetail::Status(scene); status != Vfx::VfxSpawnStatus::Accepted)
            return status;
        if (!scene.sourceOpen || tick != scene.tick)
            return Vfx::VfxSpawnStatus::OutOfOrder;
        if (scene.sealCount == scene.sealed.size())
            return Vfx::VfxSpawnStatus::EventQueueFull;
        scene.sealPrepared = true;
        return Vfx::VfxSpawnStatus::Accepted;
    }

    /** @copydoc GameplayVfxHostAccess::SealTick */
    void GameplayVfxHostAccess::SealTick(const GameplayVfxSceneLease &lease, const std::uint64_t tick) noexcept {
        if (!lease.state)
            std::terminate();
        auto &scene = *lease.state;
        if (!scene.sealPrepared || !scene.sourceOpen || scene.tick != tick || scene.sealCount == scene.sealed.size())
            std::terminate();
        const auto index = (scene.sealHead + scene.sealCount) % scene.sealed.size();
        scene.sealed[index] = {tick, scene.delta};
        ++scene.sealCount;
        scene.lastSealedTick = tick;
        scene.sourceOpen = false;
        scene.sealPrepared = false;
    }
}  // namespace Horo::Gameplay::Detail
