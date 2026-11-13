#include "ScriptModuleHostTestSupport.h"
#include "editor/gameplay/EditorPlaySessionController.h"

#include <chrono>

namespace Horo::Editor::Tests {
    using namespace Extensions;
    using namespace Extensions::Tests::ModuleHostSupport;

    namespace {
        Gameplay::BehaviorRegistry EmptyRegistry() {
            Gameplay::BehaviorRegistry registry;
            Accept(registry.Freeze());
            return registry;
        }

        SceneDocumentSnapshot Authoring() {
            SceneObjectComponentSet camera;
            camera.camera = Runtime::CameraComponent{};
            return {DocumentRevision{7},
                    DocumentStateId{11},
                    {{SceneObjectId{1}, std::nullopt, "Camera", {}, std::nullopt, std::move(camera)}}};
        }

        ScriptCallResult AwaitPlay(EditorPlaySessionController &play) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
            do {
                play.PresentationUpdate(Gameplay::FrameDeltaTime{0.001});
                if (auto result = play.TakeModuleScriptResult())
                    return std::move(*result);
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            FAIL("actual owner Play module operation did not settle within its bounded test deadline");
            return {};
        }

        void Answer42(const ScriptCallResult &result) {
            REQUIRE(result.IsSuccess());
            REQUIRE(result.values.size() == 1);
            REQUIRE(result.values[0].AsSignedInteger());
            CHECK(*result.values[0].AsSignedInteger() == 42);
        }
    }  // namespace

    TEST_CASE("Both real module VMs execute in actual Editor Play Scene-owned contexts without a GUI",
              "[Editor][Play][ScriptModule][TwoVm][Native]") {
        const ScriptModuleRuntimeKind vms[]{ScriptModuleRuntimeKind::Lua54, ScriptModuleRuntimeKind::JavaScriptNg};
        for (const auto vm : vms) {
            SignedPackage package{vm, SumSource(vm)};
            package.Capture();
            Interactive host{package};
            auto registry = EmptyRegistry();
            EditorPlaySessionController play;
            Accept(play.ConfigureModuleScripts(host.modules));
            Accept(play.Start(Authoring(), registry));
            REQUIRE(play.State() == EditorPlaySessionState::Playing);
            REQUIRE(play.SceneView());
            const auto scene = play.SceneView()->RuntimeId();
            Accept(play.BeginModuleScript(package.Program(), OperationPolicy()));
            Answer42(AwaitPlay(play));
            CHECK(play.SceneView()->RuntimeId() == scene);
            Accept(play.Pause());
            Accept(play.BeginModuleScript(package.Program(), OperationPolicy()));
            Answer42(AwaitPlay(play));
            play.Stop();
            CHECK(play.State() == EditorPlaySessionState::Idle);
            CHECK(play.BeginModuleScript(package.Program(), OperationPolicy()).HasError());
        }
    }

    TEST_CASE("Actual Play reload revokes the previous module epoch and admits the restored runtime",
              "[Editor][Play][ScriptModule][Reload]") {
        SignedPackage package{ScriptModuleRuntimeKind::Lua54, SumSource(ScriptModuleRuntimeKind::Lua54)};
        package.Capture();
        Interactive host{package};
        auto registry = EmptyRegistry();
        EditorPlaySessionController play;
        Accept(play.ConfigureModuleScripts(host.modules));
        Accept(play.Start(Authoring(), registry));
        Accept(play.BeginModuleScript(package.Program(), OperationPolicy()));
        auto snapshot = Take(play.QuiesceForReload());
        CHECK_FALSE(play.TakeModuleScriptResult());
        CHECK(play.BeginModuleScript(package.Program(), OperationPolicy()).HasError());
        Accept(play.RestoreAfterReload(registry, snapshot));
        Accept(play.BeginModuleScript(package.Program(), OperationPolicy()));
        Answer42(AwaitPlay(play));
        play.Stop();
    }

    TEST_CASE("Unready Play cannot obtain an operation even with signed sources and a genuine native host",
              "[Editor][Play][ScriptModule][Policy]") {
        SignedPackage package{ScriptModuleRuntimeKind::JavaScriptNg, SumSource(ScriptModuleRuntimeKind::JavaScriptNg)};
        package.Capture();
        Interactive host{package};
        EditorPlaySessionController play;
        Accept(play.ConfigureModuleScripts(host.modules));
        CHECK(play.BeginModuleScript(package.Program(), OperationPolicy()).HasError());
    }
}  // namespace Horo::Editor::Tests
