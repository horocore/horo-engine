#include "Horo/Editor/Localization/LocalizationService.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

namespace {
    constexpr std::array PhysicsKeys{
        "workspace.physics.title",
        "workspace.physics.authoring_only",
        "workspace.physics.unavailable",
        "workspace.physics.body",
        "workspace.physics.collider",
        "workspace.physics.constraint",
        "workspace.physics.enabled",
        "workspace.physics.sensor",
        "workspace.physics.static",
        "workspace.physics.kinematic",
        "workspace.physics.dynamic",
        "workspace.physics.motion",
        "workspace.physics.mass",
        "workspace.physics.density",
        "workspace.physics.mass_mode",
        "workspace.physics.linear_velocity",
        "workspace.physics.angular_velocity",
        "workspace.physics.linear_damping",
        "workspace.physics.angular_damping",
        "workspace.physics.linear_limit",
        "workspace.physics.angular_limit",
        "workspace.physics.translation",
        "workspace.physics.rotation_xyz",
        "workspace.physics.rotation_w",
        "workspace.physics.body_object",
        "workspace.physics.body_slot",
        "workspace.physics.profile",
        "workspace.physics.shape",
        "workspace.physics.box",
        "workspace.physics.sphere",
        "workspace.physics.capsule",
        "workspace.physics.plane",
        "workspace.physics.shape_asset",
        "workspace.physics.shape_part",
        "workspace.physics.half_extents",
        "workspace.physics.radius",
        "workspace.physics.cylinder_half_height",
        "workspace.physics.plane_normal",
        "workspace.physics.plane_distance",
        "workspace.physics.scale",
        "workspace.physics.material_slot",
        "workspace.physics.material_asset",
        "workspace.physics.remove_material",
        "workspace.physics.add_material",
        "workspace.physics.first_endpoint",
        "workspace.physics.second_endpoint",
        "workspace.physics.world",
        "workspace.physics.fixed",
        "workspace.physics.distance",
        "workspace.physics.constraint_kind",
        "workspace.physics.minimum_distance",
        "workspace.physics.maximum_distance",
        "workspace.physics.constraint_unsupported",
        "workspace.physics.remove_body",
        "workspace.physics.remove_collider",
        "workspace.physics.remove_constraint",
        "workspace.physics.add_body",
        "workspace.physics.add_collider",
        "workspace.physics.add_constraint",
        "workspace.physics.apply",
        "workspace.physics.cancel",
        "workspace.physics.invalid_identity",
        "workspace.physics.rejected",
        "workspace.physics.component_id",
        "workspace.physics.collider_slot",
        "workspace.physics.constraint_slot",
        "workspace.physics.generation",
        "workspace.physics.schema",
        "workspace.physics.object_id",
    };
}

TEST_CASE("Physics Inspector messages exist in both supported editor locales", "[unit][editor][localization][physics]") {
    using namespace Horo::Editor;
    LocalizationService service{LocaleTag{"en-US"}};
    LocalizationError error;
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    REQUIRE(service.LoadCatalogFile(root / "en-US.json", &error));
    REQUIRE(service.LoadCatalogFile(root / "tr-TR.json", &error));
    for (const auto locale : {"en-US", "tr-TR"}) {
        REQUIRE(service.Prepare(LocaleTag{locale}, &error));
        REQUIRE(service.ActivatePrepared(&error));
        for (const char *key : PhysicsKeys) {
            CHECK_FALSE(service.Get("editor", key).empty());
            CHECK(service.Get("editor", key).find("workspace.physics.") == std::string::npos);
        }
    }
}
