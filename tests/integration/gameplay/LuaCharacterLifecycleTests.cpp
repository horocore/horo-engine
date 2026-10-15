#include "GameplayCharacterContextTestSupport.h"
#include "Horo/Gameplay/LuaBehavior.h"
#include "Horo/Physics/CharacterErrors.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Gameplay;
    using namespace Horo::Character;

    TEST_CASE("Lua missing Character grant returns typed unavailable and completes a visible callback", "[gameplay-character][lua]") {
        const auto type = BehaviorTypeId::Parse("game.tests.character_missing").Value();
        auto program = LuaBehaviorProgram::Compile(R"(
return horo.behavior { type_id='game.tests.character_missing', display_name='Missing Character',
    on_fixed_update=function(ctx)
        local client, error=ctx.character.acquire()
        assert(client == nil and error.code == 'gameplay.character.unavailable')
        ctx.transform.set_position(17,0,0)
    end }
)",
                                                   type, "character_missing.horo_script");
        REQUIRE(program.HasValue());
        BehaviorRegistry registry;
        REQUIRE(registry.Register(program.Value()->Registration()).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        auto active = Tests::ActivateBehaviorRuntime(Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, type), {8}, registry);
        REQUIRE(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, {1}).x == 17);
    }
#if HORO_TEST_PHYSICS_NATIVE
    using Tests::CharacterIntegration::CharacterModuleHost;

    /** @brief Supplies only actual host-issued controller incarnation metadata to a private script projection. */
    std::string Script(const CharacterModuleHost &host, const bool replacement) {
        const auto slot = std::to_string(host.world.controller.slot.index);
        const auto generation = std::to_string(host.world.controller.slot.generation);
        const auto queue = replacement ? "assert(client:queue(handle,{tick='3',correlation='9007199254740994',velocity={2,0,0}}) ~= nil); "
                                         "ctx.transform.set_position(13,0,0)"
                                       :
                                       R"(if not queued then
                 assert(client:queue(handle,{tick='1',correlation='9007199254740993',velocity={1,0,0}}) ~= nil)
                 queued=true; ctx.transform.set_position(11,0,0)
               else
                 local snapshot=assert(client:snapshot(handle))
                 assert(snapshot.tick == '1' and snapshot.correlation == '9007199254740993')
                 assert(client:queue(handle,{tick='2',correlation='18446744073709551615',velocity={1,0,0}}) ~= nil)
                 ctx.transform.set_position(12,0,0)
               end)";
        return "local client,handle,queued; return horo.behavior {type_id='game.tests.character_script',display_name='Character',"
               "on_fixed_update=function(ctx) if not client then client=assert(ctx.character.acquire()); handle=assert(client:controller(" +
               slot + "," + generation + ")) end; " + queue + " end}";
    }

    /** @brief Advances only the actual canonical Character owner using its paired real Physics queries. */
    void Advance(CharacterModuleHost &host, const std::uint64_t tick) {
        CharacterPhysicsQueryAdapter adapter{*host.world.physics};
        REQUIRE(host.world.character
                    ->AdvanceFixedTick({.tick = tick,
                                        .sceneGeneration = 1,
                                        .fixedDelta = Duration::FromNanoseconds(16'666'667),
                                        .query = adapter.Context(host.world.Expectations(tick))})
                    .HasValue());
    }

    TEST_CASE("Lua replacement revokes queued Character work before the old VM next runs and preserves exact correlation",
              "[gameplay-character][lua][lifetime][native]") {
        CharacterModuleHost host{8};
        const auto type = BehaviorTypeId::Parse("game.tests.character_script").Value();
        auto compiled = LuaBehaviorProgram::Compile(Script(host, false), type, "character_script.horo_script");
        REQUIRE(compiled.HasValue());
        auto program = std::move(compiled).Value();
        BehaviorRegistry registry;
        REQUIRE(registry.Register(program->Registration()).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        auto scene = Runtime::RuntimeScene::Create(Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, type), {8}).Value();
        auto created = BehaviorRuntime::Create(*scene, registry, {}, {.physics = host.physics, .character = host.character});
        REQUIRE(created.HasValue());
        auto behavior = std::move(created).Value();
        REQUIRE(Tests::FixedUpdateAndReadPosition(*scene, *behavior, {1}).x == 11);
        auto incompatible =
            LuaBehaviorProgram::Compile("return horo.behavior {type_id='game.tests.other_character',display_name='Other'}",
                                        BehaviorTypeId::Parse("game.tests.other_character").Value(), "other_character.horo_script");
        REQUIRE(incompatible.HasValue());
        REQUIRE(program->ReplaceCompatible(std::move(incompatible).Value()).HasError());
        Advance(host, 1);
        REQUIRE(host.world.character->ControllerLocomotionSnapshot(host.world.controller).Value().movement.sequence ==
                9'007'199'254'740'993ULL);
        REQUIRE(Tests::FixedUpdateAndReadPosition(*scene, *behavior, {1}).x == 12);
        REQUIRE(host.world.character->TickStatistics().pendingCommands == 1);
        auto candidate = LuaBehaviorProgram::Compile(Script(host, true), type, "character_script.horo_script");
        REQUIRE(candidate.HasValue());
        REQUIRE(program->ReplaceCompatible(std::move(candidate).Value()).HasValue());
        Advance(host, 2);  // No further Lua callback has run; replacement must already have revoked the pending UINT64_MAX intent.
        REQUIRE(host.world.character->TickStatistics().pendingCommands == 0);
        REQUIRE(host.world.character->ControllerLocomotionSnapshot(host.world.controller).Value().tick == 1);
        REQUIRE(Tests::FixedUpdateAndReadPosition(*scene, *behavior, {1}).x == 13);
        Advance(host, 3);
        const auto snapshot = host.world.character->ControllerLocomotionSnapshot(host.world.controller).Value();
        REQUIRE(snapshot.tick == 3);
        REQUIRE(snapshot.movement.sequence == 9'007'199'254'740'994ULL);
        behavior->Shutdown();
        REQUIRE(host.character->Acquire("game.tests", 8, 1).HasValue());  // Runtime retirement never closes the host anchor.
    }

    /** @brief Builds a real script runtime with only host-issued routing; no test-only Character provider is substituted. */
    Tests::ActiveBehaviorRuntime ScriptRuntime(CharacterModuleHost &host, const std::string &source, BehaviorRegistry &registry,
                                               std::shared_ptr<LuaBehaviorProgram> &program) {
        const auto type = BehaviorTypeId::Parse("game.tests.character_script").Value();
        auto compiled = LuaBehaviorProgram::Compile(source, type, "character_script.horo_script");
        REQUIRE(compiled.HasValue());
        program = std::move(compiled).Value();
        REQUIRE(registry.Register(program->Registration()).HasValue());
        REQUIRE(registry.Freeze().HasValue());
        auto scene = Runtime::RuntimeScene::Create(Tests::SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, type), {8}).Value();
        auto runtime = BehaviorRuntime::Create(*scene, registry, {}, {.physics = host.physics, .character = host.character});
        REQUIRE(runtime.HasValue());
        return {std::move(scene), std::move(runtime).Value()};
    }

    TEST_CASE("Lua Character rejects malformed bounded raw requests without authored metamethods or partial queued intent",
              "[gameplay-character][lua][native]") {
        CharacterModuleHost host{8};
        BehaviorRegistry registry;
        std::shared_ptr<LuaBehaviorProgram> program;
        const std::string source =
            "return horo.behavior {type_id='game.tests.character_script',display_name='Character',"
            "on_fixed_update=function(ctx) local client=assert(ctx.character.acquire()); local handle=assert(client:controller(" +
            std::to_string(host.world.controller.slot.index) + "," + std::to_string(host.world.controller.slot.generation) + R"())
            local calls=0
            local malformed={
                {tick='18446744073709551616',correlation='1'},
                {tick='1',correlation=1.0},
                {tick='1',correlation='1',velocity={0/0,0,0}},
                {tick='1',correlation='1', ['velocity\0ignored']={1,0,0}},
                {tick='1',correlation='1',filter={layer='42345678-1234-4234-8234-123456789abc\0ignored'}},
                {tick='1',correlation='1',filter={excluded_body={0,-1}}},
                setmetatable({}, {__index=function() calls=calls+1; return 1 end}),
                {tick='1',correlation='1',velocity=setmetatable({1,0,0,0},{__len=function() error('authored length') end})}
            }
            for _,request in ipairs(malformed) do
                local accepted,failure=client:queue(handle,request)
                assert(accepted==nil and failure.code=='character.request.invalid')
            end
            assert(calls==0)
            local bad,failure=client:controller(-1,1)
            assert(bad==nil and failure.code=='character.handle.malformed')
            ctx.transform.set_position(14,0,0)
        end })";
        auto active = ScriptRuntime(host, source, registry, program);
        REQUIRE(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, {1}).x == 14);
        REQUIRE(host.world.character->TickStatistics().pendingCommands == 0);
    }

    TEST_CASE("Lua callback failure revokes already queued Character intent before canonical command closure",
              "[gameplay-character][lua][native][lifetime]") {
        CharacterModuleHost host{8};
        BehaviorRegistry registry;
        std::shared_ptr<LuaBehaviorProgram> program;
        const std::string source =
            "return horo.behavior {type_id='game.tests.character_script',display_name='Character',"
            "on_fixed_update=function(ctx) local client=assert(ctx.character.acquire()); local handle=assert(client:controller(" +
            std::to_string(host.world.controller.slot.index) + "," + std::to_string(host.world.controller.slot.generation) + R"())
            assert(client:queue(handle,{tick='1',correlation='968',velocity={1,0,0}}) ~= nil)
            ctx.transform.set_position(15,0,0)
            error('intentional script failure after queue admission')
        end })";
        auto active = ScriptRuntime(host, source, registry, program);
        REQUIRE(Tests::FixedUpdateAndReadPosition(*active.scene, *active.runtime, {1}).x == 15);
        Advance(host, 1);
        REQUIRE(host.world.character->TickStatistics().pendingCommands == 0);
        Physics::Test::RequireError(host.world.character->ControllerLocomotionSnapshot(host.world.controller),
                                    CharacterErrors::InvalidState);
        REQUIRE(host.character->Acquire("game.tests", 8, 1).HasValue());
    }
#endif
}  // namespace
