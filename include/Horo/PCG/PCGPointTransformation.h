#pragma once

/** @file PCGPointTransformation.h
 * @brief Typed settings for bounded deterministic point transformations and gradient noise. */

#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/PCG/PCGPointSchema.h"

#include <cstdint>
#include <vector>

namespace Horo::PCG {
    /** @brief Coordinate frame for translation and random displacement; local offsets use rotation, never scale. */
    enum class PCGPointCoordinateSpace : std::uint8_t {
        Snapshot,
        PointLocal
    };

    /** @brief Component transform: add frame-relative translation, compose rotation, multiply component scale.
     * Snapshot rotation pre-multiplies; PointLocal rotation post-multiplies. Local point bounds remain local.
     * This operation deliberately does not decompose an affine matrix or introduce shear. */
    struct PCGPointTransformSettings final {
        Math::Transform delta{}; /**< Finite translation/scale and nonzero finite quaternion, normalized when composing. */
        PCGPointCoordinateSpace space{PCGPointCoordinateSpace::Snapshot};
    };

    /** @brief Aligns the point's rotated +Y axis to a captured triangle's oriented normal.
     * Selection uses inclusive snapshot XZ barycentrics, nearest vertical height, then stable triangle identity.
     * Position, scale and bounds remain unchanged. Missing coverage and vertical triangles fail explicitly. */
    struct PCGPointAlignmentSettings final {
        double maximumVerticalDistance{1.0}; /**< Finite nonnegative snapshot distance to the selected surface. */
    };

    /** @brief Independent uniform displacement within the inclusive symmetric half-extent box.
     * Draws expand the stored semantic sample seed in fixed x/y/z order using an authored salt.
     * Input identity attributes and stored seeds remain unchanged; row position never selects the random stream. */
    struct PCGPointRandomOffsetSettings final {
        Math::Vec3 halfExtent{}; /**< Finite nonnegative snapshot units. */
        PCGPointCoordinateSpace space{PCGPointCoordinateSpace::Snapshot};
        std::uint64_t salt{}; /**< Authored stream separation, including zero. */
    };

    /** @brief Exact normalization of bounded gradient noise before amplitude scaling. */
    enum class PCGPointNoiseNormalization : std::uint8_t {
        Signed,
        UnitInterval
    };

    /** @brief Bounded three-dimensional gradient noise in snapshot coordinates.
     * Scalar/Vector2/Vector3/Vector4 outputs use independent fixed channels. Octaves accumulate in ascending order,
     * divide by their complete weight, and clamp to [-1,1] before the declared normalization and amplitude.
     * Integer/boolean outputs and source-identity keys are rejected; an existing attribute must have the exact type.
     * Coordinates at every octave must lie in [-1048576,1048576]; violations fail without publishing partial output. */
    struct PCGPointNoiseSettings final {
        PCGAttributeKey attribute; /**< Required canonical typed attribute identity owned by the settings. */
        PCGAttributeType type{PCGAttributeType::Scalar};
        double frequency{1.0};    /**< Positive finite snapshot inverse units. */
        double amplitude{1.0};    /**< Finite nonnegative output multiplier. */
        std::uint32_t octaves{1}; /**< Inclusive range [1,16]. */
        double persistence{0.5};  /**< Finite octave weight multiplier in [0,1]. */
        double lacunarity{2.0};   /**< Finite octave frequency multiplier in [1,8]. */
        PCGPointNoiseNormalization normalization{PCGPointNoiseNormalization::Signed};
        std::uint64_t salt{}; /**< Authored stream salt; the node provenance seed supplies the immutable gradient field root. */
    };

    /** @brief Encodes canonical version-1 component transformation settings.
     * @param settings Typed delta and coordinate frame.
     * @return Canonical network-order payload or typed invalid/allocation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointTransformationSettings(const PCGPointTransformSettings &settings);
    /** @brief Encodes canonical version-1 surface alignment settings.
     * @param settings Finite nonnegative coverage distance.
     * @return Canonical network-order payload or typed invalid/allocation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointTransformationSettings(const PCGPointAlignmentSettings &settings);
    /** @brief Encodes canonical version-1 random displacement settings.
     * @param settings Finite extents, frame and authored stream salt.
     * @return Canonical network-order payload or typed invalid/allocation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointTransformationSettings(const PCGPointRandomOffsetSettings &settings);
    /** @brief Encodes canonical version-1 noise settings used by either PerlinNoise or SimplexNoise.
     * @param settings Exact output key/type, bounded octave parameters and normalization.
     * @return Canonical network-order payload or typed invalid/allocation failure. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointTransformationSettings(const PCGPointNoiseSettings &settings);
}  // namespace Horo::PCG
