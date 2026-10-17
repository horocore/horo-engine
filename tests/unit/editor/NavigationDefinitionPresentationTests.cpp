#include "helpers/editor_ui/NavigationSettingsPresentationFixture.h"

using namespace Horo;
using namespace Horo::Tests::NavigationSettingsSupport;
using Horo::Editor::Tests::NavigationSettingsPresentationFixture;

TEST_CASE("Navigation definition controls publish reviewed profile changes and complete history", "[editor][gui][navigation][settings]") {
    NavigationSettingsPresentationFixture fixture;
    auto &draft = *fixture.document.draft;
    fixture.Definition("Walker [1]");
    REQUIRE(draft.name == "Walker");
    draft.name = "Revised walker";
    fixture.Definition("settings.navigation.put");
    REQUIRE(draft.review);
    REQUIRE(draft.review->CanCommit());
    REQUIRE_FALSE(fixture.session->IsDirty());
    REQUIRE(fixture.Current().definition->Profiles().front().displayName == "Walker");
    fixture.Definition("settings.navigation.confirm");
    REQUIRE_FALSE(draft.review);
    REQUIRE(fixture.Current().definition->Profiles().front().displayName == "Revised walker");
    REQUIRE(fixture.session->IsDirty());
    fixture.Definition("settings.navigation.undo");
    REQUIRE(fixture.Current().definition->Profiles().front().displayName == "Walker");
    fixture.Definition("settings.navigation.redo");
    REQUIRE(fixture.Current().definition->Profiles().front().displayName == "Revised walker");
    fixture.Definition("settings.navigation.save_documents");
    REQUIRE_FALSE(fixture.session->IsDirty());
    fixture.session->Close();
    auto reopened = fixture.project.Open();
    REQUIRE(reopened.HasValue());
    REQUIRE(reopened.Value()->Definitions().Value().front().definition->Profiles().front().displayName == "Revised walker");
}

TEST_CASE("Navigation definition controls reject detached malformed identity and geometry", "[editor][gui][navigation][validation]") {
    NavigationSettingsPresentationFixture fixture;
    auto &draft = *fixture.document.draft;
    SECTION("malformed identity") {
        draft.identity = "1 trailing";
    }
    SECTION("zero identity") {
        draft.identity = "0";
    }
    SECTION("invalid geometry") {
        draft.name = "Invalid";
        draft.geometry.radiusMeters = -1;
    }
    fixture.Definition("settings.navigation.put");
    REQUIRE_FALSE(fixture.document.status.empty());
    REQUIRE_FALSE(draft.review);
    REQUIRE(fixture.Current().revision == 1);
    REQUIRE_FALSE(fixture.session->IsDirty());
}

TEST_CASE("Navigation delete confirmation repairs actual Scene references and blocks unrepaired deletion",
          "[editor][gui][navigation][references]") {
    NavigationSettingsPresentationFixture fixture;
    auto &draft = *fixture.document.draft;
    draft.replacement = "2";
    fixture.Definition("settings.navigation.delete");
    REQUIRE(draft.review);
    REQUIRE(draft.review->Impacts().size() == 2);
    REQUIRE(draft.review->CanCommit());
    fixture.Definition("settings.navigation.confirm");
    REQUIRE(fixture.project.scene.Snapshot().Value().objects[1].components.navigationAgent->profile == Id<N::NavigationAgentProfileId>(2));
    fixture.Definition("settings.navigation.undo");
    draft.replacement.clear();
    fixture.Definition("settings.navigation.delete");
    REQUIRE(draft.review);
    REQUIRE_FALSE(draft.review->CanCommit());
    const auto revision = fixture.Current().revision;
    fixture.Definition("settings.navigation.confirm");
    REQUIRE(fixture.Current().revision == revision);
    REQUIRE(fixture.project.scene.Snapshot().Value().objects[1].components.navigationAgent->profile == Id<N::NavigationAgentProfileId>(1));
}

TEST_CASE("Navigation area and filter identity controls preserve typed reference repairs", "[editor][gui][navigation][references]") {
    for (const int catalog : {1, 2}) {
        DYNAMIC_SECTION("catalog " << catalog) {
            NavigationSettingsPresentationFixture fixture;
            auto &draft = *fixture.document.draft;
            draft.catalog = catalog;
            draft.identity = catalog == 1 ? "3" : "4";
            draft.replacement = "9";
            draft.sourceIdentity = "20";
            draft.flags = "1";
            fixture.Definition("settings.navigation.rename_identity");
            REQUIRE(draft.review);
            REQUIRE(draft.review->CanCommit());
            fixture.Definition("settings.navigation.confirm");
            const auto scene = fixture.project.scene.Snapshot();
            REQUIRE(scene.HasValue());
            if (catalog == 1)
                REQUIRE(scene.Value().objects[2].components.navigationModifier->area == Id<N::NavigationAreaId>(9));
            else
                REQUIRE(scene.Value().objects[1].components.navigationAgent->filter == Id<N::NavigationFilterId>(9));
            fixture.Definition("settings.navigation.undo");
            REQUIRE(fixture.Current().definition->Registry().ResolveArea(Id<N::NavigationAreaId>(3)).HasValue());
            REQUIRE(fixture.Current().definition->Registry().ResolveFilter(Id<N::NavigationFilterId>(4)).HasValue());
        }
    }
}

TEST_CASE("Navigation filter controls edit bounded cost overrides before reviewed publication", "[editor][gui][navigation][settings]") {
    NavigationSettingsPresentationFixture fixture;
    auto &draft = *fixture.document.draft;
    draft.catalog = 2;
    draft.identity = "8";
    draft.sourceIdentity = "20";
    draft.flags = "1";
    draft.overrideArea = "bad";
    fixture.Definition("settings.navigation.add_override");
    REQUIRE(draft.overrides.empty());
    draft.overrideArea = "3";
    draft.overrideCost = 2;
    fixture.Definition("settings.navigation.add_override");
    REQUIRE(draft.overrides.size() == 1);
    fixture.Definition("settings.navigation.clear_overrides");
    REQUIRE(draft.overrides.empty());
    fixture.Definition("settings.navigation.add_override");
    fixture.Definition("settings.navigation.put");
    REQUIRE(draft.review);
    REQUIRE(draft.review->CanCommit());
    fixture.Definition("settings.navigation.confirm");
    const auto filter = fixture.Current().definition->Registry().ResolveFilter(Id<N::NavigationFilterId>(8));
    REQUIRE(filter.HasValue());
    REQUIRE(filter.Value()->costOverrides.size() == 1);
    REQUIRE(filter.Value()->costOverrides.front().area == Id<N::NavigationAreaId>(3));
}

TEST_CASE("Navigation definition rendering retains unavailable owner feedback after close", "[editor][gui][navigation][lifetime]") {
    NavigationSettingsPresentationFixture fixture;
    fixture.document.status = "Retained failure details";
    fixture.session->Close();
    fixture.Definition();
    REQUIRE(fixture.document.status == "Retained failure details");
    REQUIRE(fixture.document.session.Resolve() == nullptr);
}

TEST_CASE("Navigation area and filter deletion buttons repair their distinct typed Scene fields", "[editor][gui][navigation][references]") {
    for (const int catalog : {1, 2}) {
        DYNAMIC_SECTION("catalog " << catalog) {
            NavigationSettingsPresentationFixture fixture;
            auto &draft = *fixture.document.draft;
            draft.catalog = catalog;
            fixture.Definition(catalog == 1 ? "3" : "4");
            REQUIRE(draft.identity == (catalog == 1 ? "3" : "4"));
            REQUIRE(draft.sourceIdentity == "20");
            draft.replacement = catalog == 1 ? "5" : "6";
            fixture.Definition("settings.navigation.delete");
            REQUIRE(draft.review);
            REQUIRE(draft.review->CanCommit());
            fixture.Definition("settings.navigation.confirm");
            const auto scene = fixture.project.scene.Snapshot();
            REQUIRE(scene.HasValue());
            if (catalog == 1)
                REQUIRE(scene.Value().objects[2].components.navigationModifier->area == Id<N::NavigationAreaId>(5));
            else
                REQUIRE(scene.Value().objects[1].components.navigationAgent->filter == Id<N::NavigationFilterId>(6));
        }
    }
}

TEST_CASE("Navigation definition controls preserve canonical owner on unavailable asset and malformed delete",
          "[editor][gui][navigation][validation]") {
    NavigationSettingsPresentationFixture fixture;
    fixture.Frame([&fixture] {
        Editor::DrawNavigationDefinitionEditor(fixture.document, fixture.gui.context, Asset(99));
    });
    REQUIRE_FALSE(fixture.document.status.empty());
    fixture.document.draft->identity = "not-an-id";
    fixture.Definition("settings.navigation.delete");
    REQUIRE_FALSE(fixture.document.draft->review);
    REQUIRE(fixture.Current().revision == 1);
    REQUIRE_FALSE(fixture.session->IsDirty());
}

TEST_CASE("Navigation definition destructive review visibly retains unsupported prefab owners", "[editor][gui][navigation][references]") {
    NavigationSettingsPresentationFixture fixture;
    fixture.session->Close();
    REQUIRE(fixture.project.registry
                .Publish({Record(1, "core.navmesh", "assets/nav.horoasset"), Record(2, "core.scene", "assets/a.horo"),
                          Record(3, "core.prefab", "assets/unsupported.prefab")})
                .status == Assets::AssetRegistryBuildStatus::Complete);
    auto reopened = fixture.project.Open();
    REQUIRE(reopened.HasValue());
    fixture.session = std::move(reopened).Value();
    fixture.document.session = fixture.session->Borrow();
    fixture.document.draft->replacement = "2";
    fixture.Definition("settings.navigation.delete");
    const auto &review = fixture.document.draft->review;
    REQUIRE(review);
    REQUIRE(review->UnsupportedOwners().size() == 1);
    REQUIRE(review->UnsupportedOwners().front().asset == Asset(3));
    REQUIRE_FALSE(review->CanCommit());
    fixture.Definition("settings.navigation.confirm");
    REQUIRE(fixture.Current().revision == 1);
    REQUIRE_FALSE(fixture.session->IsDirty());
}
