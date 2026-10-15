#include "GameplayRuntimeTestSupport.h"
#include "GameplayWorldComposition.h"
#include "Horo/Gameplay/LuaBehavior.h"
#include "RuntimeUiCapabilityTestSupport.h"

namespace Horo::Tests::UiAccess {
    namespace {
        using namespace Gameplay;

        BehaviorTypeId Type() {
            return Take(BehaviorTypeId::Parse("game.tests.runtime_ui"));
        }

        /** @brief Valid source admitted through the real sandbox/compiler, registry and BehaviorRuntime. */
        std::string Source(const bool queue) {
            const std::string body = queue ? R"(
                on_create=function(ctx)
                    local request,err=ctx.ui:screen(0,'00000000000000000000000000000009')
                    assert(request~=nil and err==nil)
                end,
                on_fixed_update=function(ctx)
                    assert(ctx.ui:screen(1)~=nil)
                end,
            )"
                                           : "";
            return "return horo.behavior { type_id='game.tests.runtime_ui', display_name='Runtime UI'," + body + "}";
        }

        ActiveBehaviorRuntime Activate(LuaBehaviorProgram &program, BehaviorRegistry &registry,
                                       const std::shared_ptr<const RuntimeUiClient> &client) {
            REQUIRE(registry.Register(program.Registration()).HasValue());
            REQUIRE(registry.Freeze().HasValue());
            auto definition = SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, Type());
            auto scene = Take(Runtime::RuntimeScene::Create(definition, {101}));
            auto runtime = Take(BehaviorRuntime::Create(*scene, registry, {}, {}, {}, client));
            return {std::move(scene), std::move(runtime)};
        }
    }  // namespace

    TEST_CASE("Real Lua source replacement revokes queued UI immediately while failed reload preserves prior scope",
              "[runtime-ui][lua][reload]") {
        Fixture fixture;
        auto client = fixture.Client();
        auto program = Take(LuaBehaviorProgram::Compile(Source(true), Type(), "runtime_ui.horo_script"));
        BehaviorRegistry registry;
        auto active = Activate(*program, registry, client);
        REQUIRE(program->ReplaceCompatible({}).HasError());
        REQUIRE(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
        CHECK(Take(client->ScreenGuard()).revision.Value() == 2);
        REQUIRE(active.runtime->FixedUpdate({}, {1.0 / 60.0}).HasValue());
        auto candidate = Take(LuaBehaviorProgram::Compile(Source(false), Type(), "runtime_ui_replacement.horo_script"));
        REQUIRE(program->ReplaceCompatible(std::move(candidate)).HasValue());
        // No subsequent callback/VM recreation is needed for the stale queue to be fenced.
        CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 1);
        CHECK(Take(client->ScreenGuard()).revision.Value() == 2);
        REQUIRE(active.runtime->FixedUpdate({}, {1.0 / 60.0}).HasValue());
        active.runtime->Shutdown();
        CHECK(client->Validate().HasValue());
    }

    TEST_CASE("Real Lua VM shutdown revokes admitted UI before queued publication", "[runtime-ui][lua][lifetime]") {
        Fixture fixture;
        auto client = fixture.Client();
        auto program = Take(LuaBehaviorProgram::Compile(Source(true), Type(), "runtime_ui_shutdown.horo_script"));
        BehaviorRegistry registry;
        auto active = Activate(*program, registry, client);
        active.runtime->Shutdown();
        active.runtime.reset();
        CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 0);
        CHECK(Take(client->ScreenGuard()).revision.Value() == 1);
        CHECK(client->Validate().HasValue());
    }

    TEST_CASE("Real Lua missing UI retains typed diagnostics instead of selecting an ambient owner", "[runtime-ui][lua]") {
        const auto source = R"(
            return horo.behavior {type_id='game.tests.runtime_ui',display_name='Runtime UI',
                on_fixed_update=function(ctx)
                    local value,err=ctx.ui:screen_guard()
                    assert(value==nil and err.code=='runtime_ui.capability.unavailable')
                    ctx.transform.set_position(7,0,0)
                end}
        )";
        auto program = Take(LuaBehaviorProgram::Compile(source, Type(), "ui_missing.horo_script"));
        BehaviorRegistry registry;
        auto active = Activate(*program, registry, {});
        REQUIRE(active.runtime->FixedUpdate({}, {1.0 / 60.0}).HasValue());
        auto entity = active.scene->View().Find({1});
        REQUIRE(entity);
        CHECK(Take(active.scene->View().Get(*entity)).localTransform->translation.x == 7);
    }

    TEST_CASE("Real Lua UI permissions use the same denied outcome as native clients", "[runtime-ui][lua][permission]") {
        Fixture fixture;
        auto client = fixture.Client(RuntimeUiPermission::Elements);
        const auto source = R"(
            return horo.behavior {type_id='game.tests.runtime_ui',display_name='Runtime UI',
                on_fixed_update=function(ctx)
                    local value,err=ctx.ui:screen_guard()
                    assert(value==nil and err.code=='runtime_ui.capability.denied')
                    ctx.transform.set_position(9,0,0)
                end}
        )";
        auto program = Take(LuaBehaviorProgram::Compile(source, Type(), "ui_denied.horo_script"));
        BehaviorRegistry registry;
        auto active = Activate(*program, registry, client);
        REQUIRE(active.runtime->FixedUpdate({}, {1.0 / 60.0}).HasValue());
        auto entity = active.scene->View().Find({1});
        REQUIRE(entity);
        CHECK(Take(active.scene->View().Get(*entity)).localTransform->translation.x == 9);
    }

    TEST_CASE("Actual application Gameplay composition validates explicit UI principal and revokes on withdrawal",
              "[runtime-ui][application][lua]") {
        Fixture fixture;
        auto client = fixture.Client();
        auto definition = SingleBehaviorSceneDefinition({1}, {1}, {1}, {1}, Type());
        auto scene = Take(Runtime::RuntimeScene::Create(definition, {101}));
        const std::filesystem::path root{HORO_TEST_SOURCE_ROOT};
        Application::Internal::GameplayWorldSelection selection{.moduleId = "game.tests",
                                                                .scriptSource =
                                                                    root / "tests/fixtures/gameplay_runtime_ui/RuntimeUi.horo_script",
                                                                .scriptSidecar =
                                                                    root / "tests/fixtures/gameplay_runtime_ui/RuntimeUi.horo_script.meta",
                                                                .runtimeUi = client};
        CHECK(Application::Internal::GameplayWorldComposition::Create(*scene, nullptr, 2, selection).HasError());
        auto active = Take(Application::Internal::GameplayWorldComposition::Create(*scene, nullptr, 1, selection));
        REQUIRE(active->FixedUpdate({1.0 / 60.0}).HasValue());
        active.reset();
        CHECK(Take(fixture.service.ApplyQueuedCommands(RuntimeUiCommandCutoff::OwnerCommands)) == 0);
        CHECK(Take(client->ScreenGuard()).revision.Value() == 1);
        CHECK(client->Validate().HasValue());
    }
}  // namespace Horo::Tests::UiAccess
