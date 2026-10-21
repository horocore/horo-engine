#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Render::Test {
    /** @brief Deterministic-test-only finite close budget; retains the owner throughout every pending poll. */
    template <typename Owner> void RequireDrainedOwner(Owner &owner) {
        REQUIRE(owner.BeginShutdown().HasValue());
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
        for (;;) {
            const auto progress = owner.PollDrain();
            REQUIRE(progress.HasValue());
            if (progress.Value() == RenderBackendDrainProgress::Drained)
                return;
            REQUIRE(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds{1});
        }
    }
}  // namespace Horo::Render::Test
