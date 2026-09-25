#include "Horo/Extensions/EditorSurfaceRegistry.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        [[nodiscard]] EditorSurfaceContextDescriptor ContextDescriptor(
            const std::string &id = "com.example.tools.inspector", const EditorSurfaceKind kind = EditorSurfaceKind::Tab,
            const EditorSurfacePersistence persistence = EditorSurfacePersistence::Workspace) {
            return EditorSurfaceContextDescriptor{
                .surface =
                    EditorSurfaceDescriptor{
                        .id = id,
                        .kind = kind,
                        .labelLocalizationKey = "editor.tools.inspector.label",
                        .tooltipLocalizationKey = "editor.tools.inspector.tooltip",
                        .placement = EditorSurfacePlacement{EditorSurfacePlacementKind::Workspace, "bottom.tools", 20},
                        .persistence = persistence,
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

        [[nodiscard]] const EditorSurfaceSnapshot *FindSnapshot(const std::vector<EditorSurfaceSnapshot> &snapshots,
                                                                const std::string_view id) {
            const auto found = std::ranges::find_if(snapshots, [id](const EditorSurfaceSnapshot &snapshot) {
                return snapshot.descriptor.id == id;
            });
            return found == snapshots.end() ? nullptr : &*found;
        }

        struct PersistenceSurfaces final {
            EditorSurfaceRegistration workspace;
            EditorSurfaceRegistration session;
            EditorSurfaceRegistration project;
        };

        [[nodiscard]] PersistenceSurfaces RegisterPersistenceSurfaces(EditorSurfaceRegistry &registry,
                                                                      EditorSurfaceContextProvider &provider,
                                                                      ExtensionCapabilityAdmission &admission) {
            return {
                .workspace = RegisterSurface(registry, provider, admission,
                                             ContextDescriptor("com.example.tools.workspace", EditorSurfaceKind::Tab,
                                                               EditorSurfacePersistence::Workspace)),
                .session = RegisterSurface(registry, provider, admission,
                                           ContextDescriptor("com.example.tools.session", EditorSurfaceKind::Tab,
                                                             EditorSurfacePersistence::Session)),
                .project = RegisterSurface(registry, provider, admission,
                                           ContextDescriptor("com.example.tools.project", EditorSurfaceKind::Tab,
                                                             EditorSurfacePersistence::Project)),
            };
        }

        void OpenWithOpaqueState(EditorSurfaceRegistry &registry, const std::string_view id, const std::uint8_t value) {
            REQUIRE(registry.Open(id).HasValue());
            const std::array<std::uint8_t, 1> state{value};
            REQUIRE(registry.SetOpaqueState(id, state).HasValue());
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

    TEST_CASE("Workspace persistence saves and restores only workspace surface state", "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        const PersistenceSurfaces surfaces = RegisterPersistenceSurfaces(registry, provider, admission);
        OpenWithOpaqueState(registry, surfaces.workspace.Id(), 1);
        OpenWithOpaqueState(registry, surfaces.session.Id(), 2);
        OpenWithOpaqueState(registry, surfaces.project.Id(), 3);

        EditorSurfaceWorkspaceState saved = registry.Save();
        REQUIRE(saved.surfaces.size() == 1);
        CHECK(saved.surfaces.front().surfaceId == std::string{surfaces.workspace.Id()});

        saved.surfaces.front().open = false;
        saved.surfaces.front().focused = false;
        saved.surfaces.front().opaqueState = {9};
        REQUIRE(registry.Restore(saved).HasValue());
        const std::vector<EditorSurfaceSnapshot> snapshots = registry.Snapshot();
        const EditorSurfaceSnapshot *workspaceSnapshot = FindSnapshot(snapshots, surfaces.workspace.Id());
        const EditorSurfaceSnapshot *sessionSnapshot = FindSnapshot(snapshots, surfaces.session.Id());
        const EditorSurfaceSnapshot *projectSnapshot = FindSnapshot(snapshots, surfaces.project.Id());
        REQUIRE(workspaceSnapshot != nullptr);
        REQUIRE(sessionSnapshot != nullptr);
        REQUIRE(projectSnapshot != nullptr);
        CHECK_FALSE(workspaceSnapshot->open);
        CHECK(workspaceSnapshot->opaqueState == std::vector<std::uint8_t>{9});
        CHECK(sessionSnapshot->open);
        CHECK(sessionSnapshot->opaqueState == std::vector<std::uint8_t>{2});
        CHECK(projectSnapshot->open);
        CHECK(projectSnapshot->opaqueState == std::vector<std::uint8_t>{3});
    }

    TEST_CASE("Session and project surface state survives provider unload and reattach", "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        const PersistenceSurfaces surfaces = RegisterPersistenceSurfaces(registry, provider, admission);
        OpenWithOpaqueState(registry, surfaces.workspace.Id(), 1);
        OpenWithOpaqueState(registry, surfaces.session.Id(), 2);
        OpenWithOpaqueState(registry, surfaces.project.Id(), 3);

        const std::string sessionId{surfaces.session.Id()};
        const std::string projectId{surfaces.project.Id()};
        surfaces.session.Reset();
        surfaces.project.Reset();
        const EditorSurfaceWorkspaceState afterUnload = registry.Save();
        REQUIRE(afterUnload.surfaces.size() == 1);
        CHECK(afterUnload.surfaces.front().surfaceId == std::string{surfaces.workspace.Id()});
        auto sessionReplacement = RegisterSurface(registry, provider, admission,
                                                  ContextDescriptor(sessionId, EditorSurfaceKind::Tab, EditorSurfacePersistence::Session));
        auto projectReplacement = RegisterSurface(registry, provider, admission,
                                                  ContextDescriptor(projectId, EditorSurfaceKind::Tab, EditorSurfacePersistence::Project));
        CHECK(sessionReplacement.IsRegistered());
        CHECK(projectReplacement.IsRegistered());
        const std::vector<EditorSurfaceSnapshot> reattachedSnapshots = registry.Snapshot();
        const EditorSurfaceSnapshot *sessionSnapshot = FindSnapshot(reattachedSnapshots, sessionId);
        const EditorSurfaceSnapshot *projectSnapshot = FindSnapshot(reattachedSnapshots, projectId);
        REQUIRE(sessionSnapshot != nullptr);
        REQUIRE(projectSnapshot != nullptr);
        CHECK(sessionSnapshot->open);
        CHECK(sessionSnapshot->opaqueState == std::vector<std::uint8_t>{2});
        CHECK(projectSnapshot->open);
        CHECK(projectSnapshot->opaqueState == std::vector<std::uint8_t>{3});
    }

    TEST_CASE("Workspace restore rejects session and project surfaces", "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        const PersistenceSurfaces surfaces = RegisterPersistenceSurfaces(registry, provider, admission);
        const std::string sessionId{surfaces.session.Id()};
        const std::string projectId{surfaces.project.Id()};
        for (const std::string &id : {sessionId, projectId}) {
            EditorSurfaceWorkspaceState wrongScope{
                .schemaVersion = EditorSurfaceRegistry::WorkspaceSchemaVersion,
                .surfaces = {EditorSurfaceWorkspaceEntry{
                    .surfaceId = id,
                    .provider = Provider(),
                    .open = false,
                }},
            };
            RequireErrorCode(registry.Restore(wrongScope), "editor_surface_registry_state_invalid");
        }
    }

    TEST_CASE("External editor surface removal preserves state at the configured retention limit",
              "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry{EditorSurfaceRegistryLimits{
            .maximumSurfaces = 2,
            .maximumWorkspaceEntries = 1,
        }};
        auto registration = RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.retained"));
        REQUIRE(registry.Open(registration.Id()).HasValue());
        const std::array<std::uint8_t, 2> state{7, 6};
        REQUIRE(registry.SetOpaqueState(registration.Id(), state).HasValue());

        auto secondContext = provider.Attach(ContextDescriptor("com.example.tools.second"), admission.ActivationLease());
        REQUIRE(secondContext.HasValue());
        RequireErrorCode(registry.Register(std::move(secondContext).Value()), "editor_surface_registry_capacity_exceeded");

        registration.Reset();
        const EditorSurfaceWorkspaceState saved = registry.Save();
        REQUIRE(saved.surfaces.size() == 1);
        CHECK(saved.surfaces.front().surfaceId == "com.example.tools.retained");
        CHECK(saved.surfaces.front().open);
        CHECK(saved.surfaces.front().opaqueState == std::vector<std::uint8_t>{7, 6});
    }

    TEST_CASE("Restored unresolved surfaces reserve capacity for no-throw teardown", "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry{EditorSurfaceRegistryLimits{
            .maximumSurfaces = 2,
            .maximumWorkspaceEntries = 1,
        }};
        EditorSurfaceWorkspaceState restoredState{
            .schemaVersion = EditorSurfaceRegistry::WorkspaceSchemaVersion,
            .surfaces = {EditorSurfaceWorkspaceEntry{
                .surfaceId = "com.example.tools.restored",
                .provider = Provider(),
                .open = true,
                .focused = true,
            }},
        };
        REQUIRE(registry.Restore(restoredState).HasValue());

        auto unrelated = provider.Attach(ContextDescriptor("com.example.tools.unrelated"), admission.ActivationLease());
        REQUIRE(unrelated.HasValue());
        RequireErrorCode(registry.Register(std::move(unrelated).Value()), "editor_surface_registry_capacity_exceeded");

        auto restored = RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.restored"));
        CHECK(registry.Snapshot().front().open);
        restored.Reset();
        const EditorSurfaceWorkspaceState saved = registry.Save();
        REQUIRE(saved.surfaces.size() == 1);
        CHECK(saved.surfaces.front().surfaceId == "com.example.tools.restored");
        CHECK(saved.surfaces.front().focused);
    }

    TEST_CASE("Workspace restore cannot transfer unresolved state to another persistence scope", "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        const std::string surfaceId = "com.example.tools.future";
        EditorSurfaceWorkspaceState workspace{
            .schemaVersion = EditorSurfaceRegistry::WorkspaceSchemaVersion,
            .surfaces = {EditorSurfaceWorkspaceEntry{
                .surfaceId = surfaceId,
                .provider = Provider(),
                .open = true,
            }},
        };
        REQUIRE(registry.Restore(workspace).HasValue());

        auto projectContext = provider.Attach(ContextDescriptor(surfaceId, EditorSurfaceKind::Tab, EditorSurfacePersistence::Project),
                                              admission.ActivationLease());
        REQUIRE(projectContext.HasValue());
        RequireErrorCode(registry.Register(std::move(projectContext).Value()), "editor_surface_registry_invalid");
        const EditorSurfaceWorkspaceState saved = registry.Save();
        REQUIRE(saved.surfaces.size() == 1);
        CHECK(saved.surfaces.front().surfaceId == surfaceId);

        auto workspaceRegistration = RegisterSurface(registry, provider, admission, ContextDescriptor(surfaceId));
        CHECK(registry.Snapshot().front().open);
        CHECK(workspaceRegistration.Descriptor().persistence == EditorSurfacePersistence::Workspace);
    }

    TEST_CASE("Workspace restore enforces the total opaque-state budget across persistence scopes",
              "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry{EditorSurfaceRegistryLimits{
            .maximumSurfaces = 2,
            .maximumWorkspaceEntries = 2,
            .maximumOpaqueStateBytes = 4,
            .maximumTotalStateBytes = 5,
        }};
        auto workspace =
            RegisterSurface(registry, provider, admission,
                            ContextDescriptor("com.example.tools.workspace", EditorSurfaceKind::Tab, EditorSurfacePersistence::Workspace));
        auto session =
            RegisterSurface(registry, provider, admission,
                            ContextDescriptor("com.example.tools.session", EditorSurfaceKind::Tab, EditorSurfacePersistence::Session));
        const std::array<std::uint8_t, 3> sessionBytes{1, 2, 3};
        REQUIRE(registry.SetOpaqueState(session.Id(), sessionBytes).HasValue());

        EditorSurfaceWorkspaceState workspaceState{
            .schemaVersion = EditorSurfaceRegistry::WorkspaceSchemaVersion,
            .surfaces = {EditorSurfaceWorkspaceEntry{
                .surfaceId = std::string{workspace.Id()},
                .provider = Provider(),
                .open = true,
                .opaqueState = {4, 5, 6},
            }},
        };
        RequireErrorCode(registry.Restore(workspaceState), "editor_surface_registry_state_invalid");
        const std::vector<EditorSurfaceSnapshot> snapshots = registry.Snapshot();
        const EditorSurfaceSnapshot *workspaceSnapshot = FindSnapshot(snapshots, workspace.Id());
        const EditorSurfaceSnapshot *sessionSnapshot = FindSnapshot(snapshots, session.Id());
        REQUIRE(workspaceSnapshot != nullptr);
        REQUIRE(sessionSnapshot != nullptr);
        CHECK_FALSE(workspaceSnapshot->open);
        CHECK(workspaceSnapshot->opaqueState.empty());
        CHECK(sessionSnapshot->opaqueState == std::vector<std::uint8_t>{1, 2, 3});
    }

    TEST_CASE("External editor surface registry rejects overflowing preservation limits", "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistryLimits limits;
        limits.maximumWorkspaceEntries = std::numeric_limits<std::size_t>::max();
        EditorSurfaceRegistry registry{limits};
        auto context = provider.Attach(ContextDescriptor(), admission.ActivationLease());
        REQUIRE(context.HasValue());
        RequireErrorCode(registry.Register(std::move(context).Value()), "editor_surface_registry_invalid");
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
