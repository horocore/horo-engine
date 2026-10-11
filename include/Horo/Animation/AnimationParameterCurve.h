#pragma once

/** @file AnimationParameterCurve.h
 * @brief Bounded immutable authored float curves for typed graph parameters.
 */

#include "Horo/Animation/AnimationGraph.h"
#include "Horo/Foundation/CancellationToken.h"

#include <span>
#include <utility>
#include <vector>

namespace Horo::Animation {
    /** @brief Explicit interpolation between adjacent authored keys; endpoints always clamp. */
    enum class GraphCurveInterpolation : std::uint8_t {
        Step,
        Linear
    };

    /** @brief One finite scalar value at a non-negative exact animation time. */
    struct GraphCurveKey final {
        AnimationTime time{};
        float value{};
    };

    /** @brief Control-boundary authored curve; preparation copies and validates every key. */
    struct GraphParameterCurveData final {
        std::uint32_t version{1};
        GraphParameterId parameter{};
        GraphCurveInterpolation interpolation{GraphCurveInterpolation::Linear};
        std::vector<GraphCurveKey> keys;
    };

    /** @brief Immutable float curve pinned to one graph-local parameter identity. */
    class AnimationParameterCurve final {
    public:
        static constexpr std::uint32_t MaximumKeys = 4096; /**< Finite preparation and logarithmic sample-work bound. */
        /** @brief Validates a complete authored curve before immutable publication.
         * @param data Versioned identity, interpolation and strictly time-ordered finite keys.
         * @param cancellation Control-boundary cancellation; cancelled preparation publishes nothing.
         * @return Prepared curve or registered version, binding, malformed, cancellation or capacity diagnostic.
         * @pre Control boundary; may allocate. Empty, duplicate or unsorted keys are rejected.
         */
        [[nodiscard]] static Result<AnimationParameterCurve> Create(GraphParameterCurveData data, CancellationToken cancellation = {});

        /** @brief Returns the exact graph-local float parameter binding. @return Stable typed identity. */
        [[nodiscard]] GraphParameterId Parameter() const noexcept {
            return data_.parameter;
        }

        /** @brief Returns immutable authored keys. @return Lifetime-bound read-only view. */
        [[nodiscard]] std::span<const GraphCurveKey> Keys() const noexcept {
            return data_.keys;
        }

        /** @brief Samples in logarithmic bounded work without allocation or mutation.
         * @param time Exact authored timeline position; values outside the key range clamp to endpoints.
         * @return Finite interpolated scalar, using double intermediates to avoid finite-float subtraction overflow.
         * @pre This object is a prepared curve, not a moved-from value.
         */
        [[nodiscard]] float Sample(AnimationTime time) const noexcept;

    private:
        explicit AnimationParameterCurve(GraphParameterCurveData data) : data_(std::move(data)) {}

        GraphParameterCurveData data_;
    };
}  // namespace Horo::Animation
