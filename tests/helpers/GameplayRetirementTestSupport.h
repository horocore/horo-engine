#pragma once
/** @file GameplayRetirementTestSupport.h @brief Actual native fixture admission and callback control for host-lifetime tests. */
#include "GameplayModuleTestSupport.h"
#include "GameplayRetirementFixture.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Platform/DynamicLibrary.h"

#include <catch2/catch_test_macros.hpp>
#include <cstring>

namespace Horo::Tests {
    using Horo::Gameplay::GameplayEventLease;

    struct NativeGameplayFailureControl final {
        std::unique_ptr<Platform::DynamicLibrary> image;
        void (*configure)(GameplayRetirementFixture *) noexcept {};
        GameplayRetirementFixture control;
        std::shared_ptr<GameplayEventLease> callback;

        explicit NativeGameplayFailureControl(const unsigned phase) {
            auto opened = Platform::LoadDynamicLibrary(HORO_TEST_GAME_MODULE_PATH);
            REQUIRE(opened.HasValue());
            image = std::move(opened).Value();
            auto *symbol = image->GetSymbol("ConfigureGameplayRetirementFixture");
            REQUIRE(symbol);
            static_assert(sizeof(configure) == sizeof(symbol));
            std::memcpy(&configure, &symbol, sizeof(configure));
            control.failPhase = phase;
            control.callbackOwner = &callback;
            control.publish = [](void *owner, std::shared_ptr<GameplayEventLease> lease) {
                *static_cast<std::shared_ptr<GameplayEventLease> *>(owner) = std::move(lease);
            };
            configure(&control);
        }

        ~NativeGameplayFailureControl() {
            configure(nullptr);
        }
    };

    inline Gameplay::GameModuleLoadExpectation NativeRetirementExpectation() {
        return {"game.tests", Gameplay::CurrentGameplayBuildFingerprint(), ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
    }
}  // namespace Horo::Tests
