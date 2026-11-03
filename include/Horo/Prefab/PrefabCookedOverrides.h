#pragma once
/** @file PrefabCookedOverrides.h
 * @brief Cooked-baseline adapter into the sole canonical typed delta semantics;
 * never loads source graphs.
 */
#include "Horo/Prefab/CookedPrefab.h"
#include "Horo/Prefab/PrefabOverrideOperations.h"

namespace Horo::Prefab {
    /** @brief Copies only genuine cooked provenance/member descriptors into bounded
     * operation-owned delta input.
     * @param source Actual verified cooked template, retained through the
     * operation.
     * @param limits Canonical object/byte/work bounds.
     * @return Complete stable-address baseline or typed
     * missing-provenance/duplicate/budget failure.
     */
    [[nodiscard]] Result<std::vector<ResolvedPrefabObject>> PrepareCookedPrefabOverrideBaseline(const CookedPrefab &source,
                                                                                                const PrefabLimitProfile &limits);
    /** @brief Applies saved intent to a genuine cooked baseline using the SAME
     * source/instance semantic resolver once.
     * @param source Actual verified cooked template, including genuine root
     * revision.
     * @param overrides Complete saved typed intent; unresolved state cannot expose
     * a runtime candidate.
     * @param schemas Actual immutable registered schema/permission snapshot, when
     * property/topology edits require it.
     * @param limits Canonical bounded operation policy. @param cancellation
     * Existing operation ancestry.
     * @return Complete projected descriptor input; no artifact or Scene entity is
     * published.
     */
    [[nodiscard]] Result<PrefabOverrideResolution> ResolveCookedPrefabOverrides(const CookedPrefab &source,
                                                                                const PrefabValueOverrideSet &overrides,
                                                                                const PrefabValueSchemaSnapshot *schemas,
                                                                                const PrefabLimitProfile &limits,
                                                                                const CancellationToken &cancellation = {});
}  // namespace Horo::Prefab
