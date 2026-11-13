#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Platform/DynamicLibrary.h"
#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <windows.h>

namespace Horo::Platform {
    namespace {
        class WindowsDynamicLibrary final : public DynamicLibrary {
        public:
            explicit WindowsDynamicLibrary(const wchar_t *path) : m_handle(LoadLibraryW(path)) {}

            WindowsDynamicLibrary(const WindowsDynamicLibrary &) = delete;
            WindowsDynamicLibrary &operator=(const WindowsDynamicLibrary &) = delete;
            WindowsDynamicLibrary(WindowsDynamicLibrary &&) = delete;
            WindowsDynamicLibrary &operator=(WindowsDynamicLibrary &&) = delete;

            [[nodiscard]] bool IsLoaded() const noexcept {
                return m_handle != nullptr;
            }

            ~WindowsDynamicLibrary() override {
                if (m_handle) {
                    FreeLibrary(m_handle);
                }
            }

            [[nodiscard]] void *GetSymbol(std::string_view name) const noexcept override {
                if (name.empty() || name.find('\0') != std::string_view::npos)
                    return nullptr;
                try {
                    const std::string symbolName{name};
                    return reinterpret_cast<void *>(GetProcAddress(m_handle, symbolName.c_str()));
                } catch (const std::bad_alloc &) {
                    return nullptr;
                } catch (const std::length_error &) {
                    return nullptr;
                }
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
        const std::filesystem::path canonicalPath =
            std::filesystem::canonical(Foundation::Paths::NativeApiPath(candidate), filesystemError);
        const auto nativePath = Foundation::Paths::NativeApiPath(canonicalPath);
        if (filesystemError || !std::filesystem::is_regular_file(nativePath, filesystemError))
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(
                MakeError(PlatformErrors::InvalidFormat, "Library path does not resolve to a regular file"));
        auto library = std::make_unique<WindowsDynamicLibrary>(nativePath.c_str());
        if (!library->IsLoaded()) {
            DWORD error = GetLastError();
            std::string errorMsg = "LoadLibrary failed with error code: " + std::to_string(error);
            LOG_ERROR("platform.dynamic_library", "Failed to load the validated dynamic library: %s", errorMsg.c_str());
            return Result<std::unique_ptr<DynamicLibrary>>::Failure(MakeError(PlatformErrors::InvalidFormat, errorMsg));
        }

        return Result<std::unique_ptr<DynamicLibrary>>::Success(std::move(library));
    }
}  // namespace Horo::Platform
