#include "Horo/Platform/ProjectReadFiles.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>

namespace Horo::Platform::ProjectReadErrors {
    namespace {
        /** @brief Declares stable filesystem failures without native path disclosure. */
        ErrorCodeDescriptor Descriptor(const char *code, const char *summary) {
            return {ErrorDomainId{"horo.platform.project_read"}, ErrorCode{code}, ErrorSeverity::Error, summary,
                    "Use a current authorized root and a bounded read-only operation."};
        }
    }  // namespace

    const ErrorCodeDescriptor Invalid = Descriptor("invalid", "The read-only request is malformed.");
    const ErrorCodeDescriptor UnsafePath = Descriptor("unsafe_path", "The file path or native object is unsafe.");
    const ErrorCodeDescriptor ReadFailed = Descriptor("read_failed", "The file could not be read consistently.");
    const ErrorCodeDescriptor Capacity = Descriptor("capacity", "The read exceeds a finite resource limit.");
    const ErrorCodeDescriptor Stale = Descriptor("stale", "The root authority or file observation changed.");
    const ErrorCodeDescriptor Unavailable = Descriptor("unavailable", "The read-only capability is unavailable.");
    const ErrorCodeDescriptor Cancelled = Descriptor("cancelled", "The read was cancelled or its authority expired.");
    const ErrorCodeDescriptor Deadline = Descriptor("deadline", "The read deadline expired.");
}  // namespace Horo::Platform::ProjectReadErrors

namespace Horo::Platform {
    /** @copydoc CheckProjectReadContext */
    Result<void> CheckProjectReadContext(const ProjectReadContext &context) {
        if (context.cancellation.IsCancellationRequested() || (context.authorityStopped && context.authorityStopped()))
            return Result<void>::Failure(MakeError(ProjectReadErrors::Cancelled));
        if (std::chrono::steady_clock::now() >= context.deadline)
            return Result<void>::Failure(MakeError(ProjectReadErrors::Deadline));
        return Result<void>::Success();
    }

    /** @copydoc IsSafeProjectReadPath */
    bool IsSafeProjectReadPath(const std::string_view path, const bool allowRoot) {
        if (path.empty())
            return allowRoot;
        if (path.size() > 1024 || path.front() == '/' || path.back() == '/' || !IsValidUtf8ScalarSequence(path))
            return false;
        std::size_t start{};
        for (std::size_t i = 0; i <= path.size(); ++i) {
            if (i < path.size() && path[i] != '/') {
                const auto byte = static_cast<unsigned char>(path[i]);
                if (byte < 32 || byte == 127 || std::string_view{"\\:<>\"|?*"}.find(path[i]) != std::string_view::npos)
                    return false;
                continue;
            }
            const auto part = path.substr(start, i - start);
            if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ')
                return false;
            const auto stem = part.substr(0, part.find('.'));
            const auto matches = [](const std::string_view value, const std::string_view name) {
                return value.size() == name.size() && std::equal(value.begin(), value.end(), name.begin(), [](const char a, const char b) {
                    return (a >= 'a' && a <= 'z' ? static_cast<char>(a - 'a' + 'A') : a) == b;
                });
            };
            if (matches(stem, "CON") || matches(stem, "PRN") || matches(stem, "AUX") || matches(stem, "NUL") || matches(stem, "CLOCK$") ||
                matches(stem, "CONIN$") || matches(stem, "CONOUT$") ||
                ((matches(stem.substr(0, 3), "COM") || matches(stem.substr(0, 3), "LPT")) &&
                 (stem.substr(3) == "\xc2\xb9" || stem.substr(3) == "\xc2\xb2" || stem.substr(3) == "\xc2\xb3")) ||
                (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9' &&
                 (matches(stem.substr(0, 3), "COM") || matches(stem.substr(0, 3), "LPT"))))
                return false;
            start = i + 1;
        }
        return true;
    }
}  // namespace Horo::Platform
