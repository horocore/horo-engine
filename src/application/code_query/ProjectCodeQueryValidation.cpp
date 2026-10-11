#include "Horo/Foundation/Utf8.h"
#include "ProjectCodeQueryInternal.h"

#include <algorithm>

namespace Horo::Application::CodeQueryErrors {
    namespace {
        /** @brief Declares stable application identities, without exposing paths or foreign exception text. */
        ErrorCodeDescriptor Descriptor(const char *code, const char *summary) {
            return {ErrorDomainId{"application.code_query"}, ErrorCode{code}, ErrorSeverity::Error, summary,
                    "Use a current authorized project and a bounded read-only query."};
        }
    }  // namespace

    const ErrorCodeDescriptor Invalid = Descriptor("invalid", "The code query is malformed.");
    const ErrorCodeDescriptor UnsafePath = Descriptor("unsafe_path", "The code query path is unsafe or not project-contained.");
    const ErrorCodeDescriptor ReadFailed = Descriptor("read_failed", "The project file could not be read consistently.");
    const ErrorCodeDescriptor Binary = Descriptor("binary", "The requested file contains binary data.");
    const ErrorCodeDescriptor Encoding = Descriptor("encoding", "The requested text is not canonical UTF-8.");
    const ErrorCodeDescriptor Capacity = Descriptor("capacity", "The code query exceeds a finite resource limit.");
    const ErrorCodeDescriptor Stale = Descriptor("stale", "The project or query observation revision changed.");
    const ErrorCodeDescriptor Unavailable = Descriptor("unavailable", "The requested code observation capability is unavailable.");
}  // namespace Horo::Application::CodeQueryErrors

namespace Horo::Application::CodeQueryDetail {
    /** @copydoc Scope::Check */
    Result<void> Scope::Check() const {
        if (auto stop = Platform::CheckProjectReadContext(context); stop.HasError())
            return stop;
        if (std::chrono::steady_clock::now() >= deadline)
            return Result<void>::Failure(MakeError(Platform::ProjectReadErrors::Deadline));
        return Result<void>::Success();
    }

    /** @copydoc ValidPath */
    bool ValidPath(const std::string_view path, const bool allowRoot) {
        return Platform::IsSafeProjectReadPath(path, allowRoot);
    }

    /** @copydoc ValidLimits */
    bool ValidLimits(const CodeQueryLimits &l) {
        const CodeQueryLimits hard;
        const auto bounded = [](const auto value, const auto maximum) {
            return value > 0 && value <= maximum;
        };
        return bounded(l.maximumFiles, hard.maximumFiles) && bounded(l.maximumFileBytes, hard.maximumFileBytes) &&
               bounded(l.maximumObservationBytes, hard.maximumObservationBytes) &&
               bounded(l.maximumPatternBytes, hard.maximumPatternBytes) && bounded(l.maximumPageItems, hard.maximumPageItems) &&
               bounded(l.maximumTextPageBytes, hard.maximumTextPageBytes) && bounded(l.maximumSearchSteps, hard.maximumSearchSteps) &&
               l.maximumDuration.count() > 0 && l.maximumDuration <= hard.maximumDuration;
    }

    /** @copydoc ValidateText */
    Result<void> ValidateText(const std::string_view text, const Scope &scope) {
        if (!IsValidUtf8ScalarSequence(text))
            return Result<void>::Failure(MakeError(CodeQueryErrors::Encoding));
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (i % 4096 == 0) {
                if (auto stop = scope.Check(); stop.HasError())
                    return stop;
            }
            const auto c = static_cast<unsigned char>(text[i]);
            if ((c < 32 && c != '\t' && c != '\n' && c != '\r' && c != '\f') || c == 127)
                return Result<void>::Failure(MakeError(CodeQueryErrors::Binary));
        }
        return scope.Check();
    }

    /** @copydoc Fence */
    Result<void> Fence(const CodeQueryRequest &request, const std::string_view revision, const std::size_t size) {
        if (revision.empty() || revision.size() > 256 || !IsValidUtf8ScalarSequence(revision) || request.offset > size ||
            (request.offset != 0 && !request.expectedRevision))
            return Result<void>::Failure(MakeError(CodeQueryErrors::Invalid));
        if (request.expectedRevision && *request.expectedRevision != revision)
            return Result<void>::Failure(MakeError(CodeQueryErrors::Stale));
        return Result<void>::Success();
    }
}  // namespace Horo::Application::CodeQueryDetail
