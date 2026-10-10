#include "ProjectReadNative.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <cstddef>
#include <iterator>
#include <limits>
#include <utility>
#include <windows.h>
#include <winternl.h>

namespace Horo::Platform::ProjectReadNative {
    namespace {
        using CreateRelativeFile = NTSTATUS(NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                                     ULONG, ULONG, ULONG, PVOID, ULONG);
        constexpr ULONG OpenExisting = 1;
        constexpr ULONG DirectoryOnly = 0x00000001;
        constexpr ULONG SynchronousIo = 0x00000020;
        constexpr ULONG BackupIntent = 0x00004000;
        constexpr ULONG OpenReparsePoint = 0x00200000;
        constexpr ULONG CaseInsensitive = 0x00000040;

        /** @brief Encodes one native name strictly; malformed UTF-16 cannot become a lossy relative identity. */
        Result<std::string> Utf8(const std::wstring_view name) {
            if (name.size() > 4096)
                return Result<std::string>::Failure(MakeError(ProjectReadErrors::Capacity));
            const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0,
                                                 nullptr, nullptr);
            if (size <= 0)
                return Result<std::string>::Failure(MakeError(ProjectReadErrors::UnsafePath));
            std::string result(static_cast<std::size_t>(size), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), result.data(), size, nullptr,
                                    nullptr) != size)
                return Result<std::string>::Failure(MakeError(ProjectReadErrors::UnsafePath));
            return Result<std::string>::Success(std::move(result));
        }

        /** @brief Converts a validated portable segment without replacement characters or Win32 path normalization. */
        Result<std::wstring> Wide(const std::string_view name) {
            if (name.size() > 4096)
                return Result<std::wstring>::Failure(MakeError(ProjectReadErrors::Capacity));
            const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
            if (size <= 0)
                return Result<std::wstring>::Failure(MakeError(ProjectReadErrors::UnsafePath));
            std::wstring result(static_cast<std::size_t>(size), L'\0');
            if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), result.data(), size) != size)
                return Result<std::wstring>::Failure(MakeError(ProjectReadErrors::UnsafePath));
            return Result<std::wstring>::Success(std::move(result));
        }

        /** @brief Captures an opened handle's object type and mutation identity without following a path. */
        Result<FileInfo> CaptureInfo(const HANDLE handle) {
            BY_HANDLE_FILE_INFORMATION file{};
            FILE_BASIC_INFO basic{};
            if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &file) ||
                !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic)))
                return Result<FileInfo>::Failure(MakeError(ProjectReadErrors::ReadFailed));
            if ((file.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return Result<FileInfo>::Failure(MakeError(ProjectReadErrors::UnsafePath));
            const bool directory = (file.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const auto pair = [](const DWORD high, const DWORD low) {
                return (static_cast<std::uint64_t>(high) << 32U) | low;
            };
            return Result<FileInfo>::Success(
                {!directory,
                 directory,
                 pair(file.nFileSizeHigh, file.nFileSizeLow),
                 file.nNumberOfLinks,
                 {file.dwVolumeSerialNumber, pair(file.nFileIndexHigh, file.nFileIndexLow),
                  static_cast<std::uint64_t>(basic.CreationTime.QuadPart), static_cast<std::uint64_t>(basic.LastWriteTime.QuadPart),
                  static_cast<std::uint64_t>(basic.ChangeTime.QuadPart), file.dwFileAttributes}});
        }
    }  // namespace

    /** @copydoc NormalizeHandle */
    HandleValue NormalizeHandle(const HandleValue value) noexcept {
        return value == INVALID_HANDLE_VALUE ? InvalidHandle : value;
    }

    /** @copydoc CloseHandleValue */
    void CloseHandleValue(const HandleValue value) noexcept {
        CloseHandle(value);
    }

    /** @copydoc Info */
    Result<FileInfo> Info(const Handle &handle) {
        return CaptureInfo(handle.Get());
    }

    /** @copydoc OpenChild */
    Result<Handle> OpenChild(const Handle &parent, const std::string_view name, const bool directory) {
        if (name.empty() || name.find_first_of("/\\\0", 0, 3) != std::string_view::npos || name == "." || name == "..")
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::UnsafePath));
        const auto module = GetModuleHandleW(L"ntdll.dll");
        const auto create = module ? reinterpret_cast<CreateRelativeFile>(GetProcAddress(module, "NtCreateFile")) : nullptr;
        if (!create)
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::Unavailable));
        auto wide = Wide(name);
        if (wide.HasError())
            return Result<Handle>::Failure(wide.ErrorValue());
        auto spelling = std::move(wide).Value();
        if (spelling.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::Capacity));
        const auto byteLength = static_cast<USHORT>(spelling.size() * sizeof(wchar_t));
        UNICODE_STRING nativeName{byteLength, byteLength, spelling.data()};
        OBJECT_ATTRIBUTES attributes{sizeof(OBJECT_ATTRIBUTES), parent.Get(), &nativeName, CaseInsensitive, nullptr, nullptr};
        IO_STATUS_BLOCK status{};
        HANDLE opened{};
        const ULONG options = OpenReparsePoint | SynchronousIo | BackupIntent | (directory ? DirectoryOnly : 0);
        // Each name is a single segment relative to an owned directory. No absolute descendant reopen is permitted.
        const auto result =
            create(&opened, FILE_READ_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, &attributes, &status, nullptr, FILE_ATTRIBUTE_NORMAL,
                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OpenExisting, options, nullptr, 0);
        if (result < 0)
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::ReadFailed));
        Handle child{opened};
        return AdmitChild(std::move(child), directory);
    }

    /** @copydoc OpenAnchor */
    Result<Handle> OpenAnchor(const std::filesystem::path &path) {
        Handle root{CreateFileW(path.root_path().c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!root.IsValid())
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::ReadFailed));
        return Result<Handle>::Success(std::move(root));
    }

    /** @copydoc ComponentName */
    Result<std::string> ComponentName(const std::filesystem::path &part) {
        return Utf8(part.native());
    }

    /** @copydoc OpenDirectory */
    Result<Handle> OpenDirectory(const Handle &directory) {
        FILE_ID_INFO before{};
        if (!GetFileInformationByHandleEx(directory.Get(), FileIdInfo, &before, sizeof(before)))
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::Unavailable));
        FILE_ID_DESCRIPTOR identity{};
        identity.dwSize = sizeof(identity);
        identity.Type = ExtendedFileIdType;
        identity.ExtendedFileId = before.FileId;
        // Reopen this retained object by identity, creating an independent enumeration cursor.
        Handle copy{OpenFileById(directory.Get(), &identity, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT)};
        if (!copy.IsValid())
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::Unavailable));
        FILE_ID_INFO after{};
        if (!GetFileInformationByHandleEx(copy.Get(), FileIdInfo, &after, sizeof(after)) ||
            before.VolumeSerialNumber != after.VolumeSerialNumber ||
            !std::equal(std::begin(before.FileId.Identifier), std::end(before.FileId.Identifier), std::begin(after.FileId.Identifier)))
            return Result<Handle>::Failure(MakeError(ProjectReadErrors::Stale));
        return AdmitChild(std::move(copy), true);
    }

    /** @copydoc Read */
    Result<std::size_t> Read(const Handle &handle, const std::span<char> bytes) {
        DWORD count{};
        if (bytes.size() > MAXDWORD || !ReadFile(handle.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr))
            return Result<std::size_t>::Failure(MakeError(ProjectReadErrors::ReadFailed));
        return Result<std::size_t>::Success(count);
    }

    /** @copydoc Visit */
    Result<void> Visit(const Handle &handle, const ProjectReadContext &context,
                       const std::function<Result<void>(std::string_view)> &visitor) {
        auto independent = OpenDirectory(handle);
        if (independent.HasError())
            return Result<void>::Failure(independent.ErrorValue());
        alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 16384> buffer{};
        bool restart = true;
        for (;;) {
            if (auto stop = CheckProjectReadContext(context); stop.HasError())
                return stop;
            const auto kind = restart ? FileIdBothDirectoryRestartInfo : FileIdBothDirectoryInfo;
            restart = false;
            if (!GetFileInformationByHandleEx(independent.Value().Get(), kind, buffer.data(), static_cast<DWORD>(buffer.size()))) {
                return GetLastError() == ERROR_NO_MORE_FILES ? Result<void>::Success()
                                                             : Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
            }
            std::size_t offset{};
            for (;;) {
                if (auto stop = CheckProjectReadContext(context); stop.HasError())
                    return stop;
                constexpr auto headerBytes = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
                if (offset > buffer.size() - headerBytes)
                    return Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
                const auto *entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO *>(buffer.data() + offset);
                if (entry->FileNameLength % sizeof(wchar_t) != 0 || entry->FileNameLength > buffer.size() - offset - headerBytes)
                    return Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
                const std::wstring_view name{entry->FileName, entry->FileNameLength / sizeof(wchar_t)};
                if (name != L"." && name != L"..") {
                    auto portable = Utf8(name);
                    if (portable.HasError())
                        return Result<void>::Failure(portable.ErrorValue());
                    if (auto visited = visitor(portable.Value()); visited.HasError())
                        return visited;
                }
                if (entry->NextEntryOffset == 0)
                    break;
                if (entry->NextEntryOffset < headerBytes || entry->NextEntryOffset > buffer.size() - offset ||
                    entry->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) != 0)
                    return Result<void>::Failure(MakeError(ProjectReadErrors::ReadFailed));
                offset += entry->NextEntryOffset;
            }
        }
    }
}  // namespace Horo::Platform::ProjectReadNative
#endif
