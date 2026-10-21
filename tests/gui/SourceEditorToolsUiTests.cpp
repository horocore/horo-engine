#include "EditorUiTestHarness.h"
#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/SourceFileOpenService.h"
#include "Horo/Foundation/DataBus.h"
#include "OwnedTestDirectory.h"
#include "editor/source_editor/SourceEditorTools.h"

#include <catch2/catch_test_macros.hpp>
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
        Click(*ctx, "Find");
        ctx->ItemInputValue("//**/##source-find-query", "alpha");
        Click(*ctx, "Next match");
        IM_CHECK_EQ(fixture.widget.Selection().anchor, 0U);
        IM_CHECK_EQ(fixture.widget.Selection().caret, 5U);
        ctx->ItemInputValue("//**/##source-replacement", "beta");
        Click(*ctx, "Replace");
        IM_CHECK(fixture.source.Text() == "beta alpha\r\n  line\r\n");
        Click(*ctx, "Undo");
        IM_CHECK(fixture.source.Text() == "alpha alpha\r\n  line\r\n");
        Click(*ctx, "Redo");
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
        Click(*ctx, "Go to line");
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
