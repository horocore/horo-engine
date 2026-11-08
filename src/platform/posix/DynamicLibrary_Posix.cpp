#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <dlfcn.h>
#include <filesystem>
#include <string>

namespace Horo::Platform {
    namespace {
        constexpr std::size_t MaximumLibraryPathBytes = 1024;

        class PosixDynamicLibrary final : public DynamicLibrary {
        public:
            explicit PosixDynamicLibrary(void *handle) : m_handle(handle) {}  // NOSONAR(cpp:S5008)

            ~PosixDynamicLibrary() override {
                if (m_handle) {
                    dlclose(m_handle);
                }
            }

            PosixDynamicLibrary(const PosixDynamicLibrary &) = delete;
            PosixDynamicLibrary &operator=(const PosixDynamicLibrary &) = delete;
            PosixDynamicLibrary(PosixDynamicLibrary &&) = delete;
            PosixDynamicLibrary &operator=(PosixDynamicLibrary &&) = delete;

            [[nodiscard]] void *GetSymbol(std::string_view name) const noexcept override {  // NOSONAR(cpp:S5008)
                // dlsym requires null-terminated string
                std::string symbolName{name};
                return dlsym(m_handle, symbolName.c_str());
            }

        private:
            void *m_handle = nullptr;  // NOSONAR(cpp:S5008)
        };
    }  // namespace

    /** @copydoc LoadDynamicLibrary */
    Result<std::unique_ptr<DynamicLibrary>> LoadDynamicLibrary(std::string_view path) {
        if (path.size() > MaximumLibraryPathBytes)
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(
                MakeError(PlatformErrors::InvalidFormat, "Library path exceeds the supported length"));
        return LoadDynamicLibraryAtPath(std::filesystem::path{path});
    }

    /** @copydoc LoadDynamicLibraryAtPath */
    Result<std::unique_ptr<DynamicLibrary>> LoadDynamicLibraryAtPath(const std::filesystem::path &path) {
        const auto &native = path.native();
        if (native.size() > MaximumLibraryPathBytes || native.find('\0') != std::string::npos || !path.is_absolute() ||
            std::ranges::any_of(path, [](const std::filesystem::path &part) {
            return part == "..";
        }))
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(
                MakeError(PlatformErrors::InvalidFormat, "Library path is invalid or contains traversal"));
        std::error_code error;
        const std::filesystem::path canonicalPath = std::filesystem::canonical(path, error);
        if (error || !canonicalPath.is_absolute() || !std::filesystem::is_regular_file(canonicalPath, error))
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(
                MakeError(PlatformErrors::InvalidFormat, "Library path does not resolve to a regular file"));
        // Loading project-produced native code is an explicit host capability; the path is canonical and absolute.
        void *handle = dlopen(canonicalPath.c_str(), RTLD_NOW | RTLD_LOCAL);  // NOSONAR
        if (!handle) {
            const char *nativeError = dlerror();
            std::string errorMsg = nativeError != nullptr ? nativeError : "Unknown dlopen error";
            LOG_ERROR("platform.dynamic_library", "Failed to load the validated dynamic library: %s", errorMsg.c_str());
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(MakeError(PlatformErrors::InvalidFormat, errorMsg));
        }

        return Result<std::unique_ptr<DynamicLibrary>>::Success(std::make_unique<PosixDynamicLibrary>(handle));
    }
}  // namespace Horo::Platform
