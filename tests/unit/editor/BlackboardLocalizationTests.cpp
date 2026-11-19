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
        "review_change",
        "review_removal",
        "review_retarget_destination",
        "review_retarget_none",
        "usage_retarget_candidates",
        "usage_retarget_reviewed_destination",
        "usage_retarget_blocked",
        "usage_preview_change",
        "usage_preview_removal",
        "usage_pending",
        "usage_stale",
        "usage_found",
        "usage_query_outputs",
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
        CHECK_FALSE(service.Get("editor", "workspace.content_browser.action.recent_ai").starts_with("[missing:"));
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
                              "create_failed",
                              "preview_run",
                              "preview_cancel",
                              "preview_select_scene_object",
                              "preview_running",
                              "preview_ready",
                              "preview_stale",
                              "preview_cancelled",
                              "preview_failed",
                              "preview_generator_only",
                              "preview_generated",
                              "preview_shown",
                              "preview_ranked",
                              "preview_invalid_coordinates",
                              "create_distance",
                              "create_radius",
                              "create_prefer_nearer",
                              "score_weight",
                              "center_context",
                              "context_querier",
                              "context_world_origin",
                              "context_target",
                              "preview_select_target",
                              "new_distance_radius",
                              "add_distance_test",
                              "remove_distance_test",
                              "remove_distance_test_help",
                              "preview_distance",
                              "preview_passed",
                              "preview_filtered",
                              "preview_contribution",
                              "edit_apply",
                              "edit_conflict",
                              "edit_refused"};
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
                              "edit_utility_option",
                              "edit_utility_priority",
                              "edit_utility_compensation",
                              "edit_utility_apply_option",
                              "edit_utility_delete_option",
                              "edit_utility_new_score",
                              "edit_utility_score",
                              "edit_utility_delete_score",
                              "edit_utility_weight",
                              "edit_utility_parameter",
                              "edit_utility_linear",
                              "edit_utility_power",
                              "edit_utility_logistic",
                              "edit_utility_step",
                              "edit_utility_curve",
                              "edit_utility_invert",
                              "edit_utility_apply_score",
                              "edit_utility_add_score",
                              "edit_utility_add_option",
                              "subtree_cycle",
                              "subtree_missing",
                              "subtree_stale",
                              "select_subtree",
                              "review_subtree",
                              "subtree_reviewed",
                              "append_subtree",
                              "retarget_subtree",
                              "subtree_bound",
                              "edit_tree_node",
                              "edit_tree_order",
                              "edit_tree_wrap_sequence",
                              "edit_tree_wrap_selector",
                              "edit_tree_unwrap",
                              "edit_tree_move_earlier",
                              "edit_tree_move_later",
                              "edit_tree_delete",
                              "edit_tree_append_task",
                              "edit_conflict",
                              "edit_refused",
                              "edit_save",
                              "edit_undo",
                              "edit_redo",
                              "edit_add_state",
                              "edit_terminal_connection_hint",
                              "edit_state",
                              "edit_running",
                              "edit_target",
                              "edit_connect",
                              "edit_remove_state",
                              "edit_transition",
                              "transition_priority",
                              "transition_apply_priority",
                              "transition_order",
                              "transition_precedence",
                              "edit_remove_transition",
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
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.root").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.sequence").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.selector").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.composite").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.decorator").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.service").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.task").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.unknown").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.provider_catalog_unavailable").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.provider_missing").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.provider_version_mismatch").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.layout_unavailable").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.auto_layout").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.subtree").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.locate_selection").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.fit").starts_with("[missing:"));
        CHECK_FALSE(service.Get("editor", "workspace.decision_canvas.utility_order").starts_with("[missing:"));
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
