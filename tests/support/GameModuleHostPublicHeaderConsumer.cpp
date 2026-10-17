#include "Horo/Gameplay/GameModuleHost.h"

#include <type_traits>
#include <utility>

static_assert(!std::is_default_constructible_v<Horo::Gameplay::LoadedGameModule::ConstructionKey>);
static_assert(!std::is_aggregate_v<Horo::Gameplay::LoadedGameModule::ConstructionKey>);
static_assert(!std::is_copy_constructible_v<Horo::Gameplay::LoadedGameModule>);
static_assert(
    std::is_same_v<decltype(&Horo::Gameplay::LoadedGameModule::RequestRetirement), void (Horo::Gameplay::LoadedGameModule::*)() noexcept>);
static_assert(
    std::is_same_v<decltype(&Horo::Gameplay::LoadedGameModule::TryRetire), Horo::Result<void> (Horo::Gameplay::LoadedGameModule::*)()>);
static_assert(
    std::is_same_v<decltype(std::declval<const Horo::Gameplay::GameModuleHost &>().Load(
                       std::declval<const std::filesystem::path &>(), std::declval<const Horo::Gameplay::GameModuleLoadExpectation &>())),
                   Horo::Result<Horo::Gameplay::GameModuleLoadCandidate>>);

int main() {
    return 0;
}
