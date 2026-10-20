#include "ImmutableFileNative.h"

#if !defined(_WIN32)
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#if defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace Horo::Platform::ImmutableFileNative {
    namespace {
        /** @brief Rejects redirects/type obstructions distinctly without disclosing native paths. */
        Error NativeError() {
            return MakeError(errno == ELOOP || errno == ENOTDIR ? FileStorageErrors::UnsafeObject : FileStorageErrors::IoFailed);
        }

        /** @brief Checks the opened object rather than accepting pathname metadata as access authority. */
        Result<void> Admit(const Handle &handle, const bool directory) {
            struct stat info{};
            if (fstat(handle.Get(), &info) != 0)
                return Result<void>::Failure(NativeError());
            if (directory ? !S_ISDIR(info.st_mode) : (!S_ISREG(info.st_mode) || info.st_nlink != 1))
                return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
            return Result<void>::Success();
        }

        /** @brief Opens one no-follow identity without blocking on a FIFO or device masquerading as cache content. */
        Result<std::optional<Handle>> Open(const Handle &parent, const std::string &name, const bool directory,
                                           const bool flushable = false) {
            Handle opened{openat(parent.Get(), name.c_str(),
                                 (flushable ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | (directory ? O_DIRECTORY : 0))};
            if (!opened.IsValid())
                return errno == ENOENT ? Result<std::optional<Handle>>::Success(std::nullopt)
                                       : Result<std::optional<Handle>>::Failure(NativeError());
            if (auto admitted = Admit(opened, directory); admitted.HasError())
                return Result<std::optional<Handle>>::Failure(admitted.ErrorValue());
            return Result<std::optional<Handle>>::Success(std::move(opened));
        }
    }  // namespace

    /** @copydoc Directory */
    Result<std::optional<Handle>> Directory(const Handle &parent, const std::string_view name, const bool create) {
        const std::string spelling{name};
        auto existing = Open(parent, spelling, true);
        if (existing.HasValue() && existing.Value() && create) {
            if (auto synced = Sync(parent); synced.HasError())
                return Result<std::optional<Handle>>::Failure(synced.ErrorValue());
        }
        if (existing.HasError() || existing.Value() || !create)
            return existing;
        if (mkdirat(parent.Get(), spelling.c_str(), S_IRWXU) != 0 && errno != EEXIST)
            return Result<std::optional<Handle>>::Failure(NativeError());
        auto opened = Open(parent, spelling, true);
        if (opened.HasValue() && opened.Value()) {
            if (auto synced = Sync(parent); synced.HasError())
                return Result<std::optional<Handle>>::Failure(synced.ErrorValue());
        }
        return opened;
    }

    /** @copydoc Regular */
    Result<std::optional<Handle>> Regular(const Handle &parent, const std::string_view name, const bool flushable) {
        return Open(parent, std::string{name}, false, flushable);
    }

    /** @copydoc Prepare */
    Result<std::optional<Preparation>> Prepare(const Handle &parent, std::string name) {
        if (mkdirat(parent.Get(), name.c_str(), S_IRWXU) != 0)
            return errno == EEXIST ? Result<std::optional<Preparation>>::Success(std::nullopt)
                                   : Result<std::optional<Preparation>>::Failure(NativeError());
        Handle directory{openat(parent.Get(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (struct stat info{}; !directory.IsValid() || fstat(directory.Get(), &info) != 0 || !S_ISDIR(info.st_mode) ||
                                info.st_uid != geteuid() || (info.st_mode & (S_IRWXG | S_IRWXO)) != 0)
            return Result<std::optional<Preparation>>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        Handle file{openat(directory.Get(), "payload", O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR)};
        if (!file.IsValid()) {
            const auto error = NativeError();
            Preparation failed{std::move(directory), Handle{}, std::move(name)};
            Discard(parent, failed);
            return Result<std::optional<Preparation>>::Failure(error);
        }
        return Result<std::optional<Preparation>>::Success(Preparation{std::move(directory), std::move(file), std::move(name)});
    }

    /** @copydoc Write */
    Result<void> Write(const Handle &file, const std::span<const std::uint8_t> bytes, const CancellationToken &cancellation) {
        std::size_t written{};
        while (written < bytes.size()) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(FileStorageErrors::Cancelled));
            const auto count = write(file.Get(), bytes.data() + written, std::min(std::size_t{65536}, bytes.size() - written));
            if (count <= 0)
                return Result<void>::Failure(NativeError());
            written += static_cast<std::size_t>(count);
        }
        if (auto admitted = Admit(file, false); admitted.HasError())
            return admitted;
        return fsync(file.Get()) == 0 ? Result<void>::Success() : Result<void>::Failure(NativeError());
    }

    /** @copydoc Publish */
    Result<bool> Publish(const Handle &parent, Preparation &preparation, const std::string_view destination) {
#if defined(__APPLE__) || defined(__linux__)
        const std::string name{destination};
#if defined(__APPLE__)
        if (const int result = renameatx_np(preparation.directory.Get(), "payload", parent.Get(), name.c_str(), RENAME_EXCL); result == 0) {
#else
        if (const auto result =
                syscall(SYS_renameat2, preparation.directory.Get(), "payload", parent.Get(), name.c_str(), RENAME_NOREPLACE);
            result == 0) {
#endif
            preparation.published = true;
            return Result<bool>::Success(true);
        }
        return errno == EEXIST || errno == ENOTEMPTY ? Result<bool>::Success(false) : Result<bool>::Failure(NativeError());
#else
        static_cast<void>(parent);
        static_cast<void>(preparation);
        static_cast<void>(destination);
        return Result<bool>::Failure(MakeError(FileStorageErrors::IoFailed));
#endif
    }

    /** @copydoc Sync */
    Result<void> Sync(const Handle &directory) {
        return fsync(directory.Get()) == 0 ? Result<void>::Success() : Result<void>::Failure(NativeError());
    }

    /** @copydoc Discard */
    void Discard(const Handle &parent, Preparation &preparation) noexcept {
        if (!preparation.directory.IsValid())
            return;
        // The payload name belongs exclusively to this private opened directory, even if its ancestor was renamed.
        static_cast<void>(unlinkat(preparation.directory.Get(), "payload", 0));
        struct stat owned{};
        if (fstat(preparation.directory.Get(), &owned) != 0)
            return;
        if (struct stat named{}; fstatat(parent.Get(), preparation.name.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                                 owned.st_dev == named.st_dev && owned.st_ino == named.st_ino)
            static_cast<void>(unlinkat(parent.Get(), preparation.name.c_str(), AT_REMOVEDIR));
        preparation.file = Handle{};
        preparation.directory = Handle{};
    }
}  // namespace Horo::Platform::ImmutableFileNative
#endif
