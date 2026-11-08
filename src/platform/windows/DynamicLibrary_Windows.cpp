#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <filesystem>
#include <string>
#include <windows.h>

namespace Horo::Platform {
    namespace {
        class WindowsDynamicLibrary final : public DynamicLibrary {
        public:
            explicit WindowsDynamicLibrary(HMODULE handle) : m_handle(handle) {}

            ~WindowsDynamicLibrary() override {
                if (m_handle) {
                    FreeLibrary(m_handle);
                }
            }

            [[nodiscard]] void *GetSymbol(std::string_view name) const noexcept override {
                std::string symbolName{name};
                return reinterpret_cast<void *>(GetProcAddress(m_handle, symbolName.c_str()));
            }

        private:
            HMODULE m_handle = nullptr;
        };
    }  // namespace

    /** @copydoc LoadDynamicLibrary */
    Result<std::unique_ptr<DynamicLibrary>> LoadDynamicLibrary(std::string_view path) {
        return LoadDynamicLibraryAtPath(std::filesystem::path{path});
    }

    /** @copydoc LoadDynamicLibraryAtPath */
    Result<std::unique_ptr<DynamicLibrary>> LoadDynamicLibraryAtPath(const std::filesystem::path &candidate) {
        if (!candidate.is_absolute() || candidate.native().find(L'\0') != std::wstring::npos ||
            std::ranges::any_of(candidate, [](const std::filesystem::path &part) {
            return part == "..";
        }))
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(
                MakeError(PlatformErrors::InvalidFormat, "Library path is invalid or contains traversal"));
        std::error_code filesystemError;
        const std::filesystem::path canonicalPath = std::filesystem::canonical(candidate, filesystemError);
        if (filesystemError || !std::filesystem::is_regular_file(canonicalPath, filesystemError))
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(
                MakeError(PlatformErrors::InvalidFormat, "Library path does not resolve to a regular file"));
        HMODULE handle = LoadLibraryW(canonicalPath.c_str());
        if (!handle) {
            DWORD error = GetLastError();
            std::string errorMsg = "LoadLibrary failed with error code: " + std::to_string(error);
            LOG_ERROR("platform.dynamic_library", "Failed to load the validated dynamic library: %s", errorMsg.c_str());
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(MakeError(PlatformErrors::InvalidFormat, errorMsg));
        }

        return Result<std::unique_ptr<DynamicLibrary>>::Success(std::make_unique<WindowsDynamicLibrary>(handle));
    }
}  // namespace Horo::Platform
