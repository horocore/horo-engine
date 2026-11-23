#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "Horo/Gameplay/GameplayFormation.h"
#include "Horo/Gameplay/GameplayNavigationContext.h"
#include "Horo/Gameplay/LuaBehavior.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "navigation/NavigationCoordinatorTestFixtures.h"
#ifdef HORO_TEST_NAVIGATION_DETOUR
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#endif
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
using namespace Horo;
using namespace Horo::Gameplay;
using namespace Horo::Navigation;
using namespace Horo::Navigation::CoordinatorTestSupport;

BehaviorDescriptor HoroGeneratedBehaviorDescriptor_GuardTargetBeacon();
BehaviorFactoryBinding HoroGeneratedBehaviorFactory_GuardTargetBeacon();

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

        explicit Fixture(std::string moduleId = "game.tests") {
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
            auto bound = GameplayNavigationContext::Create({std::move(moduleId), binding, true, true}, &world, &coordinator, movement.get(),
                                                           Input());
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
        auto runtime = BehaviorRuntime::Create(*scene, registry, {}, {.navigation = std::move(navigation)});
        REQUIRE(runtime.HasValue());
        return {std::move(scene), std::move(runtime).Value()};
    }

    /** @brief Places the shipped beacon and pursuit behavior on two root entities. */
    Runtime::RuntimeSceneDefinition GuardPursuitScene(const BehaviorTypeId &pursuit, const BehaviorTypeId &beacon) {
        const std::vector<BehaviorField> channel{{"channel", std::string{"test guard"}}};
        auto pursuitFields = channel;
        pursuitFields.push_back({"assist_reaction_seconds", 0.125});
        Runtime::RuntimeComponentSet guardComponents, beaconComponents;
        guardComponents.behaviors.emplace_back(BehaviorInstanceId{1}, pursuit, 1, true, std::move(pursuitFields));
        beaconComponents.behaviors.emplace_back(BehaviorInstanceId{2}, beacon, 1, true, channel);
        Runtime::SceneDefinitionBuilder builder{Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}};
        builder.Add({Runtime::SceneObjectId{2}, std::nullopt, {}, std::nullopt, std::move(guardComponents)});
        Math::Transform position;
        position.translation = {9, 0, 9};
        builder.Add({Runtime::SceneObjectId{3}, std::nullopt, position, std::nullopt, std::move(beaconComponents)});
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        return std::move(definition).Value();
    }

#ifdef HORO_TEST_NAVIGATION_DETOUR
    /** @brief Runs the commissioned route through Detour and checks its canonical movement intent. */
    void VerifyPursuitRoute(Fixture &fixture, NavigationMovementObservation current) {
        const auto querying = fixture.movement->Update(current);
        REQUIRE(querying.HasValue());
        REQUIRE(querying.Value().state == NavigationMovementState::Querying);
        REQUIRE(fixture.coordinator.Dispatch(1) == 1);
        fixture.jobs.Shutdown(ShutdownPolicy::Drain);
        const std::array callers{Caller()};
        REQUIRE(fixture.coordinator.Commit(Current(callers, 2)) == 1);
        current.path.tick = 2;
        const auto following = fixture.movement->Update(current);
        REQUIRE(following.HasValue());
        REQUIRE(following.Value().state == NavigationMovementState::Following);
        CHECK(following.Value().desiredVelocity.x > 0);
    }
#endif
}  // namespace

#ifdef HORO_TEST_NAVIGATION_DETOUR
TEST_CASE("Native formation projects reachable slots and withdraws goals from retired topology", "[gameplay][navigation][formation]") {
    auto world = NavigationWorldLifecycle::Create(4).Value();
    Activate(world, Provider());
    const auto pinned = world.Acquire(World());
    REQUIRE(pinned.HasValue());
    auto registry = NavigationAgentRegistry::Create({.maximumAgents = 3}).Value();
    const NavigationAgentSceneBinding binding{World(), Activation().scene, Activation().sceneGeneration};
    std::array<NavigationAgentDescriptor, 3> descriptors{};
    for (std::uint32_t index = 0; index < descriptors.size(); ++index) {
        descriptors[index].owner = {binding.scene, index + 1, 1};
        descriptors[index].profile = NavigationAgentProfileId::Create(1).Value();
        descriptors[index].filter = Request().filter;
    }
    auto candidate = registry.PrepareScene(binding, descriptors).Value();
    candidate->Publish();
    const auto agents = registry.Snapshot().Value();
    const std::array members{GameplayFormationMember{agents.Agents()[0].handle, {4, 0.3F, 4}},
                             GameplayFormationMember{agents.Agents()[1].handle, {3, 0.3F, 4}},
                             GameplayFormationMember{agents.Agents()[2].handle, {2, 0.3F, 4}}};
    const std::array offsets{Math::Vec3{1, 0, 0}, Math::Vec3{20, 0, 0}};
    const auto proposed = PrepareGameplayFormation({.group = GameplayFormationId::Create(1).Value(),
                                                    .revision = 1,
                                                    .shape = GameplayFormationShape::Custom,
                                                    .customSlots = offsets},
                                                   agents, members);
    REQUIRE(proposed.HasValue());
    const GameplayFormationQueryPolicy policy{.projection = {NavigationQueryKind::NearestPoint,
                                                             NavigationQualityLevel::Balanced,
                                                             {64, 16, 100}},
                                              .path = Request().requirement,
                                              .projectionHalfExtents = {1, 1, 1}};
    const auto resolved = ResolveGameplayFormation(proposed.Value(), agents, pinned.Value(), policy);
    if (resolved.HasError())
        UNSCOPED_INFO(resolved.ErrorValue().code.Value());
    REQUIRE(resolved.HasValue());
    CHECK(resolved.Value().availability == GameplayFormationAvailability::Degraded);
    CHECK(resolved.Value().source == Activation());
    REQUIRE(resolved.Value().goals.size() == 2);
    CHECK(resolved.Value().goals[1].agent == members[1].agent);
    CHECK(resolved.Value().goals[1].groundTarget == Math::Vec3{5, 0, 4});
    CHECK(resolved.Value().goals[1].movementTarget == Math::Vec3{5, 0.3F, 4});
    REQUIRE(resolved.Value().unassigned.size() == 1);
    CHECK(resolved.Value().unassigned.front() == members[2].agent);

    auto disconnected = proposed.Value();
    disconnected.slots.back().start = {100, 0, 4};
    disconnected.slots.back().unprojectedTarget = {6, 0, 4};
    const auto inaccessible = ResolveGameplayFormation(disconnected, agents, pinned.Value(), policy);
    REQUIRE(inaccessible.HasValue());
    CHECK(inaccessible.Value().goals.size() == 2);
    CHECK(inaccessible.Value().unassigned == resolved.Value().unassigned);

    auto blocked = proposed.Value();
    for (auto &slot : blocked.slots)
        slot.unprojectedTarget.x += 100.0F;
    const auto dissolved = ResolveGameplayFormation(blocked, agents, pinned.Value(), policy);
    REQUIRE(dissolved.HasValue());
    CHECK(dissolved.Value().availability == GameplayFormationAvailability::Dissolved);
    CHECK(dissolved.Value().goals.empty());
    CHECK(dissolved.Value().unassigned.size() == members.size());

    CancellationSource cancelled;
    cancelled.RequestCancellation();
    REQUIRE(ResolveGameplayFormation(proposed.Value(), agents, pinned.Value(), policy, cancelled.Token()).HasError());
    world.BeginShutdown();
    const auto stale = ResolveGameplayFormation(proposed.Value(), agents, pinned.Value(), policy);
    REQUIRE(stale.HasError());
    CHECK(stale.ErrorValue().code.Value() == NavigationErrors::QueryCancelled.code.Value());
}

TEST_CASE("Formation goals start only their exact live movement controllers", "[gameplay][navigation][formation]") {
    Fixture fixture;
    const auto agent = fixture.movement->Agent();
    const auto group = GameplayFormationId::Create(4).Value();
    const auto current = fixture.Now();
    GameplayFormationResolution resolved{.group = group,
                                         .revision = 2,
                                         .source = Activation(),
                                         .availability = GameplayFormationAvailability::Complete,
                                         .goals = {{0, agent, {4, 0, 1}, {4, 0, 1}}}};
    const auto stale = fixture.context->SubmitFormationSlot(resolved, group, 3);
    REQUIRE(stale.HasError());
    CHECK(stale.ErrorValue().code.Value() == NavigationErrors::StaleSnapshot.code.Value());
    const auto exact = resolved.goals.front().agent;
    ++resolved.goals.front().agent.slot.generation;
    const auto foreign = fixture.context->SubmitFormationSlot(resolved, group, 2);
    REQUIRE(foreign.HasError());
    CHECK(foreign.ErrorValue().code.Value() == NavigationErrors::NoNavigationData.code.Value());
    resolved.goals.front().agent = exact;

    auto admitted = fixture.context->SubmitFormationSlot(resolved, group, 2);
    REQUIRE(admitted.HasValue());
    REQUIRE(fixture.movement->Update(current).HasValue());
    fixture.Publish();
    const auto following = fixture.movement->Update(fixture.Now(2));
    REQUIRE(following.HasValue());
    CHECK(following.Value().operation.agent == agent);
    CHECK(following.Value().state == NavigationMovementState::Following);
    CHECK(following.Value().desiredVelocity.x > 0);
    CHECK(following.Value().desiredVelocity.z == 0);
    REQUIRE(admitted.Value().Cancel().HasValue());
    const auto stopped = fixture.movement->Update(fixture.Now(3));
    REQUIRE(stopped.HasValue());
    CHECK(stopped.Value().state == NavigationMovementState::Cancelled);
}
#endif

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

TEST_CASE("Busy native retirement closes retained navigation admission before runtime leases drain") {
    Fixture fixture;
    GameModuleHost host{{}, nullptr, fixture.context};
    const GameModuleLoadExpectation expected{"game.tests", CurrentGameplayBuildFingerprint(),
                                             Tests::ReadDescriptorRevision(HORO_TEST_GAME_MODULE_REVISION_PATH)};
    auto loading = host.Load(HORO_TEST_GAME_MODULE_PATH, expected);
    REQUIRE(loading.HasValue());
    REQUIRE_FALSE(loading.Value().diagnostic);
    auto module = std::move(loading).Value().owner;
    const auto retained = module->NavigationContext();
    auto definition =
        Tests::SingleBehaviorSceneDefinition(Runtime::SceneDefinitionId{1}, Runtime::SceneDefinitionRevision{1}, Runtime::SceneObjectId{2},
                                             BehaviorInstanceId{1}, BehaviorTypeId::Parse("game.tests.navigation_client").Value());
    auto scene = std::move(Runtime::RuntimeScene::Create(definition, Runtime::SceneRuntimeId{Activation().scene.Value()})).Value();
    auto creation = BehaviorRuntime::Create(*scene, module->Registry(), {}, {.navigation = retained});
    REQUIRE(creation.HasValue());
    auto runtime = std::move(creation).Value();
    auto pending = retained->Submit({.start = {1, 0, 1}, .destination = {9, 0, 9}});
    REQUIRE(pending.HasValue());

    SECTION("retirement requested") {
        module->RequestRetirement();
    }
    SECTION("reload refused while runtime lease is live") {
        auto reload = module->PrepareReload();
        REQUIRE(reload.HasError());
        CHECK(reload.ErrorValue().code.Value() == GameplayErrors::GameplayReloadRestartRequired.code.Value());
    }
    REQUIRE_FALSE(retained->IsUsable());
    auto rejected = retained->Submit({.start = {1, 0, 1}, .destination = {9, 0, 9}});
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == NavigationCapabilityErrors::ScopeRevoked.code.Value());
    auto delivery = pending.Value().Poll();
    REQUIRE(delivery.HasError());
    CHECK(delivery.ErrorValue().code.Value() == NavigationCapabilityErrors::ScopeRevoked.code.Value());
    REQUIRE(module->TryRetire().HasError());
    runtime->Shutdown();
    runtime.reset();
    REQUIRE(module->TryRetire().HasValue());
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
        BehaviorRuntime::Create(*scene, loaded.Value().owner->Registry(), {}, {.navigation = loaded.Value().owner->NavigationContext()});
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

TEST_CASE("Lua assist preferences are detached fixed-step values and remain absent when withheld") {
    const bool optsIn = GENERATE(false, true);
    std::string source = R"lua(
        return horo.behavior {
            type_id = "game.tests.navigation", display_name = "Assist probe",
    )lua";
    if (optsIn)
        source += "uses_assist_preferences = true,";
    source += R"lua(
            on_fixed_update = function(ctx, dt, assists)
                if not assists then
                    ctx.transform.set_position(2, 0, 0)
                    return
                end
                assert(assists.aim_assist == 0.5 and assists.auto_aim_snap)
                assert(assists.aggression_reduction == 0.75 and assists.reaction_time_multiplier == 2)
                assert(assists.skip_quick_time_events and assists.post_hit_invulnerability_seconds == 0.25)
                assert(assists.damage_multiplier == 0.5)
                assists.damage_multiplier = 9
                ctx.transform.set_position(1, 0, 0)
            end,
            on_presentation_update = function(ctx, dt, assists)
                assert(assists == nil)
            end
        }
    )lua";
    auto program = LuaBehaviorProgram::Compile(source, Type(), "assist-probe.horo_script");
    REQUIRE(program.HasValue());
    BehaviorRegistry registry;
    auto active = ActivateScript(*program.Value(), registry, {});
    const GameplayAssistPreferences assists{0.5F, true, 0.75F, 2.0F, true, 0.25F, 0.5F};
    const auto actor = active.scene->View().Find(Runtime::SceneObjectId{2});
    REQUIRE(actor);
    REQUIRE(active.runtime->FixedUpdate({}, FixedDeltaTime{0.1}, nullptr, &assists).HasValue());
    CHECK(active.scene->View().Get(*actor).Value().localTransform->translation.x == (optsIn ? 1.0F : 2.0F));
    CHECK(assists.damageMultiplier == 0.5F);
    active.runtime->PresentationUpdate(FrameDeltaTime{0.1});
    CHECK(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, Runtime::SceneObjectId{2}).x == 2.0F);
}

TEST_CASE("guard pursuit respects fixed-step reaction assists and its existing Navigation grant") {
    enum class Mode {
        NoPreferences,
        Delayed,
        NavigationWithheld
    };
    const auto mode = GENERATE(Mode::NoPreferences, Mode::Delayed, Mode::NavigationWithheld);
    const GameplayAssistPreferences assists{.reactionTimeMultiplier = 3.0F};
    constexpr FixedDeltaTime delta{0.125};
    Fixture fixture("game.example.guard");
    const auto script = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "examples/gameplay-ai/Assets/Scripts/GuardPursuit.horo_script";
    std::ifstream input{script, std::ios::binary};
    REQUIRE(input.is_open());
    const std::string source{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    const auto pursuit = BehaviorTypeId::Parse("game.example.guard.pursuit").Value();
    auto program = LuaBehaviorProgram::Compile(source, pursuit, "GuardPursuit.horo_script");
    REQUIRE(program.HasValue());
    auto beacon = HoroGeneratedBehaviorDescriptor_GuardTargetBeacon();
    const auto definition = GuardPursuitScene(pursuit, beacon.typeId);
    BehaviorRegistry registry;
    REQUIRE(registry.Register({std::move(beacon), HoroGeneratedBehaviorFactory_GuardTargetBeacon()}).HasValue());
    REQUIRE(registry.Register(program.Value()->Registration()).HasValue());
    REQUIRE(registry.Freeze().HasValue());
    auto scene = Runtime::RuntimeScene::Create(definition, Runtime::SceneRuntimeId{fixture.binding.scene.Value()});
    REQUIRE(scene.HasValue());
    auto runtime =
        BehaviorRuntime::Create(*scene.Value(), registry, {}, {.navigation = mode == Mode::NavigationWithheld ? nullptr : fixture.context});
    REQUIRE(runtime.HasValue());
    const auto target = scene.Value()->View().Find(Runtime::SceneObjectId{3});
    REQUIRE(target);
    auto now = fixture.Now(1);
    now.target = NavigationMovementTarget{{fixture.binding.scene, target->entity.index, target->entity.generation}, {9, 0, 9}, 1};
    REQUIRE(fixture.context->Refresh(now).HasValue());

    const auto tick = [&runtime, &assists, mode, delta] {
        REQUIRE(runtime.Value()->FixedUpdate({}, delta, nullptr, mode == Mode::NoPreferences ? nullptr : &assists).HasValue());
    };
    tick();
    CHECK(fixture.movement->Status().state == NavigationMovementState::Idle);
    if (mode != Mode::NoPreferences) {
        tick();
        CHECK(fixture.movement->Status().state == NavigationMovementState::Idle);
        tick();
        CHECK(fixture.movement->Status().state == NavigationMovementState::Idle);
    }
    tick();
    if (mode == Mode::NavigationWithheld) {
        CHECK(fixture.movement->Status().state == NavigationMovementState::Idle);
        return;
    }
    REQUIRE(fixture.movement->Status().state == NavigationMovementState::Querying);
    const auto operation = fixture.movement->Status().id;
#ifdef HORO_TEST_NAVIGATION_DETOUR
    VerifyPursuitRoute(fixture, now);
#endif
    const auto retainedState = fixture.movement->Status().state;
    Runtime::SceneCommandBuffer commands;
    commands.Destroy(*target);
    REQUIRE(scene.Value()->Commit(std::move(commands)).HasValue());
    tick();
    CHECK(fixture.movement->Status().state == retainedState);
    CHECK(fixture.movement->Status().id == operation);
    tick();
    CHECK(fixture.movement->Status().state == NavigationMovementState::Cancelled);
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
    auto creation = BehaviorRuntime::Create(*scene, module->Registry(), {}, {.navigation = module->NavigationContext()});
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
        auto candidate = std::move(failed).Value();
        REQUIRE(candidate.diagnostic);
        CHECK(candidate.diagnostic->code.Value() == NavigationCapabilityErrors::PermissionDenied.code.Value());
        REQUIRE(candidate.owner->TryRetire().HasValue());
        candidate.owner.reset();
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
        auto creation = BehaviorRuntime::Create(*scene, module->Registry(), {}, {.navigation = module->NavigationContext()});
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
