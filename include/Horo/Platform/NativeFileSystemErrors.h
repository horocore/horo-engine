#pragma once

/** @file NativeFileSystemErrors.h @brief Inert metadata for native durable filesystem failures. */
#include "Horo/Foundation/ModuleDescriptor.h"

namespace Horo::Platform {
    /** @brief Describes errors produced by NativeDurableFileSystem without inspecting or mutating storage.
     * @return Independently owned inert metadata with process-lifetime immutable descriptor pointers.
     */
    [[nodiscard]] ModuleErrorDomainDescriptor NativeFileSystemErrorDomain();
}  // namespace Horo::Platform
