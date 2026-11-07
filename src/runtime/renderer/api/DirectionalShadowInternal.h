#pragma once
#include "Horo/Runtime/Render/DirectionalShadow.h"
#include "Horo/Runtime/Render/DirectionalShadowErrors.h"

namespace Horo::Render::DirectionalShadowDetail {
    struct Basis final {
        Math::Vec3 forward, right, up;
    };

    struct DepthBounds final {
        float minimum{}, maximum{};
    };

    [[nodiscard]] Result<Basis> CameraBasis(const RenderCameraView &camera);
    [[nodiscard]] Result<DepthBounds> CasterDepth(const RenderSceneView &scene, Math::Vec3 lightDirection);
    [[nodiscard]] float SplitDistance(const RenderProjectionDescriptor &projection, const DirectionalShadowProfile &profile,
                                      std::uint32_t split) noexcept;
    [[nodiscard]] Result<DirectionalShadowCascade> FitCascade(const RenderSceneView &scene, const Basis &camera, Math::Vec3 direction,
                                                              DepthBounds casterDepth, float aspect, Math::ClipDepthRange depthRange,
                                                              const DirectionalShadowProfile &profile, std::uint32_t index,
                                                              DirectionalShadowAtlasExtent atlas);
    [[nodiscard]] std::uint64_t Fingerprint(const RenderSceneView &scene, float aspect, Math::ClipDepthRange depthRange,
                                            DirectionalShadowFrame &frame) noexcept;
}  // namespace Horo::Render::DirectionalShadowDetail
