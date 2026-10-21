#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/screens/workspace/panels/viewport/WorldSourceShapeControls.h"
#include "editor/screens/workspace/panels/viewport/WorldSourceViewportTool.h"

#include <catch2/catch_test_macros.hpp>
#include <imgui.h>
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

        StepResult Step(WorldSourceViewportTool &tool, const EditorWorkspaceViewModel &view, ImVec2 mouse, bool down, bool eligible = true,
                        bool focused = true) {
            auto &io = ImGui::GetIO();
            io.MousePos = mouse;
            io.MouseDown[0] = down;
            Begin({640, 480});
            if (!focused)
                ImGui::SetWindowFocus(nullptr);
            StepResult result;
            result.captured = tool.Draw(*ImGui::GetWindowDrawList(), {0, 0}, {640, 480}, view, result.command, context,
                                        Math::ClipDepthRange::NegativeOneToOne, eligible);
            End();
            return result;
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
        static_cast<void>(harness.Step(tool, view, {320, 240}, false, eligible));
        const auto clicked = harness.Step(tool, view, {320, 240}, true, eligible);
        CHECK_FALSE(clicked.captured);
        const auto released = harness.Step(tool, view, {340, 240}, false, eligible);
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
        bool eligible = true;
        bool focused = true;
        SECTION("origin") {
            view.worldViewport.globalOrigin = Point::FromMillimeters(1, 0, 0);
        }
        SECTION("view incarnation") {
            ++view.worldViewport.viewRevision;
        }
        SECTION("document replacement") {
            view.worldViewport.snapshot = world.Replace(Document(), original->fence).Value();
        }
        SECTION("input revocation") {
            eligible = false;
        }
        SECTION("focus loss") {
            focused = false;
        }
        SECTION("escape") {
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
        }
        static_cast<void>(harness.Step(tool, view, {340, 240}, true, eligible, focused));
        ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        const auto released = harness.Step(tool, view, {340, 240}, false, eligible, focused);
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
}  // namespace
