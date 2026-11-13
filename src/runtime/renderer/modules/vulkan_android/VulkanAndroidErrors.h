#pragma once
/** @file VulkanAndroidErrors.h @brief Private typed Android Vulkan failures. */
#include "Horo/Foundation/Result.h"

#include <string_view>
#include <vulkan/vulkan.h>

namespace Horo::Render::AndroidVulkan {
    namespace Errors {
        extern const ErrorCodeDescriptor Unsupported, Invalid, WrongThread, Closed, Pending, NativeFailure, Capacity;
    }

    /** @brief Preserves native failure status without translating it to successful presentation. @param operation Native call. @param
     * result Native status. @return Typed error. */
    [[nodiscard]] Error NativeError(std::string_view operation, VkResult result);
}  // namespace Horo::Render::AndroidVulkan
