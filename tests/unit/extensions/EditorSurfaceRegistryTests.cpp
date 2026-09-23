#include "Horo/Extensions/EditorSurfaceRegistry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        [[nodiscard]] EditorSurfaceContextDescriptor ContextDescriptor(const std::string &id = "com.example.tools.inspector",
                                                                       const EditorSurfaceKind kind = EditorSurfaceKind::Tab) {
            return EditorSurfaceContextDescriptor{
                .surface =
                    EditorSurfaceDescriptor{
                        .id = id,
                        .kind = kind,
                        .labelLocalizationKey = "editor.tools.inspector.label",
                        .tooltipLocalizationKey = "editor.tools.inspector.tooltip",
                        .placement = EditorSurfacePlacement{EditorSurfacePlacementKind::Workspace, "bottom.tools", 20},
                        .persistence = EditorSurfacePersistence::Workspace,
                        .openByDefault = false,
                        .provider = EditorSurfaceProviderIdentity{"com.example.tools", "com.example.tools.editor", 4},
                    },
            };
        }

        [[nodiscard]] ExtensionCapabilityAdmission Admission() {
            const ExtensionAdmissionPolicy policy{
                .revision = 7,
                .knownPermissions = {},
                .approvedPermissions = {},
                .availableCapabilities = {},
            };
            const ExtensionAdmissionRequest request{
                .extensionId = "com.example.tools",
                .moduleId = "com.example.tools.editor",
                .activationGeneration = 4,
                .capabilities = {},
            };
            auto admission = ExtensionCapabilityAdmission::Evaluate(request, policy);
            REQUIRE(admission.HasValue());
            return std::move(admission).Value();
        }

        [[nodiscard]] EditorSurfaceRegistration RegisterSurface(EditorSurfaceRegistry &registry, EditorSurfaceContextProvider &provider,
                                                                ExtensionCapabilityAdmission &admission,
                                                                EditorSurfaceContextDescriptor descriptor = ContextDescriptor()) {
            auto attached = provider.Attach(std::move(descriptor), admission.ActivationLease());
            REQUIRE(attached.HasValue());
            auto registration = registry.Register(std::move(attached).Value());
            REQUIRE(registration.HasValue());
            return std::move(registration).Value();
        }

        void RequireErrorCode(const auto &result, const std::string &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }

        [[nodiscard]] EditorSurfaceProviderKey Provider() {
            return EditorSurfaceProviderKey{"com.example.tools", "com.example.tools.editor"};
        }
    }  // namespace

    TEST_CASE("External editor surfaces open focus close and restore deterministically", "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto registration = RegisterSurface(registry, provider, admission);

        auto opened = registry.Open(registration.Id());
        REQUIRE(opened.HasValue());
        CHECK(opened.Value().kind == EditorSurfaceOperationKind::Opened);
        auto focused = registry.Focus(registration.Id());
        REQUIRE(focused.HasValue());
        CHECK(focused.Value().kind == EditorSurfaceOperationKind::AlreadyFocused);
        auto closed = registry.Close(registration.Id());
        REQUIRE(closed.HasValue());
        CHECK(closed.Value().kind == EditorSurfaceOperationKind::Closed);

        const std::array<std::uint8_t, 3> persistedBytes{4, 5, 6};
        EditorSurfaceWorkspaceState workspace{
            .schemaVersion = EditorSurfaceRegistry::WorkspaceSchemaVersion,
            .surfaces = {EditorSurfaceWorkspaceEntry{
                .surfaceId = std::string{registration.Id()},
                .provider = Provider(),
                .open = true,
                .focused = true,
                .opaqueState = {persistedBytes.begin(), persistedBytes.end()},
            }},
        };
        auto restored = registry.Restore(workspace);
        REQUIRE(restored.HasValue());
        REQUIRE(restored.Value().restored.size() == 1);
        const auto snapshots = registry.Snapshot();
        REQUIRE(snapshots.size() == 1);
        CHECK(snapshots.front().open);
        CHECK(snapshots.front().focused);
        CHECK(snapshots.front().opaqueState == std::vector<std::uint8_t>{4, 5, 6});
    }

    TEST_CASE("External editor surfaces retain bounded state across provider unload and reattach",
              "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto registration = RegisterSurface(registry, provider, admission);
        REQUIRE(registry.Open(registration.Id()).HasValue());
        const std::array<std::uint8_t, 2> state{9, 8};
        REQUIRE(registry.SetOpaqueState(registration.Id(), state).HasValue());

        const std::string id{registration.Id()};
        registration.Reset();
        CHECK_FALSE(registration.IsRegistered());
        RequireErrorCode(registry.Open(id), "editor_surface_registry_provider_missing");
        const auto saved = registry.Save();
        REQUIRE(saved.surfaces.size() == 1);
        CHECK(saved.surfaces.front().open);
        CHECK(saved.surfaces.front().opaqueState == std::vector<std::uint8_t>{9, 8});

        auto replacement = RegisterSurface(registry, provider, admission, ContextDescriptor(id));
        CHECK(replacement.IsRegistered());
        const auto snapshots = registry.Snapshot();
        REQUIRE(snapshots.size() == 1);
        CHECK(snapshots.front().providerStatus == EditorSurfaceProviderStatus::Active);
        CHECK(snapshots.front().open);
        CHECK(snapshots.front().opaqueState == std::vector<std::uint8_t>{9, 8});
    }

    TEST_CASE("External editor surfaces distinguish disabled and missing providers", "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto registration = RegisterSurface(registry, provider, admission);
        REQUIRE(registry.SetProviderStatus(Provider(), EditorSurfaceProviderStatus::Disabled).HasValue());
        RequireErrorCode(registry.Open(registration.Id()), "editor_surface_registry_provider_disabled");
        CHECK(registry.Snapshot().front().providerStatus == EditorSurfaceProviderStatus::Disabled);

        REQUIRE(registry.SetProviderStatus(Provider(), EditorSurfaceProviderStatus::Active).HasValue());
        REQUIRE(registry.Open(registration.Id()).HasValue());
        REQUIRE(registry.SetProviderStatus(Provider(), EditorSurfaceProviderStatus::Missing).HasValue());
        RequireErrorCode(registry.Focus(registration.Id()), "editor_surface_registry_provider_missing");
        CHECK_FALSE(registry.Snapshot().front().open);
    }

    TEST_CASE("External editor surface restore keeps missing provider state for a later registration",
              "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        EditorSurfaceWorkspaceState workspace{
            .schemaVersion = EditorSurfaceRegistry::WorkspaceSchemaVersion,
            .surfaces = {EditorSurfaceWorkspaceEntry{
                .surfaceId = "com.example.tools.later",
                .provider = Provider(),
                .open = true,
                .focused = false,
            }},
        };
        auto report = registry.Restore(workspace);
        REQUIRE(report.HasValue());
        REQUIRE(report.Value().missingProvider.size() == 1);
        RequireErrorCode(registry.Open("com.example.tools.later"), "editor_surface_registry_provider_missing");

        auto registration = RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.later"));
        CHECK(registration.IsRegistered());
        auto opened = registry.Open(registration.Id());
        REQUIRE(opened.HasValue());
        CHECK(opened.Value().kind == EditorSurfaceOperationKind::Focused);
        CHECK(registry.Snapshot().front().open);
    }

    TEST_CASE("External editor surface restore is atomic and rejects duplicate or mismatched state",
              "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto registration = RegisterSurface(registry, provider, admission);
        REQUIRE(registry.Open(registration.Id()).HasValue());
        const auto before = registry.Save();

        EditorSurfaceWorkspaceState duplicate = before;
        duplicate.surfaces.push_back(duplicate.surfaces.front());
        RequireErrorCode(registry.Restore(duplicate), "editor_surface_registry_state_invalid");
        const auto afterDuplicate = registry.Save();
        REQUIRE(afterDuplicate.surfaces.size() == before.surfaces.size());
        CHECK(afterDuplicate.surfaces.front().open == before.surfaces.front().open);

        EditorSurfaceWorkspaceState wrongOwner = before;
        wrongOwner.surfaces.front().provider = EditorSurfaceProviderKey{"com.other.tools", "com.other.tools.editor"};
        RequireErrorCode(registry.Restore(wrongOwner), "editor_surface_registry_state_invalid");
        CHECK(registry.Snapshot().front().open);
    }

    TEST_CASE("External editor surface registry rejects unsupported surfaces and closes deterministically",
              "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto descriptor = ContextDescriptor("com.example.tools.modal");
        descriptor.surface.kind = EditorSurfaceKind::Modal;
        descriptor.surface.placement.kind = EditorSurfacePlacementKind::Modal;
        descriptor.surface.persistence = EditorSurfacePersistence::None;
        auto attached = provider.Attach(std::move(descriptor), admission.ActivationLease());
        REQUIRE(attached.HasValue());
        RequireErrorCode(registry.Register(std::move(attached).Value()), "editor_surface_registry_invalid");

        auto registration = RegisterSurface(registry, provider, admission);
        RequireErrorCode(registry.Focus(registration.Id()), "editor_surface_registry_surface_closed");
        CHECK(registry.Close(registration.Id()).Value().kind == EditorSurfaceOperationKind::AlreadyClosed);
        RequireErrorCode(registry.Open("com.example.tools.unknown"), "editor_surface_registry_unknown");
    }
}  // namespace Horo::Extensions::Tests
