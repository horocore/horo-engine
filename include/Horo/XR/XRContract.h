#pragma once

/**
 * @file XRContract.h
 * @brief Backend-neutral XRApi semantic-version compatibility contract.
 */

#include "Horo/Foundation/Result.h"

#include <compare>
#include <cstdint>

namespace Horo::XR {
    /** @brief Semantic version of the Horo-owned XRApi contract, never an OpenXR API version. */
    struct XRContractVersion final {
        std::uint16_t major{}; /**< Breaking contract generation; zero is reserved. */
        std::uint16_t minor{}; /**< Backward-compatible capability additions. */
        std::uint16_t patch{}; /**< Compatible clarifications and corrections. */

        /** @brief Checks representation only. @return True when the breaking generation is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return major != 0;
        }

        constexpr auto operator<=>(const XRContractVersion &) const noexcept = default;
    };

    /** @brief Current public XRApi contract version. */
    inline constexpr XRContractVersion CurrentXRContractVersion{1, 1, 0};

    /**
     * @brief Validates that a provided XRApi contract can satisfy a required contract.
     * @param required Minimum contract required by a consumer.
     * @param provided Contract implemented by the composed producer.
     * @return Success when major versions match and provided minor is sufficient; otherwise a typed invalid or incompatible result.
     * @post Performs no registration, allocation on success, native call, or ambient state mutation.
     */
    [[nodiscard]] Result<void> RequireXRContractVersion(XRContractVersion required, XRContractVersion provided);
}  // namespace Horo::XR
