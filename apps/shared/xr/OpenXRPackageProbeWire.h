#pragma once
/** @file OpenXRPackageProbeWire.h @brief App-private vocabulary for the supervised Desktop probe. */
#include "Horo/Foundation/Sha256.h"

namespace Horo::Application::Internal::OpenXRProbeWire {
    inline constexpr std::string_view Command = "--xr-package-probe";
    inline constexpr std::string_view Record = "horo-xr-package-probe-v1";
    inline constexpr std::string_view Route = "system-default";
    inline constexpr std::string_view Renderer = "vulkan";

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
