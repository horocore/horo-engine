#include "GameplayVfxContextInternal.h"

#include <exception>

namespace Horo::Gameplay::Detail {
    /** @copydoc GameplayVfxHostAccess::PrepareSealTick */
    Vfx::VfxSpawnStatus GameplayVfxHostAccess::PrepareSealTick(const GameplayVfxSceneLease &lease, const std::uint64_t tick) noexcept {
        using enum Horo::Vfx::VfxSpawnStatus;
        if (!lease.state)
            return StaleScene;
        auto &scene = *lease.state;
        if (const auto status = VfxContextDetail::Status(scene); status != Accepted)
            return status;
        if (!scene.sourceOpen || tick != scene.tick)
            return OutOfOrder;
        if (scene.sealCount == scene.sealed.size())
            return EventQueueFull;
        scene.sealPrepared = true;
        return Accepted;
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
