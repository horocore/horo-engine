#pragma once
#include "Horo/Runtime/Render/ClusteredLightGrid.h"

namespace Horo::Render::ClusteredLightDetail {
    struct Basis final {
        Math::Vec3 forward;
        Math::Vec3 right;
        Math::Vec3 up;
    };

    [[nodiscard]] Result<Basis> CameraBasis(const RenderCameraView &camera);
    [[nodiscard]] Result<LightCluster> Cell(const ClusteredLightGridDescriptor &descriptor, const Basis &basis, std::uint32_t index);
    [[nodiscard]] float DepthBoundary(const ClusteredLightGridDescriptor &descriptor, std::uint32_t slice) noexcept;
    [[nodiscard]] std::uint64_t Signature(std::span<const LightCluster> clusters) noexcept;
}  // namespace Horo::Render::ClusteredLightDetail
