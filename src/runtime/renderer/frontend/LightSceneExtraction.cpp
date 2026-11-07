#include "Horo/Runtime/Render/LightSceneExtraction.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Horo::Render {
    namespace {
        /** @brief Rejects undeclared kinds before mapping authored component values. */
        Result<RenderLightKind> RenderKind(Runtime::LightKind kind) {
            switch (kind) {
                case Runtime::LightKind::Directional:
                    return Result<RenderLightKind>::Success(RenderLightKind::Directional);
                case Runtime::LightKind::Point:
                    return Result<RenderLightKind>::Success(RenderLightKind::Point);
                case Runtime::LightKind::Spot:
                    return Result<RenderLightKind>::Success(RenderLightKind::Spot);
            }
            return Result<RenderLightKind>::Failure(MakeError(LightCullingErrors::InvalidInput));
        }

        /** @brief Validates enabled authored cone values without reinterpreting RGB. */
        bool AuthoredValid(const Runtime::LightComponent &authored) noexcept {
            return authored.enabled && std::isfinite(authored.innerConeRadians) && std::isfinite(authored.outerConeRadians) &&
                   authored.innerConeRadians >= 0 && authored.outerConeRadians >= authored.innerConeRadians &&
                   authored.outerConeRadians <= Math::Pi;
        }

        using LightPose = std::pair<Math::Vec3, Math::Vec3>;

        /** @brief Resolves the final proper hierarchy pose without mixing scale into light direction. */
        Result<LightPose> FinalPose(const Math::Mat4 &world, const Math::Quaternion &rotation) {
            const auto position = Math::TryTransformPoint(world, {});
            if (position.HasError())
                return Result<LightPose>::Failure(position.ErrorValue());
            const auto direction = rotation.TryRotate({0, 0, -1});
            if (direction.HasError())
                return Result<LightPose>::Failure(direction.ErrorValue());
            return Result<LightPose>::Success({position.Value(), direction.Value()});
        }

        /** @brief Resolves at most64 ancestors under the actual Scene owner and cancellation scope. */
        Result<LightPose> ResolvePose(Runtime::RuntimeSceneView scene, Runtime::RuntimeEntityView entity,
                                      const CancellationToken &cancellation) {
            Math::Mat4 world = Math::Mat4::Identity();
            Math::Quaternion rotation;
            for (std::size_t depth = 0; depth < 64; ++depth) {
                if (cancellation.IsCancellationRequested())
                    return Result<LightPose>::Failure(MakeError(LightCullingErrors::Cancelled));
                const auto local = entity.localTransform->TryToMatrix();
                if (local.HasError())
                    return Result<LightPose>::Failure(local.ErrorValue());
                const auto localRotation = entity.localTransform->rotation.TryNormalized();
                if (localRotation.HasError())
                    return Result<LightPose>::Failure(localRotation.ErrorValue());
                world = Math::Multiply(local.Value(), world);
                rotation = localRotation.Value() * rotation;
                if (!entity.parent)
                    return FinalPose(world, rotation);
                const auto parent = scene.Get(*entity.parent);
                if (parent.HasError())
                    return Result<LightPose>::Failure(parent.ErrorValue());
                entity = parent.Value();
            }
            return Result<LightPose>::Failure(MakeError(LightCullingErrors::Capacity));
        }

        /** @brief Validates finite extraction reservations before touching reusable scratch. */
        Result<void> ValidateExtraction(Runtime::RuntimeSceneView scene, std::uint32_t maximumLights, std::size_t scratch) {
            if (!scene.IsCurrent() || maximumLights == 0 || maximumLights > LightCullingBudget::HardMaximumLights)
                return Result<void>::Failure(MakeError(LightCullingErrors::InvalidInput));
            if (scene.SlotCount() > 65536 || scratch < maximumLights)
                return Result<void>::Failure(MakeError(LightCullingErrors::Capacity));
            return Result<void>::Success();
        }

        /** @brief Appends one eligible canonical light and exact entity identity to unpublished scratch. */
        Result<void> AppendLight(Runtime::RuntimeSceneView scene, std::size_t slot, std::uint32_t maximumLights,
                                 std::span<IdentifiedRenderLight> scratch, ExtractedLightTable &table,
                                 const CancellationToken &cancellation) {
            const auto entity = scene.EntityAt(slot);
            if (!entity || !entity->components->light || !entity->components->light->enabled)
                return Result<void>::Success();
            if (table.count == maximumLights)
                return Result<void>::Failure(MakeError(LightCullingErrors::Capacity));
            auto pose = ResolvePose(scene, *entity, cancellation);
            if (pose.HasError())
                return Result<void>::Failure(pose.ErrorValue());
            auto light =
                BuildRenderLightValue(*entity->components->light, pose.Value().first, pose.Value().second, table.sourceColorPolicy);
            if (light.HasError())
                return Result<void>::Failure(light.ErrorValue());
            const auto identity = (std::uint64_t{entity->entity.entity.generation} << 32U) | entity->entity.entity.index;
            scratch[table.count++] = {{identity}, light.Value()};
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc BuildRenderLightValue */
    Result<RenderLight> BuildRenderLightValue(const Runtime::LightComponent &authored, Math::Vec3 worldPosition, Math::Vec3 worldDirection,
                                              SceneColorSourcePolicy sourceColorPolicy) {
        const auto kind = RenderKind(authored.kind);
        if (kind.HasError())
            return Result<RenderLight>::Failure(kind.ErrorValue());
        if (!AuthoredValid(authored))
            return Result<RenderLight>::Failure(MakeError(LightCullingErrors::InvalidInput));
        const auto color = ConvertSceneColorToAcesCg(authored.color, sourceColorPolicy);
        if (color.HasError())
            return Result<RenderLight>::Failure(color.ErrorValue());
        RenderLight light{.kind = kind.Value(),
                          .position = worldPosition,
                          .direction = worldDirection,
                          .color = color.Value(),
                          .intensity = authored.intensity,
                          .range = authored.range,
                          .innerConeCosine = std::cos(authored.innerConeRadians),
                          .outerConeCosine = std::cos(authored.outerConeRadians)};
        if (!light.IsValid())
            return Result<RenderLight>::Failure(MakeError(LightCullingErrors::InvalidInput));
        return Result<RenderLight>::Success(light);
    }

    /** @copydoc ExtractSceneLights */
    Result<ExtractedLightTable> ExtractSceneLights(Runtime::RuntimeSceneView scene, std::uint32_t maximumLights,
                                                   std::span<IdentifiedRenderLight> scratch, SceneColorSourcePolicy sourceColorPolicy,
                                                   const CancellationToken &cancellation) {
        using Return = Result<ExtractedLightTable>;
        if (auto policy = ValidateSceneColorSourcePolicy(sourceColorPolicy); policy.HasError())
            return Return::Failure(policy.ErrorValue());
        if (auto valid = ValidateExtraction(scene, maximumLights, scratch.size()); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        ExtractedLightTable table{scene.RuntimeId(), scene.StructuralRevision(), 0, sourceColorPolicy};
        for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
            if (cancellation.IsCancellationRequested())
                return Return::Failure(MakeError(LightCullingErrors::Cancelled));
            if (auto appended = AppendLight(scene, slot, maximumLights, scratch, table, cancellation); appended.HasError())
                return Return::Failure(appended.ErrorValue());
        }
        std::ranges::sort(scratch.first(table.count), {}, &IdentifiedRenderLight::identity);
        return Return::Success(table);
    }
}  // namespace Horo::Render
