#pragma once

/** @file SaveFilesystemStoragePlatform.h
 * @brief Target-private no-follow native handle and publication helpers.
 */

#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveFilesystemStorageDetails.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Horo::Runtime::SaveFilesystemNative {
    using SaveFilesystemDetails::Failure;
    using SaveFilesystemDetails::SlotName;

#ifdef _WIN32
    class Handle final {
    public:
        explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}

        Handle(Handle &&other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}

        Handle &operator=(Handle &&other) noexcept {
            if (this != &other) {
                if (value_ != INVALID_HANDLE_VALUE)
                    ::CloseHandle(value_);
                value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
            }
            return *this;
        }

        ~Handle() {
            if (value_ != INVALID_HANDLE_VALUE)
                ::CloseHandle(value_);
        }

        Handle(const Handle &) = delete;
        Handle &operator=(const Handle &) = delete;

        [[nodiscard]] HANDLE Get() const noexcept {
            return value_;
        }

        [[nodiscard]] bool IsValid() const noexcept {
            return value_ != INVALID_HANDLE_VALUE;
        }

    private:
        HANDLE value_;
    };

    constexpr ULONG kOpenReparsePoint = 0x00200000;
    constexpr ULONG kDirectoryFile = 0x00000001;
    constexpr ULONG kNonDirectoryFile = 0x00000040;
    constexpr ULONG kSynchronousIo = 0x00000020;
    constexpr ULONG kOpen = 1;
    constexpr ULONG kCreate = 2;
    constexpr ULONG kOpenIf = 3;

    /** @brief Builds native attributes borrowing the caller's name and Unicode descriptor.
     * The name has already passed the bounded component check. Both borrowed objects must
     * remain alive until the synchronous native open returns; no pointers escape that call.
     */
    [[nodiscard]] inline OBJECT_ATTRIBUTES RelativeObject(const Handle &parent, const std::wstring &name, UNICODE_STRING &text) {
        text.Buffer = const_cast<PWSTR>(name.data());
        text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
        text.MaximumLength = text.Length;
        OBJECT_ATTRIBUTES object{};
        object.Length = sizeof(object);
        object.RootDirectory = parent.Get();
        object.ObjectName = &text;
        return object;
    }

    [[nodiscard]] inline Result<Handle> RelativeOpen(const Handle &parent, const std::wstring &name, const ACCESS_MASK access,
                                                     const ULONG disposition, const ULONG options, const ULONG attributes) {
        if (name.empty() || name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
            return Result<Handle>::Failure(Failure(SaveErrors::StorageOperationInvalid, "Windows component length"));
        const HMODULE library = ::GetModuleHandleW(L"ntdll.dll");
        const auto create = library ? reinterpret_cast<decltype(&NtCreateFile)>(::GetProcAddress(library, "NtCreateFile")) : nullptr;
        if (!create)
            return Result<Handle>::Failure(Failure(SaveErrors::StorageCapabilityUnsupported, "relative Windows creation"));
        UNICODE_STRING text{};
        auto object = RelativeObject(parent, name, text);
        IO_STATUS_BLOCK status{};
        HANDLE opened = INVALID_HANDLE_VALUE;
        const NTSTATUS result = create(&opened, access | SYNCHRONIZE, &object, &status, nullptr, attributes,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition,
                                       options | kOpenReparsePoint | kSynchronousIo, nullptr, 0);
        if (result < 0)
            return Result<Handle>::Failure(Failure(SaveErrors::StoragePermanentIo, "relative Windows open", result));
        Handle value{opened};
        FILE_ATTRIBUTE_TAG_INFO tag{};
        if (!::GetFileInformationByHandleEx(value.Get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
            (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return Result<Handle>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows reparse admission"));
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandle(value.Get(), &info) || info.nNumberOfLinks != 1)
            return Result<Handle>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows hard-link admission"));
        const DWORD length = ::GetFinalPathNameByHandleW(value.Get(), nullptr, 0, FILE_NAME_NORMALIZED);
        if (length == 0)
            return Result<Handle>::Failure(Failure(SaveErrors::StoragePermanentIo, "Windows name inspection", ::GetLastError()));
        std::wstring finalName(length + 1, L'\0');
        const DWORD written =
            ::GetFinalPathNameByHandleW(value.Get(), finalName.data(), static_cast<DWORD>(finalName.size()), FILE_NAME_NORMALIZED);
        if (written == 0 || written >= finalName.size())
            return Result<Handle>::Failure(Failure(SaveErrors::StoragePermanentIo, "Windows name inspection", ::GetLastError()));
        finalName.resize(written);
        if (finalName.substr(finalName.find_last_of(L"\\/") + 1) != name)
            return Result<Handle>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows case alias"));
        return Result<Handle>::Success(std::move(value));
    }

    [[nodiscard]] inline Result<void> SameEntry(const Handle &parent, const std::wstring &name, const Handle &held, const ULONG kind) {
        auto current = RelativeOpen(parent, name, FILE_READ_ATTRIBUTES, kOpen, kind, FILE_ATTRIBUTE_NORMAL);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        BY_HANDLE_FILE_INFORMATION left{}, right{};
        if (!::GetFileInformationByHandle(current.Value().Get(), &left) || !::GetFileInformationByHandle(held.Get(), &right) ||
            left.dwVolumeSerialNumber != right.dwVolumeSerialNumber || left.nFileIndexHigh != right.nFileIndexHigh ||
            left.nFileIndexLow != right.nFileIndexLow)
            return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows entry replacement"));
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<void> ExistingWindowsTargetSafe(const Handle &directory, const std::wstring &name) {
        if (name.empty() || name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
            return Result<void>::Failure(Failure(SaveErrors::StorageOperationInvalid, "Windows target length"));
        const HMODULE library = ::GetModuleHandleW(L"ntdll.dll");
        const auto create = library ? reinterpret_cast<decltype(&NtCreateFile)>(::GetProcAddress(library, "NtCreateFile")) : nullptr;
        if (!create)
            return Result<void>::Failure(Failure(SaveErrors::StorageCapabilityUnsupported, "Windows target inspection"));
        UNICODE_STRING text{};
        auto object = RelativeObject(directory, name, text);
        IO_STATUS_BLOCK status{};
        HANDLE opened = INVALID_HANDLE_VALUE;
        const NTSTATUS result = create(&opened, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &object, &status, nullptr, FILE_ATTRIBUTE_NORMAL,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, kOpen,
                                       kNonDirectoryFile | kOpenReparsePoint | kSynchronousIo, nullptr, 0);
        if (static_cast<std::uint32_t>(result) == 0xC0000034U)
            return Result<void>::Success();
        if (result < 0)
            return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows target admission", result));
        Handle target{opened};
        FILE_ATTRIBUTE_TAG_INFO tag{};
        BY_HANDLE_FILE_INFORMATION info{};
        if (!::GetFileInformationByHandleEx(target.Get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
            !::GetFileInformationByHandle(target.Get(), &info) || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            info.nNumberOfLinks != 1)
            return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "unsafe Windows target"));
        const DWORD length = ::GetFinalPathNameByHandleW(target.Get(), nullptr, 0, FILE_NAME_NORMALIZED);
        if (length == 0)
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "Windows target name inspection", ::GetLastError()));
        std::wstring finalName(length + 1, L'\0');
        const DWORD written =
            ::GetFinalPathNameByHandleW(target.Get(), finalName.data(), static_cast<DWORD>(finalName.size()), FILE_NAME_NORMALIZED);
        if (written == 0 || written >= finalName.size())
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "Windows target name inspection", ::GetLastError()));
        finalName.resize(written);
        if (finalName.substr(finalName.find_last_of(L"\\/") + 1) != name)
            return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows target case alias"));
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<void> RenameWindowsFile(HANDLE file, HANDLE directory, const std::wstring &destination) {
        if (destination.empty() || destination.size() > (std::numeric_limits<DWORD>::max() / sizeof(wchar_t)))
            return Result<void>::Failure(Failure(SaveErrors::StorageOperationInvalid, "Windows destination length"));
        const std::size_t byteLength = destination.size() * sizeof(wchar_t);
        // FILE_RENAME_INFO includes a one-character tail member; reserve the full
        // fixed structure plus the variable-length name for Win32's size check.
        if (byteLength > std::numeric_limits<DWORD>::max() - sizeof(FILE_RENAME_INFO))
            return Result<void>::Failure(Failure(SaveErrors::StorageOperationInvalid, "Windows destination buffer"));
        const std::size_t size = sizeof(FILE_RENAME_INFO) + byteLength;
        const std::size_t words = size / sizeof(std::uint64_t) + (size % sizeof(std::uint64_t) != 0);
        std::vector<std::uint64_t> buffer(words);
        // FILE_RENAME_INFO has the FILE_RENAME_INFORMATION field layout required by NtSetInformationFile.
        auto *rename = reinterpret_cast<FILE_RENAME_INFO *>(buffer.data());
        // Published readers retain the old immutable file while this rename selects the new one.
        constexpr DWORD kReplaceIfExists = 0x00000001;
        constexpr DWORD kPosixSemantics = 0x00000002;
        rename->Flags = kReplaceIfExists | kPosixSemantics;
        rename->RootDirectory = directory;
        rename->FileNameLength = static_cast<DWORD>(byteLength);
        std::copy(destination.begin(), destination.end(), rename->FileName);
        // Win32 FileRenameInfo rejects this held-directory root; the native information
        // class accepts a simple name relative to the RootDirectory capability.
        using NtSetInformationFileFunction = NTSTATUS(NTAPI *)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, ULONG);
        const HMODULE library = ::GetModuleHandleW(L"ntdll.dll");
        const auto set =
            library ? reinterpret_cast<NtSetInformationFileFunction>(::GetProcAddress(library, "NtSetInformationFile")) : nullptr;
        if (!set)
            return Result<void>::Failure(Failure(SaveErrors::StorageCapabilityUnsupported, "relative Windows replacement"));
        IO_STATUS_BLOCK status{};
        // WDK FileRenameInformationEx is 65 (Windows 10 1709+); user-mode winternl.h omits it.
        // learn.microsoft.com/windows-hardware/drivers/ddi/wdm/ne-wdm-_file_information_class
        // POSIX replacement flags preserve open readers; unsupported filesystems fail closed.
        constexpr ULONG kFileRenameInformationEx = 65;
        const NTSTATUS result = set(file, &status, rename, static_cast<ULONG>(size), kFileRenameInformationEx);
        if (result != 0)
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "Windows atomic replacement", result));
        return Result<void>::Success();
    }
#else
    class Directory final {
    public:
        explicit Directory(const int fd = -1) noexcept : fd_(fd) {}

        Directory(Directory &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

        Directory &operator=(Directory &&other) noexcept {
            if (this != &other) {
                if (fd_ >= 0)
                    ::close(fd_);
                fd_ = std::exchange(other.fd_, -1);
            }
            return *this;
        }

        ~Directory() {
            if (fd_ >= 0)
                ::close(fd_);
        }

        Directory(const Directory &) = delete;
        Directory &operator=(const Directory &) = delete;

        [[nodiscard]] int Fd() const noexcept {
            return fd_;
        }

    private:
        int fd_;
    };

    [[nodiscard]] inline Result<Directory> Child(const Directory &parent, const std::string &name) {
        if (::mkdirat(parent.Fd(), name.c_str(), 0700) != 0 && errno != EEXIST)
            return Result<Directory>::Failure(Failure(SaveErrors::StoragePermanentIo, "directory creation", errno));
        const int fd = ::openat(parent.Fd(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            return Result<Directory>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "directory admission", errno));
        return Result<Directory>::Success(Directory{fd});
    }

    [[nodiscard]] inline Result<void> ExistingTargetSafe(const Directory &directory, const std::string &name) {
        struct stat entry{};
        if (::fstatat(directory.Fd(), name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT)
                return Result<void>::Success();
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "target inspection", errno));
        }
        if (!S_ISREG(entry.st_mode) || entry.st_nlink != 1)
            return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "unsafe target"));
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<void> WritePosixBytes(const int fd, const std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t remaining = std::min(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            const ssize_t written = ::write(fd, bytes.data() + offset, remaining);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
                return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "temporary write", written < 0 ? errno : 0));
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(fd) != 0)
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "temporary synchronization", errno));
        return Result<void>::Success();
    }
#endif
}  // namespace Horo::Runtime::SaveFilesystemNative
