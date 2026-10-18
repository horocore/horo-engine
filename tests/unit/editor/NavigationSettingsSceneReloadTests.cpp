#include "NavigationSettingsTestSupport.h"
#include "editor/document/SceneDocument.h"

using namespace Horo;
using namespace Horo::Tests::NavigationSettingsSupport;

TEST_CASE("Navigation review cannot survive canonical Scene reload with identical counters", "[navigation][settings][reload]") {
    Fixture f;

    E::SceneDocument root;
    E::EditorHistory history;
    REQUIRE(root.LoadSaved(Objects()).HasValue());
    auto bound = E::SceneDocumentTransactions::Bind(root, history, {Asset(3), 1}).Value();
    const std::array<E::SceneDocumentTransactions *, 2> scenes{&f.first, &bound};
    E::NavigationSettingsWorkflow workflow{f.registry, f.definitions, scenes};
    const std::array<E::NavigationDefinitionEdit, 1> edits{
        E::RemoveNavigationProfile{Id<N::NavigationAgentProfileId>(1), Id<N::NavigationAgentProfileId>(2)}};
    auto review = workflow.Preview(Asset(1), edits);
    REQUIRE(review.HasValue());
    REQUIRE(root.LoadSaved(Objects()).HasValue());
    REQUIRE(workflow.Commit(review.Value(), true, {}).HasError());
    REQUIRE(f.first.Snapshot().Value().revision == 0);
    REQUIRE(f.definition.Snapshot().Value().revision == 1);
}
