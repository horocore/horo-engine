#include "FullEditorUiTestActions.h"

#include "FullEditorUiTestHost.h"
#include "Horo/Editor/EditorMenuModel.h"
#include "Horo/Editor/GuiScreenHost.h"

#include <fstream>
#include <imgui_test_engine/imgui_te_context.h>
#include <string>

namespace Horo::Tests::FullEditorActions {
    void CreateRootBox(UiScenarioPipe &pipeline) {
        pipeline.Step("Create a Box from the hierarchy root menu", [](ImGuiTestContext &ui) {
            ui.ItemClick("**/##HierarchyRootDrop", ImGuiMouseButton_Right);
            ui.SetRef("//$FOCUSED");
            ui.ItemClick("###hierarchy_create_root");
            ui.Yield();
            ui.ItemClick("//**/###hierarchy_create_workspace.create.group.objects_3d");
            ui.Yield();
            ui.ItemClick("//**/###hierarchy_create_primitive.mesh.box");
        });
        pipeline.Step("Observe the created object across the workspace", [](ImGuiTestContext &ui) {
            IM_CHECK(ui.ItemInfo("//**/##hierarchy_object_row").ID != 0);
        });
    }

    void ExerciseHierarchyEdits(UiScenarioPipe &pipeline) {
        pipeline.Step("Duplicate the authored hierarchy object", [](ImGuiTestContext &ui) {
            ui.ItemClick("//**/##hierarchy_object_row", ImGuiMouseButton_Right);
            ui.SetRef("//$FOCUSED");
            ui.ItemClick("###hierarchy_action_duplicate");
            ui.Yield();
        });
        pipeline.Step("Rename a hierarchy object", [](ImGuiTestContext &ui) {
            ui.ItemClick("//**/##hierarchy_object_row", ImGuiMouseButton_Right);
            ui.SetRef("//$FOCUSED");
            ui.ItemClick("###hierarchy_action_rename");
            ui.Yield();
            ui.ItemInputValue("//**/##Rename", "RenamedBox");
            ui.KeyPress(ImGuiKey_Enter);
            ui.Yield();
        });
        pipeline.Step("Delete a hierarchy object", [](ImGuiTestContext &ui) {
            ui.ItemClick("//**/##hierarchy_object_row", ImGuiMouseButton_Right);
            ui.SetRef("//$FOCUSED");
            ui.ItemClick("###hierarchy_action_delete");
            ui.Yield();
        });
    }

    void ExerciseInspectorComponents(UiScenarioPipe &pipeline) {
        pipeline.Step("Add every built-in optional component through the Inspector", [](ImGuiTestContext &ui) {
            ui.ItemClick("//**/##hierarchy_object_row");
            ui.Yield();
            if (!ui.ItemExists("//**/###InspectorAddComponent")) {
                ui.ItemClick("//**/horo.inspector/##ActivityItem");
                ui.Yield();
            }
            IM_CHECK(ui.ItemExists("//**/###InspectorAddComponent"));
            constexpr const char *componentItems[]{"//**/###inspector_component_camera", "//**/###inspector_component_light",
                                                   "//**/###inspector_component_trigger_volume",
                                                   "//**/###inspector_component_audio_source"};
            for (const char *const item : componentItems) {
                ui.ItemClick("//**/###InspectorAddComponent");
                ui.Yield();
                ui.ItemClick(item);
                ui.Yield();
            }

            ui.MouseMove("//**/###InspectorAddComponent");
            ui.MouseWheelY(100.0F);
            ui.Yield();

            const auto selectComboOption = [&ui](const char *combo, const int option) {
                ui.ItemClick(combo);
                ui.ItemClick(("//**/###combo_option_" + std::to_string(option)).c_str());
                ui.Yield();
            };
            selectComboOption("//**/###camera_projection", 1);
            ui.MouseMove("//**/###camera_projection");
            ui.MouseWheelY(-2.0F);
            ui.Yield();
            selectComboOption("//**/###light_kind", 2);
        });
    }

    void SelectOrthographicProjection(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor) {
        pipeline.Step("Choose Orthographic projection", [](ImGuiTestContext &ui) {
            ui.ItemClick("//**/Combo###viewport_projection");
            ui.ItemClick("//**/###combo_option_1");
        });
        pipeline.Step("Observe projection through the shared viewport handoff", [&editor](ImGuiTestContext &) {
            IM_CHECK(editor.ViewportProjection() == Runtime::CameraProjection::Orthographic);
        });
    }

    namespace {
        void AddInputMappingStep(UiScenarioPipe &pipeline) {
            pipeline.Step("Exercise input mapping pages and profile actions", [](ImGuiTestContext &ui) {
                ui.ItemClick("//**/horo.input_mapping/##ActivityItem");
                ui.Yield();
                IM_CHECK(ui.ItemExists("//**/Action Maps"));
                IM_CHECK(ui.ItemExists("//**/Rebind"));
                ui.ItemClick("//**/Rebind");
                ui.Yield();
                ui.KeyPress(ImGuiKey_K);
                ui.Yield();
                ui.ItemClick("//**/Devices");
                ui.Yield();
                ui.ItemClick("//**/Profiles");
                ui.Yield();
                IM_CHECK(ui.ItemExists("//**/Save Profile"));
                ui.ItemClick("//**/Save Profile");
                ui.ItemClick("//**/Save Project Override");
            });
        }

        void AddGlobalDockSteps(UiScenarioPipe &pipeline) {
            pipeline.Step("Exercise every global dock pane", [](ImGuiTestContext &ui) {
                if (!ui.ItemExists("//**/Assets")) {
                    ui.ItemClick("//**/horo.global_dock/##ActivityItem");
                    ui.Yield();
                }
                constexpr const char *tabs[]{"Console", "Build", "Ops", "MCP", "Perf", "Physics", "Audio", "Net", "Assets"};
                for (const char *const tab : tabs) {
                    ui.ItemClick(("//**/" + std::string{tab}).c_str());
                    ui.Yield();
                }
            });
        }

        void AddMenuRoutingStep(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor) {
            pipeline.Step("Exercise workspace menu routing and retain the dirty scene", [&editor](ImGuiTestContext &ui) {
                const auto dispatch = [&editor, &ui](Editor::EditorMenuInvocation invocation) {
                    editor.DispatchMenuInvocationOnNextFrame(std::move(invocation));
                    ui.Yield();
                };
                dispatch({Editor::EditorMenuAction::SaveScene, std::nullopt});
                dispatch({Editor::EditorMenuAction::Undo, std::nullopt});
                dispatch({Editor::EditorMenuAction::Redo, std::nullopt});
                dispatch({Editor::EditorMenuAction::CreatePrimitive, Runtime::PrimitiveId{"primitive.mesh.sphere"}});
                dispatch({Editor::EditorMenuAction::None, std::nullopt});

                dispatch({Editor::EditorMenuAction::OpenProject, std::nullopt});
                IM_CHECK(ui.ItemExists("//**/Unsaved Changes"));
                ui.ItemClick("//**/Stay Here");
                ui.Yield();
                IM_CHECK(editor.ActiveRoute() == Editor::GuiRouteKind::EditorWorkspace);
            });
        }

        /** @brief Waits for the asset card and opens one of its context actions. */
        void OpenAssetCardAction(ImGuiTestContext &ui, const char *actionPath) {
            for (int frame = 0; frame < 30 && !ui.ItemExists("//**/##AssetCard"); ++frame)
                ui.Yield();
            IM_CHECK(ui.ItemExists("//**/##AssetCard"));
            ui.ItemClick("//**/##AssetCard", ImGuiMouseButton_Right);
            ui.ItemClick(actionPath);
            ui.Yield();
        }

        void AddContentBrowserStep(UiScenarioPipe &pipeline) {
            pipeline.Step("Create and navigate an Asset Browser folder", [](ImGuiTestContext &ui) {
                if (!ui.ItemExists("//**/Assets")) {
                    ui.ItemClick("//**/horo.global_dock/##ActivityItem");
                    ui.Yield();
                }
                const char *dockPath = "//##DockBottom";
                ImGuiTestItemInfo dock = ui.WindowInfo(dockPath, ImGuiTestOpFlags_NoError);
                if (dock.Window == nullptr) {
                    dockPath = "//##DockBottomLeft";
                    dock = ui.WindowInfo(dockPath, ImGuiTestOpFlags_NoError);
                }
                if (dock.Window == nullptr) {
                    dockPath = "//##DockBottomRight";
                    dock = ui.WindowInfo(dockPath, ImGuiTestOpFlags_NoError);
                }
                IM_CHECK(dock.Window != nullptr);
                const std::string dockScope{dockPath};
                ui.ItemClick((dockScope + "/**/Assets").c_str());
                ui.Yield();

                ui.MouseMoveToPos({dock.RectClipped.Max.x - 20.0F, dock.RectClipped.GetCenter().y});
                ui.MouseClick(ImGuiMouseButton_Right);
                ui.ItemClick("//**/###content_browser_action_create_folder");
                ui.Yield();
                ui.ItemInputValue("//**/##ContentBrowserCreateFolderInput", "CoverageFolder");
                ui.ItemClick("//**/Create Folder");
                ui.Yield();

                OpenAssetCardAction(ui, "//**/###content_browser_action_asset_info");
                ui.KeyPress(ImGuiKey_Escape);
                ui.Yield();

                OpenAssetCardAction(ui, "//**/###content_browser_action_rename");
                ui.ItemInputValue("//**/##ContentBrowserRenameInput", "RenamedCoverageFolder");
                ui.ItemClick("//**/Cancel");
                ui.Yield();

                OpenAssetCardAction(ui, "//**/###content_browser_action_delete");
                ui.ItemClick("//**/Cancel");
            });
        }

        void ExerciseSettingsNetworkAndPackages(ImGuiTestContext &ui) {
            ui.ItemClick("//**/Network/nav");
            ui.Yield();
            ui.ItemInputValue("//**/##max-clients", "6");
            ui.ItemClick("//**/Packages/nav");
            ui.Yield();
            ui.ItemInputValue("//**/##download-threads", "3");
        }

        void AddSettingsStep(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor) {
            pipeline.Step("Exercise editor settings sections", [&editor](ImGuiTestContext &ui) {
                editor.Screens().DispatchMenuInvocation(
                    Editor::EditorMenuInvocation{Editor::EditorMenuAction::OpenEditorSettings, std::nullopt});
                ui.Yield();
                IM_CHECK(ui.ItemExists("//**/###settings_apply"));
                ui.ItemClick("//**/###startup");
                ui.ItemClick("//**/###combo_option_1");
                ui.ItemClick("//**/confirm-exit/toggle");
                ui.ItemClick("//**/restore-workspace/toggle");
                ui.ItemInputValue("//**/##default-scene", "assets/scenes/coverage.horo");
                ui.ItemClick("//**/Appearance/nav");
                ui.Yield();
                ui.ItemInputValue("//**/##custom-theme", "coverage-theme.json");
                ui.ItemInputValue("//**/##font-size", "15");
                ui.ItemClick("//**/Input/nav");
                ui.Yield();
                ui.ItemClick("//**/invert-y/toggle");
                ui.ItemClick("//**/Rendering/nav");
                ui.Yield();
                ui.ItemClick("//**/###viewport");
                ui.ItemClick("//**/###combo_option_2");
                ui.ItemClick("//**/grid/toggle");
                ui.ItemClick("//**/###tier");
                ui.ItemClick("//**/###combo_option_2");
                ui.ItemInputValue("//**/##texture-budget", "768 MB");
                ui.ItemClick("//**/Audio/nav");
                ui.Yield();
                ui.ItemClick("//**/###audio-device");
                ui.ItemClick("//**/###combo_option_1");
                ui.ItemClick("//**/audio-enabled/toggle");
                ExerciseSettingsNetworkAndPackages(ui);
                ui.ItemClick("//**/Diagnostics/nav");
                ui.Yield();
                ui.ItemClick("//**/###log-level");
                ui.ItemClick("//**/###combo_option_2");
                ui.ItemClick("//**/write-log/toggle");
                ui.ItemClick("//**/capture-stutter/toggle");
                ui.ItemInputValue("//**/##stutter", "24.5");
                ui.ItemClick("//**/Extensions/nav");
                ui.Yield();
                ui.ItemClick("//**/Restore Defaults");
                ui.ItemClick("//**/###settings_apply");
                ui.ItemClick("//**/###settings_cancel");
            });
        }
    }  // namespace

    void ExerciseWorkspacePanels(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor) {
        AddInputMappingStep(pipeline);
        AddGlobalDockSteps(pipeline);
        AddContentBrowserStep(pipeline);
        AddMenuRoutingStep(pipeline, editor);
        AddSettingsStep(pipeline, editor);
    }

    void ExerciseAssetImport(UiScenarioPipe &pipeline, FullEditorUiTestHost &editor) {
        pipeline.Step("Import a mesh through the asset-import modal", [&editor](ImGuiTestContext &ui) {
            const std::filesystem::path source = editor.Screens().CurrentProjectRoot() / "assets" / "coverage_triangle.obj";
            std::ofstream fixture{source, std::ios::binary};
            fixture << "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
            fixture.close();
            IM_CHECK(std::filesystem::is_regular_file(source));

            ui.Yield();
            editor.Screens().DispatchMenuInvocation(Editor::EditorMenuInvocation{Editor::EditorMenuAction::ImportAssets, std::nullopt});
            ui.Yield();
            IM_CHECK(editor.BeginAssetImport(source));
            ui.Yield();
            for (int frame = 0; frame < 30 && !ui.ItemExists("//**/##ImportTab0"); ++frame)
                ui.Yield();
            IM_CHECK(ui.ItemExists("//**/##ImportTab0"));
            IM_CHECK(ui.ItemExists("//**/##QueueItem0"));
            for (int tab = 1; tab < 4; ++tab) {
                ui.ItemClick(("//**/##ImportTab" + std::to_string(tab)).c_str());
                ui.Yield();
            }
            IM_CHECK(editor.ImportFirstPendingAsset());
            ui.Yield();
            IM_CHECK(ui.ItemExists("//**/Done"));
            ui.ItemClick("//**/Done");
            ui.Yield();

            editor.Screens().DispatchMenuInvocation(Editor::EditorMenuInvocation{Editor::EditorMenuAction::ImportAssets, std::nullopt});
            ui.Yield();
            IM_CHECK(editor.BeginAssetImport(source));
            IM_CHECK(editor.ImportFirstPendingAsset());
            IM_CHECK(editor.ResolvePendingAssetConflict());
            ui.Yield();
            IM_CHECK(ui.ItemExists("//**/Done"));
            ui.ItemClick("//**/Done");
        });
    }
}  // namespace Horo::Tests::FullEditorActions
