#include "Horo/Platform/ProjectReadFiles.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <array>

namespace Horo::Platform::ProjectReadErrors {
    namespace {
        /** @brief Declares stable filesystem failures without native path disclosure. */
        ErrorCodeDescriptor Descriptor(const char *code, const char *summary) {
            return {ErrorDomainId{"horo.platform.project_read"}, ErrorCode{code}, ErrorSeverity::Error, summary,
                    "Use a current authorized root and a bounded read-only operation."};
        }
    }  // namespace

    const ErrorCodeDescriptor Invalid = Descriptor("project_read.invalid", "The read-only request is malformed.");
    const ErrorCodeDescriptor UnsafePath = Descriptor("project_read.unsafe_path", "The file path or native object is unsafe.");
    const ErrorCodeDescriptor ReadFailed = Descriptor("project_read.read_failed", "The file could not be read consistently.");
    const ErrorCodeDescriptor Capacity = Descriptor("project_read.capacity", "The read exceeds a finite resource limit.");
    const ErrorCodeDescriptor Stale = Descriptor("project_read.stale", "The root authority or file observation changed.");
    const ErrorCodeDescriptor Unavailable = Descriptor("project_read.unavailable", "The read-only capability is unavailable.");
    const ErrorCodeDescriptor Cancelled = Descriptor("project_read.cancelled", "The read was cancelled or its authority expired.");
    const ErrorCodeDescriptor Deadline = Descriptor("project_read.deadline", "The read deadline expired.");
}  // namespace Horo::Platform::ProjectReadErrors

namespace Horo::Platform {
    /** @copydoc IProjectReadFiles::SourceText */
    Result<ProjectReadTextSnapshot> IProjectReadFiles::SourceText(std::string_view, const ProjectReadContext &,
                                                                  const ProjectSourceReadLimits &) const {
        return Result<ProjectReadTextSnapshot>::Failure(MakeError(ProjectReadErrors::Unavailable));
    }

    /** @copydoc IProjectReadFiles::Inventory */
    Result<ProjectInventory> IProjectReadFiles::Inventory(std::string_view, ProjectInventoryDepth, const ProjectReadContext &,
                                                          const ProjectInventoryLimits &) const {
        return Result<ProjectInventory>::Failure(MakeError(ProjectReadErrors::Unavailable));
    }

    /** @copydoc IProjectReadFiles::InventoryText */
    Result<ProjectReadTextSnapshot> IProjectReadFiles::InventoryText(std::string_view, const ProjectReadContext &) const {
        return Result<ProjectReadTextSnapshot>::Failure(MakeError(ProjectReadErrors::Unavailable));
    }

    namespace {
        constexpr std::array ForbiddenPathCharacters{'\\', ':', '<', '>', '"', '|', '?', '*'};

        /** @brief Compares portable device names using only ASCII case folding. */
        bool MatchesDevice(const std::string_view value, const std::string_view name) {
            return value.size() == name.size() && std::equal(value.begin(), value.end(), name.begin(), [](const char a, const char b) {
                return (a >= 'a' && a <= 'z' ? static_cast<char>(a - 'a' + 'A') : a) == b;
            });
        }

        /** @brief Rejects Windows device aliases, including their superscript-digit spellings on every platform. */
        bool ReservedDevice(const std::string_view part) {
            const auto stem = part.substr(0, part.find('.'));
            if (MatchesDevice(stem, "CON") || MatchesDevice(stem, "PRN") || MatchesDevice(stem, "AUX") || MatchesDevice(stem, "NUL") ||
                MatchesDevice(stem, "CLOCK$") || MatchesDevice(stem, "CONIN$") || MatchesDevice(stem, "CONOUT$"))
                return true;
            if (!MatchesDevice(stem.substr(0, 3), "COM") && !MatchesDevice(stem.substr(0, 3), "LPT"))
                return false;
            return (stem.size() == 4 && stem[3] >= '1' && stem[3] <= '9') ||
                   (stem.size() == 5 && (stem.ends_with("\xc2\xb9") || stem.ends_with("\xc2\xb2") || stem.ends_with("\xc2\xb3")));
        }

        /** @brief Validates one nonempty portable path component before native traversal. */
        bool SafeComponent(const std::string_view part) {
            return !part.empty() && part != "." && part != ".." && part.back() != '.' && part.back() != ' ' && !ReservedDevice(part);
        }
    }  // namespace

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
                if (const auto byte = static_cast<unsigned char>(path[i]);
                    byte < 32 || byte == 127 || std::ranges::find(ForbiddenPathCharacters, path[i]) != ForbiddenPathCharacters.end())
                    return false;
                continue;
            }
            if (!SafeComponent(path.substr(start, i - start)))
                return false;
            start = i + 1;
        }
        return true;
    }
}  // namespace Horo::Platform

namespace Horo::Platform {
    /** @copydoc ProjectReadErrorDomain */
    ModuleErrorDomainDescriptor ProjectReadErrorDomain() {
        return {.id = ProjectReadErrors::Invalid.domain,
                .descriptors = {&ProjectReadErrors::Invalid, &ProjectReadErrors::UnsafePath, &ProjectReadErrors::ReadFailed,
                                &ProjectReadErrors::Capacity, &ProjectReadErrors::Stale, &ProjectReadErrors::Unavailable,
                                &ProjectReadErrors::Cancelled, &ProjectReadErrors::Deadline}};
    }
}  // namespace Horo::Platform
