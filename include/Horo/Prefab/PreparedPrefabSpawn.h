#pragma once
/** @file PreparedPrefabSpawn.h
 * @brief Verified cooked lease plus canonical saved delta preparation shared by normal spawn and private reconstruction.
 */
#include "Horo/Prefab/PrefabCookedOverrides.h"
#include "Horo/Prefab/PrefabTemplateProvider.h"

namespace Horo::Prefab {
    /** @brief Immutable complete descriptor recipe; neither an AssetCook artifact nor a source graph. */
    class PreparedPrefabSpawn final {
    public:
        /** @brief Admits optional typed deltas against the actual cooked baseline exactly once.
         * @param source Actual verified provider-issued immutable closure.
         * @param overrides Optional complete typed intent, never runtime component memory.
         * @param schemas Actual explicit validating metadata for property/topology changes.
         * @param limits Canonical bounded policy. @param cancellation Existing operation ancestry.
         * @return Complete owned preparation or original compatibility/schema/cancellation failure; no Scene mutation.
         */
        [[nodiscard]] static Result<PreparedPrefabSpawn> Prepare(PrefabTemplateLease source,
                                                                 const std::optional<PrefabValueOverrideSet> &overrides,
                                                                 const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits,
                                                                 const CancellationToken &cancellation = {});
        /** @brief Returns the actual original cook lease. @return Immutable retained source. */
        [[nodiscard]] const PrefabTemplateLease &Source() const noexcept;
        /** @brief Returns complete baseline-plus-delta descriptors. @return Borrowed owned values, never a published artifact. */
        [[nodiscard]] const CookedPrefabData &Data() const noexcept;
        /** @brief Reports explicit root-transform delta ownership, which takes precedence over baseline placement.
         * @return True only when the complete admitted intent addresses the exact cooked root transform.
         */
        [[nodiscard]] bool OverridesRootTransform() const noexcept;

    private:
        friend class PrefabTemplateProvider;
        /** @brief Adopts only complete canonical preparation or the untouched provider baseline. */
        PreparedPrefabSpawn(PrefabTemplateLease source, std::shared_ptr<const CookedPrefabData> data = {},
                            bool overridesRoot = false) noexcept;
        PrefabTemplateLease source_;
        std::shared_ptr<const CookedPrefabData> data_;
        bool overridesRoot_{};
    };
}  // namespace Horo::Prefab
