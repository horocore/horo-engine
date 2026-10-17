#include "GameplayModuleTestSupport.h"
#include "GameplayRetirementTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "GameplayWorldComposition.h"
#include "HeadlessNetworkServices.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"

#include <catch2/catch_test_macros.hpp>
#include <future>
using namespace Horo;
using namespace Horo::Application::Internal;
using namespace Horo::Gameplay;

TEST_CASE("Actual headless rollback retains failed SDK services while Physics and Scene retire", "[gameplay-physics][host][retirement]") {
    using namespace Horo::Network;
    Horo::Tests::NativeGameplayFailureControl native{2};
    GameplayWorldSelection selection{.moduleId = "game.tests",
                                     .nativeArtifact = HORO_TEST_GAME_MODULE_PATH,
                                     .descriptorRevision = Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH),
                                     .physicsPermission = GameplayPhysicsPermission::Granted,
                                     .moduleRetirements = std::make_shared<GameModuleRetirementQueue>()};
    auto queue = selection.moduleRetirements;
    const auto owner = std::this_thread::get_id();
    auto definition = Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, BehaviorTypeId::Parse("game.tests.dynamic_mover").Value());
    auto factories =
        ComposeHeadlessNetworkServices(std::make_shared<const Runtime::RuntimeSceneDefinition>(std::move(definition)), {}, selection);
    auto scene = factories
                     .services[static_cast<std::size_t>(NetworkModeServiceKind::Scene)](
                         {NetworkModeServiceKind::Scene, NetworkProjectRole::Standalone, {}, NetworkModeWorldKind::Standalone, {7}})
                     .Value();
    auto physics = factories
                       .services[static_cast<std::size_t>(NetworkModeServiceKind::Physics)](
                           {NetworkModeServiceKind::Physics, NetworkProjectRole::Standalone, {}, NetworkModeWorldKind::Standalone, {7}})
                       .Value();
    REQUIRE(scene->Prepare().HasValue());
    REQUIRE(physics->Prepare().HasValue());
    REQUIRE(scene->Activate().HasValue());
    REQUIRE(physics->Activate().HasError());
    REQUIRE(native.callback);
    REQUIRE(native.control.physics);
    CHECK(queue->Close().HasError());
    CHECK(native.control.destroyed == 0);
    CHECK(native.control.stopped == 0);
    physics->Shutdown();
    scene->Shutdown();
    physics.reset();
    scene.reset();
    factories = {};
    CHECK(native.callback->Invoke({}) == GameplayEventOutcome::CapabilityUnavailable);
    const auto binding = native.control.physics->Binding();
    REQUIRE(native.control.physics->Acquire(binding.moduleId, binding.scene, binding.sceneGeneration).HasError());
    std::async(std::launch::async, [callback = std::move(native.callback)]() mutable {
        callback.reset();
    }).get();
    CHECK(native.control.destroyed == 0);
    REQUIRE(queue->Close().HasValue());
    CHECK(native.control.stopped == 1);
    CHECK(native.control.destroyed == 1);
    CHECK(native.control.destroyedOn == owner);
}
