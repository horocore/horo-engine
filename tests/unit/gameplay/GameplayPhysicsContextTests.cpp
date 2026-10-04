#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <thread>
#include <type_traits>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Physics;

    GameplayPhysicsBinding Binding(const PhysicsWorld &world, const std::uint64_t scene = 7) {
        return {"game.tests", scene, 7, world.Identity(), true, true};
    }

    PhysicsQueryCommand Ray(const PhysicsQueryEventCapability &capability, const PhysicsWorld &world) {
        PhysicsQueryDescriptor descriptor;
        descriptor.world = capability.Identity().world;
        descriptor.sceneGeneration = 7;
        descriptor.geometry = PhysicsRayQuery{.maximumDistanceMeters = 10};
        descriptor.filter.channel = PhysicsQueryChannelId::Parse("12345678-1234-4234-8234-123456789abc").Value();
        return {capability.Identity(), world.PublishedTick().publicationRevision, descriptor};
    }

    void Activate(PhysicsWorld &world, const std::uint64_t id) {
        REQUIRE(world.Activate(PhysicsWorldId::Create(id).Value()).HasValue());
        REQUIRE(world.AdvanceFixedTick({.simulationTick = 1, .sceneGeneration = 7, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
    }
}  // namespace

TEST_CASE("Gameplay Physics admission distinguishes permission from disabled or omitted service", "[gameplay-physics]") {
    GameplayPhysicsBinding binding{"game.tests", 7, 7, PhysicsWorldId::Create(301).Value(), true, false};
    Physics::Test::RequireError(GameplayPhysicsContext::Create(binding, nullptr), GameplayErrors::PhysicsPermissionDenied);
    binding.permissionGranted = true;
    Physics::Test::RequireError(GameplayPhysicsContext::Create(binding, nullptr), GameplayErrors::PhysicsUnavailable);
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Null).Value();
    auto world = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
    REQUIRE(world->Activate(binding.world).HasValue());
    binding.moduleEnabled = false;
    Physics::Test::RequireError(GameplayPhysicsContext::Create(binding, world.get()), GameplayErrors::PhysicsUnavailable);
    binding.moduleEnabled = true;
    Physics::Test::RequireError(GameplayPhysicsContext::Create(binding, world.get()), GameplayErrors::PhysicsUnavailable);
}

#if HORO_TEST_PHYSICS_NATIVE
TEST_CASE("Gameplay Physics provides the exact existing capability and explicit two-world routing", "[gameplay-physics]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto first = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
    auto second = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
    Activate(*first, 301);
    Activate(*second, 302);
    auto one = GameplayPhysicsContext::Create(Binding(*first, 7), first.get()).Value();
    auto two = GameplayPhysicsContext::Create(Binding(*second, 8), second.get()).Value();
    auto client = one->Acquire("game.tests", 7, 7).Value();
    auto other = two->Acquire("game.tests", 8, 7).Value();
    STATIC_REQUIRE(std::is_same_v<decltype(client), PhysicsQueryEventCapability>);
    REQUIRE(client.Identity().world == first->Identity());
    REQUIRE(other.Identity().world == second->Identity());
    std::array<PhysicsQueryHit, 1> hits{};
    REQUIRE(client.Submit(Ray(client, *first), hits).HasValue());
    Physics::Test::RequireError(client.Submit(Ray(other, *second), hits), PhysicsErrors::HandleWorldMismatch);
    const auto firstTick = first->PublishedTick();
    const auto secondTick = second->PublishedTick();
    std::array<PhysicsEventRecord, 1> events{};
    REQUIRE(client.ReadEvents({client.Identity(), firstTick.eventTick, firstTick.publicationRevision, 1}, events).HasValue());
    Physics::Test::RequireError(client.ReadEvents({other.Identity(), secondTick.eventTick, secondTick.publicationRevision, 1}, events),
                                PhysicsErrors::HandleWorldMismatch);
    const auto foreignCommand = Ray(other, *second);
    Physics::Test::RequireError(client.SubmitBatch(std::span{&foreignCommand, 1}), PhysicsErrors::HandleWorldMismatch);
    Physics::Test::RequireError(one->Acquire("game.other", 7, 7), GameplayErrors::PhysicsPermissionDenied);
    Physics::Test::RequireError(one->Acquire("game.tests", 8, 7), PhysicsErrors::HandleWorldMismatch);
    Physics::Test::RequireError(one->Acquire("game.tests", 7, 8), PhysicsErrors::HandleWorldMismatch);
    auto invalid = Binding(*first);
    invalid.world = second->Identity();
    Physics::Test::RequireError(GameplayPhysicsContext::Create(invalid, first.get()), GameplayErrors::InvalidCapabilityId);
    one->Revoke();
    Physics::Test::RequireError(client.Submit(Ray(client, *first), hits), PhysicsErrors::CapabilityRevoked);
    REQUIRE(other.Submit(Ray(other, *second), hits).HasValue());
}

TEST_CASE("Module-generation cancellation revokes retained Physics clients and pending batches", "[gameplay-physics]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto world = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
    Activate(*world, 303);
    CancellationSource module;
    auto context = GameplayPhysicsContext::Create(Binding(*world), world.get(), module.Token()).Value();
    auto client = context->Acquire("game.tests", 7, 7).Value();
    const auto command = Ray(client, *world);
    auto batch = client.SubmitBatch(std::span{&command, 1}).Value();
    module.RequestCancellation();
    Physics::Test::RequireError(batch.Poll(), PhysicsErrors::CapabilityRevoked);
    Physics::Test::RequireError(context->Acquire("game.tests", 7, 7), PhysicsErrors::CapabilityRevoked);
    std::array<PhysicsQueryHit, 1> hits{};
    Physics::Test::RequireError(client.Submit(command, hits), PhysicsErrors::CapabilityRevoked);
    REQUIRE(world->ProcessQueryBatch().HasValue());
    REQUIRE(world->PublishedTick().completedTick == 1);
}

TEST_CASE("Retained Physics client and its holder are safe after world destruction", "[gameplay-physics]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto world = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
    Activate(*world, 304);
    auto context = GameplayPhysicsContext::Create(Binding(*world), world.get()).Value();
    auto client = context->Acquire("game.tests", 7, 7).Value();
    const auto command = Ray(client, *world);
    world.reset();
    std::array<PhysicsQueryHit, 1> hits{};
    Physics::Test::RequireError(client.Submit(command, hits), PhysicsErrors::CapabilityStale);
    context.reset();
    Physics::Test::RequireError(client.Submit(command, hits), PhysicsErrors::CapabilityRevoked);
}

TEST_CASE("Native module unload revokes clients before a still-live Physics world retires", "[gameplay-physics]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto world = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
    Activate(*world, 305);
    auto context = GameplayPhysicsContext::Create(Binding(*world), world.get()).Value();
    GameModuleHost host{{}, context};
    auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, {"game.tests", CurrentGameplayBuildFingerprint(),
                                                         Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)});
    REQUIRE(loaded.HasValue());
    auto module = std::move(loaded).Value();
    REQUIRE(module->PhysicsContext() == context);
    auto client = module->PhysicsContext()->Acquire("game.tests", 7, 7).Value();
    const auto command = Ray(client, *world);
    module.reset();
    std::array<PhysicsQueryHit, 1> hits{};
    Physics::Test::RequireError(client.Submit(command, hits), PhysicsErrors::CapabilityRevoked);
    auto independent = world->IssueQueryEventCapability().Value();
    REQUIRE(independent.Submit(Ray(independent, *world), hits).HasValue());
    Physics::Test::RequireError(host.Load(HORO_TEST_GAME_MODULE_PATH, {"game.tests", CurrentGameplayBuildFingerprint(),
                                                                       Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)}),
                                PhysicsErrors::CapabilityRevoked);
}

TEST_CASE("Retained pending and completed Physics batches preserve first terminal result across revoke and destruction",
          "[gameplay-physics]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Canonical).Value();
    auto world = runtime->PrepareWorld(Physics::Test::SmallWorldSettings()).Value();
    Activate(*world, 306);
    auto context = GameplayPhysicsContext::Create(Binding(*world), world.get()).Value();
    auto client = context->Acquire("game.tests", 7, 7).Value();
    const auto command = Ray(client, *world);
    auto completed = client.SubmitBatch(std::span{&command, 1}).Value();
    REQUIRE(world->ProcessQueryBatch().HasValue());
    const auto committed = completed.Poll().Value();
    REQUIRE(committed);
    auto pending = client.SubmitBatch(std::span{&command, 1}).Value();
    context->Revoke();
    bool pendingRevoked{};
    bool completedPreserved{};
    bool cancelRejected{};
    std::thread observer([&] {
        const auto observedPending = pending.Poll();
        const auto observedCompleted = completed.Poll();
        pendingRevoked =
            observedPending.HasError() && observedPending.ErrorValue().code.Value() == PhysicsErrors::CapabilityRevoked.code.Value();
        completedPreserved = observedCompleted.HasValue() && observedCompleted.Value() == committed;
        cancelRejected = !completed.Cancel();
    });
    observer.join();
    REQUIRE(pendingRevoked);
    REQUIRE(completedPreserved);
    REQUIRE(cancelRejected);
    world.reset();
    context.reset();
    Physics::Test::RequireError(pending.Poll(), PhysicsErrors::CapabilityRevoked);
    REQUIRE(completed.Poll().Value() == committed);
}
#endif
