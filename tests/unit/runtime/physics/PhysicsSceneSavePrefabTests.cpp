#include "PhysicsSceneSaveTestSupport.h"

namespace Horo::Physics::StateTest {
    namespace {
        void AssertDeferredPrefabCapture(SceneSaveFixture &fixture) {
            const auto retained = fixture.physics->RetainedPayloadBytes();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
            while (std::chrono::steady_clock::now() < deadline) {
                Check(fixture.PumpResult());
                const auto snapshot = fixture.save->Snapshot();
                REQUIRE(snapshot);
                REQUIRE_FALSE(snapshot->error);
                REQUIRE(snapshot->operationPending);
                CHECK_FALSE(Take(fixture.physics->CaptureReady()));
                CHECK(fixture.physics->RetainedPayloadBytes() == retained);
                std::this_thread::yield();
            }
            const auto pending = fixture.save->Snapshot();
            REQUIRE(pending);
            REQUIRE(pending->manager);
            const auto page = Take(pending->manager->Page({.maximumRows = 16}));
            CHECK(page.rows.empty());
        }

        TEST_CASE("Queued actual Physics Save captures a genuine cooked prefab spawned before the owner capture barrier",
                  "[physics][native][save][scene][prefab][preparation]") {
            Math::Vec3 colliderTranslation{};
            SECTION("single identity collider") {}
            SECTION("single translated collider") {
                colliderTranslation = {0.375F, 0.25F, -0.125F};
            }
            SceneSaveFixture fixture{true, colliderTranslation};
            const auto before = Take(fixture.prefabs->Inventory()->Identities()).Records().size();
            Check(fixture.save->Request({.kind = Editor::PlaySaveCommandKind::SaveNew}));
            const auto spawned = fixture.SpawnBeforeCapture();
            REQUIRE(spawned.PersistentId());
            const auto actual = Take(fixture.prefabs->Inventory()->Identities());
            REQUIRE(actual.Records().size() == before + 2);
            const auto structuralPublication = Take(fixture.native.Borrow()).physics.PublishedTick();
            REQUIRE(structuralPublication.completedTick == 5);
            REQUIRE(structuralPublication.eventTick == 0);
            AssertDeferredPrefabCapture(fixture);
            Check(fixture.native.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)));
            const auto completed = Take(fixture.native.Borrow()).physics.PublishedTick();
            REQUIRE(completed.completedTick == 6);
            REQUIRE(completed.eventTick == 6);
            REQUIRE(Take(fixture.physics->CaptureReady()));
            fixture.Ready("queued save after real paired tick six");
            const auto checkpoint = fixture.Wire();
            const auto page = Take(fixture.save->Snapshot()->manager->Page({.maximumRows = 16}));
            REQUIRE(page.rows.size() == 1);
            const auto selection =
                Take(fixture.save->Snapshot()->manager->Command(Runtime::SaveManagerCommandKind::Load, page.rows.front().slot));
            const auto oldScene = fixture.assets.service.ActiveScene()->RuntimeId();
            Check(fixture.save->Request({.kind = Editor::PlaySaveCommandKind::Load, .selection = selection}));
            fixture.Ready("restore saved prefab");
            REQUIRE(fixture.assets.service.ActiveScene()->RuntimeId() != oldScene);
            fixture.prefabContext = Take(fixture.prefabs->Bind(fixture.behaviors, fixture.components, 1));
            const auto restored = Take(fixture.prefabContext->FindPersistent(*spawned.PersistentId()));
            CHECK(restored.Root().runtime == fixture.assets.service.ActiveScene()->RuntimeId());
            CHECK(fixture.Wire() == checkpoint);
            CHECK(Take(fixture.native.Borrow()).physics.PublishedTick().completedTick == 6);
            CHECK(Take(fixture.native.Borrow()).character.PublishedTick().completedTick == 6);
            Check(fixture.native.AdvanceFixedTick(Duration::FromNanoseconds(16'666'667)));
            auto pair = Take(fixture.native.Borrow());
            CHECK(pair.physics.PublishedTick().completedTick == 7);
            CHECK(pair.character.PublishedTick().completedTick == 7);
            fixture.CloseOwners();
        }
    }  // namespace
}  // namespace Horo::Physics::StateTest
