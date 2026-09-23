#include "Horo/Extensions/EditorCommandRegistry.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        [[nodiscard]] ExtensionCapabilityAdmission Admission() {
            const ExtensionAdmissionRequest request{
                .extensionId = "com.example.extension",
                .moduleId = "com.example.editor",
                .activationGeneration = 3,
                .capabilities = {{{"editor.asset.query"}, {}}},
            };
            const ExtensionAdmissionPolicy policy{
                .revision = 2,
                .availableCapabilities = {{"editor.asset.query"}},
            };
            auto admission = ExtensionCapabilityAdmission::Evaluate(request, policy);
            REQUIRE(admission.HasValue());
            return std::move(admission).Value();
        }

        [[nodiscard]] EditorSurfaceContextDescriptor SurfaceContext(const EditorSurfaceKind kind, const std::string &surfaceId,
                                                                    const std::string &commandId, const bool requiresCapability = false) {
            const auto placementKind = kind == EditorSurfaceKind::MenuItem        ? EditorSurfacePlacementKind::Menu
                                       : kind == EditorSurfaceKind::ToolbarAction ? EditorSurfacePlacementKind::Toolbar
                                       : kind == EditorSurfaceKind::StatusItem    ? EditorSurfacePlacementKind::StatusBar
                                                                                  : EditorSurfacePlacementKind::Workspace;
            const auto placementTarget = kind == EditorSurfaceKind::MenuItem        ? "file"
                                         : kind == EditorSurfaceKind::ToolbarAction ? "main"
                                         : kind == EditorSurfaceKind::StatusItem    ? "status"
                                                                                    : "workspace";
            const std::string label = "editor.commands." + commandId.substr(commandId.find_last_of('.') + 1) + ".label";
            const std::string tooltip = "editor.commands." + commandId.substr(commandId.find_last_of('.') + 1) + ".tooltip";
            return {
                .surface =
                    EditorSurfaceDescriptor{
                        .id = surfaceId,
                        .kind = kind,
                        .labelLocalizationKey = "editor.surface.label",
                        .tooltipLocalizationKey = "editor.surface.tooltip",
                        .placement = EditorSurfacePlacement{placementKind, placementTarget, 10},
                        .requiredCapabilities = requiresCapability ? std::vector<ExtensionCapabilityId>{{"editor.asset.query"}}
                                                                   : std::vector<ExtensionCapabilityId>{},
                        .provider = EditorSurfaceProviderIdentity{"com.example.extension", "com.example.editor", 3},
                    },
                .commands = {{commandId}},
                .localization = {{"editor.surface.label"}, {"editor.surface.tooltip"}, {label}, {tooltip}},
            };
        }

        [[nodiscard]] EditorCommandDescriptor Command(const std::string &id, const std::string &labelSuffix,
                                                      const std::string &shortcut = {}) {
            return {
                .id = EditorCommandId{id},
                .labelLocalizationKey = "editor.commands." + labelSuffix + ".label",
                .tooltipLocalizationKey = "editor.commands." + labelSuffix + ".tooltip",
                .shortcut = shortcut,
            };
        }

        [[nodiscard]] EditorSurfaceContextRegistration Attach(EditorSurfaceContextProvider &provider,
                                                              ExtensionCapabilityAdmission &admission,
                                                              EditorSurfaceContextDescriptor descriptor,
                                                              const bool grantCapability = false) {
            std::vector<ExtensionCapabilityHandle> grants;
            if (grantCapability) {
                auto grant = admission.Grant({"editor.asset.query"});
                REQUIRE(grant.HasValue());
                grants.push_back(std::move(grant).Value());
            }
            auto attached = provider.Attach(std::move(descriptor), admission.ActivationLease(), grants);
            REQUIRE(attached.HasValue());
            return std::move(attached).Value();
        }

        void RequireError(const auto &result, const std::string_view code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }
    }  // namespace

    TEST_CASE("Editor command registry evaluates localized commands and emits unload-safe invocations", "[Extensions][EditorCommand]") {
        ExtensionCapabilityAdmission admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorCommandRegistry registry;
        auto context = Attach(provider, admission, SurfaceContext(EditorSurfaceKind::MenuItem, "com.example.menu", "editor.save"));
        auto descriptor = Command("editor.save", "save", "Ctrl+S");
        descriptor.predicates = {{EditorCommandPredicateKind::ProjectOpen, {}, true}};
        auto published = registry.Register(std::move(context), std::move(descriptor));
        REQUIRE(published.HasValue());
        auto registration = std::move(published).Value();

        const std::vector<std::string_view> openSurfaces{"com.example.menu"};
        const EditorCommandEvaluationContext enabledContext{
            .projectOpen = true,
            .selectionPresent = false,
            .openSurfaceIds = openSurfaces,
        };
        auto state = registry.Evaluate("editor.save", enabledContext);
        REQUIRE(state.HasValue());
        CHECK(state.Value().enabled);
        auto invocation = registry.Invoke("editor.save", enabledContext);
        REQUIRE(invocation.HasValue());
        CHECK(invocation.Value().id.value == "editor.save");
        CHECK(invocation.Value().provider.activationGeneration == 3);
        CHECK(invocation.Value().context.IsUsable());

        const EditorCommandEvaluationContext disabledContext{.projectOpen = false};
        state = registry.Evaluate("editor.save", disabledContext);
        REQUIRE(state.HasValue());
        CHECK_FALSE(state.Value().enabled);
        RequireError(registry.Invoke("editor.save", disabledContext), "editor_command_not_enabled");
        RequireError(registry.Evaluate("editor.missing", enabledContext), "editor_command_unknown");

        const EditorCommandInvocation retainedInvocation = std::move(invocation).Value();
        registration.Reset();
        CHECK_FALSE(registration.IsRegistered());
        CHECK_FALSE(retainedInvocation.context.IsUsable());
        RequireError(registry.Evaluate("editor.save", enabledContext), "editor_command_unknown");
    }

    TEST_CASE("Editor command registry reports ID and shortcut collisions deterministically", "[Extensions][EditorCommand]") {
        ExtensionCapabilityAdmission admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorCommandRegistry registry;

        auto firstContext =
            Attach(provider, admission, SurfaceContext(EditorSurfaceKind::MenuItem, "com.example.menu.first", "editor.save"));
        auto first = registry.Register(std::move(firstContext), Command("editor.save", "save", "Ctrl+S"));
        REQUIRE(first.HasValue());
        auto firstRegistration = std::move(first).Value();

        auto duplicateContext =
            Attach(provider, admission, SurfaceContext(EditorSurfaceKind::MenuItem, "com.example.menu.duplicate", "editor.save"));
        RequireError(registry.Register(std::move(duplicateContext), Command("editor.save", "save", "Ctrl+Shift+S")),
                     "editor_command_duplicate");

        auto shortcutContext =
            Attach(provider, admission, SurfaceContext(EditorSurfaceKind::ToolbarAction, "com.example.toolbar", "editor.run"));
        RequireError(registry.Register(std::move(shortcutContext), Command("editor.run", "run", "Ctrl+S")),
                     "editor_command_shortcut_conflict");

        const auto diagnostics = registry.Diagnostics();
        REQUIRE(diagnostics.size() == 2);
        CHECK(diagnostics[0].kind == EditorCommandDiagnosticKind::DuplicateId);
        CHECK(diagnostics[0].commandId == "editor.save");
        CHECK(diagnostics[1].kind == EditorCommandDiagnosticKind::ShortcutConflict);
        CHECK(diagnostics[1].conflictingCommandId == "editor.save");
        CHECK(diagnostics[1].shortcut == "Ctrl+S");
        CHECK(firstRegistration.IsRegistered());
    }

    TEST_CASE("Editor command registry accepts menu toolbar and status contributions in explicit order", "[Extensions][EditorCommand]") {
        ExtensionCapabilityAdmission admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorCommandRegistry registry;

        auto toolbarContext =
            Attach(provider, admission, SurfaceContext(EditorSurfaceKind::ToolbarAction, "com.example.toolbar", "editor.run"));
        auto toolbar = registry.Register(std::move(toolbarContext), Command("editor.run", "run"));
        REQUIRE(toolbar.HasValue());
        auto toolbarRegistration = std::move(toolbar).Value();

        auto statusContext =
            Attach(provider, admission, SurfaceContext(EditorSurfaceKind::StatusItem, "com.example.status", "editor.status"));
        auto statusCommand = Command("editor.status", "status");
        statusCommand.order = 2;
        auto status = registry.Register(std::move(statusContext), std::move(statusCommand));
        REQUIRE(status.HasValue());
        auto statusRegistration = std::move(status).Value();

        auto menuContext = Attach(provider, admission, SurfaceContext(EditorSurfaceKind::MenuItem, "com.example.menu", "editor.save"));
        auto menuCommand = Command("editor.save", "save");
        menuCommand.order = 2;
        menuCommand.priority = 10;
        auto menu = registry.Register(std::move(menuContext), std::move(menuCommand));
        REQUIRE(menu.HasValue());
        auto menuRegistration = std::move(menu).Value();

        const auto snapshot = registry.Snapshot();
        REQUIRE(snapshot.size() == 3);
        CHECK(snapshot[0].surface.kind == EditorSurfaceKind::ToolbarAction);
        CHECK(snapshot[1].command.id.value == "editor.save");
        CHECK(snapshot[2].surface.kind == EditorSurfaceKind::StatusItem);
        CHECK(toolbarRegistration.IsRegistered());
        CHECK(statusRegistration.IsRegistered());
        CHECK(menuRegistration.IsRegistered());
    }

    TEST_CASE("Editor command predicates require admitted capabilities and provider teardown revokes publication",
              "[Extensions][EditorCommand]") {
        ExtensionCapabilityAdmission admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorCommandRegistry registry;
        auto context = Attach(provider, admission,
                              SurfaceContext(EditorSurfaceKind::ToolbarAction, "com.example.toolbar.asset", "editor.asset", true), true);
        auto descriptor = Command("editor.asset", "asset");
        descriptor.predicates = {
            {EditorCommandPredicateKind::SurfaceOpen, "com.example.toolbar.asset", true},
            {EditorCommandPredicateKind::CapabilityAvailable, "editor.asset.query", true},
        };
        auto published = registry.Register(std::move(context), std::move(descriptor));
        REQUIRE(published.HasValue());
        auto registration = std::move(published).Value();

        const std::vector<std::string_view> openSurfaces{"com.example.toolbar.asset"};
        const std::vector<ExtensionCapabilityId> capabilities{{"editor.asset.query"}};
        const EditorCommandEvaluationContext evaluation{
            .openSurfaceIds = openSurfaces,
            .availableCapabilities = capabilities,
        };
        auto invocation = registry.Invoke("editor.asset", evaluation);
        REQUIRE(invocation.HasValue());
        CHECK(invocation.Value().context.IsUsable());

        admission.Revoke();
        CHECK_FALSE(registration.IsRegistered());
        CHECK(registry.Snapshot().empty());
        RequireError(registry.Evaluate("editor.asset", evaluation), "editor_command_provider_revoked");

        registry.BeginShutdown();
        CHECK(registry.IsShutdown());
    }

    TEST_CASE("Editor command registry rejects unsupported surfaces and unallowlisted localization", "[Extensions][EditorCommand]") {
        ExtensionCapabilityAdmission admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorCommandRegistry registry;

        auto panelContext = Attach(provider, admission, SurfaceContext(EditorSurfaceKind::Panel, "com.example.panel", "editor.panel"));
        RequireError(registry.Register(std::move(panelContext), Command("editor.panel", "panel")), "editor_command_invalid");

        auto context = Attach(provider, admission, SurfaceContext(EditorSurfaceKind::MenuItem, "com.example.menu", "editor.save"));
        auto descriptor = Command("editor.save", "not_allowlisted");
        RequireError(registry.Register(std::move(context), std::move(descriptor)), "editor_command_invalid");
    }
}  // namespace Horo::Extensions::Tests
