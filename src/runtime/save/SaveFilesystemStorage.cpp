#include "Horo/Runtime/Save/SaveFilesystemStorage.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "SaveFilesystemPinnedRead.h"

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

namespace Horo::Runtime {
    using namespace SaveFilesystemNative;
    using SaveFilesystemReadDetails::PinArchiveRead;
    using SaveFilesystemReadDetails::PinnedArchiveRead;

    struct SaveFilesystemStorage::State final {
    private:
        // Worker-only mutex orders archive selection and replacements. Destruction/move requires
        // quiescent callers; accepted work retains its storage owner until completion.
        mutable std::mutex operationMutex_;

    public:
        /** @brief Selects an immutable file under lexical ownership; the returned handle holds no mutex. */
        [[nodiscard]] Result<PinnedArchiveRead> PinRead(const SaveGameSlotId slot, const std::size_t maximumBytes) const {
            const std::lock_guard operationLock{operationMutex_};
            if (auto valid = Verify(); valid.HasError())
                return Result<PinnedArchiveRead>::Failure(valid.ErrorValue());
            return PinArchiveRead(Slots(), slot, maximumBytes);
        }

        /** @brief Serializes complete publication while retaining namespace lifetime ownership. */
        [[nodiscard]] Result<void> Replace(const SaveGameSlotId slot, const std::span<const std::byte> bytes) const {
            const std::lock_guard operationLock{operationMutex_};
#ifdef _WIN32
            return ReplaceWindows(slot, bytes);
#else
            return ReplacePosix(slot, bytes);
#endif
        }
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
            if (processLock.IsValid())
                return SameEntry(Slots(), L".namespace.lock", processLock, kNonDirectoryFile);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AcquireProcessLock() {
            auto opened = RelativeOpen(Slots(), L".namespace.lock", GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES, kOpenIf,
                                       kNonDirectoryFile, FILE_ATTRIBUTE_NORMAL);
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            Handle lock = std::move(opened).Value();
            OVERLAPPED offset{};
            if (!::LockFileEx(lock.Get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset)) {
                const DWORD error = ::GetLastError();
                return Result<void>::Failure(
                    Failure(error == ERROR_LOCK_VIOLATION ? SaveErrors::OperationInProgress : SaveErrors::StorageCapabilityUnsupported,
                            "namespace lock", error));
            }
            processLock = std::move(lock);
            return Verify();
        }

    private:
        Handle processLock;
        std::filesystem::path rootPath;
        std::vector<Handle> directories;
        std::vector<std::wstring> names;

    public:
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
            if (processLock.Fd() >= 0 &&
                (::fstatat(Slots().Fd(), ".namespace.lock", &actual, AT_SYMLINK_NOFOLLOW) != 0 || ::fstat(processLock.Fd(), &held) != 0 ||
                 !S_ISREG(actual.st_mode) || actual.st_nlink != 1 || actual.st_dev != held.st_dev || actual.st_ino != held.st_ino))
                return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "namespace lock replacement"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AcquireProcessLock() {
            const int fd = ::openat(Slots().Fd(), ".namespace.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
            if (fd < 0)
                return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "namespace lock admission", errno));
            Directory lock{fd};
            if (struct stat info{}; ::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
                return Result<void>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "unsafe namespace lock"));
            int result;
            do {
                result = ::flock(fd, LOCK_EX | LOCK_NB);
            } while (result != 0 && errno == EINTR);
            if (result != 0) {
                const int error = errno;
                return Result<void>::Failure(Failure(error == EWOULDBLOCK || error == EAGAIN ? SaveErrors::OperationInProgress
                                                                                             : SaveErrors::StorageCapabilityUnsupported,
                                                     "namespace lock", error));
            }
            processLock = std::move(lock);
            return Verify();
        }

    private:
        Directory processLock;
        std::filesystem::path rootPath;
        std::vector<Directory> directories;
        std::vector<std::string> names;

    public:
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
        const auto step = [&directories, &names](const std::string &component) {
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
        const auto step = [&directories, &names](const std::string &component) {
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
        if (auto locked = state.Value()->AcquireProcessLock(); locked.HasError())
            return Result<SaveFilesystemStorage>::Failure(locked.ErrorValue());
        return Result<SaveFilesystemStorage>::Success(SaveFilesystemStorage{std::move(state).Value()});
    }

    /** @copydoc SaveFilesystemStorage::Read */
    Result<std::vector<std::byte>> SaveFilesystemStorage::Read(const SaveGameSlotId slot, const std::size_t maximumBytes) const {
        if (!state_ || !slot.IsValid() || maximumBytes == 0)
            return Result<std::vector<std::byte>>::Failure(Failure(SaveErrors::StorageOperationInvalid, "read validation"));
        auto pinned = state_->PinRead(slot, maximumBytes);
        if (pinned.HasError())
            return Result<std::vector<std::byte>>::Failure(pinned.ErrorValue());
        return std::move(pinned).Value().Read();
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
        const std::string temporary =
            std::format(".{}.{}.{}.temporary", destination, ::getpid(), sequence.fetch_add(1, std::memory_order_relaxed));
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
        return state_->Replace(slot, bytes);
    }
}  // namespace Horo::Runtime
