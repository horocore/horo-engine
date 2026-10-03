#include "Horo/PlatformServices/PlatformServiceErrors.h"

#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "Horo/PlatformServices/PlatformServicesBackend.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"
#include "Horo/PlatformServices/PlatformUserSession.h"

#include <array>
#include <format>

namespace Horo::PlatformServices {
    namespace PlatformServiceErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.service"};
        }

        const ErrorCodeDescriptor ProviderFailed{Domain,
                                                 ErrorCode{"platform.provider.failed"},
                                                 ErrorSeverity::Error,
                                                 "Platform provider operation failed.",
                                                 "Inspect the stable category and retry policy.",
                                                 false,
                                                 false};
        const ErrorCodeDescriptor Offline{Domain,
                                          ErrorCode{"platform.provider.offline"},
                                          ErrorSeverity::Error,
                                          "Provider is offline.",
                                          "Retry when connectivity returns.",
                                          true,
                                          true};
        const ErrorCodeDescriptor NotSignedIn{Domain,
                                              ErrorCode{"platform.provider.not_signed_in"},
                                              ErrorSeverity::Error,
                                              "Provider session is not signed in.",
                                              "Sign in before retrying.",
                                              false,
                                              true};
        const ErrorCodeDescriptor Forbidden{Domain,
                                            ErrorCode{"platform.provider.forbidden"},
                                            ErrorSeverity::Error,
                                            "Provider denied this operation.",
                                            "Check account access and policy.",
                                            false,
                                            true};
        const ErrorCodeDescriptor RateLimited{Domain,
                                              ErrorCode{"platform.provider.rate_limited"},
                                              ErrorSeverity::Error,
                                              "Provider rate limit was reached.",
                                              "Retry after provider backoff.",
                                              true,
                                              false};
        const ErrorCodeDescriptor PreconditionFailed{Domain,
                                                     ErrorCode{"platform.provider.precondition_failed"},
                                                     ErrorSeverity::Error,
                                                     "Provider precondition failed.",
                                                     "Refresh state before retrying.",
                                                     false,
                                                     false};
        const ErrorCodeDescriptor QuotaExceeded{Domain,
                                                ErrorCode{"platform.provider.quota_exceeded"},
                                                ErrorSeverity::Error,
                                                "Provider quota was exceeded.",
                                                "Reduce usage or increase quota.",
                                                false,
                                                true};
        const ErrorCodeDescriptor InvalidResponse{Domain,
                                                  ErrorCode{"platform.provider.invalid_response"},
                                                  ErrorSeverity::Error,
                                                  "Provider returned an invalid response.",
                                                  "Inspect provider integration.",
                                                  false,
                                                  false};
        const ErrorCodeDescriptor TransientFailure{Domain,
                                                   ErrorCode{"platform.provider.transient_failure"},
                                                   ErrorSeverity::Error,
                                                   "Provider failed temporarily.",
                                                   "Retry according to operation policy.",
                                                   true,
                                                   false};
        const ErrorCodeDescriptor PermanentFailure{Domain,
                                                   ErrorCode{"platform.provider.permanent_failure"},
                                                   ErrorSeverity::Error,
                                                   "Provider operation failed permanently.",
                                                   "Inspect provider configuration.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor Unknown{Domain,
                                          ErrorCode{"platform.provider.unknown"},
                                          ErrorSeverity::Error,
                                          "Provider returned an unrecognized failure.",
                                          "Inspect correlated provider logs.",
                                          false,
                                          false};
    }  // namespace PlatformServiceErrors

    namespace {
        using Category = PlatformProviderFailureCategory;
        constexpr std::array<Category, 10> Categories{Category::Offline,         Category::NotSignedIn,        Category::Forbidden,
                                                      Category::RateLimited,     Category::PreconditionFailed, Category::QuotaExceeded,
                                                      Category::InvalidResponse, Category::TransientFailure,   Category::PermanentFailure,
                                                      Category::Unknown};
        const std::array<const ErrorCodeDescriptor *, 10> Descriptors{&PlatformServiceErrors::Offline,
                                                                      &PlatformServiceErrors::NotSignedIn,
                                                                      &PlatformServiceErrors::Forbidden,
                                                                      &PlatformServiceErrors::RateLimited,
                                                                      &PlatformServiceErrors::PreconditionFailed,
                                                                      &PlatformServiceErrors::QuotaExceeded,
                                                                      &PlatformServiceErrors::InvalidResponse,
                                                                      &PlatformServiceErrors::TransientFailure,
                                                                      &PlatformServiceErrors::PermanentFailure,
                                                                      &PlatformServiceErrors::Unknown};

        [[nodiscard]] bool Matches(const Error &error, const ErrorCodeDescriptor &descriptor) noexcept {
            return error.domain.Value() == descriptor.domain.Value() && error.code.Value() == descriptor.code.Value();
        }
    }  // namespace

    /** @copydoc MakePlatformProviderError */
    Error MakePlatformProviderError(const PlatformProviderFailureCategory category, const std::uint64_t requestId,
                                    const std::uint64_t generation) {
        std::size_t index = Categories.size() - 1;
        for (std::size_t candidate = 0; candidate < Categories.size(); ++candidate) {
            if (Categories[candidate] == category) {
                index = candidate;
                break;
            }
        }
        Error cause = MakeError(*Descriptors[index]);
        cause.diagnostics.push_back({.code = DiagnosticCode{"platform.provider.correlation"},
                                     .severity = DiagnosticSeverity::Note,
                                     .message = std::format("request={}; generation={}", requestId, generation)});
        return WithCause(MakeError(PlatformServiceErrors::ProviderFailed), std::move(cause));
    }

    /** @copydoc PlatformProviderCategory */
    std::optional<PlatformProviderFailureCategory> PlatformProviderCategory(const Error &error) noexcept {
        if (!Matches(error, PlatformServiceErrors::ProviderFailed) || error.cause.Get() == nullptr)
            return std::nullopt;
        for (std::size_t index = 0; index < Descriptors.size(); ++index) {
            if (Matches(*error.cause.Get(), *Descriptors[index]))
                return Categories[index];
        }
        return std::nullopt;
    }

    /** @copydoc ClassifyPlatformServiceError */
    PlatformServiceErrorKind ClassifyPlatformServiceError(const Error &error) noexcept {
        if (Matches(error, PlatformServiceErrors::ProviderFailed)) {
            if (PlatformProviderCategory(error) == PlatformProviderFailureCategory::NotSignedIn)
                return PlatformServiceErrorKind::AuthenticationRequired;
            if (PlatformProviderCategory(error) == PlatformProviderFailureCategory::Forbidden)
                return PlatformServiceErrorKind::AccessDenied;
            return PlatformServiceErrorKind::ProviderFailed;
        }
        if (Matches(error, PlatformSessionErrors::NoSubject) || Matches(error, PlatformSessionErrors::Authenticating) ||
            Matches(error, PlatformSessionErrors::Failed))
            return PlatformServiceErrorKind::AuthenticationRequired;
        if (Matches(error, PlatformSessionErrors::AccessDenied) || Matches(error, PlatformSessionErrors::AccessRestricted) ||
            Matches(error, PlatformSessionErrors::AccessRevoked))
            return PlatformServiceErrorKind::AccessDenied;
        if (Matches(error, BackendErrors::ServiceUnavailable) || Matches(error, BackendErrors::UnsupportedOperation) ||
            Matches(error, BackendErrors::RequiredServiceUnavailable))
            return PlatformServiceErrorKind::CapabilityUnavailable;
        if (Matches(error, FrontendErrors::NullProvider))
            return PlatformServiceErrorKind::NullProvider;
        if (Matches(error, RequestErrors::Cancelled))
            return PlatformServiceErrorKind::Cancelled;
        if (Matches(error, RequestErrors::TimedOut))
            return PlatformServiceErrorKind::TimedOut;
        return PlatformServiceErrorKind::Other;
    }

    /** @copydoc PlatformServiceErrorDomain */
    ModuleErrorDomainDescriptor PlatformServiceErrorDomain() {
        ModuleErrorDomainDescriptor domain{.id = ErrorDomainId{"horo.platform.service"}};
        domain.descriptors.reserve(Descriptors.size() + 1);
        domain.descriptors.push_back(&PlatformServiceErrors::ProviderFailed);
        domain.descriptors.insert(domain.descriptors.end(), Descriptors.begin(), Descriptors.end());
        return domain;
    }
}  // namespace Horo::PlatformServices
