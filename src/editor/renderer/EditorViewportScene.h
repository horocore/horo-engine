#pragma once

/**
 * @file EditorViewportScene.h
 * @brief Editor-private generic mesh resource views and instances consumed by viewport adapters.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Render/DirectionalShadow.h"
#include "Horo/Runtime/Render/Mesh.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "editor/project_model/EditorViewportCamera.h"

#include <optional>
#include <span>

namespace Horo::Editor {
    /** @brief Non-owning immutable CPU mesh resource pinned by the owning extracted snapshot. */
    using EditorViewportMeshResourceView = Render::RenderMeshResourceView;

    /** @brief One backend-neutral renderable instance in the editor viewport scene. */
    using EditorViewportInstance = Render::RenderStaticMeshInstance;

    /** @brief Non-owning immutable editor scene view consumed synchronously by Render. */
    struct EditorViewportSceneView {
        EditorViewportCamera camera{};
        std::span<const EditorViewportMeshResourceView> meshResources{};
        std::span<const EditorViewportInstance> instances{};
        std::span<const Render::RenderLight> lights{};
        std::uint64_t lightSceneDomain{};
        std::uint64_t lightSnapshotRevision{};

        /** @brief Reports whether the camera and every instance contain supported finite values. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Computes a bounded semantic revision from exact immutable light values and producer identities.
     * @param lights Complete scene table, not a per-draw selected subset.
     * @return Nonzero content signature, or zero if the complete-table bound is exceeded. No allocation or retained input. */
    [[nodiscard]] std::uint64_t ComputeEditorViewportLightRevision(std::span<const Render::RenderLight> lights) noexcept;

    /** @brief Shared canonical fixed-capacity shadow frame consumed by concrete viewport adapters. */
    using EditorViewportDirectionalShadowView = Render::DirectionalShadowFrame;

    /** @brief One visible world point projected to top-left-origin normalized viewport coordinates. */
    struct EditorViewportPointProjection {
        Math::Vec2 viewportPosition{};
        float ndcDepth{0.0F};
    };

    /** @brief Builds one generic render-camera MVP using Horo scene conventions. */
    [[nodiscard]] Result<Math::Mat4> BuildRenderMvp(const Render::RenderCameraView &camera, const Math::Mat4 &localToWorld, float aspect,
                                                    Math::ClipDepthRange depthRange) noexcept;

    /**
     * @brief Builds the canonical single-cascade compatibility profile with a square viewport aspect.
     * @param scene Valid immutable render scene.
     * @param depthRange Clip-depth convention required by the consuming backend.
     * @return Empty when no directional light or mesh is present; otherwise a validated shadow view.
     */
    [[nodiscard]] Result<std::optional<EditorViewportDirectionalShadowView>> BuildEditorViewportDirectionalShadowView(
        const Render::RenderSceneView &scene, Math::ClipDepthRange depthRange) noexcept;

    /** @brief Builds the validated view-projection matrix for one editor camera. */
    [[nodiscard]] Result<Math::Mat4> BuildEditorViewportViewProjection(const EditorViewportCamera &camera, float aspect,
                                                                       Math::ClipDepthRange depthRange) noexcept;

    /**
     * @brief Projects a world point through the canonical editor camera contract.
     * @param camera Valid editor viewport camera.
     * @param worldPoint Finite point in scene space.
     * @param aspect Positive finite viewport width-to-height ratio.
     * @param depthRange Explicit consuming-backend clip-depth convention.
     * @return A visible top-left-origin normalized projection, empty when behind the view origin or outside the depth interval, or a typed
     * math failure.
     */
    [[nodiscard]] Result<std::optional<EditorViewportPointProjection>> ProjectEditorViewportPoint(const EditorViewportCamera &camera,
                                                                                                  Math::Vec3 worldPoint, float aspect,
                                                                                                  Math::ClipDepthRange depthRange) noexcept;

    /**
     * @brief Maps a normalized viewport projection to finite top-left-origin pixel coordinates.
     * @param projection Finite normalized viewport projection.
     * @param origin Finite top-left viewport origin in pixels.
     * @param extent Positive finite viewport extent in pixels.
     * @return Mapped pixel coordinates or a typed coordinate failure.
     */
    [[nodiscard]] Result<Math::Vec2> MapEditorViewportPointToPixels(const EditorViewportPointProjection &projection, Math::Vec2 origin,
                                                                    Math::Vec2 extent) noexcept;

    /** @brief Builds a world-space ray from top-left-origin normalized viewport coordinates and an explicit clip-depth range. */
    [[nodiscard]] Result<Math::Ray> BuildEditorViewportRay(const EditorViewportCamera &camera, float normalizedX, float normalizedY,
                                                           float aspect, Math::ClipDepthRange depthRange) noexcept;

    /** @brief Converts editor camera state to the public backend-neutral render camera contract. */
    [[nodiscard]] Render::RenderCameraView ToRenderCamera(const EditorViewportCamera &camera) noexcept;
}  // namespace Horo::Editor
