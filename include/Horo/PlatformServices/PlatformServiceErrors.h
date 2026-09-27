#pragma once

/** @file PlatformServiceErrors.h
 * @brief Provider-neutral Platform Services failure identities and translation.
 */

#include "Horo/Foundation/ErrorCode.h"
#include "Horo/Foundation/ModuleDescriptor.h"

#include <cstdint>
#include <optional>

namespace Horo::PlatformServices {
    /** @brief Stable provider failure classes shared by application and host adapters. */
    enum class PlatformProviderFailureCategory : std::uint8_t {
        Offline,
        NotSignedIn,
        Forbidden,
        RateLimited,
        PreconditionFailed,
        QuotaExceeded,
        InvalidResponse,
        TransientFailure,
        PermanentFailure,
        Unknown
    };

    /** @brief Stable application meaning independent of the originating provider or host. */
    enum class PlatformServiceErrorKind : std::uint8_t {
        ProviderFailed,
        AuthenticationRequired,
        AccessDenied,
        CapabilityUnavailable,
        NullProvider,
        Cancelled,
        TimedOut,
        Other
    };

    namespace PlatformServiceErrors {
        extern const ErrorCodeDescriptor ProviderFailed;
        extern const ErrorCodeDescriptor Offline;
        extern const ErrorCodeDescriptor NotSignedIn;
        extern const ErrorCodeDescriptor Forbidden;
        extern const ErrorCodeDescriptor RateLimited;
        extern const ErrorCodeDescriptor PreconditionFailed;
        extern const ErrorCodeDescriptor QuotaExceeded;
        extern const ErrorCodeDescriptor InvalidResponse;
        extern const ErrorCodeDescriptor TransientFailure;
        extern const ErrorCodeDescriptor PermanentFailure;
        extern const ErrorCodeDescriptor Unknown;
    }  // namespace PlatformServiceErrors

    /**
     * @brief Converts one provider-reported category to a stable operation error and safe cause.
     * @param category Validated provider category; invalid enum representations become Unknown.
     * @param requestId Frontend-owned correlation identity, never a provider account identifier.
     * @param generation Frontend-owned request generation.
     * @return Stable provider failure with a category cause and bounded correlation diagnostic.
     */
    [[nodiscard]] Error MakePlatformProviderError(PlatformProviderFailureCategory category, std::uint64_t requestId,
                                                  std::uint64_t generation);

    /** @brief Returns a provider-neutral category from a declared provider failure cause, if present. */
    [[nodiscard]] std::optional<PlatformProviderFailureCategory> PlatformProviderCategory(const Error &error) noexcept;

    /** @brief Gives application and host adapters the same stable meaning for an error. */
    [[nodiscard]] PlatformServiceErrorKind ClassifyPlatformServiceError(const Error &error) noexcept;

    /** @brief Inert registry contribution for the Platform Services error domain. */
    [[nodiscard]] ModuleErrorDomainDescriptor PlatformServiceErrorDomain();
}  // namespace Horo::PlatformServices
