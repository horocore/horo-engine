#pragma once

/**
 * @file EditorViewportScene.h
 * @brief Editor-private generic mesh resource views and instances consumed by viewport adapters.
 */

#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Render/Mesh.h"
#include "Horo/Runtime/Render/RenderScene.h"
#include "editor/project_model/EditorViewportCamera.h"

#include <optional>
#include <span>

namespace Horo::Editor {
    /** @brief Resolution shared by the baseline editor directional-shadow implementations. */
    inline constexpr std::uint32_t EditorViewportDirectionalShadowMapResolution = 2048;

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

        /** @brief Reports whether the camera and every instance contain supported finite values. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One bounded directional-light shadow view shared by concrete editor viewport adapters. */
    struct EditorViewportDirectionalShadowView {
        Math::Mat4 viewProjection{Math::Mat4::Identity()};
        std::size_t lightIndex{};

        /** @brief Reports whether the selected light index and matrix are valid for the source scene. */
        [[nodiscard]] bool IsValid(const Render::RenderSceneView &scene) const noexcept;
    };

    /** @brief One visible world point projected to top-left-origin normalized viewport coordinates. */
    struct EditorViewportPointProjection {
        Math::Vec2 viewportPosition{};
        float ndcDepth{0.0F};
    };

    /** @brief Builds one generic render-camera MVP using Horo scene conventions. */
    [[nodiscard]] Result<Math::Mat4> BuildRenderMvp(const Render::RenderCameraView &camera, const Math::Mat4 &localToWorld, float aspect,
                                                    Math::ClipDepthRange depthRange) noexcept;

    /**
     * @brief Fits one stable orthographic shadow camera to the submitted static-mesh bounds.
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

    /** @brief Builds a world-space ray from top-left-origin normalized viewport coordinates and an explicit clip-depth range. */
    [[nodiscard]] Result<Math::Ray> BuildEditorViewportRay(const EditorViewportCamera &camera, float normalizedX, float normalizedY,
                                                           float aspect, Math::ClipDepthRange depthRange) noexcept;

    /** @brief Converts editor camera state to the public backend-neutral render camera contract. */
    [[nodiscard]] Render::RenderCameraView ToRenderCamera(const EditorViewportCamera &camera) noexcept;
}  // namespace Horo::Editor
