/**
 * @file
 * @brief Private error and durability primitives shared by native filesystem implementations.
 */
#pragma once

#include "Horo/Foundation/Platform.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Horo::PlatformFileInternal {
    inline const ErrorDomainId PlatformDomain{"horo.platform.filesystem"};
    inline const ErrorCodeDescriptor IoFailed{.domain = PlatformDomain,
                                              .code = ErrorCode{"filesystem.io_failed"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Durable filesystem operation failed.",
                                              .remediationHint = "Check filesystem permissions and available storage.",
                                              .retryable = true,
                                              .userActionable = true};
    inline const ErrorCodeDescriptor LockBusy{.domain = PlatformDomain,
                                              .code = ErrorCode{"filesystem.lock_busy"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The file is locked by another operation.",
                                              .remediationHint = "Wait for the other project mutation to finish.",
                                              .retryable = true,
                                              .userActionable = true};

    /** @brief Preserves the failing native path in a typed filesystem error. */
    [[nodiscard]] inline Error FsError(const ErrorCodeDescriptor &code, const std::filesystem::path &path) {
        return MakeError(code, std::string(code.summary) + " Path: " + path.generic_string());
    }

#if !defined(_WIN32)
    /** @brief Flushes file data with the strongest supported native durability primitive. */
    [[nodiscard]] inline bool FlushFileDescriptor(const int descriptor) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
        if (fcntl(descriptor, F_FULLFSYNC) == 0)
            return true;
#endif
        return fsync(descriptor) == 0;
    }

#endif
}  // namespace Horo::PlatformFileInternal
