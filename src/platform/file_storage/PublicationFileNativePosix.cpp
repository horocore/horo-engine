#include "../PlatformFileInternal.h"
#include "PublicationFileNative.h"

#if !defined(_WIN32)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <memory>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

namespace Horo::Platform::PublicationFileNative {
    namespace {
        /** @brief Preserves admission versus infrastructure failure without exposing native paths. */
        Error Failure() {
            return MakeError(errno == ELOOP || errno == ENOTDIR ? FileStorageErrors::UnsafeObject : FileStorageErrors::IoFailed);
        }

        /** @brief Classifies one no-follow native stat result without using it as later open authority. */
        FileEntryInfo Entry(const struct stat &info) {
            using enum FileEntryKind;
            if (S_ISREG(info.st_mode))
                return {.kind = RegularFile,
                        .sizeBytes = static_cast<std::uint64_t>(info.st_size),
                        .hardLinks = static_cast<std::uint64_t>(info.st_nlink)};
            if (S_ISDIR(info.st_mode))
                return {.kind = Directory};
            return {.kind = S_ISLNK(info.st_mode) ? Redirect : Other};
        }

        /** @brief Verifies regular single-link identity before writes or replacement. */
        bool Regular(const struct stat &info) noexcept {
            return S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_size >= 0;
        }

        /** @brief Owns kernel exclusion and the retained parent used to verify the exact live lock-file identity. */
        class Lease final : public ExclusiveFileLock::State {
        public:
            Lease(Handle parent, Handle file, std::string name, std::filesystem::path path)
                : parent_(std::move(parent)), file_(std::move(file)), name_(std::move(name)), path_(std::move(path)) {}

            /** @copydoc ExclusiveFileLock::State::ProtectsPath */
            bool ProtectsPath(const std::filesystem::path &path) const override {
                if (path != path_)
                    return false;
                struct stat owned{};
                struct stat named{};
                return fstat(file_.Get(), &owned) == 0 && fstatat(parent_.Get(), name_.c_str(), &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                       Regular(owned) && Regular(named) && owned.st_dev == named.st_dev && owned.st_ino == named.st_ino;
            }

        private:
            Handle parent_;
            Handle file_;
            std::string name_;
            std::filesystem::path path_;
        };
    }  // namespace

    /** @copydoc Inspect */
    Result<std::optional<FileEntryInfo>> Inspect(const Handle &parent, const std::string_view name) {
        struct stat info{};
        if (fstatat(parent.Get(), std::string{name}.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0)
            return errno == ENOENT ? Result<std::optional<FileEntryInfo>>::Success(std::nullopt)
                                   : Result<std::optional<FileEntryInfo>>::Failure(Failure());
        if (info.st_size < 0)
            return Result<std::optional<FileEntryInfo>>::Failure(MakeError(FileStorageErrors::IoFailed));
        return Result<std::optional<FileEntryInfo>>::Success(Entry(info));
    }

    /** @copydoc List */
    Result<std::vector<FileDirectoryEntry>> List(const Handle &directory, const std::size_t maximumEntries,
                                                 const CancellationToken &cancellation) {
        using Entries = std::vector<FileDirectoryEntry>;
        auto independent = ProjectReadNative::OpenDirectory(directory);
        if (independent.HasError())
            return Result<Entries>::Failure(WrapError(FileStorageErrors::IoFailed, independent.ErrorValue()));
        const int descriptor = independent.Value().Get();
        auto *raw = fdopendir(descriptor);
        if (!raw)
            return Result<Entries>::Failure(Failure());
        static_cast<void>(std::move(independent).Value().Release());
        const std::unique_ptr<DIR, decltype(&closedir)> cursor{raw, &closedir};
        Entries entries;
        for (;;) {
            if (cancellation.IsCancellationRequested())
                return Result<Entries>::Failure(MakeError(FileStorageErrors::Cancelled));
            errno = 0;
            const auto *entry = readdir(cursor.get());
            if (!entry)
                return errno == 0 ? Result<Entries>::Success(std::move(entries)) : Result<Entries>::Failure(Failure());
            const std::string_view name{entry->d_name};
            if (name == "." || name == "..")
                continue;
            if (entries.size() == maximumEntries)
                return Result<Entries>::Failure(MakeError(FileStorageErrors::Capacity));
            auto info = Inspect(directory, name);
            if (info.HasError())
                return Result<Entries>::Failure(info.ErrorValue());
            if (!info.Value())
                return Result<Entries>::Failure(MakeError(FileStorageErrors::IoFailed));
            entries.emplace_back(std::filesystem::path{name}, *info.Value());
        }
    }

    /** @copydoc Create */
    Result<Handle> Create(const Handle &parent, const std::string_view name, bool &created, const std::filesystem::perms permissions) {
        const auto mode = permissions == std::filesystem::perms::unknown ? S_IRUSR | S_IWUSR : static_cast<mode_t>(permissions) & 0777U;
        Handle file{
            openat(parent.Get(), std::string{name}.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, mode)};
        if (!file.IsValid())
            return Result<Handle>::Failure(errno == EEXIST ? MakeError(FileStorageErrors::Conflict) : Failure());
        created = true;
        if (struct stat info{}; fstat(file.Get(), &info) != 0 || !Regular(info))
            return Result<Handle>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        if (permissions != std::filesystem::perms::unknown && fchmod(file.Get(), mode) != 0)
            return Result<Handle>::Failure(Failure());
        return Result<Handle>::Success(std::move(file));
    }

    /** @copydoc CreateRelativeDirectory */
    Result<bool> CreateRelativeDirectory(const Handle &parent, const std::string_view name) {
        if (const std::string spelling{name}; mkdirat(parent.Get(), spelling.c_str(), S_IRWXU) == 0)
            return Result<bool>::Success(true);
        else if (errno != EEXIST)
            return Result<bool>::Failure(Failure());
        auto existing = ImmutableFileNative::Directory(parent, name, false);
        if (existing.HasError())
            return Result<bool>::Failure(existing.ErrorValue());
        return existing.Value() ? Result<bool>::Success(false) : Result<bool>::Failure(MakeError(FileStorageErrors::IoFailed));
    }

    /** @copydoc Available */
    Result<std::uint64_t> Available(const Handle &directory) {
        struct statvfs info{};
        if (fstatvfs(directory.Get(), &info) != 0)
            return Result<std::uint64_t>::Failure(Failure());
        const auto unit = static_cast<std::uint64_t>(info.f_frsize);
        const auto blocks = static_cast<std::uint64_t>(info.f_bavail);
        if (unit != 0 && blocks > UINT64_MAX / unit)
            return Result<std::uint64_t>::Failure(MakeError(FileStorageErrors::Capacity));
        return Result<std::uint64_t>::Success(blocks * unit);
    }

    /** @copydoc FlushFile */
    Result<void> FlushFile(const Handle &file) {
        return PlatformFileInternal::FlushFileDescriptor(file.Get()) ? Result<void>::Success() : Result<void>::Failure(Failure());
    }

    /** @copydoc Lock */
    Result<std::unique_ptr<ExclusiveFileLock::State>> Lock(Handle parent, std::string name, std::filesystem::path logicalPath,
                                                           const std::string_view ownerMetadata) {
        using State = std::unique_ptr<ExclusiveFileLock::State>;
        Handle file{openat(parent.Get(), name.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, S_IRUSR | S_IWUSR)};
        if (!file.IsValid())
            return Result<State>::Failure(Failure());
        if (struct stat info{}; fstat(file.Get(), &info) != 0 || !Regular(info))
            return Result<State>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        if (flock(file.Get(), LOCK_EX | LOCK_NB) != 0)
            return Result<State>::Failure(errno == EWOULDBLOCK || errno == EAGAIN ? MakeError(PlatformFileInternal::LockBusy) : Failure());
        auto owned = std::make_unique<Lease>(std::move(parent), std::move(file), std::move(name), logicalPath);
        if (!owned->ProtectsPath(logicalPath))
            return Result<State>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        // Metadata is diagnostic only; the retained native lock is the authority.
        static_cast<void>(ownerMetadata);
        return Result<State>::Success(std::move(owned));
    }

    /** @copydoc Replace */
    Result<void> Replace(const Handle &sourceParent, const std::string_view source, const Handle &destinationParent,
                         const std::string_view destination, AtomicFileReplacementReceipt &receipt) {
        auto from = Inspect(sourceParent, source);
        auto to = Inspect(destinationParent, destination);
        if (from.HasError() || to.HasError())
            return Result<void>::Failure(from.HasError() ? from.ErrorValue() : to.ErrorValue());
        if (!from.Value() ||
            (from.Value()->kind != FileEntryKind::Directory &&
             (from.Value()->kind != FileEntryKind::RegularFile || from.Value()->hardLinks != 1U)) ||
            (to.Value() &&
             (to.Value()->kind != from.Value()->kind || (to.Value()->kind == FileEntryKind::RegularFile && to.Value()->hardLinks != 1U))))
            return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        const std::string fromName{source};
        if (const std::string toName{destination};
            renameat(sourceParent.Get(), fromName.c_str(), destinationParent.Get(), toName.c_str()) != 0)
            return Result<void>::Failure(Failure());
        receipt.RecordCommitted();
        if (auto synced = ImmutableFileNative::Sync(sourceParent); synced.HasError())
            return synced;
        return ImmutableFileNative::Sync(destinationParent);
    }

    /** @copydoc Remove */
    Result<void> Remove(const Handle &parent, const std::string_view name) {
        auto info = Inspect(parent, name);
        if (info.HasError())
            return Result<void>::Failure(info.ErrorValue());
        if (!info.Value())
            return Result<void>::Success();
        const bool directory = info.Value()->kind == FileEntryKind::Directory;
        if (!directory && (info.Value()->kind != FileEntryKind::RegularFile || info.Value()->hardLinks != 1U))
            return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        if (unlinkat(parent.Get(), std::string{name}.c_str(), directory ? AT_REMOVEDIR : 0) != 0)
            return Result<void>::Failure(Failure());
        return ImmutableFileNative::Sync(parent);
    }
}  // namespace Horo::Platform::PublicationFileNative
#endif
