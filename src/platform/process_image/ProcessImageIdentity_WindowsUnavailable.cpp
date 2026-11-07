#include "ProcessImageIdentity.h"

#if !defined(_WIN32)
#error This is the explicitly unsupported Windows mapped-image profile, not a portable positive provider.
#endif

namespace Horo::Platform {
    namespace {
        const ErrorCodeDescriptor
            Unavailable{.domain = ErrorDomainId{"horo.platform.process_image"},
                        .code = ErrorCode{"platform.process_image.unavailable"},
                        .defaultSeverity = ErrorSeverity::Error,
                        .summary = "Authoritative Windows mapped executable identity is unavailable.",
                        .remediationHint =
                            "Use a qualified OS-backed mapped-image provider; a pathname or PE header is not identity evidence.",
                        .retryable = false,
                        .userActionable = true};

        template <typename T> Result<T> MissingMappedFileAuthority() {
            return Result<T>::Failure(
                MakeError(Unavailable,
                          "This Windows profile cannot bind the actual mapped main image to an authentic retained backing-file identity. "
                          "No process-image lease, artifact digest or Physics state authority was issued."));
        }
    }  // namespace

    Result<void> ProcessImageIdentity::Verify() const {
        return MissingMappedFileAuthority<void>();
    }

    Result<void> ProcessImageIdentity::VerifyStaticCodeAddress(const void *) const {
        return MissingMappedFileAuthority<void>();
    }

    Result<ProcessImageIdentity> CaptureCurrentProcessImageIdentity() {
        return MissingMappedFileAuthority<ProcessImageIdentity>();
    }
}  // namespace Horo::Platform
