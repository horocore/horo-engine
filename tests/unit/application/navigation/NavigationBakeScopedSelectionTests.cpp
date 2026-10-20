#include "navigation/NavigationBakeServiceFixture.h"

namespace Horo::Application {
    namespace {
        using namespace Horo::Navigation;
        using namespace Horo::Navigation::TestSupport;
        using namespace BakeTestSupport;

        NavigationBakeScopeId Scope(std::uint8_t value) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.front() = value;
            return NavigationBakeScopeId::ForScene(bytes).Value();
        }
    }  // namespace

    TEST_CASE("Native bake and selected rebuild keep matching surface IDs isolated across Scenes", "[navigation][cook][scope]") {
        BakeHarness harness;
        harness.fixture.scopes = {Scope(1), Scope(2)};
        REQUIRE(Terminal(*harness.service, harness.operations, Submit(harness)).state == OperationState::Succeeded);
        const auto previous = harness.service->Published();
        REQUIRE(previous->tiles.tiles.size() == 8);
        const auto builds = harness.builder->builds.load();
        harness.fixture.ExcludeBorder();
        auto request = Request(harness);
        request.rebuildSurfaces = std::vector{NavigationBakeSurfaceKey{Id<SurfaceId>(1), Scope(1)}};
        const auto admitted = harness.service->Submit(std::move(request));
        REQUIRE(admitted.HasValue());
        REQUIRE(Terminal(*harness.service, harness.operations, admitted.Value()).state == OperationState::Succeeded);
        const auto current = harness.service->Published();
        REQUIRE(current->tiles.tiles.size() == 8);
        CHECK(harness.builder->builds.load() > builds);
        CHECK(harness.builder->builds.load() < builds + 4);
        for (const auto &tile : previous->tiles.tiles)
            if (tile->Key().scope == Scope(2)) {
                const auto found = std::ranges::find_if(current->tiles.tiles, [&tile](const auto &candidate) {
                    return candidate->Key() == tile->Key();
                });
                REQUIRE(found != current->tiles.tiles.end());
                CHECK((*found)->ContentIdentity() == tile->ContentIdentity());
            }
        const auto disk = ResolveNavigationBakePublication(harness.config);
        REQUIRE(disk.HasValue());
        CHECK(disk.Value()->tiles.tiles.size() == 8);
        auto invalid = Request(harness);
        invalid.rebuildSurfaces = std::vector{NavigationBakeSurfaceKey{Id<SurfaceId>(1), Scope(3)}};
        const auto rejected = harness.service->Submit(std::move(invalid));
        CHECK(rejected.HasError());
    }
}  // namespace Horo::Application
