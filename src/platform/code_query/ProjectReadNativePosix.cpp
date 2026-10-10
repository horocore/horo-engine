#include "ProjectReadNative.h"

#if !defined(_WIN32)
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
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
    Result<Handle> OpenChild(const Handle &parent, const std::string_view name, const bool directory) {
        if (name.empty() || name.find_first_of("/\0", 0, 2) != std::string_view::npos || name == "..")
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::UnsafePath));
        const std::string segment{name};
        Handle child{openat(parent.Get(), segment.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (directory ? O_DIRECTORY : 0))};
        if (!child.IsValid())
            return Result<Handle>::Failure(OpenError());
        return AdmitChild(std::move(child), directory);
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
    Result<std::size_t> Read(const Handle &handle, const std::span<char> bytes) {
        // One bounded syscall: EINTR fails explicitly rather than retrying beyond the caller's deadline.
        const auto count = read(handle.Get(), bytes.data(), bytes.size());
        if (count < 0)
            return Result<std::size_t>::Failure(MakeError(ProjectReadErrors::ReadFailed));
        return Result<std::size_t>::Success(static_cast<std::size_t>(count));
    }

    /** @copydoc Visit */
    Result<void> Visit(const Handle &handle, const ProjectReadContext &context,
                       const std::function<Result<void>(std::string_view)> &visitor) {
        auto independent = OpenDirectory(handle);
        if (independent.HasError())
            return Result<void>::Failure(independent.ErrorValue());
        auto cursor = std::move(independent).Value();
        std::unique_ptr<DIR, DirectoryCloser> directory{fdopendir(cursor.Get())};
        if (!directory)
            return Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
        static_cast<void>(cursor.Release());
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
            if (auto visited = visitor(name); visited.HasError())
                return visited;
        }
    }
}  // namespace Horo::Platform::ProjectReadNative
#endif
