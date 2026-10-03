#include "Horo/Foundation/Platform.h"

#include "PlatformFileInternal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

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
#if defined(_WIN32)
        /** @brief Admits only one regular, non-aliased disk file for lock metadata. */
        [[nodiscard]] bool IsPrivateLockFile(const HANDLE handle) {
            BY_HANDLE_FILE_INFORMATION info{};
            return GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) &&
                   (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0U && info.nNumberOfLinks == 1U;
        }

        /** @brief Validates the opened lock handle before replacing and flushing diagnostic owner text. */
        [[nodiscard]] bool WriteLockOwner(const HANDLE handle, const std::string_view ownerMetadata) {
            if (!IsPrivateLockFile(handle))
                return false;
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(handle, zero, nullptr, FILE_BEGIN) || !SetEndOfFile(handle))
                return false;
            std::size_t offset = 0U;
            while (offset < ownerMetadata.size()) {
                const auto count = static_cast<DWORD>((std::min)(ownerMetadata.size() - offset, static_cast<std::size_t>(MAXDWORD)));
                DWORD written{};
                if (!WriteFile(handle, ownerMetadata.data() + offset, count, &written, nullptr) || written == 0U)
                    return false;
                offset += written;
            }
            if (!FlushFileBuffers(handle))
                return false;
            return true;
        }

        /** @brief Appends only through a private Windows regular-file handle at the exact offset. */
        [[nodiscard]] bool AppendPrivateBytes(const std::filesystem::path &path, const std::uint64_t expectedOffset,
                                              const std::span<const std::byte> bytes) {
            const DWORD disposition = expectedOffset == 0U ? CREATE_NEW : OPEN_EXISTING;
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, disposition,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return false;
            BY_HANDLE_FILE_INFORMATION info{};
            LARGE_INTEGER size{};
            bool ok = GetFileType(handle) == FILE_TYPE_DISK && GetFileInformationByHandle(handle, &info) &&
                      (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) == 0U &&
                      info.nNumberOfLinks == 1U && GetFileSizeEx(handle, &size) && size.QuadPart >= 0 &&
                      static_cast<std::uint64_t>(size.QuadPart) == expectedOffset;
            LARGE_INTEGER zero{};
            if (ok)
                ok = SetFilePointerEx(handle, zero, nullptr, FILE_END) != 0;
            std::size_t offset = 0U;
            while (ok && offset < bytes.size()) {
                const auto count = static_cast<DWORD>((std::min)(bytes.size() - offset, static_cast<std::size_t>(MAXDWORD)));
                DWORD written{};
                ok = WriteFile(handle, bytes.data() + offset, count, &written, nullptr) && written == count;
                offset += written;
            }
            if (ok)
                ok = FlushFileBuffers(handle) != 0;
            if (!CloseHandle(handle))
                ok = false;
            return ok;
        }
#else
        [[nodiscard]] bool FlushFileDescriptor(const int descriptor) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
            if (fcntl(descriptor, F_FULLFSYNC) == 0)
                return true;
#endif
            return fsync(descriptor) == 0;
        }

        /** @brief Admits only one regular, non-aliased file for lock metadata. */
        [[nodiscard]] bool IsPrivateLockFile(const int descriptor) {
            struct stat info{};
            return fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1U;
        }

        /** @brief Appends only through a private POSIX regular-file descriptor at the exact offset. */
        [[nodiscard]] bool AppendPrivateBytes(const std::filesystem::path &path, const std::uint64_t expectedOffset,
                                              const std::span<const std::byte> bytes) {
            if (expectedOffset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
                return false;
            const int flags = O_WRONLY | O_NOFOLLOW | O_CLOEXEC | (expectedOffset == 0U ? O_CREAT | O_EXCL : 0);
            const int descriptor = open(path.c_str(), flags, 0600);
            if (descriptor < 0)
                return false;
            struct stat status{};
            bool ok = fstat(descriptor, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 1U && status.st_size >= 0 &&
                      static_cast<std::uint64_t>(status.st_size) == expectedOffset;
            if (ok)
                ok = lseek(descriptor, static_cast<off_t>(expectedOffset), SEEK_SET) == static_cast<off_t>(expectedOffset);
            std::size_t offset = 0U;
            while (ok && offset < bytes.size()) {
                const auto count = (std::min)(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
                const ssize_t written = write(descriptor, bytes.data() + offset, count);
                if (written <= 0) {
                    ok = false;
                    break;
                }
                offset += static_cast<std::size_t>(written);
            }
            if (ok)
                ok = FlushFileDescriptor(descriptor);
            if (close(descriptor) != 0)
                ok = false;
            return ok;
        }
#endif
    }  // namespace

    struct ProductLaunchLease::State {
        State() = default;
        State(const State &) = delete;
        State &operator=(const State &) = delete;
        State(State &&) = delete;
        State &operator=(State &&) = delete;
#if defined(_WIN32)
        HANDLE handle{INVALID_HANDLE_VALUE};
#else
        int descriptor{-1};
#endif
        bool maintenance{};

        ~State() {
#if defined(_WIN32)
            if (handle != INVALID_HANDLE_VALUE)
                CloseHandle(handle);
#else
            // A forked or duplicated descriptor shares this flock; only the last close may release it.
            if (descriptor >= 0)
                close(descriptor);
#endif
        }
    };

    ProductLaunchLease::ProductLaunchLease() noexcept = default;

    ProductLaunchLease::ProductLaunchLease(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    ProductLaunchLease::~ProductLaunchLease() = default;
    ProductLaunchLease::ProductLaunchLease(ProductLaunchLease &&) noexcept = default;
    ProductLaunchLease &ProductLaunchLease::operator=(ProductLaunchLease &&) noexcept = default;

    ProductLaunchLease::operator bool() const noexcept {
        return state_ != nullptr;
    }

    bool ProductLaunchLease::IsMaintenance() const noexcept {
        return state_ != nullptr && state_->maintenance;
    }

    std::uintptr_t ProductLaunchLease::NativeHandle() const noexcept {
#if defined(_WIN32)
        return state_ == nullptr ? 0U : reinterpret_cast<std::uintptr_t>(state_->handle);
#else
        return state_ == nullptr ? 0U : static_cast<std::uintptr_t>(state_->descriptor);
#endif
    }

    ProductLaunchLease ProductLaunchLease::AdoptMaintenanceNative(const std::uintptr_t native) {
        auto state = std::make_unique<State>();
#if defined(_WIN32)
        state->handle = reinterpret_cast<HANDLE>(native);
#else
        state->descriptor = static_cast<int>(native);
#endif
        state->maintenance = true;
        return ProductLaunchLease(std::move(state));
    }

    /** @copydoc NativeDurableFileSystem::TryAcquireProductLaunch */
    Result<ProductLaunchLease> NativeDurableFileSystem::TryAcquireProductLaunch(const std::filesystem::path &installationRoot) const {
        return TryAcquireProductLease(installationRoot, false);
    }

    /** @copydoc NativeDurableFileSystem::TryAcquireProductMaintenance */
    Result<ProductLaunchLease> NativeDurableFileSystem::TryAcquireProductMaintenance(const std::filesystem::path &installationRoot) const {
        return TryAcquireProductLease(installationRoot, true);
    }

    /** @brief Acquires one OS-held launch or maintenance lease without changing an existing installation root. */
    Result<ProductLaunchLease> NativeDurableFileSystem::TryAcquireProductLease(const std::filesystem::path &installationRoot,
                                                                               const bool maintenance) const {
        const auto path = installationRoot / ".product-launch.lock";
        if (!installationRoot.is_absolute() || std::ranges::any_of(installationRoot, [](const auto &part) {
            return part == "." || part == "..";
        }))
            return Result<ProductLaunchLease>::Failure(FsError(IoFailed, path));
        if (std::error_code error; !std::filesystem::is_directory(std::filesystem::symlink_status(installationRoot, error)) || error)
            return Result<ProductLaunchLease>::Failure(FsError(IoFailed, path));
        auto state = std::make_unique<ProductLaunchLease::State>();
        state->maintenance = maintenance;
#if defined(_WIN32)
        const DWORD sharing = maintenance ? 0U : FILE_SHARE_READ | FILE_SHARE_WRITE;
        state->handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, sharing, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (state->handle == INVALID_HANDLE_VALUE)
            return Result<ProductLaunchLease>::Failure(FsError(GetLastError() == ERROR_SHARING_VIOLATION ? LockBusy : IoFailed, path));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(state->handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U ||
            (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U || info.nNumberOfLinks != 1U)
            return Result<ProductLaunchLease>::Failure(FsError(IoFailed, path));
#else
        state->descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (state->descriptor < 0)
            return Result<ProductLaunchLease>::Failure(FsError(IoFailed, path));
        if (struct stat info{}; fstat(state->descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
            return Result<ProductLaunchLease>::Failure(FsError(IoFailed, path));
        if (flock(state->descriptor, (maintenance ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
            return Result<ProductLaunchLease>::Failure(FsError(errno == EWOULDBLOCK ? LockBusy : IoFailed, path));
#endif
        return Result<ProductLaunchLease>::Success(ProductLaunchLease(std::move(state)));
    }

    /** @copydoc DurableFileSystem::AvailableBytes */
    Result<std::uint64_t> NativeDurableFileSystem::AvailableBytes(const std::filesystem::path &path) const {
        std::error_code error;
        const auto info = std::filesystem::space(path, error);
        if (error)
            return Result<std::uint64_t>::Failure(FsError(IoFailed, path));
        return Result<std::uint64_t>::Success(info.available);
    }

    /** @copydoc DurableFileSystem::WriteDurable */
    Result<void> NativeDurableFileSystem::WriteDurable(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error)
            return Result<void>::Failure(FsError(IoFailed, path));
#if defined(_WIN32)
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return Result<void>::Failure(FsError(IoFailed, path));
        DWORD written{};
        const bool ok = bytes.size() <= MAXDWORD && WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                        written == bytes.size() && FlushFileBuffers(handle);
        CloseHandle(handle);
        if (!ok)
            return Result<void>::Failure(FsError(IoFailed, path));
#else
        const int descriptor = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (descriptor < 0)
            return Result<void>::Failure(FsError(IoFailed, path));
        std::size_t offset = 0;
        bool ok = true;
        while (offset < bytes.size()) {
            const ssize_t count = write(descriptor, bytes.data() + offset, bytes.size() - offset);
            if (count <= 0) {
                ok = false;
                break;
            }
            offset += static_cast<std::size_t>(count);
        }
        ok = ok && FlushFileDescriptor(descriptor);
        close(descriptor);
        if (!ok)
            return Result<void>::Failure(FsError(IoFailed, path));
#endif
        return SyncDirectory(path.parent_path());
    }

    /** @copydoc NativeDurableFileSystem::AppendPrivateDurable */
    Result<void> NativeDurableFileSystem::AppendPrivateDurable(const std::filesystem::path &path, const std::uint64_t expectedOffset,
                                                               const std::span<const std::byte> bytes) {
        if (path.empty() || path.parent_path().empty() || bytes.empty() ||
            bytes.size() > std::numeric_limits<std::uint64_t>::max() - expectedOffset)
            return Result<void>::Failure(FsError(IoFailed, path));
        return AppendPrivateBytes(path, expectedOffset, bytes) ? SyncDirectory(path.parent_path())
                                                               : Result<void>::Failure(FsError(IoFailed, path));
    }

    /** @copydoc DurableFileSystem::CopyDurable */
    Result<void> NativeDurableFileSystem::CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) {
        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error || !std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing, error))
            return Result<void>::Failure(FsError(IoFailed, error ? destination : source));
#if defined(_WIN32)
        HANDLE handle =
            CreateFileW(destination.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        const bool flushed = handle != INVALID_HANDLE_VALUE && FlushFileBuffers(handle);
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
#else
        const int descriptor = open(destination.c_str(), O_RDONLY);
        const bool flushed = descriptor >= 0 && FlushFileDescriptor(descriptor);
        if (descriptor >= 0)
            close(descriptor);
#endif
        if (!flushed)
            return Result<void>::Failure(FsError(IoFailed, destination));
        return SyncDirectory(destination.parent_path());
    }

    /** @copydoc DurableFileSystem::AtomicReplace */
    Result<void> NativeDurableFileSystem::AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) {
        AtomicFileReplacementReceipt receipt;
        return AtomicReplaceTracked(prepared, destination, receipt);
    }

    /** @copydoc NativeDurableFileSystem::AtomicReplaceTracked */
    Result<void> NativeDurableFileSystem::AtomicReplaceTracked(const std::filesystem::path &prepared,
                                                               const std::filesystem::path &destination,
                                                               AtomicFileReplacementReceipt &receipt) {
        if (receipt.WasCommitted())
            return Result<void>::Failure(FsError(IoFailed, destination));
        std::error_code error;
        std::filesystem::create_directories(destination.parent_path(), error);
        if (error)
            return Result<void>::Failure(FsError(IoFailed, destination));
#if defined(_WIN32)
        if (const BOOL ok = MoveFileExW(prepared.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH); !ok)
            return Result<void>::Failure(FsError(IoFailed, destination));
#else
        if (::rename(prepared.c_str(), destination.c_str()) != 0)
            return Result<void>::Failure(FsError(IoFailed, destination));
#endif
        receipt.RecordCommitted();
        return SyncDirectory(destination.parent_path());
    }

    /** @copydoc DurableFileSystem::RemoveDurable */
    Result<void> NativeDurableFileSystem::RemoveDurable(const std::filesystem::path &path) {
        if (std::error_code error; !std::filesystem::remove(path, error) && error)
            return Result<void>::Failure(FsError(IoFailed, path));
        return SyncDirectory(path.parent_path());
    }

    /** @copydoc DurableFileSystem::SyncDirectory */
    Result<void> NativeDurableFileSystem::SyncDirectory(const std::filesystem::path &path) {
#if defined(_WIN32)
        static_cast<void>(path);
        return Result<void>::Success();
#else
        const int descriptor = open(path.c_str(), O_RDONLY | O_DIRECTORY);
        if (descriptor < 0)
            return Result<void>::Failure(FsError(IoFailed, path));
        const bool ok = fsync(descriptor) == 0;
        close(descriptor);
        return ok ? Result<void>::Success() : Result<void>::Failure(FsError(IoFailed, path));
#endif
    }

    /** @copydoc PlatformServices::PlatformServices */
    PlatformServices::PlatformServices(FileSystem &files, Clock &clock, ProcessService &processes,  // NOSONAR(cpp:S107)
                                       UserDirectories &directories, const PlatformCapabilities capabilities, CredentialStore *credentials,
                                       NativeDialogs *dialogs, CrashService *crash) noexcept
        : files(files), clock(clock), processes(processes), directories(directories), capabilities(capabilities), credentials(credentials),

          dialogs(dialogs), crash(crash) {
        this->capabilities.hasCredentialStore = credentials != nullptr;
        this->capabilities.hasNativeDialogs = dialogs != nullptr;
        this->capabilities.hasCrashService = crash != nullptr;
    }

    /** @copydoc PlatformServices::Capabilities */
    const PlatformCapabilities &PlatformServices::Capabilities() const noexcept {
        return capabilities;
    }

    /** @copydoc FileSystem::Exists */
    bool NullFileSystem::Exists(const std::filesystem::path &path) const {
        static_cast<void>(path);
        return false;
    }

    /** @copydoc DeterministicClock::DeterministicClock */
    DeterministicClock::DeterministicClock(const Duration initial) noexcept : m_now(initial) {}

    /** @copydoc Clock::MonotonicNow */
    Duration DeterministicClock::MonotonicNow() const {
        return m_now;
    }

    /** @copydoc DeterministicClock::Advance */
    void DeterministicClock::Advance(const Duration elapsed) noexcept {
        m_now = m_now + elapsed;
    }

    /** @copydoc SteadyClock::MonotonicNow */
    Duration SteadyClock::MonotonicNow() const {
        return Duration::FromNanoseconds(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    /** @copydoc WallClock::UtcNow */
    std::chrono::system_clock::time_point SystemWallClock::UtcNow() const {
        return std::chrono::system_clock::now();
    }

    /** @copydoc ProcessService::CurrentProcess */
    ProcessMetadata NullProcessService::CurrentProcess() const {
        return {};
    }

    /** @copydoc ProcessService::EnvironmentValue */
    std::optional<std::string> NullProcessService::EnvironmentValue(const std::string_view name) const {
        static_cast<void>(name);
        return std::nullopt;
    }

    /** @copydoc StaticUserDirectories::StaticUserDirectories */
    StaticUserDirectories::StaticUserDirectories(UserDirectoryPaths paths) : m_paths(std::move(paths)) {}

    /** @copydoc UserDirectories::Config */
    const std::filesystem::path &StaticUserDirectories::Config() const {
        return m_paths.config;
    }

    /** @copydoc UserDirectories::State */
    const std::filesystem::path &StaticUserDirectories::State() const {
        return m_paths.state;
    }

    /** @copydoc UserDirectories::Cache */
    const std::filesystem::path &StaticUserDirectories::Cache() const {
        return m_paths.cache;
    }

    /** @copydoc UserDirectories::Logs */
    const std::filesystem::path &StaticUserDirectories::Logs() const {
        return m_paths.logs;
    }

    /** @copydoc UserDirectories::Crash */
    const std::filesystem::path &StaticUserDirectories::Crash() const {
        return m_paths.crash;
    }

    /** @copydoc UserDirectories::Temporary */
    const std::filesystem::path &StaticUserDirectories::Temporary() const {
        return m_paths.temporary;
    }
}  // namespace Horo
