#include "VulkanAndroidErrors.h"

#include <format>

namespace Horo::Render::AndroidVulkan {
    namespace {
        /** @brief Constructs private immutable error metadata. */
        ErrorCodeDescriptor Describe(const char *code, const char *summary, bool retryable = false) {
            return {ErrorDomainId{"horo.render.vulkan_android"},
                    ErrorCode{code},
                    ErrorSeverity::Error,
                    summary,
                    "Use the explicit supported Android Vulkan development profile and preserve owner-lane lifetime.",
                    retryable,
                    true};
        }
    }  // namespace

    namespace Errors {
        const ErrorCodeDescriptor Unsupported =
            Describe("render.vulkan_android.unsupported", "The requested Android Vulkan operation is unsupported.");
        const ErrorCodeDescriptor Invalid = Describe("render.vulkan_android.invalid", "The Android Vulkan request or state is invalid.");
        const ErrorCodeDescriptor WrongThread =
            Describe("render.vulkan_android.wrong_thread", "Android Vulkan requires its serialized owner lane.");
        const ErrorCodeDescriptor Closed = Describe("render.vulkan_android.closed", "Android Vulkan admission is closed.");
        const ErrorCodeDescriptor Pending =
            Describe("render.vulkan_android.pending", "Android Vulkan retirement or drawable availability is pending.", true);
        const ErrorCodeDescriptor NativeFailure =
            Describe("render.vulkan_android.native_failure", "An Android Vulkan native operation failed.");
        const ErrorCodeDescriptor Capacity = Describe("render.vulkan_android.capacity", "A bounded Android Vulkan capacity was exceeded.");
    }  // namespace Errors

    /** @copydoc NativeError */
    Error NativeError(std::string_view operation, VkResult result) {
        return MakeError(Errors::NativeFailure, std::format("{} failed with VkResult {}.", operation, static_cast<int>(result)));
    }
}  // namespace Horo::Render::AndroidVulkan
