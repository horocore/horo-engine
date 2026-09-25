#include "editor/modals/build/BuildWorkflowPreviewState.h"

#include <catch2/catch_test_macros.hpp>
#include <string_view>

using namespace Horo::Editor;

TEST_CASE("Preview release creates a candidate only after verification", "[unit][editor]") {
    BuildWorkflowPreviewState state;
    REQUIRE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Release, .name = "Game", .version = "1.0"}));
    REQUIRE_FALSE(state.HasVerifiedCandidate());
    REQUIRE_FALSE(state.IsPublished());
    for (int stage = 0; stage < 7; ++stage)
        state.Update(1.8F);
    REQUIRE(state.HasVerifiedCandidate());
    REQUIRE_FALSE(state.IsPublished());
    REQUIRE(state.CandidateName() == "Game 1.0");
    REQUIRE(state.Job()->status == BuildPreviewStatus::Completed);
    REQUIRE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Publish}));
    REQUIRE_FALSE(state.IsPublished());
    for (int stage = 0; stage < 3; ++stage)
        state.Update(1.8F);
    REQUIRE(state.IsPublished());
    REQUIRE_FALSE(state.HasVerifiedCandidate());
    REQUIRE_FALSE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Publish}));
}

TEST_CASE("Preview publication requires its own job", "[unit][editor]") {
    BuildWorkflowPreviewState state;
    REQUIRE_FALSE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Publish}));
    REQUIRE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Build}));
    for (int stage = 0; stage < 4; ++stage)
        state.Update(1.8F);
    REQUIRE_FALSE(state.HasVerifiedCandidate());
    REQUIRE_FALSE(state.IsPublished());
    REQUIRE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Release}));
    state.Cancel();
    state.Update(30.0F);
    REQUIRE_FALSE(state.HasVerifiedCandidate());
}

TEST_CASE("Build preview includes selected test and run stages without publication", "[unit][editor]") {
    BuildWorkflowPreviewState state;
    REQUIRE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Build, .testAfterBuild = true, .runAfterBuild = true}));
    REQUIRE(std::string_view{state.Job()->stages.back().labelKey} == "build.preview.step.run");
    REQUIRE(std::string_view{state.Job()->stages[state.Job()->stages.size() - 2].labelKey} == "build.preview.step.test");
    for (std::size_t stage = 0; stage < state.Job()->stages.size(); ++stage)
        state.Update(1.8F);
    REQUIRE(state.Job()->status == BuildPreviewStatus::Completed);
    REQUIRE_FALSE(state.HasVerifiedCandidate());
    REQUIRE_FALSE(state.IsPublished());
}

TEST_CASE("Build preview exposes named, timed pipeline stages", "[unit][editor]") {
    BuildWorkflowPreviewState state;
    REQUIRE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Build}));
    const auto &stages = state.Job()->stages;
    REQUIRE(stages.size() == 4);
    CHECK(std::string_view{stages[0].labelKey} == "build.preview.step.validate");
    CHECK(std::string_view{stages[1].labelKey} == "build.preview.step.compile");
    CHECK(std::string_view{stages[2].labelKey} == "build.preview.step.cook");
    CHECK(std::string_view{stages[3].labelKey} == "build.preview.step.package");
    state.Update(0.9F);
    CHECK(state.Job()->stageProgress == std::vector<float>{0.5F, 0.0F, 0.0F, 0.0F});
    CHECK(state.Progress() == 0.125F);
    CHECK(state.Job()->stageIndex == 0);
    state.Update(0.9F);
    CHECK(state.Job()->stageIndex == 1);
    CHECK(state.Job()->stageProgress == std::vector<float>{1.0F, 0.0F, 0.0F, 0.0F});
    for (int stage = 0; stage < 3; ++stage)
        state.Update(1.8F);
    CHECK(state.Job()->status == BuildPreviewStatus::Completed);
    CHECK(state.Progress() == 1.0F);

    REQUIRE(state.Start(BuildPreviewRequest{.kind = BuildPreviewKind::Build, .compileOnly = true}));
    CHECK(state.Job()->stages.size() == 2);
}
