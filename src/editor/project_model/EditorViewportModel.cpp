/** @copydoc EditorViewportModel.h */

#include "editor/project_model/EditorViewportModel.h"

#include "EditorModelErrors.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
        constexpr float CameraVerticalLimitThreshold = 0.995F;

        [[nodiscard]] Result<Math::Vec3> RotateAroundAxis(const Math::Vec3 value, const Math::Vec3 axis, const float radians) noexcept {
            const Result<Math::Quaternion> rotation = Math::Quaternion::TryFromAxisAngle(axis, radians);
            if (rotation.HasError())
                return Result<Math::Vec3>::Failure(rotation.ErrorValue());
            return rotation.Value().TryRotate(value);
        }

        struct NavigationBasis {
            Math::Vec3 forward{};
            Math::Vec3 right{};
            Math::Vec3 up{};
        };

        [[nodiscard]] Result<Math::Vec3> TryCameraForward(const EditorViewportCamera &camera) noexcept {
            return Math::TryNormalize(camera.target - camera.position);
        }

        [[nodiscard]] Result<NavigationBasis> TryNavigationBasis(const EditorViewportCamera &camera,
                                                                 const EditorViewportNavigationDelta &delta) noexcept {
            constexpr Math::Vec3 sceneUp{0.0F, 1.0F, 0.0F};
            Result<Math::Vec3> forward = TryCameraForward(camera);
            if (forward.HasError())
                return Result<NavigationBasis>::Failure(forward.ErrorValue());
            forward = RotateAroundAxis(forward.Value(), sceneUp, delta.yawRadians);
            if (forward.HasError())
                return Result<NavigationBasis>::Failure(forward.ErrorValue());
            const float initialVerticality = std::fabs(Math::Dot(forward.Value(), sceneUp));
            const Math::Vec3 referenceUp = initialVerticality >= CameraVerticalLimitThreshold ? camera.up : sceneUp;
            Result<Math::Vec3> right = Math::TryNormalize(Math::Cross(forward.Value(), referenceUp));
            if (right.HasError())
                return Result<NavigationBasis>::Failure(right.ErrorValue());
            const Result<Math::Vec3> pitched = RotateAroundAxis(forward.Value(), right.Value(), delta.pitchRadians);
            if (pitched.HasError())
                return Result<NavigationBasis>::Failure(pitched.ErrorValue());
            if (const float nextVerticality = std::fabs(Math::Dot(pitched.Value(), sceneUp));
                nextVerticality < CameraVerticalLimitThreshold || nextVerticality < initialVerticality)
                forward = pitched;
            const Math::Vec3 finalUp = std::fabs(Math::Dot(forward.Value(), sceneUp)) >= CameraVerticalLimitThreshold ? camera.up : sceneUp;
            right = Math::TryNormalize(Math::Cross(forward.Value(), finalUp));
            if (right.HasError())
                return Result<NavigationBasis>::Failure(right.ErrorValue());
            const Result<Math::Vec3> up = Math::TryNormalize(Math::Cross(right.Value(), forward.Value()));
            if (up.HasError())
                return Result<NavigationBasis>::Failure(up.ErrorValue());
            return Result<NavigationBasis>::Success({forward.Value(), right.Value(), up.Value()});
        }

        [[nodiscard]] bool IsFinite(const EditorViewportNavigationDelta &delta) noexcept {
            return std::isfinite(delta.yawRadians) && std::isfinite(delta.pitchRadians) && std::isfinite(delta.moveRight) &&
                   std::isfinite(delta.moveUp) && std::isfinite(delta.moveForward) && std::isfinite(delta.dollyScale) &&
                   delta.dollyScale > 0.0F;
        }

        [[nodiscard]] bool IsEmpty(const EditorViewportNavigationDelta &delta) noexcept {
            return delta.yawRadians == 0.0F && delta.pitchRadians == 0.0F && delta.moveRight == 0.0F && delta.moveUp == 0.0F &&
                   delta.moveForward == 0.0F && delta.dollyScale == 1.0F;
        }

        [[nodiscard]] bool IsValid(const SceneObjectTransformPreview &preview) noexcept {
            const Math::Quaternion rotation = preview.localTransform.rotation;
            const float rotationLengthSquared =
                rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w;
            return preview.object.IsValid() && Math::IsFinite(preview.localTransform.translation) &&
                   Math::IsFinite(preview.localTransform.scale) && std::isfinite(rotation.x) && std::isfinite(rotation.y) &&
                   std::isfinite(rotation.z) && std::isfinite(rotation.w) && rotationLengthSquared > 0.0F;
        }

        [[nodiscard]] Error MakeViewportError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }
    }  // namespace

    /** @copydoc EditorViewportModel::EditorViewportModel */
    EditorViewportModel::EditorViewportModel(EditorDataBus &events) noexcept : events_(&events) {}

    /** @copydoc EditorViewportModel::Current */
    const EditorViewportSnapshot &EditorViewportModel::Current() const noexcept {
        return current_;
    }

    /** @copydoc EditorViewportModel::Navigate */
    Result<void> EditorViewportModel::Navigate(const EditorViewportNavigationDelta &delta) {
        if (!IsFinite(delta)) {
            return Result<void>::Failure(
                MakeViewportError(ViewportModelErrors::InvalidNavigation, "Viewport navigation delta must be finite."));
        }
        if (IsEmpty(delta)) {
            return Result<void>::Success();
        }

        EditorViewportCamera camera = current_.camera;
        if (!camera.IsValid())
            return Result<void>::Failure(MakeViewportError(ViewportModelErrors::InvalidCamera, "Viewport camera is invalid."));
        const float targetDistance = Math::Length(camera.target - camera.position);
        const Result<NavigationBasis> basis = TryNavigationBasis(camera, delta);
        if (basis.HasError())
            return Result<void>::Failure(basis.ErrorValue());
        const auto &[forward, right, localUp] = basis.Value();

        if (delta.orbit) {
            camera.position = camera.target - forward * targetDistance;
        } else {
            camera.target = camera.position + forward * targetDistance;
        }

        const Math::Vec3 translation = right * delta.moveRight + localUp * delta.moveUp + forward * delta.moveForward;
        camera.position += translation;
        camera.target += translation;
        if (delta.dollyScale != 1.0F) {
            if (camera.projection == Runtime::CameraProjection::Perspective) {
                const float distance = std::clamp(targetDistance * delta.dollyScale, camera.nearPlane * 2.0F, camera.farPlane * 0.8F);
                camera.position = camera.target - forward * distance;
            } else {
                camera.orthographicHeight = std::clamp(camera.orthographicHeight * delta.dollyScale, 0.01F, 100000.0F);
            }
        }
        camera.up = localUp;
        if (!camera.IsValid()) {
            return Result<void>::Failure(
                MakeViewportError(ViewportModelErrors::InvalidCamera, "Viewport navigation produced an invalid camera."));
        }

        current_.camera = camera;
        ++current_.revision.value;
        events_->Publish(ViewportChangedEvent{current_.revision, ViewportChangeKind::CameraMoved});
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportModel::AlignToAxis */
    Result<void> EditorViewportModel::AlignToAxis(const EditorViewportAxisView axis) {
        const EditorViewportCamera &currentCamera = current_.camera;
        if (!currentCamera.IsValid())
            return Result<void>::Failure(MakeViewportError(ViewportModelErrors::InvalidCamera, "Viewport camera is invalid."));
        Math::Vec3 direction;
        Math::Vec3 up{0.0F, 1.0F, 0.0F};
        switch (axis) {
            case EditorViewportAxisView::PositiveX:
                direction = {1.0F, 0.0F, 0.0F};
                break;
            case EditorViewportAxisView::NegativeX:
                direction = {-1.0F, 0.0F, 0.0F};
                break;
            case EditorViewportAxisView::PositiveY:
                direction = {0.0F, 1.0F, 0.0F};
                up = {0.0F, 0.0F, -1.0F};
                break;
            case EditorViewportAxisView::NegativeY:
                direction = {0.0F, -1.0F, 0.0F};
                up = {0.0F, 0.0F, 1.0F};
                break;
            case EditorViewportAxisView::PositiveZ:
                direction = {0.0F, 0.0F, 1.0F};
                break;
            case EditorViewportAxisView::NegativeZ:
                direction = {0.0F, 0.0F, -1.0F};
                break;
            default:
                return Result<void>::Failure(MakeViewportError(ViewportModelErrors::InvalidNavigation, "Viewport axis is invalid."));
        }
        EditorViewportCamera next = currentCamera;
        next.position = next.target + direction * Math::Length(next.position - next.target);
        next.up = up;
        if (!next.IsValid())
            return Result<void>::Failure(
                MakeViewportError(ViewportModelErrors::InvalidCamera, "Axis alignment produced an invalid camera."));
        if (next.position == currentCamera.position && next.up == currentCamera.up)
            return Result<void>::Success();
        current_.camera = next;
        ++current_.revision.value;
        events_->Publish(ViewportChangedEvent{current_.revision, ViewportChangeKind::CameraMoved});
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportModel::SetProjection */
    Result<void> EditorViewportModel::SetProjection(const Runtime::CameraProjection projection) {
        if (projection != Runtime::CameraProjection::Perspective && projection != Runtime::CameraProjection::Orthographic)
            return Result<void>::Failure(MakeViewportError(ViewportModelErrors::InvalidProjection, "Viewport projection is invalid."));
        if (current_.camera.projection == projection)
            return Result<void>::Success();
        EditorViewportCamera camera = current_.camera;
        const Result<Math::Vec3> forwardResult = TryCameraForward(camera);
        if (forwardResult.HasError())
            return Result<void>::Failure(forwardResult.ErrorValue());
        const Math::Vec3 forward = forwardResult.Value();
        const float distance = Math::Length(camera.target - camera.position);
        if (projection == Runtime::CameraProjection::Orthographic)
            camera.orthographicHeight = std::max(0.01F, 2.0F * distance * std::tan(camera.verticalFovRadians * 0.5F));
        else {
            const float perspectiveDistance = camera.orthographicHeight / (2.0F * std::tan(camera.verticalFovRadians * 0.5F));
            camera.position = camera.target - forward * perspectiveDistance;
        }
        camera.projection = projection;
        if (!camera.IsValid())
            return Result<void>::Failure(
                MakeViewportError(ViewportModelErrors::InvalidCamera, "Projection change produced an invalid camera."));
        current_.camera = camera;
        ++current_.revision.value;
        events_->Publish(ViewportChangedEvent{current_.revision, ViewportChangeKind::CameraProjectionChanged});
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportModel::Focus */
    Result<void> EditorViewportModel::Focus(const Math::Aabb &worldBounds, const float aspect) {
        if (!worldBounds.IsValid() || !std::isfinite(aspect) || aspect <= 0.0F)
            return Result<void>::Failure(
                MakeViewportError(ViewportModelErrors::InvalidFocusBounds, "Focus bounds and aspect must be valid."));
        const Result<Math::BoundingSphere> sphereResult = Math::SphereFromAabb(worldBounds);
        if (sphereResult.HasError())
            return Result<void>::Failure(sphereResult.ErrorValue());
        const Math::BoundingSphere sphere = sphereResult.Value();
        const float radius = std::max(sphere.radius, 0.25F);
        EditorViewportCamera camera = current_.camera;
        const Result<Math::Vec3> forwardResult = TryCameraForward(camera);
        if (forwardResult.HasError())
            return Result<void>::Failure(forwardResult.ErrorValue());
        const Math::Vec3 forward = forwardResult.Value();
        camera.target = sphere.center;
        if (camera.projection == Runtime::CameraProjection::Perspective) {
            const float verticalHalfFov = camera.verticalFovRadians * 0.5F;
            const float horizontalHalfFov = std::atan(std::tan(verticalHalfFov) * aspect);
            const float limitingHalfFov = std::min(verticalHalfFov, horizontalHalfFov);
            const float distance = radius / std::sin(limitingHalfFov) * 1.2F;
            camera.position = camera.target - forward * distance;
        } else {
            camera.orthographicHeight = 2.0F * radius * 1.2F / std::min(aspect, 1.0F);
            camera.position = camera.target - forward * Math::Length(current_.camera.target - current_.camera.position);
        }
        if (!camera.IsValid())
            return Result<void>::Failure(
                MakeViewportError(ViewportModelErrors::InvalidCamera, "Focus operation produced an invalid camera."));
        current_.camera = camera;
        ++current_.revision.value;
        events_->Publish(ViewportChangedEvent{current_.revision, ViewportChangeKind::CameraFocused});
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportModel::SetTransformPreview */
    Result<void> EditorViewportModel::SetTransformPreview(const SceneObjectTransformPreview &preview) {
        return SetTransformPreviews(std::span{&preview, 1});
    }

    /** @copydoc EditorViewportModel::SetTransformPreviews */
    Result<void> EditorViewportModel::SetTransformPreviews(const std::span<const SceneObjectTransformPreview> previews) {
        for (std::size_t index = 0; index < previews.size(); ++index) {
            if (!IsValid(previews[index])) {
                return Result<void>::Failure(
                    MakeViewportError(ViewportModelErrors::InvalidTransformPreview, "Viewport transform previews must be finite."));
            }
            const auto duplicate =
                std::ranges::find(previews.subspan(index + 1), previews[index].object, &SceneObjectTransformPreview::object);
            if (duplicate != previews.subspan(index + 1).end()) {
                return Result<void>::Failure(MakeViewportError(ViewportModelErrors::InvalidTransformPreview,
                                                               "Viewport transform preview object identities must be unique."));
            }
        }
        if (std::ranges::equal(current_.transformPreviews, previews)) {
            return Result<void>::Success();
        }
        current_.transformPreviews.assign(previews.begin(), previews.end());
        ++current_.revision.value;
        events_->Publish(ViewportChangedEvent{current_.revision, ViewportChangeKind::ScenePreviewChanged});
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportModel::ClearTransformPreview */
    bool EditorViewportModel::ClearTransformPreview() {
        if (current_.transformPreviews.empty()) {
            return false;
        }
        current_.transformPreviews.clear();
        ++current_.revision.value;
        events_->Publish(ViewportChangedEvent{current_.revision, ViewportChangeKind::ScenePreviewChanged});
        return true;
    }

    /** @copydoc EditorViewportModel::SetLightPreview */
    Result<void> EditorViewportModel::SetLightPreview(const SceneObjectLightPreview &preview) {
        if (!preview.object.IsValid() || !IsValidLightComponent(preview.light)) {
            return Result<void>::Failure(
                MakeViewportError(ViewportModelErrors::InvalidLightPreview, "Viewport light preview must be valid."));
        }
        if (current_.lightPreview == preview)
            return Result<void>::Success();
        current_.lightPreview = preview;
        ++current_.revision.value;
        events_->Publish(ViewportChangedEvent{current_.revision, ViewportChangeKind::ScenePreviewChanged});
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportModel::ClearLightPreview */
    bool EditorViewportModel::ClearLightPreview() {
        if (!current_.lightPreview.has_value())
            return false;
        current_.lightPreview.reset();
        ++current_.revision.value;
        events_->Publish(ViewportChangedEvent{current_.revision, ViewportChangeKind::ScenePreviewChanged});
        return true;
    }
}  // namespace Horo::Editor
