#pragma once
/** @brief Actual module-owned durable behavior fields shared only by native regression consumers. */
#include "Horo/Gameplay/BehaviorRestore.h"

namespace Horo::Tests {
    struct GameplayBehaviorDurableFields final {
        std::uint64_t value{};
        std::optional<Gameplay::BehaviorEntityRestoreTarget> peer;
        std::optional<Runtime::SaveRestoreEntityTarget> resolvedPeer;
    };

    class IGameplayBehaviorDurableFixture {
    public:
        virtual ~IGameplayBehaviorDurableFixture() = default;
        [[nodiscard]] virtual GameplayBehaviorDurableFields DurableFields() const = 0;
        virtual void SetDurableFields(GameplayBehaviorDurableFields) = 0;
    };
}  // namespace Horo::Tests
