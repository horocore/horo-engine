/**
 * @file EditorIcons.h
 * @brief Typed icon identities, token resolution, and drawing for the editor design system.
 */
#pragma once

#include <cstdint>
#include <imgui.h>
#include <optional>
#include <span>
#include <string_view>

namespace Horo::Editor::Ui {

    /** @brief Stable semantic identity for a built-in editor icon. */
    enum class UiIcon : std::uint8_t {
        None,
        Generic,
        Info,
        Warning,
        Error,
        Create,
        Rename,
        Duplicate,
        Delete,
        Reset,
        Check,
        CheckboxUnchecked,
        Settings,
        MoreVertical,
        Visibility,
        VisibilityOff,
        Lock,
        HierarchyGeneric,
        HierarchyMesh,
        Camera,
        AudioSource,
        Light,
        SpotLight,
        DirectionalLight,
        PointLight,
        Sphere,
        Capsule,
        Cylinder,
        Cone,
        Plane,
        Quad,
        TriggerVolume,
        ArrowBack,
        ArrowForward,
        ArrowUpward,
        Search,
        GridView,
        ViewList,
        CreateNewFolder,
        Favorite,
        History,
        Storage,
        Package,
        AccountTree,
        Tag,
        Folder,
        Image,
        AudioFile,
        Description,
        Pause,
        Download,
        Stop,
        Play,
        Record,
        VolumeOff,
        ClearAll,
        SceneObject,
        Pending,
        Success,
        Cancelled,
        Count,
    };

    /** @brief Immutable bridge between external icon tokens and typed editor icons. */
    class UiIconRegistry final {
    public:
        UiIconRegistry() = delete;

        /**
         * @brief Resolves a catalog or serialized icon token at an editor boundary.
         * @param token Stable external icon token.
         * @return The matching icon, or an empty optional when the token is unknown.
         */
        [[nodiscard]] static std::optional<UiIcon> Resolve(std::string_view token) noexcept;

        /**
         * @brief Returns the canonical token for a typed icon.
         * @param icon Typed icon identity.
         * @return Canonical token, or an empty view for @ref UiIcon::None and invalid values.
         */
        [[nodiscard]] static std::string_view Token(UiIcon icon) noexcept;

        /**
         * @brief Returns the null-terminated Material Symbols ranges required by the editor icon atlas.
         * @return ImGui-compatible inclusive glyph-range pairs followed by zero.
         */
        [[nodiscard]] static std::span<const ImWchar> MaterialSymbolGlyphRanges() noexcept;
    };

    /**
     * @brief Draws one typed editor icon into the supplied bounds.
     * @param drawList Target draw list; null is ignored.
     * @param icon Typed icon identity.
     * @param position Upper-left draw position.
     * @param size Available icon bounds.
     * @param color Packed icon color.
     * @param iconFont Optional centralized Material Symbols font; mapped symbols use it instead of fallback geometry.
     */
    void DrawEditorIcon(ImDrawList *drawList, UiIcon icon, ImVec2 position, ImVec2 size, ImU32 color, ImFont *iconFont = nullptr);

}  // namespace Horo::Editor::Ui
