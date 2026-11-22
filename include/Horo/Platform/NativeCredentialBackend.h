#pragma once

/** @file NativeCredentialBackend.h @brief Host composition for operating-system credential storage. */

#include "Horo/Security/CredentialStore.h"

#include <memory>

namespace Horo::Platform {
    /** @brief Create the native per-user, machine-local credential backend.
     * @return A stateless Windows Credential Manager backend or an empty pointer on unsupported platforms.
     * @details Creation performs no credential lookup. The host retains storage authority and supplies typed references.
     */
    [[nodiscard]] std::shared_ptr<Security::CredentialBackend> CreateNativeCredentialBackend();
}  // namespace Horo::Platform
