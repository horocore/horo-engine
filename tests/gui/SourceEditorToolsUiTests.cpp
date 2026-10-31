#include "EditorUiTestHarness.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/DataBus.h"
#include "OwnedTestDirectory.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/GlobalDockPanel.h"
#include "editor/screens/workspace/panels/global_dock/panes/network/GlobalDockNetworkPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/operations/GlobalDockOperationsPane.h"
#include "editor/source_editor/SourceEditorTools.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cfloat>
#include <fstream>
#include <imgui_internal.h>
#include <imgui_test_engine/imgui_te_context.h>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;

    /** @brief Drives visible production source tools and the real widget through canonical publications. */
    struct SourceToolsUi final {
        Horo::Tests::EditorUiTestHarness harness{Horo::Tests::CreateHeadlessEditorUiTestSurface()};
        Horo::Tests::OwnedTestDirectory directory{"source-tools-ui"};
        SourceFileOpenService documents{directory.Path()};
        SourceEditorWidget widget{SourceEditingViewId{7}};
        SourceEditorTools tools;
        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        LocalizationService localization{LocaleTag{"en-US"}};
        Theme::Fonts fonts{ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault, ImGui::GetIO().FontDefault};
        ThemeContext theme{fonts};
        EditorSettingsSnapshot settings;
        EditorGuiContext context{engineEvents, editorEvents, localization, theme, settings};
        SourceDocumentSnapshot source;
        std::optional<Result<SourceEditingPublication>> outcome;
        ImGuiID applyLine{};
        unsigned submitted{};

        SourceToolsUi() {
            {
                std::ofstream file(directory.Path() / "code.cpp", std::ios::binary);
                file << "alpha alpha\r\n  line\r\n";
                REQUIRE(file.good());
            }
            const auto opened = documents.Open({.path = "code.cpp", .mode = SourceOpenMode::EmbeddedOnly});
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().sourceSnapshot);
            source = *opened.Value().sourceSnapshot;
            LocalizationError error;
            REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor/en-US.json",
                                                 &error));
            REQUIRE(localization.Prepare(LocaleTag{"en-US"}, &error));
            REQUIRE(localization.ActivatePrepared(&error));
        }

        void Frame() {
            ImGui::SetNextWindowPos({0, 0});
            ImGui::SetNextWindowSize({420, 620});
            ImGui::Begin("Source tools", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
            widget.Publish(source);
            if (outcome && outcome->HasValue())
                static_cast<void>(widget.Restore(outcome->Value()));
            const auto history = documents.Documents().History(source.Identity().instance);
            const auto availability = history.HasValue() ? history.Value() : SourceHistoryState{};
            auto intent = tools.Draw(widget, source, availability, outcome, context);
            const bool focus = !intent && tools.TakeFocusRequest();
            const auto available = ImGui::GetContentRegionAvail();
            if (auto typed = widget.Draw({available.x, available.y, 1, focus, !intent}))
                intent = std::move(typed);
            // Use the actual wrapped action's numeric ID: the toggle and apply button share localized visible copy.
            const int high = 0;
            const int low = 7;
            const auto first = ImHashData(&high, sizeof(high), ImGui::GetCurrentWindow()->IDStack.back());
            const auto second = ImHashData(&low, sizeof(low), first);
            const auto action = ImHashStr("navigate-apply", 0, second);
            applyLine = ImHashStr("Go to line", 0, action);
            ImGui::End();
            if (intent) {
                ++submitted;
                outcome = SubmitSourceEditingCommand(documents.Documents(), *intent);
                if (outcome->HasValue())
                    source = outcome->Value().snapshot;
            }
        }
    };

    /** @brief Clicks controls discovered by their real localized labels, including wrapped rows. */
    void Click(ImGuiTestContext &ui, const char *label) {
        const std::string path = std::string{"//**/"} + label;
        ui.ItemClick(path.c_str());
        ui.Yield(2);
    }

    /** @brief Tests search, selected replacement, history replay and a no-match query in the actual visible controls. */
    void SearchAndHistory(ImGuiTestContext *ctx, SourceToolsUi &fixture) {
        Click(*ctx, "workspace.source_edit.find/##icon-button");
        ctx->ItemInputValue("//**/##source-find-query", "alpha");
        Click(*ctx, "Next match");
        IM_CHECK_EQ(fixture.widget.Selection().anchor, 0U);
        IM_CHECK_EQ(fixture.widget.Selection().caret, 5U);
        ctx->ItemInputValue("//**/##source-replacement", "beta");
        Click(*ctx, "Replace");
        IM_CHECK(fixture.source.Text() == "beta alpha\r\n  line\r\n");
        Click(*ctx, "workspace.source_edit.undo/##icon-button");
        IM_CHECK(fixture.source.Text() == "alpha alpha\r\n  line\r\n");
        Click(*ctx, "workspace.source_edit.redo/##icon-button");
        IM_CHECK(fixture.source.Text() == "beta alpha\r\n  line\r\n");
        ctx->ItemInputValue("//**/##source-find-query", "missing");
        const auto revision = fixture.source.Revision();
        Click(*ctx, "Next match");
        IM_CHECK_EQ(fixture.source.Revision(), revision);
    }

    /** @brief Tests atomic Replace All, search close and line navigation without a document write. */
    void ReplaceAllAndNavigate(ImGuiTestContext *ctx, SourceToolsUi &fixture) {
        ctx->ItemInputValue("//**/##source-find-query", "alpha");
        ctx->ItemInputValue("//**/##source-replacement", "A");
        Click(*ctx, "Replace all");
        IM_CHECK(fixture.source.Text() == "beta A\r\n  line\r\n");
        Click(*ctx, "Close search");
        Click(*ctx, "workspace.source_edit.go_to/##icon-button");
        ctx->ItemInputValue("//**/##source-line-number", "2");
        const auto revision = fixture.source.Revision();
        ctx->ItemClick(fixture.applyLine);
        ctx->Yield(2);
        IM_CHECK_EQ(fixture.source.Revision(), revision);
        IM_CHECK_EQ(fixture.widget.Selection().anchor, 8U);
        IM_CHECK_EQ(fixture.widget.Selection().caret, 8U);
    }
}  // namespace

TEST_CASE("Visible source tools edit search history and navigate through the canonical document owner", "[ui][imgui][editor][source]") {
    SourceToolsUi fixture;
    const auto result = fixture.harness.Run("horo_source", "editing_controls", [&](ImGuiTestContext *) {
        fixture.Frame();
    }, [&](ImGuiTestContext *ui) {
        ui->Yield(3);
        SearchAndHistory(ui, fixture);
        ReplaceAllAndNavigate(ui, fixture);
    });
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
    REQUIRE(fixture.outcome);
    CHECK(fixture.outcome->HasValue());
    CHECK(fixture.submitted == 4);
    CHECK(fixture.source.Text() == "beta A\r\n  line\r\n");
}

TEST_CASE("Focused source editing hands primary clicks to global dock tabs", "[ui][imgui][editor][source][focus]") {
    SourceToolsUi fixture;
    Horo::Editor::GlobalDockPanel dock;
    Horo::Editor::EditorWorkspaceViewModel model;
    const auto result = fixture.harness.Run("horo_source", "dock_pointer_handoff", [&](ImGuiTestContext *) {
        fixture.Frame();
        ImGui::SetNextWindowPos({0, 625});
        ImGui::SetNextWindowSize({1100, 160});
        ImGui::Begin("SourceDockHandoff", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        Horo::Editor::EditorWorkspaceViewCommandData command;
        dock.DrawPanel(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), model, command, fixture.context);
        ImGui::End();
    }, [&](ImGuiTestContext *ui) {
        ui->Yield(3);
        ui->MouseMoveToPos({120, 300});
        ui->MouseClick();
        ui->Yield(2);
        IM_CHECK(fixture.widget.OwnsInput());
        ui->ItemClick("//SourceDockHandoff/Console");
        ui->Yield(2);
        IM_CHECK(dock.ActiveTab() == Horo::Editor::GlobalDockTab::Console);
        IM_CHECK(!fixture.widget.OwnsInput());
        ui->ItemClick("//SourceDockHandoff/Assets");
        ui->Yield(2);
        IM_CHECK(dock.ActiveTab() == Horo::Editor::GlobalDockTab::Assets);
    });
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}

TEST_CASE("Project browser reaches native gameplay files through the Sources root", "[ui][imgui][editor][source][browser]") {
    SourceToolsUi fixture;
    const auto locale = GENERATE("en-US", "tr-TR");
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
    REQUIRE(fixture.localization.Prepare(Horo::Editor::LocaleTag{locale}));
    REQUIRE(fixture.localization.ActivatePrepared());
    const auto assets = fixture.directory.Path() / "Assets/Scripts";
    const auto gameplay = fixture.directory.Path() / "source/gameplay";
    std::filesystem::create_directories(assets);
    std::filesystem::create_directories(gameplay);
    std::ofstream(gameplay / "GameModule.cpp") << "// module";
    std::ofstream(gameplay / "Slide.cpp") << "void slide() {}\n";
    std::ofstream(gameplay / "Slide.cpp.horo-source.lock");
    const float browserWidth = GENERATE(1100.0F, 420.0F);
    Horo::Editor::EditorWorkspaceViewModel model;
    model.projectRoot = fixture.directory.Path().string();
    model.contentBrowser = Horo::Editor::BuildContentBrowserDirectory(fixture.directory.Path(), {}, {});
    Horo::Editor::GlobalDockPanel dock;
    const auto result = fixture.harness.Run("horo_source", "native_source_browser", [&](ImGuiTestContext *) {
        fixture.Frame();
        ImGui::SetNextWindowPos({0, 625});
        ImGui::SetNextWindowSize({browserWidth, 160});
        ImGui::Begin("NativeSourceBrowser", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        Horo::Editor::EditorWorkspaceViewCommandData command;
        dock.DrawPanel(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail(), model, command, fixture.context);
        ImGui::End();
        if (command.command == Horo::Editor::EditorWorkspaceViewCommand::NavigateContentBrowser && command.stringPayload)
            model.contentBrowser = Horo::Editor::BuildContentBrowserDirectory(fixture.directory.Path(), *command.stringPayload, {});
        if (command.sourceOpenRequest) {
            const auto opened = fixture.documents.Open(*command.sourceOpenRequest);
            REQUIRE(opened.HasValue());
            REQUIRE(opened.Value().sourceSnapshot);
            fixture.source = *opened.Value().sourceSnapshot;
        }
    }, [&](ImGuiTestContext *ui) {
        ui->Yield(3);
        if (browserWidth >= 640) {
            ui->ItemClick("//**/ContentBrowserRoot/Combo###ContentBrowserRoot");
            ui->ItemClick("//**/###combo_option_1");
        } else {
            ui->ItemClick("//**/$$1/##location");
        }
        ui->Yield(2);
        IM_CHECK(model.contentBrowser.rootKind == Horo::Editor::ContentBrowserRootKind::NativeSources);
        ui->ItemDoubleClick("//**/##AssetCard");
        ui->Yield(2);
        IM_CHECK(model.contentBrowser.entries.size() == 2);
        ui->ItemDoubleClick("//**/$$1/##AssetCard");
        ui->Yield(2);
        IM_CHECK(fixture.source.Text() == "void slide() {}\n");
    });
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}

TEST_CASE("Operations rows show bounded live progress and usable text actions", "[ui][imgui][editor][operations]") {
    SourceToolsUi fixture;
    const auto locale = GENERATE("en-US", "tr-TR");
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
    REQUIRE(fixture.localization.Prepare(Horo::Editor::LocaleTag{locale}));
    REQUIRE(fixture.localization.ActivatePrepared());
    const float width = GENERATE(1100.0F, 420.0F);
    Horo::OperationStore store{8, 8};
    const auto failed = store.Begin({.kind = Horo::OperationKind::Build, .title = "Build gameplay module", .progress = 0.1F});
    REQUIRE(failed);
    REQUIRE(store.Update(*failed, {.state = Horo::OperationState::Failed, .message = "configure exited with code 1.", .progress = 0.1F}));
    Horo::Editor::GlobalDockOperationsPane pane;
    pane.Attach(&store, &store);
    bool detailsOpen = false;
    bool cancelled = false;
    bool runningAdded = false;
    float meterWidth = 0;
    ImGuiID rowsWindow{};
    const auto result = fixture.harness.Run("horo_operations", "row_layout", [&](ImGuiTestContext *) {
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({width, 350});
        ImGui::Begin("OpsLayout", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        pane.Draw(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, fixture.context);
        detailsOpen = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        float left = FLT_MAX;
        float right = -FLT_MAX;
        for (const auto *child : ImGui::GetCurrentWindow()->DC.ChildWindows) {
            if (std::string_view{child->Name}.find("OperationsRows") == std::string_view::npos)
                continue;
            rowsWindow = child->ID;
            const auto metrics = Horo::Editor::ResolveGlobalDockPaneMetrics();
            const float rowsTop = child->DC.CursorStartPos.y + metrics.tableHeaderHeight +
                                  (runningAdded ? metrics.tableRowHeight + ImGui::GetStyle().ItemSpacing.y : 0.0F);
            const float rowsBottom = rowsTop + metrics.tableRowHeight;
            for (const auto &vertex : child->DrawList->VtxBuffer) {
                if (vertex.col == Horo::Editor::Theme::U32(Horo::Editor::Theme::Accent()) && vertex.pos.y >= rowsTop &&
                    vertex.pos.y <= rowsBottom) {
                    left = std::min(left, vertex.pos.x);
                    right = std::max(right, vertex.pos.x);
                }
            }
        }
        meterWidth = right >= left ? right - left : 0;
        ImGui::End();
    }, [&](ImGuiTestContext *ui) {
        ui->Yield(3);
        IM_CHECK_EQ(meterWidth, 0.0F);
        ui->ScrollToX(rowsWindow, 10000.0F);
        ui->Yield(2);
        ui->ItemClick("//**/$$0/OperationAction");
        ui->Yield(2);
        IM_CHECK(detailsOpen);
        ui->KeyPress(ImGuiKey_Escape);
        ui->MouseMoveToPos({0, 0});
        ui->Yield(2);
        const auto running = store.Begin(
            {.kind = Horo::OperationKind::Cook, .title = "Cook assets", .progress = 0.5F, .cancellable = true, .requestCancel = [&] {
            cancelled = true;
        }});
        IM_CHECK(running.has_value());
        runningAdded = true;
        IM_CHECK(store.Update(*running, {.state = Horo::OperationState::Running, .message = "Cooking assets", .progress = 0.5F}));
        ui->Yield(3);
        IM_CHECK(meterWidth > 0);
        IM_CHECK_LE(meterWidth, 77.0F * Horo::Editor::Theme::GetActiveTokens().sizes.uiScale);
        ui->ItemClick("//**/$$1/OperationAction");
        ui->Yield(2);
        IM_CHECK(cancelled);
    });
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}

TEST_CASE("Network records filter and open source-fenced responsive details", "[ui][imgui][editor][network]") {
    SourceToolsUi fixture;
    const auto locale = GENERATE("en-US", "tr-TR");
    REQUIRE(fixture.localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                 (std::string{locale} + ".json")));
    REQUIRE(fixture.localization.Prepare(Horo::Editor::LocaleTag{locale}));
    REQUIRE(fixture.localization.ActivatePrepared());
    const float width = GENERATE(1100.0F, 520.0F);
    Horo::Application::NetworkDebuggerService service;
    REQUIRE(service.Begin(17, 6, true, Horo::Network::NetworkDiagnosticProvider::Deterministic));
    const auto connection = Horo::Network::ConnectionHandle::Create(0, 1).Value();
    REQUIRE(
        service.Producer().Observe(service.Producer().Source(),
                                   Horo::Network::NetworkConnectionRecord{.connection = connection,
                                                                          .event = Horo::Network::NetworkTransportEventKind::Connected}));
    REQUIRE(
        service.Producer().Observe(service.Producer().Source(),
                                   Horo::Network::NetworkConnectionRecord{.connection = connection,
                                                                          .event = Horo::Network::NetworkTransportEventKind::PacketReceived,
                                                                          .bytes = 4096}));
    REQUIRE(service.Publish(service.Producer().Source()));
    Horo::Editor::GlobalDockNetworkPane pane;
    pane.Attach(&service, &service);
    bool replaceSource = false;
    bool detailsVisible = false;
    ImVec2 listPosition{}, detailsPosition{};
    std::string visibleText;
    const auto result = fixture.harness.Run("horo_network", "record_details", [&](ImGuiTestContext *) {
        if (replaceSource) {
            REQUIRE(service.Begin(18, 7, true));
            replaceSource = false;
        }
        REQUIRE(service.Publish(service.Producer().Source()));
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize({width, 650});
        ImGui::Begin("NetworkLayout", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        ImGui::LogToBuffer();
        pane.Draw(ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x, fixture.context);
        visibleText = ImGui::GetCurrentContext()->LogBuffer.c_str();
        ImGui::LogFinish();
        detailsVisible = false;
        for (const auto *window : ImGui::GetCurrentContext()->Windows) {
            if (!window->Active)
                continue;
            if (std::string_view{window->Name}.find("##NetworkEvidence") != std::string_view::npos)
                listPosition = window->Pos;
            if (std::string_view{window->Name}.find("##NetworkDetails") != std::string_view::npos) {
                detailsVisible = true;
                detailsPosition = window->Pos;
            }
        }
        ImGui::End();
    }, [&](ImGuiTestContext *ui) {
        ui->Yield(3);
        IM_CHECK(!detailsVisible);
        ui->ItemClick("//**/connections/##chip");
        ui->Yield(2);
        const auto &received = fixture.localization.Get("editor", "workspace.global_dock.network.event.received");
        ui->ItemInputValue("//**/##NetworkSearch", received.c_str());
        ui->Yield(2);
        ui->ItemClick("//**/$$0/2");
        ui->Yield(3);
        IM_CHECK(detailsVisible);
        IM_CHECK(visibleText.find("4096") != std::string::npos);
        if (width >= 760)
            IM_CHECK(detailsPosition.x > listPosition.x);
        else
            IM_CHECK(detailsPosition.y > listPosition.y);
        ui->ItemClick("//**/NetworkCloseDetails");
        ui->Yield(2);
        IM_CHECK(!detailsVisible);
        ui->ItemClick("//**/$$0/2");
        ui->Yield(2);
        IM_CHECK(detailsVisible);
        replaceSource = true;
        ui->Yield(3);
        IM_CHECK(!detailsVisible);
        IM_CHECK(pane.Projection().snapshot.source.scene == 18);
    });
    INFO(result.testEngineLog);
    REQUIRE(result.Succeeded());
}
