#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "ViewportPanelRenderSupport.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"
#include "editor/screens/workspace/panels/viewport/ViewportPanel.h"
#include "editor/screens/workspace/panels/viewport/WorldSourceShapeControls.h"
#include "editor/screens/workspace/panels/viewport/WorldSourceViewportTool.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_map>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    namespace WS = WorldStreaming;
    using Point = Math::WorldCoordinate64;

    class Labels final : public ILocalizationService {
    public:
        const std::string &Get(std::string_view, std::string_view key) const override {
            const auto [entry, inserted] =
                labels_.try_emplace(std::string{key}, key == "workspace.world_sources.title" ? "Sources" : std::string{key});
            static_cast<void>(inserted);
            return entry->second;
        }

    private:
        mutable std::unordered_map<std::string, std::string> labels_;
    };

    struct GuiOwner {
        GuiOwner() {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.LogFilename = nullptr;
            io.DisplaySize = {1280, 720};
            io.DeltaTime = 1.0F / 60.0F;
            io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
        }

        ~GuiOwner() {
            ImGui::DestroyContext();
        }
    };

    struct FrameOptions {
        bool eligible{true};
        bool focused{true};
        ImVec2 size{640, 480};
        ImVec2 origin{};
        Math::ClipDepthRange depth{Math::ClipDepthRange::NegativeOneToOne};
        std::optional<std::string_view> activate;
        bool popupAction{};
    };

    /** @brief Headless ImGui frames exercise the actual viewport adapter without a GPU or substitute command producer. */
    struct Harness {
        GuiOwner gui;
        EngineDataBus engine;
        EditorDataBus editor;
        Labels localization;
        Theme::Fonts fonts{.sans = ImGui::GetIO().Fonts->Fonts.front(),
                           .sansCompact = ImGui::GetIO().Fonts->Fonts.front(),
                           .sansEmphasis = ImGui::GetIO().Fonts->Fonts.front()};
        ThemeContext theme{.fonts = fonts};
        EditorSettingsSnapshot settings{};
        EditorGuiContext context{.engineEvents = engine,
                                 .editorEvents = editor,
                                 .localization = localization,
                                 .theme = theme,
                                 .settings = settings};

        struct StepResult {
            bool captured{};
            EditorWorkspaceViewCommandData command;
        };

        void Begin(ImVec2 size) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos({0, 0}, ImGuiCond_Always);
            ImGui::SetNextWindowSize(size, ImGuiCond_Always);
            if (ImGui::GetCurrentContext()->OpenPopupStack.empty())
                ImGui::SetNextWindowFocus();
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
            ImGui::Begin("WorldSourceInput", nullptr,
                         ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);
        }

        void End() {
            ImGui::End();
            ImGui::PopStyleVar();
            ImGui::Render();
        }

        /** @brief Delivers keyboard activation to an actual submitted widget, never manufacturing an editor command. */
        void Activate(std::string_view label, ImGuiWindow &window) {
            auto &state = *ImGui::GetCurrentContext();
            ImGui::FocusWindow(&window);
            const auto id = window.GetID(label.data(), label.data() + label.size());
            state.NavActivateId = id;
            state.NavActivateDownId = id;
            state.NavActivatePressedId = id;
            state.NavActivateFlags = ImGuiActivateFlags_PreferTweak;
            state.NavInputSource = ImGuiInputSource_Keyboard;
        }

        StepResult Step(WorldSourceViewportTool &tool, const EditorWorkspaceViewModel &view, ImVec2 mouse, bool down,
                        const FrameOptions &options = {}) {
            auto &io = ImGui::GetIO();
            io.MousePos = mouse;
            io.MouseDown[0] = down;
            Begin(options.size);
            if (!options.focused)
                ImGui::SetWindowFocus(nullptr);
            if (options.activate) {
                auto *window = ImGui::GetCurrentWindow();
                if (options.popupAction) {
                    REQUIRE_FALSE(ImGui::GetCurrentContext()->OpenPopupStack.empty());
                    window = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
                    REQUIRE(window != nullptr);
                }
                Activate(*options.activate, *window);
            }
            StepResult result;
            result.captured = tool.Draw({*ImGui::GetWindowDrawList(), options.origin, options.size, view, options.depth, options.eligible},
                                        result.command, context);
            End();
            return result;
        }
    };

    /** @brief Reuses source-tool ImGui frames with the actual panel's routed input and presentation lifecycle. */
    struct PanelHarness : Harness {
        Horo::Editor::Tests::ViewportRenderSupport::FakeViewportRenderer renderer;
        Input::RawInputCollector collector;
        Input::InputRouter router;
        Input::InputContextToken workspace =
            router.PushContext(Input::InputContextId{"editor.workspace"}, Input::InputContextKind::EditorWorkspace);
        ViewportPanel panel;
        Input::FrameNumber frame{1};

        PanelHarness() {
            PanelContext binding{.dataBus = editor,
                                 .viewportRenderer = &renderer,
                                 .inputRouter = &router,
                                 .workspaceInputContext = &workspace};
            panel.OnAttach(binding);
        }

        ~PanelHarness() {
            panel.OnDetach();
        }

        EditorWorkspaceViewCommandData StepPanel(const EditorWorkspaceViewModel &view, ImVec2 mouse, bool down) {
            auto &io = ImGui::GetIO();
            io.AddMousePosEvent(mouse.x, mouse.y);
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
            Begin({640, 480});
            collector.BeginFrame(frame++);
            collector.SetPointerPosition(io.MousePos.x, io.MousePos.y);
            collector.SetPointerButton(Input::PointerButton::Primary, io.MouseDown[ImGuiMouseButton_Left]);
            router.BeginFrame(collector.Commit());
            EditorWorkspaceViewCommandData command;
            panel.DrawPanel({0, 0}, {640, 480}, view, command, context);
            End();
            return command;
        }
    };

    Assets::AssetId Owner() {
        std::array<std::uint8_t, 16> bytes{};
        bytes[0] = 42;
        return Assets::AssetId::FromBytes(bytes);
    }

    PartitionedWorldDocument Document() {
        PartitionedWorldDocument document{
            .settings = {.partition = WS::WorldPartitionId::Create(Owner().Bytes()).Value(),
                         .bounds = {},
                         .grid = WS::WorldCellQuantizationPolicy::Create({}, 1000, {-1, 1, -1, 1, -1, 1}, 1).Value()}};
        document.sources.push_back({.id = WS::StreamingSourceId::Create(1).Value(),
                                    .revision = WS::StreamingSourceRevision::Create(1).Value(),
                                    .shape = WS::StreamingSourceSphere{{}, 1000},
                                    .priority = WS::StreamingSourcePriority::Create(1).Value()});
        return document;
    }

    /** @brief Opens the actual toolbar popup and warms its layout for keyboard navigation. */
    void OpenSources(Harness &harness, WorldSourceViewportTool &tool, const EditorWorkspaceViewModel &view) {
        ImGui::GetIO().DisplaySize = {1920, 2160};
        static_cast<void>(harness.Step(tool, view, {-100, -100}, false));
        REQUIRE(harness.Step(tool, view, {-100, -100}, false, {.activate = "Sources"}).captured);
        REQUIRE(harness.Step(tool, view, {-100, -100}, false).captured);
        REQUIRE_FALSE(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    }

    /** @brief Activates a real localized popup widget through ImGui's keyboard path. */
    Harness::StepResult PopupAction(Harness &harness, WorldSourceViewportTool &tool, const EditorWorkspaceViewModel &view,
                                    std::string_view key) {
        static_cast<void>(harness.Step(tool, view, {-100, -100}, false));
        return harness.Step(tool, view, {-100, -100}, false, {.activate = key, .popupAction = true});
    }

    /** @brief Draws actual shape widgets, optionally activating one scoped form action. */
    Result<void> ShapeFrame(Harness &harness, PartitionedWorldSource &source, std::optional<std::string_view> action = {},
                            std::optional<std::size_t> plane = {}) {
        ImGui::GetIO().DisplaySize = {1920, 2160};
        harness.Begin({900, 2000});
        if (action) {
            if (plane.has_value())
                ImGui::PushID(static_cast<int>(*plane));
            harness.Activate(*action, *ImGui::GetCurrentWindow());
            if (plane.has_value())
                ImGui::PopID();
        }
        auto result = DrawWorldSourceShapeControls(source, harness.context);
        harness.End();
        return result;
    }

    TEST_CASE("Actual viewport handle drag commits once through canonical source history", "[unit][editor][gui][world_document]") {
        Harness harness;
        PartitionedWorldWorkspace world{Owner()};
        const auto original = world.Open(Document()).Value();
        EditorWorkspaceViewModel view;
        view.worldViewport = {original, {}, 1};
        WorldSourceViewportTool tool;
        static_cast<void>(harness.Step(tool, view, {320, 240}, false));
        CHECK(harness.Step(tool, view, {320, 240}, true).captured);
        CHECK(harness.Step(tool, view, {340, 240}, true).captured);
        CHECK(world.Snapshot().Value() == original);
        const auto released = harness.Step(tool, view, {340, 240}, false);
        REQUIRE(released.command.worldSource.has_value());
        CHECK(released.command.worldSource->action == WorldSourceToolAction::Edit);
        const auto committed = world.EditSource(released.command.worldSource->edit);
        REQUIRE(committed.HasValue());
        CHECK(std::get<WS::StreamingSourceSphere>(committed.Value()->document.sources[0].shape).center != Point{});
        const auto undone = world.ReplaySources(committed.Value()->fence, false);
        REQUIRE(undone.HasValue());
        CHECK(std::get<WS::StreamingSourceSphere>(undone.Value()->document.sources[0].shape).center == Point{});
    }

    TEST_CASE("Hiding the actual viewport cancels a source drag before the same document is shown again",
              "[unit][editor][gui][world_document]") {
        PartitionedWorldWorkspace world{Owner()};
        const auto original = world.Open(Document()).Value();
        EditorWorkspaceViewModel view{.worldViewport = {original, {}, 1}};
        for (const bool hide : {false, true}) {
            PanelHarness harness;
            static_cast<void>(harness.StepPanel(view, {320, 240}, false));
            static_cast<void>(harness.StepPanel(view, {320, 240}, true));
            static_cast<void>(harness.StepPanel(view, {340, 240}, true));
            if (hide) {
                EditorWorkspaceViewCommandData route{.command = EditorWorkspaceViewCommand::SaveScene};
                harness.panel.OnPresentationHidden(route);
                CHECK(route.command == EditorWorkspaceViewCommand::SaveScene);
            }
            const auto released = harness.StepPanel(view, {340, 240}, false);
            CHECK(released.worldSource.has_value() == !hide);
            CHECK(world.Snapshot().Value() == original);
        }
    }

    TEST_CASE("Viewport play and revoked input never capture source gestures", "[unit][editor][gui][world_document]") {
        Harness harness;
        PartitionedWorldWorkspace world{Owner()};
        const auto original = world.Open(Document()).Value();
        EditorWorkspaceViewModel view;
        view.worldViewport = {original, {}, 1};
        bool eligible = true;
        SECTION("play") {
            view.playState = EditorPlayState::Playing;
        }
        SECTION("revoked input") {
            eligible = false;
        }
        WorldSourceViewportTool tool;
        static_cast<void>(harness.Step(tool, view, {320, 240}, false, {.eligible = eligible}));
        const auto clicked = harness.Step(tool, view, {320, 240}, true, {.eligible = eligible});
        CHECK_FALSE(clicked.captured);
        const auto released = harness.Step(tool, view, {340, 240}, false, {.eligible = eligible});
        CHECK(released.command.command == EditorWorkspaceViewCommand::None);
        CHECK(world.Snapshot().Value() == original);
    }

    TEST_CASE("Active source gesture cancels when its view origin or exact root changes", "[unit][editor][gui][world_document]") {
        Harness harness;
        PartitionedWorldWorkspace world{Owner()};
        const auto original = world.Open(Document()).Value();
        EditorWorkspaceViewModel view;
        view.worldViewport = {original, {}, 1};
        WorldSourceViewportTool tool;
        static_cast<void>(harness.Step(tool, view, {320, 240}, false));
        REQUIRE(harness.Step(tool, view, {320, 240}, true).captured);
        FrameOptions options;
        SECTION("origin") {
            view.worldViewport.globalOrigin = Point::FromMillimeters(1, 0, 0);
        }
        SECTION("view incarnation") {
            ++view.worldViewport.viewRevision;
        }
        SECTION("camera pose changes within the same view") {
            view.viewportCamera.position.x += 1;
            view.viewportCamera.target.x += 1;
        }
        SECTION("document replacement") {
            view.worldViewport.snapshot = world.Replace(Document(), original->fence).Value();
        }
        SECTION("input revocation") {
            options.eligible = false;
        }
        SECTION("focus loss") {
            options.focused = false;
        }
        SECTION("escape") {
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        }
        SECTION("surface extent replacement") {
            options.size.x = 700;
        }
        SECTION("surface origin replacement") {
            options.origin.x = 10;
        }
        SECTION("depth convention replacement") {
            options.depth = Math::ClipDepthRange::ZeroToOne;
        }
        static_cast<void>(harness.Step(tool, view, {340, 240}, true, options));
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        const auto released = harness.Step(tool, view, {340, 240}, false, options);
        CHECK(released.command.command == EditorWorkspaceViewCommand::None);
        CHECK(world.Snapshot().Value()->document.sources == original->document.sources);
    }

    TEST_CASE("Shape-specific controls render valid detached fields at narrow and wide widths", "[unit][editor][gui][world_document]") {
        Harness harness;
        for (const float width : {260.0F, 900.0F}) {
            for (std::size_t kind = 0; kind < 4; ++kind) {
                auto source = Document().sources[0];
                if (kind == 1)
                    source.shape = WS::StreamingSourceBox{{}, Point::FromMillimeters(1000, 1000, 1000)};
                if (kind == 2) {
                    WS::StreamingSourceFrustum frustum{.maximumExtentMillimeters = 1000};
                    const std::array<Math::Vec3, 6> normals{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
                    for (std::size_t index = 0; index < normals.size(); ++index)
                        frustum.planes[index] = {normals[index], 500};
                    source.shape = frustum;
                }
                if (kind == 3) {
                    WS::StreamingSourcePathVolume path{.pointCount = 2, .halfExtentMillimeters = 100};
                    path.points[1] = Point::FromMillimeters(1000, 0, 0);
                    source.shape = path;
                }
                const auto before = source;
                harness.Begin({width, 700});
                const auto drawn = DrawWorldSourceShapeControls(source, harness.context);
                harness.End();
                CHECK(drawn.HasValue());
                CHECK(source == before);
                CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
            }
        }
    }

    TEST_CASE("Actual source popup publishes every supported shape through the canonical controller",
              "[unit][editor][gui][world_document]") {
        Harness harness;
        PartitionedWorldWorkspace world{Owner()};
        auto document = Document();
        document.sources.clear();
        const auto original = world.Open(document).Value();
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{"test-project", runtime, {}, {.world = {.partitionedWorld = &world}}};
        const auto origin = Point::FromMillimeters(9'000'000'000'000'000, 0, 0);
        REQUIRE(controller.SetWorldViewportBinding({original, origin, 1}).HasValue());
        WorldSourceViewportTool tool;
        OpenSources(harness, tool, controller.ViewModel());
        const auto missing = PopupAction(harness, tool, controller.ViewModel(), "workspace.world_sources.remove");
        CHECK_FALSE(missing.command.worldSource.has_value());
        constexpr std::array<const char *, 4> shapes{"sphere", "box", "frustum", "path"};
        for (std::size_t index = 0; index < shapes.size(); ++index) {
            const auto created =
                PopupAction(harness, tool, controller.ViewModel(), std::string{"workspace.world_sources."} + shapes[index]);
            REQUIRE(created.command.worldSource.has_value());
            REQUIRE(created.command.worldSource->action == WorldSourceToolAction::Create);
            REQUIRE(created.command.worldSource->edit.value.has_value());
            CHECK(created.command.worldSource->edit.value->shape.index() == index);
            CHECK(created.command.worldSource->globalOrigin == origin);
            controller.ProcessCommand(created.command);
            REQUIRE_FALSE(controller.ViewModel().worldSourceError.has_value());
            CHECK(world.Snapshot().Value()->document.sources.size() == index + 1);
        }
        CHECK(world.Snapshot().Value()->document.sources.size() == 4);
        CHECK(world.Snapshot().Value()->dirty);
    }

    TEST_CASE("Actual source popup preserves no-op edits removal history and cancellation", "[unit][editor][gui][world_document]") {
        Harness harness;
        PartitionedWorldWorkspace world{Owner()};
        const auto complete = world.Open(Document()).Value();
        Runtime::RuntimeSceneService runtime;
        EditorWorkspaceController controller{"test-project", runtime, {}, {.world = {.partitionedWorld = &world}}};
        REQUIRE(controller.SetWorldViewportBinding({complete, {}, 1}).HasValue());
        WorldSourceViewportTool tool;
        OpenSources(harness, tool, controller.ViewModel());
        const auto id = complete->document.sources.front().id;
        const auto selected = PopupAction(harness, tool, controller.ViewModel(), std::to_string(id.Value()));
        CHECK_FALSE(selected.command.worldSource.has_value());
        const auto applied = PopupAction(harness, tool, controller.ViewModel(), "workspace.world_sources.apply");
        REQUIRE(applied.command.worldSource.has_value());
        CHECK(applied.command.worldSource->action == WorldSourceToolAction::Edit);
        CHECK(applied.command.worldSource->edit.source == id);
        controller.ProcessCommand(applied.command);
        CHECK(world.Snapshot().Value() == complete);
        const auto removed = PopupAction(harness, tool, controller.ViewModel(), "workspace.world_sources.remove");
        REQUIRE(removed.command.worldSource.has_value());
        controller.ProcessCommand(removed.command);
        CHECK(world.Snapshot().Value()->document.sources.empty());
        const auto undo = PopupAction(harness, tool, controller.ViewModel(), "workspace.world_sources.undo");
        REQUIRE(undo.command.worldSource.has_value());
        controller.ProcessCommand(undo.command);
        CHECK(world.Snapshot().Value()->document.sources.size() == 1);
        const auto redo = PopupAction(harness, tool, controller.ViewModel(), "workspace.world_sources.redo");
        REQUIRE(redo.command.worldSource.has_value());
        controller.ProcessCommand(redo.command);
        CHECK(world.Snapshot().Value()->document.sources.empty());
        const auto restored = PopupAction(harness, tool, controller.ViewModel(), "workspace.world_sources.undo");
        REQUIRE(restored.command.worldSource.has_value());
        controller.ProcessCommand(restored.command);
        const auto retained = world.Snapshot().Value();
        const auto remaining = retained->document.sources.front().id;
        static_cast<void>(PopupAction(harness, tool, controller.ViewModel(), std::to_string(remaining.Value())));
        const auto cancelled = PopupAction(harness, tool, controller.ViewModel(), "workspace.world_sources.cancel");
        CHECK_FALSE(cancelled.command.worldSource.has_value());
        CHECK(world.Snapshot().Value() == retained);
    }

    TEST_CASE("Source popup rejects unrepresentable defaults without publishing partial geometry", "[unit][editor][gui][world_document]") {
        Harness harness;
        PartitionedWorldWorkspace world{Owner()};
        const auto root = world.Open(Document()).Value();
        EditorWorkspaceViewModel view;
        view.worldViewport = {root, Point::FromMillimeters(std::numeric_limits<std::int64_t>::max(), 0, 0), 1};
        WorldSourceViewportTool tool;
        OpenSources(harness, tool, view);
        for (const auto *key : {"workspace.world_sources.box", "workspace.world_sources.path"}) {
            const auto rejected = PopupAction(harness, tool, view, key);
            CHECK_FALSE(rejected.command.worldSource.has_value());
            CHECK(world.Snapshot().Value() == root);
        }
    }

    TEST_CASE("Source popup without an admitted world cannot emit authoring commands", "[unit][editor][gui][world_document]") {
        Harness harness;
        EditorWorkspaceViewModel view;
        WorldSourceViewportTool tool;
        OpenSources(harness, tool, view);
        const auto result = PopupAction(harness, tool, view, "workspace.world_sources.sphere");
        CHECK_FALSE(result.command.worldSource.has_value());
    }

    TEST_CASE("Invalid pointer release rolls back a previously valid source drag preview", "[unit][editor][gui][world_document]") {
        Harness harness;
        PartitionedWorldWorkspace world{Owner()};
        const auto root = world.Open(Document()).Value();
        EditorWorkspaceViewModel view;
        view.worldViewport = {root, {}, 1};
        WorldSourceViewportTool tool;
        static_cast<void>(harness.Step(tool, view, {320, 240}, false));
        REQUIRE(harness.Step(tool, view, {320, 240}, true).captured);
        REQUIRE(harness.Step(tool, view, {340, 240}, true).captured);
        static_cast<void>(harness.Step(tool, view, {1.0e9F, 240}, true));
        const auto rejected = harness.Step(tool, view, {1.0e9F, 240}, false);
        CHECK_FALSE(rejected.command.worldSource.has_value());
        CHECK(world.Snapshot().Value() == root);
        static_cast<void>(harness.Step(tool, view, {320, 240}, false));
        CHECK(harness.Step(tool, view, {320, 240}, true).captured);
        tool.Cancel();
        const auto afterCancel = harness.Step(tool, view, {340, 240}, false);
        CHECK_FALSE(afterCancel.command.worldSource.has_value());
        CHECK(world.Snapshot().Value() == root);
    }

    TEST_CASE("Frustum plane normalization is explicit and failure preserves the authored draft", "[unit][editor][gui][world_document]") {
        Harness harness;
        auto source = Document().sources.front();
        WS::StreamingSourceFrustum frustum{.maximumExtentMillimeters = 1000};
        const std::array<Math::Vec3, 6> normals{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
        for (std::size_t index = 0; index < normals.size(); ++index)
            frustum.planes[index] = {normals[index], 500};
        frustum.planes[0].inwardNormal = {2, 0, 0};
        source.shape = frustum;
        const auto before = source;
        CHECK(ShapeFrame(harness, source).HasError());
        CHECK(source == before);
        REQUIRE(ShapeFrame(harness, source, "workspace.world_sources.normalize", 0).HasValue());
        CHECK(std::get<WS::StreamingSourceFrustum>(source.shape).planes[0].inwardNormal == normals[0]);
        std::get<WS::StreamingSourceFrustum>(source.shape).planes[0].inwardNormal = {};
        const auto invalid = source;
        CHECK(ShapeFrame(harness, source, "workspace.world_sources.normalize", 0).HasError());
        CHECK(source == invalid);
    }

    TEST_CASE("Path form adds removes and rejects overflowing vertices without a partial edit", "[unit][editor][gui][world_document]") {
        Harness harness;
        auto source = Document().sources.front();
        WS::StreamingSourcePathVolume path{.pointCount = 2, .halfExtentMillimeters = 100};
        path.points[1] = Point::FromMillimeters(1000, 0, 0);
        source.shape = path;
        REQUIRE(ShapeFrame(harness, source).HasValue());
        REQUIRE(ShapeFrame(harness, source, "workspace.world_sources.add_point").HasValue());
        REQUIRE(std::get<WS::StreamingSourcePathVolume>(source.shape).pointCount == 3);
        CHECK(std::get<WS::StreamingSourcePathVolume>(source.shape).points[2] == Point::FromMillimeters(2000, 0, 0));
        REQUIRE(ShapeFrame(harness, source, "workspace.world_sources.remove_point").HasValue());
        CHECK(std::get<WS::StreamingSourcePathVolume>(source.shape).pointCount == 2);
        const auto minimum = source;
        REQUIRE(ShapeFrame(harness, source, "workspace.world_sources.remove_point").HasValue());
        CHECK(source == minimum);
        auto &geometry = std::get<WS::StreamingSourcePathVolume>(source.shape);
        geometry.points[1] = Point::FromMillimeters(std::numeric_limits<std::int64_t>::max(), 0, 0);
        const auto overflow = source;
        CHECK(ShapeFrame(harness, source, "workspace.world_sources.add_point").HasError());
        CHECK(source == overflow);
        geometry.pointCount = 1;
        const auto malformed = source;
        CHECK(ShapeFrame(harness, source).HasError());
        CHECK(source == malformed);
    }

}  // namespace
