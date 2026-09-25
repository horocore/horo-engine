#include "Horo/Editor/EditorMenuModel.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string_view>

using namespace Horo::Editor;

namespace {
    const EditorMenuItem *FindChild(const EditorMenuItem &parent, const std::string_view labelKey) {
        for (const EditorMenuItem &child : parent.children) {
            if (child.labelKey == labelKey) {
                return &child;
            }
        }
        return nullptr;
    }

    const EditorMenuItem *FindAction(const EditorMenuItem &parent, const EditorMenuAction action) {
        if (parent.action == action) {
            return &parent;
        }
        for (const EditorMenuItem &child : parent.children) {
            if (const EditorMenuItem *match = FindAction(child, action)) {
                return match;
            }
        }
        return nullptr;
    }

    TEST_CASE("Mirrors The Workspace Design Menu Hierarchy", "[unit][editor]") {
        const EditorMenuModel &model = GetEditorMenuModel();
        constexpr std::array expectedTopLevelKeys = {
            "web_workspace.menu.file",      "web_workspace.menu.edit",   "web_workspace.menu.assets", "web_workspace.menu.game_object",
            "web_workspace.menu.component", "web_workspace.menu.window", "web_workspace.menu.build",  "web_workspace.menu.help",
        };

        REQUIRE((model.menus.size() == expectedTopLevelKeys.size()));
        for (std::size_t index = 0; index < expectedTopLevelKeys.size(); ++index) {
            REQUIRE((model.menus[index].kind == EditorMenuItemKind::Submenu));
            REQUIRE((model.menus[index].labelKey == expectedTopLevelKeys[index]));
        }

        const EditorMenuItem *workspace = FindChild(model.menus[5], "web_workspace.menu.workspace");
        const EditorMenuItem *assetEditors = FindChild(model.menus[5], "web_workspace.menu.asset_editors");
        const EditorMenuItem *tools = FindChild(model.menus[5], "web_workspace.menu.tools");
        const EditorMenuItem *diagnostics = FindChild(model.menus[5], "web_workspace.menu.diagnostics");
        const EditorMenuItem *runtime = FindChild(model.menus[5], "web_workspace.menu.runtime");
        REQUIRE((workspace != nullptr && workspace->kind == EditorMenuItemKind::Submenu));
        REQUIRE((assetEditors != nullptr && assetEditors->kind == EditorMenuItemKind::Submenu));
        REQUIRE((tools != nullptr && tools->kind == EditorMenuItemKind::Submenu));
        REQUIRE((diagnostics != nullptr && diagnostics->kind == EditorMenuItemKind::Submenu));
        REQUIRE((runtime != nullptr && runtime->kind == EditorMenuItemKind::Submenu));
    }

    TEST_CASE("Distinguishes Implemented And Placeholder Commands", "[unit][editor]") {
        const EditorMenuModel &model = GetEditorMenuModel();
        const EditorMenuItem *save = FindAction(model.menus[0], EditorMenuAction::SaveScene);
        const EditorMenuItem *saveAs = FindAction(model.menus[0], EditorMenuAction::SaveSceneAs);
        const EditorMenuItem *saveCopyAs = FindAction(model.menus[0], EditorMenuAction::SaveSceneCopyAs);
        const EditorMenuItem *exit = FindAction(model.menus[0], EditorMenuAction::ExitApplication);
        const EditorMenuItem *newProject = FindAction(model.menus[0], EditorMenuAction::NewProject);
        const EditorMenuItem *openProject = FindAction(model.menus[0], EditorMenuAction::OpenProject);
        const EditorMenuItem *undo = FindAction(model.menus[1], EditorMenuAction::Undo);
        const EditorMenuItem *redo = FindAction(model.menus[1], EditorMenuAction::Redo);
        REQUIRE((save != nullptr && save->enabledByDefault));
        REQUIRE((saveAs != nullptr && saveAs->enabledByDefault));
        REQUIRE((saveCopyAs != nullptr && saveCopyAs->enabledByDefault));
        REQUIRE((exit != nullptr && exit->enabledByDefault));
        REQUIRE((newProject != nullptr && newProject->enabledByDefault));
        REQUIRE((openProject != nullptr && openProject->enabledByDefault));
        REQUIRE((undo != nullptr && undo->enabledByDefault));
        REQUIRE((redo != nullptr && redo->enabledByDefault));

        const EditorMenuItem *reimport = FindChild(model.menus[2], "web_workspace.menu.reimport_all");
        REQUIRE((reimport != nullptr));
        REQUIRE((reimport->action == EditorMenuAction::None));
        REQUIRE((!reimport->enabledByDefault));
    }

    TEST_CASE("Build Menu Exposes Four Independent Preview Workflows", "[unit][editor]") {
        const EditorMenuItem &build = GetEditorMenuModel().menus[6];
        REQUIRE((build.children.size() == 4));
        constexpr std::array actions{EditorMenuAction::OpenBuildPreview, EditorMenuAction::OpenTestPreview,
                                     EditorMenuAction::OpenReleasePreview, EditorMenuAction::OpenPublishPreview};
        constexpr std::array keys{"web_workspace.menu.build_job", "web_workspace.menu.run_tests", "web_workspace.menu.prepare_release",
                                  "web_workspace.menu.publish_candidate"};
        for (std::size_t index = 0; index < actions.size(); ++index) {
            REQUIRE((build.children[index].action == actions[index]));
            REQUIRE((build.children[index].labelKey == keys[index]));
            REQUIRE((build.children[index].enabledByDefault));
        }
    }

    TEST_CASE("Builds The Shared Catalog Create Tree", "[unit][editor]") {
        const std::vector<EditorMenuItem> &items = GetPrimitiveCreateMenuItems();
        REQUIRE((items.size() == 6));
        REQUIRE((items[0].labelKey == "primitive.object.empty"));
        constexpr std::array expectedGroups = {"workspace.create.group.objects_3d", "workspace.create.group.cameras",
                                               "workspace.create.group.lights", "workspace.create.group.volumes",
                                               "workspace.create.group.audio"};
        std::size_t primitiveCount = 1;
        for (std::size_t index = 0; index < expectedGroups.size(); ++index) {
            REQUIRE((items[index + 1].kind == EditorMenuItemKind::Submenu));
            REQUIRE((items[index + 1].labelKey == expectedGroups[index]));
            primitiveCount += items[index + 1].children.size();
            for (const EditorMenuItem &primitive : items[index + 1].children) {
                REQUIRE((primitive.action == EditorMenuAction::CreatePrimitive));
                REQUIRE((primitive.primitive.has_value()));
                REQUIRE((primitive.enabledByDefault));
                REQUIRE((!primitive.iconToken.empty()));
                REQUIRE((primitive.primitive->value.find("primitive.collider.") == std::string_view::npos));
            }
        }
        REQUIRE((primitiveCount == 14));

        const EditorMenuItem *create = FindChild(GetEditorMenuModel().menus[3], "workspace.create");
        REQUIRE((create != nullptr && create->kind == EditorMenuItemKind::Submenu));
        REQUIRE((create->children.size() == items.size()));
    }
}  // namespace
