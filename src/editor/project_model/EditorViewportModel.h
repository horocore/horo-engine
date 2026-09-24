#pragma once

/**
 * @file EditorViewportModel.h
 * @brief Editor-session authority for backend-neutral viewport camera navigation.
 */

#include "Horo/Editor/EditorDataBus.h"
#include "Horo/Foundation/Result.h"
#include "editor/document/SceneDocument.h"
#include "editor/project_model/EditorViewportCamera.h"

#include <optional>
#include <span>
#include <vector>

namespace Horo::Editor {
    /** @brief Monotonic viewport-state revision within one editor session. */
    struct ViewportRevision {
        std::uint64_t value{0};

        [[nodiscard]] constexpr auto operator<=>(const ViewportRevision &) const noexcept = default;
    };

    /** @brief Reason the authoritative viewport state changed. */
    enum class ViewportChangeKind : std::uint8_t {
        CameraMoved,
        CameraProjectionChanged,
        CameraFocused,
        ScenePreviewChanged,
    };

    /** @brief Notification emitted after viewport state commits. */
    struct ViewportChangedEvent {
        static constexpr auto HoroEventTypeName = "ViewportChangedEvent";

        ViewportRevision revision;
        ViewportChangeKind kind{ViewportChangeKind::CameraMoved};
    };

    /** @brief One frame of camera-relative editor navigation intent. */
    struct EditorViewportNavigationDelta {
        float yawRadians{0.0F};   /**< Rotation around scene up. */
        float pitchRadians{0.0F}; /**< Rotation around current camera right. */
        float moveRight{0.0F};    /**< Camera-local right translation in scene units. */
        float moveUp{0.0F};       /**< Camera-local up translation in scene units. */
        float moveForward{0.0F};  /**< Camera-local forward translation in scene units. */
        float dollyScale{1.0F};   /**< Multiplicative target distance or orthographic-height scale. */
        bool orbit{false};        /**< Keep the target fixed and rotate the camera position around it. */
    };

    /** @brief Immutable editor viewport state snapshot. */
    struct EditorViewportSnapshot {
        ViewportRevision revision;
        EditorViewportCamera camera;
        std::vector<SceneObjectTransformPreview> transformPreviews;
        std::optional<SceneObjectLightPreview> lightPreview;
    };

    /** @brief Owns and validates editor-session camera/navigation state. */
    class EditorViewportModel final {
    public:
        /**
         * @brief Creates a viewport authority with the default editor camera.
         * @param events Borrowed editor-session notification bus that outlives this model.
         */
        explicit EditorViewportModel(EditorDataBus &events) noexcept;

        /** @brief Returns the current immutable viewport snapshot. */
        [[nodiscard]] const EditorViewportSnapshot &Current() const noexcept;

        /**
         * @brief Applies one camera-relative navigation delta atomically.
         * @param delta Finite input delta already scaled by the viewport input mapper.
         * @return Success, or a typed validation error without changing the camera.
         */
        [[nodiscard]] Result<void> Navigate(const EditorViewportNavigationDelta &delta);

        /**
         * @brief Aligns the camera with one signed world axis while preserving target, distance, and projection.
         * @param axis Signed world-axis viewpoint selected by the viewport compass.
         * @return Success, or a validation error without changing camera state.
         */
        [[nodiscard]] Result<void> AlignToAxis(EditorViewportAxisView axis);

        /** @brief Changes editor projection while preserving apparent scale at the orbit target. */
        [[nodiscard]] Result<void> SetProjection(Runtime::CameraProjection projection);

        /** @brief Frames validated world bounds with a deterministic margin. */
        [[nodiscard]] Result<void> Focus(const Math::Aabb &worldBounds, float aspect);

        /**
         * @brief Commits one transient transform override to authoritative viewport workspace state.
         * @param preview Stable object identity and finite local transform to preview.
         * @return Success, or a typed validation error without changing preview state.
         */
        [[nodiscard]] Result<void> SetTransformPreview(const SceneObjectTransformPreview &preview);

        /**
         * @brief Commits transient transform overrides as one authoritative preview set.
         * @param previews Stable, unique object identities and finite local transforms.
         * @return Success, or a typed validation error without changing preview state.
         */
        [[nodiscard]] Result<void> SetTransformPreviews(std::span<const SceneObjectTransformPreview> previews);

        /**
         * @brief Clears all active transient transform overrides.
         * @return True when preview state changed and a notification was published.
         */
        bool ClearTransformPreview();

        /**
         * @brief Commits one transient Light-component override to viewport workspace state.
         * @param preview Stable object identity and validated authored Light component.
         * @return Success, or a typed validation error without changing preview state.
         */
        [[nodiscard]] Result<void> SetLightPreview(const SceneObjectLightPreview &preview);

        /**
         * @brief Clears the active transient Light-component override.
         * @return True when preview state changed and a notification was published.
         */
        bool ClearLightPreview();

    private:
        EditorDataBus *events_{nullptr};
        EditorViewportSnapshot current_{};
    };
}  // namespace Horo::Editor
