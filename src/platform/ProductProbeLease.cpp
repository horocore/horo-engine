#include "Horo/Foundation/Platform.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Horo {
    namespace {
        const ErrorDomainId PlatformDomain{"horo.platform.filesystem"};
        const ErrorCodeDescriptor InvalidLease{.domain = PlatformDomain,
                                               .code = ErrorCode{"filesystem.invalid_inherited_lease"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Inherited product maintenance lease is invalid.",
                                               .remediationHint = "Launch the product through a trusted installer host.",
                                               .retryable = false,
                                               .userActionable = false};

        [[nodiscard]] Error LeaseError(const std::filesystem::path &path) {
            return MakeError(InvalidLease, std::string(InvalidLease.summary) + " Path: " + path.generic_string());
        }

        [[nodiscard]] bool ReadTransferNumber(std::uintptr_t &native) {
            std::array<char, 32> value{};
#if defined(_WIN32)
            const DWORD length = GetEnvironmentVariableA("HORO_PRODUCT_PROBE_LEASE", value.data(), static_cast<DWORD>(value.size()));
            if (length == 0 || length >= value.size())
                return false;
#else
            const char *inherited = std::getenv("HORO_PRODUCT_PROBE_LEASE");
            if (inherited == nullptr)
                return false;
            std::size_t length = 0;
            while (length < value.size() && inherited[length] != '\0')
                ++length;
            if (length == 0 || length == value.size())
                return false;
            std::copy_n(inherited, length, value.data());
#endif
            const auto parsed = std::from_chars(value.data(), value.data() + length, native);
            return parsed.ec == std::errc{} && parsed.ptr == value.data() + length;
        }

#if defined(_WIN32)
        [[nodiscard]] std::wstring FinalPath(const HANDLE source) {
            const DWORD length = GetFinalPathNameByHandleW(source, nullptr, 0, FILE_NAME_NORMALIZED);
            if (length == 0)
                return {};
            std::wstring result(length + 1U, L'\0');
            const DWORD copied = GetFinalPathNameByHandleW(source, result.data(), length + 1U, FILE_NAME_NORMALIZED);
            if (copied == 0 || copied > length)
                return {};
            result.resize(copied);
            return result;
        }

        [[nodiscard]] bool ValidInheritedHandle(const std::uintptr_t native, const std::filesystem::path &rootPath) {
            const auto handle = reinterpret_cast<HANDLE>(native);
            BY_HANDLE_FILE_INFORMATION info{};
            if (handle == nullptr || handle == INVALID_HANDLE_VALUE || GetFileType(handle) != FILE_TYPE_DISK ||
                !GetFileInformationByHandle(handle, &info) ||
                (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U || info.nNumberOfLinks != 1U)
                return false;
            const HANDLE root = CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            BY_HANDLE_FILE_INFORMATION rootInfo{};
            const bool validRoot = root != INVALID_HANDLE_VALUE && GetFileInformationByHandle(root, &rootInfo) &&
                                   (rootInfo.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U &&
                                   (rootInfo.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0U;
            const auto actual = FinalPath(handle);
            std::wstring expected = validRoot ? FinalPath(root) : std::wstring{};
            if (root != INVALID_HANDLE_VALUE)
                CloseHandle(root);
            if (expected.empty() || actual.empty())
                return false;
            if (expected.back() != L'\\')
                expected.push_back(L'\\');
            expected.append(L".product-launch.lock");
            return _wcsicmp(actual.c_str(), expected.c_str()) == 0 && SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
        }
#else
        [[nodiscard]] bool ValidInheritedDescriptor(const std::uintptr_t native, const std::filesystem::path &path) {
            if (native > static_cast<std::uintptr_t>(std::numeric_limits<int>::max()))
                return false;
            const auto descriptor = static_cast<int>(native);
            struct stat inheritedInfo{};
            struct stat expectedInfo{};
            const int expected = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            const bool valid = descriptor >= 0 && expected >= 0 && fstat(descriptor, &inheritedInfo) == 0 &&
                               fstat(expected, &expectedInfo) == 0 && S_ISREG(inheritedInfo.st_mode) && inheritedInfo.st_nlink == 1 &&
                               inheritedInfo.st_dev == expectedInfo.st_dev && inheritedInfo.st_ino == expectedInfo.st_ino &&
                               flock(descriptor, LOCK_EX | LOCK_NB) == 0 && fcntl(descriptor, F_SETFD, FD_CLOEXEC) == 0;
            if (expected >= 0)
                close(expected);
            return valid;
        }
#endif
    }  // namespace

    /** @copydoc NativeDurableFileSystem::AdoptInheritedProductMaintenance */
    Result<ProductLaunchLease> NativeDurableFileSystem::AdoptInheritedProductMaintenance(
        const std::filesystem::path &installationRoot) const {
        const auto path = installationRoot / ".product-launch.lock";
        if (!installationRoot.is_absolute() || std::ranges::any_of(installationRoot, [](const auto &part) {
            return part == "." || part == "..";
        }))
            return Result<ProductLaunchLease>::Failure(LeaseError(path));
        std::uintptr_t native{};
        if (!ReadTransferNumber(native))
            return Result<ProductLaunchLease>::Failure(LeaseError(path));
#if defined(_WIN32)
        if (!ValidInheritedHandle(native, installationRoot))
            return Result<ProductLaunchLease>::Failure(LeaseError(path));
        static_cast<void>(SetEnvironmentVariableA("HORO_PRODUCT_PROBE_LEASE", nullptr));
#else
        if (!ValidInheritedDescriptor(native, path))
            return Result<ProductLaunchLease>::Failure(LeaseError(path));
        static_cast<void>(unsetenv("HORO_PRODUCT_PROBE_LEASE"));
#endif
        return Result<ProductLaunchLease>::Success(ProductLaunchLease::AdoptMaintenanceNative(native));
    }
}  // namespace Horo
