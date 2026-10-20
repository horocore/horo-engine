#include "navigation/NavigationBakeServiceFixture.h"

namespace Horo::Application {
    namespace {
        using namespace Horo::Navigation;
        using namespace Horo::Navigation::TestSupport;
        using namespace BakeTestSupport;

        /** @brief Removes current geometry membership while retaining the exact authored surface in admission metadata. */
        NavigationBakeRequest EmptySelection(const BakeHarness &harness, NavigationBakeExecutionMode mode) {
            auto request = Request(harness, mode);
            request.authoredSurfaces.emplace_back(Id<SurfaceId>(1));
            request.rebuildSurfaces = std::vector{NavigationBakeSurfaceKey{Id<SurfaceId>(1)}};
            return request;
        }
    }  // namespace

    TEST_CASE("Selected empty authored surface removes old tiles while unselected topology remains verified",
              "[navigation][cook][selection][empty]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto previous = harness.service->Published();
        const auto builds = harness.builder->builds.load();
        harness.fixture.surfaces.erase(harness.fixture.surfaces.begin());
        const auto operation = harness.service->Submit(EmptySelection(harness, NavigationBakeExecutionMode::BuildAndPublish));
        REQUIRE(operation.HasValue());
        REQUIRE(Terminal(*harness.service, harness.operations, operation.Value()).state == OperationState::Succeeded);
        const auto current = harness.service->Published();
        REQUIRE(current->tiles.tiles.size() == 4);
        CHECK(current->rebuiltTiles == 0);
        CHECK(current->reusedTiles == 4);
        CHECK(harness.builder->builds.load() == builds);
        for (const auto &tile : current->tiles.tiles)
            CHECK(tile->Key().surface == Id<SurfaceId>(2));
        CHECK(previous->tiles.tiles.size() == 8);
        CHECK(ResolveNavigationBakePublication(harness.config).Value()->tiles.tiles.size() == 4);
    }

    TEST_CASE("Selected complete empty replacement validates without writes and publishes without a provider build",
              "[navigation][cook][selection][empty]") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto previous = harness.service->Published();
        const auto builds = harness.builder->builds.load();
        harness.fixture.RemoveSources();
        const auto validation = harness.service->Submit(EmptySelection(harness, NavigationBakeExecutionMode::ValidateOnly));
        REQUIRE(validation.HasValue());
        REQUIRE(Terminal(*harness.service, harness.operations, validation.Value()).state == OperationState::Succeeded);
        CHECK(harness.service->Published() == previous);
        CHECK(ResolveNavigationBakePublication(harness.config).Value()->tiles.tiles.size() == 4);
        const auto operation = harness.service->Submit(EmptySelection(harness, NavigationBakeExecutionMode::BuildAndPublish));
        REQUIRE(operation.HasValue());
        REQUIRE(Terminal(*harness.service, harness.operations, operation.Value()).state == OperationState::Succeeded);
        CHECK(harness.service->Published()->tiles.tiles.empty());
        CHECK(ResolveNavigationBakePublication(harness.config).Value()->tiles.tiles.empty());
        CHECK(harness.builder->builds.load() == builds);
    }

    TEST_CASE("Selected capture cannot silently remove an unselected old tile in either execution mode or after restart",
              "[navigation][cook][selection][empty][restart]") {
        BakeHarness harness;
        harness.fixture.surfaces.push_back(Id<SurfaceId>(2));
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto previous = harness.service->Published();
        const auto builds = harness.builder->builds.load();
        harness.fixture.RemoveSources();
        if (GENERATE(false, true)) {
            harness.service->Close();
            harness.service = NavigationBakeService::Create(harness.config, harness.operations, harness.jobs).Value();
        }
        const auto mode = GENERATE(NavigationBakeExecutionMode::ValidateOnly, NavigationBakeExecutionMode::BuildAndPublish);
        auto request = EmptySelection(harness, mode);
        request.authoredSurfaces.emplace_back(Id<SurfaceId>(2));
        const auto operation = harness.service->Submit(std::move(request));
        REQUIRE(operation.HasValue());
        const auto terminal = Terminal(*harness.service, harness.operations, operation.Value());
        REQUIRE(terminal.state == OperationState::Failed);
        REQUIRE(terminal.error);
        CHECK(terminal.error->code.Value() == NavigationErrors::BakeSelectionIncomplete.code.Value());
        CHECK(harness.builder->builds.load() == builds);
        CHECK(ResolveNavigationBakePublication(harness.config).Value()->tiles.tiles.size() == previous->tiles.tiles.size());
    }

    TEST_CASE("Restarted empty selection preserves malformed selector errors without replacing durable authority",
              "[navigation][cook][selection][empty][restart]") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto builds = harness.builder->builds.load();
        harness.service->Close();
        harness.service = NavigationBakeService::Create(harness.config, harness.operations, harness.jobs).Value();
        std::ofstream{harness.config.targetRoot / "current.json", std::ios::trunc} << "{broken";
        const auto expected = ResolveNavigationBakePublication(harness.config);
        REQUIRE(expected.HasError());
        harness.fixture.RemoveSources();
        const auto operation = harness.service->Submit(EmptySelection(harness, NavigationBakeExecutionMode::BuildAndPublish));
        REQUIRE(operation.HasValue());
        const auto terminal = Terminal(*harness.service, harness.operations, operation.Value());
        REQUIRE(terminal.state == OperationState::Failed);
        REQUIRE(terminal.error);
        CHECK(terminal.error->domain.Value() == expected.ErrorValue().domain.Value());
        CHECK(terminal.error->code.Value() == expected.ErrorValue().code.Value());
        CHECK(harness.builder->builds.load() == builds);
        CHECK_FALSE(harness.service->Published());
        std::ifstream current{harness.config.targetRoot / "current.json"};
        std::string line;
        std::getline(current, line);
        CHECK(line == "{broken");
    }

    TEST_CASE("Complete authored catalog rejects invalid duplicate missing and excessive surface membership before admission",
              "[navigation][cook][selection][catalog]") {
        BakeHarness harness;
        auto request = Request(harness);
        SECTION("missing tile surface") {
            request.authoredSurfaces.clear();
        }
        SECTION("invalid surface") {
            request.authoredSurfaces.front().surface = {};
        }
        SECTION("duplicate surface") {
            request.authoredSurfaces.push_back(request.authoredSurfaces.front());
        }
        SECTION("catalog ceiling") {
            request.authoredSurfaces.assign(NavigationBakeInputLimits::MaximumSurfaceBindings + 1U,
                                            NavigationBakeSurfaceKey{Id<SurfaceId>(1)});
        }
        const auto rejected = harness.service->Submit(std::move(request));
        CHECK(rejected.HasError());
        CHECK_FALSE(harness.operations.SnapshotIfChanged(0));
        CHECK(harness.builder->builds.load() == 0);
    }
}  // namespace Horo::Application
