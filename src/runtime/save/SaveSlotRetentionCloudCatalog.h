#pragma once

#include "SaveSlotLifecycleInternal.h"

#include <nlohmann/json.hpp>

namespace Horo::Runtime::SaveSlotLifecycleDetail {
    /** @brief Encodes exact bounded provider/account/generation CAS evidence beside its retirement. */
    [[nodiscard]] nlohmann::json EncodeRetentionCloud(const std::optional<SaveSlotRetentionCloudDeletion> &cloud);
    /** @brief Decodes provider-neutral evidence bound to the catalog's admitted namespace. */
    [[nodiscard]] std::optional<SaveSlotRetentionCloudDeletion> DecodeRetentionCloud(const nlohmann::json &value,
                                                                                     const SaveNamespaceId &name);
    /** @brief Reuses existing cloud metadata validation for a single exact retained generation. */
    [[nodiscard]] bool ValidRetentionCloud(const Retired &record, const SaveNamespaceId &name);
}  // namespace Horo::Runtime::SaveSlotLifecycleDetail
