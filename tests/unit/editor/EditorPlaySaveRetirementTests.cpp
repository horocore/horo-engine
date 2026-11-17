#include "EditorPlaySaveRetirement.h"
#include "EditorPlaySceneHost.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Uses a genuine assetless Scene owner to observe retained lifetime independently of queue counters. */
    class OwnedSceneHost final : public EditorPlaySceneHost {
    public:
        OwnedSceneHost() {
            Runtime::SceneDefinitionBuilder builder({41}, {3});
            Runtime::RuntimeEntityDefinition entity{.object = {1}};
            entity.components.camera = Runtime::CameraComponent{};
            builder.Add(std::move(entity));
            auto definition = std::move(builder).Build();
            REQUIRE(definition.HasValue());
            REQUIRE(scenes_.Startup(token_).HasValue());
            REQUIRE(scenes_.QueuePreparation(std::move(definition).Value()).HasValue());
            REQUIRE(scenes_.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, {.frameNumber = 1, .cancellation = token_})
                        .HasValue());
            REQUIRE(scenes_.ActiveScene());
        }

        Runtime::RuntimeSceneService &Scenes() noexcept override {
            return scenes_;
        }

        Result<void> FixedUpdate(Gameplay::FixedDeltaTime) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {
            scenes_.Shutdown();
        }

    private:
        CancellationSource lifetime_;
        CancellationToken token_{lifetime_.Token()};
        Runtime::RuntimeSceneService scenes_{Runtime::SceneRuntimeId{701}};
    };
}  // namespace

TEST_CASE("Play retirement reserves finite physical slots before activation and releases unused reservations",
          "[editor][play][save][retirement]") {
    auto created = EditorPlaySaveRetirement::Create(2);
    REQUIRE(created.HasValue());
    auto queue = std::move(created).Value();
    auto first = queue->Reserve();
    auto second = queue->Reserve();
    REQUIRE(first.HasValue());
    REQUIRE(second.HasValue());
    CHECK(queue->Reserve().HasError());
    auto reservation = std::move(first).Value();
    reservation = {};
    CHECK(queue->Reserve().HasValue());
    CHECK_FALSE(queue->HasPending());
}

TEST_CASE("Play leave moves actual Scene ownership into reserved storage until the owner drain retires it",
          "[editor][play][save][retirement]") {
    auto created = EditorPlaySaveRetirement::Create(1);
    REQUIRE(created.HasValue());
    auto queue = std::move(created).Value();
    auto reserved = queue->Reserve();
    REQUIRE(reserved.HasValue());
    auto reservation = std::move(reserved).Value();
    auto host = std::make_unique<OwnedSceneHost>();
    const auto retainedView = *host->Scenes().ActiveScene();
    EditorPlayRetiredUnit unit;
    unit.scene = std::move(host);
    unit.binding = std::make_shared<EditorPlayRuntimeBinding>();
    unit.binding->revoked = true;
    unit.frame = 1;
    reservation.Retain(std::move(unit));
    CHECK(queue->HasPending());
    CHECK(retainedView.IsCurrent());
    CHECK(queue->Reserve().HasError());
    const auto drained = queue->Poll();
    REQUIRE(drained.HasValue());
    CHECK(drained.Value() == 1);
    CHECK_FALSE(retainedView.IsCurrent());
    CHECK_FALSE(queue->HasPending());
    REQUIRE(queue->Close().HasValue());
    CHECK(queue->Reserve().HasError());
}

TEST_CASE("Closing Play retirement admission keeps a previously reserved actual owner transfer valid", "[editor][play][save][retirement]") {
    auto created = EditorPlaySaveRetirement::Create(1);
    REQUIRE(created.HasValue());
    auto queue = std::move(created).Value();
    auto reserved = queue->Reserve();
    REQUIRE(reserved.HasValue());
    auto reservation = std::move(reserved).Value();
    REQUIRE(queue->Close().HasValue());
    EditorPlayRetiredUnit unit;
    unit.scene = std::make_unique<OwnedSceneHost>();
    const auto view = *unit.scene->Scenes().ActiveScene();
    unit.frame = 1;
    reservation.Retain(std::move(unit));
    CHECK(view.IsCurrent());
    REQUIRE(queue->Close().HasValue());
    CHECK_FALSE(view.IsCurrent());
}
