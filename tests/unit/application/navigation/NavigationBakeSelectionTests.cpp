#include "navigation/NavigationBakeServiceFixture.h"

namespace Horo::Application {
    namespace {
        using namespace Horo::Navigation;
        using namespace Horo::Navigation::TestSupport;
        using namespace BakeTestSupport;

        /** @brief Submits a rebuild selection while retaining the complete definition capture. */
        Result<OperationId> Selected(const BakeHarness &harness, std::vector<SurfaceId> surfaces) {
            return harness.service->Submit(Request(harness, NavigationBakeExecutionMode::BuildAndPublish, std::move(surfaces)));
        }
    }  // namespace

    TEST_CASE("Selected-surface admission rejects ambiguous empty and absent surface selections", "[navigation][cook][selection]") {
        BakeHarness harness;
        auto surfaces = std::vector{Id<SurfaceId>(1)};
        SECTION("empty") {
            surfaces.clear();
        }
        SECTION("invalid identity") {
            surfaces = {SurfaceId{}};
        }
        SECTION("duplicate") {
            surfaces.push_back(surfaces.front());
        }
        SECTION("absent from complete closure") {
            surfaces = {Id<SurfaceId>(2)};
        }
        SECTION("selection count exceeds capture ceiling") {
            surfaces.assign(NavigationBakeInputLimits::MaximumSurfaceBindings + 1U, Id<SurfaceId>(1));
        }
        const auto rejected = Selected(harness, std::move(surfaces));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == NavigationErrors::BakeInputInvalid.code.Value());
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(harness.service->Published());
    }

    TEST_CASE("Selected-surface cold admission never builds an unselected surface or publishes a subset", "[navigation][cook][selection]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        const auto request = Selected(harness, {Id<SurfaceId>(1)});
        REQUIRE(request.HasValue());
        const auto result = Terminal(*harness.service, harness.operations, request.Value());
        REQUIRE(result.state == OperationState::Failed);
        REQUIRE(result.error.has_value());
        CHECK(result.error->code.Value() == NavigationErrors::BakeSelectionIncomplete.code.Value());
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(harness.service->Published());
        CHECK_FALSE(std::filesystem::exists(harness.config.targetRoot / "current.json"));
    }

    TEST_CASE("Selected-surface rebuild preserves complete verified unselected topology and rejects stale output",
              "[navigation][cook][selection]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto original = harness.service->Published();
        REQUIRE(original);
        REQUIRE(original->tiles.tiles.size() == 8);
        harness.fixture.ExcludeBorder();
        const auto selected = Selected(harness, {Id<SurfaceId>(1)});
        REQUIRE(selected.HasValue());
        REQUIRE(Terminal(*harness.service, harness.operations, selected.Value()).state == OperationState::Succeeded);
        const auto updated = harness.service->Published();
        REQUIRE(updated);
        REQUIRE(updated->tiles.tiles.size() == 8);
        CHECK(updated->rebuiltTiles == 2);
        CHECK(updated->reusedTiles == 6);
        for (std::size_t index = 4; index < 8; ++index)
            CHECK(updated->tiles.tiles[index] == original->tiles.tiles[index]);
        const auto builds = harness.builder->builds.load();
        const auto previous = Assets::ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, harness.config.targetRoot);
        REQUIRE(previous.HasValue());
        harness.fixture.ExcludeBorder();
        harness.fixture.modifiers.front().surface = Id<SurfaceId>(2);
        const auto stale = Selected(harness, {Id<SurfaceId>(1)});
        REQUIRE(stale.HasValue());
        const auto rejected = Terminal(*harness.service, harness.operations, stale.Value());
        REQUIRE(rejected.state == OperationState::Failed);
        REQUIRE(rejected.error.has_value());
        CHECK(rejected.error->code.Value() == NavigationErrors::BakeSelectionIncomplete.code.Value());
        CHECK(harness.builder->builds.load() == builds);
        CHECK(harness.service->Published() == updated);
        const auto retained = Assets::ResolveCurrentCookGeneration(Horo::NativeDurableFileSystem{}, harness.config.targetRoot);
        REQUIRE(retained.HasValue());
        CHECK(retained.Value().manifestDigest == previous.Value().manifestDigest);
    }

    TEST_CASE("Canonical selected-surface order joins one native operation", "[navigation][cook][selection]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        harness.builder->pause.store(true);
        const auto first = Selected(harness, {Id<SurfaceId>(2), Id<SurfaceId>(1)});
        REQUIRE(first.HasValue());
        const auto joined = Selected(harness, {Id<SurfaceId>(1), Id<SurfaceId>(2)});
        REQUIRE(joined.HasValue());
        CHECK(joined.Value() == first.Value());
        harness.builder->pause.store(false);
        REQUIRE(Terminal(*harness.service, harness.operations, first.Value()).state == OperationState::Succeeded);
        REQUIRE(harness.service->Published());
        CHECK(harness.service->Published()->tiles.tiles.size() == 8);
    }
}  // namespace Horo::Application
