#pragma once

#include "Horo/Foundation/ErrorCode.h"

#include <typeindex>

namespace Horo::PlatformServices::Detail {
    /** @brief Identifies result contracts whose provider failures may carry private cloud data. */
    [[nodiscard]] bool IsCloudResult(std::type_index type) noexcept;

    /** @brief Rebuilds a cloud provider failure from bounded host-owned descriptors before it becomes observable. */
    [[nodiscard]] Error RedactCloudProviderFailure(const Error &error);
}  // namespace Horo::PlatformServices::Detail
