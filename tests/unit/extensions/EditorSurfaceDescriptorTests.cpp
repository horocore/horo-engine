#include "Horo/Extensions/EditorSurfaceDescriptor.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Extensions::Tests {
    namespace {
        EditorSurfaceDescriptor ValidDescriptor() {
            return EditorSurfaceDescriptor{
                .id = "com.example.tools.inspector",
                .kind = EditorSurfaceKind::Tab,
                .labelLocalizationKey = "editor.tools.inspector.label",
                .tooltipLocalizationKey = "editor.tools.inspector.tooltip",
                .placement = EditorSurfacePlacement{EditorSurfacePlacementKind::Workspace, "bottom.tools", 20},
                .persistence = EditorSurfacePersistence::Workspace,
                .openByDefault = false,
                .requiredCapabilities = {{"editor.asset_query"}},
                .requiredPermissions = {{"editor.workspace.read"}},
                .provider = EditorSurfaceProviderIdentity{"com.example.tools", "com.example.tools.editor", 4},
            };
        }
    }  // namespace

    TEST_CASE("Editor surface descriptors validate typed ownership and placement", "[Extensions][EditorSurface]") {
        const auto descriptor = ValidDescriptor();
        const auto result = ValidateEditorSurfaceDescriptor(descriptor);
        REQUIRE(result.HasValue());
    }

    TEST_CASE("Editor surface descriptors reject incompatible placement and persistence", "[Extensions][EditorSurface]") {
        auto descriptor = ValidDescriptor();
        descriptor.placement.kind = EditorSurfacePlacementKind::Menu;
        auto result = ValidateEditorSurfaceDescriptor(descriptor);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "editor_surface_descriptor_invalid");

        descriptor = ValidDescriptor();
        descriptor.kind = EditorSurfaceKind::MenuItem;
        descriptor.placement = EditorSurfacePlacement{EditorSurfacePlacementKind::Menu, {}, 0};
        descriptor.persistence = EditorSurfacePersistence::Workspace;
        result = ValidateEditorSurfaceDescriptor(descriptor);
        REQUIRE(result.HasError());
    }

    TEST_CASE("Editor surface descriptors reject malformed identities and duplicate authority", "[Extensions][EditorSurface]") {
        auto descriptor = ValidDescriptor();
        descriptor.provider.activationGeneration = 0;
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor).HasError());

        descriptor = ValidDescriptor();
        descriptor.requiredCapabilities.push_back({"editor.asset_query"});
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor).HasError());

        descriptor = ValidDescriptor();
        descriptor.id = "Com.Example.Invalid";
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor).HasError());
    }

    TEST_CASE("Editor surface descriptors enforce bounded collections", "[Extensions][EditorSurface]") {
        auto descriptor = ValidDescriptor();
        EditorSurfaceDescriptorLimits limits;
        limits.maximumCapabilities = 1;
        descriptor.requiredCapabilities.push_back({"editor.log.read"});
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor, limits).HasError());

        descriptor = ValidDescriptor();
        descriptor.openByDefault = true;
        descriptor.persistence = EditorSurfacePersistence::None;
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor).HasError());

        descriptor = ValidDescriptor();
        descriptor.requiredCapabilities.clear();
        descriptor.requiredPermissions.clear();
        limits.maximumCapabilities = 0;
        limits.maximumPermissions = 0;
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor, limits).HasValue());
    }

    TEST_CASE("Editor surface descriptors validate separated event requests", "[Extensions][EditorSurface][DataBus]") {
        auto descriptor = ValidDescriptor();
        descriptor.requestedEditorEvents = {Horo::EditorEventKind::AssetImported, Horo::EditorEventKind::MetricsChanged};
        descriptor.requestedProcessEvents = {Horo::EditorEventKind::ProjectOpened};
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor).HasValue());

        descriptor.requestedEditorEvents.push_back(Horo::EditorEventKind::AssetImported);
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor).HasError());

        descriptor = ValidDescriptor();
        descriptor.requestedEditorEvents = {Horo::EditorEventKind::AssetImported};
        descriptor.requestedProcessEvents = {Horo::EditorEventKind::AssetImported};
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor).HasError());

        descriptor = ValidDescriptor();
        descriptor.requestedProcessEvents = {static_cast<Horo::EditorEventKind>(255)};
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor).HasError());

        descriptor = ValidDescriptor();
        EditorSurfaceDescriptorLimits limits;
        limits.maximumEditorEvents = 1;
        descriptor.requestedEditorEvents = {Horo::EditorEventKind::AssetImported, Horo::EditorEventKind::MetricsChanged};
        REQUIRE(ValidateEditorSurfaceDescriptor(descriptor, limits).HasError());
    }

    TEST_CASE("Editor surface descriptors accept every typed placement mapping", "[Extensions][EditorSurface]") {
        struct PlacementCase final {
            EditorSurfaceKind kind;
            EditorSurfacePlacementKind placement;
            EditorSurfacePersistence persistence;
        };

        constexpr std::array cases{
            PlacementCase{EditorSurfaceKind::Panel, EditorSurfacePlacementKind::Workspace, EditorSurfacePersistence::Workspace},
            PlacementCase{EditorSurfaceKind::Tab, EditorSurfacePlacementKind::Workspace, EditorSurfacePersistence::Workspace},
            PlacementCase{EditorSurfaceKind::Modal, EditorSurfacePlacementKind::Modal, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::ModalPage, EditorSurfacePlacementKind::Modal, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::SettingsPage, EditorSurfacePlacementKind::Settings, EditorSurfacePersistence::Project},
            PlacementCase{EditorSurfaceKind::Inspector, EditorSurfacePlacementKind::Inspector, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::PropertyDrawer, EditorSurfacePlacementKind::Inspector, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::ViewportOverlay, EditorSurfacePlacementKind::Viewport, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::Gizmo, EditorSurfacePlacementKind::Viewport, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::AssetPreview, EditorSurfacePlacementKind::Viewport, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::StatusItem, EditorSurfacePlacementKind::StatusBar, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::ActivityItem, EditorSurfacePlacementKind::ActivityBar, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::MenuItem, EditorSurfacePlacementKind::Menu, EditorSurfacePersistence::None},
            PlacementCase{EditorSurfaceKind::ToolbarAction, EditorSurfacePlacementKind::Toolbar, EditorSurfacePersistence::None},
        };
        for (const auto &test : cases) {
            auto descriptor = ValidDescriptor();
            descriptor.kind = test.kind;
            descriptor.placement.kind = test.placement;
            descriptor.persistence = test.persistence;
            CHECK(ValidateEditorSurfaceDescriptor(descriptor).HasValue());
        }
    }
}  // namespace Horo::Extensions::Tests
