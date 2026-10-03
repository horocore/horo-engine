#pragma once

/**
 * @file MetricDescriptorInternal.h
 * @brief Shared target-private descriptor construction for Physics and Character metrics.
 */

#include "Horo/Foundation/Telemetry/Telemetry.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::PhysicsMetricDetail {
    /** @brief Builds one bounded descriptor without registering or binding a series. */
    [[nodiscard]] inline Telemetry::InstrumentDescriptor MakeDescriptor(const std::string_view subsystem,
                                                                        const Telemetry::InstrumentKind kind, std::string name,
                                                                        const Telemetry::MetricUnit unit, std::string description,
                                                                        const Telemetry::MetricCollectionLevel level,
                                                                        std::vector<Telemetry::DimensionDescriptor> dimensions) {
        const auto maximumSeries = dimensions.empty() ? 1U : static_cast<std::uint32_t>(dimensions.front().allowedValues.size());
        return {.kind = kind,
                .name = std::move(name),
                .subsystem = std::string{subsystem},
                .unit = unit,
                .description = std::move(description),
                .dimensions = std::move(dimensions),
                .maxSeries = maximumSeries,
                .minimumCollectionLevel = level};
    }
}  // namespace Horo::PhysicsMetricDetail
