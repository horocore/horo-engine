#include "Horo/Gameplay/PersistenceInstallation.h"

#include <type_traits>

using Horo::Runtime::GameplayPersistenceInstallation;
static_assert(!std::is_default_constructible_v<GameplayPersistenceInstallation>);
static_assert(!std::is_aggregate_v<GameplayPersistenceInstallation>);
static_assert(!std::is_copy_constructible_v<GameplayPersistenceInstallation>);
static_assert(std::is_nothrow_move_constructible_v<GameplayPersistenceInstallation>);
static_assert(!std::is_constructible_v<GameplayPersistenceInstallation, std::shared_ptr<const Horo::Runtime::GameplayPersistenceDescriptor>,
                                       std::shared_ptr<const Horo::Runtime::ICanonicalStateAdapter>,
                                       std::shared_ptr<const std::atomic_bool>, Horo::CancellationToken>);
