#pragma once
/** @file OpenXRWindowsLoaderImage.h @brief Cold Windows OpenXR image and signed import inventory verification. */
#include "Horo/Foundation/Result.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace Horo::Application::Internal {
    inline constexpr std::size_t MaximumOpenXRAbiDependencies = 32;
    inline constexpr std::size_t MaximumOpenXRLoaderImageBytes = 64U * 1024U * 1024U;

    /** @brief Checks the closed filename grammar shared by Windows PE imports and the signed ABI inventory.
     * @param name One ordinary DLL basename. @return True for a finite ASCII basename, ignoring ASCII case.
     */
    [[nodiscard]] bool IsWindowsOpenXRAbiDll(std::string_view name) noexcept;

    /** @brief Verifies the exact direct Windows OpenXR loader imports before any staging or execution.
     * @param image Digest-verified bytes from the authenticated component archive.
     * @param architecture Exact selected Windows tuple architecture. The implemented image profile is x64 PE32+.
     * @param signedImports Exact immutable signed OS ABI dependency basenames.
     * @return Success or typed invalid image, unsupported ABI, or signed dependency mismatch.
     * @details The bounded inspection never maps or executes code. Delay imports, bound imports and managed images are
     * outside this profile. Success describes the direct import table only; native loader/OS ABI authority is separate.
     */
    [[nodiscard]] Result<void> VerifyWindowsOpenXRLoaderImage(std::span<const std::byte> image, std::string_view architecture,
                                                              std::span<const std::string> signedImports);
}  // namespace Horo::Application::Internal
