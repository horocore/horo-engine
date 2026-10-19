#include "Horo/Editor/SceneDocumentTransaction.h"
#include "NavigationSettingsTestSupport.h"
#include "navigation/NavigationSceneSourceFixture.h"

#include <algorithm>

using namespace Horo;
using namespace Horo::Editor;
namespace SourceTest = Horo::Tests::NavigationSources;
namespace SettingsTest = Horo::Tests::NavigationSettingsSupport;

namespace {
    /** @brief Real canonical Scene and definition owners, with one explicit source binding under their shared workflow. */
    struct SourceWorkflow final {
        Assets::AssetRegistry registry;
        NavigationDefinitionDocument definition =
            NavigationDefinitionDocument::Open(SourceTest::Asset(1), 1, 1,
                                               Navigation::NavigationDefinition::Create(SettingsTest::Definition()).Value())
                .Value();
        SceneDocumentTransactions scene = SceneDocumentTransactions::Create({SourceTest::Asset(2), 1}, {SourceTest::Object()}).Value();
        std::array<NavigationDefinitionDocument *, 1> definitions{&definition};
        std::array<SceneDocumentTransactions *, 1> scenes{&scene};
        NavigationSettingsWorkflow workflow{registry, definitions, scenes};

        SourceWorkflow() {
            REQUIRE(registry
                        .Publish({SettingsTest::Record(1, "core.navmesh", "assets/nav.horoasset"),
                                  SettingsTest::Record(2, "core.scene", "assets/scene.horo")})
                        .status == Assets::AssetRegistryBuildStatus::Complete);
        }
    };
}  // namespace

TEST_CASE("Navigation source area and filter occurrences are repaired through canonical grouped history",
          "[navigation][settings][source]") {
    SourceWorkflow fixture;
    std::array<NavigationDefinitionEdit, 1> edits;
    SECTION("area") {
        edits[0] = RemoveNavigationArea{SourceTest::Id<Navigation::NavigationAreaId>(3), SourceTest::Id<Navigation::NavigationAreaId>(5)};
    }
    SECTION("filter") {
        edits[0] =
            RemoveNavigationFilter{SourceTest::Id<Navigation::NavigationFilterId>(4), SourceTest::Id<Navigation::NavigationFilterId>(6)};
    }
    const auto review = fixture.workflow.Preview(SourceTest::Asset(1), edits);
    REQUIRE(review.HasValue());
    REQUIRE(review.Value().CanCommit());
    CHECK(std::ranges::any_of(review.Value().Impacts(), [](const auto &impact) {
        return impact.field == NavigationReferenceField::SourceArea || impact.field == NavigationReferenceField::SourceFilter;
    }));
    REQUIRE(fixture.workflow.Commit(review.Value(), true, {}).HasValue());
    const auto changed = fixture.scene.Snapshot().Value().objects.front().components.navigationSources.front();
    if (std::holds_alternative<RemoveNavigationArea>(edits[0]))
        CHECK(changed.area == SourceTest::Id<Navigation::NavigationAreaId>(5));
    else
        CHECK(changed.filter == SourceTest::Id<Navigation::NavigationFilterId>(6));
    REQUIRE(fixture.workflow.Replay(false, {}).HasValue());
    CHECK(fixture.scene.Snapshot().Value().objects.front().components.navigationSources.front() ==
          SourceTest::Object().components.navigationSources.front());
    REQUIRE(fixture.workflow.Replay(true, {}).HasValue());
    CHECK(fixture.scene.Snapshot().Value().objects.front().components.navigationSources.front() == changed);
}

TEST_CASE("Navigation source references block unreviewed removal without changing Scene or definition", "[navigation][settings][source]") {
    SourceWorkflow fixture;
    const std::array<NavigationDefinitionEdit, 1> edits{RemoveNavigationFilter{SourceTest::Id<Navigation::NavigationFilterId>(4), {}}};
    const auto review = fixture.workflow.Preview(SourceTest::Asset(1), edits);
    REQUIRE(review.HasValue());
    CHECK_FALSE(review.Value().CanCommit());
    CHECK(fixture.workflow.Commit(review.Value(), true, {}).HasError());
    CHECK(fixture.scene.Snapshot().Value().revision == 0);
    CHECK(fixture.definition.Snapshot().Value().revision == 1);
}

TEST_CASE("Canonical Scene transactions reject dangling navigation source bindings before publishing history",
          "[navigation][settings][source]") {
    SourceWorkflow fixture;
    const auto before = fixture.scene.Snapshot().Value();
    auto desired = before.objects.front();
    SECTION("collider removed") {
        desired.components.colliders.clear();
    }
    SECTION("surface removed") {
        desired.components.navigationSurface.reset();
    }
    SECTION("wrong local collider") {
        desired.components.navigationSources.front().collider = {99};
    }
    const SceneObjectReplacement change{desired.id,
                                        FingerprintSceneObjectContent(before.objects.front(), 1024 * 1024).Value(),
                                        {},
                                        desired,
                                        {}};
    CHECK(fixture.scene.Execute({before.session, before.revision, before.state, true, {}}, std::span{&change, 1}).HasError());
    const auto after = fixture.scene.Snapshot().Value();
    CHECK(after.revision == before.revision);
    CHECK(after.state == before.state);
    CHECK(after.objects.front().components == before.objects.front().components);
}
