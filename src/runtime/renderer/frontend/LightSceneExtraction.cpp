#include "Horo/Runtime/Render/LightSceneExtraction.h"

#include <algorithm>
#include <cmath>

namespace Horo::Render {
    namespace {
        /** @brief Reject undeclared kinds before mapping authored component values. */
        Result<RenderLightKind> RenderKind(const Runtime::LightKind kind) {
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

        /** @brief Resolve bounded world position and scale-independent light orientation. */
        Result<RenderLight> Extract(const Runtime::RuntimeSceneView scene, Runtime::RuntimeEntityView entity,
                                    const CancellationToken &cancellation) {
            const auto &authored = *entity.components->light;
            auto kind = RenderKind(authored.kind);
            if (kind.HasError())
                return Result<RenderLight>::Failure(kind.ErrorValue());
            if (!std::isfinite(authored.innerConeRadians) || !std::isfinite(authored.outerConeRadians) || authored.innerConeRadians < 0 ||
                authored.outerConeRadians < authored.innerConeRadians || authored.outerConeRadians > Math::Pi)
                return Result<RenderLight>::Failure(MakeError(LightCullingErrors::InvalidInput));
            Math::Mat4 world = Math::Mat4::Identity();
            Math::Quaternion rotation;
            for (std::size_t depth = 0; depth < 64; ++depth) {
                if (cancellation.IsCancellationRequested())
                    return Result<RenderLight>::Failure(MakeError(LightCullingErrors::Cancelled));
                const auto local = entity.localTransform->TryToMatrix();
                if (local.HasError())
                    return Result<RenderLight>::Failure(local.ErrorValue());
                const auto localRotation = entity.localTransform->rotation.TryNormalized();
                if (localRotation.HasError())
                    return Result<RenderLight>::Failure(localRotation.ErrorValue());
                world = Math::Multiply(local.Value(), world);
                rotation = localRotation.Value() * rotation;
                if (!entity.parent) {
                    const auto position = Math::TryTransformPoint(world, {});
                    const auto direction = rotation.TryRotate({0, 0, -1});
                    if (position.HasError())
                        return Result<RenderLight>::Failure(position.ErrorValue());
                    if (direction.HasError())
                        return Result<RenderLight>::Failure(direction.ErrorValue());
                    RenderLight light{.kind = kind.Value(),
                                      .position = position.Value(),
                                      .direction = direction.Value(),
                                      .color = authored.color,
                                      .intensity = authored.intensity,
                                      .range = authored.range,
                                      .innerConeCosine = std::cos(authored.innerConeRadians),
                                      .outerConeCosine = std::cos(authored.outerConeRadians)};
                    if (!light.IsValid())
                        return Result<RenderLight>::Failure(MakeError(LightCullingErrors::InvalidInput));
                    return Result<RenderLight>::Success(light);
                }
                const auto parent = scene.Get(*entity.parent);
                if (parent.HasError())
                    return Result<RenderLight>::Failure(parent.ErrorValue());
                entity = parent.Value();
            }
            return Result<RenderLight>::Failure(MakeError(LightCullingErrors::Capacity));
        }
    }  // namespace

    /** @copydoc ExtractSceneLights */
    Result<ExtractedLightTable> ExtractSceneLights(const Runtime::RuntimeSceneView scene, const std::uint32_t maximumLights,
                                                   const std::span<IdentifiedRenderLight> scratch, const CancellationToken &cancellation) {
        if (!scene.IsCurrent() || maximumLights == 0 || maximumLights > LightCullingBudget::HardMaximumLights)
            return Result<ExtractedLightTable>::Failure(MakeError(LightCullingErrors::InvalidInput));
        if (scene.SlotCount() > 65536 || scratch.size() < maximumLights)
            return Result<ExtractedLightTable>::Failure(MakeError(LightCullingErrors::Capacity));
        ExtractedLightTable table{scene.RuntimeId(), scene.StructuralRevision(), 0};
        for (std::size_t slot = 0; slot < scene.SlotCount(); ++slot) {
            if (cancellation.IsCancellationRequested())
                return Result<ExtractedLightTable>::Failure(MakeError(LightCullingErrors::Cancelled));
            const auto entity = scene.EntityAt(slot);
            if (!entity || !entity->components->light || !entity->components->light->enabled)
                continue;
            if (table.count == maximumLights)
                return Result<ExtractedLightTable>::Failure(MakeError(LightCullingErrors::Capacity));
            auto light = Extract(scene, *entity, cancellation);
            if (light.HasError())
                return Result<ExtractedLightTable>::Failure(light.ErrorValue());
            const auto identity = (std::uint64_t{entity->entity.entity.generation} << 32U) | entity->entity.entity.index;
            scratch[table.count++] = {{identity}, light.Value()};
        }
        std::ranges::sort(scratch.first(table.count), {}, &IdentifiedRenderLight::identity);
        return Result<ExtractedLightTable>::Success(table);
    }
}  // namespace Horo::Render
