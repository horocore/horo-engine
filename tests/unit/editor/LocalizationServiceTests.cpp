#include "../../support/OwnedTestDirectory.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/Localization/LocalizedGui.h"

#include <array>
#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>

namespace {
    Horo::Editor::LocalizationCatalog Catalog(const char *locale, const char *text) {
        Horo::Editor::LocalizationCatalog catalog{.locale = Horo::Editor::LocaleTag{locale}};
        catalog.messages.try_emplace(Horo::Editor::MessageKey{"editor", "settings.title"}, text);
        return catalog;
    }

    constexpr std::array globalDockKeys{
        "workspace.global_dock.platform.profile",
        "workspace.global_dock.platform.selection",
        "workspace.global_dock.platform.profile.editor",
        "workspace.global_dock.platform.profile.development",
        "workspace.global_dock.platform.profile.shipping",
        "workspace.global_dock.platform.profile.headless",
        "workspace.global_dock.platform.profile.server",
        "workspace.global_dock.platform.profile.test",
        "workspace.global_dock.platform.profile.unsupported",
        "workspace.global_dock.platform.mode.null",
        "workspace.global_dock.platform.mode.test",
        "workspace.global_dock.platform.mode.exact",
        "workspace.global_dock.platform.mode.absent",
        "workspace.global_dock.platform.service.achievements",
        "workspace.global_dock.platform.service.stats",
        "workspace.global_dock.platform.service.cloud",
        "workspace.global_dock.platform.service.presence",
        "workspace.global_dock.platform.service.friends",
        "workspace.global_dock.platform.service.session",
        "workspace.global_dock.platform.service.available",
        "workspace.global_dock.platform.service.unavailable",
        "workspace.global_dock.tab.platform",
        "workspace.global_dock.platform.title",
        "workspace.global_dock.platform.unavailable",
        "workspace.global_dock.platform.privacy",
        "workspace.global_dock.platform.active",
        "workspace.global_dock.platform.retained",
        "workspace.global_dock.platform.pending",
        "workspace.global_dock.platform.dispatching",
        "workspace.global_dock.platform.reconciling",
        "workspace.global_dock.platform.suspended",
        "workspace.global_dock.platform.succeeded",
        "workspace.global_dock.platform.failed",
        "workspace.global_dock.platform.expired",
        "workspace.global_dock.platform.superseded",
        "workspace.global_dock.platform.abandoned",
        "workspace.global_dock.platform.queued_notice",
        "workspace.global_dock.platform.suspend",
        "workspace.global_dock.platform.command_failed",
        "workspace.global_dock.tab.assets",
        "workspace.global_dock.tab.console",
        "workspace.global_dock.tab.mcp",
        "workspace.mcp_commands.open_debug_capture",
        "workspace.mcp_commands.open_scene_proposals",
        "workspace.viewport.agent_review.summary",
        "workspace.viewport.agent_review.position",
        "workspace.viewport.agent_review.rotation",
        "workspace.viewport.agent_review.scale",
        "workspace.viewport.agent_review.spatial_warning",
        "workspace.viewport.agent_review.rename",
        "workspace.viewport.agent_review.enable_light",
        "workspace.viewport.agent_review.disable_light",
        "workspace.viewport.agent_review.apply",
        "workspace.viewport.agent_review.discard",
        "workspace.viewport.agent_review.invalid",
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
        "workspace.global_dock.mcp.empty",
        "workspace.global_dock.mcp.copy",
        "workspace.global_dock.mcp.operation.finished",
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
        "workspace.content_browser.loading",
        "workspace.content_browser.unavailable",
        "workspace.content_browser.no_results",
        "workspace.content_browser.search",
        "workspace.content_browser.filter.all_types",
        "workspace.content_browser.sort.name",
        "workspace.content_browser.sort.type",
        "workspace.content_browser.asset.unregistered",
        "workspace.content_browser.prefab.type",
        "workspace.content_browser.preview.queued",
        "workspace.content_browser.preview.unavailable",
        "workspace.content_browser.prefab.migration_required",
    };
    constexpr std::array recoveryKeys{
        "workspace.paired_recovery.keep_description",
        "workspace.paired_recovery.keep",
        "workspace.paired_recovery.available",
        "workspace.paired_recovery.reopen",
        "workspace.paired_recovery.invalid",
        "workspace.paired_recovery.review",
        "workspace.paired_recovery.title",
        "workspace.paired_recovery.description",
        "workspace.paired_recovery.restore",
        "workspace.paired_recovery.cancel",
        "workspace.paired_recovery.blocked",
        "workspace.paired_recovery.failed",
        "workspace.recovery.available",
        "workspace.recovery.restore",
        "workspace.recovery.discard",
    };
    constexpr std::array prefabOverrideKeys{
        "workspace.prefab.diff.revert_value",    "workspace.prefab.diff.revert_component", "workspace.prefab.diff.revert_object",
        "workspace.prefab.diff.revert_instance", "workspace.prefab.diff.apply_value",      "workspace.prefab.diff.apply_component",
        "workspace.prefab.diff.apply_object",    "workspace.prefab.diff.apply_instance",
    };
    constexpr std::array projectLoadingKeys{
        "project_loading.status.loading_scene",      "project_loading.error.scene",
        "project_loading.error.platform_identity",   "project_loading.error.creation_durability",
        "project_loading.error.creation_unexpected", "project_loading.error.creation_publication",
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
        for (const char *key : keys) {
            INFO("Catalog key: " << key);
            REQUIRE((!service.Get("editor", key).starts_with("[missing:")));
        }
    }

    void ActivatePreparedCatalog(Horo::Editor::LocalizationService &service, const char *locale) {
        REQUIRE((service.Prepare(Horo::Editor::LocaleTag{locale})));
        REQUIRE((service.ActivatePrepared()));
    }

    /** @brief Covers the optional local XR listening controls in both shipped languages. */
    void RequireXrListeningCatalogKeys(const Horo::Editor::LocalizationService &service) {
        constexpr std::array keys{
            "workspace.local_agent.xr_listening_idle",
            "workspace.local_agent.xr_listening_limit",
            "workspace.local_agent.xr_listening_provider_timeout",
            "workspace.local_agent.xr_listening_update_limit",
            "workspace.local_agent.xr_listening_resources_unavailable",
            "workspace.local_agent.xr_listening_provider_unavailable",
            "workspace.local_agent.xr_listening_low_confidence",
            "workspace.local_agent.xr_listening_privacy",
            "workspace.local_agent.xr_listening_active",
            "workspace.local_agent.xr_listening_finishing",
            "workspace.local_agent.xr_listening_draft",
            "workspace.local_agent.xr_listening_speech_only",
            "workspace.local_agent.xr_listening_spatial_rejected",
            "workspace.local_agent.xr_listening_spatial",
            "workspace.local_agent.xr_listening_interrupted",
            "workspace.local_agent.xr_listening_microphone_permission",
            "workspace.local_agent.xr_listening_speech_permission",
            "workspace.local_agent.xr_listening_refresh",
            "workspace.local_agent.xr_listening_input",
            "workspace.local_agent.xr_listening_start",
            "workspace.local_agent.xr_listening_finish",
            "workspace.local_agent.xr_listening_cancel",
            "workspace.local_agent.xr_listening_discard",
        };
        RequireCatalogKeys(service, keys);
    }

    /** @brief Covers the localized read-only XR evidence reviews. */
    void RequireXrEvidenceCatalogKeys(const Horo::Editor::LocalizationService &service) {
        constexpr std::array keys{
            "workspace.local_agent.xr_reference_review",     "workspace.local_agent.xr_pointing_review",
            "workspace.local_agent.xr_location_review",      "workspace.local_agent.xr_location_coordinates",
            "workspace.local_agent.xr_surface_review",       "workspace.local_agent.xr_surface_empty",
            "workspace.local_agent.xr_surface_unavailable",  "workspace.local_agent.xr_surface_floor",
            "workspace.local_agent.xr_surface_ceiling",      "workspace.local_agent.xr_surface_wall",
            "workspace.local_agent.xr_surface_platform",     "workspace.local_agent.xr_surface_unknown",
            "workspace.local_agent.xr_contacts_review",      "workspace.local_agent.xr_contacts_unavailable",
            "workspace.local_agent.xr_contacts_tick",        "workspace.local_agent.xr_contacts_empty",
            "workspace.local_agent.xr_contacts_trigger",     "workspace.local_agent.xr_contacts_contact",
            "workspace.local_agent.xr_pointing_unavailable", "workspace.local_agent.xr_pointing_empty",
            "workspace.local_agent.xr_reference_stale",      "workspace.local_agent.xr_reference_revision",
            "workspace.local_agent.xr_reference_mounted",    "workspace.local_agent.xr_reference_empty",
            "workspace.local_agent.xr_reference_ambiguous",  "workspace.local_agent.xr_reference_available",
            "workspace.local_agent.xr_reference_hidden",     "workspace.local_agent.xr_reference_locked",
            "workspace.local_agent.xr_reference_deleted",    "workspace.local_agent.xr_reference_out_of_scope",
        };
        RequireCatalogKeys(service, keys);
    }

    /** @brief Requires the localized controls and close decisions belonging to persistent document tabs. */
    void RequireDocumentCatalogKeys(const Horo::Editor::LocalizationService &service) {
        constexpr std::array sourceEditingKeys{
            "workspace.source_edit.undo",
            "workspace.source_edit.redo",
            "workspace.source_edit.find",
            "workspace.source_edit.replace",
            "workspace.source_edit.replace_all",
            "workspace.source_edit.next",
            "workspace.source_edit.previous",
            "workspace.source_edit.go_to",
            "workspace.source_edit.query",
            "workspace.source_edit.replacement",
            "workspace.source_edit.line",
            "workspace.source_edit.no_match",
            "workspace.source_edit.invalid",
            "workspace.source_edit.history_evicted",
            "workspace.source_edit.history_oversized",
            "workspace.source_edit.close_search",
            "workspace.source_review.previous",
            "workspace.source_review.next",
            "workspace.source_review.conflict",
            "workspace.source_review.dismiss",
            "workspace.source_review.source_diagnostics",
            "workspace.source_review.applied_diagnostics",
            "workspace.source_review.errors",
            "workspace.source_review.warnings",
            "workspace.source_review.diagnostics_unavailable",
            "workspace.local_agent.source_patch_review",
            "workspace.local_agent.cloud_use",
            "workspace.local_agent.cloud_profile_working",
            "workspace.local_agent.cloud_profile_failed",
            "workspace.local_agent.cloud_model_save",
            "workspace.local_agent.cloud_model_saved",
            "workspace.local_agent.cloud_use_local",
            "workspace.local_agent.cloud_endpoint",
            "workspace.local_agent.cloud_key_enroll",
            "workspace.local_agent.cloud_key_revoke",
            "workspace.local_agent.cloud_key_saved",
            "workspace.local_agent.cloud_key_removed",
            "workspace.local_agent.cloud_key_failed",
            "workspace.local_agent.cloud_review",
            "workspace.local_agent.cloud_review_prompt",
            "workspace.local_agent.cloud_review_discovery",
            "workspace.local_agent.cloud_approve",
            "workspace.local_agent.cloud_discard",
            "workspace.local_agent.cloud_unavailable",
            "workspace.local_agent.cloud_auth_failed",
            "workspace.local_agent.cloud_rate_limited",
            "workspace.local_agent.cloud_timeout",
            "workspace.local_agent.cloud_working",
            "workspace.local_agent.source_patch_stale",
            "workspace.local_agent.source_patch_invalid",
            "workspace.local_agent.source_patch_unavailable",
            "workspace.local_agent.patch_file_create",
            "workspace.local_agent.patch_file_edit",
            "workspace.local_agent.patch_file_delete",
            "workspace.local_agent.patch_file_rename",
            "workspace.local_agent.patch_file_bytes",
            "workspace.local_agent.patch_file_ranges",
            "workspace.local_agent.patch_file_pending",
            "workspace.local_agent.patch_file_source_unavailable",
            "workspace.local_agent.patch_file_disk_changed",
            "workspace.local_agent.patch_file_dirty_delete",
            "workspace.local_agent.patch_file_case_only_rename",
            "workspace.local_agent.source_patch_destination",
            "workspace.local_agent.source_patch_destination_review",
            "workspace.local_agent.source_patch_create_preview",
            "workspace.local_agent.source_patch_check_syntax",
            "workspace.local_agent.source_patch_syntax_clean",
            "workspace.local_agent.source_patch_syntax_invalid",
            "workspace.local_agent.source_patch_syntax_unavailable",
            "workspace.local_agent.source_patch_check_cpp",
            "workspace.local_agent.source_patch_cpp_clean",
            "workspace.local_agent.source_patch_cpp_findings",
            "workspace.local_agent.source_patch_cpp_unavailable",
            "workspace.local_agent.source_patch_diagnostics_truncated",
            "workspace.local_agent.source_patch_create_excerpt",
            "workspace.local_agent.source_patch_structural_preview",
            "workspace.local_agent.source_patch_confirm_delete",
            "workspace.local_agent.source_patch_confirm_moves",
            "workspace.local_agent.source_patch_group_recovery",
            "workspace.local_agent.source_patch_cross_volume",
            "workspace.local_agent.source_patch_move_unsupported",
            "workspace.local_agent.source_patch_confirm_rename",
            "workspace.local_agent.source_patch_rename_complete",
            "workspace.local_agent.source_patch_rename_recovery",
            "workspace.local_agent.source_patch_delete_complete",
            "workspace.local_agent.source_patch_delete_recovery",
            "workspace.local_agent.patch_file_parent_missing",
            "workspace.local_agent.patch_file_redirect",
            "workspace.local_agent.patch_file_destination_alias",
            "workspace.local_agent.patch_file_destination_unavailable",
            "workspace.local_agent.patch_file_destination_occupied",
            "workspace.local_agent.patch_file_overlap",
            "workspace.local_agent.patch_file_rejected",
            "workspace.local_agent.patch_file_accepted",
            "workspace.local_agent.patch_file_mixed",
            "workspace.local_agent.source_patch_accept_file",
            "workspace.local_agent.source_patch_reject_file",
            "workspace.local_agent.source_patch_accept_all",
            "workspace.local_agent.source_patch_undo",
            "workspace.local_agent.source_patch_apply",
            "workspace.local_agent.source_patch_applied",
            "workspace.local_agent.source_patch_apply_failed",
            "workspace.local_agent.source_patch_revert",
            "workspace.local_agent.source_patch_reverted",
            "workspace.local_agent.source_patch_revert_failed",
            "workspace.local_agent.source_patch_transaction_conflict",
            "workspace.local_agent.source_patch_transaction_cancelled",
            "workspace.local_agent.source_patch_transaction_preparing",
            "workspace.local_agent.source_patch_audit_proposal",
            "workspace.local_agent.source_patch_audit_historical",
            "workspace.local_agent.source_patch_audit_applied",
            "workspace.local_agent.source_patch_audit_saved",
            "workspace.local_agent.source_patch_audit_reverted",
            "workspace.local_agent.source_patch_audit_stale",
            "workspace.local_agent.source_validation_save",
            "workspace.local_agent.source_validation_building",
            "workspace.local_agent.source_validation_build_passed",
            "workspace.local_agent.source_validation_build_failed",
            "workspace.local_agent.source_validation_testing",
            "workspace.local_agent.source_validation_tests_passed",
            "workspace.local_agent.source_validation_tests_failed",
            "workspace.local_agent.source_validation_stale",
            "workspace.local_agent.source_validation_approve_build",
            "workspace.local_agent.source_validation_approve_test",
            "workspace.local_agent.source_patch_audit_history_error",
            "workspace.local_agent.source_patch_hunks",
            "workspace.local_agent.source_patch_checkpoint_save",
            "workspace.local_agent.source_patch_checkpoint_load",
            "workspace.local_agent.source_patch_checkpoint_saved",
            "workspace.local_agent.source_patch_checkpoint_loaded",
            "workspace.local_agent.source_patch_checkpoint_missing",
            "workspace.local_agent.source_patch_checkpoint_stale",
            "workspace.local_agent.source_patch_checkpoint_error",
            "workspace.local_agent.source_patch_range",
            "workspace.local_agent.source_patch_offset",
            "workspace.local_agent.source_patch_previous_hunk",
            "workspace.local_agent.source_patch_next_hunk",
            "workspace.local_agent.source_patch_accept_hunk",
            "workspace.local_agent.source_patch_reject_hunk",
            "workspace.local_agent.source_patch_reject",
        };
        RequireCatalogKeys(service, sourceEditingKeys);
        constexpr std::array documentKeys{"workspace.documents.close",         "workspace.documents.move_left",
                                          "workspace.documents.move_right",    "workspace.documents.save",
                                          "workspace.documents.preview",       "workspace.documents.preview_limit",
                                          "workspace.documents.close_title",   "workspace.documents.close_prompt",
                                          "workspace.documents.close_failed",  "workspace.documents.save_close",
                                          "workspace.documents.discard_close", "workspace.documents.cancel"};
        RequireCatalogKeys(service, documentKeys);
    }

    /** @brief Verifies the package removal review controls and reference labels in each loaded catalog. */
    void RequireRemovalReviewKeys(const Horo::Editor::LocalizationService &service) {
        constexpr std::array keys{"workspace.packages.previous_references",
                                  "workspace.packages.next_references",
                                  "workspace.packages.reference_importer",
                                  "workspace.packages.reference_behavior",
                                  "workspace.packages.reference_component",
                                  "workspace.packages.reference_service",
                                  "workspace.packages.reference_decision",
                                  "workspace.packages.reference_perception",
                                  "workspace.packages.reference_unreadable",
                                  "workspace.packages.reference_saved_source_unreadable",
                                  "workspace.packages.reference_registration"};
        RequireCatalogKeys(service, keys);
    }

    void RequireSettingsCatalogKeys(const Horo::Editor::LocalizationService &service) {
        REQUIRE_FALSE(service.Get("editor", "settings.nav.packages").starts_with("[missing:"));
        REQUIRE_FALSE(service.Get("editor", "settings.nav.updates").starts_with("[missing:"));
        constexpr std::array updateKeys{"settings.updates.check",
                                        "settings.updates.channel",
                                        "settings.updates.download",
                                        "settings.updates.release_notes",
                                        "settings.updates.compatibility",
                                        "settings.updates.confirm.restart",
                                        "settings.updates.confirm.rollback",
                                        "settings.updates.phase.available",
                                        "settings.updates.phase.verifying",
                                        "settings.updates.phase.restart_required",
                                        "settings.updates.phase.failed",
                                        "settings.updates.phase.rolled_back"};
        RequireCatalogKeys(service, updateKeys);
        REQUIRE_FALSE(service.Get("editor", "settings.packages.download_threads").starts_with("[missing:"));
        REQUIRE(service.Get("editor", "settings.network.download_threads").starts_with("[missing:"));
        constexpr std::array packageKeys{"workspace.packages.button",
                                         "workspace.packages.title",
                                         "workspace.packages.policy_hint",
                                         "workspace.packages.plan_unavailable",
                                         "workspace.packages.verified",
                                         "workspace.packages.locked",
                                         "workspace.packages.semantic_unavailable",
                                         "workspace.packages.restore",
                                         "workspace.packages.install",
                                         "workspace.packages.package",
                                         "workspace.packages.required_by",
                                         "workspace.packages.preview_removal",
                                         "workspace.packages.removal_kept",
                                         "workspace.packages.removal_candidates",
                                         "workspace.packages.removal_native_in_use",
                                         "workspace.packages.removal_reference_check",
                                         "workspace.packages.removal_save_and_stop",
                                         "workspace.packages.confirm_removal",
                                         "workspace.packages.removal_committed",
                                         "workspace.packages.descriptor",
                                         "workspace.packages.activate",
                                         "workspace.packages.gameplay_module",
                                         "workspace.packages.activate_gameplay_module",
                                         "workspace.packages.disable",
                                         "workspace.packages.cancel",
                                         "workspace.packages.running",
                                         "workspace.packages.terminal",
                                         "workspace.packages.operation_failed",
                                         "workspace.packages.execution_trust_required",
                                         "workspace.packages.close",
                                         "workspace.packages.retry",
                                         "workspace.packages.recovery_hint"};
        RequireCatalogKeys(service, packageKeys);
        RequireCatalogKeys(service, globalDockKeys);
        RequireXrListeningCatalogKeys(service);
        RequireXrEvidenceCatalogKeys(service);
        RequireDocumentCatalogKeys(service);
        RequireCatalogKeys(service, contentBrowserKeys);
        RequireCatalogKeys(service, std::array{"workspace.source_language.unavailable",
                                               "workspace.source_language.definition",
                                               "workspace.source_language.waiting",
                                               "workspace.source_language.retry",
                                               "workspace.source_language.no_definition",
                                               "workspace.source_language.actions",
                                               "workspace.source_language.references",
                                               "workspace.source_language.choose_reference",
                                               "workspace.source_language.no_references",
                                               "workspace.source_language.document_symbols",
                                               "workspace.source_language.choose_document_symbol",
                                               "workspace.source_language.no_document_symbols",
                                               "workspace.source_language.code_actions",
                                               "workspace.source_language.choose_code_action",
                                               "workspace.source_language.no_code_actions",
                                               "workspace.source_language.code_action_before",
                                               "workspace.source_language.code_action_after",
                                               "workspace.source_language.apply_code_action",
                                               "workspace.source_language.dismiss_code_action",
                                               "workspace.source_language.complete",
                                               "workspace.source_language.choose_completion",
                                               "workspace.source_language.no_completion",
                                               "workspace.source_language.script_diagnostics",
                                               "workspace.source_language.unknown_script_member",
                                               "workspace.source_language.script_actions",
                                               "workspace.source_language.describe_symbol",
                                               "workspace.source_language.no_symbol",
                                               "workspace.source_language.script_module_unavailable"});
        RequireCatalogKeys(service, recoveryKeys);
        RequireCatalogKeys(service, prefabOverrideKeys);
        RequireCatalogKeys(service, projectLoadingKeys);
        RequireCatalogKeys(service, sceneConflictKeys);
        constexpr std::array xrSetupKeys{
            "workspace.xr_setup.title",
            "workspace.xr_setup.runtime_discovered",
            "workspace.xr_setup.runtime_missing",
            "workspace.xr_setup.renderer_ready",
            "workspace.xr_setup.renderer_missing",
            "workspace.xr_setup.profile_projection",
            "workspace.xr_setup.profile_tracked",
            "workspace.xr_setup.profile_unavailable",
            "workspace.xr_setup.test_not_run",
            "workspace.xr_setup.test_queued",
            "workspace.xr_setup.test_passed",
            "workspace.xr_setup.test_failed",
            "workspace.xr_setup.test_cancelled",
            "workspace.xr_setup.permissions",
            "workspace.xr_setup.busy",
            "workspace.xr_setup.unavailable",
            "workspace.xr_setup.renderer_incompatible",
            "workspace.xr_setup.unsupported",
            "workspace.xr_setup.permission_denied",
            "workspace.xr_setup.stale",
            "workspace.xr_setup.capacity",
            "workspace.xr_setup.invalid",
            "workspace.xr_setup.cleanup",
            "workspace.xr_setup.run_test",
            "workspace.xr_setup.haptic_left",
            "workspace.xr_setup.haptic_right",
            "workspace.xr_setup.haptic_queued",
            "workspace.xr_setup.haptic_submitted",
            "workspace.xr_setup.haptic_unsupported",
            "workspace.xr_setup.haptic_rejected",
            "workspace.xr_setup.haptic_cancelled",
            "workspace.xr_setup.planes_scope",
            "workspace.xr_setup.planes_allow",
            "workspace.xr_setup.planes_revoke",
            "workspace.xr_setup.planes_armed",
            "workspace.xr_setup.planes_waiting",
            "workspace.xr_setup.planes_tracking",
            "workspace.xr_setup.planes_rejected",
            "workspace.xr_setup.gaze_scope",
            "workspace.xr_setup.gaze_allow",
            "workspace.xr_setup.gaze_armed",
            "workspace.xr_setup.gaze_waiting",
            "workspace.xr_setup.gaze_tracking",
            "workspace.xr_setup.gaze_revoke",
            "workspace.xr_setup.gaze_rejected",
            "workspace.xr_setup.passthrough_scope",
            "workspace.xr_setup.passthrough_allow",
            "workspace.xr_setup.passthrough_revoke",
            "workspace.xr_setup.passthrough_armed",
            "workspace.xr_setup.passthrough_pending",
            "workspace.xr_setup.passthrough_active",
            "workspace.xr_setup.passthrough_denied",
            "workspace.xr_setup.passthrough_rejected",
            "workspace.xr_setup.inspect_title",
            "workspace.xr_setup.inspect_unavailable",
            "workspace.xr_setup.inspect_idle",
            "workspace.xr_setup.inspect_waiting",
            "workspace.xr_setup.inspect_unfocused",
            "workspace.xr_setup.inspect_events",
            "workspace.xr_setup.inspect_current",
            "workspace.xr_setup.inspect_stale",
            "workspace.xr_setup.inspect_redacted",
            "workspace.xr_setup.inspect_session",
            "workspace.xr_setup.inspect_session_inactive",
            "workspace.xr_setup.inspect_session_ready",
            "workspace.xr_setup.inspect_session_running",
            "workspace.xr_setup.inspect_session_visible",
            "workspace.xr_setup.inspect_session_focused",
            "workspace.xr_setup.inspect_session_stopping",
            "workspace.xr_setup.inspect_session_lost",
            "workspace.xr_setup.inspect_session_destroyed",
            "workspace.xr_setup.inspect_generations",
            "workspace.xr_setup.inspect_focus",
            "workspace.xr_setup.inspect_yes",
            "workspace.xr_setup.inspect_no",
            "workspace.xr_setup.inspect_capability_revision",
            "workspace.xr_setup.inspect_projection",
            "workspace.xr_setup.inspect_view",
            "workspace.xr_setup.inspect_local",
            "workspace.xr_setup.inspect_stage",
            "workspace.xr_setup.inspect_world",
            "workspace.xr_setup.inspect_action_space",
            "workspace.xr_setup.inspect_action_projection",
            "workspace.xr_setup.inspect_hands",
            "workspace.xr_setup.inspect_eye_gaze",
            "workspace.xr_setup.inspect_unsupported",
            "workspace.xr_setup.inspect_not_available",
            "workspace.xr_setup.inspect_available",
            "workspace.xr_setup.inspect_permission_required",
            "workspace.xr_setup.inspect_denied",
            "workspace.xr_setup.inspect_dependency_missing",
            "workspace.xr_setup.inspect_incompatible",
            "workspace.xr_setup.inspect_temporary",
            "workspace.xr_setup.inspect_policy_disabled",
            "workspace.xr_setup.inspect_not_requested",
            "workspace.xr_setup.inspect_required",
            "workspace.xr_setup.inspect_optional_enabled",
            "workspace.xr_setup.inspect_optional_disabled",
            "workspace.xr_setup.inspect_projection_fallback",
            "workspace.xr_setup.inspect_tracking",
            "workspace.xr_setup.inspect_active",
            "workspace.xr_setup.inspect_tracking_lost",
            "workspace.xr_setup.inspect_tracking_permission",
            "workspace.xr_setup.inspect_tracking_revisions",
            "workspace.xr_setup.inspect_capture_ms",
            "workspace.xr_setup.inspect_devices",
            "workspace.xr_setup.inspect_poses",
            "workspace.xr_setup.inspect_left_hand",
            "workspace.xr_setup.inspect_right_hand",
            "workspace.xr_setup.inspect_position_validity",
            "workspace.xr_setup.inspect_orientation_validity",
            "workspace.xr_setup.inspect_confidence",
            "workspace.xr_setup.inspect_space_references",
            "workspace.xr_setup.inspect_action_count",
            "workspace.xr_setup.inspect_source_head",
            "workspace.xr_setup.inspect_source_left_controller",
            "workspace.xr_setup.inspect_source_right_controller",
            "workspace.xr_setup.inspect_source_tracker",
            "workspace.xr_setup.inspect_source_left_hand",
            "workspace.xr_setup.inspect_source_right_hand",
            "workspace.xr_setup.inspect_binding_source",
            "workspace.xr_setup.inspect_boolean",
            "workspace.xr_setup.inspect_scalar",
            "workspace.xr_setup.inspect_vector",
            "workspace.xr_setup.inspect_omitted_actions",
            "workspace.xr_setup.inspect_tracked",
            "workspace.xr_setup.inspect_disconnected",
            "workspace.xr_setup.inspect_hand_generation",
            "workspace.xr_setup.close",
        };
        RequireCatalogKeys(service, xrSetupKeys);
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
        const Horo::Tests::OwnedTestDirectory directory{"localization catalog"};
        const auto path = directory.Path() / "horo-localization-test.json";
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
        const std::filesystem::path catalogRoot = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        const auto enPath = catalogRoot / "en-US.json";
        const auto trPath = catalogRoot / "tr-TR.json";
        REQUIRE(std::filesystem::is_regular_file(enPath));
        REQUIRE(std::filesystem::is_regular_file(trPath));

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
        CHECK(service.Get("editor", "workspace.prefab_document.compare") == "Compare with disk");
        CHECK(service.Get("editor", "workspace.prefab_document.keep") == "Keep local draft");
        CHECK(service.Get("editor", "workspace.prefab_document.undo") == "Undo source edit");
        CHECK(service.Get("editor", "workspace.prefab_document.discard_recovery").find("keep source edits") != std::string::npos);
        CHECK(service.Get("editor", "workspace.prefab_document.source_controls").find("Scene viewport") != std::string::npos);
        CHECK(service.Get("editor", "workspace.prefab_document.preview_unavailable").find("isolated runtime") != std::string::npos);
        CHECK(service.Get("editor", "render_graph.title") == "Render Graph");
        CHECK(service.Get("editor", "render_graph.unavailable") == "Unavailable");
        CHECK(service.Get("editor", "render_graph.next") == "Next page");
        REQUIRE((service.Get("editor", "workspace.global_dock.tab.localization") == "L10n"));
        REQUIRE((service.Get("editor", "workspace.game_asset.category.missing") == "Missing Gameplay Asset Type"));
        RequireSettingsCatalogKeys(service);
        RequireRemovalReviewKeys(service);
        for (const auto key : {"workspace.activity.move_left", "workspace.activity.move_right", "workspace.activity.move_bottom",
                               "workspace.activity.move_earlier", "workspace.activity.move_later"})
            CHECK(service.Get("editor", key).find("workspace.activity.") == std::string::npos);

        REQUIRE((service.Prepare(Horo::Editor::LocaleTag{"tr-TR"}, &error)));
        REQUIRE((service.ActivatePrepared(&error)));
        REQUIRE((service.Get("editor", "settings.input.shortcut.click_to_record") == "Kaydetmek için tıkla"));
        CHECK(service.Get("editor", "workspace.prefab_document.compare") == "Diskteki sürümle karşılaştır");
        CHECK(service.Get("editor", "workspace.prefab_document.keep") == "Yerel taslağı koru");
        REQUIRE((service.Get("editor", "settings.input.shortcut.press_keys") == "Tuşlara basın..."));
        REQUIRE((service.Get("editor", "workspace.content_browser.embedded") == "YERLEŞİK"));
        REQUIRE((service.Get("editor", "workspace.content_browser.project_asset_dock") == "Proje varlık paneli"));
        REQUIRE((service.Get("editor", "workspace.content_browser.empty") == "Bu klasör boş."));
        REQUIRE((service.Get("editor", "workspace.global_dock.tab.assets") == "Varlıklar"));
        CHECK(service.Get("editor", "render_graph.title") == "Render Grafiği");
        CHECK(service.Get("editor", "render_graph.unavailable") == "Kullanılamıyor");
        CHECK(service.Get("editor", "render_graph.next") == "Sonraki sayfa");
        REQUIRE((service.Get("editor", "workspace.global_dock.tab.localization") == "L10n"));
        REQUIRE((service.Get("editor", "workspace.game_asset.category.missing") == "Eksik Oynanış Asset Türü"));
        RequireSettingsCatalogKeys(service);
        RequireRemovalReviewKeys(service);
        for (const auto key : {"workspace.activity.move_left", "workspace.activity.move_right", "workspace.activity.move_bottom",
                               "workspace.activity.move_earlier", "workspace.activity.move_later"})
            CHECK(service.Get("editor", key).find("workspace.activity.") == std::string::npos);
    }

    TEST_CASE("Selected XR mirror meaning is explicit in both supported locales", "[unit][editor][xr][localization]") {
        const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        for (const auto &[locale, label] :
             std::array{std::pair{"en-US", "XR left-eye mirror"}, std::pair{"tr-TR", "XR sol göz yansıtması"}}) {
            Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{locale}};
            Horo::Editor::LocalizationError error;
            REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json"), &error));
            REQUIRE(service.Prepare(Horo::Editor::LocaleTag{locale}, &error));
            REQUIRE(service.ActivatePrepared(&error));
            CHECK(service.Get("editor", "workspace.game.xr_mirror") == label);
        }
    }

    TEST_CASE("Play Decision provider context is translated in both supported locales", "[unit][editor][ai][localization]") {
        const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        constexpr std::array providerKeys{"workspace.game.ai_problem_provider",
                                          "workspace.game.ai_problem_graph",
                                          "workspace.game.ai_problem_node",
                                          "workspace.game.ai_inspection_title",
                                          "workspace.game.ai_inspection_compare",
                                          "workspace.game.ai_inspection_filter",
                                          "workspace.game.ai_inspection_redacted",
                                          "workspace.game.ai_inspection_age",
                                          "workspace.game.ai_inspection_provider",
                                          "workspace.game.ai_inspection_provider_available",
                                          "workspace.game.ai_inspection_provider_unavailable",
                                          "workspace.game.ai_inspection_capability_spatial",
                                          "workspace.game.ai_inspection_capability_sight",
                                          "workspace.game.ai_inspection_capability_acoustic",
                                          "workspace.game.ai_inspection_capability_contact",
                                          "workspace.game.ai_inspection_capability_relay",
                                          "workspace.game.ai_inspection_capability_script",
                                          "workspace.game.ai_inspection_navigation",
                                          "workspace.game.ai_inspection_navigation_disabled",
                                          "workspace.game.ai_inspection_omitted_tasks",
                                          "workspace.game.ai_inspection_trace",
                                          "workspace.game.ai_inspection_transition",
                                          "workspace.game.ai_inspection_utility_consideration",
                                          "workspace.game.ai_inspection_utility_total",
                                          "workspace.game.ai_inspection_trace_overflow",
                                          "workspace.game.ai_debug_pause",
                                          "workspace.game.ai_debug_step",
                                          "workspace.game.ai_debug_resume",
                                          "workspace.game.ai_debug_set_breakpoint",
                                          "workspace.game.ai_debug_clear_breakpoint"};
        for (const auto locale : {"en-US", "tr-TR"}) {
            Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{locale}};
            Horo::Editor::LocalizationError error;
            REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json"), &error));
            REQUIRE(service.Prepare(Horo::Editor::LocaleTag{locale}, &error));
            REQUIRE(service.ActivatePrepared(&error));
            RequireCatalogKeys(service, providerKeys);
        }
    }

    constexpr const char *AudioInspectorKeys[] = {
        "workspace.inspector.audio.allow",
        "workspace.inspector.audio.asset_missing",
        "workspace.inspector.audio.bus",
        "workspace.inspector.audio.bus_missing",
        "workspace.inspector.audio.busy",
        "workspace.inspector.audio.clip",
        "workspace.inspector.audio.concurrency",
        "workspace.inspector.audio.concurrency_group",
        "workspace.inspector.audio.doppler",
        "workspace.inspector.audio.extension",
        "workspace.inspector.audio.keep_alive",
        "workspace.inspector.audio.lifecycle",
        "workspace.inspector.audio.listener_missing",
        "workspace.inspector.audio.listener_view",
        "workspace.inspector.audio.listener_weight",
        "workspace.inspector.audio.loop",
        "workspace.inspector.audio.max_instances",
        "workspace.inspector.audio.music",
        "workspace.inspector.audio.play_on_start",
        "workspace.inspector.audio.preview",
        "workspace.inspector.audio.preview_active",
        "workspace.inspector.audio.preview_closed",
        "workspace.inspector.audio.preview_closing",
        "workspace.inspector.audio.preview_failed",
        "workspace.inspector.audio.preview_opening",
        "workspace.inspector.audio.priority",
        "workspace.inspector.audio.provider_id",
        "workspace.inspector.audio.provider_missing",
        "workspace.inspector.audio.reject",
        "workspace.inspector.audio.replace",
        "workspace.inspector.audio.sound_id",
        "workspace.inspector.audio.sound_kind",
        "workspace.inspector.audio.stale",
        "workspace.inspector.audio.steal_furthest",
        "workspace.inspector.audio.steal_oldest",
        "workspace.inspector.audio.steal_priority",
        "workspace.inspector.audio.steal_quietest",
        "workspace.inspector.audio.stop",
        "workspace.inspector.audio.stop_unload",
        "workspace.inspector.audio.stream",
        "workspace.inspector.audio.unassigned",
        "workspace.inspector.audio.variation",
        "workspace.inspector.audio.virtualize",
        "workspace.inspector.audio_listener",
        "workspace.inspector.audio_listener_invalid",
        "workspace.inspector.kind.audio_listener",
    };

    TEST_CASE("Audio inspector catalog keys resolve in both shipped locales", "[unit][editor][audio][localization]") {
        const std::filesystem::path root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        Horo::Editor::LocalizationError error;
        for (const auto locale : {"en-US", "tr-TR"})
            REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json"), &error));
        for (const auto locale : {"en-US", "tr-TR"}) {
            REQUIRE(service.Prepare(Horo::Editor::LocaleTag{locale}, &error));
            REQUIRE(service.ActivatePrepared(&error));
            for (const auto key : AudioInspectorKeys) {
                INFO(locale);
                INFO(key);
                CHECK_FALSE(service.Get("editor", key).empty());
                CHECK(service.Get("editor", key) != key);
            }
        }
    }

    constexpr std::array PCGCanvasKeys{
        "workspace.pcg.preview.refresh",
        "workspace.pcg.preview.cancel",
        "workspace.pcg.preview.running",
        "workspace.pcg.preview.failed",
        "workspace.pcg.preview.current",
        "workspace.pcg.preview.empty",
        "workspace.pcg.undo",
        "workspace.pcg.redo",
        "workspace.pcg.reset_view",
        "workspace.pcg.focus",
        "workspace.pcg.zoom",
        "workspace.pcg.navigation_hint",
        "workspace.pcg.palette",
        "workspace.pcg.filter",
        "workspace.pcg.no_catalog",
        "workspace.pcg.unknown",
        "workspace.pcg.input",
        "workspace.pcg.output",
        "workspace.pcg.inspector",
        "workspace.pcg.select_hint",
        "workspace.pcg.remove",
        "workspace.pcg.identity",
        "workspace.pcg.version",
        "workspace.pcg.payload",
        "workspace.pcg.connections",
        "workspace.pcg.disconnect",
        "workspace.pcg.cancel_connection",
        "workspace.pcg.rejected",
        "workspace.pcg.nodes.grid",
        "workspace.pcg.nodes.density_filter",
        "workspace.pcg.nodes.merge",
        "workspace.pcg.nodes.forward",
        "workspace.pcg.nodes.jitter",
        "workspace.pcg.nodes.stratified",
        "workspace.pcg.nodes.poisson",
        "workspace.pcg.nodes.density_range",
        "workspace.pcg.nodes.bounds",
        "workspace.pcg.nodes.slope",
        "workspace.pcg.nodes.distance",
        "workspace.pcg.nodes.attribute",
        "workspace.pcg.nodes.transform",
        "workspace.pcg.nodes.alignment",
        "workspace.pcg.nodes.offset",
        "workspace.pcg.nodes.perlin",
        "workspace.pcg.nodes.simplex",
        "workspace.pcg.types.points",
        "workspace.pcg.types.boolean",
        "workspace.pcg.types.signed",
        "workspace.pcg.types.unsigned",
        "workspace.pcg.types.scalar",
        "workspace.pcg.types.vector2",
        "workspace.pcg.types.vector3",
        "workspace.pcg.types.vector4",
    };

    TEST_CASE("PCG canvas and built-in palette copy resolves in both shipped locales", "[unit][editor][pcg][localization]") {
        const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        for (const auto locale : {"en-US", "tr-TR"})
            REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json")));
        for (const auto locale : {"en-US", "tr-TR"}) {
            REQUIRE(service.Prepare(Horo::Editor::LocaleTag{locale}));
            REQUIRE(service.ActivatePrepared());
            RequireCatalogKeys(service, PCGCanvasKeys);
        }
    }

    constexpr std::array PCGAuthoringKeys{
        "workspace.pcg.authoring.input_key",
        "workspace.pcg.authoring.remove_input",
        "workspace.pcg.authoring.expose_input",
        "workspace.pcg.authoring.title",
        "workspace.pcg.authoring.invalid_value",
        "workspace.pcg.authoring.unsupported",
        "workspace.pcg.authoring.unavailable",
        "workspace.pcg.authoring.rejected",
        "workspace.pcg.authoring.apply",
        "workspace.pcg.authoring.cancel",
        "workspace.pcg.authoring.no_properties",
        "workspace.pcg.authoring.deterministic_defaults",
        "workspace.pcg.authoring.external_input",
        "workspace.pcg.authoring.non_deterministic",
        "workspace.pcg.authoring.profile_deterministic",
        "workspace.pcg.authoring.add_default",
        "workspace.pcg.authoring.remove_default",
        "workspace.pcg.authoring.tier",
        "workspace.pcg.authoring.baseline",
        "workspace.pcg.authoring.standard",
        "workspace.pcg.authoring.high",
        "workspace.pcg.authoring.mode",
        "workspace.pcg.authoring.offline",
        "workspace.pcg.authoring.runtime",
        "workspace.pcg.authoring.hybrid",
        "workspace.pcg.authoring.seed",
        "workspace.pcg.authoring.grid",
        "workspace.pcg.authoring.volume",
        "workspace.pcg.authoring.fraction",
        "workspace.pcg.authoring.strata",
        "workspace.pcg.authoring.count",
        "workspace.pcg.authoring.attempts",
        "workspace.pcg.authoring.minimum_distance",
        "workspace.pcg.authoring.minimum",
        "workspace.pcg.authoring.maximum",
        "workspace.pcg.authoring.minimum_degrees",
        "workspace.pcg.authoring.maximum_degrees",
        "workspace.pcg.authoring.attribute",
        "workspace.pcg.authoring.comparison",
        "workspace.pcg.authoring.equal",
        "workspace.pcg.authoring.not_equal",
        "workspace.pcg.authoring.less",
        "workspace.pcg.authoring.less_equal",
        "workspace.pcg.authoring.greater",
        "workspace.pcg.authoring.greater_equal",
        "workspace.pcg.authoring.predicate",
        "workspace.pcg.authoring.predicate_type",
        "workspace.pcg.authoring.translation",
        "workspace.pcg.authoring.rotation",
        "workspace.pcg.authoring.scale",
        "workspace.pcg.authoring.space",
        "workspace.pcg.authoring.snapshot",
        "workspace.pcg.authoring.point_local",
        "workspace.pcg.authoring.maximum_vertical_distance",
        "workspace.pcg.authoring.half_extent",
        "workspace.pcg.authoring.salt",
        "workspace.pcg.authoring.attribute_type",
        "workspace.pcg.authoring.boolean",
        "workspace.pcg.authoring.signed",
        "workspace.pcg.authoring.unsigned",
        "workspace.pcg.authoring.scalar",
        "workspace.pcg.authoring.vector2",
        "workspace.pcg.authoring.vector3",
        "workspace.pcg.authoring.vector4",
        "workspace.pcg.authoring.frequency",
        "workspace.pcg.authoring.amplitude",
        "workspace.pcg.authoring.octaves",
        "workspace.pcg.authoring.persistence",
        "workspace.pcg.authoring.lacunarity",
        "workspace.pcg.authoring.normalization",
        "workspace.pcg.authoring.signed_noise",
        "workspace.pcg.authoring.unit_interval",
        "workspace.pcg.authoring.default",
        "workspace.pcg.authoring.pin_default",
    };

    TEST_CASE("PCG authoring catalog keys resolve in both shipped locales", "[unit][editor][pcg][localization]") {
        const std::filesystem::path root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        Horo::Editor::LocalizationError error;
        for (const auto locale : {"en-US", "tr-TR"})
            REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json"), &error));
        for (const auto locale : {"en-US", "tr-TR"}) {
            REQUIRE(service.Prepare(Horo::Editor::LocaleTag{locale}, &error));
            REQUIRE(service.ActivatePrepared(&error));
            RequireCatalogKeys(service, PCGAuthoringKeys);
        }
    }

    TEST_CASE("World layer outliner messages resolve in both shipped locales", "[unit][editor][localization][layers]") {
        const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
        Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
        Horo::Editor::LocalizationError error;
        for (const auto locale : {"en-US", "tr-TR"})
            REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json"), &error));
        for (const auto locale : {"en-US", "tr-TR"}) {
            REQUIRE(service.Prepare(Horo::Editor::LocaleTag{locale}, &error));
            REQUIRE(service.ActivatePrepared(&error));
            for (const auto key : {
                     "workspace.world_layers.title",          "workspace.world_layers.create",      "workspace.world_layers.undo",
                     "workspace.world_layers.redo",           "workspace.world_layers.filter",      "workspace.world_layers.all_owners",
                     "workspace.world_layers.streaming",      "workspace.world_layers.gameplay",    "workspace.world_layers.network",
                     "workspace.world_layers.current",        "workspace.world_layers.set_current", "workspace.world_layers.clear_current",
                     "workspace.world_layers.move_up",        "workspace.world_layers.move_down",   "workspace.world_layers.empty",
                     "workspace.world_layers.current_hidden", "workspace.world_layers.name",        "workspace.world_layers.name_hint",
                     "workspace.world_layers.cancel",         "workspace.world_layers.stale",       "workspace.world_layers.rejected",
                 }) {
                INFO(locale);
                INFO(key);
                CHECK_FALSE(service.Get("editor", key).empty());
                CHECK(service.Get("editor", key) != key);
            }
        }
    }
}  // namespace

TEST_CASE("Microphone permission settings resolve in each shipped editor locale", "[unit][editor][localization]") {
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    for (const auto locale : {"en-US", "tr-TR"}) {
        const Horo::Editor::LocaleTag tag{locale};
        Horo::Editor::LocalizationService service{tag};
        REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json")));
        REQUIRE(service.Prepare(tag));
        REQUIRE(service.ActivatePrepared());
        for (const auto suffix : {"title", "required", "pending", "granted", "denied", "unavailable", "request", "input", "scan", "test",
                                  "test_description", "start", "stop", "starting", "capturing", "test_failed"}) {
            const auto key = std::string{"settings.audio.microphone."} + suffix;
            INFO(locale);
            CHECK_FALSE(service.Get("editor", key).empty());
            CHECK(service.Get("editor", key) != key);
        }
    }
}

TEST_CASE("Decision query authoring and preview copy resolve in both shipped locales", "[unit][editor][decision][localization]") {
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
    Horo::Editor::LocalizationError error;
    for (const auto locale : {"en-US", "tr-TR"})
        REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json"), &error));
    for (const auto locale : {"en-US", "tr-TR"}) {
        REQUIRE(service.Prepare(Horo::Editor::LocaleTag{locale}, &error));
        REQUIRE(service.ActivatePrepared(&error));
        for (const auto key : {"bound_query", "select_query", "review_query", "no_query_keys", "query_output_key", "select_query_key",
                               "query_timeout", "apply_query_output"}) {
            const auto full = std::string{"workspace.decision_graph."} + key;
            INFO(locale);
            INFO(full);
            CHECK_FALSE(service.Get("editor", full).empty());
            CHECK(service.Get("editor", full) != full);
        }
        constexpr auto framePoint = "workspace.environment_query.preview_frame_candidate";
        CHECK_FALSE(service.Get("editor", framePoint).empty());
        CHECK(service.Get("editor", framePoint) != framePoint);
    }
}

TEST_CASE("Local model panel copy resolves in both shipped locales", "[unit][editor][agent][localization]") {
    using namespace Horo::Editor;
    const auto root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    constexpr GuiLocalizationScope panel{"workspace.local_agent"};
    for (const auto locale : {"en-US", "tr-TR"}) {
        LocalizationService service{LocaleTag{locale}};
        LocalizationError error;
        REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json"), &error));
        REQUIRE(service.Prepare(LocaleTag{locale}, &error));
        REQUIRE(service.ActivatePrepared(&error));
        for (const auto key : {"title",
                               "endpoint",
                               "discover",
                               "model",
                               "prompt",
                               "ask",
                               "regenerate",
                               "stop",
                               "working",
                               "invalid",
                               "unavailable",
                               "timeout",
                               "cancelled",
                               "failed",
                               "no_models",
                               "empty_response",
                               "scene_count",
                               "scene_query_review",
                               "scene_query_arguments",
                               "scene_query_scope",
                               "scene_query_impact",
                               "scene_query_deny",
                               "scene_query_running",
                               "scene_query_succeeded",
                               "scene_query_denied",
                               "scene_query_cancelled",
                               "scene_query_timeout",
                               "scene_query_failed",
                               "scene_query_audit",
                               "scene_query_revoke",
                               "ask_with_scene_count",
                               "tools_unsupported",
                               "history_review",
                               "history_save",
                               "history_saving",
                               "history_saved",
                               "history_write_error",
                               "history_manage",
                               "history_changed",
                               "history_working",
                               "history_name",
                               "history_rename",
                               "history_archive",
                               "history_restore",
                               "history_export",
                               "history_copy_export",
                               "history_delete",
                               "history_delete_confirm",
                               "history_cancel",
                               "history_recovery_offer",
                               "history_recovery_prompt",
                               "history_recovery_start",
                               "history_recovery_answer",
                               "history_draft_offer",
                               "history_draft_preview",
                               "history_draft_save",
                               "history_draft_cancel",
                               "history_draft_saved",
                               "history_draft_unavailable",
                               "history_input_tokens",
                               "history_output_tokens",
                               "history_reference_review",
                               "history_reference_save",
                               "history_reference_record",
                               "history_reference_answer",
                               "history_reference_asset",
                               "history_reference_document",
                               "history_reference_tool",
                               "history_reference_build",
                               "history_reference_revision",
                               "history_recovery_keep_incomplete",
                               "history_incomplete",
                               "history_streaming",
                               "context_offer",
                               "context_file_offer",
                               "context_scene_selection_offer",
                               "context_scene_selection_unavailable",
                               "context_viewport_offer",
                               "context_viewport_unavailable",
                               "context_viewport_add_transforms",
                               "context_viewport_remove_transforms",
                               "context_file_empty",
                               "context_saved_file_offer",
                               "context_saved_file_path",
                               "context_saved_file_reading",
                               "context_saved_file_unavailable",
                               "context_file_unavailable",
                               "context_file_select",
                               "context_file_add",
                               "context_file_remove",
                               "context_files_preview",
                               "context_file_previous",
                               "context_file_next",
                               "context_range_offer",
                               "context_range_unavailable",
                               "context_build_offer",
                               "context_completed_build_offer",
                               "context_build_diagnostics_offer",
                               "context_build_diagnostics_unavailable",
                               "context_active_build_diagnostics_offer",
                               "context_active_build_diagnostics_unavailable",
                               "context_build_diagnostics_reading",
                               "context_editor_diagnostics_offer",
                               "context_editor_diagnostics_unavailable",
                               "context_editor_diagnostics_reading",
                               "context_completed_build_unavailable",
                               "context_build_unavailable",
                               "context_review",
                               "context_clipboard_note",
                               "context_clipboard_report",
                               "context_clipboard_unavailable",
                               "context_approve",
                               "context_cancel",
                               "context_unavailable",
                               "context_stale",
                               "history_recent_messages",
                               "history_title",
                               "history_unavailable"}) {
            INFO(locale);
            INFO(key);
            const auto &value = panel.Text(service, key);
            CHECK_FALSE(value.empty());
            CHECK(value != std::string{"workspace.local_agent."} + key);
        }
    }
}

TEST_CASE("UI canvas hierarchy and catalog copy resolve in both shipped locales", "[unit][editor][ui_canvas][localization]") {
    const std::filesystem::path root = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/localization/editor";
    Horo::Editor::LocalizationService service{Horo::Editor::LocaleTag{"en-US"}};
    Horo::Editor::LocalizationError error;
    for (const auto locale : {"en-US", "tr-TR"})
        REQUIRE(service.LoadCatalogFile(root / (std::string{locale} + ".json"), &error));
    for (const auto locale : {"en-US", "tr-TR"}) {
        REQUIRE(service.Prepare(Horo::Editor::LocaleTag{locale}, &error));
        REQUIRE(service.ActivatePrepared(&error));
        for (const auto key : {"undo",
                               "redo",
                               "reload",
                               "command_failed",
                               "preview.inspect_problem_element",
                               "preview.inspect_problem_property",
                               "search",
                               "create",
                               "move_up",
                               "move_down",
                               "reparent_hint",
                               "no_results",
                               "kind.canvas",
                               "kind.screen",
                               "kind.panel",
                               "kind.frame",
                               "kind.layout_group",
                               "kind.text",
                               "kind.image",
                               "kind.button",
                               "kind.progress_bar",
                               "kind.slider",
                               "kind.toggle",
                               "kind.text_input",
                               "kind.scroll_view",
                               "kind.collection_view"}) {
            const std::string full = std::string{"workspace.ui_canvas."} + key;
            INFO(locale);
            INFO(full);
            CHECK_FALSE(service.Get("editor", full).empty());
            CHECK(service.Get("editor", full) != full);
        }
        for (const auto key : {"workspace.ui_inspector.layout_measured_dip", "workspace.ui_inspector.layout_content_dip"}) {
            INFO(locale);
            INFO(key);
            CHECK_FALSE(service.Get("editor", key).empty());
            CHECK(service.Get("editor", key) != key);
        }
    }
}
