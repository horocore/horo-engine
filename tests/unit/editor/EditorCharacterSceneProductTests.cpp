#include "CharacterSceneProductFixture.h"
#include "EditorProjectSceneHost.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "editor/gameplay/EditorPlaySessionController.h"
#include "navigation/ProjectSceneNavigationFixture.h"

#include <chrono>
#include <thread>

namespace Horo::Application::CharacterSceneProductTest {
    namespace {
        /** @brief Preserves the original typed product-start failure before requiring genuine admission. */
        void RequirePlayStarted(Result<void> result) {
            INFO((result.HasError() ? result.ErrorValue().code.Value() + ": " + result.ErrorValue().message : "Editor Play started"));
            REQUIRE(result.HasValue());
        }

        /** @brief Pumps only actual asynchronous Scene lifecycle progress, never a Physics/gameplay fixed tick. */
        void FinishPlayPreparation(Editor::EditorPlaySessionController &play) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
            while (play.State() == Editor::EditorPlaySessionState::Starting && std::chrono::steady_clock::now() < deadline) {
                play.PresentationUpdate({0});
                std::this_thread::sleep_for(std::chrono::milliseconds{1});
            }
            if (play.LastError())
                UNSCOPED_INFO(play.LastError()->message);
        }

        Editor::EditorPlaySceneFactory BindProjectFactory(Fixture &fixture, Physics::PhysicsRuntimeMode mode) {
            auto factory =
                Internal::MakeEditorProjectSceneFactory(fixture.project.jobs,
                                                        Internal::EditorProjectScenePolicy{.physicsMode = mode,
                                                                                           .cooked = Internal::
                                                                                               SceneCookSelection{fixture.project.cooked
                                                                                                                      .path,
                                                                                                                  fixture.project.request
                                                                                                                      .assets.target}});
            return
                [factory = std::move(factory), root = fixture.project.project.dir.path](Runtime::SceneRuntimeId identity,
                                                                                        const Runtime::RuntimeSceneDefinition &definition) {
                return factory(root, identity, definition);
            };
        }

    }  // namespace

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Actual configured Editor Play recovers Character before readiness and keeps authored root unchanged",
              "[character][scene][editor][product][canonical]") {
        Fixture fixture;
        Gameplay::BehaviorRegistry registry;
        REQUIRE(registry.Freeze().HasValue());
        Editor::EditorPlaySessionController play;
        const Editor::SceneDocumentSnapshot authored{{1}, {1}, fixture.source.objects};
        RequirePlayStarted(play.Start(authored, registry, BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Canonical)));
        FinishPlayPreparation(play);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
        REQUIRE(play.SceneView());
        const auto first = play.SceneView()->RuntimeId();
        const auto receipt = play.CollisionReceipt();
        REQUIRE(receipt);
        CHECK(receipt->state == Editor::EditorScenePhysicsState::Active);
        CHECK(receipt->projectId == "p1");
        CHECK(receipt->documentRevision != 0);
        CHECK(receipt->fingerprint != 0);
        CHECK(receipt->generation == 1);
        CHECK(receipt->generation != first.value);  // Collision filter authority is not a Scene runtime identity.

        const auto entity = play.SceneView()->Find({1});
        REQUIRE(entity);
        CHECK(play.SceneView()->Get(*entity).Value().localTransform->translation.y > 0.495F);
        CHECK(authored.objects.front().localTransform.translation.y == 0.495F);
        CHECK(play.Start(authored, registry, BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Canonical)).HasError());
        const auto firstTick = play.FixedUpdate({}, {1.0 / 60.0});
        INFO(
            (firstTick.HasError() ? firstTick.ErrorValue().code.Value() + ": " + firstTick.ErrorValue().message : "Editor tick succeeded"));
        REQUIRE(firstTick.HasValue());
        play.Stop();
        CHECK_FALSE(play.SceneView());
        CHECK_FALSE(play.CollisionReceipt());
        RequirePlayStarted(play.Start(authored, registry, BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Canonical)));
        FinishPlayPreparation(play);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
        CHECK(play.SceneView()->RuntimeId().value > first.value);
        play.Stop();
        CHECK(authored.objects.front().localTransform.translation.y == 0.495F);
    }

#endif

    TEST_CASE("Explicit Null Editor Scene host refuses enabled Character without fallback", "[character][scene][editor][product][null]") {
        Fixture fixture;
        Gameplay::BehaviorRegistry registry;
        REQUIRE(registry.Freeze().HasValue());
        Editor::EditorPlaySessionController play;
        const Editor::SceneDocumentSnapshot authored{{1}, {1}, fixture.source.objects};
        const auto started = play.Start(authored, registry, BindProjectFactory(fixture, Physics::PhysicsRuntimeMode::Null));
        FinishPlayPreparation(play);
        CHECK((started.HasError() || play.State() == Editor::EditorPlaySessionState::Failed));
        REQUIRE(play.LastError());
        CHECK(play.LastError()->code.Value() == Physics::PhysicsErrors::CapabilityUnavailable.code.Value());
        CHECK_FALSE(play.SceneView());
        CHECK(authored.objects.front().localTransform.translation.y == 0.495F);
    }

    TEST_CASE("Editor Play admits exact cooked scoped Navigation alongside actual Scene startup and retirement",
              "[navigation][scene][editor][product]") {
        NavigationProductFixture fixture;
        using Input = NavigationProductFixture::Input;
        Input input = Input::Valid;
        bool modifyDocument = false;
        std::optional<Assets::AssetId> selected = fixture.project.sceneId;
        bool expected = HORO_TEST_NAVIGATION_NATIVE != 0;
        SECTION("a matching document plays with the admitted provider") {}
        SECTION("a foreign scoped artifact has no local world") {
            input = Input::ForeignScope;
            expected = false;
        }
        SECTION("a stale cooked project profile fails the aggregate") {
            input = Input::StalePolicy;
            expected = false;
        }
        SECTION("a missing surface cannot activate as an empty world") {
            input = Input::MissingSurface;
            expected = false;
        }
        SECTION("an uncommitted document change cannot admit old topology") {
            modifyDocument = true;
            expected = false;
        }
        SECTION("the selected asset must be a committed Scene") {
            selected = fixture.definitionId;
            expected = false;
        }
        fixture.Publish(input);
        Editor::SceneDocumentSnapshot authored{{3}, {1}, fixture.source.objects};
        if (modifyDocument)
            authored.objects.front().localTransform.translation.x = 5;
        const Internal::EditorProjectScenePolicy policy{.cooked = Internal::SceneCookSelection{fixture.project.cooked.path,
                                                                                               fixture.project.request.assets.target},
                                                        .playNavigationScene = selected};
        auto projectFactory = Internal::MakeEditorProjectSceneFactory(fixture.project.jobs, policy);
        const Editor::EditorPlaySceneFactory factory = [&projectFactory, &fixture](const Runtime::SceneRuntimeId identity,
                                                                                   const Runtime::RuntimeSceneDefinition &definition) {
            return projectFactory(fixture.project.project.dir.path, identity, definition);
        };
        Gameplay::BehaviorRegistry registry;
        REQUIRE(registry.Freeze().HasValue());
        Editor::EditorPlaySessionController play;
        const auto started = play.Start(authored, registry, factory);
        FinishPlayPreparation(play);
        if (!expected) {
            CHECK((started.HasError() || play.State() == Editor::EditorPlaySessionState::Failed));
            REQUIRE(play.LastError());
            if (modifyDocument)
                CHECK(play.LastError()->code.Value() == Navigation::NavigationErrors::StaleSnapshot.code.Value());
            CHECK_FALSE(play.SceneView());
            return;
        }
        RequirePlayStarted(started);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
        REQUIRE(play.SceneView());
        CHECK(play.SceneView()->Find({3}).has_value());
        const auto first = play.SceneView()->RuntimeId();
        play.Stop();
        CHECK_FALSE(play.SceneView());
        RequirePlayStarted(play.Start(authored, registry, factory));
        FinishPlayPreparation(play);
        REQUIRE(play.State() == Editor::EditorPlaySessionState::Playing);
        REQUIRE(play.SceneView());
        CHECK(play.SceneView()->RuntimeId().value > first.value);
        play.Stop();
        CHECK_FALSE(play.SceneView());
    }

    TEST_CASE("Canonical preview project scope cannot reuse another project's immutable capture", "[character][scene][editor][scope]") {
        Fixture first;
        Fixture second;
        CHECK(Internal::ValidateEditorPreviewProjectScope(first.project.project.dir.path, first.project.project.dir.path,
                                                          Physics::PhysicsRuntimeMode::Canonical)
                  .HasValue());
        CHECK(Internal::ValidateEditorPreviewProjectScope(first.project.project.dir.path, second.project.project.dir.path,
                                                          Physics::PhysicsRuntimeMode::Canonical)
                  .HasError());
        CHECK(Internal::ValidateEditorPreviewProjectScope(first.project.project.dir.path, second.project.project.dir.path,
                                                          Physics::PhysicsRuntimeMode::Null)
                  .HasValue());
    }
}  // namespace Horo::Application::CharacterSceneProductTest
