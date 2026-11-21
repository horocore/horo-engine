#include "Horo/Gameplay/GameModule.h"

#include <array>

namespace {
    std::array<std::uint32_t, 3> entries{};
}

/** @brief Reports a previous SDK without exposing any generated descriptor layout. */
extern "C" HORO_GAME_EXPORT const Horo::Gameplay::GameModuleDescriptor *GetGameModuleDescriptor() noexcept {
    static const Horo::Gameplay::GameModuleDescriptor descriptor{
        .sdkBoundaryVersion = Horo::Gameplay::GameplaySdkBoundaryVersion - 1,
        .moduleId = "game.ai.legacy",
        .buildFingerprint = Horo::Gameplay::CurrentGameplayBuildFingerprint().data(),
    };
    ++entries[0];
    return &descriptor;
}

/** @brief Counts forbidden bundle export entry without handing the host a newer layout. */
extern "C" HORO_GAME_EXPORT const Horo::Gameplay::GeneratedGameplayDescriptorBundle *GetGameplayDescriptorBundle() noexcept {
    ++entries[1];
    return nullptr;
}

/** @brief Counts forbidden module creation; no gameplay object is constructed. */
extern "C" HORO_GAME_EXPORT Horo::Gameplay::IGameModule *CreateGameModule() noexcept {
    ++entries[2];
    return nullptr;
}

/** @brief Completes the required fixture export pair without owning a gameplay object. */
extern "C" HORO_GAME_EXPORT void DestroyGameModule(Horo::Gameplay::IGameModule *) noexcept {}

/** @brief Borrows fixed entry counters only while the test retains this actual image. */
extern "C" HORO_GAME_EXPORT const std::uint32_t *HoroTestLegacyPerceptionEntries() noexcept {
    return entries.data();
}
