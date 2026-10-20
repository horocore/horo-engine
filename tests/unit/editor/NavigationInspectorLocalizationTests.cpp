#include "Horo/Editor/Localization/LocalizationService.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

TEST_CASE("Navigation inspector copy is complete in both supported locales", "[unit][editor][navigation][localization]") {
    using namespace Horo::Editor;
    LocalizationService service{LocaleTag{"en-US"}};
    LocalizationError error;
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    REQUIRE(service.LoadCatalogFile(root / "en-US.json", &error));
    REQUIRE(service.LoadCatalogFile(root / "tr-TR.json", &error));
    constexpr std::array keys{"workspace.navigation_inspector.surface",
                              "workspace.navigation_inspector.region",
                              "workspace.navigation_inspector.modifier",
                              "workspace.navigation_inspector.link",
                              "workspace.navigation_inspector.agent",
                              "workspace.navigation_inspector.missing",
                              "workspace.navigation_inspector.degraded",
                              "workspace.navigation_inspector.center",
                              "workspace.navigation_inspector.extents",
                              "workspace.navigation_inspector.profiles",
                              "workspace.navigation_inspector.remove_missing",
                              "workspace.navigation_inspector.definition",
                              "workspace.navigation_inspector.scope",
                              "workspace.navigation_inspector.subtree",
                              "workspace.navigation_inspector.bounds",
                              "workspace.navigation_inspector.region_mode",
                              "workspace.navigation_inspector.include",
                              "workspace.navigation_inspector.exclude",
                              "workspace.navigation_inspector.sources",
                              "workspace.navigation_inspector.explicit",
                              "workspace.navigation_inspector.static",
                              "workspace.navigation_inspector.shape",
                              "workspace.navigation_inspector.box",
                              "workspace.navigation_inspector.cylinder",
                              "workspace.navigation_inspector.radius",
                              "workspace.navigation_inspector.half_height",
                              "workspace.navigation_inspector.operation",
                              "workspace.navigation_inspector.area",
                              "workspace.navigation_inspector.area_cost",
                              "workspace.navigation_inspector.cost",
                              "workspace.navigation_inspector.position",
                              "workspace.navigation_inspector.connection_radius",
                              "workspace.navigation_inspector.start",
                              "workspace.navigation_inspector.end",
                              "workspace.navigation_inspector.kind",
                              "workspace.navigation_inspector.jump",
                              "workspace.navigation_inspector.ladder",
                              "workspace.navigation_inspector.door",
                              "workspace.navigation_inspector.teleport",
                              "workspace.navigation_inspector.direction",
                              "workspace.navigation_inspector.forward",
                              "workspace.navigation_inspector.both",
                              "workspace.navigation_inspector.profile",
                              "workspace.navigation_inspector.filter",
                              "workspace.navigation_inspector.override_radius",
                              "workspace.navigation_inspector.enabled",
                              "workspace.navigation_inspector.invalid",
                              "workspace.navigation_inspector.apply",
                              "workspace.navigation_inspector.cancel",
                              "workspace.navigation_inspector.rejected"};
    for (const auto locale : {"en-US", "tr-TR"}) {
        REQUIRE(service.Prepare(LocaleTag{locale}, &error));
        REQUIRE(service.ActivatePrepared(&error));
        for (const auto *key : keys) {
            CHECK_FALSE(service.Get("editor", key).empty());
            CHECK(service.Get("editor", key) != key);
        }
    }
}
