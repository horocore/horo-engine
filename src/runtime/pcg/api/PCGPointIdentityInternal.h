#pragma once

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/PCG/PCGPointCloudWorkspace.h"
#include "PCGPointGenerationInternal.h"

namespace Horo::PCG::detail {
    /** @brief Checks complete schema-1 composite identity, allowing legacy identity-free point schemas. */
    [[nodiscard]] Result<bool> HasPointIdentity(const PCGPointSchema &schema);

    /** @brief Proves two bounded composite arrays and fixed bottom-up merge comparison work. */
    [[nodiscard]] Result<PointGenerationCharge> ChargePointIdentity(std::size_t maximumPoints);

    /** @brief Validates retained roots/sample IDs against the single canonical seed encoder and rejects duplicate composites.
     * @param inputs Immutable prefixes, copied only into admitted temporary identity arrays; point order remains unchanged.
     * @param cancellation Existing operation token; polled while copying and sorting bounded runs.
     * @return Success or typed malformed/mismatched/duplicate identity, capacity or closed-operation failure.
     * @pre ChargePointIdentity of the complete input count has been admitted before workspace creation. */
    [[nodiscard]] Result<void> ValidatePointIdentities(std::span<const PCGPointReadView> inputs, CancellationToken cancellation);
}  // namespace Horo::PCG::detail
