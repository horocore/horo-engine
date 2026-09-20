#include "Horo/Editor/Localization/LocalizationService.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace {
    Horo::Editor::LocalizationCatalog Catalog(const char *locale, const char *text) {
        Horo::Editor::LocalizationCatalog catalog{.locale = Horo::Editor::LocaleTag{locale}};
        catalog.messages.emplace(Horo::Editor::MessageKey{"editor", "settings.title"}, text);
        return catalog;
    }

    constexpr std::array globalDockKeys{
        "workspace.global_dock.tab.assets",
        "workspace.global_dock.tab.console",
        "workspace.global_dock.tab.mcp",
        "workspace.global_dock.tab.performance",
        "workspace.global_dock.tab.physics",
        "workspace.global_dock.tab.audio",
        "workspace.global_dock.tab.network",
        "workspace.global_dock.tab.localization",
        "workspace.global_dock.preview.mcp",
        "workspace.global_dock.preview.performance",
        "workspace.global_dock.preview.physics",
        "workspace.global_dock.preview.audio",
        "workspace.global_dock.preview.network",
        "workspace.global_dock.preview.localization",
        "workspace.global_dock.console.filter.error",
        "workspace.global_dock.console.filter.warn",
        "workspace.global_dock.console.filter.info",
        "workspace.global_dock.console.filter.debug",
        "workspace.global_dock.console.filter.trace",
        "workspace.global_dock.console.filter.all",
        "workspace.global_dock.console.search",
        "workspace.global_dock.console.clear",
        "workspace.global_dock.console.source.all",
        "workspace.global_dock.console.collapse",
        "workspace.global_dock.console.auto_scroll",
        "workspace.global_dock.console.column.time",
        "workspace.global_dock.console.column.level",
        "workspace.global_dock.console.column.source",
        "workspace.global_dock.console.column.message",
        "workspace.global_dock.console.footer.logs",
        "workspace.global_dock.console.footer.info",
        "workspace.global_dock.console.footer.warning",
        "workspace.global_dock.console.footer.error",
        "workspace.global_dock.console.footer.on",
        "workspace.global_dock.console.footer.off",
        "workspace.global_dock.console.row.info",
        "workspace.global_dock.console.row.warning",
        "workspace.global_dock.console.row.error",
        "workspace.global_dock.console.empty",
        "workspace.global_dock.mcp.bridge",
        "workspace.global_dock.mcp.tools",
        "workspace.global_dock.mcp.awaiting",
        "workspace.global_dock.performance.gpu",
        "workspace.global_dock.performance.cpu",
        "workspace.global_dock.performance.memory",
        "workspace.global_dock.physics.solver",
        "workspace.global_dock.physics.layers",
        "workspace.global_dock.physics.memory",
        "workspace.global_dock.audio.master",
        "workspace.global_dock.audio.busses",
        "workspace.global_dock.audio.device",
        "workspace.global_dock.network.ping",
        "workspace.global_dock.network.replication",
        "workspace.global_dock.network.connection",
        "workspace.global_dock.localization.locale",
        "workspace.global_dock.localization.strings",
        "workspace.global_dock.localization.fonts",
    };
    constexpr std::array contentBrowserKeys{
        "workspace.content_browser.loading",   "workspace.content_browser.unavailable",      "workspace.content_browser.no_results",
        "workspace.content_browser.search",    "workspace.content_browser.filter.all_types", "workspace.content_browser.sort.name",
        "workspace.content_browser.sort.type",
    };
    constexpr std::array recoveryKeys{
        "workspace.recovery.available",
        "workspace.recovery.restore",
        "workspace.recovery.discard",
    };
    constexpr std::array projectLoadingKeys{
        "project_loading.status.loading_scene",
        "project_loading.error.scene",
    };
    constexpr std::array sceneConflictKeys{
        "workspace.scene_conflict.available",      "workspace.scene_conflict.compare",
        "workspace.scene_conflict.reload",         "workspace.scene_conflict.overwrite",
        "workspace.scene_compare.title",           "workspace.scene_compare.description",
        "workspace.scene_compare.loading",         "workspace.scene_compare.failed",
        "workspace.scene_compare.added",           "workspace.scene_compare.removed",
        "workspace.scene_compare.modified",        "workspace.scene_compare.summary.added",
        "workspace.scene_compare.summary.removed", "workspace.scene_compare.summary.modified",
        "workspace.scene_compare.field.name",      "workspace.scene_compare.field.id",
        "workspace.scene_compare.field.parent",    "workspace.scene_compare.field.transform",
        "workspace.scene_compare.field.primitive", "workspace.scene_compare.field.components",
        "workspace.scene_compare.empty",
    };

    template <typename Range> void RequireCatalogKeys(const Horo::Editor::LocalizationService &service, const Range &keys) {
        for (const char *key : keys)
            REQUIRE((!service.Get("editor", key).starts_with("[missing:")));
    }

    void ActivatePreparedCatalog(Horo::Editor::LocalizationService &service, const char *locale) {
        REQUIRE((service.Prepare(Horo::Editor::LocaleTag{locale})));
        REQUIRE((service.ActivatePrepared()));
    }

    void RequireSettingsCatalogKeys(const Horo::Editor::LocalizationService &service) {
        REQUIRE_FALSE(service.Get("editor", "settings.nav.packages").starts_with("[missing:"));
        REQUIRE_FALSE(service.Get("editor", "settings.packages.download_threads").starts_with("[missing:"));
        REQUIRE(service.Get("editor", "settings.network.download_threads").starts_with("[missing:"));
        RequireCatalogKeys(service, globalDockKeys);
        RequireCatalogKeys(service, contentBrowserKeys);
        RequireCatalogKeys(service, recoveryKeys);
        RequireCatalogKeys(service, projectLoadingKeys);
        RequireCatalogKeys(service, sceneConflictKeys);
    }

    TEST_CASE("Locale Tags Normalize And Reject Invalid Input", "[unit][editor]") {
        const auto normalized = Horo::Editor::LocaleTag::Parse("tr-TR");
        REQUIRE((normalized.has_value()));
        REQUIRE((normalized->value == "tr-TR"));
        REQUIRE((!Horo::Editor::LocaleTag::Parse("not a locale").has_value()));
    }

    TEST_CASE("Locale Switch Uses Immutable Prepared Snapshot", "[unit][editor]") {
        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        REQUIRE((service.RegisterCatalog(Catalog("en-US", "Settings"))));
        REQUIRE((service.RegisterCatalog(Catalog("tr-TR", "Ayarlar"))));
        REQUIRE((service.Prepare(Horo::Editor::LocaleTag{"en-US"})));
        REQUIRE((service.ActivatePrepared()));

        REQUIRE((service.Get("editor", "settings.title") == "Settings"));
        const auto before = service.Snapshot();

        REQUIRE((service.Prepare(Horo::Editor::LocaleTag{"tr-TR"})));
        REQUIRE((service.Get("editor", "settings.title") == "Settings"));
        REQUIRE((service.ActivatePrepared()));
        REQUIRE((service.Get("editor", "settings.title") == "Ayarlar"));
        REQUIRE((before.revision != service.Snapshot().revision));
    }

    TEST_CASE("Failed Preparation Leaves Active Locale Unchanged", "[unit][editor]") {
        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        REQUIRE((service.RegisterCatalog(Catalog("en-US", "Settings"))));
        ActivatePreparedCatalog(service, "en-US");

        Horo::Editor::LocalizationError error;
        REQUIRE((!service.Prepare(Horo::Editor::LocaleTag{"de-DE"}, &error)));
        REQUIRE((error.code == "editor.localization.catalog_missing"));
        REQUIRE((service.ActiveLocale().value == "en-US"));
    }

    TEST_CASE("Missing Message Does Not Use Source Fallback", "[unit][editor]") {
        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        REQUIRE((service.RegisterCatalog(Catalog("en-US", "Settings"))));
        ActivatePreparedCatalog(service, "en-US");

        REQUIRE((service.Get("editor", "missing") == "[missing:editor:missing]"));
    }

    TEST_CASE("Catalog File Loader Parses Resource Format", "[unit][editor]") {
        const auto path = std::filesystem::temp_directory_path() / "horo-localization-test.json";
        {
            std::ofstream output(path);
            output
                << R"({"schemaVersion":1,"messageFormat":"plain-text-1","locale":"tr-TR","namespace":"editor","messages":{"settings.title":{"text":"Ayarlar"}}})";
        }

        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        Horo::Editor::LocalizationError error;
        REQUIRE((service.LoadCatalogFile(path, &error)));
        REQUIRE((service.Prepare(Horo::Editor::LocaleTag{"tr-TR"}, &error)));
        REQUIRE((service.ActivatePrepared(&error)));
        REQUIRE((service.Get("editor", "settings.title") == "Ayarlar"));
        std::filesystem::remove(path);
    }

    TEST_CASE("Asset Localization Catalogs Contain Required Editor Messages", "[unit][editor]") {
        const std::filesystem::path enPath = "assets/localization/editor/en-US.json";
        const std::filesystem::path trPath = "assets/localization/editor/tr-TR.json";
        if (!std::filesystem::exists(enPath) || !std::filesystem::exists(trPath))
            return;

        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        Horo::Editor::LocalizationError error;
        REQUIRE((service.LoadCatalogFile(enPath, &error)));
        REQUIRE((service.LoadCatalogFile(trPath, &error)));

        REQUIRE((service.Prepare(Horo::Editor::LocaleTag{"en-US"}, &error)));
        REQUIRE((service.ActivatePrepared(&error)));
        REQUIRE((service.Get("editor", "settings.input.shortcut.click_to_record") == "Click to record"));
        REQUIRE((service.Get("editor", "settings.input.shortcut.press_keys") == "Press keys..."));
        REQUIRE((service.Get("editor", "workspace.content_browser.embedded") == "EMBEDDED"));
        REQUIRE((service.Get("editor", "workspace.content_browser.project_asset_dock") == "Project asset dock"));
        REQUIRE((service.Get("editor", "workspace.content_browser.empty") == "This folder is empty."));
        REQUIRE((service.Get("editor", "workspace.global_dock.tab.assets") == "Assets"));
        REQUIRE((service.Get("editor", "workspace.global_dock.tab.localization") == "L10n"));
        REQUIRE((service.Get("editor", "workspace.game_asset.category.missing") == "Missing Gameplay Asset Type"));
        RequireSettingsCatalogKeys(service);

        REQUIRE((service.Prepare(Horo::Editor::LocaleTag{"tr-TR"}, &error)));
        REQUIRE((service.ActivatePrepared(&error)));
        REQUIRE((service.Get("editor", "settings.input.shortcut.click_to_record") == "Kaydetmek için tıkla"));
        REQUIRE((service.Get("editor", "settings.input.shortcut.press_keys") == "Tuşlara basın..."));
        REQUIRE((service.Get("editor", "workspace.content_browser.embedded") == "YERLEŞİK"));
        REQUIRE((service.Get("editor", "workspace.content_browser.project_asset_dock") == "Proje varlık paneli"));
        REQUIRE((service.Get("editor", "workspace.content_browser.empty") == "Bu klasör boş."));
        REQUIRE((service.Get("editor", "workspace.global_dock.tab.assets") == "Varlıklar"));
        REQUIRE((service.Get("editor", "workspace.global_dock.tab.localization") == "L10n"));
        REQUIRE((service.Get("editor", "workspace.game_asset.category.missing") == "Eksik Oynanış Asset Türü"));
        RequireSettingsCatalogKeys(service);
    }
}  // namespace
