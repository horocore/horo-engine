#include "../PlatformFileInternal.h"
#include "PublicationFileNative.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <limits>
#include <utility>
#include <windows.h>
#include <winternl.h>

namespace Horo::Platform::PublicationFileNative {
    namespace {
        using OpenRelative = NTSTATUS(NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG,
                                               ULONG, ULONG, PVOID, ULONG);
        enum class Access {
            Inspect,
            Mutate,
            Lock,
            Create
        };

        /** @brief Mirrors the native 32-bit query discriminator without requiring driver-only SDK declarations. */
        enum class VolumeInformationClass : ULONG {
            FullSize = 7
        };

        /** @brief Captures classification and identity from the already opened object. */
        Result<BY_HANDLE_FILE_INFORMATION> ObjectInfo(const Handle &handle) {
            BY_HANDLE_FILE_INFORMATION info{};
            if (!GetFileInformationByHandle(handle.Get(), &info))
                return Result<BY_HANDLE_FILE_INFORMATION>::Failure(MakeError(FileStorageErrors::IoFailed));
            return Result<BY_HANDLE_FILE_INFORMATION>::Success(info);
        }

        /** @brief Distinguishes redirects before ordinary regular/directory admission. */
        FileEntryInfo Entry(const BY_HANDLE_FILE_INFORMATION &info) {
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U)
                return {.kind = FileEntryKind::Redirect};
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0U)
                return {.kind = FileEntryKind::Directory};
            if ((info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) != 0U)
                return {.kind = FileEntryKind::Other};
            return {.kind = FileEntryKind::RegularFile,
                    .sizeBytes = (std::uint64_t{info.nFileSizeHigh} << 32U) | info.nFileSizeLow,
                    .hardLinks = info.nNumberOfLinks};
        }

        /** @brief Constructs the exact native access mask without granting create or write rights to inspection. */
        ACCESS_MASK Mask(const Access access) noexcept {
            ACCESS_MASK mask = FILE_READ_ATTRIBUTES | SYNCHRONIZE;
            if (access != Access::Inspect)
                mask |= DELETE;
            if (access == Access::Lock || access == Access::Create)
                mask |= FILE_READ_DATA | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES;
            return mask;
        }

        /** @brief Opens one component against a retained parent; only private creation sets the caller's creation receipt. */
        Result<std::optional<Handle>> Open(const Handle &parent, const std::string_view name, const Access access, bool *created = nullptr,
                                           const bool directory = false) {
            const auto entry = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateFile");
            const auto create = std::bit_cast<OpenRelative>(entry);
            if (!create)
                return Result<std::optional<Handle>>::Failure(MakeError(FileStorageErrors::IoFailed));
            auto spelling = std::filesystem::path{std::u8string{name.begin(), name.end()}}.native();
            if (spelling.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
                return Result<std::optional<Handle>>::Failure(MakeError(FileStorageErrors::Capacity));
            const auto length = static_cast<USHORT>(spelling.size() * sizeof(wchar_t));
            UNICODE_STRING nativeName{length, length, spelling.data()};
            OBJECT_ATTRIBUTES attributes{};
            attributes.Length = sizeof(attributes);
            attributes.RootDirectory = parent.Get();
            attributes.ObjectName = &nativeName;
            attributes.Attributes = 0x00000040U;
            IO_STATUS_BLOCK io{};
            HANDLE raw{};
            const ULONG disposition = access == Access::Create ? 2U : access == Access::Lock ? 3U : 1U;
            const ULONG options = 0x00200000U | 0x00000020U | 0x00004000U | (directory ? 0x00000001U : 0U);
            const auto result = create(&raw, Mask(access), &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition, options, nullptr, 0);
            if (result < 0) {
                const auto status = static_cast<ULONG>(result);
                if (status == 0xc0000034U || status == 0xc000003aU || status == 0xc000000fU)
                    return Result<std::optional<Handle>>::Success(std::nullopt);
                if (access == Access::Lock && status == 0xc0000043U)
                    return Result<std::optional<Handle>>::Failure(MakeError(PlatformFileInternal::LockBusy));
                return Result<std::optional<Handle>>::Failure(
                    MakeError(status == 0xc0000035U ? FileStorageErrors::Conflict : FileStorageErrors::IoFailed));
            }
            Handle handle{raw};
            if (created)
                *created = true;
            return Result<std::optional<Handle>>::Success(std::move(handle));
        }

        /** @brief Rejects redirects, special objects and extra regular links before a destructive operation. */
        bool Admitted(const FileEntryInfo &entry) noexcept {
            return entry.kind == FileEntryKind::Directory || (entry.kind == FileEntryKind::RegularFile && entry.hardLinks == 1U);
        }

        /** @brief Owns one byte-range lock; metadata-only inspection remains possible while the lease is retained. */
        class Lease final : public ExclusiveFileLock::State {
        public:
            Lease(Handle parent, Handle file, std::string name, std::filesystem::path path)
                : parent_(std::move(parent)), file_(std::move(file)), name_(std::move(name)), path_(std::move(path)) {}

            /** @copydoc ExclusiveFileLock::State::ProtectsPath */
            bool ProtectsPath(const std::filesystem::path &path) const override {
                if (path != path_)
                    return false;
                auto named = Open(parent_, name_, Access::Inspect);
                if (named.HasError() || !named.Value())
                    return false;
                auto held = ObjectInfo(file_);
                auto current = ObjectInfo(*named.Value());
                if (held.HasError() || current.HasError())
                    return false;
                const auto a = held.Value(), b = current.Value();
                return Admitted(Entry(b)) && Entry(b).kind == FileEntryKind::RegularFile &&
                       a.dwVolumeSerialNumber == b.dwVolumeSerialNumber && a.nFileIndexHigh == b.nFileIndexHigh &&
                       a.nFileIndexLow == b.nFileIndexLow;
            }

        private:
            Handle parent_;
            Handle file_;
            std::string name_;
            std::filesystem::path path_;
        };

        /** @brief Serializes a bounded relative rename without forming a flexible-array pointer. */
        Result<void> Rename(const Handle &file, const Handle &parent, const std::string_view name, AtomicFileReplacementReceipt &receipt) {
            const auto spelling = std::filesystem::path{std::u8string{name.begin(), name.end()}}.native();
            const auto nameBytes = spelling.size() * sizeof(wchar_t);
            const auto totalBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
            alignas(FILE_RENAME_INFO) std::array<std::byte, 16384> buffer{};
            if (totalBytes > buffer.size())
                return Result<void>::Failure(MakeError(FileStorageErrors::Capacity));
            FILE_RENAME_INFO header{};
            std::ranges::fill(std::as_writable_bytes(std::span{&header, 1}), std::byte{});
            header.ReplaceIfExists = TRUE;
            header.RootDirectory = parent.Get();
            header.FileNameLength = static_cast<DWORD>(nameBytes);
            std::ranges::copy(std::as_bytes(std::span{&header, 1}).first(offsetof(FILE_RENAME_INFO, FileName)), buffer.begin());
            std::ranges::copy(std::as_bytes(std::span{spelling}), buffer.begin() + offsetof(FILE_RENAME_INFO, FileName));
            if (!SetFileInformationByHandle(file.Get(), FileRenameInfo, buffer.data(), static_cast<DWORD>(totalBytes)))
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
            receipt.RecordCommitted();
            return Result<void>::Success();
        }

        /** @brief Parses one kernel directory batch into a complete bounded inventory. */
        Result<void> Batch(const Handle &directory, const std::span<const std::byte> bytes, const std::size_t maximumEntries,
                           const CancellationToken &cancellation, std::vector<FileDirectoryEntry> &entries) {
            std::size_t offset{};
            for (;;) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(FileStorageErrors::Cancelled));
                constexpr auto headerBytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                if (offset > bytes.size() - headerBytes)
                    return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
                FILE_ID_BOTH_DIR_INFO entry{};
                std::ranges::copy(bytes.subspan(offset, headerBytes), std::as_writable_bytes(std::span{&entry, 1}).begin());
                if (entry.FileNameLength % sizeof(wchar_t) != 0 || entry.FileNameLength > bytes.size() - offset - headerBytes)
                    return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
                std::wstring name(entry.FileNameLength / sizeof(wchar_t), L'\0');
                std::ranges::copy(bytes.subspan(offset + headerBytes, entry.FileNameLength),
                                  std::as_writable_bytes(std::span{name}).begin());
                if (name != L"." && name != L"..") {
                    if (entries.size() == maximumEntries)
                        return Result<void>::Failure(MakeError(FileStorageErrors::Capacity));
                    const std::filesystem::path path{name};
                    auto portable = ProjectReadNative::ComponentName(path);
                    if (portable.HasError())
                        return Result<void>::Failure(WrapError(FileStorageErrors::InvalidPath, portable.ErrorValue()));
                    auto info = Inspect(directory, portable.Value());
                    if (info.HasError() || !info.Value())
                        return Result<void>::Failure(info.HasError() ? info.ErrorValue() : MakeError(FileStorageErrors::IoFailed));
                    entries.push_back({path, *info.Value()});
                }
                if (entry.NextEntryOffset == 0)
                    return Result<void>::Success();
                if (entry.NextEntryOffset < headerBytes || entry.NextEntryOffset > bytes.size() - offset ||
                    entry.NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) != 0)
                    return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
                offset += entry.NextEntryOffset;
            }
        }
    }  // namespace

    /** @copydoc Inspect */
    Result<std::optional<FileEntryInfo>> Inspect(const Handle &parent, const std::string_view name) {
        auto handle = Open(parent, name, Access::Inspect);
        if (handle.HasError())
            return Result<std::optional<FileEntryInfo>>::Failure(handle.ErrorValue());
        if (!handle.Value())
            return Result<std::optional<FileEntryInfo>>::Success(std::nullopt);
        auto info = ObjectInfo(*handle.Value());
        return info.HasError() ? Result<std::optional<FileEntryInfo>>::Failure(info.ErrorValue())
                               : Result<std::optional<FileEntryInfo>>::Success(Entry(info.Value()));
    }

    /** @copydoc List */
    Result<std::vector<FileDirectoryEntry>> List(const Handle &directory, const std::size_t maximumEntries,
                                                 const CancellationToken &cancellation) {
        using Entries = std::vector<FileDirectoryEntry>;
        auto cursor = ProjectReadNative::OpenDirectory(directory);
        if (cursor.HasError())
            return Result<Entries>::Failure(WrapError(FileStorageErrors::IoFailed, cursor.ErrorValue()));
        alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 16384> buffer{};
        bool restart = true;
        Entries entries;
        for (;;) {
            if (cancellation.IsCancellationRequested())
                return Result<Entries>::Failure(MakeError(FileStorageErrors::Cancelled));
            const auto kind = restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
            restart = false;
            if (!GetFileInformationByHandleEx(cursor.Value().Get(), kind, buffer.data(), static_cast<DWORD>(buffer.size())))
                return GetLastError() == ERROR_NO_MORE_FILES ? Result<Entries>::Success(std::move(entries))
                                                             : Result<Entries>::Failure(MakeError(FileStorageErrors::IoFailed));
            if (auto batch = Batch(directory, buffer, maximumEntries, cancellation, entries); batch.HasError())
                return Result<Entries>::Failure(batch.ErrorValue());
        }
    }

    /** @copydoc Create */
    Result<Handle> Create(const Handle &parent, const std::string_view name, bool &created, const std::filesystem::perms permissions) {
        auto opened = Open(parent, name, Access::Create, &created);
        if (opened.HasError())
            return Result<Handle>::Failure(opened.ErrorValue());
        if (!opened.Value())
            return Result<Handle>::Failure(MakeError(FileStorageErrors::IoFailed));
        auto info = ObjectInfo(*opened.Value());
        if (info.HasError() || Entry(info.Value()).kind != FileEntryKind::RegularFile || info.Value().nNumberOfLinks != 1U)
            return Result<Handle>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        if (permissions != std::filesystem::perms::unknown &&
            (permissions & std::filesystem::perms::owner_write) == std::filesystem::perms::none) {
            FILE_BASIC_INFO basic{};
            basic.FileAttributes = FILE_ATTRIBUTE_READONLY;
            if (!SetFileInformationByHandle(opened.Value()->Get(), FileBasicInfo, &basic, sizeof(basic)))
                return Result<Handle>::Failure(MakeError(FileStorageErrors::IoFailed));
        }
        return Result<Handle>::Success(*std::move(opened).Value());
    }

    /** @copydoc CreateRelativeDirectory */
    Result<bool> CreateRelativeDirectory(const Handle &parent, const std::string_view name) {
        auto created = Open(parent, name, Access::Create, nullptr, true);
        if (created.HasError() && created.ErrorValue().code.Value() == FileStorageErrors::Conflict.code.Value()) {
            auto existing = ImmutableFileNative::Directory(parent, name, false);
            if (existing.HasError())
                return Result<bool>::Failure(existing.ErrorValue());
            return existing.Value() ? Result<bool>::Success(false) : Result<bool>::Failure(MakeError(FileStorageErrors::IoFailed));
        }
        if (created.HasError())
            return Result<bool>::Failure(created.ErrorValue());
        return created.Value() ? Result<bool>::Success(true) : Result<bool>::Failure(MakeError(FileStorageErrors::IoFailed));
    }

    /** @copydoc Available */
    Result<std::uint64_t> Available(const Handle &directory) {
        struct FullSizeInfo {
            LARGE_INTEGER total, available, actual;
            ULONG sectors, bytes;
        } info{};

        using QueryVolume = NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, VolumeInformationClass);
        const auto query = std::bit_cast<QueryVolume>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryVolumeInformationFile"));
        IO_STATUS_BLOCK io{};
        if (!query || query(directory.Get(), &io, &info, sizeof(info), VolumeInformationClass::FullSize) < 0 || info.available.QuadPart < 0)
            return Result<std::uint64_t>::Failure(MakeError(FileStorageErrors::IoFailed));
        const auto unit = std::uint64_t{info.sectors} * info.bytes;
        const auto count = static_cast<std::uint64_t>(info.available.QuadPart);
        if (unit != 0 && count > UINT64_MAX / unit)
            return Result<std::uint64_t>::Failure(MakeError(FileStorageErrors::Capacity));
        return Result<std::uint64_t>::Success(count * unit);
    }

    /** @copydoc FlushFile */
    Result<void> FlushFile(const Handle &file) {
        return ImmutableFileNative::Sync(file);
    }

    /** @copydoc Lock */
    Result<std::unique_ptr<ExclusiveFileLock::State>> Lock(Handle parent, std::string name, std::filesystem::path logicalPath,
                                                           const std::string_view ownerMetadata) {
        using State = std::unique_ptr<ExclusiveFileLock::State>;
        auto file = Open(parent, name, Access::Lock);
        if (file.HasError())
            return Result<State>::Failure(file.ErrorValue());
        if (!file.Value())
            return Result<State>::Failure(MakeError(FileStorageErrors::IoFailed));
        auto info = ObjectInfo(*file.Value());
        if (info.HasError() || Entry(info.Value()).kind != FileEntryKind::RegularFile || info.Value().nNumberOfLinks != 1U)
            return Result<State>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        OVERLAPPED overlap{};
        if (!LockFileEx(file.Value()->Get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap))
            return Result<State>::Failure(
                MakeError(GetLastError() == ERROR_LOCK_VIOLATION ? PlatformFileInternal::LockBusy : FileStorageErrors::IoFailed));
        auto owned = std::make_unique<Lease>(std::move(parent), *std::move(file).Value(), std::move(name), logicalPath);
        if (!owned->ProtectsPath(logicalPath))
            return Result<State>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        static_cast<void>(ownerMetadata);
        return Result<State>::Success(std::move(owned));
    }

    /** @copydoc Replace */
    Result<void> Replace(const Handle &sourceParent, const std::string_view source, const Handle &destinationParent,
                         const std::string_view destination, AtomicFileReplacementReceipt &receipt) {
        auto file = Open(sourceParent, source, Access::Mutate);
        if (file.HasError())
            return Result<void>::Failure(file.ErrorValue());
        if (!file.Value())
            return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
        auto from = ObjectInfo(*file.Value());
        auto to = Inspect(destinationParent, destination);
        if (from.HasError() || to.HasError())
            return Result<void>::Failure(from.HasError() ? from.ErrorValue() : to.ErrorValue());
        const auto entry = Entry(from.Value());
        if (!Admitted(entry) || (to.Value() && (!Admitted(*to.Value()) || to.Value()->kind != entry.kind)))
            return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        if (auto renamed = Rename(*file.Value(), destinationParent, destination, receipt); renamed.HasError())
            return renamed;
        if (auto synced = ImmutableFileNative::Sync(sourceParent); synced.HasError())
            return synced;
        return ImmutableFileNative::Sync(destinationParent);
    }

    /** @copydoc Remove */
    Result<void> Remove(const Handle &parent, const std::string_view name) {
        auto file = Open(parent, name, Access::Mutate);
        if (file.HasError())
            return Result<void>::Failure(file.ErrorValue());
        if (!file.Value())
            return Result<void>::Success();
        auto info = ObjectInfo(*file.Value());
        if (info.HasError() || !Admitted(Entry(info.Value())))
            return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(file.Value()->Get(), FileDispositionInfo, &disposition, sizeof(disposition)))
            return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
        file = Result<std::optional<Handle>>::Success(std::nullopt);
        return ImmutableFileNative::Sync(parent);
    }
}  // namespace Horo::Platform::PublicationFileNative
#endif
