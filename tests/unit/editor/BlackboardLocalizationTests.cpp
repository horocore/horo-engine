#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/Localization/LocalizedGui.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>

TEST_CASE("Blackboard workspace controls resolve in both shipped locales", "[unit][editor][blackboard][localization]") {
    using namespace Horo::Editor;
    constexpr GuiLocalizationScope text{"workspace.blackboard"};
    constexpr std::array Keys{
        "limit",
        "invalid",
        "save",
        "reload",
        "undo",
        "redo",
        "conflict",
        "operation_failed",
        "add",
        "no_keys",
        "keys",
        "name",
        "apply_name",
        "move_up",
        "move_down",
        "remove",
        "reference_required",
        "type",
        "bool",
        "integer",
        "scalar",
        "entity",
        "asset",
        "coordinate",
        "shape",
        "single",
        "collection",
        "access",
        "read_only",
        "read_write",
        "presence",
        "required",
        "optional",
        "default_set",
        "clear_default",
        "default_value",
        "default_none",
        "value_false",
        "value_true",
        "apply_default",
        "default_invalid",
        "collection_capacity",
        "usage_review",
        "usage_pending",
        "usage_stale",
        "usage_found",
        "usage_none",
        "usage_incomplete",
        "usage_registry",
        "usage_decision_source",
        "usage_decision_provider",
        "usage_scene",
        "usage_prefab",
        "usage_queries",
        "usage_perception",
        "usage_previous",
        "usage_next",
        "usage_schema_bindings",
        "create_title",
        "create_schema_name",
        "create_key_name",
        "create_required_default",
        "create_confirm",
        "create_failed",
    };
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    const std::array locales{"en-US", "tr-TR"};
    LocalizationService service{{"en-US"}};
    for (const auto locale : locales)
        REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json")));
    for (const auto locale : locales) {
        REQUIRE(service.Prepare({locale}));
        REQUIRE(service.ActivatePrepared());
        CHECK_FALSE(service.Get("editor", "workspace.content_browser.action.create_blackboard").starts_with("[missing:"));
        for (const auto key : Keys) {
            INFO(locale);
            INFO(key);
            CHECK_FALSE(text.Text(service, key).empty());
            CHECK_FALSE(text.Text(service, key).starts_with("[missing:"));
        }
    }
}

TEST_CASE("Environment Query workspace labels resolve in both shipped locales", "[unit][editor][eqs][localization]") {
    using namespace Horo::Editor;
    constexpr GuiLocalizationScope text{"workspace.environment_query"};
    constexpr std::array keys{"invalid",
                              "limit",
                              "copy_failed",
                              "read_only",
                              "generator",
                              "test",
                              "unknown",
                              "descriptors_valid",
                              "descriptors_incompatible",
                              "descriptors_unavailable",
                              "create_title",
                              "create_name",
                              "create_step",
                              "create_outer",
                              "create_annulus",
                              "create_inner",
                              "create_limit",
                              "create_confirm",
                              "create_failed"};
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    const std::array locales{"en-US", "tr-TR"};
    LocalizationService service{{"en-US"}};
    for (const auto locale : locales)
        REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json")));
    for (const auto locale : locales) {
        REQUIRE(service.Prepare({locale}));
        REQUIRE(service.ActivatePrepared());
        CHECK_FALSE(service.Get("editor", "workspace.content_browser.action.create_environment_query").starts_with("[missing:"));
        for (const auto key : keys) {
            INFO(locale);
            INFO(key);
            CHECK_FALSE(text.Text(service, key).empty());
            CHECK_FALSE(text.Text(service, key).starts_with("[missing:"));
        }
    }
}

TEST_CASE("Decision graph workspace and initial state machine labels resolve in both shipped locales",
          "[unit][editor][decision][localization]") {
    using namespace Horo::Editor;
    constexpr GuiLocalizationScope text{"workspace.decision_graph"};
    constexpr std::array keys{"behavior_tree",
                              "state_machine",
                              "utility",
                              "read_only",
                              "nodes",
                              "unknown",
                              "invalid",
                              "limit",
                              "create_title",
                              "create_name",
                              "create_schema",
                              "create_select_schema",
                              "create_outcome",
                              "succeeded",
                              "failed",
                              "create_confirm",
                              "create_failed",
                              "new_tree",
                              "new_utility",
                              "create_task",
                              "select_task",
                              "open_scoring_schema",
                              "scoring_key",
                              "select_scoring_key",
                              "score_minimum",
                              "score_maximum",
                              "create_task_graph",
                              "catalog_changed"};
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    const std::array locales{"en-US", "tr-TR"};
    LocalizationService service{{"en-US"}};
    for (const auto locale : locales)
        REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json")));
    for (const auto locale : locales) {
        REQUIRE(service.Prepare({locale}));
        REQUIRE(service.ActivatePrepared());
        CHECK_FALSE(service.Get("editor", "workspace.content_browser.action.create_behavior_tree").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.content_browser.action.create_utility").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.content_browser.action.create_state_machine").starts_with("[missing:"));
        for (const auto key : keys) {
            INFO(locale);
            INFO(key);
            CHECK_FALSE(text.Text(service, key).empty());
            CHECK_FALSE(text.Text(service, key).starts_with("[missing:"));
        }
    }
}
