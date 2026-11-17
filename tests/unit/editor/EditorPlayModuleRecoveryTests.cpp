#include "ScriptModuleProductRecoverySupport.h"
#include "editor/gameplay/EditorPlaySessionController.h"

namespace Horo::Editor::Tests {
    using namespace Extensions;
    using namespace Extensions::Tests::ProductRecoverySupport;

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
            FAIL("actual Play recovery operation exceeded its bounded deadline");
            return {};
        }
    }  // namespace

    TEST_CASE("Actual Editor Play safe points update both module VMs without replacing their live Scene or source state",
              "[Editor][Play][ScriptModule][Recovery][TwoVm]") {
        for (const auto vm : Vms) {
            SignedPackage package{vm, Source(vm), true, true};
            package.Capture();
            Interactive host{package};
            auto registry = EmptyRegistry();
            EditorPlaySessionController play;
            Accept(play.ConfigureModuleScripts(host.modules));
            Accept(play.Start(Authoring(), registry));
            const auto scene = play.SceneView()->RuntimeId();
            Accept(play.BeginModuleScript(package.Program(), OperationPolicy()));
            const auto native = Take(play.RequestNativeModuleUpdate(*package.Program()));
            CHECK(native.kind == ScriptModuleUpdateKind::RestartRequired);
            Accept(play.RequestModuleReferenceUpdate({1, 1, 0}));
            CHECK(play.RequestModuleReferenceUpdate({1, 1, 0}).HasError());
            Answer42(AwaitPlay(play));
            const auto report = play.TakeModuleUpdateReport();
            REQUIRE(report);
            Compatible(*report);
            CHECK(play.SceneView()->RuntimeId() == scene);
            play.Stop();
            CHECK(play.RequestModuleReferenceUpdate({1, 1, 0}).HasError());
            CHECK(play.State() == EditorPlaySessionState::Idle);
        }
    }

    TEST_CASE("A rejected Editor provider update leaves the old operation and canonical authoring Scene recoverable",
              "[Editor][Play][ScriptModule][Recovery][LastGood]") {
        for (const auto vm : Vms) {
            SignedPackage package{vm, Source(vm, false), true, true};
            package.Capture();
            Interactive host{package};
            auto registry = EmptyRegistry();
            EditorPlaySessionController play;
            Accept(play.ConfigureModuleScripts(host.modules));
            const auto authoring = Authoring();
            Accept(play.Start(authoring, registry));
            const auto scene = play.SceneView()->RuntimeId();
            Accept(play.BeginModuleScript(package.Program(), OperationPolicy()));
            Accept(play.RequestModuleReferenceUpdate({2, 0, 0}));
            Answer42(AwaitPlay(play));
            const auto report = play.TakeModuleUpdateReport();
            REQUIRE(report);
            REQUIRE(report->diagnostic);
            CHECK_FALSE(report->diagnostic->message.empty());
            CHECK(play.SceneView()->RuntimeId() == scene);
            CHECK(authoring.objects.size() == 1);
            play.Stop();
        }
    }
}  // namespace Horo::Editor::Tests
