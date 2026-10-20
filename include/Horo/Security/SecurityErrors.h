#pragma once

/**
 * @file SecurityErrors.h
 * @brief Stable namespaced failures for credential, entropy, integrity, and signature operations.
 * @note Serialized codes use the security. prefix; error domains and severities are unchanged.
 */

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::SecurityErrors {
    extern const ErrorCodeDescriptor InvalidInput;
    extern const ErrorCodeDescriptor EntropyUnavailable;
    extern const ErrorCodeDescriptor CredentialBackendUnavailable;
    extern const ErrorCodeDescriptor CredentialNotFound;
    extern const ErrorCodeDescriptor CredentialExpired;
    extern const ErrorCodeDescriptor CredentialRevoked;
    extern const ErrorCodeDescriptor UnsupportedAlgorithm;
    extern const ErrorCodeDescriptor UnknownSigningKey;
    extern const ErrorCodeDescriptor SignatureProviderUnavailable;
    extern const ErrorCodeDescriptor InvalidSignature;
    extern const ErrorCodeDescriptor IntegrityMismatch;
    extern const ErrorCodeDescriptor MissingEvidence;
    extern const ErrorCodeDescriptor StaleEvidence;
}  // namespace Horo::SecurityErrors
