#include "Horo/Platform/ConfigurationFileStore.h"

#include "Horo/Platform/PlatformErrors.h"
#include "PlatformFileInternal.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Horo {
    namespace {
        [[nodiscard]] std::filesystem::path TransactionPath(const std::filesystem::path &destination, const std::string_view suffix) {
            std::filesystem::path result = destination;
            result += suffix;
            return result;
        }

#if defined(_WIN32)
        /** @brief Preserves the native failure identity without including configuration bytes. */
        [[nodiscard]] Error WindowsReadFailure(const std::filesystem::path &path, const DWORD nativeError) {
            auto error = PlatformFileInternal::FsError(PlatformErrors::ConfigurationReadFailed, path);
            error.message += " Win32 error: " + std::to_string(nativeError);
            return error;
        }

        /** @brief Opens one load-time document through bounded transient replacement conflicts.
         * @param path Native input identity. @param nativeError Receives the last native failure, or zero on success.
         * @return An owned handle for the caller to close, or INVALID_HANDLE_VALUE after at most sixteen attempts. */
        [[nodiscard]] HANDLE OpenWindowsDocument(const std::filesystem::path &path, DWORD &nativeError) {
            constexpr unsigned maximumAttempts = 16;
            for (unsigned attempt = 0; attempt < maximumAttempts; ++attempt) {
                const HANDLE handle = CreateFileW(PlatformFileInternal::NativeApiPath(path).c_str(), GENERIC_READ,
                                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
                nativeError = handle == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
                const bool transient =
                    nativeError == ERROR_SHARING_VIOLATION || nativeError == ERROR_ACCESS_DENIED || nativeError == ERROR_DELETE_PENDING;
                if (handle != INVALID_HANDLE_VALUE || !transient || attempt + 1 == maximumAttempts)
                    return handle;
                Sleep(1);
            }
            return INVALID_HANDLE_VALUE;
        }

        /** @brief Retains the opened document through concurrent native rename/replacement without denying delete sharing. */
        [[nodiscard]] Result<std::string> ReadWindowsDocument(const std::filesystem::path &path, const std::size_t maximumBytes) {
            DWORD nativeError = ERROR_SUCCESS;
            const HANDLE handle = OpenWindowsDocument(path, nativeError);
            if (handle == INVALID_HANDLE_VALUE)
                return Result<std::string>::Failure(WindowsReadFailure(path, nativeError));
            const auto close = [](void *file) {
                static_cast<void>(CloseHandle(file));
            };
            const std::unique_ptr<void, decltype(close)> file{handle, close};
            std::string document;
            document.reserve(std::min<std::size_t>(maximumBytes, 64 * 1024));
            std::array<char, 8192> buffer{};
            for (;;) {
                DWORD count = 0;
                if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                    return Result<std::string>::Failure(WindowsReadFailure(path, GetLastError()));
                if (count == 0)
                    return Result<std::string>::Success(std::move(document));
                if (document.size() > maximumBytes || count > maximumBytes - document.size())
                    return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationFileTooLarge, path));
                document.append(buffer.data(), count);
            }
        }
#endif
    }  // namespace

    /** @copydoc ConfigurationFileStore::ConfigurationFileStore */
    ConfigurationFileStore::ConfigurationFileStore(DurableFileSystem &files) noexcept : files_(files) {}

    /** @copydoc ConfigurationFileStore::Read */
    Result<std::string> ConfigurationFileStore::Read(const std::filesystem::path &path, const ConfigurationLimits &limits) const {
        if (path.empty())
            return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationReadFailed, path));
        const auto maximumBytes = std::min(limits.maximumDocumentBytes, ConfigurationLimits{}.maximumDocumentBytes);
#if defined(_WIN32)
        return ReadWindowsDocument(path, maximumBytes);
#else
        std::ifstream input(PlatformFileInternal::NativeApiPath(path), std::ios::binary);
        if (!input)
            return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationReadFailed, path));

        std::string document;
        document.reserve(std::min<std::size_t>(maximumBytes, 64 * 1024));
        std::array<char, 8192> buffer{};
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count <= 0)
                break;
            const auto byteCount = static_cast<std::size_t>(count);
            if (document.size() > maximumBytes || byteCount > maximumBytes - document.size())
                return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationFileTooLarge, path));
            document.append(buffer.data(), byteCount);
        }
        if (input.bad())
            return Result<std::string>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationReadFailed, path));
        return Result<std::string>::Success(std::move(document));
#endif
    }

    /** @copydoc ConfigurationFileStore::Write */
    Result<void> ConfigurationFileStore::Write(const std::filesystem::path &path, const ConfigurationSnapshot &snapshot,
                                               const ConfigurationLimits &limits) const {
        if (path.empty())
            return Result<void>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationWriteFailed, path));
        const std::string document = snapshot.ToJson();
        if (document.size() > std::min(limits.maximumDocumentBytes, ConfigurationLimits{}.maximumDocumentBytes))
            return Result<void>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationFileTooLarge, path));
        std::error_code pathError;
        const std::filesystem::path destination = std::filesystem::absolute(path, pathError);
        if (pathError)
            return Result<void>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationWriteFailed, path));
        const std::filesystem::path parent = destination.parent_path();
        const std::filesystem::path lockPath = TransactionPath(destination, ".lock");
        const std::filesystem::path preparedPath = TransactionPath(destination, ".tmp");

        if (Result<ExclusiveFileLock> acquired = files_.TryAcquireExclusive(lockPath, "horo.configuration"); acquired.HasError()) {
            return Result<void>::Failure(acquired.ErrorValue());
        } else {
            Result<std::uint64_t> available = files_.AvailableBytes(parent);
            if (available.HasError())
                return Result<void>::Failure(available.ErrorValue());
            if (available.Value() < document.size())
                return Result<void>::Failure(PlatformFileInternal::FsError(PlatformErrors::ConfigurationWriteFailed, path));

            const std::span<const char> characters{document.data(), document.size()};
            if (auto removed = files_.RemoveDurable(preparedPath); removed.HasError())
                return removed;
            bool created = false;
            if (Result<void> written = files_.WritePrivateDurable(preparedPath, std::as_bytes(characters), created); written.HasError()) {
                if (created)
                    static_cast<void>(files_.RemoveDurable(preparedPath));
                return Result<void>::Failure(written.ErrorValue());
            }
            if (Result<void> replaced = files_.AtomicReplace(preparedPath, destination); replaced.HasError()) {
                static_cast<void>(files_.RemoveDurable(preparedPath));
                return Result<void>::Failure(replaced.ErrorValue());
            }
            return Result<void>::Success();
        }
    }
}  // namespace Horo
