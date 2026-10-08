/**
 * @file
 * @brief Owns native publication writer exclusion and its process-local lifetime registry.
 */

#include "Horo/Foundation/Platform.h"
#include "PlatformFileInternal.h"

#include <cerrno>
#include <mutex>
#include <unordered_set>
#include <utility>

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
    using PlatformFileInternal::FsError;
    using PlatformFileInternal::IoFailed;
    using PlatformFileInternal::LockBusy;
#if !defined(_WIN32)
    using PlatformFileInternal::FlushFileDescriptor;
#endif
    namespace {
        struct StringHash {
            using is_transparent = void;

            [[nodiscard]] std::size_t operator()(const std::string_view sv) const noexcept {
                return std::hash<std::string_view>{}(sv);
            }
        };

        struct ProcessLockRegistry {
            std::mutex mutex;
            std::unordered_set<std::string, StringHash, std::equal_to<>> locks;
        };

        ProcessLockRegistry &GetProcessLockRegistry() {
            static ProcessLockRegistry registry;
            return registry;
        }

        [[nodiscard]] std::string LockKey(const std::filesystem::path &path) {
            std::error_code error;
            const auto parent = std::filesystem::weakly_canonical(path.parent_path(), error);
            return (error ? path.lexically_normal() : parent / path.filename()).generic_string();
        }

    }  // namespace

    struct ExclusiveFileLock::State {
        State() = default;
        State(const State &) = delete;
        State &operator=(const State &) = delete;

        State(State &&other) noexcept
            : processKey(std::move(other.processKey)), processRegistered(std::exchange(other.processRegistered, false))
#if defined(_WIN32)
              ,
              handle(std::exchange(other.handle, INVALID_HANDLE_VALUE))
#else
              ,
              descriptor(std::exchange(other.descriptor, -1))
#endif
        {
        }

        State &operator=(State &&other) noexcept {
            if (this != &other) {
                Release();
                processKey = std::move(other.processKey);
                processRegistered = std::exchange(other.processRegistered, false);
#if defined(_WIN32)
                handle = std::exchange(other.handle, INVALID_HANDLE_VALUE);
#else
                descriptor = std::exchange(other.descriptor, -1);
#endif
            }
            return *this;
        }

        std::string processKey;
        bool processRegistered{};
#if defined(_WIN32)
        HANDLE handle{INVALID_HANDLE_VALUE};
#else
        int descriptor{-1};
#endif

        void Release() noexcept {
#if defined(_WIN32)
            if (handle != INVALID_HANDLE_VALUE) {
                CloseHandle(handle);
                handle = INVALID_HANDLE_VALUE;
            }
#else
            if (descriptor >= 0) {
                struct flock unlock = {};
                unlock.l_type = F_UNLCK;
                unlock.l_whence = SEEK_SET;
                static_cast<void>(fcntl(descriptor, F_SETLK, &unlock));
                close(descriptor);
                descriptor = -1;
            }
#endif
            if (processRegistered) {
                auto &registry = GetProcessLockRegistry();
                std::lock_guard lock(registry.mutex);
                registry.locks.erase(processKey);
                processRegistered = false;
                processKey.clear();
            }
        }

        ~State() {
            Release();
        }
    };

    namespace {
        /** @brief Validates native lock authority and creates only its canonical parent directory. */
        [[nodiscard]] Result<std::filesystem::path> PrepareExclusiveLockDirectory(const std::filesystem::path &path,
                                                                                  const std::string_view ownerMetadata) {
            if (!path.is_absolute() || path.filename().empty() || path.filename() == "." || path.filename() == ".." ||
                path.lexically_normal() != path || path.native().find(std::filesystem::path::value_type{}) != std::string::npos ||
                ownerMetadata.size() > 4096U)
                return Result<std::filesystem::path>::Failure(FsError(IoFailed, path));
            std::error_code error;
            const auto canonicalParent = std::filesystem::weakly_canonical(path.parent_path(), error);
            if (error)
                return Result<std::filesystem::path>::Failure(FsError(IoFailed, path));
            std::filesystem::create_directories(canonicalParent, error);
            if (error)
                return Result<std::filesystem::path>::Failure(FsError(IoFailed, path));
            const auto parent = std::filesystem::canonical(canonicalParent, error);
            if (error || parent != canonicalParent)
                return Result<std::filesystem::path>::Failure(FsError(IoFailed, path));
            return Result<std::filesystem::path>::Success(parent / path.filename());
        }

        /** @brief Reserves process exclusion only after allocating the state that will release it. */
        [[nodiscard]] bool ReserveProcessLock(ExclusiveFileLock::State &state, const std::filesystem::path &path) {
            state.processKey = LockKey(path);
            auto &registry = GetProcessLockRegistry();
            std::lock_guard lock(registry.mutex);
            if (!registry.locks.emplace(state.processKey).second)
                return false;
            state.processRegistered = true;
            return true;
        }

#if defined(_WIN32)
        /** @brief Acquires Windows sharing exclusion and verifies a single-link regular native lock file. */
        [[nodiscard]] Result<void> AcquireNativeExclusiveLock(ExclusiveFileLock::State &state, const std::filesystem::path &path) {
            state.handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (state.handle == INVALID_HANDLE_VALUE)
                return Result<void>::Failure(FsError(GetLastError() == ERROR_SHARING_VIOLATION ? LockBusy : IoFailed, path));
            BY_HANDLE_FILE_INFORMATION information{};
            if (!GetFileInformationByHandle(state.handle, &information) || GetFileType(state.handle) != FILE_TYPE_DISK ||
                (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0U ||
                information.nNumberOfLinks != 1U)
                return Result<void>::Failure(FsError(IoFailed, path));
            return Result<void>::Success();
        }

        /** @brief Writes diagnostic-only metadata after Windows exclusion has been acquired. */
        [[nodiscard]] Result<void> WriteExclusiveLockMetadata(const ExclusiveFileLock::State &state, const std::filesystem::path &path,
                                                              const std::string_view ownerMetadata) {
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(state.handle, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(state.handle))
                return Result<void>::Failure(FsError(IoFailed, path));
            DWORD written{};
            if (!ownerMetadata.empty() &&
                (!WriteFile(state.handle, ownerMetadata.data(), static_cast<DWORD>(ownerMetadata.size()), &written, nullptr) ||
                 written != ownerMetadata.size()))
                return Result<void>::Failure(FsError(IoFailed, path));
            return FlushFileBuffers(state.handle) ? Result<void>::Success() : Result<void>::Failure(FsError(IoFailed, path));
        }
#else
        /** @brief Acquires nonblocking POSIX record-lock authority on a verified single-link regular file. */
        [[nodiscard]] Result<void> AcquireNativeExclusiveLock(ExclusiveFileLock::State &state, const std::filesystem::path &path) {
            state.descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (state.descriptor < 0)
                return Result<void>::Failure(FsError(IoFailed, path));
            if (struct stat information{};
                fstat(state.descriptor, &information) != 0 || !S_ISREG(information.st_mode) || information.st_nlink != 1)
                return Result<void>::Failure(FsError(IoFailed, path));
            struct flock lock = {};
            lock.l_type = F_WRLCK;
            lock.l_whence = SEEK_SET;
            if (fcntl(state.descriptor, F_SETLK, &lock) != 0)
                return Result<void>::Failure(FsError(errno == EACCES || errno == EAGAIN ? LockBusy : IoFailed, path));
            return Result<void>::Success();
        }

        /** @brief Writes diagnostic-only metadata after POSIX record-lock authority has been acquired. */
        [[nodiscard]] Result<void> WriteExclusiveLockMetadata(const ExclusiveFileLock::State &state, const std::filesystem::path &path,
                                                              const std::string_view ownerMetadata) {
            if (ftruncate(state.descriptor, 0) != 0)
                return Result<void>::Failure(FsError(IoFailed, path));
            if (!ownerMetadata.empty() &&
                write(state.descriptor, ownerMetadata.data(), ownerMetadata.size()) != static_cast<ssize_t>(ownerMetadata.size()))
                return Result<void>::Failure(FsError(IoFailed, path));
            return FlushFileDescriptor(state.descriptor) ? Result<void>::Success() : Result<void>::Failure(FsError(IoFailed, path));
        }
#endif
    }  // namespace

    ExclusiveFileLock::ExclusiveFileLock() noexcept = default;

    ExclusiveFileLock::ExclusiveFileLock(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    ExclusiveFileLock::~ExclusiveFileLock() = default;
    ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock &&) noexcept = default;
    ExclusiveFileLock &ExclusiveFileLock::operator=(ExclusiveFileLock &&) noexcept = default;

    ExclusiveFileLock::operator bool() const noexcept {
        return state_ != nullptr;
    }

    /** @copydoc ExclusiveFileLock::ProtectsPath */
    bool ExclusiveFileLock::ProtectsPath(const std::filesystem::path &path) const {
        if (state_ == nullptr || !path.is_absolute())
            return false;
        if (std::error_code error; std::filesystem::weakly_canonical(path.parent_path(), error) != path.parent_path() || error)
            return false;
        return state_->processKey == LockKey(path);
    }

    /** @copydoc DurableFileSystem::TryAcquireExclusive */
    Result<ExclusiveFileLock> NativeDurableFileSystem::TryAcquireExclusive(const std::filesystem::path &path,
                                                                           const std::string_view ownerMetadata) {
        auto prepared = PrepareExclusiveLockDirectory(path, ownerMetadata);
        if (prepared.HasError())
            return Result<ExclusiveFileLock>::Failure(prepared.ErrorValue());
        const auto &lockPath = prepared.Value();
        auto state = std::make_unique<ExclusiveFileLock::State>();
        if (!ReserveProcessLock(*state, lockPath))
            return Result<ExclusiveFileLock>::Failure(FsError(LockBusy, lockPath));
        if (auto acquired = AcquireNativeExclusiveLock(*state, lockPath); acquired.HasError())
            return Result<ExclusiveFileLock>::Failure(acquired.ErrorValue());
        if (auto written = WriteExclusiveLockMetadata(*state, lockPath, ownerMetadata); written.HasError())
            return Result<ExclusiveFileLock>::Failure(written.ErrorValue());
        return Result<ExclusiveFileLock>::Success(ExclusiveFileLock(std::move(state)));
    }

}  // namespace Horo
