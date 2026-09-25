#include "Horo/Editor/EditorMenuModel.h"

#include <initializer_list>

namespace Horo::Editor {
    namespace {
        EditorMenuItem Command(const std::string_view labelKey, const EditorMenuAction action = EditorMenuAction::None,
                               const bool enabled = false, const std::string_view shortcut = {},
                               const std::string_view macKeyEquivalent = {}, const std::string_view iconToken = {},
                               const std::optional<Runtime::PrimitiveId> primitive = std::nullopt) {
            return EditorMenuItem{EditorMenuItemKind::Command,
                                  labelKey,
                                  action,
                                  shortcut,
                                  macKeyEquivalent,
                                  iconToken,
                                  primitive,
                                  enabled,
                                  {}};
        }

        EditorMenuItem Separator() {
            return EditorMenuItem{.kind = EditorMenuItemKind::Separator};
        }

        EditorMenuItem Submenu(const std::string_view labelKey, std::initializer_list<EditorMenuItem> children,
                               const std::string_view iconToken = {}) {
            EditorMenuItem item;
            item.kind = EditorMenuItemKind::Submenu;
            item.labelKey = labelKey;
            item.iconToken = iconToken;
            item.enabledByDefault = true;
            item.children.assign(children);
            return item;
        }

        EditorMenuItem Submenu(const std::string_view labelKey, const std::vector<EditorMenuItem> &children,
                               const std::string_view iconToken = {}) {
            EditorMenuItem item;
            item.kind = EditorMenuItemKind::Submenu;
            item.labelKey = labelKey;
            item.iconToken = iconToken;
            item.enabledByDefault = !children.empty();
            item.children = children;
            return item;
        }

        [[nodiscard]] std::string_view GroupLabelKey(const Runtime::PrimitiveCreationGroup group) noexcept {
            using enum Runtime::PrimitiveCreationGroup;
            switch (group) {
                case Objects3D:
                    return "workspace.create.group.objects_3d";
                case Cameras:
                    return "workspace.create.group.cameras";
                case Lights:
                    return "workspace.create.group.lights";
                case Volumes:
                    return "workspace.create.group.volumes";
                case Audio:
                    return "workspace.create.group.audio";
                case Root:
                case NotCreatable:
                    return {};
            }
            return {};
        }

        [[nodiscard]] std::string_view GroupIconToken(const Runtime::PrimitiveCreationGroup group) noexcept {
            using enum Runtime::PrimitiveCreationGroup;
            switch (group) {
                case Objects3D:
                    return "create.group.objects_3d";
                case Cameras:
                    return "primitive.camera";
                case Lights:
                    return "create.group.lights";
                case Volumes:
                    return "primitive.trigger_volume";
                case Audio:
                    return "primitive.audio_source";
                case Root:
                case NotCreatable:
                    return {};
            }
            return {};
        }

        [[nodiscard]] std::vector<EditorMenuItem> BuildPrimitiveCreateMenuItems() {
            using enum Runtime::PrimitiveCreationGroup;
            std::vector<EditorMenuItem> result;
            for (const Runtime::PrimitiveDescriptor &descriptor : Runtime::PrimitiveCatalog::All()) {
                if (descriptor.creationGroup == Root) {
                    result.push_back(
                        Command(descriptor.id.value, EditorMenuAction::CreatePrimitive, true, {}, {}, descriptor.iconToken, descriptor.id));
                }
            }
            constexpr std::array kGroups{Objects3D, Cameras, Lights, Volumes, Audio};
            for (const Runtime::PrimitiveCreationGroup group : kGroups) {
                std::vector<EditorMenuItem> children;
                for (const Runtime::PrimitiveDescriptor &descriptor : Runtime::PrimitiveCatalog::All()) {
                    if (descriptor.creationGroup == group) {
                        children.push_back(Command(descriptor.id.value, EditorMenuAction::CreatePrimitive, true, {}, {},
                                                   descriptor.iconToken, descriptor.id));
                    }
                }
                if (!children.empty()) {
                    result.push_back(Submenu(GroupLabelKey(group), children, GroupIconToken(group)));
                }
            }
            return result;
        }
    }  // namespace

    /** @copydoc GetPrimitiveCreateMenuItems */
    const std::vector<EditorMenuItem> &GetPrimitiveCreateMenuItems() {
        static const std::vector<EditorMenuItem> items = BuildPrimitiveCreateMenuItems();
        return items;
    }

    /** @copydoc GetEditorMenuModel */
    const EditorMenuModel &GetEditorMenuModel() {
        static const EditorMenuModel model{{
            Submenu("web_workspace.menu.file",
                    {
                        Command("web_workspace.menu.new_project", EditorMenuAction::NewProject, true, "Ctrl+N", "n"),
                        Command("web_workspace.menu.open_project", EditorMenuAction::OpenProject, true, "Ctrl+O", "o"),
                        Command("web_workspace.menu.open_recent"),
                        Separator(),
                        Command("web_workspace.menu.save", EditorMenuAction::SaveScene, true, "Ctrl+S", "s"),
                        Command("web_workspace.menu.save_as", EditorMenuAction::SaveSceneAs, true, "Ctrl+Shift+S", "S"),
                        Command("web_workspace.menu.save_copy_as", EditorMenuAction::SaveSceneCopyAs, true),
                        Separator(),
                        Command("web_workspace.menu.editor_settings", EditorMenuAction::OpenEditorSettings, true, "Ctrl+,", ","),
                        Command("web_workspace.menu.exit", EditorMenuAction::ExitApplication, true, "Alt+F4", "q"),
                    }),
            Submenu("web_workspace.menu.edit",
                    {
                        Command("web_workspace.menu.undo", EditorMenuAction::Undo, true, "Ctrl+Z", "z"),
                        Command("web_workspace.menu.redo", EditorMenuAction::Redo, true, "Ctrl+Shift+Z", "Z"),
                        Separator(),
                        Command("web_workspace.menu.cut"),
                        Command("web_workspace.menu.copy"),
                        Command("web_workspace.menu.paste"),
                        Separator(),
                        Command("web_workspace.menu.preferences"),
                    }),
            Submenu("web_workspace.menu.assets",
                    {
                        Command("web_workspace.menu.import_asset", EditorMenuAction::ImportAssets, true),
                        Command("web_workspace.menu.reimport_all"),
                        Command("web_workspace.menu.refresh_asset_index"),
                    }),
            Submenu("web_workspace.menu.game_object",
                    {
                        Submenu("workspace.create", GetPrimitiveCreateMenuItems(), "action.create"),
                        Separator(),
                        Command("web_workspace.menu.character_setup"),
                    }),
            Submenu("web_workspace.menu.component", {Command("web_workspace.menu.add_component")}),
            Submenu("web_workspace.menu.window",
                    {
                        Submenu("web_workspace.menu.workspace",
                                {
                                    Command("web_workspace.menu.editor_workspace"),
                                    Command("web_workspace.menu.ui_canvas_editor"),
                                    Command("web_workspace.menu.cinematic_sequencer"),
                                }),
                        Submenu("web_workspace.menu.asset_editors",
                                {
                                    Command("web_workspace.menu.animation_editor"),
                                    Command("web_workspace.menu.material_editor"),
                                    Command("web_workspace.menu.particle_editor"),
                                    Command("web_workspace.menu.prefab_editor"),
                                    Command("web_workspace.menu.shader_graph"),
                                }),
                        Submenu("web_workspace.menu.tools",
                                {
                                    Command("web_workspace.menu.asset_browser"),
                                    Command("web_workspace.menu.plugin_manager"),
                                    Command("web_workspace.menu.package_manager"),
                                    Command("web_workspace.menu.primitives_panel"),
                                    Command("web_workspace.menu.decal_placement"),
                                    Command("web_workspace.menu.destruction_setup"),
                                    Command("web_workspace.menu.save_load_manager"),
                                    Command("web_workspace.menu.post_processing_stack"),
                                    Command("web_workspace.menu.xr_setup"),
                                }),
                        Submenu("web_workspace.menu.diagnostics",
                                {
                                    Command("web_workspace.menu.physics_debugger"),
                                    Command("web_workspace.menu.network_debugger"),
                                    Command("web_workspace.menu.navigation_bake"),
                                    Command("web_workspace.menu.lod_debugger"),
                                    Command("web_workspace.menu.virtual_texturing_debug"),
                                    Command("web_workspace.menu.observability_dashboard"),
                                }),
                        Submenu("web_workspace.menu.runtime",
                                {
                                    Command("web_workspace.menu.mcp_panel"),
                                    Command("web_workspace.menu.console_panel"),
                                    Command("web_workspace.menu.audio_mixer"),
                                    Command("web_workspace.menu.input_mapping_editor"),
                                    Command("web_workspace.menu.pcg_graph_editor"),
                                    Command("web_workspace.menu.gameplay_behavior_editor"),
                                }),
                        Separator(),
                        Command("web_workspace.menu.localization_editor"),
                    }),
            Submenu("web_workspace.menu.build",
                    {
                        Command("web_workspace.menu.build_job", EditorMenuAction::OpenBuildPreview, true),
                        Command("web_workspace.menu.run_tests", EditorMenuAction::OpenTestPreview, true),
                        Command("web_workspace.menu.prepare_release", EditorMenuAction::OpenReleasePreview, true),
                        Command("web_workspace.menu.publish_candidate", EditorMenuAction::OpenPublishPreview, true),
                    }),
            Submenu("web_workspace.menu.help", {Command("web_workspace.menu.documentation")}),
        }};
        return model;
    }
}  // namespace Horo::Editor
