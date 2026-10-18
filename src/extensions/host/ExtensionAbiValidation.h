/** @file
 * @brief Private validation for append-only native moduleApi result tables.
 */
#pragma once

#include "Horo/Extensions/ExtensionAbi.h"

namespace Horo::Extensions {
    /**
     * @brief Validate a host-owned moduleApi result and clear absent legacy fields.
     * @param moduleApi Host-allocated, zero-initialized result after the load callback.
     * @return False for truncated fields or a size beyond the supplied output capacity.
     * @pre The callback received storage for exactly sizeof(HoroExtensionModuleApi).
     */
    [[nodiscard]] bool NormalizeModuleApi(HoroExtensionModuleApi &moduleApi) noexcept;

    /**
     * @brief Negotiate inert moduleApi requirements before invoking its load entry point.
     * @param query Optional moduleApi query; null selects the legacy 1.0 contract.
     * @param host Complete host-owned capability table offered to the moduleApi.
     * @param negotiated Optional host-owned output receiving validated requirements on success; zero for legacy/no query or failure.
     * @return Success only for supported versions, sizes, and required functions.
     */
    [[nodiscard]] HoroExtensionStatus NegotiateModuleAbi(HoroExtensionQueryFunc query, const HoroExtensionHostApi &host,
                                                         HoroExtensionRequirements *negotiated = nullptr) noexcept;
}  // namespace Horo::Extensions
