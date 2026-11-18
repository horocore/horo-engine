#pragma once
/** @file OpenXRPackageProbeWire.h @brief App-private vocabulary for the supervised Desktop probe. */
#include "Horo/Foundation/Sha256.h"

#include <algorithm>
#include <cstdint>

namespace Horo::Application::Internal::OpenXRProbeWire {
    inline constexpr std::string_view Command = "--xr-package-probe";
    inline constexpr std::string_view Record = "horo-xr-package-probe-v1";
    inline constexpr std::string_view FailureRecord = "horo-xr-package-probe-failure-v1";
    inline constexpr std::string_view LocalConnection = "desktop-local";
    inline constexpr std::string_view Route = "system-default";
    inline constexpr std::string_view Renderer = "vulkan";

    /** @brief Closed redacted reasons allowed through the Desktop process pipe. */
    enum class FailureReason : std::uint8_t {
        Startup,
        Renderer,
        RuntimeUnavailable,
        RuntimeRejected,
        Unsupported,
        Invalid,
        Capacity,
        Stale,
        Permission,
        Retirement,
        Count
    };

    inline constexpr std::array<std::string_view, static_cast<std::size_t>(FailureReason::Count)>
        FailureNames{"startup-unavailable", "renderer-unavailable", "runtime-unavailable", "runtime-rejected",  "unsupported-profile",
                     "invalid-evidence",    "capacity-exceeded",    "stale-evidence",      "permission-denied", "retirement-failed"};

    /** @brief Returns the fixed wire spelling for a known native owner outcome. */
    [[nodiscard]] constexpr std::string_view FailureName(FailureReason reason) noexcept {
        return FailureNames[static_cast<std::size_t>(reason)];
    }

    /** @brief Names only the native desktop hosts admitted by this process contract. */
    [[nodiscard]] constexpr std::string_view DesktopHost() noexcept {
#if defined(_WIN32)
        return "windows";
#elif defined(__linux__) && !defined(__ANDROID__)
        return "linux";
#else
        return {};
#endif
    }

    /** @brief Bounds private runtime and HMD labels before either process starts a probe. */
    [[nodiscard]] inline bool ValidTargetLabel(std::string_view label) noexcept {
        return !label.empty() && label.size() < 256 && !label.starts_with('-') && std::ranges::none_of(label, [](unsigned char byte) {
            return byte < ' ' || byte == 127;
        });
    }

    /** @brief Binds the declared runtime/HMD names to one receipt without copying either label.
     * @param runtime Official runtime label, with no embedded NUL.
     * @param system Official HMD system label, with no embedded NUL.
     * @return Digest of the domain-qualified name pair. The label agrees with the child only after its private runtime check.
     */
    [[nodiscard]] inline Sha256Digest TargetDigest(std::string_view runtime, std::string_view system) noexcept {
        constexpr char Separator{};
        const std::array<std::span<const std::byte>, 4> fragments{std::as_bytes(std::span{"Horo Desktop OpenXR probe v1"}),
                                                                  std::as_bytes(std::span{runtime}),
                                                                  std::as_bytes(std::span{&Separator, 1}),
                                                                  std::as_bytes(std::span{system})};
        return ComputeSha256Fragments(fragments);
    }
}  // namespace Horo::Application::Internal::OpenXRProbeWire
