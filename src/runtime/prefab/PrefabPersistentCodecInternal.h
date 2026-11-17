#pragma once
/** @file PrefabPersistentCodecInternal.h
 * @brief One trusted finite quota policy for both canonical dynamic-prefab codec directions.
 */
#include "Horo/Prefab/PrefabPersistentCodec.h"

#include <algorithm>

namespace Horo::Prefab::Detail {
    /** @brief Projects validated domain limits into existing canonical Save quotas without changing their authority.
     * @param limits Valid bounds; callers must first require IsValidPrefabPersistenceLimits.
     * @return Shared codec configuration; manual recipe counts additionally require remaining-wire checks.
     */
    inline Runtime::CanonicalCodecLimits PersistentCodecLimits(const PrefabPersistenceLimits &limits) {
        Runtime::CanonicalCodecLimits result;
        result.maximumBytes = limits.maximumBytes;
        result.maximumDecodedBytes = limits.maximumBytes * 2;
        result.maximumStringBytes = std::min(MaximumAssetCookTargetIdBytes, limits.maximumBytes);
        result.maximumCollectionElements = limits.maximumMembers;
        result.maximumReadWorkBytes = limits.maximumBytes * 4;
        return result;
    }
}  // namespace Horo::Prefab::Detail
