#include "ProjectReadNative.h"

#if !defined(_WIN32)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#include <utility>

namespace Horo::Platform::ProjectReadNative {
    namespace {
        /** @brief Maps no-follow rejection without exposing native names or errno text. */
        Error OpenError() {
            return MakeError(errno == ELOOP || errno == ENOTDIR ? ProjectReadErrors::UnsafePath : ProjectReadErrors::ReadFailed);
        }

        /** @brief Owns the independent directory iterator's consumed descriptor. */
        struct DirectoryCloser final {
            void operator()(DIR *value) const noexcept {
                if (value)
                    closedir(value);
            }
        };

        /** @brief Classifies no-follow stat evidence without selecting a backend or following a source link. */
        ProjectInventoryKind InventoryKind(const mode_t mode) noexcept {
            using enum ProjectInventoryKind;
            if (S_ISLNK(mode))
                return Link;
            if (S_ISDIR(mode))
                return Directory;
            if (S_ISREG(mode))
                return Regular;
            return Special;
        }
    }  // namespace

    /** @copydoc NormalizeHandle */
    HandleValue NormalizeHandle(const HandleValue value) noexcept {
        return value < 0 ? InvalidHandle : value;
    }

    /** @copydoc CloseHandleValue */
    void CloseHandleValue(const HandleValue value) noexcept {
        close(value);
    }

    /** @copydoc Info */
    Result<FileInfo> Info(const Handle &handle) {
        struct stat value{};
        if (fstat(handle.Get(), &value) != 0 || value.st_size < 0)
            return Result<FileInfo>::Failure(MakeError(ProjectReadErrors::ReadFailed));
#if defined(__APPLE__)
        const auto modified = value.st_mtimespec;
        const auto changed = value.st_ctimespec;
#else
        const auto modified = value.st_mtim;
        const auto changed = value.st_ctim;
#endif
        return Result<FileInfo>::Success({S_ISREG(value.st_mode),
                                          S_ISDIR(value.st_mode),
                                          static_cast<std::uint64_t>(value.st_size),
                                          static_cast<std::uint64_t>(value.st_nlink),
                                          {static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino),
                                           static_cast<std::uint64_t>(modified.tv_sec), static_cast<std::uint64_t>(modified.tv_nsec),
                                           static_cast<std::uint64_t>(changed.tv_sec), static_cast<std::uint64_t>(changed.tv_nsec)}});
    }

    /** @copydoc OpenChild */
    Result<Handle> OpenChild(const Handle &parent, const std::string_view name, const bool directory, const bool allowRegularHardlinks) {
        if (name.empty() || name.find_first_of("/\0", 0, 2) != std::string_view::npos || name == "..")
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::UnsafePath));
        const std::string segment{name};
        Handle child{openat(parent.Get(), segment.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (directory ? O_DIRECTORY : 0))};
        if (!child.IsValid())
            return Result<Handle>::Failure(OpenError());
        return AdmitChild(std::move(child), directory, allowRegularHardlinks);
    }

    /** @copydoc OpenAnchor */
    Result<Handle> OpenAnchor(const std::filesystem::path &) {
        Handle root{open("/", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (!root.IsValid())
            return Result<Handle>::Failure(OpenError());
        return Result<Handle>::Success(std::move(root));
    }

    /** @copydoc ComponentName */
    Result<std::string> ComponentName(const std::filesystem::path &part) {
        return Result<std::string>::Success(part.native());
    }

    /** @copydoc OpenDirectory */
    Result<Handle> OpenDirectory(const Handle &directory) {
        return OpenChild(directory, ".", true);
    }

    /** @copydoc Read */
    Result<std::size_t> Read(const Handle &handle, const ReadChunk bytes) {
        static_assert(ReadChunkBytes <= static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        // A single stack-owned transfer descriptor binds the destination to its fixed capacity.
        // readv advances the independent cursor just like read; no buffering, allocation or EINTR retry.
        const iovec destination{bytes.data(), bytes.size()};
        const auto count = readv(handle.Get(), &destination, 1);
        if (count < 0 || static_cast<std::size_t>(count) > bytes.size())
            return Result<std::size_t>::Failure(MakeError(ProjectReadErrors::ReadFailed));
        return Result<std::size_t>::Success(static_cast<std::size_t>(count));
    }

    /** @copydoc Visit */
    Result<void> Visit(const Handle &handle, const ProjectReadContext &context, const EntryVisitor &visitor) {
        auto independent = OpenDirectory(handle);
        if (independent.HasError())
            return Result<void>::Failure(independent.ErrorValue());
        auto cursor = std::move(independent).Value();
        std::unique_ptr<DIR, DirectoryCloser> directory{fdopendir(cursor.Get())};
        if (!directory)
            return Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
        static_cast<void>(cursor.Release());
        const int descriptor = dirfd(directory.get());
        if (descriptor < 0)
            return Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
        for (;;) {
            if (auto stop = CheckProjectReadContext(context); stop.HasError())
                return stop;
            errno = 0;
            const dirent *entry = readdir(directory.get());
            if (!entry)
                return errno == 0 ? Result<void>::Success() : Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
            const std::string_view name{entry->d_name};
            if (name == "." || name == "..")
                continue;
            struct stat info{};
            if (fstatat(descriptor, entry->d_name, &info, AT_SYMLINK_NOFOLLOW) != 0)
                return Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
            const auto kind = InventoryKind(info.st_mode);
            if (auto visited = visitor.Classified(name, kind); visited.HasError())
                return visited;
        }
    }
}  // namespace Horo::Platform::ProjectReadNative
#endif
