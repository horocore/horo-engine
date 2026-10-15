#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Gameplay/LuaBehavior.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"
#ifdef HORO_TEST_NAVIGATION_DETOUR
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#endif
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iterator>
using namespace Horo;
using namespace Horo::Gameplay;
using namespace Horo::Navigation;
using namespace Horo::Navigation::CoordinatorTestSupport;

namespace {
    std::unique_ptr<INavigationQueryBackend> Provider() {
#ifdef HORO_TEST_NAVIGATION_DETOUR
        const std::array<Math::Vec3, 4> vertices{{{0, 0, 0}, {10, 0, 0}, {10, 0, 10}, {0, 0, 10}}};
        const std::array polygons{
            GroundedNavigationPolygon{.vertexIndices = {0, 1, 2, 3, 0, 0}, .vertexCount = 4, .area = NavigationAreaId::Create(1).Value()}};
        const NavigationDescriptorSource source{NavigationDescriptorSourceKind::Project, NavigationDescriptorSourceId::Create(1).Value()};
        const std::array areas{NavigationAreaDescriptor{.id = NavigationAreaId::Create(1).Value(),
                                                        .source = source,
                                                        .traversalCost = 1,
                                                        .flags = {.bits = 1}}};
        const std::array filters{NavigationQueryFilterDescriptor{.id = Request().filter, .source = source}};
        auto made = CreateRecastDetourNavigationQueryBackend({.world = World(),
                                                              .topology = Topology(),
                                                              .vertices = vertices,
                                                              .polygons = polygons,
                                                              .maximumQueryNodes = 64,
                                                              .maximumResultPoints = 16,
                                                              .maximumConcurrentQueries = 1,
                                                              .areas = areas,
                                                              .filters = filters});
        REQUIRE(made.HasValue());
        return std::move(made).Value();
#else
        return MakeObservedNavigationBackend();
#endif
    }

    struct Fixture final {
        JobSystem jobs{{.workerCount = 0}};
        NavigationWorldLifecycle world{std::move(NavigationWorldLifecycle::Create(4)).Value()};
        NavigationCoordinator coordinator{std::move(NavigationCoordinator::Create(jobs, Limits())).Value()};
        NavigationAgentRegistry registry{std::move(NavigationAgentRegistry::Create({.maximumAgents = 1})).Value()};
        NavigationAgentSceneBinding binding{World(), Activation().scene, Activation().sceneGeneration};
        std::unique_ptr<NavigationAgentSceneCandidate> candidate;
        std::unique_ptr<NavigationMovementController> movement;
        std::shared_ptr<GameplayNavigationContext> context;

        Fixture() {
            Activate(world, Provider());
            const std::array descriptors{NavigationAgentDescriptor{.owner = {binding.scene, 0, 1},
                                                                   .profile = NavigationAgentProfileId::Create(1).Value(),
                                                                   .filter = Request().filter}};
            candidate = std::move(registry.PrepareScene(binding, descriptors)).Value();
            candidate->Publish();
            auto made = NavigationMovementController::Create(registry, world, coordinator,
                                                             registry.Snapshot().Value().Agents().front().handle, Caller());
            REQUIRE(made.HasValue());
            movement = std::move(made).Value();
            auto bound =
                GameplayNavigationContext::Create({"game.tests", binding, true, true}, &world, &coordinator, movement.get(), Input());
            REQUIRE(bound.HasValue());
            context = std::move(bound).Value();
            REQUIRE(context->Refresh(Now()).HasValue());
        }

        NavigationMovementObservation Now(std::uint64_t tick = 0) const {
            return {.binding = binding,
                    .position = {1, 0, 1},
                    .stepSeconds = 0.1F,
                    .path = {.provenance = Source(), .linkRevision = 1, .goalRevision = 1, .tick = tick}};
        }

        void Publish(std::uint64_t tick = 1) {
            static_cast<void>(coordinator.Dispatch(tick));
            jobs.Shutdown(ShutdownPolicy::Drain);
            static_cast<void>(coordinator.Commit(Current(std::array{Caller()}, tick)));
        }

        ~Fixture() {
            if (context)
                context->Revoke();
            coordinator.BeginShutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
            static_cast<void>(coordinator.Commit(Current(std::array{Caller()}, 100)));
            if (context)
                static_cast<void>(context->DrainRetired());
            movement->BeginShutdown();
            static_cast<void>(movement->Update(Now(100)));
        }
    };

    BehaviorTypeId Type() {
        return BehaviorTypeId::Parse("game.tests.navigation").Value();
    }

    std::string Lua(const std::string &callback) {
        return "local retained; return horo.behavior "
               "{type_id='game.tests.navigation',display_name='Navigation',on_fixed_update=function(ctx,dt) " +
               callback + " end}";
    }

    Tests::ActiveBehaviorRuntime ActivateScript(LuaBehaviorProgram &program, BehaviorRegistry &registry,
                                                std::shared_ptr<const GameplayNavigationContext> navigation) {
        REQUIRE(registry.Register(program.Registration()).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        auto definition = Tests::SingleBehaviorSceneDefinition(Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1},
                                                               Runtime::SceneObjectId{2}, BehaviorInstanceId{1}, Type());
        auto scene = std::move(Runtime::RuntimeScene::Create(definition, Runtime::SceneRuntimeId{Activation().scene.Value()})).Value();
        auto runtime = BehaviorRuntime::Create(*scene, registry, {}, nullptr, std::move(navigation));
        REQUIRE(runtime.HasValue());
        return {std::move(scene), std::move(runtime).Value()};
    }
}  // namespace

TEST_CASE("Loaded native module reload and shutdown revoke pending navigation delivery before unload") {
    Fixture fixture;
    GameModuleHost host{{}, nullptr, fixture.context};
    const GameModuleLoadExpectation expected{"game.tests", CurrentGameplayBuildFingerprint(),
                                             Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
    auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, expected);
    REQUIRE(loaded.HasValue());
    REQUIRE_FALSE(loaded.Value().diagnostic);
    std::unique_ptr<LoadedGameModule> module = std::move(loaded).Value().owner;
    REQUIRE(module->NavigationContext() == fixture.context);
    auto duplicate = host.Load(HORO_TEST_GAME_MODULE_PATH, expected);
    REQUIRE(duplicate.HasError());
    CHECK(duplicate.ErrorValue().code.Value() == NavigationCapabilityErrors::PermissionDenied.code.Value());
    REQUIRE(fixture.context->IsUsable());
    REQUIRE_FALSE(module->Cancellation().IsCancellationRequested());
    auto request = module->NavigationContext()->Submit({.start = {1, 0, 1}, .destination = {9, 0, 9}});
    REQUIRE(request.HasValue());
    SECTION("reload") {
        REQUIRE(module->PrepareReload().HasValue());
    }
    SECTION("shutdown") {
        module.reset();
    }
    auto result = request.Value().Poll();
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == NavigationCapabilityErrors::ScopeRevoked.code.Value());
    auto reused = host.Load(HORO_TEST_GAME_MODULE_PATH, expected);
    REQUIRE(reused.HasError());
    CHECK(reused.ErrorValue().code.Value() == NavigationCapabilityErrors::ScopeRevoked.code.Value());
    fixture.Publish();
    REQUIRE(fixture.context->DrainRetired().Value());
    REQUIRE(fixture.coordinator.IsDrained());
}

TEST_CASE("Lua retained navigation closures and opaque requests survive callbacks with native typed failures") {
    Fixture fixture;
    auto program = LuaBehaviorProgram::
        Compile(Lua("if not retained then retained=ctx.navigation.path(1,0,1,9,0,9) else local value,err=retained:poll(); if value then "
                    "assert(value.path.status==0); ctx.transform.set_position(7,0,0) elseif err then "
                    "assert(err.code=='navigation.data.unavailable'); ctx.transform.set_position(8,0,0) end end"),
                Type(), "navigation.horo_script");
    REQUIRE(program.HasValue());
    BehaviorRegistry registry;
    auto active = ActivateScript(*program.Value(), registry, fixture.context);
    REQUIRE(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{2}).x == 0);
    fixture.Publish();
    REQUIRE(fixture.context->Refresh(fixture.Now(1)).HasValue());
#ifdef HORO_TEST_NAVIGATION_DETOUR
    CHECK(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{2}).x == 7);
#else
    CHECK(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{2}).x == 8);
#endif
    REQUIRE(fixture.coordinator.IsDrained());
}

TEST_CASE("Actual Lua replacement cancels outstanding native queries and suppresses retired VM delivery") {
    Fixture fixture;
    auto program = LuaBehaviorProgram::Compile(Lua("if not retained then retained=ctx.navigation.path(1,0,1,9,0,9) end"), Type(),
                                               "before.horo_script");
    REQUIRE(program.HasValue());
    BehaviorRegistry registry;
    auto active = ActivateScript(*program.Value(), registry, fixture.context);
    REQUIRE(active.runtime->FixedUpdate({}, FixedDeltaTime{0.1}).HasValue());
    auto replacement = LuaBehaviorProgram::Compile(Lua("ctx.transform.set_position(3,0,0)"), Type(), "after.horo_script");
    REQUIRE(replacement.HasValue());
    REQUIRE(program.Value()->ReplaceCompatible(std::move(replacement).Value()).HasValue());
    CHECK(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{2}).x == 3);
    fixture.Publish();
    REQUIRE(fixture.context->Refresh(fixture.Now(1)).HasValue());
    REQUIRE(fixture.coordinator.IsDrained());
}

TEST_CASE("Native move and follow share canonical replacement cancellation and parent context propagation") {
    Fixture fixture;
    CancellationSource parent;
    auto move = fixture.context->Submit(
        {.operation = GameplayNavigationOperation::MoveTo, .destination = {9, 0, 9}, .cancellation = parent.Token()});
    REQUIRE(move.HasValue());
    REQUIRE(fixture.movement->Update(fixture.Now()).HasValue());
    parent.RequestCancellation();
    REQUIRE(fixture.movement->Update(fixture.Now(1)).Value().state == NavigationMovementState::Cancelled);
    auto result = move.Value().Poll();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value());
    CHECK(result.Value()->movement->state == NavigationMovementState::Cancelled);
    fixture.Publish();
    static_cast<void>(fixture.movement->Update(fixture.Now(2)));
}

#ifdef HORO_TEST_NAVIGATION_DETOUR
TEST_CASE("Actual loaded native behavior uses the same Detour result path as Lua") {
    Fixture fixture;
    GameModuleHost host{{}, nullptr, fixture.context};
    const GameModuleLoadExpectation expected{"game.tests", CurrentGameplayBuildFingerprint(),
                                             Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
    auto loaded = host.Load(HORO_TEST_GAME_MODULE_PATH, expected);
    REQUIRE(loaded.HasValue());
    REQUIRE_FALSE(loaded.Value().diagnostic);
    auto definition =
        Tests::SingleBehaviorSceneDefinition(Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}, Runtime::SceneObjectId{2},
                                             BehaviorInstanceId{1}, BehaviorTypeId::Parse("game.tests.navigation_client").Value());
    auto scene = std::move(Runtime::RuntimeScene::Create(definition, Runtime::SceneRuntimeId{Activation().scene.Value()})).Value();
    auto runtime =
        BehaviorRuntime::Create(*scene, loaded.Value().owner->Registry(), {}, nullptr, loaded.Value().owner->NavigationContext());
    REQUIRE(runtime.HasValue());
    CHECK(Tests::FixedUpdateAndReadPosition(*scene, *runtime.Value(), Runtime::SceneObjectId{2}).x == 0);
    fixture.Publish();
    REQUIRE(fixture.context->Refresh(fixture.Now(1)).HasValue());
    CHECK(Tests::FixedUpdateAndReadPosition(*scene, *runtime.Value(), Runtime::SceneObjectId{2}).x == 7);
    runtime.Value()->Shutdown();
    REQUIRE(fixture.context->DrainRetired().Value());
    REQUIRE(fixture.coordinator.IsDrained());
}
#endif

TEST_CASE("Lua missing navigation is a typed unavailable result through the actual behavior runtime") {
    auto program =
        LuaBehaviorProgram::Compile(Lua("local value,err=ctx.navigation.path(1,0,1,9,0,9); assert(value==nil); "
                                        "assert(err.code=='navigation.capability.unavailable'); ctx.transform.set_position(4,0,0)"),
                                    Type(), "unavailable.horo_script");
    REQUIRE(program.HasValue());
    BehaviorRegistry registry;
    auto active = ActivateScript(*program.Value(), registry, {});
    CHECK(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{2}).x == 4);
}

TEST_CASE("Lua move-to and follow expose canonical replacement and explicit cancellation semantics") {
    Fixture fixture;
    auto now = fixture.Now();
    now.target = NavigationMovementTarget{{fixture.binding.scene, 5, 1}, {9, 0, 9}, 1};
    REQUIRE(fixture.context->Refresh(now).HasValue());
    const std::string source = Lua(
        "local first,err=ctx.navigation.move_to(9,0,9); assert(first and not err); local second,denial=ctx.navigation.follow(9,0,9,5,1); "
        "assert(second and not denial); local old=first:poll(); assert(old.movement.state==" +
        std::to_string(static_cast<int>(NavigationMovementState::Superseded)) +
        " ); assert(second:cancel()); local ended=second:poll(); assert(ended.movement.state==" +
        std::to_string(static_cast<int>(NavigationMovementState::Cancelled)) + " ); ctx.transform.set_position(5,0,0)");
    auto program = LuaBehaviorProgram::Compile(source, Type(), "movement.horo_script");
    REQUIRE(program.HasValue());
    BehaviorRegistry registry;
    auto active = ActivateScript(*program.Value(), registry, fixture.context);
    CHECK(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{2}).x == 5);
}

TEST_CASE("Pending actual native behavior navigation keeps SDK code alive through retired host drain") {
    Fixture fixture;
    GameModuleHost host{{}, nullptr, fixture.context};
    const GameModuleLoadExpectation expected{"game.tests", CurrentGameplayBuildFingerprint(),
                                             Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
    auto loading = host.Load(HORO_TEST_GAME_MODULE_PATH, expected);
    REQUIRE(loading.HasValue());
    REQUIRE_FALSE(loading.Value().diagnostic);
    auto module = std::move(loading).Value().owner;
    const auto moduleCancellation = module->Cancellation();
    auto definition =
        Tests::SingleBehaviorSceneDefinition(Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}, Runtime::SceneObjectId{2},
                                             BehaviorInstanceId{1}, BehaviorTypeId::Parse("game.tests.navigation_client").Value());
    auto scene = std::move(Runtime::RuntimeScene::Create(definition, Runtime::SceneRuntimeId{Activation().scene.Value()})).Value();
    auto creation = BehaviorRuntime::Create(*scene, module->Registry(), {}, nullptr, module->NavigationContext());
    REQUIRE(creation.HasValue());
    auto runtime = std::move(creation).Value();
    CHECK(Tests::FixedUpdateAndReadPosition(*scene, *runtime, Runtime::SceneObjectId{2}).x == 0);
    runtime->Shutdown();
    runtime.reset();
    module.reset();
    REQUIRE(moduleCancellation.IsCancellationRequested());
    REQUIRE_FALSE(fixture.context->IsUsable());
    fixture.Publish();
    REQUIRE(fixture.context->DrainRetired().Value());
    REQUIRE(fixture.coordinator.IsDrained());
}

TEST_CASE("Native navigation code pins defer shadow artifact removal until retired handles release") {
    Fixture fixture;
    const auto root = std::filesystem::temp_directory_path() /
                      ("horo nav code image " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path shadow;
    {
        GameModuleHost host{{}, nullptr, fixture.context};
        const GameModuleLoadExpectation expected{"game.tests", CurrentGameplayBuildFingerprint(),
                                                 Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
        auto loading = host.LoadShadowCopy(HORO_TEST_GAME_MODULE_PATH, root, expected);
        REQUIRE(loading.HasValue());
        REQUIRE_FALSE(loading.Value().diagnostic);
        auto module = std::move(loading).Value().owner;
        shadow = module->LoadedArtifactPath();
        auto request = module->NavigationContext()->Submit({.start = {1, 0, 1}, .destination = {9, 0, 9}});
        REQUIRE(request.HasValue());
        module.reset();
        REQUIRE_FALSE(fixture.context->IsUsable());
        REQUIRE(std::filesystem::exists(shadow));
        fixture.Publish();
        REQUIRE(fixture.context->DrainRetired().Value());
        REQUIRE(std::filesystem::exists(shadow));
        REQUIRE(request.Value().Poll().HasError());
    }
    fixture.context.reset();
    REQUIRE_FALSE(std::filesystem::exists(shadow));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

TEST_CASE("Failed native startup retains an owned shadow image only until the revoked scope releases") {
    Fixture fixture;
    const auto root = std::filesystem::temp_directory_path() /
                      ("horo nav failed image " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    auto denied = GameplayNavigationContext::Create({"game.tests", fixture.binding, false, false}, &fixture.world, &fixture.coordinator,
                                                    fixture.movement.get(), Input());
    REQUIRE(denied.HasValue());
    fixture.context = std::move(denied).Value();
    {
        GameModuleHost host{{}, nullptr, fixture.context};
        const GameModuleLoadExpectation expected{"game.tests", CurrentGameplayBuildFingerprint(),
                                                 Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
        auto failed = host.LoadShadowCopy(HORO_TEST_GAME_MODULE_PATH, root, expected);
        REQUIRE(failed.HasValue());
        REQUIRE(failed.Value().diagnostic);
        CHECK(failed.Value().diagnostic->code.Value() == NavigationCapabilityErrors::PermissionDenied.code.Value());
        REQUIRE(failed.Value().owner->TryRetire().HasValue());
        failed.Value().owner.reset();
        REQUIRE_FALSE(fixture.context->IsUsable());
        REQUIRE(std::distance(std::filesystem::directory_iterator{root}, std::filesystem::directory_iterator{}) == 1);
        REQUIRE(fixture.context->DrainRetired().Value());
    }
    fixture.context.reset();
    REQUIRE(std::filesystem::is_empty(root));
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}

#ifdef HORO_TEST_NAVIGATION_DETOUR
TEST_CASE("Completed native navigation result outlives every module scope and code image") {
    Fixture fixture;
    const auto root = std::filesystem::temp_directory_path() /
                      ("horo nav detached result " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::shared_ptr<const GameplayNavigationResult> completed;
    std::filesystem::path shadow;
    {
        GameModuleHost host{{}, nullptr, fixture.context};
        const GameModuleLoadExpectation expected{"game.tests", CurrentGameplayBuildFingerprint(),
                                                 Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
        auto loading = host.LoadShadowCopy(HORO_TEST_GAME_MODULE_PATH, root, expected);
        REQUIRE(loading.HasValue());
        REQUIRE_FALSE(loading.Value().diagnostic);
        auto module = std::move(loading).Value().owner;
        shadow = module->LoadedArtifactPath();
        auto definition = Tests::SingleBehaviorSceneDefinition(Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1},
                                                               Runtime::SceneObjectId{2}, BehaviorInstanceId{1},
                                                               BehaviorTypeId::Parse("game.tests.navigation_client").Value());
        auto scene = std::move(Runtime::RuntimeScene::Create(definition, Runtime::SceneRuntimeId{Activation().scene.Value()})).Value();
        auto creation = BehaviorRuntime::Create(*scene, module->Registry(), {}, nullptr, module->NavigationContext());
        REQUIRE(creation.HasValue());
        auto runtime = std::move(creation).Value();
        CHECK(Tests::FixedUpdateAndReadPosition(*scene, *runtime, Runtime::SceneObjectId{2}).x == 0);
        fixture.Publish();
        CHECK(Tests::FixedUpdateAndReadPosition(*scene, *runtime, Runtime::SceneObjectId{2}).x == 7);
        {
            auto image = Platform::LoadDynamicLibrary(shadow.string());
            REQUIRE(image.HasValue());
            void *symbol = image.Value()->GetSymbol("HoroTestTakeNavigationResult");
            REQUIRE(symbol != nullptr);
            void (*take)(void *){};
            static_assert(sizeof(take) == sizeof(symbol));
            std::memcpy(&take, &symbol, sizeof(take));
            take(&completed);
        }
        REQUIRE(completed);
        REQUIRE(completed->path);
        runtime->Shutdown();
        runtime.reset();
        module.reset();
        REQUIRE(fixture.context->DrainRetired().Value());
    }
    fixture.context.reset();
    REQUIRE_FALSE(std::filesystem::exists(shadow));
    REQUIRE(completed->path);
    CHECK_FALSE(completed->path->points.empty());
    completed.reset();
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
}
#endif
