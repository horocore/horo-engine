#include "Horo/Platform/NativeFileSystemErrors.h"

#include "PlatformFileInternal.h"

namespace Horo::Platform {
    /** @copydoc NativeFileSystemErrorDomain */
    ModuleErrorDomainDescriptor NativeFileSystemErrorDomain() {
        return {.id = PlatformFileInternal::IoFailed.domain,
                .descriptors = {&PlatformFileInternal::IoFailed, &PlatformFileInternal::LockBusy}};
    }
}  // namespace Horo::Platform
