#pragma once

/** @file FileManager.h
 * @brief Desktop file-manager dispatch without native types or shell command strings.
 */

#include "Horo/Foundation/Result.h"

#include <filesystem>

namespace Horo {
    namespace FileManagerErrors {
        extern const ErrorCodeDescriptor InvalidTarget;  /**< Missing, relative, linked or unsupported filesystem target. */
        extern const ErrorCodeDescriptor Unavailable;    /**< This host has no desktop file-manager implementation. */
        extern const ErrorCodeDescriptor DispatchFailed; /**< The desktop rejected the request. */
    }  // namespace FileManagerErrors

    /** @brief Opens a directory, or reveals a regular file in its containing directory.
     * @param absolutePath Existing absolute native path; a symbolic link at the target and embedded NULs are rejected.
     * @return Success when the desktop accepts dispatch, otherwise a typed validation, availability or native failure.
     * @pre Call on the graphical host's main thread following an explicit user action.
     * @note Windows and macOS select files. Linux opens the containing directory through the desktop URI handler.
     * A successful dispatch does not guarantee that the external application finished opening. Headless hosts return Unavailable.
     * @throws std::bad_alloc When request or error storage cannot be allocated.
     */
    [[nodiscard]] Result<void> RevealInFileManager(const std::filesystem::path &absolutePath);
}  // namespace Horo
