#include "Horo/Editor/Localization/LocalizationService.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

TEST_CASE("World source authoring messages exist in both supported locales", "[unit][editor][localization][world_document]") {
    using namespace Horo::Editor;
    LocalizationService service{LocaleTag{"en-US"}};
    LocalizationError error;
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    REQUIRE(service.LoadCatalogFile(root / "en-US.json", &error));
    REQUIRE(service.LoadCatalogFile(root / "tr-TR.json", &error));
    constexpr std::array keys{"title",   "unavailable", "authoring_only", "sphere",    "box",    "frustum", "path",     "priority",
                              "target",  "unloaded",    "loaded",         "activated", "intent", "camera",  "gameplay", "network",
                              "preload", "color",       "apply",          "remove",    "cancel", "undo",    "redo",     "rejected"};
    for (const auto locale : {"en-US", "tr-TR"}) {
        REQUIRE(service.Prepare(LocaleTag{locale}, &error));
        REQUIRE(service.ActivatePrepared(&error));
        for (const char *suffix : keys) {
            const std::string key = std::string{"workspace.world_sources."} + suffix;
            CHECK_FALSE(service.Get("editor", key).empty());
            CHECK(service.Get("editor", key).find("workspace.world_sources.") == std::string::npos);
        }
    }
}
