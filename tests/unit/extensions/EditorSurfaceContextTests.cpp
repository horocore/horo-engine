#include "Horo/Extensions/EditorSurfaceContext.h"

#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        [[nodiscard]] EditorSurfaceContextDescriptor ContextDescriptor() {
            return {
                .surface =
                    EditorSurfaceDescriptor{
                        .id = "com.example.tools.inspector",
                        .kind = EditorSurfaceKind::Tab,
                        .labelLocalizationKey = "editor.tools.inspector.label",
                        .tooltipLocalizationKey = "editor.tools.inspector.tooltip",
                        .placement = EditorSurfacePlacement{EditorSurfacePlacementKind::Workspace, "bottom.tools", 20},
                        .persistence = EditorSurfacePersistence::Workspace,
                        .openByDefault = false,
                        .requiredCapabilities = {{"editor.selection.query"}, {"editor.asset.query"}},
                        .requiredPermissions = {{"editor.workspace.read"}},
                        .provider = EditorSurfaceProviderIdentity{"com.example.tools", "com.example.tools.editor", 4},
                    },
                .commands = {{"editor.asset.open"}, {"editor.asset.refresh"}},
                .state = {{"selection.asset"}, {"presentation.expanded"}},
                .services = {{"selection.query"}, {"asset.query"}},
                .localization = {{"editor.tools.inspector.label"}, {"editor.tools.inspector.tooltip"}},
                .diagnostics = {{"editor.asset.missing"}, {"editor.asset.invalid"}},
            };
        }

        [[nodiscard]] ExtensionCapabilityAdmission Admission() {
            const ExtensionAdmissionPolicy policy{
                .revision = 7,
                .knownPermissions = {{"editor.workspace.read"}},
                .approvedPermissions = {{"editor.workspace.read"}},
                .availableCapabilities = {{"editor.asset.query"}, {"editor.selection.query"}},
            };
            const ExtensionAdmissionRequest request{
                .extensionId = "com.example.tools",
                .moduleId = "com.example.tools.editor",
                .activationGeneration = 4,
                .capabilities =
                    {
                        {{"editor.asset.query"}, {{"editor.workspace.read"}}},
                        {{"editor.selection.query"}, {{"editor.workspace.read"}}},
                    },
            };
            auto admission = ExtensionCapabilityAdmission::Evaluate(request, policy);
            REQUIRE(admission.HasValue());
            return std::move(admission).Value();
        }

        [[nodiscard]] std::vector<ExtensionCapabilityHandle> Grants(ExtensionCapabilityAdmission &admission) {
            auto asset = admission.Grant({"editor.asset.query"});
            REQUIRE(asset.HasValue());
            auto selection = admission.Grant({"editor.selection.query"});
            REQUIRE(selection.HasValue());
            std::vector<ExtensionCapabilityHandle> grants;
            grants.push_back(std::move(asset).Value());
            grants.push_back(std::move(selection).Value());
            return grants;
        }

        void RequireErrorCode(const auto &result, const std::string &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }
    }  // namespace

    TEST_CASE("Editor surface context exposes only the approved typed access lists", "[Extensions][EditorSurface][Context]") {
        auto admission = Admission();
        auto grants = Grants(admission);
        EditorSurfaceContextProvider provider;
        auto attached = provider.Attach(ContextDescriptor(), admission.ActivationLease(), grants);
        REQUIRE(attached.HasValue());
        auto registration = std::move(attached).Value();
        const EditorSurfaceContext context = registration.Context();

        CHECK(context.IsUsable());
        CHECK(context.Provider().extensionId == "com.example.tools");
        CHECK(context.Provider().moduleId == "com.example.tools.editor");
        CHECK(context.Provider().activationGeneration == 4);
        CHECK(context.Commands().size() == 2);
        CHECK(context.State().size() == 2);
        CHECK(context.Services().size() == 2);
        CHECK(context.Localization().size() == 2);
        CHECK(context.Diagnostics().size() == 2);
        CHECK(context.Allows(EditorSurfaceCommandId{"editor.asset.open"}));
        CHECK_FALSE(context.Allows(EditorSurfaceCommandId{"editor.project.delete"}));
        CHECK(context.Allows(EditorSurfaceStateKey{"selection.asset"}));
        CHECK(context.Allows(EditorSurfaceServiceId{"asset.query"}));
        CHECK(context.Allows(EditorSurfaceLocalizationKey{"editor.tools.inspector.label"}));
        CHECK(context.Allows(EditorSurfaceDiagnosticCode{"editor.asset.missing"}));

        auto use = context.AcquireCapabilityUse({"editor.selection.query"});
        REQUIRE(use.HasValue());
        CHECK(use.Value().Activation().ExtensionId() == "com.example.tools");
        CHECK(use.Value().Activation().Generation() == 4);
        RequireErrorCode(context.AcquireCapabilityUse({"editor.project.write"}), "capability_unavailable");
    }

    TEST_CASE("Editor surface context validation bounds and canonicalizes every access category", "[Extensions][EditorSurface][Context]") {
        auto descriptor = ContextDescriptor();
        descriptor.commands.push_back({"editor.asset.open"});
        RequireErrorCode(ValidateEditorSurfaceContextDescriptor(descriptor), "editor_surface_context_invalid");

        descriptor = ContextDescriptor();
        descriptor.localization.front().value = "Editor.Tools.Invalid";
        RequireErrorCode(ValidateEditorSurfaceContextDescriptor(descriptor), "editor_surface_context_invalid");

        descriptor = ContextDescriptor();
        EditorSurfaceContextLimits limits;
        limits.maximumCommands = 0;
        descriptor.commands.clear();
        limits.maximumStateKeys = 0;
        descriptor.state.clear();
        limits.maximumServices = 0;
        descriptor.services.clear();
        limits.maximumLocalizationKeys = 0;
        descriptor.localization.clear();
        limits.maximumDiagnostics = 0;
        descriptor.diagnostics.clear();
        CHECK(ValidateEditorSurfaceContextDescriptor(descriptor, limits).HasValue());

        limits.maximumIdentityBytes = 3;
        limits.maximumDiagnostics = 1;
        descriptor.diagnostics.push_back({"editor.asset.missing"});
        RequireErrorCode(ValidateEditorSurfaceContextDescriptor(descriptor, limits), "editor_surface_context_invalid");
    }

    TEST_CASE("Editor surface context binds capability grants to the exact provider activation", "[Extensions][EditorSurface][Context]") {
        auto admission = Admission();
        auto grants = Grants(admission);
        EditorSurfaceContextProvider provider;

        auto mismatched = ContextDescriptor();
        mismatched.surface.provider.extensionId = "com.example.other";
        RequireErrorCode(provider.Attach(std::move(mismatched), admission.ActivationLease(), grants),
                         "editor_surface_context_provider_mismatch");

        auto descriptor = ContextDescriptor();
        descriptor.surface.requiredCapabilities = {{"editor.selection.query"}};
        auto extraGrant = admission.Grant({"editor.asset.query"});
        REQUIRE(extraGrant.HasValue());
        std::vector<ExtensionCapabilityHandle> extra{std::move(extraGrant).Value()};
        RequireErrorCode(provider.Attach(std::move(descriptor), admission.ActivationLease(), extra), "capability_unavailable");

        admission.Revoke();
        RequireErrorCode(provider.Attach(ContextDescriptor(), admission.ActivationLease(), grants), "editor_surface_context_revoked");
    }

    TEST_CASE("Editor surface context teardown revokes copies before provider activation ends", "[Extensions][EditorSurface][Context]") {
        auto admission = Admission();
        auto grants = Grants(admission);
        EditorSurfaceContextProvider provider;
        auto attached = provider.Attach(ContextDescriptor(), admission.ActivationLease(), grants);
        REQUIRE(attached.HasValue());
        auto registration = std::move(attached).Value();
        const EditorSurfaceContext context = registration.Context();
        CHECK(registration.IsRegistered());

        registration.Reset();
        CHECK_FALSE(registration.IsRegistered());
        CHECK_FALSE(context.IsUsable());
        CHECK_FALSE(registration.Context().IsUsable());
        RequireErrorCode(context.AcquireCapabilityUse({"editor.selection.query"}), "editor_surface_context_revoked");

        auto replacement = provider.Attach(ContextDescriptor(), admission.ActivationLease(), grants);
        REQUIRE(replacement.HasValue());
        provider.BeginShutdown();
        CHECK(provider.IsShutdown());
        CHECK_FALSE(replacement.Value().Context().IsUsable());
        RequireErrorCode(provider.Attach(ContextDescriptor(), admission.ActivationLease(), grants), "editor_surface_context_shutdown");
    }

    TEST_CASE("Editor surface context provider enforces its live-context bound", "[Extensions][EditorSurface][Context]") {
        EditorSurfaceContextLimits limits;
        limits.maximumContexts = 1;
        EditorSurfaceContextProvider provider{limits};
        auto admission = Admission();
        auto grants = Grants(admission);
        auto first = provider.Attach(ContextDescriptor(), admission.ActivationLease(), grants);
        REQUIRE(first.HasValue());
        auto firstRegistration = std::move(first).Value();
        RequireErrorCode(provider.Attach(ContextDescriptor(), admission.ActivationLease(), grants),
                         "editor_surface_context_capacity_exceeded");
        firstRegistration.Reset();
        auto replacement = provider.Attach(ContextDescriptor(), admission.ActivationLease(), grants);
        REQUIRE(replacement.HasValue());
    }
}  // namespace Horo::Extensions::Tests
