#pragma once
#include "Horo/Gameplay/GameEventRegistry.h"

#include <thread>

namespace Horo::Gameplay {
    class GameplayPhysicsContext;
}

namespace Horo::Tests {
    /** @brief Explicit native fixture control, borrowed only while its owning test retains all module generations. */
    struct GameplayRetirementFixture final {
        std::shared_ptr<const Gameplay::GameplayPhysicsContext> physics;
        unsigned failPhase{};
        unsigned created{};
        unsigned registered{};
        unsigned started{};
        unsigned prepared{};
        unsigned stopped{};
        unsigned destroyed{};
        std::thread::id destroyedOn;
        void *callbackOwner{};
        void *stopOwner{};
        void *allocationOwner{};
        void (*armAllocationFailure)(void *) noexcept {};
        void (*onStop)(void *){};
        void (*publish)(void *, std::shared_ptr<Gameplay::GameplayEventLease>){};
    };
}  // namespace Horo::Tests
