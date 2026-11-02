#pragma once
/** @file PrefabDocumentOverrideLayers.h @brief Private composition-layer admission shared by canonical prefab codecs. */
#include "PrefabValueOverridesInternal.h"

namespace Horo::Prefab::Detail {
    /** @brief Decode only a bounded existing override envelope; absence remains absence. */
    [[nodiscard]] Result<std::optional<PrefabValueOverrideSet>> ParseOverrideLayer(const Json &owner, const char *field,
                                                                                   const PrefabProjectPolicy &policy);
    /** @brief Validate exact source/revision associations and charge all embedded bytes before source publication. */
    [[nodiscard]] Result<void> ValidateOverrideLayers(const PrefabDocumentData &data, const PrefabLimitProfile &limits,
                                                      std::size_t &payloadBytes);
    /** @brief Reuse the sole canonical override encoder; caller already admitted the complete immutable document. */
    [[nodiscard]] ValueDetail::Wire EncodeOverrideLayer(const PrefabValueOverrideSet &set);
}  // namespace Horo::Prefab::Detail
