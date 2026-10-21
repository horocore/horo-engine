#include "editor/screens/workspace/panels/inspector/NavigationInspectorSession.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Two authored values with distinct radius overrides prove selection does not flatten unrelated values. */
    EditorWorkspaceViewModel View() {
        EditorWorkspaceViewModel view;
        view.physicsContext = {{Assets::AssetId::Parse("10000000-0000-0000-0000-000000000001").Value(), 1}, 3, 4, true, {}};
        view.selectionRevision = 7;
        view.selectedObjects = {{1}, {2}};
        view.primarySelection = SceneObjectId{1};
        for (std::uint64_t id : {1, 2}) {
            SceneObject object{.id = {id}, .name = "Agent " + std::to_string(id)};
            object.components.navigationAgent =
                Runtime::NavigationAgentComponent{.profile = Navigation::NavigationAgentProfileId::Create(1).Value(),
                                                  .filter = Navigation::NavigationFilterId::Create(1).Value(),
                                                  .radiusOverride = static_cast<float>(id)};
            view.objects.push_back(std::move(object));
        }
        return view;
    }
}  // namespace

TEST_CASE("Navigation multi-selection retains independent drafts and emits one complete request", "[editor][navigation][inspector]") {
    auto view = View();
    NavigationInspectorSession session;
    session.Begin(view);
    REQUIRE(session.Drafts().size() == 2);
    auto &first = std::get<Runtime::NavigationAgentComponent>(session.Drafts()[0].edit.value);
    auto &second = std::get<Runtime::NavigationAgentComponent>(session.Drafts()[1].edit.value);
    first.radiusOverride = -1;
    second.enabled = false;
    session.Begin(view);
    CHECK(session.Dirty());
    CHECK(std::get<Runtime::NavigationAgentComponent>(session.Drafts()[0].edit.value).radiusOverride == -1);
    const auto request = session.Request();
    REQUIRE(request.edits.size() == 2);
    CHECK(request.objects == view.selectedObjects);
    CHECK(request.context.session == view.physicsContext.session);
    CHECK(request.selectionRevision == view.selectionRevision);
    CHECK(view.objects[0].components.navigationAgent->radiusOverride == 1);
    CHECK(view.objects[1].components.navigationAgent->enabled);
    session.Cancel();
    CHECK_FALSE(session.Dirty());
    CHECK(session.Request().edits.empty());
}

TEST_CASE("Navigation drafts are revoked on selection owner and descriptor changes", "[editor][navigation][inspector]") {
    auto view = View();
    NavigationInspectorSession session;
    session.Begin(view);
    std::get<Runtime::NavigationAgentComponent>(session.Drafts()[0].edit.value).radiusOverride = -1;
    SECTION("document revision") {
        ++view.physicsContext.revision;
    }
    SECTION("opening replacement") {
        ++view.physicsContext.session.generation;
    }
    SECTION("selection revision") {
        ++view.selectionRevision;
    }
    SECTION("selection contents") {
        view.selectedObjects.erase(view.selectedObjects.begin());
    }
    SECTION("descriptor publication") {
        view.navigationDefinitions.emplace_back();
    }
    session.Begin(view);
    CHECK_FALSE(session.Dirty());
    session.Clear();
    CHECK(session.Drafts().empty());
}
