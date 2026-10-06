#include "EditorSurfaceRegistryTestSupport.h"

namespace Horo::Extensions::Tests {
    using namespace RegistrySupport;

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

    void CheckPlacementRestore(EditorSurfaceRegistry &registry, const EditorSurfaceProviderIdentity &provider, const std::string_view id) {
        const auto saved = registry.Save();
        auto malformedPlacement = saved;
        for (auto &entry : malformedPlacement.surfaces)
            if (entry.surfaceId == id)
                entry.activityPlacement = EditorActivityPlacement{EditorActivitySide::Bottom, 3, 0};
        const auto restoredRevision = registry.Revision();
        CHECK(registry.Restore(malformedPlacement).HasError());
        CHECK(registry.Revision() == restoredRevision);
        CHECK(FindSnapshot(registry.Snapshot(), id)->descriptor.activity->side == EditorActivitySide::Right);
        REQUIRE(registry.MoveActivity(provider, id, {EditorActivitySide::Bottom, 1, 0}).HasValue());
        REQUIRE(registry.Restore(saved).HasValue());
        CHECK(FindSnapshot(registry.Snapshot(), id)->descriptor.activity->side == EditorActivitySide::Right);
        auto legacy = saved;
        legacy.schemaVersion = 1;
        for (auto &entry : legacy.surfaces)
            entry.activityPlacement.reset();
        REQUIRE(registry.Restore(legacy).HasValue());
        CHECK(FindSnapshot(registry.Snapshot(), id)->descriptor.activity->side == EditorActivitySide::Left);
        REQUIRE(registry.Restore(saved).HasValue());
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
        CheckPlacementRestore(registry, first.surface.provider, a.Id());
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

}  // namespace Horo::Extensions::Tests
