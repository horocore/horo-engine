#pragma once
/** @file PrefabPersistentCodec.h
 * @brief Bounded canonical Save record encoding of exact dynamic prefab recipes, independent of source documents.
 */
#include "Horo/Prefab/PrefabPersistentState.h"
#include "Horo/Runtime/Save/SaveCanonicalCodec.h"

namespace Horo::Prefab {
    /** @brief Encodes stable identity order through the existing canonical Save primitives and SAME typed override codec.
     * @param state Coherent actual capture or private metadata admission candidate; serialization grants no world authority.
     * @param limits Aggregate record/member/byte bounds. @param profile Existing prefab semantic policy.
     * @return Complete sealed canonical bytes or original typed shape/budget/allocation failure.
     */
    [[nodiscard]] Result<Runtime::CanonicalEncodedValue> EncodePrefabPersistentState(const PrefabPersistentState &state,
                                                                                     const PrefabPersistenceLimits &limits,
                                                                                     const PrefabLimitProfile &profile);
    /** @brief Decodes data only and validates the complete recipe before any content load/Scene work.
     * @param bytes Admitted canonical record bytes. @param limits Trusted aggregate bounds.
     * @param profile Actual prefab semantic policy.
     * @return Complete owned recipe data; actual installed content/native participants remain separate mandatory admission.
     */
    [[nodiscard]] Result<PrefabPersistentState> DecodePrefabPersistentState(std::span<const std::byte> bytes,
                                                                            const PrefabPersistenceLimits &limits,
                                                                            const PrefabLimitProfile &profile);
}  // namespace Horo::Prefab
