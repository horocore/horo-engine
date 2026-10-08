#pragma once
#include "Horo/Extensions/EditorSurfaceRegistry.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests::RegistrySupport {
    [[nodiscard]] inline EditorSurfaceContextDescriptor ContextDescriptor(
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

    [[nodiscard]] inline ExtensionCapabilityAdmission Admission() {
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

    [[nodiscard]] inline EditorSurfaceRegistration RegisterSurface(EditorSurfaceRegistry &registry, EditorSurfaceContextProvider &provider,
                                                                   ExtensionCapabilityAdmission &admission,
                                                                   EditorSurfaceContextDescriptor descriptor = ContextDescriptor()) {
        auto attached = provider.Attach(std::move(descriptor), admission.ActivationLease());
        REQUIRE(attached.HasValue());
        auto registration = registry.Register(std::move(attached).Value());
        REQUIRE(registration.HasValue());
        return std::move(registration).Value();
    }

    inline void RequireErrorCode(const auto &result, const std::string &code) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == code);
    }

    [[nodiscard]] inline EditorSurfaceProviderKey Provider() {
        return EditorSurfaceProviderKey{"com.example.tools", "com.example.tools.editor"};
    }

    [[nodiscard]] inline const EditorSurfaceSnapshot *FindSnapshot(const std::vector<EditorSurfaceSnapshot> &snapshots,
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

    [[nodiscard]] inline PersistenceSurfaces RegisterPersistenceSurfaces(EditorSurfaceRegistry &registry,
                                                                         EditorSurfaceContextProvider &provider,
                                                                         ExtensionCapabilityAdmission &admission) {
        return {
            .workspace = RegisterSurface(registry, provider, admission,
                                         ContextDescriptor("com.example.tools.workspace", EditorSurfaceKind::Tab,
                                                           EditorSurfacePersistence::Workspace)),
            .session =
                RegisterSurface(registry, provider, admission,
                                ContextDescriptor("com.example.tools.session", EditorSurfaceKind::Tab, EditorSurfacePersistence::Session)),
            .project =
                RegisterSurface(registry, provider, admission,
                                ContextDescriptor("com.example.tools.project", EditorSurfaceKind::Tab, EditorSurfacePersistence::Project)),
        };
    }

    inline void OpenWithOpaqueState(EditorSurfaceRegistry &registry, const std::string_view id, const std::uint8_t value) {
        REQUIRE(registry.Open(id).HasValue());
        const std::array<std::uint8_t, 1> state{value};
        REQUIRE(registry.SetOpaqueState(id, state).HasValue());
    }
}  // namespace Horo::Extensions::Tests::RegistrySupport
