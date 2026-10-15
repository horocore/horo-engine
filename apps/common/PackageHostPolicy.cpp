#include "PackageHostPolicy.h"

#include "Horo/Extensions/ExtensionAbi.h"

namespace Horo::Application::Internal {
    /** @copydoc NativePackagePlatform */
    Result<Packages::PackagePlatform> NativePackagePlatform() {
#if defined(_WIN32)
        const char *system = "windows";
#elif defined(__APPLE__)
        const char *system = "macos";
#elif defined(__linux__)
        const char *system = "linux";
#else
        const char *system = nullptr;
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
        const char *architecture = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
        const char *architecture = "x64";
#else
        const char *architecture = nullptr;
#endif
        if (system == nullptr || architecture == nullptr)
            return Result<Packages::PackagePlatform>::Failure(
                Error{ErrorCode{"packages.host.platform_unavailable"}, ErrorDomainId{"packages.host"}});
        return Result<Packages::PackagePlatform>::Success(
            {system, architecture, "horo-extension-abi-" + std::to_string(HORO_EXTENSION_ABI_VERSION)});
    }
}  // namespace Horo::Application::Internal
