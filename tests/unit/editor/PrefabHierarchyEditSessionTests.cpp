#include "../../support/AllocationProbe.h"
#include "PrefabHierarchyTestSupport.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyEditSession.h"

namespace Horo::Editor {
    using namespace SceneDocumentTestSupport;

    namespace {
        /** @brief Builds the real published source projection used by alias and range-selection regressions. */
        std::shared_ptr<const PrefabHierarchyProjection> CaptureProjection(PrefabHierarchyTestSupport::Project &project) {
            const std::array roots{project.asset};
            auto captured = project.Capture(project.registry.Snapshot(), roots, ScenePrefabLimits(), {});
            REQUIRE(captured.HasValue());
            SceneDocument document;
            REQUIRE(document.LoadSaved({}, {{.instanceId = project.instance, .sourcePrefab = PrefabAsset()}}).HasValue());
            auto sources = BuildScenePrefabProjection(document.Snapshot(), captured.Value().sources.resolver, ScenePrefabLimits());
            REQUIRE(sources.HasValue());
            auto pinned = std::make_shared<const SceneSource::ScenePrefabProjection>(std::move(sources).Value());
            auto projection = BuildPrefabHierarchyProjection(PrefabAsset(9).Asset(), 1, 1, pinned, 3, {}, ScenePrefabLimits());
            REQUIRE(projection.HasValue());
            return std::make_shared<const PrefabHierarchyProjection>(std::move(projection).Value());
        }

        /** @brief Measures only repeated reads after projection and visible-row caches have been populated. */
        Horo::Tests::AllocationProbe::Measurement UnchangedProjectionAllocations(HierarchyEditSession &session,
                                                                                 const EditorWorkspaceViewModel &model) {
            Horo::Tests::AllocationProbe::ScopedMeasurement allocations;
            for (int frame = 0; frame < 16; ++frame) {
                session.Synchronize(model);
                static_cast<void>(session.VisibleRows(""));
            }
            return allocations.Snapshot();
        }
    }  // namespace

    TEST_CASE("Hierarchy row aliases never become ordinary mutation targets and survive refresh", "[editor][prefab][hierarchy][native]") {
        PrefabHierarchyTestSupport::Project project;
        EditorWorkspaceViewModel model;
        model.documentRevision = {1};
        model.objects = {{.id = {std::numeric_limits<std::uint64_t>::max()}, .name = "Reserved authored identity"}};
        model.prefabHierarchy = CaptureProjection(project);
        model.prefabSelection = model.prefabHierarchy->entries.back().target;
        HierarchyEditSession session;
        session.Synchronize(model);
        const auto selected = session.SelectedId();
        REQUIRE(selected.has_value());
        CHECK(*selected != model.objects.front().id.value);
        const auto intent = session.SelectCommand(*selected, HierarchySelectionGesture::Replace);
        REQUIRE(intent.prefabHierarchy.has_value());
        CHECK_FALSE(intent.objectSelection.has_value());
        CHECK(intent.prefabHierarchy->target == *model.prefabSelection);
        const auto rejected = session.DeleteSelectionCommand();
        REQUIRE(rejected.prefabHierarchy.has_value());
        CHECK(ValidatePrefabHierarchyCommand(*rejected.prefabHierarchy, *model.prefabHierarchy).HasError());
        model.prefabHierarchy = std::make_shared<const PrefabHierarchyProjection>(*model.prefabHierarchy);
        session.Synchronize(model);
        CHECK(session.SelectedId() == selected);
        static_cast<void>(session.VisibleRows(""));
        CHECK(UnchangedProjectionAllocations(session, model).requests == 0);
        model.prefabHierarchy.reset();
        model.prefabSelection.reset();
        session.Synchronize(model);
        CHECK(session.PrefabTarget(*selected) == nullptr);
    }

    TEST_CASE("Hierarchy inclusive range selects authored rows while excluding source-owned Prefab aliases",
              "[editor][prefab][hierarchy][selection]") {
        PrefabHierarchyTestSupport::Project project;
        EditorWorkspaceViewModel model;
        model.documentRevision = {1};
        model.objects = {{.id = {1}, .name = "A authored start"}, {.id = {2}, .name = "Z authored end"}};
        model.primarySelection = SceneObjectId{1};
        model.selectedObjects = {SceneObjectId{1}};
        model.prefabHierarchy = CaptureProjection(project);
        HierarchyEditSession session;
        session.Synchronize(model);
        const auto &rows = session.VisibleRows("");
        REQUIRE(std::ranges::any_of(rows, [&session](const auto &row) {
            return row.node && session.PrefabTarget(row.node->id) != nullptr;
        }));
        static_cast<void>(session.SelectCommand(1, HierarchySelectionGesture::Replace));
        const auto range = session.SelectCommand(2, HierarchySelectionGesture::Range);
        REQUIRE(range.objectSelection.has_value());
        CHECK((range.objectSelection->objects == std::vector<SceneObjectId>{{1}, {2}}));
        CHECK(range.objectSelection->primary == SceneObjectId{2});
        CHECK_FALSE(range.prefabHierarchy.has_value());
        static_cast<void>(session.VisibleRows("Z authored end"));
        const auto missingAnchor = session.SelectCommand(2, HierarchySelectionGesture::Range);
        REQUIRE(missingAnchor.objectSelection.has_value());
        CHECK((missingAnchor.objectSelection->objects == std::vector<SceneObjectId>{{2}}));
    }
}  // namespace Horo::Editor
