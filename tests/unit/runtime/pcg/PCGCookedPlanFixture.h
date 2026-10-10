#pragma once
/** @file PCGCookedPlanFixture.h @brief Test-only synthetic workspace model access; never an executable cook path. */
#include "Horo/PCG/PCGCookedPlan.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::PCG::detail {
    /** @brief Preserves independent-output revocation coverage beyond the current single-output built-ins.
     * @details This fixture is defined only in HoroPCGTests. Its deliberately synthetic plan is used only by
     * workspace model tests, never by CompilePCGGraph or EvaluatePCGCpu. */
    struct PCGCookedPlanFixture final {
        [[nodiscard]] static PCGCookedPlan WithAdditionalPointOutputs(const PCGCookedPlan &plan) {
            auto data = plan.data_;
            REQUIRE(!data.nodes.empty());
            for (const auto value : {102U, 103U}) {
                const auto pin = PinId::Create(value);
                REQUIRE(pin.HasValue());
                data.nodes.front().pins.push_back(
                    {pin.Value(), PCGPinDirection::Output, PCGPinType::PointSet, PCGPinCardinality::Multiple});
            }
            return PCGCookedPlan{std::move(data)};
        }
    };
}  // namespace Horo::PCG::detail
