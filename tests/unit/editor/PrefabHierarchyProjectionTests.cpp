#include "PrefabHierarchyTestSupport.h"
#include "SceneDocumentTestSupport.h"
#include "editor/screens/workspace/prefab/PrefabHierarchyProjection.h"

namespace Horo::Editor {
    using namespace SceneDocumentTestSupport;

    TEST_CASE("Real prefab Inspector projection consumes immutable default and instance value diffs", "[prefab][value-snapshot][editor]") {
        const auto asset = PrefabAsset().Asset();
        const auto resolver = ScenePrefabResolver(asset, {{.localId = {}, .name = "Root"}});
        const auto instance = Prefab::PrefabInstanceId::Create(1).Value();
        const auto candidate = resolver.Resolve(asset, instance, ScenePrefabLimits()).Value();
        Math::Transform moved;
        moved.translation = {4, 0, 0};
        auto edited = Prefab::PreparePrefabValueEdit(candidate, {},
                                                     {Prefab::PrefabValueEditKind::Assign,
                                                      Prefab::PrefabObjectAddress::Create({}, {}).Value(), moved},
                                                     nullptr, ScenePrefabLimits())
                          .Value();
        SceneDocument document;
        REQUIRE(
            document.LoadSaved({}, {{.instanceId = instance, .sourcePrefab = PrefabAsset(), .valueOverrides = edited.rebased}}).HasValue());
        auto sources = BuildScenePrefabProjection(document.Snapshot(), resolver, ScenePrefabLimits()).Value();
        auto projection = BuildPrefabHierarchyProjection(PrefabAsset(9).Asset(), 1, document.Snapshot().revision.value,
                                                         std::make_shared<const SceneSource::ScenePrefabProjection>(std::move(sources)), 2,
                                                         {}, ScenePrefabLimits());
        REQUIRE(projection.HasValue());
        REQUIRE(projection.Value().entries[1].valueDiff.size() == 1);
        const auto &diff = projection.Value().entries[1].valueDiff[0];
        CHECK(diff.changed);
        CHECK(std::string_view{diff.originKey} == "workspace.prefab.diff.origin.instance");
        CHECK(diff.source != diff.effective);
        CHECK_FALSE(diff.schemaDefault);
        CHECK_FALSE(projection.Value().entries[1].valueDiffIncomplete);
        const auto retained = diff.effective;
        REQUIRE(document.LoadSaved({}, {{.instanceId = instance, .sourcePrefab = PrefabAsset()}}).HasValue());
        auto current = BuildScenePrefabProjection(document.Snapshot(), resolver, ScenePrefabLimits()).Value();
        CHECK(current.instances[0].valueSnapshot->Values()[0].origin == Prefab::PrefabValueOrigin::Source);
        CHECK(diff.effective == retained);
    }

    TEST_CASE("Prefab hierarchy keeps source-qualified member selection separate across placements", "[editor][prefab][hierarchy]") {
        const auto asset = PrefabAsset().Asset();
        const auto resolver =
            ScenePrefabResolver(asset, {{.localId = Prefab::LocalObjectId{0}, .name = "Root"},
                                        {.localId = Prefab::LocalObjectId{1}, .parentLocalId = Prefab::LocalObjectId{0}, .name = "Child"}});
        const auto first = Prefab::PrefabInstanceId::Create(1).Value();
        const auto second = Prefab::PrefabInstanceId::Create(2).Value();
        SceneDocument document;
        REQUIRE(document
                    .LoadSaved({}, {{.instanceId = first, .sourcePrefab = PrefabAsset()},
                                    {.instanceId = second, .sourcePrefab = PrefabAsset()}})
                    .HasValue());
        auto source = BuildScenePrefabProjection(document.Snapshot(), resolver, ScenePrefabLimits());
        REQUIRE(source.HasValue());
        auto pinned = std::make_shared<const SceneSource::ScenePrefabProjection>(std::move(source).Value());
        auto prepared = BuildPrefabHierarchyProjection(PrefabAsset(9).Asset(), 1, document.Snapshot().revision.value, pinned, 6, {},
                                                       ScenePrefabLimits());
        REQUIRE(prepared.HasValue());
        const auto &rows = prepared.Value().entries;
        REQUIRE(rows.size() == 6);
        CHECK((rows[0].target == PrefabHierarchyTarget{first, std::nullopt}));
        CHECK((rows[3].target == PrefabHierarchyTarget{second, std::nullopt}));
        CHECK(rows[2].target != rows[5].target);
        CHECK(rows[2].source == asset);
        REQUIRE(rows[2].sceneObject.has_value());
        REQUIRE(rows[5].sceneObject.has_value());
        CHECK(rows[2].sceneObject != rows[5].sceneObject);
        CHECK(rows[2].parent == rows[1].target);
        CHECK(prepared.Value().Find(rows[2].target) == &rows[2]);
        CHECK(BuildPrefabHierarchyProjection(PrefabAsset(9).Asset(), 1, 1, pinned, 5, {}, ScenePrefabLimits()).HasError());
        const std::array occupied{SceneSource::SceneObjectSnapshot{.id = {rows[2].sceneObject->value}, .name = "Collision"}};
        auto collision = BuildPrefabHierarchyProjection(PrefabAsset(9).Asset(), 1, 1, pinned, 6, occupied, ScenePrefabLimits());
        REQUIRE(collision.HasValue());
        REQUIRE(collision.Value().entries[2].failure.has_value());
        CHECK_FALSE(collision.Value().entries[2].sceneObject.has_value());
    }

    TEST_CASE("Prefab hierarchy retains broken placement identity for repair and source navigation", "[editor][prefab][hierarchy]") {
        auto source = std::make_shared<SceneSource::ScenePrefabProjection>();
        const auto instance = Prefab::PrefabInstanceId::Create(7).Value();
        source->instances.push_back({.authored = {.instanceId = instance, .sourcePrefab = PrefabAsset()}});
        auto projection = BuildPrefabHierarchyProjection(PrefabAsset(9).Asset(), 2, 3, source, 1, {}, ScenePrefabLimits());
        REQUIRE(projection.HasValue());
        REQUIRE(projection.Value().entries.size() == 1);
        CHECK(projection.Value().entries.front().target.instance == instance);
        CHECK(projection.Value().entries.front().source == PrefabAsset().Asset());
        CHECK_FALSE(projection.Value().entries.front().target.member.has_value());
        CHECK(BuildPrefabHierarchyProjection({}, 2, 3, source, 1, {}, ScenePrefabLimits()).HasError());
    }

}  // namespace Horo::Editor
