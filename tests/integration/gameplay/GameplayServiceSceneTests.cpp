#include "GameplayRuntimeTestSupport.h"
#include "GameplayWorldSelectionTestSupport.h"
#include "Horo/Prefab/PrefabErrors.h"
#include "PhysicsTestUtils.h"
#include "ScenePhysicsHost.h"

#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Application::Internal;
using namespace Horo::Gameplay;
using namespace Horo::Physics;
using TestSupport::Native;
using TestSupport::Script;

TEST_CASE("Service-backed gameplay uses the published Scene without a fabricated prefab provider", "[gameplay-physics][host][scene]") {
    for (const bool script : {false, true}) {
        Runtime::RuntimeSceneService scenes{Runtime::SceneRuntimeId{73}};
        GameplayWorldComposition *activeGameplay{};
        REQUIRE(scenes.AddStructuralParticipant(MakeGameplayStructuralParticipant(activeGameplay)).HasValue());
        CancellationSource cancellation;
        REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
        const auto type = BehaviorTypeId::Parse(script ? "game.tests.physics_context" : "game.tests.dynamic_mover").Value();
        REQUIRE(scenes.QueuePreparation(Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, type)).HasValue());
        const Runtime::FrameContext frame{.frameNumber = 1, .cancellation = cancellation.Token()};
        REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
        REQUIRE(scenes.ActiveScene().has_value());
        const auto scene = scenes.ActiveScene()->RuntimeId();
        CHECK(scene == Runtime::SceneRuntimeId{73});
        auto gameplay = GameplayWorldComposition::Create(scenes, nullptr, 1, script ? Script() : Native());
        REQUIRE(gameplay.HasValue());
        activeGameplay = gameplay.Value().get();
        const auto retained = activeGameplay->PhysicsContext();
        Physics::Test::RequireError(retained->Acquire("game.tests", scene.value, 1), GameplayErrors::PhysicsUnavailable);
        REQUIRE(activeGameplay->FixedUpdate({1.0 / 60.0}).HasValue());
        CHECK(scenes.ActiveScene()->RuntimeId() == scene);
        activeGameplay->Shutdown();
        activeGameplay = nullptr;
        Physics::Test::RequireError(retained->Acquire("game.tests", scene.value, 1), PhysicsErrors::CapabilityRevoked);
        scenes.Shutdown();
    }
}

TEST_CASE("Service-backed gameplay rejects a prefab grant when no provider was selected", "[gameplay-physics][host][scene]") {
    Runtime::RuntimeSceneService scenes;
    auto selection = Native();
    selection.prefabPermission = GameplayPrefabPermission::Granted;
    Physics::Test::RequireError(GameplayWorldComposition::Create(scenes, nullptr, 1, selection), Prefab::PrefabErrors::AdmissionRejected);
    CHECK_FALSE(scenes.ActiveScene().has_value());
}

TEST_CASE("Explicit Null Scene host advances gameplay quanta without manufacturing native publications",
          "[gameplay-physics][host][scene][null]") {
    auto runtime = PhysicsRuntime::Create(PhysicsRuntimeMode::Null);
    REQUIRE(runtime.HasValue());
    PhysicsSceneActivationAuthority authority;
    ScenePhysicsHost host;
    Runtime::RuntimeSceneService scenes;
    auto physics = PhysicsWorldSettings::Capture({});
    auto character = Character::CharacterWorldSettings::Capture({});
    REQUIRE(physics.HasValue());
    REQUIRE(character.HasValue());
    auto participant = host.MakeParticipant(*runtime.Value(), authority, {std::move(physics).Value(), std::move(character).Value(), {}});
    REQUIRE(participant.HasValue());
    REQUIRE(scenes.AddActivationParticipant(std::move(participant).Value()).HasValue());
    CancellationSource cancellation;
    REQUIRE(scenes.Startup(cancellation.Token()).HasValue());
    Runtime::SceneDefinitionBuilder builder{{1}, {1}};
    auto definition = std::move(builder).Build();
    REQUIRE(definition.HasValue());
    REQUIRE(scenes.QueuePreparation(std::move(definition).Value()).HasValue());
    REQUIRE(scenes.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 1, .cancellation = cancellation.Token()})
                .HasValue());
    REQUIRE_FALSE(scenes.TakeOperationError());
    auto scope = host.Borrow();
    REQUIRE(scope.HasValue());
    REQUIRE(scope.Value().physics.State() == PhysicsWorldState::ActiveNull);
    REQUIRE(host.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)).HasValue());
    REQUIRE(host.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)).HasValue());
    CHECK(host.AdvanceFixedTick(Duration{}).HasError());
    CHECK(scope.Value().physics.PublishedTick().completedTick == 0);
    CHECK(scope.Value().physics.PublishedTick().publicationRevision == 0);
    CHECK(scope.Value().character.PublishedTick().completedTick == 0);
    Physics::Test::RequireError(scope.Value().physics.AdvanceFixedTick({.simulationTick = 1,
                                                                        .sceneGeneration = scope.Value().scene.value,
                                                                        .fixedDelta = Duration::FromNanoseconds(16'666'667)}),
                                PhysicsErrors::CapabilityUnavailable);
    scenes.Shutdown();
    CHECK_FALSE(host.HasPublishedScene());
}
