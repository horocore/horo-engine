#pragma once
#include "Horo/Extensions/EditorViewportHost.h"

#include <array>

namespace Horo::Tests {
    /** @brief Creates real activation-scoped, revocable viewport authority and verified code ownership for tests. */
    struct ViewportContribution final {
        Extensions::EditorSurfaceRegistry registry;
        Extensions::ExtensionAdmissionPolicy policy{.revision = 1,
                                                    .knownPermissions = {{"editor.gizmo"}, {"editor.viewport-overlay"}},
                                                    .approvedPermissions = {{"editor.gizmo"}, {"editor.viewport-overlay"}},
                                                    .availableCapabilities = {Extensions::EditorGizmoCapability,
                                                                              Extensions::EditorViewportOverlayCapability}};
        Extensions::EditorViewportHost host{registry, policy};
        Extensions::EditorSurfaceContextProvider contexts;
        std::optional<Extensions::ExtensionCapabilityAdmission> admission;
        std::shared_ptr<void> code = std::make_shared<int>(1);
        std::shared_ptr<Extensions::ExtensionRetirement> retirement =
            std::make_shared<Extensions::ExtensionRetirement>("fixture.package", std::vector<std::string>{"fixture.module"});
        std::optional<Extensions::EditorViewportRegistration> registration;

        bool Publish(Extensions::EditorViewportDefinition definition, std::string id = "fixture.gizmo", bool overlay = false) {
            using namespace Extensions;
            if (!admission) {
                auto result = ExtensionCapabilityAdmission::Evaluate({"fixture.package",
                                                                      "fixture.module",
                                                                      1,
                                                                      {{EditorGizmoCapability, {{"editor.gizmo"}}},
                                                                       {EditorViewportOverlayCapability, {{"editor.viewport-overlay"}}}}},
                                                                     policy);
                if (result.HasError())
                    return false;
                admission.emplace(std::move(result).Value());
                if (!retirement->BindModuleCode("fixture.module", code))
                    return false;
            }
            EditorSurfaceContextDescriptor descriptor;
            descriptor.surface = {.id = std::move(id),
                                  .kind = overlay ? EditorSurfaceKind::ViewportOverlay : EditorSurfaceKind::Gizmo,
                                  .labelLocalizationKey = "fixture.gizmo",
                                  .placement = {EditorSurfacePlacementKind::Viewport, {}, 0},
                                  .requiredCapabilities = {overlay ? EditorViewportOverlayCapability : EditorGizmoCapability},
                                  .requiredPermissions = {{overlay ? "editor.viewport-overlay" : "editor.gizmo"}},
                                  .provider = {"fixture.package", "fixture.module", 1}};
            auto grant = admission->Grant(overlay ? EditorViewportOverlayCapability : EditorGizmoCapability);
            if (grant.HasError())
                return false;
            const std::array handles{std::move(grant).Value()};
            auto context = contexts.Attach(descriptor, admission->ActivationLease(), handles);
            if (context.HasError())
                return false;
            auto result = host.Register(std::move(context).Value(), std::move(definition), retirement);
            if (result.HasError())
                return false;
            registration.emplace(std::move(result).Value());
            return true;
        }
    };
}  // namespace Horo::Tests
