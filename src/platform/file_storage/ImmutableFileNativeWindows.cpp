#include "ImmutableFileNative.h"

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

namespace Horo::Platform::ImmutableFileNative {
    namespace {
        using CreateRelativeFile = NTSTATUS(NTAPI *)(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG,
                                                     ULONG, ULONG, ULONG, PVOID, ULONG);
        enum class OpenKind {
            Directory,
            CreateParents,
            Read,
            FlushableRead,
            PrivateDirectory,
            PrivateFile
        };

        /** @brief Separates exclusive preparation, parent creation and read-only namespace admission. */
        ULONG CreationDisposition(const OpenKind kind) noexcept {
            using enum OpenKind;
            if (kind == PrivateDirectory || kind == PrivateFile)
                return 2U;
            return kind == CreateParents ? 3U : 1U;
        }

        /** @brief Maps object redirection/type failures without disclosing an NT path. */
        Error OpenError(const ULONG status) {
            const bool unsafe = status == 0xc0000103U || status == 0xc0000024U || status == 0xc000050bU;
            return MakeError(unsafe ? FileStorageErrors::UnsafeObject : FileStorageErrors::IoFailed,
                             "Native immutable component open failed; NTSTATUS " + std::to_string(status));
        }

        /** @brief Traversal handles carry no directory data access that can conflict with the native relative rename target open. */
        ACCESS_MASK OpenAccess(const OpenKind kind) noexcept {
            const bool directory = kind == OpenKind::Directory || kind == OpenKind::CreateParents || kind == OpenKind::PrivateDirectory;
            ACCESS_MASK access = FILE_READ_ATTRIBUTES | SYNCHRONIZE | (directory ? FILE_TRAVERSE : FILE_READ_DATA);
            if (kind == OpenKind::PrivateDirectory)
                access |= FILE_ADD_FILE | DELETE;
            if (kind == OpenKind::PrivateFile)
                access |= FILE_WRITE_DATA | DELETE;
            if (kind == OpenKind::FlushableRead)
                access |= FILE_WRITE_DATA;
            return access;
        }

        /** @brief Opens or exclusively creates one component relative to an already retained directory identity. */
        Result<std::optional<Handle>> Open(const Handle &parent, const std::string_view name, const OpenKind kind) {
            if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\\0", 0, 3) != std::string_view::npos)
                return Result<std::optional<Handle>>::Failure(MakeError(FileStorageErrors::InvalidPath));
            const auto ntLibrary = GetModuleHandleW(L"ntdll.dll");
            const auto entry = ntLibrary ? GetProcAddress(ntLibrary, "NtCreateFile") : nullptr;
            const auto create = std::bit_cast<CreateRelativeFile>(entry);
            if (!create)
                return Result<std::optional<Handle>>::Failure(MakeError(FileStorageErrors::IoFailed));
            // All callers admit UTF-8 components first; the standard UTF-8 path conversion does not use the ANSI code page.
            auto spelling = std::filesystem::path{std::u8string{name.begin(), name.end()}}.native();
            if (spelling.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
                return Result<std::optional<Handle>>::Failure(MakeError(FileStorageErrors::Capacity));
            const bool directory = kind == OpenKind::Directory || kind == OpenKind::CreateParents || kind == OpenKind::PrivateDirectory;
            const bool exclusive = kind == OpenKind::PrivateDirectory || kind == OpenKind::PrivateFile;
            const auto byteLength = static_cast<USHORT>(spelling.size() * sizeof(wchar_t));
            UNICODE_STRING nativeName{byteLength, byteLength, spelling.data()};
            OBJECT_ATTRIBUTES attributes{};
            attributes.Length = sizeof(attributes);
            attributes.RootDirectory = parent.Get();
            attributes.ObjectName = &nativeName;
            attributes.Attributes = 0x00000040U;
            IO_STATUS_BLOCK io{};
            HANDLE opened{};
            constexpr ULONG openReparsePoint = 0x00200000U;
            constexpr ULONG synchronousIo = 0x00000020U;
            constexpr ULONG backupIntent = 0x00004000U;
            constexpr ULONG directoryOnly = 0x00000001U;
            constexpr ULONG nonDirectoryOnly = 0x00000040U;
            const ULONG disposition = CreationDisposition(kind);
            const ULONG options = openReparsePoint | synchronousIo | backupIntent | (directory ? directoryOnly : nonDirectoryOnly);
            if (const auto result = create(&opened, OpenAccess(kind), &attributes, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, disposition, options, nullptr, 0);
                result < 0) {
                const auto status = static_cast<ULONG>(result);
                if (status == 0xc0000034U || status == 0xc000003aU || status == 0xc000000fU || (exclusive && status == 0xc0000035U))
                    return Result<std::optional<Handle>>::Success(std::nullopt);
                return Result<std::optional<Handle>>::Failure(OpenError(status));
            }
            Handle object{opened};
            auto info = ProjectReadNative::Info(object);
            if (info.HasError())
                return Result<std::optional<Handle>>::Failure(WrapError(FileStorageErrors::UnsafeObject, info.ErrorValue()));
            if (directory ? !info.Value().directory : (!info.Value().regular || info.Value().links != 1))
                return Result<std::optional<Handle>>::Failure(MakeError(FileStorageErrors::UnsafeObject));
            return Result<std::optional<Handle>>::Success(std::move(object));
        }

        /** @brief Marks an operation-owned handle for removal without reopening its former pathname. */
        void DeleteOwned(const Handle &handle) noexcept {
            if (handle.IsValid()) {
                FILE_DISPOSITION_INFO disposition{TRUE};
                static_cast<void>(SetFileInformationByHandle(handle.Get(), FileDispositionInfo, &disposition, sizeof(disposition)));
            }
        }
    }  // namespace

    /** @copydoc Directory */
    Result<std::optional<Handle>> Directory(const Handle &parent, const std::string_view name, const bool create) {
        return Open(parent, name, create ? OpenKind::CreateParents : OpenKind::Directory);
    }

    /** @copydoc Regular */
    Result<std::optional<Handle>> Regular(const Handle &parent, const std::string_view name, const bool flushable) {
        return Open(parent, name, flushable ? OpenKind::FlushableRead : OpenKind::Read);
    }

    /** @copydoc Prepare */
    Result<std::optional<Preparation>> Prepare(const Handle &parent, std::string name) {
        auto directory = Open(parent, name, OpenKind::PrivateDirectory);
        if (directory.HasError())
            return Result<std::optional<Preparation>>::Failure(directory.ErrorValue());
        if (!directory.Value())
            return Result<std::optional<Preparation>>::Success(std::nullopt);
        auto file = Open(*directory.Value(), "payload", OpenKind::PrivateFile);
        if (file.HasError() || !file.Value()) {
            DeleteOwned(*directory.Value());
            return Result<std::optional<Preparation>>::Failure(file.HasError() ? file.ErrorValue()
                                                                               : MakeError(FileStorageErrors::IoFailed));
        }
        return Result<std::optional<Preparation>>::Success(
            Preparation{*std::move(directory).Value(), *std::move(file).Value(), std::move(name)});
    }

    /** @copydoc Write */
    Result<void> Write(const Handle &file, const std::span<const std::byte> bytes, const CancellationToken &cancellation) {
        std::size_t offset{};
        while (offset < bytes.size()) {
            if (cancellation.IsCancellationRequested())
                return Result<void>::Failure(MakeError(FileStorageErrors::Cancelled));
            DWORD written{};
            if (const auto chunk = static_cast<DWORD>(std::min(std::size_t{65536}, bytes.size() - offset));
                !WriteFile(file.Get(), bytes.data() + offset, chunk, &written, nullptr) || written == 0 || written > chunk)
                return Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
            offset += written;
        }
        auto info = ProjectReadNative::Info(file);
        if (info.HasError())
            return Result<void>::Failure(WrapError(FileStorageErrors::UnsafeObject, info.ErrorValue()));
        if (!info.Value().regular || info.Value().links != 1)
            return Result<void>::Failure(MakeError(FileStorageErrors::UnsafeObject));
        return Sync(file);
    }

    /** @copydoc Publish */
    Result<bool> Publish(const Handle &parent, Preparation &preparation, const std::string_view destination) {
        const auto spelling = std::filesystem::path{std::u8string{destination.begin(), destination.end()}}.native();
        const auto nameBytes = spelling.size() * sizeof(wchar_t);
        const auto totalBytes = sizeof(FILE_RENAME_INFO) + nameBytes;
        alignas(FILE_RENAME_INFO) std::array<std::byte, 16384> buffer{};
        if (totalBytes > buffer.size())
            return Result<bool>::Failure(MakeError(FileStorageErrors::Capacity));
        // Serialize a native header and its bounded UTF-16 tail without forming an out-of-bounds flexible-array pointer.
        FILE_RENAME_INFO header{};
        std::ranges::fill(std::as_writable_bytes(std::span{&header, 1}), std::byte{});
        header.ReplaceIfExists = FALSE;
        header.RootDirectory = parent.Get();
        header.FileNameLength = static_cast<DWORD>(nameBytes);
        std::ranges::copy(std::as_bytes(std::span{&header, 1}).first(offsetof(FILE_RENAME_INFO, FileName)), buffer.begin());
        std::ranges::copy(std::as_bytes(std::span{spelling}), buffer.begin() + offsetof(FILE_RENAME_INFO, FileName));
        if (SetFileInformationByHandle(preparation.file.Get(), FileRenameInfo, buffer.data(), static_cast<DWORD>(totalBytes))) {
            preparation.published = true;
            return Result<bool>::Success(true);
        }
        const auto error = GetLastError();
        return error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS
                   ? Result<bool>::Success(false)
                   : Result<bool>::Failure(MakeError(FileStorageErrors::IoFailed,
                                                     "Native immutable relative publication failed; Win32 code " + std::to_string(error)));
    }

    /** @copydoc Sync */
    Result<void> Sync(const Handle &handle) {
        auto info = ProjectReadNative::Info(handle);
        if (info.HasError())
            return Result<void>::Failure(WrapError(FileStorageErrors::IoFailed, info.ErrorValue()));
        // Match the existing native filesystem policy: Windows has no supported directory-flush operation here.
        if (info.Value().directory)
            return Result<void>::Success();
        return FlushFileBuffers(handle.Get()) ? Result<void>::Success() : Result<void>::Failure(MakeError(FileStorageErrors::IoFailed));
    }

    /** @copydoc Discard */
    void Discard(const Handle &, Preparation &preparation) noexcept {
        if (!preparation.published)
            DeleteOwned(preparation.file);
        preparation.file = Handle{};
        DeleteOwned(preparation.directory);
    }
}  // namespace Horo::Platform::ImmutableFileNative
#endif
