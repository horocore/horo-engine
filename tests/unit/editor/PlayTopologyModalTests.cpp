#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/modals/play_topology/PlayTopologyModal.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <fstream>
#include <imgui_internal.h>

using namespace Horo;
using namespace Horo::Editor;

namespace {
    struct Harness final {
        Harness()
            : localization(LocaleTag{"en-US"}), theme{fonts}, context{.engineEvents = engineEvents,
                                                                      .editorEvents = editorEvents,
                                                                      .localization = localization,
                                                                      .theme = theme,
                                                                      .settings = settings},
              modalHost(editorEvents, input),
              root(std::filesystem::temp_directory_path() /
                   ("horo-topology-modal-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            ImGui::CreateContext();
            auto &io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DeltaTime = 1.0F / 60;
            auto *font = io.Fonts->AddFontDefault();
            (void)io.Fonts->Build();
            fonts = {.sans = font, .sansCompact = font, .sansEmphasis = font};
            std::filesystem::create_directories(root);
            for (const auto *locale : {"en-US", "tr-TR"})
                REQUIRE(localization.LoadCatalogFile(std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor" /
                                                     (std::string{locale} + ".json")));
            REQUIRE(localization.Prepare(LocaleTag{"en-US"}));
            REQUIRE(localization.ActivatePrepared());
        }

        ~Harness() {
            (void)modalHost.RequestCloseAllForShutdown();
            ImGui::DestroyContext();
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        void Open() {
            auto store = std::make_unique<Application::PlayTopologyStore>(files, root / "project.json", root / "user.json");
            REQUIRE(modalHost.OpenRoot(std::make_unique<PlayTopologyModal>(context, std::move(store))).HasValue());
        }

        void Draw(const ImVec2 size = {320, 600}) {
            ImGui::GetIO().DisplaySize = size;
            ImGui::NewFrame();
            modalHost.Draw();
            ImGui::Render();
        }

        EngineDataBus engineEvents;
        EditorDataBus editorEvents;
        LocalizationService localization;
        Theme::Fonts fonts;
        ThemeContext theme;
        EditorSettingsSnapshot settings;
        EditorGuiContext context;
        Input::InputRouter input;
        EditorModalHost modalHost;
        NativeDurableFileSystem files;
        std::filesystem::path root;
    };
}  // namespace

TEST_CASE("Play profile modal supports narrow localized cancelled and shutdown lifecycles", "[editor][gui][network][play_topology]") {
    Harness harness;
    for (const auto *locale : {"en-US", "tr-TR"}) {
        REQUIRE(harness.localization.Prepare(LocaleTag{locale}));
        REQUIRE(harness.localization.ActivatePrepared());
        for (const auto *key : {"title", "invalid", "stale", "storage", "save_project", "save_user", "user_hint", "clients"})
            CHECK_FALSE(harness.localization.Get("editor", std::string{"workspace.play_topology."} + key).starts_with("[missing:"));
        harness.Open();
        harness.Draw();
        harness.Draw();
        bool found{};
        for (const auto *window : ImGui::GetCurrentContext()->Windows) {
            if (std::string_view{window->Name}.find("PlayTopologyProfiles") == std::string_view::npos)
                continue;
            INFO("locale: " << locale << ", window: " << window->Name);
            CHECK(window->Size.x <= 320);
            CHECK(window->ScrollMax.x == 0);
            found = true;
        }
        CHECK(found);
        REQUIRE(harness.modalHost.RequestClose(*harness.modalHost.TopModalId(), ModalCloseReason::Cancelled).HasValue());
        harness.modalHost.OnUpdate(0);
        CHECK_FALSE(std::filesystem::exists(harness.root / "project.json"));
        CHECK_FALSE(std::filesystem::exists(harness.root / "user.json"));
    }
}

TEST_CASE("Play profile modal displays storage failure with persistence disabled", "[editor][gui][network][play_topology]") {
    Harness harness;
    {
        std::ofstream file(harness.root / "project.json");
        file << "{}";
    }
    harness.Open();
    harness.Draw();
    harness.Draw();
    CHECK(std::filesystem::file_size(harness.root / "project.json") == 2);
    CHECK_FALSE(std::filesystem::exists(harness.root / "user.json"));
    REQUIRE(harness.modalHost.RequestCloseAllForShutdown().HasValue());
}

TEST_CASE("Play profile form uses padded responsive columns and compact footer actions", "[editor][gui][network][play_topology]") {
    for (const auto *locale : {"en-US", "tr-TR"}) {
        for (const float width : {320.0F, 900.0F}) {
            Harness harness;
            REQUIRE(harness.localization.Prepare(LocaleTag{locale}));
            REQUIRE(harness.localization.ActivatePrepared());
            harness.Open();
            harness.Draw({width, 700});
            harness.Draw({width, 700});
            ImGuiWindow *body{}, *footer{};
            for (auto *window : ImGui::GetCurrentContext()->Windows) {
                const std::string_view name{window->Name};
                if (name.find("PlayTopologyProfiles") == std::string_view::npos)
                    continue;
                if (name.find("##TopologyBody") != std::string_view::npos)
                    body = window;
                if (name.find("##ModalFooter") != std::string_view::npos)
                    footer = window;
                CHECK(window->ScrollMax.x == 0);
            }
            REQUIRE(body);
            REQUIRE(footer);
            CHECK(body->WindowPadding.x >= 20);
            CHECK(body->WindowPadding.y >= 14);
            const auto *table = ImGui::GetCurrentContext()->Tables.GetByKey(body->GetID("##TopologyIdentity"));
            if (width == 900) {
                REQUIRE(table);
                REQUIRE(table->ColumnsCount == 2);
                CHECK(table->Columns[0].WidthGiven == Catch::Approx(table->Columns[1].WidthGiven).margin(1));
                float leftLabelY = 10000, rightLabelY = 10000;
                for (const auto &vertex : body->DrawList->VtxBuffer) {
                    if (vertex.col != Theme::U32(Theme::Dim()))
                        continue;
                    if (vertex.pos.x >= table->Columns[0].WorkMinX && vertex.pos.x < table->Columns[0].WorkMaxX)
                        leftLabelY = std::min(leftLabelY, vertex.pos.y);
                    if (vertex.pos.x >= table->Columns[1].WorkMinX && vertex.pos.x < table->Columns[1].WorkMaxX)
                        rightLabelY = std::min(rightLabelY, vertex.pos.y);
                }
                REQUIRE(leftLabelY < 10000);
                REQUIRE(rightLabelY < 10000);
                CHECK(leftLabelY == Catch::Approx(rightLabelY).margin(1));
            } else {
                CHECK(table == nullptr);
            }
            ImRect closeBounds{{10000, 10000}, {-10000, -10000}};
            for (const auto &vertex : footer->DrawList->VtxBuffer)
                if (vertex.col == Theme::U32(Theme::Bg3()))
                    closeBounds.Add(vertex.pos);
            REQUIRE(closeBounds.GetWidth() > 0);
            CHECK(closeBounds.GetWidth() < footer->Size.x * 0.5F);
            CHECK(closeBounds.Min.x >= footer->Pos.x + 20);
            CHECK_FALSE(std::filesystem::exists(harness.root / "project.json"));
            CHECK_FALSE(std::filesystem::exists(harness.root / "user.json"));
        }
    }
}
