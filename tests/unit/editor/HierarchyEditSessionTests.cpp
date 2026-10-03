#include "editor/screens/workspace/panels/hierarchy/HierarchyEditSession.h"
#include "editor/screens/workspace/panels/hierarchy/HierarchyRowLayout.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("Hierarchy edit session projects scene objects without ImGui", "[unit][editor]") {
    using namespace Horo::Editor;

    EditorWorkspaceViewModel viewModel;
    viewModel.documentRevision = DocumentRevision{3};
    viewModel.objects = {
        SceneObject{
            .id = SceneObjectId{1},
            .name = "Root",
            .kind = SceneObjectKind::GameObject,
        },
        SceneObject{
            .id = SceneObjectId{2},
            .parent = SceneObjectId{1},
            .name = "Camera",
            .kind = SceneObjectKind::Camera,
        },
    };
    viewModel.primarySelection = SceneObjectId{2};
    viewModel.selectedObjects = {SceneObjectId{2}};

    HierarchyEditSession session;
    session.Synchronize(viewModel);

    const std::vector<HierarchyVisibleRow> &rows = session.VisibleRows("");
    REQUIRE((rows.size() == 2));
    REQUIRE((rows[0].node->name == "Root"));
    REQUIRE((rows[1].node->name == "Camera"));
    REQUIRE((rows[1].depth == 1));
    REQUIRE((session.SelectedId() == HierarchyNodeId{2}));
    REQUIRE((session.ParentId(1) == std::nullopt));
    REQUIRE((session.ParentId(2) == HierarchyNodeId{1}));

    const std::vector<HierarchyVisibleRow> &filtered = session.VisibleRows("camera");
    REQUIRE((filtered.size() == 2));
}

TEST_CASE("Hierarchy edit session reduces semantic actions to typed commands", "[unit][editor]") {
    using namespace Horo::Editor;

    EditorWorkspaceViewModel viewModel;
    viewModel.documentRevision = DocumentRevision{1};
    viewModel.objects = {
        SceneObject{.id = SceneObjectId{7}, .name = "First"},
        SceneObject{.id = SceneObjectId{8}, .name = "Second"},
        SceneObject{.id = SceneObjectId{9}, .name = "Third"},
    };
    viewModel.primarySelection = SceneObjectId{7};
    viewModel.selectedObjects = {SceneObjectId{7}};
    HierarchyEditSession session;
    session.Synchronize(viewModel);
    static_cast<void>(session.VisibleRows(""));

    const EditorWorkspaceViewCommandData selected = session.SelectCommand(8, HierarchySelectionGesture::Replace);
    REQUIRE((selected.command == EditorWorkspaceViewCommand::SelectObject));
    REQUIRE((selected.objectSelection->objects == std::vector{SceneObjectId{8}}));
    REQUIRE((selected.objectSelection->primary == SceneObjectId{8}));

    const EditorWorkspaceViewCommandData toggled = session.SelectCommand(8, HierarchySelectionGesture::Toggle);
    REQUIRE((toggled.objectSelection->objects == std::vector({SceneObjectId{7}, SceneObjectId{8}})));
    REQUIRE((toggled.objectSelection->primary == SceneObjectId{8}));

    const EditorWorkspaceViewCommandData ranged = session.SelectCommand(9, HierarchySelectionGesture::Range);
    REQUIRE((ranged.objectSelection->objects == std::vector({SceneObjectId{8}, SceneObjectId{9}})));
    REQUIRE((ranged.objectSelection->primary == SceneObjectId{9}));

    const EditorWorkspaceViewCommandData renamed = HierarchyEditSession::RenameCommand(7, "Gameplay Camera");
    REQUIRE((renamed.command == EditorWorkspaceViewCommand::UpdateObjectName));
    REQUIRE((renamed.stringPayload == "Gameplay Camera"));
    REQUIRE((HierarchyEditSession::RenameCommand(7, "").command == EditorWorkspaceViewCommand::None));

    const EditorWorkspaceViewCommandData duplicated = HierarchyEditSession::DuplicateCommand(7);
    REQUIRE((duplicated.command == EditorWorkspaceViewCommand::DuplicateObject));

    const HierarchyNode editableNode{.id = 7, .locallyVisible = true, .locallyLocked = false};
    const EditorWorkspaceViewCommandData hidden = HierarchyEditSession::ToggleVisibilityCommand(editableNode);
    REQUIRE((hidden.command == EditorWorkspaceViewCommand::UpdateObjectEditorState));
    REQUIRE((hidden.objectPayload == SceneObjectId{7}));
    REQUIRE((hidden.editorStatePayload == SceneObjectEditorState{.visible = false, .locked = false}));
    const EditorWorkspaceViewCommandData locked = HierarchyEditSession::ToggleLockCommand(editableNode);
    REQUIRE((locked.command == EditorWorkspaceViewCommand::UpdateObjectEditorState));
    REQUIRE((locked.objectPayload == SceneObjectId{7}));
    REQUIRE((locked.editorStatePayload == SceneObjectEditorState{.visible = true, .locked = true}));

    viewModel.selectedObjects = {SceneObjectId{7}, SceneObjectId{8}};
    viewModel.primarySelection = SceneObjectId{8};
    session.Synchronize(viewModel);
    const EditorWorkspaceViewCommandData deleted = session.DeleteSelectionCommand();
    REQUIRE((deleted.command == EditorWorkspaceViewCommand::DeleteSelectedObjects));
    REQUIRE((deleted.objectSelection->objects == std::vector({SceneObjectId{7}, SceneObjectId{8}})));
    REQUIRE((deleted.objectSelection->primary == SceneObjectId{8}));

    viewModel.selectedObjects = {SceneObjectId{9}};
    viewModel.primarySelection = SceneObjectId{9};
    session.Synchronize(viewModel);
    const EditorWorkspaceViewCommandData outsideSelectionDelete = session.DeleteSelectionCommand();
    REQUIRE((outsideSelectionDelete.objectSelection->objects == std::vector{SceneObjectId{9}}));

    REQUIRE((HierarchyEditSession::IsDeleteShortcut(Horo::Input::Key::Delete, {})));
    REQUIRE((HierarchyEditSession::IsDeleteShortcut(Horo::Input::Key::Backspace, Horo::Input::ModifierState{.shift = true})));
    REQUIRE((HierarchyEditSession::IsDeleteShortcut(Horo::Input::Key::Backspace, Horo::Input::ModifierState{.command = true})));
    REQUIRE((!HierarchyEditSession::IsDeleteShortcut(Horo::Input::Key::Backspace, {})));
}

TEST_CASE("Hierarchy display type resolution follows component priority and concrete light kind", "[unit][editor]") {
    using namespace Horo::Editor;

    SceneObject generic{.id = SceneObjectId{1}, .name = "Generic"};
    REQUIRE((ResolveHierarchyNodeType(generic) == HierarchyNodeType::Empty));

    SceneObject mesh{.id = SceneObjectId{2}, .name = "Mesh", .kind = SceneObjectKind::Mesh};
    REQUIRE((ResolveHierarchyNodeType(mesh) == HierarchyNodeType::Mesh));

    SceneObject directional{.id = SceneObjectId{3}, .name = "Sun"};
    directional.components.light = Horo::Runtime::LightComponent{.kind = Horo::Runtime::LightKind::Directional};
    REQUIRE((ResolveHierarchyNodeType(directional) == HierarchyNodeType::DirectionalLight));
    directional.components.light->kind = Horo::Runtime::LightKind::Point;
    REQUIRE((ResolveHierarchyNodeType(directional) == HierarchyNodeType::PointLight));
    directional.components.light->kind = Horo::Runtime::LightKind::Spot;
    REQUIRE((ResolveHierarchyNodeType(directional) == HierarchyNodeType::SpotLight));

    SceneObject prioritized{.id = SceneObjectId{4}, .name = "Camera With Light", .kind = SceneObjectKind::Mesh};
    prioritized.components.camera = Horo::Runtime::CameraComponent{};
    prioritized.components.light = Horo::Runtime::LightComponent{};
    prioritized.components.audioSource = Horo::Runtime::AudioSourceComponent{};
    REQUIRE((ResolveHierarchyNodeType(prioritized) == HierarchyNodeType::DirectionalLight));
}

TEST_CASE("Hierarchy row layout preserves icon columns and safely shrinks the label", "[unit][editor]") {
    using namespace Horo::Editor;

    const HierarchyRowLayout regular = CalculateHierarchyRowLayout(280.0F, 0, 1.0F, 36.0F);
    REQUIRE((regular.IsValid()));
    REQUIRE((regular.height == 30.0F));
    REQUIRE((regular.row.minimum == 0.0F));
    REQUIRE((regular.row.maximum == 280.0F));
    REQUIRE((regular.chevron.minimum == 7.0F));
    REQUIRE((regular.chevron.Width() == 13.0F));
    REQUIRE((regular.typeIcon.minimum == 26.0F));
    REQUIRE((regular.typeIcon.Width() == 16.0F));
    REQUIRE((regular.label.maximum <= regular.actions.minimum));
    REQUIRE((regular.visibilityAction.Width() == 18.0F));
    REQUIRE((regular.lockAction.Width() == 18.0F));
    REQUIRE((regular.visibilityAction.maximum == regular.lockAction.minimum));

    const HierarchyRowLayout nested = CalculateHierarchyRowLayout(280.0F, 3, 1.0F, 36.0F);
    REQUIRE((nested.IsValid()));
    REQUIRE((nested.chevron.minimum > regular.chevron.minimum));
    REQUIRE((nested.typeIcon.minimum > regular.typeIcon.minimum));

    const HierarchyRowLayout child = CalculateHierarchyRowLayout(280.0F, 1, 1.0F, 36.0F);
    const HierarchyRowLayout grandchild = CalculateHierarchyRowLayout(280.0F, 2, 1.0F, 36.0F);
    REQUIRE((child.typeIcon.minimum - regular.typeIcon.minimum == 18.0F));
    REQUIRE((grandchild.typeIcon.minimum - child.typeIcon.minimum == 18.0F));

    const HierarchyRowLayout leaf = CalculateHierarchyRowLayout(280.0F, 0, 1.0F, 36.0F);
    REQUIRE((leaf.IsValid()));
    REQUIRE((leaf.chevron.Width() == 13.0F));
    REQUIRE((leaf.typeIcon.minimum == regular.typeIcon.minimum));

    const HierarchyRowLayout narrow = CalculateHierarchyRowLayout(48.0F, 8, 2.0F, 20.0F);
    REQUIRE((narrow.IsValid()));
    REQUIRE((narrow.label.Width() == 0.0F));
    REQUIRE((narrow.typeIcon.maximum <= narrow.label.minimum));
    REQUIRE((narrow.label.maximum <= narrow.actions.minimum));
    REQUIRE((narrow.visibilityAction.Width() == narrow.lockAction.Width()));
    REQUIRE((narrow.visibilityAction.maximum == narrow.lockAction.minimum));

    for (const float scale : {0.75F, 1.0F, 1.5F, 2.0F}) {
        const HierarchyRowLayout scaled = CalculateHierarchyRowLayout(280.0F * scale, 0, scale, 48.0F * scale);
        REQUIRE((scaled.IsValid()));
        REQUIRE((scaled.chevron.minimum == 7.0F * scale));
        REQUIRE((scaled.chevron.Width() == 13.0F * scale));
        REQUIRE((scaled.typeIcon.minimum == 26.0F * scale));
        REQUIRE((scaled.visibilityAction.Width() == 24.0F * scale));
        REQUIRE((scaled.lockAction.Width() == 24.0F * scale));
        REQUIRE((scaled.visibilityAction.maximum == scaled.lockAction.minimum));
    }

    for (const float width : {160.0F, 280.0F, 480.0F}) {
        const HierarchyRowLayout resized = CalculateHierarchyRowLayout(width, 0, 1.0F, 48.0F);
        REQUIRE((resized.IsValid()));
        REQUIRE((resized.typeIcon.minimum == 26.0F));
        REQUIRE((resized.actions.maximum == width - 7.0F));
    }
}
