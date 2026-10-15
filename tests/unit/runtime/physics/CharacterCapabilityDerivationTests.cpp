#include "CharacterWorldTestHelpers.h"

namespace Horo::Character {
    namespace {
        using namespace TestDetail;

        TEST_CASE("Character derived grants isolate child retirement and inherit parent queued-command cancellation",
                  "[character][capability][lifetime]") {
            auto active = SpawnedActiveWorldWithController();
            auto parent = active.world->IssueCapability().Value();
            auto child = parent.Derive().Value();
            auto sibling = parent.Derive().Value();
            REQUIRE(child.Identity().generation != parent.Identity().generation);
            REQUIRE(child.QueueMovementCommand(Movement(active.controller, 1, 77)).HasValue());
            child.Revoke();
            RequireError(child.Validate(), CharacterErrors::CapabilityRevoked);
            REQUIRE(parent.Validate().HasValue());
            REQUIRE(sibling.Validate().HasValue());
            CommandTrace trace;
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(1, trace.Observer())).HasValue());
            REQUIRE(trace.movementCount == 0);
            REQUIRE(sibling.QueueMovementCommand(Movement(active.controller, 2, 88)).HasValue());
            parent.Revoke();
            RequireError(sibling.Validate(), CharacterErrors::CapabilityRevoked);
            RequireError(sibling.Derive(), CharacterErrors::CapabilityRevoked);
            REQUIRE(active.world->AdvanceFixedTick(FixedTick(2, trace.Observer())).HasValue());
            REQUIRE(trace.movementCount == 0);
        }

        TEST_CASE("Character delegation depth bounds cancellation traversal after intermediate clients expire",
                  "[character][capability][capacity]") {
            auto world = PreparedWorld();
            auto child = world->IssueCapability().Value();
            for (std::uint8_t depth = 0; depth < MaximumCharacterCapabilityDerivationDepth; ++depth) {
                auto next = child.Derive();
                REQUIRE(next.HasValue());
                child = std::move(next).Value();
            }
            REQUIRE(child.Validate().HasValue());
            RequireError(child.Derive(), CharacterErrors::CapacityExceeded);
        }

        TEST_CASE("Character derived admission retains inert affinity and stale-world failures", "[character][capability][lifetime]") {
            const CharacterCapability inert;
            RequireError(inert.Validate(), CharacterErrors::CapabilityUnavailable);
            RequireError(inert.Derive(), CharacterErrors::CapabilityUnavailable);
            auto world = PreparedWorld();
            auto parent = world->IssueCapability().Value();
            bool rejected{};
            std::thread worker([&parent, &rejected] {
                const auto result = parent.Derive();
                rejected = result.HasError() && result.ErrorValue().code.Value() == CharacterErrors::ThreadAffinityViolation.code.Value();
            });
            worker.join();
            REQUIRE(rejected);
            auto child = parent.Derive().Value();
            world.reset();
            RequireError(child.Validate(), CharacterErrors::CapabilityStale);
            RequireError(parent.Derive(), CharacterErrors::CapabilityStale);
        }

    }  // namespace
}  // namespace Horo::Character
