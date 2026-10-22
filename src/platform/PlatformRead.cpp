#include "Horo/Foundation/ImmutableFileStorage.h"
#include "Horo/Foundation/Platform.h"

#include <algorithm>
#include <array>
#include <bit>
#include <new>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace Horo {
    namespace {
        using Bytes = std::vector<std::uint8_t>;

        /** @brief Rejects ambiguous input before native access; the host retains parent-namespace authority. */
        bool AdmittedPath(const std::filesystem::path &path) {
            if (!path.is_absolute() || path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
                return false;
            if (path.has_relative_path() && path.filename().empty())
                return false;
            if (std::ranges::any_of(path, [](const auto &part) {
                return part == "." || part == "..";
            }))
                return false;
#if defined(_WIN32)
            const auto &text = path.native();
            if (text.starts_with(L"\\\\.\\") || text.starts_with(L"\\\\?\\"))
                return false;
#endif
            return true;
        }

        /** @brief Captures native object identity and mutation fields without observing last-access updates from our read. */
        struct ReadSnapshot {
            std::uint64_t length{};
            std::uint64_t links{};
            bool regular{};
            std::array<std::uint64_t, 8> identity{};
            bool operator==(const ReadSnapshot &) const = default;
        };

        /** @brief Owns one no-follow, nonblocking regular-file cursor through read verification and every failure. */
        class ReadHandle final {
        public:
            explicit ReadHandle(const std::filesystem::path &path)
#if defined(_WIN32)
                : value_(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr))
#else
                : value_(open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC))
#endif
            {
            }

            ~ReadHandle() {
#if defined(_WIN32)
                if (value_ != INVALID_HANDLE_VALUE)
                    CloseHandle(value_);
#else
                if (value_ >= 0)
                    close(value_);
#endif
            }

            ReadHandle(const ReadHandle &) = delete;
            ReadHandle &operator=(const ReadHandle &) = delete;

            /** @brief Returns regular-file metadata from the retained cursor, rejecting redirects and special objects. */
            Result<ReadSnapshot> Snapshot(const bool requireSingleLink = true) const {
#if defined(_WIN32)
                BY_HANDLE_FILE_INFORMATION info{};
                if (value_ == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(value_, &info))
                    return Result<ReadSnapshot>::Failure(MakeError(FileStorageErrors::IoFailed));
                const ReadSnapshot result{.length = (std::uint64_t{info.nFileSizeHigh} << 32U) | info.nFileSizeLow,
                                          .links = info.nNumberOfLinks,
                                          .regular =
                                              GetFileType(value_) == FILE_TYPE_DISK &&
                                              (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0,
                                          .identity = {info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
                                                       (std::uint64_t{info.ftLastWriteTime.dwHighDateTime} << 32U) |
                                                           info.ftLastWriteTime.dwLowDateTime,
                                                       (std::uint64_t{info.ftCreationTime.dwHighDateTime} << 32U) |
                                                           info.ftCreationTime.dwLowDateTime,
                                                       info.dwFileAttributes, 0, 0}};
#else
                struct stat info{};
                if (value_ < 0 || fstat(value_, &info) != 0 || info.st_size < 0)
                    return Result<ReadSnapshot>::Failure(MakeError(FileStorageErrors::IoFailed));
#if defined(__APPLE__)
                const auto modified = info.st_mtimespec;
                const auto changed = info.st_ctimespec;
#else
                const auto modified = info.st_mtim;
                const auto changed = info.st_ctim;
#endif
                const ReadSnapshot result{.length = static_cast<std::uint64_t>(info.st_size),
                                          .links = static_cast<std::uint64_t>(info.st_nlink),
                                          .regular = S_ISREG(info.st_mode),
                                          .identity = {static_cast<std::uint64_t>(info.st_dev), static_cast<std::uint64_t>(info.st_ino),
                                                       static_cast<std::uint64_t>(modified.tv_sec),
                                                       static_cast<std::uint64_t>(modified.tv_nsec),
                                                       static_cast<std::uint64_t>(changed.tv_sec),
                                                       static_cast<std::uint64_t>(changed.tv_nsec),
                                                       static_cast<std::uint64_t>(info.st_mode), 0}};
#endif
                if (!result.regular || (requireSingleLink && result.links != 1U))
                    return Result<ReadSnapshot>::Failure(MakeError(FileStorageErrors::UnsafeObject));
                return Result<ReadSnapshot>::Success(result);
            }

            /** @brief Retains full native ownership identity; unavailable IDs cannot authorize cleanup. */
            std::optional<FileEntryIdentity> EntryIdentity(const ReadSnapshot &snapshot) const noexcept {
#if defined(_WIN32)
                FILE_ID_INFO info{};
                if (!GetFileInformationByHandleEx(value_, FileIdInfo, &info, sizeof(info)))
                    return std::nullopt;
                // ReFS needs all 128 file-ID bits and the full volume serial; the read snapshot's legacy ID is insufficient.
                auto identity = snapshot.identity;
                identity[0] = info.VolumeSerialNumber;
                const auto fileId = std::bit_cast<std::array<std::uint64_t, 2>>(info.FileId);
                identity[1] = fileId[0];
                identity[2] = fileId[1];
                return FileEntryIdentity{identity};
#else
                return FileEntryIdentity{snapshot.identity};
#endif
            }

            /** @brief Performs one bounded transfer without retrying an interrupted OS call. */
            Result<std::size_t> Read(std::span<std::uint8_t> buffer) const {
#if defined(_WIN32)
                DWORD count{};
                if (!ReadFile(value_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr))
                    return Result<std::size_t>::Failure(MakeError(FileStorageErrors::IoFailed));
#else
                // One stack-owned transfer descriptor binds the destination to its capacity.
                const iovec destination{buffer.data(), buffer.size()};
                const auto count = readv(value_, &destination, 1);
                if (count < 0 || static_cast<std::size_t>(count) > buffer.size())
                    return Result<std::size_t>::Failure(MakeError(FileStorageErrors::IoFailed));
#endif
                return Result<std::size_t>::Success(static_cast<std::size_t>(count));
            }

        private:
#if defined(_WIN32)
            HANDLE value_{INVALID_HANDLE_VALUE};
#else
            int value_{-1};
#endif
        };

        /** @brief Returns cancellation without native access or successful partial data. */
        Result<void> CheckReadStop(const CancellationToken &cancel) {
            return cancel.IsCancellationRequested() ? Result<void>::Failure(MakeError(FileStorageErrors::Cancelled))
                                                    : Result<void>::Success();
        }

        /** @brief Preserves no-follow ancestor admission inside the native authority, under its protected-parent precondition. */
        Result<void> CheckNativeParents(const std::filesystem::path &path, const CancellationToken &cancellation = {}) {
            auto cursor = path.root_path();
            for (const auto &component : path.parent_path().relative_path()) {
                if (auto stop = CheckReadStop(cancellation); stop.HasError())
                    return stop;
                cursor /= component;
#if defined(_WIN32)
                if (const auto attributes = GetFileAttributesW(cursor.c_str());
                    attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
                    return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
#endif
                std::error_code error;
                const auto status = std::filesystem::symlink_status(cursor, error);
                if (status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
                    return Result<void>::Success();
                if (error)
                    return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
                if (!std::filesystem::is_directory(status) || std::filesystem::is_symlink(status))
                    return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
            }
            return Result<void>::Success();
        }

        /** @brief Completes a finite transfer through one borrowed cursor; no buffer or token escapes on failure. */
        Result<Bytes> TransferBytes(const ReadHandle &handle, const std::size_t length, const CancellationToken &cancellation) {
            Bytes bytes(length);
            std::size_t offset{};
            while (offset < bytes.size()) {
                if (auto stop = CheckReadStop(cancellation); stop.HasError())
                    return Result<Bytes>::Failure(stop.ErrorValue());
                auto count = handle.Read(std::span{bytes}.subspan(offset, std::min(std::size_t{4096}, bytes.size() - offset)));
                if (count.HasError())
                    return Result<Bytes>::Failure(count.ErrorValue());
                if (count.Value() == 0)
                    return Result<Bytes>::Failure(MakeError(FileStorageErrors::IoFailed));
                offset += count.Value();
            }
            return Result<Bytes>::Success(std::move(bytes));
        }

        /** @brief Collects only immediate children after native directory admission; exhaustion never publishes a prefix. */
        Result<std::vector<FileDirectoryEntry>> CollectEntries(const NativeDurableFileSystem &files, const std::filesystem::path &path,
                                                               const std::size_t maximumEntries, const CancellationToken &cancellation) {
            using Entries = std::vector<FileDirectoryEntry>;
            std::error_code error;
            std::filesystem::directory_iterator cursor(path, error);
            const std::filesystem::directory_iterator end;
            if (error)
                return Result<Entries>::Failure(MakeError(FileStorageErrors::IoFailed));
            Entries entries;
            while (cursor != end) {
                if (auto stop = CheckReadStop(cancellation); stop.HasError())
                    return Result<Entries>::Failure(stop.ErrorValue());
                if (entries.size() == maximumEntries)
                    return Result<Entries>::Failure(MakeError(FileStorageErrors::Capacity));
                const auto info = files.InspectEntry(cursor->path());
                if (info.HasError())
                    return Result<Entries>::Failure(info.ErrorValue());
                if (!info.Value())
                    return Result<Entries>::Failure(MakeError(FileStorageErrors::IoFailed));
                entries.emplace_back(cursor->path().filename(), *info.Value());
                cursor.increment(error);
                if (error)
                    return Result<Entries>::Failure(MakeError(FileStorageErrors::IoFailed));
            }
            if (auto stop = CheckReadStop(cancellation); stop.HasError())
                return Result<Entries>::Failure(stop.ErrorValue());
            return Result<Entries>::Success(std::move(entries));
        }
    }  // namespace

    /** @copydoc NativeDurableFileSystem::ReadFileBounded */
    Result<std::vector<std::uint8_t>> NativeDurableFileSystem::ReadFileBounded(const std::filesystem::path &path,
                                                                               const std::size_t maximumBytes,
                                                                               const CancellationToken &cancellation) const {
        if (!AdmittedPath(path))
            return Result<Bytes>::Failure(MakeError(FileStorageErrors::InvalidPath));
        if (maximumBytes == 0 || maximumBytes > ImmutableFileStorage::MaximumEntryBytes)
            return Result<Bytes>::Failure(MakeError(FileStorageErrors::Capacity));
        if (auto stop = CheckReadStop(cancellation); stop.HasError())
            return Result<Bytes>::Failure(stop.ErrorValue());
        if (auto parents = CheckNativeParents(path, cancellation); parents.HasError())
            return Result<Bytes>::Failure(parents.ErrorValue());
        const ReadHandle handle{path};
        const auto before = handle.Snapshot();
        if (before.HasError())
            return Result<Bytes>::Failure(before.ErrorValue());
        if (before.Value().length > maximumBytes)
            return Result<Bytes>::Failure(MakeError(FileStorageErrors::Capacity));
        try {
            auto bytes = TransferBytes(handle, static_cast<std::size_t>(before.Value().length), cancellation);
            if (bytes.HasError())
                return bytes;
            const auto after = handle.Snapshot();
            if (after.HasError())
                return Result<Bytes>::Failure(after.ErrorValue());
            if (before.Value() != after.Value())
                return Result<Bytes>::Failure(MakeError(FileStorageErrors::UnsafeObject));
            if (auto stop = CheckReadStop(cancellation); stop.HasError())
                return Result<Bytes>::Failure(stop.ErrorValue());
            return bytes;
        } catch (const std::bad_alloc &) {
            return Result<Bytes>::Failure(MakeError(FileStorageErrors::Capacity));
        }
    }

    /** @copydoc NativeDurableFileSystem::InspectEntry */
    Result<std::optional<FileEntryInfo>> NativeDurableFileSystem::InspectEntry(const std::filesystem::path &path) const {
        if (!AdmittedPath(path))
            return Result<std::optional<FileEntryInfo>>::Failure(MakeError(FileStorageErrors::InvalidPath));
        if (auto parents = CheckNativeParents(path); parents.HasError())
            return Result<std::optional<FileEntryInfo>>::Failure(parents.ErrorValue());
#if defined(_WIN32)
        if (const auto attributes = GetFileAttributesW(path.c_str());
            attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
            return Result<std::optional<FileEntryInfo>>::Success(FileEntryInfo{.kind = FileEntryKind::Redirect});
#endif
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (status.type() == std::filesystem::file_type::not_found && (!error || error == std::errc::no_such_file_or_directory))
            return Result<std::optional<FileEntryInfo>>::Success(std::nullopt);
        if (error)
            return Result<std::optional<FileEntryInfo>>::Failure(MakeError(FileStorageErrors::IoFailed));
        FileEntryInfo info;
        if (std::filesystem::is_symlink(status))
            info.kind = FileEntryKind::Redirect;
        else if (std::filesystem::is_directory(status))
            info.kind = FileEntryKind::Directory;
        else if (std::filesystem::is_regular_file(status)) {
            info.kind = FileEntryKind::RegularFile;
            const ReadHandle handle{path};
            const auto snapshot = handle.Snapshot(false);
            if (snapshot.HasValue()) {
                info.sizeBytes = snapshot.Value().length;
                info.hardLinks = snapshot.Value().links;
                info.identity = handle.EntryIdentity(snapshot.Value());
            } else {
                // Metadata inspection does not require content-read authority. Missing identity remains explicitly unavailable.
                info.sizeBytes = std::filesystem::file_size(path, error);
                if (error)
                    return Result<std::optional<FileEntryInfo>>::Failure(MakeError(FileStorageErrors::IoFailed));
                info.hardLinks = std::filesystem::hard_link_count(path, error);
                if (error)
                    return Result<std::optional<FileEntryInfo>>::Failure(MakeError(FileStorageErrors::IoFailed));
            }
        }
        return Result<std::optional<FileEntryInfo>>::Success(info);
    }

    /** @copydoc NativeDurableFileSystem::ListDirectory */
    Result<std::vector<FileDirectoryEntry>> NativeDurableFileSystem::ListDirectory(const std::filesystem::path &path,
                                                                                   const std::size_t maximumEntries,
                                                                                   const CancellationToken &cancellation) const {
        using Entries = std::vector<FileDirectoryEntry>;
        if (maximumEntries == 0 || maximumEntries > 1048576U)
            return Result<Entries>::Failure(MakeError(FileStorageErrors::Capacity));
        if (auto stop = CheckReadStop(cancellation); stop.HasError())
            return Result<Entries>::Failure(stop.ErrorValue());
        const auto root = InspectEntry(path);
        if (root.HasError())
            return Result<Entries>::Failure(root.ErrorValue());
        if (!root.Value() || root.Value()->kind != FileEntryKind::Directory)
            return Result<Entries>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        try {
            return CollectEntries(*this, path, maximumEntries, cancellation);
        } catch (const std::bad_alloc &) {
            return Result<Entries>::Failure(MakeError(FileStorageErrors::Capacity));
        }
    }
}  // namespace Horo
