#pragma once

/** @file SceneColorEncoding.h
 * @brief Explicit versioned color ingress into scene-linear ACEScg/AP1; never a backend-selected interpretation.
 */
#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"

#include <cstdint>

namespace Horo::Render {
    /** @brief Declared source role. Legacy interpretation is an explicit host migration choice, not a claim about old data. */
    enum class SceneColorEncoding : std::uint8_t {
        Unspecified,
        LinearAcesCg,
        LegacyLinearRec709D65,
        SrgbRec709D65
    };

    /** @brief Versioned source interpretation recorded with extracted color values.
     * Version1 uses AP1/D60 linear working RGB. Rec709/D65 sources include the pinned chromatic adaptation into AP1/D60.
     * sRGB decoding uses the standard piecewise EOTF; the linear segment extends below zero, preserving finite negatives.
     * Unspecified and unsupported versions fail; no format, backend, filename or ambient display profile chooses this value.
     */
    struct SceneColorSourcePolicy final {
        SceneColorEncoding encoding{SceneColorEncoding::Unspecified};
        std::uint32_t version{1};
        /** @brief Compares the complete declared interpretation. @param other Policy to compare. @return Exact equality. */
        bool operator==(const SceneColorSourcePolicy &other) const = default;
    };

    /** @brief Validates a declared supported source role and version.
     * @param policy Explicit host/import-selected interpretation. @return Success or typed invalid-policy failure.
     */
    [[nodiscard]] Result<void> ValidateSceneColorSourcePolicy(SceneColorSourcePolicy policy);

    /** @brief Converts one declared color before componentwise scene lighting; alpha and data-texture semantics are outside this API.
     * @param source Finite source RGB, including finite negative or above-one values.
     * @param policy Explicit versioned source interpretation. @return Finite signed scene-linear AP1 RGB or typed policy/numeric failure.
     * @details Performs no clamp, tone map, exposure, output transfer, asset discovery or implicit legacy inference.
     */
    [[nodiscard]] Result<Math::Vec3> ConvertSceneColorToAcesCg(Math::Vec3 source, SceneColorSourcePolicy policy);

    namespace SceneColorErrors {
        extern const ErrorCodeDescriptor InvalidPolicy;  /**< Missing, unknown or unsupported versioned source interpretation. */
        extern const ErrorCodeDescriptor NonFiniteInput; /**< Source contains NaN or infinity. */
        extern const ErrorCodeDescriptor OutOfRange;     /**< Converted values cannot be represented as finite float32. */
    }  // namespace SceneColorErrors
}  // namespace Horo::Render
