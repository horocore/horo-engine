#pragma once

/**
 * @file EditorSurfaceDescriptor.h
 * @brief Backend-neutral, host-owned descriptors for extension editor surfaces.
 */

#include "Horo/Extensions/ExtensionCapabilityAdmission.h"
#include "Horo/Foundation/EditorEventTypes.h"
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Extensions {
    /** @brief Stable event identity requested by an editor surface descriptor. */
    using EditorSurfaceEventId = Horo::EditorEventKind;

    /** @brief Extension contribution kinds that are hosted by an editor surface authority. */
    enum class EditorSurfaceKind : std::uint8_t {
        Panel,
        Tab,
        Modal,
        ModalPage,
        SettingsPage,
        Inspector,
        PropertyDrawer,
        ViewportOverlay,
        Gizmo,
        AssetPreview,
        StatusItem,
        ActivityItem,
        MenuItem,
        ToolbarAction,
    };

    /** @brief Host-owned placement authority requested by an editor surface descriptor. */
    enum class EditorSurfacePlacementKind : std::uint8_t {
        Workspace,
        Modal,
        Settings,
        Inspector,
        Viewport,
        StatusBar,
        ActivityBar,
        Menu,
        Toolbar,
    };

    /** @brief Durable state policy for a surface; trust and activation state never use this field. */
    enum class EditorSurfacePersistence : std::uint8_t {
        None,
        Session,
        Workspace,
        Project,
    };

    /** @brief Typed placement metadata interpreted by the owning editor host. */
    struct EditorSurfacePlacement final {
        EditorSurfacePlacementKind kind{EditorSurfacePlacementKind::Workspace};
        std::string target;
        std::int32_t order{};
    };

    /** @brief Exact module activation that owns a published surface descriptor. */
    struct EditorSurfaceProviderIdentity final {
        std::string extensionId;
        std::string moduleId;
        std::uint64_t activationGeneration{};

        bool operator==(const EditorSurfaceProviderIdentity &) const noexcept = default;
    };

    /** @brief Host-owned rail for an activity destination and its bound drawer. */
    enum class EditorActivitySide : std::uint8_t {
        Left,
        Right,
        Bottom
    };

    /** @brief Inert binding to a same-provider workspace panel; no activation or drawing is performed. */
    struct EditorActivityDestination final {
        EditorActivitySide side{EditorActivitySide::Left};
        std::uint8_t group{};     /**< Host activity group; bounded to the three workspace groups. */
        std::string drawerId;     /**< Stable panel identity owned by the same exact provider activation. */
        std::string iconResource; /**< Verified package-relative icon resource; never a renderer handle. */
    };

    /** @brief Bounded copied presentation state; badges never contain executable provider state. */
    struct EditorActivityPresentation final {
        bool visible{true};
        bool enabled{true};
        std::uint32_t badgeCount{};

        bool operator==(const EditorActivityPresentation &) const noexcept = default;
    };

    /** @brief Immutable, inert metadata for one extension-contributed editor surface. */
    struct EditorSurfaceDescriptor final {
        std::string id;
        EditorSurfaceKind kind{EditorSurfaceKind::Panel};
        std::string labelLocalizationKey;
        std::string tooltipLocalizationKey;
        EditorSurfacePlacement placement;
        EditorSurfacePersistence persistence{EditorSurfacePersistence::None};
        bool openByDefault{};
        std::vector<ExtensionCapabilityId> requiredCapabilities;
        std::vector<ExtensionPermissionId> requiredPermissions;
        EditorSurfaceProviderIdentity provider;
        std::vector<EditorSurfaceEventId> requestedEditorEvents;
        std::vector<EditorSurfaceEventId> requestedProcessEvents;
        std::optional<EditorActivityDestination> activity; /**< Required only for ActivityItem surfaces. */
    };

    /** @brief Bounds applied before a descriptor becomes visible to an editor host. */
    struct EditorSurfaceDescriptorLimits final {
        std::size_t maximumIdentityBytes{256};
        std::size_t maximumLocalizationKeyBytes{128};
        std::size_t maximumPlacementTargetBytes{256};
        std::size_t maximumCapabilities{32};
        std::size_t maximumPermissions{32};
        std::size_t maximumEditorEvents{16};
        std::size_t maximumProcessEvents{16};
    };

    /**
     * @brief Validates one complete surface descriptor without registering or activating it.
     * @param descriptor Inert metadata supplied by the extension composition boundary.
     * @param limits Host-owned resource bounds.
     * @return Success or a typed descriptor-rejected error; the input is never modified.
     * @post No editor host, service locator, callback, native handle, or global state is touched.
     */
    [[nodiscard]] Result<void> ValidateEditorSurfaceDescriptor(const EditorSurfaceDescriptor &descriptor,
                                                               const EditorSurfaceDescriptorLimits &limits = {});
}  // namespace Horo::Extensions
