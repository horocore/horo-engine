#include "Horo/Editor/DefaultWorkspacePanels.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/PCG/PCGCookedPlan.h"
#include "PCGCanvasTestSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "helpers/editor_ui/HeadlessEditorGuiFixture.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    using namespace PCGDocumentTestSupport;
    using PCGCanvasTestSupport::CanvasWorkspace;

    namespace {
        /** @brief Publishes the binary graph registry fixture before workspace composition. */
        void PublishGraphAsset(Assets::AssetRegistry &registry) {
            const auto source = ProjectPath::Parse(Identity().key.source.Value()).Value();
            const auto metadata = ProjectPath::Parse(Identity().key.source.Value() + ".horo").Value();
            REQUIRE(registry.Publish({{Asset(), Assets::AssetTypeId::Parse("core.pcg_graph").Value(), source, metadata}}).status ==
                    Assets::AssetRegistryBuildStatus::Complete);
        }

        /** @brief Verifies that the common document command retires the canonical clean PCG owner and its tab together. */
        void CheckWorkspacePCGClose(EditorWorkspaceController &controller, const DocumentInstanceId instance) {
            EditorWorkspaceViewCommandData close;
            close.command = EditorWorkspaceViewCommand::CloseWorkspaceDocument;
            close.documentInstance = instance;
            controller.ProcessCommand(close);
            CHECK(controller.PCGDocuments().Snapshots().Value().empty());
            CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().empty());
        }
    }  // namespace

    TEST_CASE("Workspace PCG activation owns binary sessions dirty tabs recovery save and close", "[unit][editor][pcg][workspace]") {
        Project project;
        Assets::AssetRegistry registry;
        PublishGraphAsset(registry);
        Runtime::RuntimeSceneService runtime;
        CancellationSource cancellation;
        REQUIRE(runtime.Startup(cancellation.Token()).HasValue());
        EditorWorkspaceController controller(project.root, runtime, registry.Snapshot(),
                                             {.mutations = &project.mutations,
                                              .durableFiles = &project.files,
                                              .documentSessionOwner = Asset(9),
                                              .pcgCatalog = Catalog()});
        EditorWorkspaceViewCommandData open;
        open.command = EditorWorkspaceViewCommand::OpenSourceFile;
        open.sourceOpenRequest = SourceOpenRequest{.path = project.path, .mode = SourceOpenMode::EmbeddedOnly};
        controller.ProcessCommand(open);
        auto documents = controller.PCGDocuments().Snapshots();
        REQUIRE(documents.HasValue());
        REQUIRE(documents.Value().size() == 1);
        const auto initial = documents.Value().front();
        REQUIRE(controller.ViewModel().workspacePanelHost.DocumentTabs().size() == 1);
        CHECK(controller.ViewModel().workspacePanelHost.ActiveDocument() == initial.identity.instance);
        REQUIRE(controller.ExecutePCGDocument(initial.identity.instance, Edit(initial, 42)).HasValue());
        CHECK(controller.HasDirtyPCGDocuments());
        CHECK_FALSE(controller.ViewModel().isDirty);
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        controller.ProcessCommand(open);
        CHECK(controller.PCGDocuments().Snapshots().Value().size() == 1);
        controller.UpdateAutosave(61, 1);
        REQUIRE(controller.PCGDocuments().InspectRecovery(initial.identity.instance).Value());
        CHECK(Assets::CaptureAssetSource(project.root, project.path, 1024 * 1024).Value().bytes == Capture().bytes);
        ErrorIs(controller.ClosePCGDocument(initial.identity.instance, false), PCGDocumentError(PCGDocumentCommandFailureReason::Dirty));
        project.files.failWrite = true;
        REQUIRE(controller.SavePCGDocuments().HasError());
        CHECK(controller.HasDirtyPCGDocuments());
        CHECK(controller.ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        project.files.failWrite = false;
        REQUIRE(controller.SavePCGDocuments().HasValue());
        CHECK_FALSE(controller.HasDirtyPCGDocuments());
        CHECK_FALSE(controller.ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        CheckWorkspacePCGClose(controller, initial.identity.instance);
        controller.ProcessCommand(open);
        auto reopened = controller.PCGDocuments().Snapshots().Value().front();
        CHECK(reopened.session != initial.session);
        CHECK(reopened.source->Data().deterministicSeed == 42);
        ErrorIs(controller.ExecutePCGDocument(reopened.identity.instance, Edit(initial, 43)),
                PCGDocumentError(PCGDocumentCommandFailureReason::Stale));
        REQUIRE(controller.ShutdownPCGDocuments().HasValue());
    }

    TEST_CASE("PCG palette recipes use actual catalog schemas and owning settings codecs", "[unit][editor][pcg][canvas]") {
        auto composed = CreatePCGEditorCatalog();
        REQUIRE(composed.HasValue());
        CHECK(composed.Value().palette.size() == 17);
        const std::array grants{PCG::PCGCapability::Validation};
        auto created = PCG::PCGNodeCatalog::Create({PCG::PCGHostProfile::Interactive, PCG::PCGCapabilitySet::Create(grants).Value()});
        REQUIRE(created.HasValue());
        auto catalog = std::move(created).Value();
        for (const auto &entry : composed.Value().palette)
            REQUIRE(catalog.Register(entry.kind).HasValue());
        const auto root = catalog.Snapshot().Value();
        for (const auto &entry : composed.Value().palette) {
            const auto descriptor = root.Find(entry.prototype.type).Value();
            PCG::PCGCookedNode candidate;
            candidate.id = Id<PCG::NodeId>(1);
            candidate.type = entry.prototype.type;
            candidate.version = entry.prototype.version;
            candidate.runtimeContractVersion = descriptor->runtimeContractVersion;
            candidate.determinism = descriptor->determinism;
            candidate.requiredCapabilities = descriptor->requiredCapabilities;
            candidate.payload = entry.prototype.payload;
            for (const auto &pin : entry.prototype.pins)
                candidate.pins.push_back({pin.id, pin.direction, pin.type, pin.cardinality});
            REQUIRE(root.Validate(candidate).HasValue());
        }
    }

    TEST_CASE("Registered PCG workspace creates real nodes and replays one semantic edit", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        const auto before = fixture.View().snapshot;
        for (const auto &entry : fixture.controller->ViewModel().pcgPalette) {
            auto request = fixture.Request(PCGEditorAction::CreateNode);
            request.type = entry.prototype.type;
            fixture.Send(std::move(request));
            REQUIRE_FALSE(fixture.View().error);
        }
        CHECK(fixture.View().snapshot.source->Data().nodes.size() == before.source->Data().nodes.size() + 17);
        CHECK(fixture.View().snapshot.dirty);
        CHECK(fixture.controller->ViewModel().workspacePanelHost.DocumentTabs().front().dirty);
        CHECK_FALSE(fixture.controller->ViewModel().isDirty);
        const auto after = fixture.View().snapshot;
        fixture.Send(fixture.Request(PCGEditorAction::Undo));
        REQUIRE_FALSE(fixture.View().error);
        CHECK(fixture.View().snapshot.source->Data().nodes.size() == after.source->Data().nodes.size() - 1);
        fixture.Send(fixture.Request(PCGEditorAction::Redo));
        REQUIRE_FALSE(fixture.View().error);
        CHECK(fixture.View().snapshot.state == after.state);
    }

    TEST_CASE("PCG workspace selection and navigation never dirty source and survive refocusing", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        const auto before = fixture.View().snapshot;
        auto request = fixture.Request();
        request.presentation.pan = {64, -32};
        request.presentation.zoom = 1.5F;
        request.presentation.filter = "Noise";
        request.presentation.selection = {Id<PCG::NodeId>(1)};
        request.presentation.placements.front().position = {300, 400};
        fixture.Send(request);
        REQUIRE_FALSE(fixture.View().error);
        fixture.Unchanged(before);
        fixture.Open();
        CHECK(fixture.View().canvas.pan.x == 64);
        CHECK(fixture.View().canvas.selection == request.presentation.selection);
        CHECK(fixture.View().canvas.placements.front().position.x == 300);
        fixture.Send(fixture.Request(PCGEditorAction::Undo));
        REQUIRE(fixture.View().error);  // Presentation created no undo entry.
        fixture.Unchanged(before);
    }

    TEST_CASE("PCG node removal closes edges and exposed bindings in one reversible transaction", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        const auto before = fixture.View().snapshot;
        auto request = fixture.Request(PCGEditorAction::RemoveSelection);
        request.presentation.selection = {Id<PCG::NodeId>(1)};
        fixture.Send(request);
        REQUIRE_FALSE(fixture.View().error);
        CHECK(fixture.View().snapshot.source->Data().nodes.size() == 1);
        CHECK(fixture.View().snapshot.source->Data().edges.empty());
        CHECK(fixture.View().snapshot.source->Data().exposedInputs.empty());
        fixture.Send(fixture.Request(PCGEditorAction::Undo));
        REQUIRE_FALSE(fixture.View().error);
        CHECK(fixture.View().snapshot.source->Data() == before.source->Data());
        CHECK_FALSE(fixture.View().snapshot.dirty);
    }

    TEST_CASE("PCG typed connection completion and disconnect share canonical undo", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        auto disconnect = fixture.Request(PCGEditorAction::Disconnect);
        disconnect.edge = Id<PCG::EdgeId>(1);
        fixture.Send(disconnect);
        REQUIRE_FALSE(fixture.View().error);
        REQUIRE(fixture.View().snapshot.source->Data().edges.empty());
        auto connect = fixture.Request(PCGEditorAction::Connect);
        connect.sourceNode = Id<PCG::NodeId>(1);
        connect.sourcePin = Id<PCG::PinId>(2);
        connect.targetNode = Id<PCG::NodeId>(2);
        connect.targetPin = Id<PCG::PinId>(3);
        fixture.Send(connect);
        REQUIRE_FALSE(fixture.View().error);
        REQUIRE(fixture.View().snapshot.source->Data().edges.size() == 1);
        fixture.Send(fixture.Request(PCGEditorAction::Undo));
        CHECK(fixture.View().snapshot.source->Data().edges.empty());
        fixture.Send(fixture.Request(PCGEditorAction::Undo));
        CHECK_FALSE(fixture.View().snapshot.dirty);
    }

    TEST_CASE("PCG invalid connections reject before canonical root revision or dirty publication", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        const auto before = fixture.View().snapshot;
        auto request = fixture.Request(PCGEditorAction::Connect);
        request.sourceNode = Id<PCG::NodeId>(1);
        request.sourcePin = Id<PCG::PinId>(2);
        request.targetNode = Id<PCG::NodeId>(2);
        request.targetPin = Id<PCG::PinId>(3);
        SECTION("duplicate occupied single input") {}
        SECTION("dangling pin") {
            request.targetPin = Id<PCG::PinId>(99);
        }
        SECTION("wrong direction") {
            request.sourcePin = Id<PCG::PinId>(1);
        }
        SECTION("self cycle") {
            request.targetNode = request.sourceNode;
            request.targetPin = Id<PCG::PinId>(1);
        }
        SECTION("stale revision") {
            ++request.fence.expectedRevision;
        }
        SECTION("stale catalog") {
            ++request.fence.catalogRevision;
        }
        SECTION("unauthorized") {
            request.fence.canEdit = false;
        }
        SECTION("cancelled") {
            fixture.cancellation.RequestCancellation();
            request.fence.cancellation = fixture.cancellation.Token();
        }
        fixture.Send(request);
        REQUIRE(fixture.View().error);
        fixture.Unchanged(before);
        fixture.Send(fixture.Request(PCGEditorAction::Undo));
        REQUIRE(fixture.View().error);
        fixture.Unchanged(before);
    }

    TEST_CASE("PCG mismatched pin types and indirect cycles reject through actual source validation", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        const auto initial = fixture.View().snapshot;
        auto node = initial.source->Data().nodes[1];
        node.pins.push_back(
            {Id<PCG::PinId>(4), PCG::PCGPinDirection::Output, PCG::PCGPinType::Scalar, PCG::PCGPinCardinality::Multiple, {}});
        auto edit = Fence(initial);
        edit.operations.emplace_back(PutPCGNode{node});
        REQUIRE(fixture.controller->ExecutePCGDocument(initial.identity.instance, edit).HasValue());
        auto request = fixture.Request(PCGEditorAction::Connect);
        request.sourceNode = Id<PCG::NodeId>(2);
        request.sourcePin = Id<PCG::PinId>(4);
        request.targetNode = Id<PCG::NodeId>(1);
        request.targetPin = Id<PCG::PinId>(1);
        SECTION("cycle") {}
        SECTION("type mismatch") {
            const auto snapshot = fixture.View().snapshot;
            auto changed = snapshot.source->Data().nodes.front();
            changed.pins.front().type = PCG::PCGPinType::Boolean;
            changed.pins.front().defaultValue = PCG::PCGGraphValue{true};
            auto command = Fence(snapshot);
            command.operations = {RemovePCGInput{Id<PCG::ExposedInputId>(1)}, PutPCGNode{changed}};
            REQUIRE(fixture.controller->ExecutePCGDocument(snapshot.identity.instance, command).HasValue());
            request.fence = Fence(fixture.View().snapshot);
        }
        const auto before = fixture.View().snapshot;
        fixture.Send(request);
        REQUIRE(fixture.View().error);
        fixture.Unchanged(before);
    }

    TEST_CASE("PCG canvas rejects malformed bounded presentation and unknown recipes", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        const auto before = fixture.View().snapshot;
        auto request = fixture.Request();
        SECTION("nonfinite zoom") {
            request.presentation.zoom = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("out of range zoom") {
            request.presentation.zoom = 0;
        }
        SECTION("duplicate placement") {
            request.presentation.placements[1].node = request.presentation.placements[0].node;
        }
        SECTION("stale selection") {
            request.presentation.selection = {Id<PCG::NodeId>(99)};
        }
        SECTION("unbounded filter") {
            request.presentation.filter.resize(129, 'x');
        }
        SECTION("incomplete connection") {
            request.presentation.connectingNode = Id<PCG::NodeId>(1);
        }
        SECTION("unknown creation type") {
            request.action = PCGEditorAction::CreateNode;
            request.type = Id<PCG::NodeTypeId>(999);
        }
        SECTION("empty removal") {
            request.action = PCGEditorAction::RemoveSelection;
        }
        fixture.Send(request);
        REQUIRE(fixture.View().error);
        fixture.Unchanged(before);
        CHECK(fixture.View().canvas.zoom == 1.0F);
        CHECK(fixture.View().canvas.filter.empty());
    }

    TEST_CASE("PCG workspace cancels gestures across navigation publication replacement and close", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        auto gesture = fixture.Request();
        gesture.presentation.draggingNode = Id<PCG::NodeId>(1);
        gesture.presentation.dragOrigin = gesture.presentation.placements.front().position;
        gesture.presentation.placements.front().position = {900, 900};
        gesture.presentation.connectingNode = Id<PCG::NodeId>(1);
        gesture.presentation.connectingPin = Id<PCG::PinId>(2);
        fixture.Send(gesture);
        REQUIRE_FALSE(fixture.View().error);
        EditorWorkspaceViewCommandData focus;
        focus.command = EditorWorkspaceViewCommand::FocusWorkspaceDocument;
        focus.documentInstance = fixture.View().snapshot.identity.instance;
        fixture.controller->ProcessCommand(focus);
        CHECK_FALSE(fixture.View().canvas.draggingNode);
        CHECK_FALSE(fixture.View().canvas.connectingPin);
        CHECK(fixture.View().canvas.placements.front().position.x == gesture.presentation.dragOrigin.x);
        fixture.Send(gesture);
        auto edit = Edit(fixture.View().snapshot, 50);
        REQUIRE(fixture.controller->ExecutePCGDocument(fixture.View().snapshot.identity.instance, edit).HasValue());
        CHECK_FALSE(fixture.View().canvas.connectingPin);
        CHECK_FALSE(fixture.View().canvas.draggingNode);
        fixture.Send(gesture);
        REQUIRE(fixture.View().error);
        const auto old = fixture.View().snapshot;
        REQUIRE(fixture.controller->ClosePCGDocument(old.identity.instance, true).HasValue());
        CHECK(fixture.controller->ViewModel().pcgDocuments.empty());
        fixture.Open();
        CHECK(fixture.View().snapshot.session != old.session);
        CHECK(fixture.View().canvas.selection.empty());
        fixture.Send(gesture);
        REQUIRE(fixture.View().error);
        REQUIRE(fixture.controller->ShutdownPCGDocuments(true).HasValue());
        CHECK(fixture.controller->ViewModel().pcgDocuments.empty());
    }

    TEST_CASE("PCG catalog replacement clears pending connection against the actual document owner", "[unit][editor][pcg][canvas]") {
        Project project;
        auto document = project.Open();
        PCGWorkspaceDocument projection;
        RefreshPCGCanvas(projection, document.Snapshot().Value());
        projection.canvas.connectingNode = Id<PCG::NodeId>(1);
        projection.canvas.connectingPin = Id<PCG::PinId>(2);
        REQUIRE(document.ReplaceCatalog(Catalog(2)).HasValue());
        RefreshPCGCanvas(projection, document.Snapshot().Value());
        CHECK_FALSE(projection.canvas.connectingPin);
        CHECK(projection.snapshot.catalogRevision == 2);
        CHECK_FALSE(projection.snapshot.dirty);
    }

    TEST_CASE("PCG graph canvas renders both theme layers and localized narrow inspector without source mutation",
              "[unit][editor][pcg][canvas][render]") {
        CanvasWorkspace fixture;
        Tests::EditorGuiContextFixture gui{{1400, 1000}};
        ImGui::GetIO().IniFilename = nullptr;
        const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        for (const auto locale : {"en-US", "tr-TR"})
            REQUIRE(gui.localization.LoadCatalogFile(root / (std::string{locale} + ".json")));
        const auto before = fixture.View().snapshot;
        auto select = fixture.Request();
        select.presentation.selection = {Id<PCG::NodeId>(1)};
        fixture.Send(select);
        for (const auto locale : {"en-US", "tr-TR"}) {
            REQUIRE(gui.localization.Prepare(LocaleTag{locale}));
            REQUIRE(gui.localization.ActivatePrepared());
            for (const auto theme : {Theme::Preset::HoroDark, Theme::Preset::Light}) {
                Theme::SetThemePreset(theme);
                Theme::ApplyCurrentTheme();
                CHECK(ImGui::GetStyle().Colors[ImGuiCol_Text].x == Theme::GetActiveTokens().colors.textPrimary.x);
                for (const float width : {300.0F, 1300.0F}) {
                    for (int frame = 0; frame < 2; ++frame) {
                        gui.imgui.BeginFrame();
                        ImGui::SetNextWindowPos({0, 0});
                        ImGui::SetNextWindowSize({width, 900});
                        ImGui::Begin("PCG", nullptr, ImGuiWindowFlags_NoSavedSettings);
                        CHECK_FALSE(DrawPCGGraphEditor(fixture.View(), fixture.controller->ViewModel().pcgPalette, gui.localization,
                                                       gui.imgui.Fonts()));
                        ImGui::End();
                        gui.imgui.EndFrame();
                    }
                    const auto *draw = ImGui::GetDrawData();
                    REQUIRE(draw);
                    CHECK(draw->TotalVtxCount > 0);
                    for (int list = 0; list < draw->CmdListsCount; ++list)
                        for (const auto &vertex : draw->CmdLists[list]->VtxBuffer) {
                            CHECK(std::isfinite(vertex.pos.x));
                            CHECK(std::isfinite(vertex.pos.y));
                        }
                }
            }
        }
        fixture.Unchanged(before);
        Theme::SetThemePreset(Theme::Preset::HoroDark);
    }

    TEST_CASE("PCG document presenter is registered and detaches without closing the canonical session",
              "[unit][editor][pcg][canvas][lifecycle]") {
        CanvasWorkspace fixture;
        Tests::EditorGuiContextFixture gui;
        WorkspacePanelRegistry registry;
        RegisterDefaultWorkspacePanels(registry);
        const auto &panels = registry.GetPanelsForArea(WorkspaceDockArea::Document);
        const auto found = std::ranges::find_if(panels, [](const auto &panel) {
            return panel->GetId() == PCGGraphPanelId;
        });
        REQUIRE(found != panels.end());
        CHECK((*found)->GetDisplayName() == "web_workspace.menu.pcg_graph_editor");
        PanelContext context{gui.editorEvents};
        registry.AttachAll(context);
        const auto before = fixture.View().snapshot;
        gui.imgui.BeginFrame();
        ImGui::Begin("RegisteredPCG");
        EditorWorkspaceViewCommandData command;
        (*found)->DrawPanel({}, {900, 600}, fixture.controller->ViewModel(), command, gui.context);
        ImGui::End();
        gui.imgui.EndFrame();
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        registry.DetachAll();
        fixture.Unchanged(before);
        // Detached drawing is inert even outside an ImGui frame.
        (*found)->DrawPanel({}, {}, fixture.controller->ViewModel(), command, gui.context);
        CHECK(command.command == EditorWorkspaceViewCommand::None);
        CHECK(fixture.controller->PCGDocuments().Snapshots().Value().size() == 1);
        registry.Clear();
        fixture.Unchanged(before);
    }

    TEST_CASE("PCG active document owns common undo redo save and retains failed-save state", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        const auto before = fixture.View().snapshot;
        auto create = fixture.Request(PCGEditorAction::CreateNode);
        create.type = fixture.controller->ViewModel().pcgPalette.front().prototype.type;
        fixture.Send(create);
        REQUIRE_FALSE(fixture.View().error);
        const auto edited = fixture.View().snapshot;
        EditorWorkspaceViewCommandData common;
        common.command = EditorWorkspaceViewCommand::UndoScene;
        fixture.controller->ProcessCommand(common);
        CHECK(fixture.View().snapshot.state == before.state);
        common.command = EditorWorkspaceViewCommand::RedoScene;
        fixture.controller->ProcessCommand(common);
        CHECK(fixture.View().snapshot.state == edited.state);
        CHECK_FALSE(fixture.controller->ViewModel().canUndo);
        const auto beforeSave = fixture.View().snapshot;
        fixture.project.files.failWrite = true;
        common.command = EditorWorkspaceViewCommand::SaveScene;
        fixture.controller->ProcessCommand(common);
        REQUIRE(fixture.View().error);
        fixture.Unchanged(beforeSave);
        CHECK(fixture.View().snapshot.dirty);
        fixture.project.files.failWrite = false;
        fixture.controller->ProcessCommand(common);
        REQUIRE_FALSE(fixture.View().error);
        CHECK_FALSE(fixture.View().snapshot.dirty);
        CheckWorkspacePCGClose(*fixture.controller, fixture.View().snapshot.identity.instance);
        CHECK(fixture.controller->ViewModel().pcgDocuments.empty());
    }

    TEST_CASE("PCG node and pin identity exhaustion rejects without a partial creation transaction", "[unit][editor][pcg][canvas]") {
        auto data = Graph();
        SECTION("node identity") {
            data.nodes[1].id = Id<PCG::NodeId>(std::numeric_limits<std::uint64_t>::max());
            data.edges.front().targetNode = data.nodes[1].id;
        }
        SECTION("pin identity") {
            data.nodes[1].pins.front().id = Id<PCG::PinId>(std::numeric_limits<std::uint64_t>::max());
            data.edges.front().targetPin = data.nodes[1].pins.front().id;
        }
        auto graph = PCG::PCGGraphAsset::Create(data, {.supportedNodeTypes = Catalog().types});
        REQUIRE(graph.HasValue());
        const auto palette = CreatePCGEditorCatalog();
        REQUIRE(palette.HasValue());
        PCGEditorRequest request;
        request.action = PCGEditorAction::CreateNode;
        request.type = palette.Value().palette.front().prototype.type;
        ErrorIs(PreparePCGEdit(request, graph.Value(), palette.Value().palette),
                PCGDocumentError(PCGDocumentCommandFailureReason::Exhausted));
        CHECK(graph.Value().Data().nodes.size() == 2);
    }

    TEST_CASE("Active PCG document routes keyboard removal independently of scene selection", "[unit][editor][pcg][canvas]") {
        CanvasWorkspace fixture;
        CHECK(fixture.controller->HasActivePCGDocument());
        const auto sceneDirty = fixture.controller->ViewModel().isDirty;
        auto selection = fixture.Request();
        selection.presentation.selection = {Id<PCG::NodeId>(1)};
        fixture.Send(selection);
        EditorWorkspaceViewCommandData command;
        command.command = EditorWorkspaceViewCommand::DeleteSelectedObjects;
        fixture.controller->ProcessCommand(command);
        REQUIRE_FALSE(fixture.View().error);
        CHECK(fixture.View().snapshot.source->Data().nodes.size() == 1);
        CHECK(fixture.controller->ViewModel().isDirty == sceneDirty);
        command.command = EditorWorkspaceViewCommand::UndoScene;
        fixture.controller->ProcessCommand(command);
        REQUIRE_FALSE(fixture.View().error);
        CHECK_FALSE(fixture.View().snapshot.dirty);
        command.command = EditorWorkspaceViewCommand::ChangeActivePanel;
        command.targetIndex = 3;
        command.stringPayload = "horo.viewport";
        fixture.controller->ProcessCommand(command);
        CHECK_FALSE(fixture.controller->HasActivePCGDocument());
    }
}  // namespace Horo::Editor
