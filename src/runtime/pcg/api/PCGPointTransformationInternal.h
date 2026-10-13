#pragma once

#include "Horo/PCG/PCGCpuEvaluator.h"
#include "Horo/PCG/PCGPointTransformation.h"

#include <array>
#include <variant>

namespace Horo::PCG::detail {
    using PointTransformationSettings =
        std::variant<PCGPointTransformSettings, PCGPointAlignmentSettings, PCGPointRandomOffsetSettings, PCGPointNoiseSettings>;
    /** @brief Closed sparse transformation-kind classification; reserved kinds never dispatch. */
    [[nodiscard]] bool IsPointTransformation(PCGCpuNodeKind kind) noexcept;
    /** @brief Decodes exact canonical version-1 settings; never retains borrowed payload storage. */
    [[nodiscard]] Result<PointTransformationSettings> DecodePointTransformation(const PCGCookedNode &node);
    /** @brief Checks complete bounded point/surface/octave work before workspace allocation. */
    [[nodiscard]] Result<PCGNodeCost> ChargePointTransformation(const PointTransformationSettings &settings, std::size_t points,
                                                                std::size_t surfaces);
    /** @brief Proves exact preservation schema, or one explicitly typed noise output, and equal point capacity. */
    [[nodiscard]] Result<void> ValidatePointTransformation(const PointTransformationSettings &settings, const PCGPointOutputBound &input,
                                                           const PCGPointOutputBound &output);
    /** @brief Computes one finite core transform without changing source identity, density, seed or local bounds. */
    [[nodiscard]] Result<Math::Transform> TransformPoint(const PointTransformationSettings &settings, const PCGSpatialSnapshot &spatial,
                                                         const Math::Transform &source, std::uint64_t seed, CancellationToken cancellation);
    /** @brief Evaluates bounded real Perlin or Simplex gradient noise in fixed channel/octave order. */
    [[nodiscard]] Result<std::array<double, 4>> PointNoise(const PCGPointNoiseSettings &settings, PCGCpuNodeKind kind, Math::Vec3 position,
                                                           std::uint64_t fieldSeed, CancellationToken cancellation);
    /** @brief Stores the exact declared floating-point noise column without integer/boolean conversion. */
    [[nodiscard]] Result<void> WritePointNoise(const PCGPointNoiseSettings &settings, const PCGPointWriteView &writer, std::size_t index,
                                               const std::array<double, 4> &value);
}  // namespace Horo::PCG::detail
