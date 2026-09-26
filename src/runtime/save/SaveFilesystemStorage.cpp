#include "Horo/Runtime/Save/SaveFilesystemStorage.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveFilesystemStorageDetails.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <winternl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Horo::Runtime {
    namespace {
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

        [[nodiscard]] Result<Handle> RelativeOpen(const Handle &parent, const std::wstring &name, const ACCESS_MASK access,
                                                  const ULONG disposition, const ULONG options, const ULONG attributes) {
            if (name.empty() || name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
                return Result<Handle>::Failure(Failure(SaveErrors::StorageOperationInvalid, "Windows component length"));
            const HMODULE library = ::GetModuleHandleW(L"ntdll.dll");
            const auto create = library ? reinterpret_cast<decltype(&NtCreateFile)>(::GetProcAddress(library, "NtCreateFile")) : nullptr;
            if (!create)
                return Result<Handle>::Failure(Failure(SaveErrors::StorageCapabilityUnsupported, "relative Windows creation"));
            UNICODE_STRING text{};
            text.Buffer = const_cast<PWSTR>(name.data());
            text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
            text.MaximumLength = text.Length;
            OBJECT_ATTRIBUTES object{};
            object.Length = sizeof(object);
            object.RootDirectory = parent.Get();
            object.ObjectName = &text;
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

        [[nodiscard]] Result<void> SameEntry(const Handle &parent, const std::wstring &name, const Handle &held, const ULONG kind) {
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

        [[nodiscard]] Result<void> ExistingWindowsTargetSafe(const Handle &directory, const std::wstring &name) {
            if (name.empty() || name.size() > std::numeric_limits<USHORT>::max() / sizeof(wchar_t))
                return Result<void>::Failure(Failure(SaveErrors::StorageOperationInvalid, "Windows target length"));
            const HMODULE library = ::GetModuleHandleW(L"ntdll.dll");
            const auto create = library ? reinterpret_cast<decltype(&NtCreateFile)>(::GetProcAddress(library, "NtCreateFile")) : nullptr;
            if (!create)
                return Result<void>::Failure(Failure(SaveErrors::StorageCapabilityUnsupported, "Windows target inspection"));
            UNICODE_STRING text{};
            text.Buffer = const_cast<PWSTR>(name.data());
            text.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
            text.MaximumLength = text.Length;
            OBJECT_ATTRIBUTES object{};
            object.Length = sizeof(object);
            object.RootDirectory = directory.Get();
            object.ObjectName = &text;
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

        [[nodiscard]] Result<void> RenameWindowsFile(HANDLE file, HANDLE directory, const std::wstring &destination) {
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
            rename->ReplaceIfExists = TRUE;
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
            // WDK FILE_INFORMATION_CLASS fixes FileRenameInformation at 10; the user-mode
            // winternl.h enum omits it. See MicrosoftDocs/windows-driver-docs-ddi at
            // 7515063cea4c9e98db6a92986c5b4ddb0463fd16, ne-wdm-_file_information_class.md.
            constexpr ULONG kFileRenameInformation = 10;
            const NTSTATUS result = set(file, &status, rename, static_cast<ULONG>(size), kFileRenameInformation);
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

        [[nodiscard]] Result<Directory> Child(const Directory &parent, const std::string &name) {
            if (::mkdirat(parent.Fd(), name.c_str(), 0700) != 0 && errno != EEXIST)
                return Result<Directory>::Failure(Failure(SaveErrors::StoragePermanentIo, "directory creation", errno));
            const int fd = ::openat(parent.Fd(), name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0)
                return Result<Directory>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "directory admission", errno));
            return Result<Directory>::Success(Directory{fd});
        }

        [[nodiscard]] Result<void> ExistingTargetSafe(const Directory &directory, const std::string &name) {
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

        [[nodiscard]] Result<void> WritePosixBytes(const int fd, const std::span<const std::byte> bytes) {
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const std::size_t remaining =
                    std::min(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
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
    }  // namespace

    struct SaveFilesystemStorage::State final {
#ifdef _WIN32
        State(std::filesystem::path path, std::vector<Handle> opened, std::vector<std::wstring> components)
            : rootPath(std::move(path)), directories(std::move(opened)), names(std::move(components)) {}

        [[nodiscard]] const Handle &Slots() const noexcept {
            return directories.back();
        }

        [[nodiscard]] Result<void> Verify() const {
            Handle current{::CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            if (!current.IsValid())
                return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows root replacement"));
            BY_HANDLE_FILE_INFORMATION actual{}, held{};
            if (!::GetFileInformationByHandle(current.Get(), &actual) || !::GetFileInformationByHandle(directories.front().Get(), &held) ||
                actual.dwVolumeSerialNumber != held.dwVolumeSerialNumber || actual.nFileIndexHigh != held.nFileIndexHigh ||
                actual.nFileIndexLow != held.nFileIndexLow)
                return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows root replacement"));
            FILE_ATTRIBUTE_TAG_INFO tag{};
            if (!::GetFileInformationByHandleEx(current.Get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows root redirection"));
            for (std::size_t index = 1; index < directories.size(); ++index) {
                if (auto same = SameEntry(directories[index - 1], names[index - 1], directories[index], kDirectoryFile); same.HasError())
                    return same;
            }
            return Result<void>::Success();
        }

        std::filesystem::path rootPath;
        std::vector<Handle> directories;
        std::vector<std::wstring> names;
        [[nodiscard]] static Result<std::unique_ptr<State>> OpenWindows(const ProductSaveRoot &root, const SaveNamespaceId &name);
        [[nodiscard]] Result<void> ReplaceWindows(SaveGameSlotId slot, std::span<const std::byte> bytes) const;
#else
        State(std::filesystem::path path, std::vector<Directory> opened, std::vector<std::string> components)
            : rootPath(std::move(path)), directories(std::move(opened)), names(std::move(components)) {}

        [[nodiscard]] const Directory &Slots() const noexcept {
            return directories.back();
        }

        [[nodiscard]] Result<void> Verify() const {
            struct stat actual{};
            struct stat held{};
            if (::lstat(rootPath.c_str(), &actual) != 0 || ::fstat(directories.front().Fd(), &held) != 0 || !S_ISDIR(actual.st_mode) ||
                actual.st_dev != held.st_dev || actual.st_ino != held.st_ino)
                return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "root replacement"));
            for (std::size_t index = 1; index < directories.size(); ++index) {
                if (::fstatat(directories[index - 1].Fd(), names[index - 1].c_str(), &actual, AT_SYMLINK_NOFOLLOW) != 0 ||
                    ::fstat(directories[index].Fd(), &held) != 0 || !S_ISDIR(actual.st_mode) || actual.st_dev != held.st_dev ||
                    actual.st_ino != held.st_ino)
                    return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "namespace replacement"));
            }
            return Result<void>::Success();
        }

        std::filesystem::path rootPath;
        std::vector<Directory> directories;
        std::vector<std::string> names;
        [[nodiscard]] static Result<std::unique_ptr<State>> OpenPosix(const ProductSaveRoot &root, const SaveNamespaceId &name);
        [[nodiscard]] Result<void> ReplacePosix(SaveGameSlotId slot, std::span<const std::byte> bytes) const;
#endif
    };

    SaveFilesystemStorage::SaveFilesystemStorage(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    SaveFilesystemStorage::SaveFilesystemStorage(SaveFilesystemStorage &&) noexcept = default;
    SaveFilesystemStorage &SaveFilesystemStorage::operator=(SaveFilesystemStorage &&) noexcept = default;
    SaveFilesystemStorage::~SaveFilesystemStorage() = default;

#ifdef _WIN32
    Result<std::unique_ptr<SaveFilesystemStorage::State>> SaveFilesystemStorage::State::OpenWindows(const ProductSaveRoot &root,
                                                                                                    const SaveNamespaceId &name) {
        Handle openedRoot{::CreateFileW(root.CanonicalPath().c_str(), FILE_READ_ATTRIBUTES | FILE_TRAVERSE | FILE_ADD_SUBDIRECTORY,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
        if (!openedRoot.IsValid())
            return Result<std::unique_ptr<State>>::Failure(
                Failure(SaveErrors::SaveRootContainmentViolation, "Windows root admission", ::GetLastError()));
        FILE_ATTRIBUTE_TAG_INFO rootTag{};
        if (!::GetFileInformationByHandleEx(openedRoot.Get(), FileAttributeTagInfo, &rootTag, sizeof(rootTag)) ||
            (rootTag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 || (rootTag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return Result<std::unique_ptr<State>>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "Windows root admission"));
        std::vector<Handle> directories;
        std::vector<std::wstring> names;
        directories.push_back(std::move(openedRoot));
        const auto step = [&directories, &names](const std::string &component) -> Result<void> {
            const std::wstring wide(component.begin(), component.end());
            auto child = RelativeOpen(directories.back(), wide,
                                      FILE_READ_ATTRIBUTES | FILE_TRAVERSE | FILE_ADD_SUBDIRECTORY | FILE_ADD_FILE | FILE_DELETE_CHILD,
                                      kOpenIf, kDirectoryFile, FILE_ATTRIBUTE_DIRECTORY);
            if (child.HasError())
                return Result<void>::Failure(child.ErrorValue());
            names.push_back(wide);
            directories.push_back(std::move(child).Value());
            return Result<void>::Success();
        };
        if (auto opened = SaveFilesystemDetails::OpenNamespaceComponents(name, step); opened.HasError())
            return Result<std::unique_ptr<State>>::Failure(opened.ErrorValue());
        auto state = std::make_unique<State>(root.CanonicalPath(), std::move(directories), std::move(names));
        if (auto valid = state->Verify(); valid.HasError())
            return Result<std::unique_ptr<State>>::Failure(valid.ErrorValue());
        return Result<std::unique_ptr<State>>::Success(std::move(state));
    }
#else
    Result<std::unique_ptr<SaveFilesystemStorage::State>> SaveFilesystemStorage::State::OpenPosix(const ProductSaveRoot &root,
                                                                                                  const SaveNamespaceId &name) {
        const int rootFd = ::open(root.CanonicalPath().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (rootFd < 0)
            return Result<std::unique_ptr<State>>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "root admission", errno));
        std::vector<Directory> directories;
        std::vector<std::string> names;
        directories.emplace_back(rootFd);
        const auto step = [&directories, &names](const std::string &component) -> Result<void> {
            auto child = Child(directories.back(), component);
            if (child.HasError())
                return Result<void>::Failure(child.ErrorValue());
            names.push_back(component);
            directories.push_back(std::move(child).Value());
            return Result<void>::Success();
        };
        if (auto opened = SaveFilesystemDetails::OpenNamespaceComponents(name, step); opened.HasError())
            return Result<std::unique_ptr<State>>::Failure(opened.ErrorValue());
        auto state = std::make_unique<State>(root.CanonicalPath(), std::move(directories), std::move(names));
        if (auto valid = state->Verify(); valid.HasError())
            return Result<std::unique_ptr<State>>::Failure(valid.ErrorValue());
        return Result<std::unique_ptr<State>>::Success(std::move(state));
    }
#endif

    /** @copydoc SaveFilesystemStorage::Open */
    Result<SaveFilesystemStorage> SaveFilesystemStorage::Open(const ProductSaveRoot &root, const SaveNamespaceId &name) {
        if (!root.IsValid() || !name.IsValid() || root.Product() != name.product)
            return Result<SaveFilesystemStorage>::Failure(Failure(SaveErrors::StorageOperationInvalid, "namespace validation"));
#ifdef _WIN32
        auto state = State::OpenWindows(root, name);
#else
        auto state = State::OpenPosix(root, name);
#endif
        if (state.HasError())
            return Result<SaveFilesystemStorage>::Failure(state.ErrorValue());
        return Result<SaveFilesystemStorage>::Success(SaveFilesystemStorage{std::move(state).Value()});
    }

    /** @copydoc SaveFilesystemStorage::Read */
    Result<std::vector<std::byte>> SaveFilesystemStorage::Read(const SaveGameSlotId slot, const std::size_t maximumBytes) const {
        if (!state_ || !slot.IsValid() || maximumBytes == 0)
            return Result<std::vector<std::byte>>::Failure(Failure(SaveErrors::StorageOperationInvalid, "read validation"));
        if (auto valid = state_->Verify(); valid.HasError())
            return Result<std::vector<std::byte>>::Failure(valid.ErrorValue());
#ifdef _WIN32
        const std::string narrow = SlotName(slot);
        const std::wstring name(narrow.begin(), narrow.end());
        auto opened = RelativeOpen(state_->Slots(), name, GENERIC_READ, kOpen, kNonDirectoryFile, FILE_ATTRIBUTE_NORMAL);
        if (opened.HasError())
            return Result<std::vector<std::byte>>::Failure(opened.ErrorValue());
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(opened.Value().Get(), &size) || size.QuadPart < 0 || static_cast<std::uintmax_t>(size.QuadPart) > maximumBytes)
            return Result<std::vector<std::byte>>::Failure(Failure(SaveErrors::StorageOperationInvalid, "Windows slot size"));
        std::vector<std::byte> bytes(static_cast<std::size_t>(size.QuadPart));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const DWORD amount = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
            DWORD received{};
            if (!::ReadFile(opened.Value().Get(), bytes.data() + offset, amount, &received, nullptr) || received == 0)
                return Result<std::vector<std::byte>>::Failure(
                    Failure(SaveErrors::StoragePermanentIo, "Windows slot read", ::GetLastError()));
            offset += received;
        }
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
#else
        const std::string name = SlotName(slot);
        const int fd = ::openat(state_->Slots().Fd(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
        if (fd < 0)
            return Result<std::vector<std::byte>>::Failure(Failure(SaveErrors::StoragePermanentIo, "slot open", errno));
        Directory file{fd};
        struct stat entry{};
        if (::fstat(fd, &entry) != 0)
            return Result<std::vector<std::byte>>::Failure(Failure(SaveErrors::StoragePermanentIo, "slot inspection", errno));
        if (!S_ISREG(entry.st_mode) || entry.st_nlink != 1)
            return Result<std::vector<std::byte>>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "unsafe slot"));
        if (entry.st_size < 0 || static_cast<std::uintmax_t>(entry.st_size) > maximumBytes)
            return Result<std::vector<std::byte>>::Failure(Failure(SaveErrors::StorageOperationInvalid, "slot size"));
        std::vector<std::byte> bytes(static_cast<std::size_t>(entry.st_size));
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const std::size_t remaining = std::min(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
            // EINTR retries unchanged; EOF/errors return; a positive read advances by at most remaining.
            // flawfinder: ignore - offset < bytes.size() and remaining <= bytes.size() - offset.
            const ssize_t read = ::read(fd, bytes.data() + offset, remaining);
            if (read < 0 && errno == EINTR)
                continue;
            if (read <= 0)
                return Result<std::vector<std::byte>>::Failure(Failure(SaveErrors::StoragePermanentIo, "slot read", read < 0 ? errno : 0));
            offset += static_cast<std::size_t>(read);
        }
        return Result<std::vector<std::byte>>::Success(std::move(bytes));
#endif
    }

#ifdef _WIN32
    Result<void> SaveFilesystemStorage::State::ReplaceWindows(const SaveGameSlotId slot, const std::span<const std::byte> bytes) const {
        if (auto valid = Verify(); valid.HasError())
            return valid;
        const std::string narrow = SlotName(slot);
        const std::wstring destination(narrow.begin(), narrow.end());
        if (auto safe = ExistingWindowsTargetSafe(Slots(), destination); safe.HasError())
            return safe;
        static std::atomic_uint64_t sequence{0};
        const std::wstring temporary = L"." + destination + L"." + std::to_wstring(::GetCurrentProcessId()) + L"." +
                                       std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)) + L".temporary";
        auto created = RelativeOpen(Slots(), temporary, GENERIC_WRITE | FILE_READ_ATTRIBUTES | DELETE, kCreate, kNonDirectoryFile,
                                    FILE_ATTRIBUTE_NORMAL);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        Handle file = std::move(created).Value();
        SaveFilesystemDetails::WindowsTemporary cleanup{file.Get()};
        if (auto written = SaveFilesystemDetails::WriteWindowsBytes(file.Get(), bytes); written.HasError())
            return written;
        if (auto valid = Verify(); valid.HasError())
            return valid;
        if (auto safe = ExistingWindowsTargetSafe(Slots(), destination); safe.HasError())
            return safe;
        if (auto renamed = RenameWindowsFile(file.Get(), Slots().Get(), destination); renamed.HasError())
            return renamed;
        cleanup.Published();
        if (!::FlushFileBuffers(file.Get()))
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "Windows publication synchronization", ::GetLastError()));
        return Result<void>::Success();
    }
#else
    Result<void> SaveFilesystemStorage::State::ReplacePosix(const SaveGameSlotId slot, const std::span<const std::byte> bytes) const {
        if (auto valid = Verify(); valid.HasError())
            return valid;
        const std::string destination = SlotName(slot);
        if (auto safe = ExistingTargetSafe(Slots(), destination); safe.HasError())
            return safe;
        static std::atomic_uint64_t sequence{0};
        const std::string temporary = "." + destination + "." + std::to_string(::getpid()) + "." +
                                      std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".temporary";
        const int fd = ::openat(Slots().Fd(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "temporary creation", errno));
        bool published = false;
        const auto cleanup = [&] {
            ::close(fd);
            if (!published)
                ::unlinkat(Slots().Fd(), temporary.c_str(), 0);
        };
        if (auto written = WritePosixBytes(fd, bytes); written.HasError()) {
            cleanup();
            return written;
        }
        if (auto valid = Verify(); valid.HasError()) {
            cleanup();
            return valid;
        }
        if (auto safe = ExistingTargetSafe(Slots(), destination); safe.HasError()) {
            cleanup();
            return safe;
        }
        if (::renameat(Slots().Fd(), temporary.c_str(), Slots().Fd(), destination.c_str()) != 0) {
            const int error = errno;
            cleanup();
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "atomic replacement", error));
        }
        published = true;
        ::close(fd);
        if (::fsync(Slots().Fd()) != 0)
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "directory synchronization", errno));
        return Result<void>::Success();
    }
#endif

    /** @copydoc SaveFilesystemStorage::Replace */
    Result<void> SaveFilesystemStorage::Replace(const SaveGameSlotId slot, const std::span<const std::byte> bytes) const {
        if (!state_ || !slot.IsValid() || bytes.empty())
            return Result<void>::Failure(Failure(SaveErrors::StorageOperationInvalid, "replacement validation"));
#ifdef _WIN32
        return state_->ReplaceWindows(slot, bytes);
#else
        return state_->ReplacePosix(slot, bytes);
#endif
    }
}  // namespace Horo::Runtime
