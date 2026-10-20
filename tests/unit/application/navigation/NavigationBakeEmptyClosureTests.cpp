#include "navigation/NavigationBakeServiceFixture.h"

namespace Horo::Application {
    namespace {
        using namespace Horo::Navigation;
        using namespace Horo::Navigation::TestSupport;
        using namespace BakeTestSupport;
    }  // namespace

    TEST_CASE("Removing all navigation sources publishes an empty generation without reusing old topology", "[navigation][cook][empty]") {
        BakeHarness harness;
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto retained = harness.service->Published();
        REQUIRE(retained);
        REQUIRE_FALSE(retained->tiles.tiles.empty());
        const auto builds = harness.builder->builds.load();
        harness.fixture.RemoveSources();
        if (GENERATE(false, true))
            harness.fixture.hasContribution = true;  // An authored source clipped to nothing still has freshness evidence.
        const auto empty = Submit(harness);
        REQUIRE(Terminal(*harness.service, harness.operations, empty).state == OperationState::Succeeded);
        const auto current = harness.service->Published();
        REQUIRE(current);
        CHECK(current != retained);
        CHECK(current->generation.manifestDigest != retained->generation.manifestDigest);
        CHECK(current->tiles.tiles.empty());
        CHECK(current->rebuiltTiles == 0);
        CHECK(current->reusedTiles == 0);
        CHECK(harness.builder->builds.load() == builds);
        CHECK_FALSE(retained->tiles.tiles.empty());
        const auto disk = ResolveNavigationBakePublication(harness.config);
        REQUIRE(disk.HasValue());
        CHECK(disk.Value()->tiles.tiles.empty());
        CHECK(disk.Value()->tiles.inputFingerprint == current->tiles.inputFingerprint);
    }

    TEST_CASE("Failed empty replacement preserves the previous live and durable generation", "[navigation][cook][empty]") {
        auto files = std::make_shared<ControlledFiles>();
        BakeHarness harness{files};
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto previous = harness.service->Published();
        files->failReplacement.store(true);
        harness.fixture.RemoveSources();
        CHECK(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Failed);
        CHECK(harness.service->Published() == previous);
        const auto disk = ResolveNavigationBakePublication(harness.config);
        REQUIRE(disk.HasValue());
        CHECK(disk.Value()->tiles.inputFingerprint == previous->tiles.inputFingerprint);
        CHECK_FALSE(disk.Value()->tiles.tiles.empty());
    }

    TEST_CASE("Empty replacement rejects partial tiles selected surfaces stale sources and missing compatibility before admission",
              "[navigation][cook][empty]") {
        BakeHarness harness;
        const auto previousTiles = harness.fixture.Tiles();
        const auto oldSources = harness.fixture.Observations();
        harness.fixture.RemoveSources();
        auto request = Request(harness, NavigationBakeExecutionMode::ValidateOnly);
        SECTION("foreign tile closure") {
            request.tiles = previousTiles;
        }
        SECTION("selected surface missing") {
            request.rebuildSurfaces = std::vector{NavigationBakeSurfaceKey{Id<SurfaceId>(1)}};
        }
        SECTION("stale source closure") {
            request.sources = oldSources;
        }
        SECTION("missing provider") {
            request.compatibility.provider = {};
        }
        SECTION("missing schemas") {
            request.compatibility.schemas = {};
        }
        SECTION("missing settings") {
            request.compatibility.settings = {};
        }
        const auto rejected = harness.service->Submit(std::move(request));
        CHECK(rejected.HasError());
        CHECK(harness.builder->builds.load() == 0);
        CHECK_FALSE(harness.operations.SnapshotIfChanged(0));
    }
}  // namespace Horo::Application
