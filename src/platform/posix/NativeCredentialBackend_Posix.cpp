#include "Horo/Platform/NativeCredentialBackend.h"

namespace Horo::Platform {
    /** @copydoc CreateNativeCredentialBackend */
    std::shared_ptr<Security::CredentialBackend> CreateNativeCredentialBackend() {
        return {};
    }
}  // namespace Horo::Platform
