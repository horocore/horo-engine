#include "Horo/Runtime/Camera/CameraErrors.h"
#include "Horo/Runtime/Camera/CameraService.h"

namespace Horo::Runtime {
    namespace {
        /** @brief Resolves a fixed-depth scene hierarchy without retaining any scene borrow. */
        Result<Math::Transform> ResolvePose(const RuntimeSceneView scene, EntityRef current) {
            constexpr std::size_t MaximumCameraAncestors = 64;
            Math::Mat4 world = Math::Mat4::Identity();
            for (std::size_t depth = 0; depth < MaximumCameraAncestors; ++depth) {
                const auto entity = scene.Get(current);
                if (entity.HasError())
                    return Result<Math::Transform>::Failure(entity.ErrorValue());
                if (!entity.Value().localTransform)
                    return Result<Math::Transform>::Failure(MakeError(CameraErrors::InvalidTarget));
                const auto local = entity.Value().localTransform->TryToMatrix();
                if (local.HasError())
                    return Result<Math::Transform>::Failure(local.ErrorValue());
                world = Math::Multiply(local.Value(), world);
                if (!entity.Value().parent)
                    return Math::TryDecomposeAffineTRS(world);
                current = *entity.Value().parent;
            }
            return Result<Math::Transform>::Failure(MakeError(CameraErrors::CapacityExceeded));
        }

        /** @brief Copies lens values only for the declared supported projection vocabulary. */
        Result<Render::RenderProjectionDescriptor> ResolveLens(const CameraComponent &camera) {
            using enum CameraProjection;
            Render::RenderProjectionDescriptor result;
            switch (camera.projection) {
                case Perspective:
                    result.kind = Render::RenderProjectionKind::Perspective;
                    break;
                case Orthographic:
                    result.kind = Render::RenderProjectionKind::Orthographic;
                    break;
                default:
                    return Result<Render::RenderProjectionDescriptor>::Failure(MakeError(CameraErrors::InvalidTarget));
            }
            result.verticalFovRadians = camera.verticalFieldOfViewRadians;
            result.orthographicHeight = camera.orthographicHeight;
            result.nearPlane = camera.nearPlane;
            result.farPlane = camera.farPlane;
            if (!camera.enabled || !result.IsValid())
                return Result<Render::RenderProjectionDescriptor>::Failure(MakeError(CameraErrors::InvalidTarget));
            return Result<Render::RenderProjectionDescriptor>::Success(result);
        }
    }  // namespace

    /** @copydoc ResolveSceneCamera */
    Result<CameraProposal> ResolveSceneCamera(const RuntimeSceneView scene, const EntityRef camera) {
        const auto entity = scene.Get(camera);
        if (entity.HasError())
            return Result<CameraProposal>::Failure(entity.ErrorValue());
        if (!entity.Value().components || !entity.Value().components->camera)
            return Result<CameraProposal>::Failure(MakeError(CameraErrors::InvalidTarget));
        const auto lens = ResolveLens(*entity.Value().components->camera);
        if (lens.HasError())
            return Result<CameraProposal>::Failure(lens.ErrorValue());
        const auto pose = ResolvePose(scene, camera);
        if (pose.HasError())
            return Result<CameraProposal>::Failure(pose.ErrorValue());
        const auto &transform = pose.Value();
        Render::RenderCameraView values{transform.translation, transform.translation + transform.rotation.Rotate({0.0F, 0.0F, -1.0F}),
                                        transform.rotation.Rotate({0.0F, 1.0F, 0.0F}), lens.Value()};
        if (!values.IsValid())
            return Result<CameraProposal>::Failure(MakeError(CameraErrors::InvalidTarget));
        return Result<CameraProposal>::Success({camera, values});
    }
}  // namespace Horo::Runtime
