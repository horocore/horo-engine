#include "Horo/Foundation/ErrorCode.h"

#include <algorithm>
#include <utility>

namespace Horo {
    ErrorCause::ErrorCause(std::shared_ptr<const Error> cause) noexcept : cause_(std::move(cause)) {}

    ErrorCause::operator bool() const noexcept {
        return cause_ != nullptr;
    }

    const Error *ErrorCause::Get() const noexcept {
        return cause_.get();
    }

    /** @copydoc MakeError */
    Error MakeError(const ErrorCodeDescriptor &descriptor, std::string message) {
        if (message.empty())
            message.assign(descriptor.summary);

        return Error{
            .code = descriptor.code,
            .domain = descriptor.domain,
            .severity = descriptor.defaultSeverity,
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL != 0
            // Checked string moves allocate a proxy inside noexcept; copying keeps error assembly fallible.
            .message = message,
#else
            .message = std::move(message),
#endif
            .diagnostics = std::vector<Diagnostic>(std::size_t{0}),
        };
    }

    /** @copydoc WithCause */
    Error WithCause(Error outer, Error cause) {
        outer.cause = ErrorCause{std::make_shared<const Error>(std::move(cause))};
        return outer;
    }

    /** @copydoc WrapError */
    Error WrapError(const ErrorCodeDescriptor &descriptor, Error cause, std::string message) {
        return WithCause(MakeError(descriptor, std::move(message)), std::move(cause));
    }

    /** @copydoc ErrorChainContains */
    bool ErrorChainContains(const Error &error, const ErrorDomainId &domain, const ErrorCode &code) noexcept {
        for (const Error *current = &error; current != nullptr; current = current->cause.Get()) {
            if (current->domain.Value() == domain.Value() && current->code.Value() == code.Value())
                return true;
        }
        return false;
    }

    /** @copydoc DiagnosticCodeForDeclaredError */
    std::optional<DiagnosticCode> DiagnosticCodeForDeclaredError(const Error &error, const std::string_view expectedDomain,
                                                                 const std::span<const ErrorCodeDescriptor *const> descriptors) {
        if (error.domain.Value() != expectedDomain)
            return std::nullopt;
        const auto descriptor = std::ranges::find_if(descriptors, [&](const ErrorCodeDescriptor *candidate) {
            return candidate != nullptr && candidate->domain.Value() == expectedDomain && error.code.Value() == candidate->code.Value();
        });
        if (descriptor == descriptors.end())
            return std::nullopt;
        return DiagnosticCode{std::string{(*descriptor)->code.Value()}};
    }

    /** @copydoc DiagnosticSeverityForError */
    std::optional<DiagnosticSeverity> DiagnosticSeverityForError(const ErrorSeverity severity) noexcept {
        DiagnosticSeverity diagnosticSeverity{};
        switch (severity) {
            case ErrorSeverity::Info:
                diagnosticSeverity = DiagnosticSeverity::Note;
                break;
            case ErrorSeverity::Warning:
                diagnosticSeverity = DiagnosticSeverity::Warning;
                break;
            case ErrorSeverity::Error:
                diagnosticSeverity = DiagnosticSeverity::Error;
                break;
            case ErrorSeverity::Critical:
                diagnosticSeverity = DiagnosticSeverity::Fatal;
                break;
            default:
                return std::nullopt;
        }
        return diagnosticSeverity;
    }
}  // namespace Horo
