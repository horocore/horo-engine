#include "Horo/Runtime/Save/SaveErrors.h"

namespace Horo::Runtime::SaveErrors {
    namespace {
        const ErrorDomainId kDomain{"horo.save"};
        constexpr auto kError = ErrorSeverity::Error;
    }  // namespace

    const ErrorCodeDescriptor ProtectionInvalid{kDomain, ErrorCode{"save.protection.invalid"}, kError,
                                                "Save protection metadata or bounds are invalid.",
                                                "Use a valid bounded protection request."};
    const ErrorCodeDescriptor ProtectionRequired{kDomain, ErrorCode{"save.protection.required"}, kError,
                                                 "Trusted save policy requires authenticated encryption.",
                                                 "Use the configured protection provider; plaintext fallback is forbidden."};
    const ErrorCodeDescriptor ProtectionUnsupported{kDomain, ErrorCode{"save.protection.unsupported"}, kError,
                                                    "The configured save protection provider or algorithm is unsupported.",
                                                    "Install or select an explicitly supported provider."};
    const ErrorCodeDescriptor ProtectionUnavailable{kDomain, ErrorCode{"save.protection.unavailable"}, kError,
                                                    "The save protection provider is unavailable.",
                                                    "Restore the provider before retrying; do not fall back to plaintext."};
    const ErrorCodeDescriptor ProtectionKeyRotated{kDomain, ErrorCode{"save.protection.key_rotated"}, kError,
                                                   "The save protection key reference was rotated.",
                                                   "Use an authorized historical key or recovery flow."};
    const ErrorCodeDescriptor ProtectionKeyRevoked{kDomain, ErrorCode{"save.protection.key_revoked"}, kError,
                                                   "The save protection key reference was revoked.",
                                                   "Use an authorized recovery flow; do not bypass authentication."};
    const ErrorCodeDescriptor ProtectionAuthenticationFailed{kDomain, ErrorCode{"save.protection.authentication_failed"}, kError,
                                                             "Protected save authentication failed.",
                                                             "Reject the archive without decoding or exposing plaintext."};
}  // namespace Horo::Runtime::SaveErrors
