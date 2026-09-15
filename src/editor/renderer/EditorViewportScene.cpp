/** @copydoc EditorViewportScene.h */

#include "editor/renderer/EditorViewportScene.h"

#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <span>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] bool ValidateMeshResource(const EditorViewportMeshResourceView &resource,
                                                const std::span<const EditorViewportMeshResourceView> allResources,
                                                const std::size_t index) noexcept {
            if (!resource.IsValid())
                return false;
            for (std::size_t other = index + 1; other < allResources.size(); ++other)
                if (allResources[other].handle == resource.handle)
                    return false;
            if (!std::ranges::all_of(resource.vertices, [](const Render::MeshVertex &vertex) {
                return Math::IsFinite(vertex.position) && Math::IsFinite(vertex.normal) && Math::IsFinite(vertex.uv);
            })) {
                return false;
            }
            return std::ranges::all_of(resource.indices, [&](const std::uint32_t vertexIndex) {
                return vertexIndex < resource.vertices.size();
            });
        }
    }  // namespace

    /** @copydoc EditorViewportSceneView::IsValid */
    bool EditorViewportSceneView::IsValid() const noexcept {
        if (!camera.IsValid()) {
            return false;
        }
        for (std::size_t index = 0; index < meshResources.size(); ++index) {
            if (!ValidateMeshResource(meshResources[index], meshResources, index))
                return false;
        }
        for (const EditorViewportInstance &instance : instances) {
            if (!instance.IsValid())
                return false;
            if (std::ranges::find(meshResources, instance.mesh, &EditorViewportMeshResourceView::handle) == meshResources.end())
                return false;
        }
        if (lights.size() > Render::MaximumForwardLights)
            return false;
        return std::ranges::all_of(lights, &Render::RenderLight::IsValid);
    }

    /** @copydoc EditorViewportDirectionalShadowView::IsValid */
    bool EditorViewportDirectionalShadowView::IsValid(const Render::RenderSceneView &scene) const noexcept {
        return lightIndex < scene.lights.size() && scene.lights[lightIndex].kind == Render::RenderLightKind::Directional &&
               Math::IsFinite(viewProjection);
    }

    /** @copydoc BuildRenderMvp */
    Result<Math::Mat4> BuildRenderMvp(const Render::RenderCameraView &camera, const Math::Mat4 &localToWorld, const float aspect,
                                      const Math::ClipDepthRange depthRange) noexcept {
        if (!camera.IsValid() || !Math::IsFinite(localToWorld) || !std::isfinite(aspect) || aspect <= 0.0F) {
            return Result<Math::Mat4>::Failure(
                MakeError(RendererErrors::InvalidRenderCamera, "Render camera, transform, or aspect ratio is invalid."));
        }
        const Result<Math::Mat4> view = Math::TryLookAt(camera.position, camera.target, camera.up);
        if (view.HasError())
            return Result<Math::Mat4>::Failure(view.ErrorValue());
        const Result<Math::Mat4> projection =
            camera.projection.kind == Render::RenderProjectionKind::Perspective
                ? Math::TryPerspective(camera.projection.verticalFovRadians, aspect, camera.projection.nearPlane,
                                       camera.projection.farPlane, depthRange)
                : Math::TryOrthographic(camera.projection.orthographicHeight, aspect, camera.projection.nearPlane,
                                        camera.projection.farPlane, depthRange);
        if (projection.HasError())
            return Result<Math::Mat4>::Failure(projection.ErrorValue());
        return Result<Math::Mat4>::Success(Math::Multiply(Math::Multiply(projection.Value(), view.Value()), localToWorld));
    }

    /** @copydoc BuildEditorViewportDirectionalShadowView */
    Result<std::optional<EditorViewportDirectionalShadowView>> BuildEditorViewportDirectionalShadowView(
        const Render::RenderSceneView &scene, const Math::ClipDepthRange depthRange) noexcept {
        if (!scene.IsValid()) {
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(
                MakeError(RendererErrors::ViewportInvalidScene, "Cannot fit a directional shadow view to an invalid render scene."));
        }
        const auto light = std::ranges::find_if(scene.lights, [](const Render::RenderLight &candidate) {
            return candidate.kind == Render::RenderLightKind::Directional && candidate.intensity > Math::DefaultEpsilon;
        });
        if (light == scene.lights.end() || scene.instances.empty())
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Success(std::nullopt);

        Math::Aabb worldBounds{
            .minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
            .maximum = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()},
        };
        for (const Render::RenderStaticMeshInstance &instance : scene.instances) {
            const Result<Math::Aabb> transformed = Math::TransformAabb(instance.localBounds, instance.localToWorld);
            if (transformed.HasError())
                return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(transformed.ErrorValue());
            worldBounds.minimum = {
                std::min(worldBounds.minimum.x, transformed.Value().minimum.x),
                std::min(worldBounds.minimum.y, transformed.Value().minimum.y),
                std::min(worldBounds.minimum.z, transformed.Value().minimum.z),
            };
            worldBounds.maximum = {
                std::max(worldBounds.maximum.x, transformed.Value().maximum.x),
                std::max(worldBounds.maximum.y, transformed.Value().maximum.y),
                std::max(worldBounds.maximum.z, transformed.Value().maximum.z),
            };
        }

        const Result<Math::BoundingSphere> worldSphere = Math::SphereFromAabb(worldBounds);
        if (worldSphere.HasError())
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(worldSphere.ErrorValue());
        Math::Vec3 center = worldSphere.Value().center;
        const float radius = std::max(worldSphere.Value().radius, 1.0F);
        const double paddedRadiusValue = static_cast<double>(radius) * 1.15;
        if (!std::isfinite(paddedRadiusValue) || paddedRadiusValue > std::numeric_limits<float>::max())
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(
                MakeError(RendererErrors::InvalidCoordinates, "Directional shadow bounds are not representable."));
        const auto paddedRadius = static_cast<float>(paddedRadiusValue);
        const Result<Math::Vec3> direction = Math::TryNormalize(light->direction);
        if (direction.HasError())
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(direction.ErrorValue());
        const Math::Vec3 up = std::abs(Math::Dot(direction.Value(), Math::Vec3{0.0F, 1.0F, 0.0F})) > 0.95F ? Math::Vec3{1.0F, 0.0F, 0.0F}
                                                                                                           : Math::Vec3{0.0F, 1.0F, 0.0F};
        const Result<Math::Vec3> side = Math::TryNormalize(Math::Cross(direction.Value(), up));
        if (side.HasError())
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(side.ErrorValue());
        const Math::Vec3 lightUp = Math::Cross(side.Value(), direction.Value());
        const float worldUnitsPerTexel = (paddedRadius * 2.0F) / static_cast<float>(EditorViewportDirectionalShadowMapResolution);
        const auto snapToTexel = [worldUnitsPerTexel](const float coordinate) {
            return std::round(coordinate / worldUnitsPerTexel) * worldUnitsPerTexel;
        };
        center += side.Value() * (snapToTexel(Math::Dot(center, side.Value())) - Math::Dot(center, side.Value()));
        center += lightUp * (snapToTexel(Math::Dot(center, lightUp)) - Math::Dot(center, lightUp));
        const Math::Vec3 eye = center - direction.Value() * (paddedRadius * 2.0F);
        const Result<Math::Mat4> view = Math::TryLookAt(eye, center, lightUp);
        if (view.HasError())
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(view.ErrorValue());
        const Result<Math::Mat4> projection = Math::TryOrthographic(paddedRadius * 2.0F, 1.0F, 0.1F, paddedRadius * 4.0F, depthRange);
        if (projection.HasError())
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(projection.ErrorValue());

        EditorViewportDirectionalShadowView shadow{
            .viewProjection = Math::Multiply(projection.Value(), view.Value()),
            .lightIndex = static_cast<std::size_t>(std::distance(scene.lights.begin(), light)),
        };
        if (!shadow.IsValid(scene)) {
            return Result<std::optional<EditorViewportDirectionalShadowView>>::Failure(
                MakeError(RendererErrors::ViewportInvalidScene, "Directional shadow view fitting produced an invalid result."));
        }
        return Result<std::optional<EditorViewportDirectionalShadowView>>::Success(shadow);
    }

    /** @copydoc BuildEditorViewportViewProjection */
    Result<Math::Mat4> BuildEditorViewportViewProjection(const EditorViewportCamera &camera, const float aspect,
                                                         const Math::ClipDepthRange depthRange) noexcept {
        if (!camera.IsValid() || !std::isfinite(aspect) || aspect <= 0.0F) {
            return Result<Math::Mat4>::Failure(MakeError(RendererErrors::InvalidCamera, "Viewport camera or aspect ratio is invalid."));
        }
        const Result<Math::Mat4> view = Math::TryLookAt(camera.position, camera.target, camera.up);
        if (view.HasError())
            return Result<Math::Mat4>::Failure(view.ErrorValue());
        const Result<Math::Mat4> projection =
            camera.projection == Runtime::CameraProjection::Perspective
                ? Math::TryPerspective(camera.verticalFovRadians, aspect, camera.nearPlane, camera.farPlane, depthRange)
                : Math::TryOrthographic(camera.orthographicHeight, aspect, camera.nearPlane, camera.farPlane, depthRange);
        if (projection.HasError())
            return Result<Math::Mat4>::Failure(projection.ErrorValue());
        return Result<Math::Mat4>::Success(Math::Multiply(projection.Value(), view.Value()));
    }

    /** @copydoc ProjectEditorViewportPoint */
    Result<std::optional<EditorViewportPointProjection>> ProjectEditorViewportPoint(const EditorViewportCamera &camera,
                                                                                    const Math::Vec3 worldPoint, const float aspect,
                                                                                    const Math::ClipDepthRange depthRange) noexcept {
        const Result<Math::Mat4> viewProjection = BuildEditorViewportViewProjection(camera, aspect, depthRange);
        if (viewProjection.HasError())
            return Result<std::optional<EditorViewportPointProjection>>::Failure(viewProjection.ErrorValue());
        if (!Math::IsFinite(worldPoint))
            return Result<std::optional<EditorViewportPointProjection>>::Failure(
                MakeError(RendererErrors::InvalidCoordinates, "Viewport projection point must be finite."));
        if (const Math::Vec4 clip = Math::TransformHomogeneous(viewProjection.Value(), {worldPoint.x, worldPoint.y, worldPoint.z, 1.0F});
            Math::IsFinite(clip) && clip.w <= Math::DefaultEpsilon) {
            return Result<std::optional<EditorViewportPointProjection>>::Success(std::nullopt);
        }
        const Result<Math::Vec3> projected = Math::TryProject(viewProjection.Value(), worldPoint);
        if (projected.HasError())
            return Result<std::optional<EditorViewportPointProjection>>::Failure(projected.ErrorValue());
        if (const float minimumDepth = depthRange == Math::ClipDepthRange::NegativeOneToOne ? -1.0F : 0.0F;
            projected.Value().z < minimumDepth || projected.Value().z > 1.0F) {
            return Result<std::optional<EditorViewportPointProjection>>::Success(std::nullopt);
        }
        return Result<std::optional<EditorViewportPointProjection>>::Success(EditorViewportPointProjection{
            .viewportPosition = {projected.Value().x * 0.5F + 0.5F, 0.5F - projected.Value().y * 0.5F},
            .ndcDepth = projected.Value().z,
        });
    }

    /** @copydoc BuildEditorViewportRay */
    Result<Math::Ray> BuildEditorViewportRay(const EditorViewportCamera &camera, const float normalizedX, const float normalizedY,
                                             const float aspect, const Math::ClipDepthRange depthRange) noexcept {
        if (!std::isfinite(normalizedX) || !std::isfinite(normalizedY) || normalizedX < 0.0F || normalizedX > 1.0F || normalizedY < 0.0F ||
            normalizedY > 1.0F) {
            return Result<Math::Ray>::Failure(
                MakeError(RendererErrors::InvalidCoordinates, "Viewport coordinates are outside normalized bounds."));
        }
        const Result<Math::Mat4> viewProjection = BuildEditorViewportViewProjection(camera, aspect, depthRange);
        if (viewProjection.HasError())
            return Result<Math::Ray>::Failure(viewProjection.ErrorValue());
        const Result<Math::Mat4> inverse = Math::TryInverse(viewProjection.Value());
        if (inverse.HasError())
            return Result<Math::Ray>::Failure(inverse.ErrorValue());
        const Math::Vec2 ndc{normalizedX * 2.0F - 1.0F, 1.0F - normalizedY * 2.0F};
        const float nearDepth = depthRange == Math::ClipDepthRange::NegativeOneToOne ? -1.0F : 0.0F;
        const Result<Math::Vec3> nearPoint = Math::TryUnproject(inverse.Value(), {ndc.x, ndc.y, nearDepth});
        const Result<Math::Vec3> farPoint = Math::TryUnproject(inverse.Value(), {ndc.x, ndc.y, 1.0F});
        if (nearPoint.HasError())
            return Result<Math::Ray>::Failure(nearPoint.ErrorValue());
        if (farPoint.HasError())
            return Result<Math::Ray>::Failure(farPoint.ErrorValue());
        if (camera.projection == Runtime::CameraProjection::Perspective)
            return Math::TryMakeRay(camera.position, farPoint.Value() - camera.position, camera.nearPlane, camera.farPlane);
        return Math::TryMakeRay(nearPoint.Value(), farPoint.Value() - nearPoint.Value(), 0.0F, camera.farPlane - camera.nearPlane);
    }

    Render::RenderCameraView ToRenderCamera(const EditorViewportCamera &camera) noexcept {
        return Render::RenderCameraView{
            .position = camera.position,
            .target = camera.target,
            .up = camera.up,
            .projection =
                Render::RenderProjectionDescriptor{
                    .kind = camera.projection == Runtime::CameraProjection::Perspective ? Render::RenderProjectionKind::Perspective
                                                                                        : Render::RenderProjectionKind::Orthographic,
                    .verticalFovRadians = camera.verticalFovRadians,
                    .orthographicHeight = camera.orthographicHeight,
                    .nearPlane = camera.nearPlane,
                    .farPlane = camera.farPlane,
                },
        };
    }
}  // namespace Horo::Editor
