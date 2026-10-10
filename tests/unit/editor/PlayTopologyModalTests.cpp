#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Foundation/DataBus.h"
#include "editor/modals/play_topology/PlayTopologyModal.h"

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

        void Draw() {
            ImGui::GetIO().DisplaySize = {320, 600};
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
