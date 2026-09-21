#include "Horo/Navigation/NavigationAgentRegistry.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <type_traits>
#include <utility>

namespace Horo::Navigation {
    namespace {
        using TestSupport::RequireError;

        NavigationAgentSceneBinding Binding(const std::uint64_t world, const std::uint64_t scene, const std::uint64_t generation) {
            return {.world = NavigationWorldId::Create(world).Value(),
                    .scene = NavigationSceneRuntimeId::Create(scene).Value(),
                    .sceneGeneration = NavigationSceneGeneration::Create(generation).Value()};
        }

        NavigationAgentDescriptor Agent(const NavigationAgentSceneBinding binding, const std::uint32_t entityIndex,
                                        const std::uint32_t entityGeneration, const std::uint64_t profile = 1,
                                        const std::uint64_t filter = 2) {
            return {.owner = {.scene = binding.scene, .entityIndex = entityIndex, .entityGeneration = entityGeneration},
                    .profile = NavigationAgentProfileId::Create(profile).Value(),
                    .filter = NavigationFilterId::Create(filter).Value(),
                    .radiusOverride = 0.5F};
        }
    }  // namespace

    static_assert(!std::is_copy_constructible_v<NavigationAgentRegistry>);
    static_assert(!std::is_copy_assignable_v<NavigationAgentRegistry>);
    static_assert(std::is_nothrow_move_constructible_v<NavigationAgentRegistry>);
    static_assert(!std::is_move_assignable_v<NavigationAgentRegistry>);
    static_assert(!std::is_default_constructible_v<NavigationAgentSceneCandidate::CreationKey>);

    TEST_CASE("Navigation agent registration publishes a complete deterministic population", "[unit][navigation][headless]") {
        auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 4})).Value();
        const NavigationAgentSceneBinding binding = Binding(11, 21, 1);
        const std::array descriptors{Agent(binding, 8, 3), Agent(binding, 2, 4, 4, 5)};

        auto prepared = registry.PrepareScene(binding, descriptors);
        REQUIRE(prepared.HasValue());
        auto candidate = std::move(prepared).Value();
        REQUIRE(registry.Snapshot().HasError());
        REQUIRE(candidate->ValidatePublication().HasValue());
        candidate->Publish();

        const auto snapshot = registry.Snapshot();
        REQUIRE(snapshot.HasValue());
        REQUIRE(snapshot.Value().Binding() == binding);
        REQUIRE(snapshot.Value().Agents().size() == 2);
        REQUIRE(snapshot.Value().Agents()[0].owner.entityIndex == 2);
        REQUIRE(snapshot.Value().Agents()[1].owner.entityIndex == 8);
        REQUIRE(snapshot.Value().Find(snapshot.Value().Agents()[0].handle).HasValue());
    }

    TEST_CASE("Navigation agent candidate failure leaves the published population unchanged", "[unit][navigation][headless]") {
        auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 2})).Value();
        const NavigationAgentSceneBinding binding = Binding(31, 41, 1);
        const std::array initial{Agent(binding, 1, 7)};
        auto first = std::move(registry.PrepareScene(binding, initial)).Value();
        first->Publish();

        auto duplicate = Agent(binding, 2, 9);
        const std::array invalid{duplicate, duplicate};
        RequireError(registry.PrepareScene(Binding(32, 41, 2), invalid), NavigationErrors::AgentRegistryConflict);

        const auto snapshot = registry.Snapshot();
        REQUIRE(snapshot.HasValue());
        REQUIRE(snapshot.Value().Binding() == binding);
        REQUIRE(snapshot.Value().Agents().size() == 1);
        REQUIRE(snapshot.Value().Agents()[0].owner.entityIndex == 1);
    }

    TEST_CASE("Navigation agent handles reject stale generations and replaced worlds", "[unit][navigation][headless]") {
        auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 2})).Value();
        const NavigationAgentSceneBinding firstBinding = Binding(51, 61, 1);
        auto first = std::move(registry.PrepareScene(firstBinding, std::array{Agent(firstBinding, 3, 8)})).Value();
        first->Publish();
        const CrowdAgentHandle oldHandle = registry.Snapshot().Value().Agents().front().handle;

        REQUIRE(registry.UnregisterAtSafePoint(oldHandle).HasValue());
        RequireError(registry.Find(oldHandle), NavigationErrors::InvalidHandle);
        const auto replacementHandle = registry.RegisterAtSafePoint(Agent(firstBinding, 3, 9));
        REQUIRE(replacementHandle.HasValue());
        REQUIRE(replacementHandle.Value().slot.generation != oldHandle.slot.generation);

        const NavigationAgentSceneBinding secondBinding = Binding(52, 62, 2);
        auto second = std::move(registry.PrepareScene(secondBinding, std::span<const NavigationAgentDescriptor>{})).Value();
        REQUIRE(second->ValidatePublication().HasValue());
        second->Publish();
        first->Shutdown();
        RequireError(registry.Find(replacementHandle.Value()), NavigationErrors::InvalidHandle);
        REQUIRE(registry.ActiveBinding().Value() == secondBinding);

        const NavigationAgentOwner staleOwner{.scene = firstBinding.scene, .entityIndex = 3, .entityGeneration = 9};
        RequireError(registry.UnregisterOwnerAtSafePoint(staleOwner), NavigationErrors::AgentRegistryStale);
    }

    TEST_CASE("Navigation agent admission rejects malformed descriptors and closes cleanly", "[unit][navigation][headless]") {
        RequireError(NavigationAgentRegistry::Create({.maximumAgents = 0}), NavigationErrors::AgentRegistryCapacityExceeded);

        auto registry = std::move(NavigationAgentRegistry::Create({.maximumAgents = 1})).Value();
        const auto binding = Binding(71, 81, 1);
        auto invalid = Agent(binding, 1, 1);
        invalid.radiusOverride = -1.0F;
        RequireError(registry.PrepareScene(binding, std::array{invalid}), NavigationErrors::AgentDescriptorInvalid);

        registry.BeginShutdown();
        registry.BeginShutdown();
        RequireError(registry.PrepareScene(binding, std::span<const NavigationAgentDescriptor>{}),
                     NavigationErrors::AgentRegistryShuttingDown);
        RequireError(registry.Snapshot(), NavigationErrors::AgentRegistryShuttingDown);
    }
}  // namespace Horo::Navigation
