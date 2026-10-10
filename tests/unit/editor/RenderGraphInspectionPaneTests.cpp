#include "../../../apps/HoroEditor/app/RenderGraphInspectionPane.h"
#include "../runtime/renderer/RenderGraphInspectionTestSupport.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Runtime/Render/RenderGraphInspectionErrors.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPanel.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <imgui_internal.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Owns state shared with the retained query callback across GUI draw frames. */
    struct InspectionProbe {
        using Snapshot = std::shared_ptr<const Render::RenderGraphInspectionSnapshot>;
        Snapshot publication;
        std::size_t queries{};
        bool failed{};

        Result<Snapshot> Read() {
            ++queries;
            return failed ? Result<Snapshot>::Failure(MakeError(Render::RenderGraphInspectionErrors::Closed))
                          : Result<Snapshot>::Success(publication);
        }
    };

    /** @brief Owns the complete software GUI fixture, with no window/device or native renderer. */
    struct GuiFixture {
        GuiFixture() : ownedContext(ImGui::CreateContext()) {
            auto &io = ImGui::GetIO();
            io.DisplaySize = {1280, 900};
            io.DeltaTime = 1.0F / 60.0F;
            io.IniFilename = nullptr;
            io.Fonts->AddFontDefault();
            static_cast<void>(io.Fonts->Build());
        }

        ~GuiFixture() {
            ImGui::DestroyContext(ownedContext);
        }

        GuiFixture(const GuiFixture &) = delete;
        GuiFixture &operator=(const GuiFixture &) = delete;
        GuiFixture(GuiFixture &&) = delete;
        GuiFixture &operator=(GuiFixture &&) = delete;

        ImGuiContext *const ownedContext;
    };

    /** @brief Draws one bounded pane region while preserving commands as read-only inspection output. */
    void DrawPane(GlobalDockPanel &panel, const EditorGuiContext &context, const float width) {
        ImGui::NewFrame();
        ImGui::SetNextWindowSize({width, 700});
        ImGui::Begin("InspectionFixture", nullptr, ImGuiWindowFlags_NoSavedSettings);
        EditorWorkspaceViewModel model;
        EditorWorkspaceViewCommandData command;
        panel.DrawPanel(ImGui::GetCursorScreenPos(), {width, 640}, model, command, context);
        ImGui::End();
        ImGui::Render();
        REQUIRE(ImGui::GetDrawData() != nullptr);
        CHECK(ImGui::GetDrawData()->TotalVtxCount > 0);
    }

    /** @brief Finds the real dock content child without exposing pane implementation state. */
    ImGuiWindow &ContentWindow() {
        ImGuiWindow *parent = ImGui::FindWindowByName("InspectionFixture");
        REQUIRE(parent != nullptr);
        for (ImGuiWindow *window : ImGui::GetCurrentContext()->Windows) {
            if (window->ParentWindow == parent && window->ChildId == parent->GetID("##Content"))
                return *window;
        }
        FAIL("Inspection dock content child is missing");
        return *parent;
    }

    /** @brief Sends actual mouse press/release frames through the composed shared controls. */
    void Click(GlobalDockPanel &panel, const EditorGuiContext &context, const ImVec2 position, const float width) {
        auto &io = ImGui::GetIO();
        io.AddMousePosEvent(position.x, position.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
        DrawPane(panel, context, width);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
        DrawPane(panel, context, width);
    }

    /** @brief Selects each actual localized section through its open shared popup. */
    void SelectSection(GlobalDockPanel &panel, const EditorGuiContext &context, const int section, const float width) {
        const ImVec2 origin = ContentWindow().DC.CursorStartPos;
        Click(panel, context, {origin.x + 10, origin.y + 10}, width);
        auto &popups = ImGui::GetCurrentContext()->OpenPopupStack;
        REQUIRE(popups.Size == 1);
        REQUIRE(popups.back().Window != nullptr);
        const ImVec2 firstRow = popups.back().Window->DC.CursorStartPos;
        const float rowHeight = Ui::ScaledLayoutValue(28.0F);
        Click(panel, context, {firstRow.x + 10, firstRow.y + (static_cast<float>(section) + 0.5F) * rowHeight}, width);
        CHECK(ImGui::GetCurrentContext()->OpenPopupStack.empty());
    }

    /** @brief Checks the actual table, including all submitted rows beyond the viewport clip. */
    void RequireRows(const int count) {
        const ImGuiID id = ContentWindow().GetID("##RenderGraphRecords");
        const ImGuiTable *table = ImGui::GetCurrentContext()->Tables.GetByKey(id);
        REQUIRE(table != nullptr);
        CHECK(table->LastFrameActive == ImGui::GetFrameCount());
        CHECK(table->ColumnsCount == 4);
        CHECK(table->CurrentRow == count);
    }

    /** @brief Resolves page-control positions from shared metrics rather than fixed offsets. */
    ImVec2 PageButtonPosition(const bool next) {
        const auto &tokens = Theme::GetActiveTokens();
        const float comboHeight = DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Small).minimumHeight;
        const float buttonHeight = DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Medium).minimumHeight;
        const float spacing = ImGui::GetStyle().ItemSpacing.y;
        const ImVec2 origin = ContentWindow().DC.CursorStartPos;
        return {origin.x + 10, origin.y + comboHeight + spacing + buttonHeight * 0.5F + (next ? buttonHeight + spacing : 0.0F)};
    }

    /** @brief Creates more than one display page through the real bounded graph compilers. */
    std::shared_ptr<const Render::RenderGraphInspectionSnapshot> MultiPageSnapshot() {
        using namespace Render;
        auto builder = Test::RequireBuilder({.maxPasses = 129, .maxResources = 1, .maxUsages = 1, .maxDependencies = 1});
        for (std::size_t index = 0; index < 129; ++index)
            static_cast<void>(Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics));
        auto graph = Test::RequireGraph(builder);
        auto schedule = Test::RequireSchedule(graph);
        auto lifetime = CompileRenderGraphLifetimePlan(graph, schedule, {});
        REQUIRE(lifetime.HasValue());
        auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, Test::Queues, {});
        REQUIRE(synchronization.HasValue());
        auto execution = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), Test::Queues);
        REQUIRE(execution.HasValue());
        auto captured = CaptureRenderGraphInspection(graph, schedule, lifetime.Value(), execution.Value(), {{41}, {11}, 2});
        REQUIRE(captured.HasValue());
        return std::move(captured).Value();
    }

    /** @brief Exercises all localized typed sections at narrow and wide bounds. */
    void ExerciseSections(GlobalDockPanel &panel, const EditorGuiContext &context, LocalizationService &localization) {
        for (const auto locale : {"en-US", "tr-TR"}) {
            REQUIRE(localization.Prepare(LocaleTag{locale}));
            REQUIRE(localization.ActivatePrepared());
            DrawPane(panel, context, 220);
            DrawPane(panel, context, 900);
            constexpr std::array Rows{3, 2, 2, 2, 2, 3, 1};
            for (int section = 0; section < static_cast<int>(Rows.size()); ++section) {
                SelectSection(panel, context, section, 220);
                RequireRows(Rows[static_cast<std::size_t>(section)]);
                DrawPane(panel, context, 900);
                RequireRows(Rows[static_cast<std::size_t>(section)]);
            }
        }
    }

    /** @brief Exercises real page controls and resets navigation when the immutable source changes. */
    void ExercisePaging(GlobalDockPanel &panel, const EditorGuiContext &context,
                        std::shared_ptr<const Render::RenderGraphInspectionSnapshot> &publication) {
        const auto initial = publication;
        publication = MultiPageSnapshot();
        DrawPane(panel, context, 220);
        SelectSection(panel, context, 0, 220);
        RequireRows(128);
        Click(panel, context, PageButtonPosition(true), 220);
        RequireRows(1);
        Click(panel, context, PageButtonPosition(true), 220);
        RequireRows(1);
        Click(panel, context, PageButtonPosition(false), 220);
        RequireRows(128);
        Click(panel, context, PageButtonPosition(true), 220);
        publication = initial;
        DrawPane(panel, context, 220);
        RequireRows(3);
    }

    /** @brief Installs both real catalogs and activates the initial language. */
    void LoadCatalogs(LocalizationService &localization) {
        const std::filesystem::path catalogs{std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor"};
        REQUIRE(localization.LoadCatalogFile(catalogs / "en-US.json"));
        REQUIRE(localization.LoadCatalogFile(catalogs / "tr-TR.json"));
        REQUIRE(localization.Prepare(LocaleTag{"en-US"}));
        REQUIRE(localization.ActivatePrepared());
    }
}  // namespace

TEST_CASE("Render graph pane is inert while hidden and renders localized narrow empty failed and captured states",
          "[unit][editor][gui][inspection]") {
    GuiFixture fixture;
    EngineDataBus engineEvents;
    EditorDataBus editorEvents;
    LocalizationService localization{LocaleTag{"en-US"}};
    LoadCatalogs(localization);
    ImFont *font = ImGui::GetIO().Fonts->Fonts.front();
    const Theme::Fonts fonts{.sans = font, .sansCompact = font, .sansEmphasis = font};
    const ThemeContext theme{fonts};
    const EditorSettingsSnapshot settings{};
    const EditorGuiContext context{engineEvents, editorEvents, localization, theme, settings};
    const auto probe = std::make_shared<InspectionProbe>();
    auto pane = MakeRenderGraphInspectionPane([probe] {
        return probe->Read();
    });
    CHECK(probe->queries == 0);
    IGlobalDockPane *inspectionPane = pane.get();
    GlobalDockPanel panel;
    REQUIRE(panel.RegisterPane(std::move(pane)));
    DrawPane(panel, context, 700);
    CHECK(probe->queries == 0);
    REQUIRE(panel.ActivatePane("horo.global_dock.render_graph"));
    DrawPane(panel, context, 220);
    CHECK(probe->queries == 1);
    probe->failed = true;
    DrawPane(panel, context, 220);
    CHECK(probe->queries == 2);
    probe->failed = false;
    const auto sources = Render::Test::CompileSources();
    const auto captured =
        Render::CaptureRenderGraphInspection(sources.graph, sources.schedule, sources.lifetime, sources.execution, {{41}, {9}, 1});
    REQUIRE(captured.HasValue());
    probe->publication = captured.Value();
    ExerciseSections(panel, context, localization);
    ExercisePaging(panel, context, probe->publication);
    const auto beforeHidden = probe->queries;
    REQUIRE(panel.ActivatePane("horo.global_dock.assets"));
    DrawPane(panel, context, 500);
    CHECK(probe->queries == beforeHidden);
    inspectionPane->Detach();
    REQUIRE(panel.ActivatePane("horo.global_dock.render_graph"));
    DrawPane(panel, context, 220);
    CHECK(probe->queries == beforeHidden);
    CHECK(probe.use_count() == 1);
}
