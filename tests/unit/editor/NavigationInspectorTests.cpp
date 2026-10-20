#include "Horo/Editor/NavigationInspector.h"
#include "NavigationSettingsTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Tests::NavigationSettingsSupport;

    /** @brief Build a full real Scene owner with all five navigation component families and unrelated camera data. */
    std::vector<SceneSource::SceneObjectSnapshot> ObjectsWithLinks() {
        auto objects = Objects();
        objects.front().components.camera.emplace();
        objects.front().components.navigationRegion =
            Runtime::NavigationRegionComponent{.id = Id<Navigation::NavigationRegionId>(11), .surface = Id<Navigation::SurfaceId>(10)};
        objects.front().components.navigationLink =
            Runtime::NavigationLinkComponent{.id = Id<Navigation::NavigationLinkId>(12),
                                             .start = {Id<Navigation::SurfaceId>(10), {0, 0, 0}},
                                             .end = {Id<Navigation::SurfaceId>(10), {1, 0, 0}},
                                             .profiles = {Id<Navigation::NavigationAgentProfileId>(1)}};
        return objects;
    }

    /** @brief Capture exact definition metadata and the sole Scene/history port without widgets. */
    struct Owner final {
        NavigationDefinitionDocument definition =
            NavigationDefinitionDocument::Open(Asset(1), 1, 1, Navigation::NavigationDefinition::Create(Definition()).Value()).Value();
        std::array<NavigationDefinitionSnapshot, 1> definitions{definition.Snapshot().Value()};
        SceneDocumentTransactions scene = SceneDocumentTransactions::Create({Asset(2), 1}, ObjectsWithLinks()).Value();
    };

    /** @brief Owner-issued permission and exact content fence, regenerated for every replay. */
    SceneTransactionContext Fence(const SceneTransactionSnapshot &snapshot) {
        return {snapshot.session, snapshot.revision, snapshot.state, true, {}};
    }
}  // namespace

TEST_CASE("Navigation inspectors atomically edit all component families through canonical history",
          "[unit][editor][navigation][inspector]") {
    Owner owner;
    const auto before = owner.scene.Snapshot().Value();
    auto surface = *before.objects[0].components.navigationSurface;
    surface.bakeScope = Runtime::NavigationBakeScope::LocalBounds;
    surface.localBounds.emplace();
    auto region = *before.objects[0].components.navigationRegion;
    region.localBounds.center.x = 7;
    auto link = *before.objects[0].components.navigationLink;
    link.direction = Runtime::NavigationLinkDirection::Bidirectional;
    link.end.localPosition.x = 4;
    auto agent = *before.objects[1].components.navigationAgent;
    agent.radiusOverride = 0.7F;
    auto modifier = *before.objects[2].components.navigationModifier;
    modifier.volume = Runtime::NavigationCylinderVolume{.radius = 3};
    const std::array edits{NavigationInspectorEdit{{1}, surface}, NavigationInspectorEdit{{1}, region}, NavigationInspectorEdit{{1}, link},
                           NavigationInspectorEdit{{2}, agent}, NavigationInspectorEdit{{3}, modifier}};
    auto prepared = PrepareNavigationInspectorEdits(before, edits, owner.definitions);
    REQUIRE(prepared.HasValue());
    REQUIRE(prepared.Value().size() == 3);
    auto committed = owner.scene.Execute(Fence(before), prepared.Value());
    REQUIRE(committed.HasValue());
    REQUIRE(committed.Value().committed);
    const auto after = owner.scene.Snapshot().Value();
    CHECK(after.revision == before.revision + 1);
    CHECK(after.objects[0].components.camera == before.objects[0].components.camera);
    CHECK(after.objects[0].components.navigationSurface->generation == surface.generation + 1);
    CHECK(after.objects[0].components.navigationLink->end.localPosition.x == 4);
    CHECK(after.objects[1].components.navigationAgent->radiusOverride == agent.radiusOverride);
    REQUIRE(owner.scene.Replay(Fence(after), false).HasValue());
    const auto undone = owner.scene.Snapshot().Value();
    CHECK(undone.objects == before.objects);
    CHECK(undone.state == before.state);
    REQUIRE(owner.scene.Replay(Fence(undone), true).HasValue());
    CHECK(owner.scene.Snapshot().Value().objects == after.objects);
}

TEST_CASE("Navigation rejected batches preserve content revision dirty state and history", "[unit][editor][navigation][inspector]") {
    Owner owner;
    auto before = owner.scene.Snapshot().Value();
    auto agent = *before.objects[1].components.navigationAgent;
    agent.radiusOverride = 0.7F;
    auto modifier = *before.objects[2].components.navigationModifier;
    std::vector<NavigationInspectorEdit> edits{{{2}, agent}, {{3}, modifier}};
    std::vector<NavigationDefinitionSnapshot> definitions(owner.definitions.begin(), owner.definitions.end());
    SECTION("negative shape") {
        std::get<Runtime::NavigationLocalBounds>(modifier.volume).halfExtents.x = -1;
        edits.back().value = modifier;
    }
    SECTION("nonfinite cost") {
        modifier.operation = Runtime::NavigationModifierOperation::OverrideAreaAndCost;
        modifier.traversalCost = std::numeric_limits<float>::quiet_NaN();
        edits.back().value = modifier;
    }
    SECTION("missing surface") {
        modifier.surface = Id<Navigation::SurfaceId>(999);
        edits.back().value = modifier;
    }
    SECTION("missing area descriptor") {
        modifier.area = Id<Navigation::NavigationAreaId>(999);
        edits.back().value = modifier;
    }
    SECTION("missing profile descriptor") {
        agent.profile = Id<Navigation::NavigationAgentProfileId>(999);
        edits.front().value = agent;
    }
    SECTION("missing filter descriptor") {
        agent.filter = Id<Navigation::NavigationFilterId>(999);
        edits.front().value = agent;
    }
    SECTION("degraded descriptor inventory") {
        definitions.clear();
    }
    SECTION("identity reassignment") {
        modifier.id = Id<Navigation::NavigationModifierId>(999);
        edits.back().value = modifier;
    }
    SECTION("generation reassignment") {
        ++modifier.generation;
        edits.back().value = modifier;
    }
    SECTION("schema reassignment") {
        ++modifier.schemaVersion;
        edits.back().value = modifier;
    }
    SECTION("duplicate component target") {
        edits.push_back(edits.front());
    }
    SECTION("locked target") {
        before.objects[2].editorState.locked = true;
    }
    SECTION("coincident endpoints") {
        auto link = *before.objects[0].components.navigationLink;
        link.end = link.start;
        edits.push_back({{1}, link});
    }
    SECTION("generation overflow") {
        before.objects[2].components.navigationModifier->generation = UINT64_MAX;
        modifier.generation = UINT64_MAX;
        modifier.enabled = false;
        edits.back().value = modifier;
    }
    CHECK(PrepareNavigationInspectorEdits(before, edits, definitions).HasError());
    const auto after = owner.scene.Snapshot().Value();
    CHECK(after.revision == 0);
    CHECK_FALSE(after.dirty);
    CHECK(after.objects == ObjectsWithLinks());
    CHECK(owner.scene.Replay(Fence(after), false).HasError());
}

TEST_CASE("Navigation preparation and publication honor resource and owner fences", "[unit][editor][navigation][inspector]") {
    Owner owner;
    const auto before = owner.scene.Snapshot().Value();
    auto agent = *before.objects[1].components.navigationAgent;
    agent.radiusOverride = 0.7F;
    const std::array edits{NavigationInspectorEdit{{2}, agent}};
    SECTION("bounded preparation") {
        CHECK(PrepareNavigationInspectorEdits(before, edits, owner.definitions, {.maximumPreparedBytes = 1}).HasError());
    }
    SECTION("zero change allowance") {
        CHECK(PrepareNavigationInspectorEdits(before, edits, owner.definitions, {.maximumChanges = 0}).HasError());
    }
    SECTION("cancelled commit") {
        auto prepared = PrepareNavigationInspectorEdits(before, edits, owner.definitions);
        REQUIRE(prepared.HasValue());
        CancellationSource stop;
        stop.RequestCancellation();
        auto context = Fence(before);
        context.cancellation = stop.Token();
        CHECK(owner.scene.Execute(context, prepared.Value()).HasError());
    }
    SECTION("stale commit") {
        auto prepared = PrepareNavigationInspectorEdits(before, edits, owner.definitions);
        REQUIRE(prepared.HasValue());
        REQUIRE(owner.scene.Execute(Fence(before), prepared.Value()).HasValue());
        CHECK(owner.scene.Execute(Fence(before), prepared.Value()).HasError());
    }
    SECTION("unchanged edit is no-op") {
        auto original = *before.objects[1].components.navigationAgent;
        const std::array unchanged{NavigationInspectorEdit{{2}, original}};
        auto prepared = PrepareNavigationInspectorEdits(before, unchanged, owner.definitions);
        REQUIRE(prepared.HasValue());
        auto result = owner.scene.Execute(Fence(before), prepared.Value());
        REQUIRE(result.HasValue());
        CHECK_FALSE(result.Value().committed);
    }
}
