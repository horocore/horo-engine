#include "Horo/Security/SecurityErrors.h"

namespace Horo::SecurityErrors {
    namespace {
        const ErrorDomainId Domain{"horo.security"};

        [[nodiscard]] ErrorCodeDescriptor Descriptor(const char *code, const char *summary, const char *hint, const bool retryable = false,
                                                     const bool userActionable = false) {
            return {.domain = Domain,
                    .code = ErrorCode{code},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary,
                    .remediationHint = hint,
                    .retryable = retryable,
                    .userActionable = userActionable};
        }
    }  // namespace

    const ErrorCodeDescriptor InvalidInput =
        Descriptor("security.invalid_input", "Security input is malformed.", "Use a canonical bounded value.");
    const ErrorCodeDescriptor EntropyUnavailable =
        Descriptor("security.entropy_unavailable", "Secure operating-system entropy is unavailable.", "Retry after the platform recovers.",
                   true);
    const ErrorCodeDescriptor CredentialBackendUnavailable =
        Descriptor("security.credential_backend_unavailable", "The secure credential provider is unavailable.",
                   "Configure or restore a secure provider.", true, true);
    const ErrorCodeDescriptor CredentialNotFound = Descriptor("security.credential_not_found", "The credential reference is unknown.",
                                                              "Authenticate again and replace the reference.", false, true);
    const ErrorCodeDescriptor CredentialExpired = Descriptor("security.credential_expired", "The credential has expired.",
                                                             "Authenticate again and rotate the credential.", false, true);
    const ErrorCodeDescriptor CredentialRevoked = Descriptor("security.credential_revoked", "The credential has been revoked.",
                                                             "Authenticate again and create a new credential.", false, true);
    const ErrorCodeDescriptor UnsupportedAlgorithm =
        Descriptor("security.unsupported_algorithm", "The signature algorithm is unsupported.", "Use an approved signature algorithm.");
    const ErrorCodeDescriptor UnknownSigningKey =
        Descriptor("security.unknown_signing_key", "The signing key is not trusted.", "Install an approved signing root.", false, true);
    const ErrorCodeDescriptor SignatureProviderUnavailable =
        Descriptor("security.signature_provider_unavailable", "The signature provider is unavailable.",
                   "Restore the configured crypto provider.", true);
    const ErrorCodeDescriptor InvalidSignature = Descriptor("security.invalid_signature", "The artifact signature is invalid.",
                                                            "Obtain an authentic artifact from a trusted publisher.", false, true);
    const ErrorCodeDescriptor IntegrityMismatch =
        Descriptor("security.integrity_mismatch", "The artifact digest does not match its signed envelope.",
                   "Discard the corrupted artifact.", false, true);
    const ErrorCodeDescriptor MissingEvidence =
        Descriptor("security.missing_evidence", "Native activation requires verified signature evidence.",
                   "Provide a signed artifact and configured trust roots.");
    const ErrorCodeDescriptor StaleEvidence =
        Descriptor("security.stale_evidence", "Verification evidence does not describe the current artifact.",
                   "Reverify the exact artifact bytes.");
}  // namespace Horo::SecurityErrors
