#include "PhysicsSceneSaveTestSupport.h"

namespace Horo::Physics::StateTest {
    namespace {
        TEST_CASE("Real Physics Save uses the production worker transport and 1481 Scene aggregate final gate",
                  "[physics][native][save][scene][integration]") {
            SceneSaveFixture fixture;
            const auto originalScene = fixture.assets.service.ActiveScene()->RuntimeId();
            const auto checkpoint = fixture.Wire();
            const auto selection = fixture.SaveThenSelect();
            for (unsigned index = 0; index < 4; ++index)
                Check(fixture.native.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)));
            REQUIRE(Take(fixture.native.Borrow()).physics.PublishedTick().completedTick == 9);
            Check(fixture.save->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
            fixture.Ready();
            REQUIRE(fixture.assets.service.ActiveScene()->RuntimeId() != originalScene);
            CHECK(fixture.Wire() == checkpoint);
            auto pair = Take(fixture.native.Borrow());
            CHECK(pair.physics.PublishedTick().completedTick == 5);
            CHECK(pair.character.PublishedTick().completedTick == 5);
            Check(fixture.native.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)));
            CHECK(pair.physics.PublishedTick().completedTick == 6);
            CHECK(pair.character.PublishedTick().completedTick == 6);
            (void)fixture.SaveThenSelect();
            fixture.CloseOwners();
            CHECK(fixture.closed);
        }

        TEST_CASE("Real queued Physics Save re-prepares after owner structural revision and cancels without early publication",
                  "[physics][native][save][scene][preparation]") {
            SceneSaveFixture fixture;
            const auto original = fixture.assets.service.ActiveScene()->RuntimeId();
            Check(fixture.save->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}));
            Check(fixture.save->BeginMutation());
            Runtime::SceneCommandBuffer commands;
            const auto scene = *fixture.assets.service.ActiveScene();
            const auto entity = *scene.Find({3});
            const auto current = Take(scene.Get(entity));
            auto transform = *current.localTransform;
            transform.translation.x += 0.25F;
            commands.SetLocalTransform(entity, transform);
            Check(fixture.assets.service.QueueStructuralCommands(std::move(commands)));
            Check(fixture.assets.service.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges,
                                                 {.frameNumber = ++fixture.frame, .cancellation = fixture.assets.cancellation.Token()}));
            Check(fixture.save->EndMutation());
            fixture.Ready();
            REQUIRE(fixture.assets.service.ActiveScene()->RuntimeId() == original);
            const auto page = Take(fixture.save->Snapshot()->manager->Page({.maximumRows = 16}));
            REQUIRE(page.rows.size() == 1);
            const auto selection =
                Take(fixture.save->Snapshot()->manager->Command(Runtime::SaveManagerCommandKind::Load, page.rows.front().slot));
            Check(fixture.save->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
            Check(fixture.save->Request({.kind = Editor::PlaySaveCommandKind::Cancel}));
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
            while (fixture.save->Snapshot()->operationPending && std::chrono::steady_clock::now() < deadline) {
                (void)fixture.PumpResult();
                std::this_thread::yield();
            }
            CHECK_FALSE(fixture.save->Snapshot()->operationPending);
            CHECK(fixture.assets.service.ActiveScene()->RuntimeId() == original);
            fixture.CloseOwners();
        }
    }  // namespace
}  // namespace Horo::Physics::StateTest
