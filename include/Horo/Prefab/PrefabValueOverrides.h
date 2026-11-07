#pragma once
/** @file PrefabValueOverrides.h
 * @brief Bounded schema capture, canonical value deltas and detached instance
 * resolution.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Prefab/PrefabOverrideOperations.h"
#include "Horo/Prefab/PrefabSourceResolver.h"

namespace Horo::Prefab {
    /** @brief Complete detached resolution; unresolved sets retain their intent but
     * have no runtime candidate. */
    struct PrefabValueOverrideResolution final {
        std::optional<EffectivePrefabCandidate> effective;
        PrefabValueOverrideSet rebased;
        std::vector<PrefabValueProvenance> provenance;
        std::size_t redundantRecords{};
    };

    /** @brief Resolve against actual immutable prefab objects; source data is never
     * mutated.
     * @param source Real canonical source expansion. @param set Current instance
     * intent.
     * @param schemas Explicit immutable schema snapshot; null admits only
     * transforms and preserves property records as orphans.
     * @param limits Captured resource policy. @param cancellation Cooperative
     * operation token.
     * @return Detached effective candidate/provenance or preserved
     * conflicts/orphans; malformed inputs fail without publication. */
    [[nodiscard]] Result<PrefabValueOverrideResolution> ResolvePrefabValueOverrides(const EffectivePrefabCandidate &source,
                                                                                    const PrefabValueOverrideSet &set,
                                                                                    const PrefabValueSchemaSnapshot *schemas,
                                                                                    const PrefabLimitProfile &limits,
                                                                                    const CancellationToken &cancellation = {});
    /** @brief Prepare one semantic assignment/revert/rebase through the same
     * resolver as runtime conversion.
     * @param source Real revision-pinned source. @param current Optional existing
     * owned intent. @param edit Typed operation.
     * @param schemas Explicit schema snapshot. @param limits Captured policy.
     * @param cancellation Cooperative token.
     * @return Complete transaction candidate; caller validates document revision
     * then atomically records history. */
    [[nodiscard]] Result<PrefabValueOverrideResolution> PreparePrefabValueEdit(
        const EffectivePrefabCandidate &source, const std::optional<PrefabValueOverrideSet> &current, const PrefabValueEdit &edit,
        const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits, const CancellationToken &cancellation = {});

    /** @brief Prepare complete component topology intent through the shared
     * detached resolver and existing document history.
     * @param source Exact immutable immediate source. @param current Existing
     * combined instance intent, if present.
     * @param edit Typed add/remove/replace/revert intent. @param schemas Captured
     * complete validating metadata.
     * @param limits Captured resource policy. @param cancellation Cooperative
     * operation token.
     * @return Complete transaction candidate or preserved conflict/orphan evidence;
     * failure publishes no partial changes. */
    [[nodiscard]] Result<PrefabValueOverrideResolution> PreparePrefabComponentEdit(
        const EffectivePrefabCandidate &source, const std::optional<PrefabValueOverrideSet> &current, const PrefabComponentEdit &edit,
        const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits, const CancellationToken &cancellation = {});
}  // namespace Horo::Prefab
