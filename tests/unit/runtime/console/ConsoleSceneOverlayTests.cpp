#include "ConsoleSceneOverlayInternal.h"

#include <catch2/catch_test_macros.hpp>
#include <thread>

using namespace Horo;
using namespace Horo::Debug;
using namespace Horo::Debug::Internal;

namespace {
    /** @brief Activates real assetless Scene storage through the existing lifecycle owner. */
    void Activate(Runtime::RuntimeSceneService &scene, const std::uint64_t revision, const std::size_t entities = 3) {
        Runtime::SceneDefinitionBuilder builder({1}, {revision});
        for (std::size_t index = 0; index < entities; ++index)
            builder.Add(Runtime::RuntimeEntityDefinition{.object = {index + 1},
                                                         .parent = index == 0 ? std::nullopt : std::optional{Runtime::SceneObjectId{1}}});
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        REQUIRE(scene.QueuePreparation(std::move(definition).Value()).HasValue());
        const Runtime::FrameContext frame{revision, {}, 0.0, 0, {}, false, {}};
        REQUIRE(scene.OnPhase(Runtime::RuntimePhase::CommitDeferredLifecycleChanges, frame).HasValue());
    }
}  // namespace

TEST_CASE("Scene overlay admission is profile/grant bounded without Scene activation", "[console][overlay][scene]") {
    CHECK(ConsoleSceneOverlay::Create(ProductProfile::Shipping, {15, true, true}).HasError());
    CHECK(ConsoleSceneOverlay::Create(ProductProfile::Profile, {15, true, true}).HasError());
    CHECK(ConsoleSceneOverlay::Create(ProductProfile::Diagnostics, {15, true, true}).HasError());
    CHECK(ConsoleSceneOverlay::Create(ProductProfile::Server, {15, true, true}).HasError());
    CHECK(ConsoleSceneOverlay::Create(ProductProfile::Editor, {}).HasError());
    CHECK(ConsoleSceneOverlay::Create(ProductProfile::Development, {3}, {0, 1}).HasError());
    CHECK(ConsoleSceneOverlay::Create(ProductProfile::Development, {3}, {4097, 1}).HasError());
    CHECK(ConsoleSceneOverlay::Create(ProductProfile::Development, {3}, {1, 257}).HasError());
    auto made = ConsoleSceneOverlay::Create(ProductProfile::Development, {3});
    REQUIRE(made.HasValue());
    auto overlay = std::move(made).Value();
    CHECK_FALSE(overlay.Available());
    CHECK(overlay.Rows().empty());
    REQUIRE(overlay.SetVisible(true).HasValue());
    CHECK_FALSE(overlay.Refresh({}).Value());
    CHECK_FALSE(overlay.Available());
}

TEST_CASE("Scene overlay copies bounded identities and refreshes only visible presentation at four Hz", "[console][overlay][scene]") {
    Runtime::RuntimeSceneService scene(Runtime::SceneRuntimeId{91});
    REQUIRE(scene.Startup({}).HasValue());
    Activate(scene, 1);
    const auto view = scene.ActiveScene();
    REQUIRE(view);
    auto made = ConsoleSceneOverlay::Create(ProductProfile::Editor, {3}, {2, 2});
    REQUIRE(made.HasValue());
    auto overlay = std::move(made).Value();
    REQUIRE(overlay.Publish(&*view).HasValue());
    CHECK_FALSE(overlay.Refresh({}).Value());
    CHECK(overlay.Rows().empty());
    REQUIRE(overlay.SetVisible(true).HasValue());
    CHECK(overlay.Refresh({}).Value());
    CHECK(overlay.Available());
    CHECK(overlay.Truncated());
    CHECK(overlay.RuntimeId() == view->RuntimeId());
    REQUIRE(overlay.Rows().size() == 2);
    const auto original = overlay.Rows()[0].entity;
    CHECK(overlay.Rows()[1].parent == original);
    const auto revision = overlay.Revision();
    CHECK_FALSE(overlay.Refresh(std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{250}).Value());
    CHECK(overlay.Revision() == revision);
    Activate(scene, 2);
    const auto replacement = scene.ActiveScene();
    REQUIRE(replacement);
    REQUIRE(overlay.Publish(&*replacement).HasValue());
    CHECK_FALSE(overlay.Refresh(std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{499}).Value());
    CHECK(overlay.Rows()[0].entity == original);
    CHECK(overlay.Refresh(std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{500}).Value());
    CHECK(overlay.Rows()[0].entity != original);
    CHECK(overlay.Revision() > revision);
    REQUIRE(overlay.SetVisible(false).HasValue());
    CHECK(overlay.Rows().empty());
    CHECK_FALSE(overlay.Available());
    CHECK(overlay.RuntimeId().value == 0);
    scene.Shutdown();
}

TEST_CASE("Scene overlay safely observes retirement and exact-budget scenes without retaining storage", "[console][overlay][scene]") {
    auto scene = std::make_unique<Runtime::RuntimeSceneService>(Runtime::SceneRuntimeId{92});
    REQUIRE(scene->Startup({}).HasValue());
    Activate(*scene, 1, 2);
    const auto view = scene->ActiveScene();
    REQUIRE(view);
    auto made = ConsoleSceneOverlay::Create(ProductProfile::Editor, {3}, {2, 2});
    REQUIRE(made.HasValue());
    auto overlay = std::move(made).Value();
    REQUIRE(overlay.Publish(&*view).HasValue());
    REQUIRE(overlay.SetVisible(true).HasValue());
    REQUIRE(overlay.Refresh({}).Value());
    CHECK_FALSE(overlay.Truncated());
    bool denied{};
    std::thread foreign([&] {
        denied = overlay.Publish(nullptr).HasError() && overlay.SetVisible(false).HasError() && overlay.Refresh({}).HasError();
    });
    foreign.join();
    REQUIRE(denied);
    CHECK(overlay.Rows().size() == 2);
    scene->Shutdown();
    scene.reset();
    REQUIRE_FALSE(view->IsCurrent());
    CHECK(overlay.Refresh(std::chrono::steady_clock::time_point{} + std::chrono::milliseconds{250}).Value());
    CHECK_FALSE(overlay.Available());
    CHECK(overlay.Rows().empty());
    REQUIRE(overlay.Publish(nullptr).HasValue());
    CHECK(overlay.Rows().empty());
}
