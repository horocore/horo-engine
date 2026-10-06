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

    TEST_CASE("Extension retirement withdraws a real editor surface while retaining its executable owner",
              "[Extensions][EditorSurface][Retirement]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto registration = RegisterSurface(registry, provider, admission);
        auto retirement = std::make_shared<ExtensionRetirement>("com.example.tools", std::vector<std::string>{"com.example.tools.editor"});
        auto code = std::make_shared<int>(0);
        REQUIRE(retirement->BindModuleCode("com.example.tools.editor", code));
        REQUIRE(registration.AttachRetirement(retirement));
        CHECK_FALSE(registration.AttachRetirement(retirement));
        REQUIRE(registry.Open(registration.Id()).HasValue());
        const auto report = retirement->BeginRetirement();
        CHECK_FALSE(registration.IsRegistered());
        CHECK(registry.Open("com.example.tools.inspector").HasError());
        REQUIRE(report.outstanding.size() == 1);
        CHECK(report.outstanding.front().kind == ExtensionLeaseKind::UiSurface);
        CHECK(report.outstanding.front().subject == "com.example.tools.inspector");
        CHECK_FALSE(retirement->IsDrained());
        registration.Reset();
        CHECK(retirement->IsDrained());
    }

    TEST_CASE("Activity registration binds only a live same-generation panel", "[Extensions][EditorSurface][Activity]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto activity = ContextDescriptor("com.example.tools.activity", EditorSurfaceKind::ActivityItem);
        activity.surface.placement.kind = EditorSurfacePlacementKind::ActivityBar;
        activity.surface.activity = EditorActivityDestination{EditorActivitySide::Right, 1, "com.example.tools.drawer", "icons/tools.svg"};
        auto orphan = provider.Attach(activity, admission.ActivationLease());
        REQUIRE(orphan.HasValue());
        CHECK(registry.Register(std::move(orphan).Value()).HasError());
        auto drawer =
            RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.drawer", EditorSurfaceKind::Panel));
        auto registered = RegisterSurface(registry, provider, admission, activity);
        REQUIRE(registry.PublishActivity(activity.surface.provider, registered.Id(), {false, true, 42}).HasValue());
        const auto snapshots = registry.Snapshot();
        const auto *snapshot = FindSnapshot(snapshots, registered.Id());
        REQUIRE(snapshot != nullptr);
        CHECK_FALSE(snapshot->activity.visible);
        CHECK(snapshot->activity.badgeCount == 42);
        REQUIRE(registry.PublishActivity(activity.surface.provider, registered.Id(), {true, true, 42}).HasValue());
        REQUIRE(registry.ToggleActivity(activity.surface.provider, registered.Id()).HasValue());
        CHECK(FindSnapshot(registry.Snapshot(), drawer.Id())->open);
        CHECK(FindSnapshot(registry.Snapshot(), drawer.Id())->focused);
        REQUIRE(registry.ToggleActivity(activity.surface.provider, registered.Id()).HasValue());
        CHECK_FALSE(FindSnapshot(registry.Snapshot(), drawer.Id())->open);
        REQUIRE(registry.PublishActivity(activity.surface.provider, registered.Id(), {false, true, 42}).HasValue());
        CHECK(registry.ToggleActivity(activity.surface.provider, registered.Id()).HasError());
        const auto saved = registry.Save();
        const auto entry = std::ranges::find(saved.surfaces, registered.Id(), &EditorSurfaceWorkspaceEntry::surfaceId);
        REQUIRE(entry != saved.surfaces.end());
        CHECK(entry->activityVisible);
        REQUIRE(registry.SetActivityVisibility(registered.Id(), false).HasValue());
        REQUIRE(registry.PublishActivity(activity.surface.provider, registered.Id(), {true, true, 42}).HasValue());
        CHECK_FALSE(FindSnapshot(registry.Snapshot(), registered.Id())->activity.visible);
        CHECK(registry.ToggleActivity(activity.surface.provider, registered.Id()).HasError());
        CHECK(registry.Open(registered.Id()).HasError());
        CHECK(registry.Focus(registered.Id()).HasError());
        CHECK(registry.Open(drawer.Id()).HasError());
        CHECK(registry.Focus(drawer.Id()).HasError());
        auto stale = activity.surface.provider;
        ++stale.activationGeneration;
        CHECK(registry.PublishActivity(stale, registered.Id(), {true, true, 0}).HasError());
        admission.Revoke();
        CHECK(registry.PublishActivity(activity.surface.provider, registered.Id(), {true, true, 0}).HasError());
        CHECK_FALSE(FindSnapshot(registry.Snapshot(), registered.Id())->open);
    }

    TEST_CASE("Activity user placement is atomic durable and generation bound", "[Extensions][EditorSurface][Activity]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto drawerA =
            RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.drawer-a", EditorSurfaceKind::Panel));
        auto drawerB =
            RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.drawer-b", EditorSurfaceKind::Panel));
        auto first = ContextDescriptor("com.example.tools.a", EditorSurfaceKind::ActivityItem);
        first.surface.placement.kind = EditorSurfacePlacementKind::ActivityBar;
        first.surface.activity = EditorActivityDestination{EditorActivitySide::Left, 0, std::string{drawerA.Id()}, "icons/tools.svg"};
        auto second = first;
        second.surface.id = "com.example.tools.b";
        second.surface.activity->drawerId = std::string{drawerB.Id()};
        second.surface.activity->side = EditorActivitySide::Right;
        auto a = RegisterSurface(registry, provider, admission, first);
        auto b = RegisterSurface(registry, provider, admission, second);
        REQUIRE(registry.ToggleActivity(first.surface.provider, a.Id()).HasValue());
        REQUIRE(registry.ToggleActivity(second.surface.provider, b.Id()).HasValue());
        REQUIRE(registry.MoveActivity(first.surface.provider, a.Id(), {EditorActivitySide::Right, 2, 0}).HasValue());
        auto snapshots = registry.Snapshot();
        CHECK(FindSnapshot(snapshots, a.Id())->open);
        CHECK(FindSnapshot(snapshots, a.Id())->focused);
        CHECK(FindSnapshot(snapshots, drawerA.Id())->open);
        CHECK(FindSnapshot(snapshots, drawerA.Id())->focused);
        CHECK_FALSE(FindSnapshot(snapshots, b.Id())->open);
        CHECK_FALSE(FindSnapshot(snapshots, drawerB.Id())->open);
        CHECK(FindSnapshot(snapshots, a.Id())->descriptor.activity->group == 2);
        REQUIRE(registry.PublishActivity(first.surface.provider, a.Id(), {true, true, 9}).HasValue());
        CHECK(FindSnapshot(registry.Snapshot(), a.Id())->descriptor.activity->side == EditorActivitySide::Right);
        const auto revision = registry.Revision();
        CHECK(registry.MoveActivity(first.surface.provider, a.Id(), {EditorActivitySide::Bottom, 0, 1}).HasError());
        CHECK(registry.Revision() == revision);
        auto stale = first.surface.provider;
        ++stale.activationGeneration;
        CHECK(registry.MoveActivity(stale, a.Id(), {EditorActivitySide::Bottom, 0, 0}).HasError());
        REQUIRE(registry.MoveActivity(first.surface.provider, a.Id(), {EditorActivitySide::Right, 0, 0}).HasValue());
        REQUIRE(registry.MoveActivity(second.surface.provider, b.Id(), {EditorActivitySide::Right, 0, 0}).HasValue());
        snapshots = registry.Snapshot();
        CHECK(FindSnapshot(snapshots, b.Id())->descriptor.placement.order == 0);
        CHECK(FindSnapshot(snapshots, a.Id())->descriptor.placement.order == 1);
        REQUIRE(registry.Close(a.Id()).HasValue());
        const auto saved = registry.Save();
        auto malformedPlacement = saved;
        for (auto &entry : malformedPlacement.surfaces)
            if (entry.surfaceId == a.Id())
                entry.activityPlacement = EditorActivityPlacement{EditorActivitySide::Bottom, 3, 0};
        const auto restoredRevision = registry.Revision();
        CHECK(registry.Restore(malformedPlacement).HasError());
        CHECK(registry.Revision() == restoredRevision);
        CHECK(FindSnapshot(registry.Snapshot(), a.Id())->descriptor.activity->side == EditorActivitySide::Right);
        REQUIRE(registry.MoveActivity(first.surface.provider, a.Id(), {EditorActivitySide::Bottom, 1, 0}).HasValue());
        REQUIRE(registry.Restore(saved).HasValue());
        CHECK(FindSnapshot(registry.Snapshot(), a.Id())->descriptor.activity->side == EditorActivitySide::Right);
        auto legacy = saved;
        legacy.schemaVersion = 1;
        for (auto &entry : legacy.surfaces)
            entry.activityPlacement.reset();
        REQUIRE(registry.Restore(legacy).HasValue());
        CHECK(FindSnapshot(registry.Snapshot(), a.Id())->descriptor.activity->side == EditorActivitySide::Left);
        REQUIRE(registry.Restore(saved).HasValue());
        const auto id = std::string{a.Id()};
        a.Reset();
        a = RegisterSurface(registry, provider, admission, first);
        CHECK(FindSnapshot(registry.Snapshot(), id)->descriptor.activity->side == EditorActivitySide::Right);
        admission.Revoke();
        CHECK(registry.MoveActivity(first.surface.provider, id, {EditorActivitySide::Bottom, 0, 0}).HasError());
    }

    TEST_CASE("Activity activation replaces the same side and malformed restore is atomic", "[Extensions][EditorSurface][Activity]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto drawer =
            RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.drawer", EditorSurfaceKind::Panel));
        auto first = ContextDescriptor("com.example.tools.first", EditorSurfaceKind::ActivityItem);
        first.surface.placement.kind = EditorSurfacePlacementKind::ActivityBar;
        first.surface.activity = EditorActivityDestination{EditorActivitySide::Left, 0, "com.example.tools.drawer", "icons/tools.svg"};
        auto secondDrawer =
            RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.second-drawer", EditorSurfaceKind::Panel));
        auto second = first;
        second.surface.id = "com.example.tools.second";
        second.surface.activity->drawerId = std::string{secondDrawer.Id()};
        auto a = RegisterSurface(registry, provider, admission, first);
        auto b = RegisterSurface(registry, provider, admission, second);
        REQUIRE(registry.ToggleActivity(first.surface.provider, a.Id()).HasValue());
        REQUIRE(registry.ToggleActivity(second.surface.provider, b.Id()).HasValue());
        CHECK_FALSE(FindSnapshot(registry.Snapshot(), a.Id())->open);
        CHECK(FindSnapshot(registry.Snapshot(), b.Id())->open);
        auto invalid = registry.Save();
        for (auto &entry : invalid.surfaces) {
            if (entry.surfaceId == a.Id() || entry.surfaceId == b.Id())
                entry.open = true;
        }
        REQUIRE(registry.Restore(invalid).HasError());
        CHECK_FALSE(FindSnapshot(registry.Snapshot(), a.Id())->open);
        CHECK(FindSnapshot(registry.Snapshot(), b.Id())->open);
        REQUIRE(registry.Close(secondDrawer.Id()).HasValue());
        CHECK_FALSE(FindSnapshot(registry.Snapshot(), b.Id())->open);
        CHECK_FALSE(FindSnapshot(registry.Snapshot(), b.Id())->focused);
        auto malformed = registry.Save();
        for (auto &entry : malformed.surfaces) {
            if (entry.surfaceId == a.Id()) {
                entry.open = true;
                entry.activityVisible = false;
            }
        }
        CHECK(registry.Restore(malformed).HasError());
        auto inconsistent = registry.Save();
        for (auto &entry : inconsistent.surfaces)
            if (entry.surfaceId == drawer.Id())
                entry.open = true;
        CHECK(registry.Restore(inconsistent).HasError());
        drawer.Reset();
        secondDrawer.Reset();
        CHECK(FindSnapshot(registry.Snapshot(), a.Id())->providerStatus == EditorSurfaceProviderStatus::Missing);
        CHECK(FindSnapshot(registry.Snapshot(), b.Id())->providerStatus == EditorSurfaceProviderStatus::Missing);
        CHECK(registry.ToggleActivity(second.surface.provider, b.Id()).HasError());
    }

    TEST_CASE("Drawer form publication enforces generation and context allowlists", "[Extensions][EditorSurface][Activity]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry;
        auto descriptor = ContextDescriptor("com.example.tools.drawer", EditorSurfaceKind::Panel);
        descriptor.localization = {{"editor.tools.inspector.label"}};
        auto drawer = RegisterSurface(registry, provider, admission, descriptor);
        EditorUiForm form{
            .id = {"com.example.tools.form"},
            .title = {EditorUiTextKind::LocalizationKey, "editor.tools.inspector.label"},
            .nodes = {EditorUiNode{EditorUiTextNode{
                .base = {.id = {"com.example.tools.content"},
                         .label = {EditorUiTextKind::LocalizationKey, "editor.tools.inspector.label"},
                         .focusPolicy = EditorUiFocusPolicy::Never},
                .text = {EditorUiTextKind::TechnicalText, "diagnostic output"},
            }}},
        };
        REQUIRE(registry.PublishForm(descriptor.surface.provider, drawer.Id(), form).HasValue());
        REQUIRE(FindSnapshot(registry.Snapshot(), drawer.Id())->form.has_value());
        auto unauthorized = form;
        unauthorized.title.value = "editor.tools.unapproved.label";
        CHECK(registry.PublishForm(descriptor.surface.provider, drawer.Id(), unauthorized).HasError());
        auto stale = descriptor.surface.provider;
        ++stale.activationGeneration;
        CHECK(registry.PublishForm(stale, drawer.Id(), form).HasError());
        form.nodes = {EditorUiNode{EditorUiActionNode{
            .base = {.id = {"com.example.tools.apply"}, .label = {EditorUiTextKind::LocalizationKey, "editor.tools.inspector.label"}},
            .action = {"com.example.tools.apply"},
        }}};
        CHECK(registry.PublishForm(descriptor.surface.provider, drawer.Id(), form).HasError());
        const auto snapshots = registry.Snapshot();
        REQUIRE(FindSnapshot(snapshots, drawer.Id())->form.has_value());
        CHECK(EditorUiNodeKindOf(FindSnapshot(snapshots, drawer.Id())->form->nodes.front()) == EditorUiNodeKind::Text);
        admission.Revoke();
        CHECK_FALSE(FindSnapshot(registry.Snapshot(), drawer.Id())->form.has_value());
    }

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

    TEST_CASE("Editor surfaces reject per-entry and total opaque-state overflow without mutation",
              "[Extensions][EditorSurface][Registry]") {
        auto admission = Admission();
        EditorSurfaceContextProvider provider;
        EditorSurfaceRegistry registry{EditorSurfaceRegistryLimits{
            .maximumSurfaces = 2,
            .maximumOpaqueStateBytes = 4,
            .maximumTotalStateBytes = 5,
        }};
        auto first = RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.first"));
        auto second = RegisterSurface(registry, provider, admission, ContextDescriptor("com.example.tools.second"));
        const std::array<std::uint8_t, 3> valid{1, 2, 3};
        const std::array<std::uint8_t, 5> oversized{1, 2, 3, 4, 5};
        REQUIRE(registry.SetOpaqueState(first.Id(), valid).HasValue());
        RequireErrorCode(registry.SetOpaqueState(first.Id(), oversized), "editor_surface_registry_state_invalid");
        RequireErrorCode(registry.SetOpaqueState(second.Id(), valid), "editor_surface_registry_state_invalid");

        const auto snapshots = registry.Snapshot();
        const auto *firstSnapshot = FindSnapshot(snapshots, first.Id());
        const auto *secondSnapshot = FindSnapshot(snapshots, second.Id());
        REQUIRE(firstSnapshot != nullptr);
        REQUIRE(secondSnapshot != nullptr);
        CHECK(firstSnapshot->opaqueState == std::vector<std::uint8_t>{1, 2, 3});
        CHECK(secondSnapshot->opaqueState.empty());
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
