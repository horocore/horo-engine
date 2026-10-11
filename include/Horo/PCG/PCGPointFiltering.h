#pragma once
/** @file PCGPointFiltering.h
 * @brief Typed canonical settings for bounded stable point compaction. */
#include "Horo/PCG/PCGPointSchema.h"

namespace Horo::PCG {
    /** @brief Inclusive finite density interval within [0,1]. */
    struct PCGDensityRangeFilterSettings final {
        double minimum{};
        double maximum{1.0};
    };

    /** @brief Keeps transform positions inside the inclusive snapshot-space box. */
    struct PCGBoundsFilterSettings final {
        Math::Aabb bounds{};
    };

    /** @brief Inclusive terrain slope interval in degrees within [0,180].
     * @details At each point's XZ position, samples the containing surface with nearest vertical height;
     * equal heights use stable surface identity order. Missing projected coverage fails the whole evaluation. */
    struct PCGSlopeFilterSettings final {
        double minimumDegrees{};
        double maximumDegrees{90.0};
    };

    /** @brief Stable greedy separation in snapshot units; equality at the minimum distance is accepted.
     * @details Visits points in input order and compares against previously accepted positions.
     * All pair checks and accepted-index storage are admitted before execution. */
    struct PCGDistanceFilterSettings final {
        double minimumDistance{};
    };
    /** @brief Exact typed comparison. Vector and Boolean values support equality and inequality only. */
    enum class PCGAttributeComparison : std::uint8_t {
        Equal,
        NotEqual,
        Less,
        LessEqual,
        Greater,
        GreaterEqual
    };
    /** @brief Closed predicate values; integers retain their full width without numeric or string coercion. */
    using PCGAttributePredicateValue = std::variant<bool, std::int64_t, std::uint64_t, double, Math::Vec2, Math::Vec3, Math::Vec4>;

    /** @brief Exact schema-key/type predicate; missing or mismatched columns fail even for empty input. */
    struct PCGAttributeFilterSettings final {
        PCGAttributeKey key;
        PCGAttributeComparison comparison{PCGAttributeComparison::Equal};
        PCGAttributePredicateValue value;
    };

    /** @brief Closed settings vocabulary for the five catalog-owned filter operations. */
    using PCGPointFilterSettings = std::variant<PCGDensityRangeFilterSettings, PCGBoundsFilterSettings, PCGSlopeFilterSettings,
                                                PCGDistanceFilterSettings, PCGAttributeFilterSettings>;
    /** @brief Encodes validated settings in canonical version-1 network order.
     * @param settings Typed operation settings; the matching catalog kind must own the payload.
     * @return Owned payload or typed invalid/capacity failure; signed zero is canonicalized. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodePCGPointFilterSettings(const PCGPointFilterSettings &settings);
}  // namespace Horo::PCG
