#pragma once

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveFilesystemStorage.h"
#include "Horo/Runtime/Save/SaveSlotLifecycle.h"
#include "SaveFilesystemPinnedRead.h"

#include <mutex>
#include <new>
#include <string_view>

namespace Horo::Runtime {
    namespace Native = SaveFilesystemNative;
    using SaveFilesystemReadDetails::PinnedArchiveRead;

    struct SaveFilesystemStorage::State final {
    private:
        // Worker-only mutex orders archive selection and replacements. Destruction/move requires
        // quiescent callers; accepted work retains its storage owner until completion.
        mutable std::mutex operationMutex_;
        ISaveSlotLifecycleIoObserver *observer_{};

    public:
        void SetObserver(ISaveSlotLifecycleIoObserver *observer) noexcept {
            observer_ = observer;
        }

        /** @brief Observes internal lifecycle artifacts only; the caller holds worker operation ownership. */
        [[nodiscard]] Result<void> Before(const SaveSlotLifecycleIoStage stage, const std::string_view name) const {
            if (!observer_ || (name != ".lifecycle.catalog" && name != ".lifecycle.journal" && !name.starts_with(".generation.")))
                return Result<void>::Success();
            using enum SaveSlotLifecycleFileKind;
            auto kind = Generation;
            if (name == ".lifecycle.catalog")
                kind = Catalog;
            else if (name == ".lifecycle.journal")
                kind = Journal;
            return observer_->Before(stage, kind);
        }

        /** @brief Reports synchronization failure after visibility using preallocated outcome evidence. */
        [[nodiscard]] Result<void> SynchronizePublication(const std::string &name, Error fallback,
#ifdef _WIN32
                                                          HANDLE file,
#else
                                                          int directory,
#endif
                                                          const bool observe = true, const bool externalExport = false) const {
            try {
                auto synced = observe ? Before(SaveSlotLifecycleIoStage::DirectorySync, name) : Result<void>::Success();
#ifdef _WIN32
                if (synced.HasValue() && !::FlushFileBuffers(file))
                    synced = Result<void>::Failure(
                        Native::Failure(SaveErrors::StoragePermanentIo, "Windows publication synchronization", ::GetLastError()));
#else
                if (synced.HasValue() && ::fsync(directory) != 0)
                    synced = Result<void>::Failure(Native::Failure(SaveErrors::StoragePermanentIo, "directory synchronization", errno));
#endif
                if (synced.HasError() && (name == ".lifecycle.catalog" || externalExport))
                    return Result<void>::Failure(WrapError(SaveErrors::SlotCommitOutcomeUnknown, synced.ErrorValue()));
                return synced;
            } catch (const std::bad_alloc &) {
                return Result<void>::Failure(std::move(fallback));
            }
        }

        /** @brief Reconciles selected manifest durability before any recovery cleanup. */
        [[nodiscard]] Result<void> SynchronizeCatalog() const {
            const std::lock_guard operationLock{operationMutex_};
            if (auto valid = Verify(); valid.HasError())
                return valid;
            auto fallback = MakeError(SaveErrors::SlotCommitOutcomeUnknown);
#ifdef _WIN32
            auto file = Native::RelativeOpen(Slots(), L".lifecycle.catalog", GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                             Native::kOpen, Native::kNonDirectoryFile, FILE_ATTRIBUTE_NORMAL);
            if (file.HasError())
                return Result<void>::Failure(file.ErrorValue());
            return SynchronizePublication(".lifecycle.catalog", std::move(fallback), file.Value().Get(), false);
#else
            const int fd = ::openat(Slots().Fd(), ".lifecycle.catalog", O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0)
                return Result<void>::Failure(Native::Failure(SaveErrors::StoragePermanentIo, "selection reconciliation", errno));
            Native::Directory file{fd};
            if (struct stat entry{}; ::fstat(fd, &entry) != 0 || !S_ISREG(entry.st_mode) || entry.st_nlink != 1)
                return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "selection reconciliation"));
            if (::fsync(fd) != 0)
                return Result<void>::Failure(std::move(fallback));
            return SynchronizePublication(".lifecycle.catalog", std::move(fallback), Slots().Fd(), false);
#endif
        }

        /** @brief Inspects a fixed internal record, distinguishing absence without following redirects. */
        [[nodiscard]] Result<bool> Exists(const std::string &name) const {
            const std::lock_guard operationLock{operationMutex_};
            if (auto valid = Verify(); valid.HasError())
                return Result<bool>::Failure(valid.ErrorValue());
#ifdef _WIN32
            bool missing = false;
            auto opened = Native::RelativeOpen(Slots(), std::wstring(name.begin(), name.end()), FILE_READ_ATTRIBUTES, Native::kOpen,
                                               Native::kNonDirectoryFile, FILE_ATTRIBUTE_NORMAL, &missing);
            if (missing)
                return Result<bool>::Success(false);
            if (opened.HasError())
                return Result<bool>::Failure(opened.ErrorValue());
#else
            struct stat entry{};
            if (::fstatat(Slots().Fd(), name.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0) {
                if (errno == ENOENT)
                    return Result<bool>::Success(false);
                return Result<bool>::Failure(Native::Failure(SaveErrors::StoragePermanentIo, "record inspection", errno));
            }
            if (!S_ISREG(entry.st_mode) || entry.st_nlink != 1)
                return Result<bool>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "unsafe record"));
#endif
            return Result<bool>::Success(true);
        }

        /** @brief Removes a retired internal record under namespace operation ownership. */
        [[nodiscard]] Result<void> Remove(const std::string &name) const {
            const std::lock_guard operationLock{operationMutex_};
            if (auto valid = Verify(); valid.HasError())
                return valid;
            if (auto admitted = Before(SaveSlotLifecycleIoStage::Remove, name); admitted.HasError())
                return admitted;
#ifdef _WIN32
            bool missing = false;
            auto opened =
                Native::RelativeOpen(Slots(), std::wstring(name.begin(), name.end()), DELETE | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                     Native::kOpen, Native::kNonDirectoryFile, FILE_ATTRIBUTE_NORMAL, &missing);
            if (missing)
                return Result<void>::Success();
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!::SetFileInformationByHandle(opened.Value().Get(), FileDispositionInfo, &disposition, sizeof(disposition)))
                return Result<void>::Failure(
                    Native::Failure(SaveErrors::StoragePermanentIo, "retired generation removal", ::GetLastError()));
            if (auto admitted = Before(SaveSlotLifecycleIoStage::DirectorySync, name); admitted.HasError())
                return admitted;
            if (!::FlushFileBuffers(opened.Value().Get()))
                return Result<void>::Failure(
                    Native::Failure(SaveErrors::StoragePermanentIo, "Windows retirement synchronization", ::GetLastError()));
#else
            if (auto safe = Native::ExistingTargetSafe(Slots(), name); safe.HasError())
                return safe;
            if (::unlinkat(Slots().Fd(), name.c_str(), 0) != 0 && errno != ENOENT)
                return Result<void>::Failure(Native::Failure(SaveErrors::StoragePermanentIo, "retired generation removal", errno));
            if (auto admitted = Before(SaveSlotLifecycleIoStage::DirectorySync, name); admitted.HasError())
                return admitted;
            if (::fsync(Slots().Fd()) != 0)
                return Result<void>::Failure(Native::Failure(SaveErrors::StoragePermanentIo, "retired generation synchronization", errno));
#endif
            return Result<void>::Success();
        }

        /** @brief Selects an immutable file under lexical ownership; the returned handle holds no mutex. */
        [[nodiscard]] Result<PinnedArchiveRead> PinRead(const std::string &name, const std::size_t maximumBytes) const {
            const std::lock_guard operationLock{operationMutex_};
            if (auto valid = Verify(); valid.HasError())
                return Result<PinnedArchiveRead>::Failure(valid.ErrorValue());
            return SaveFilesystemReadDetails::PinNamedArchiveRead(Slots(), name, maximumBytes);
        }

        /** @brief Materializes exact owned bytes after contained immutable-file selection. */
        [[nodiscard]] Result<std::vector<std::byte>> ReadBytes(const std::string &name, const std::size_t maximumBytes) const {
            auto pinned = PinRead(name, maximumBytes);
            if (pinned.HasError())
                return Result<std::vector<std::byte>>::Failure(pinned.ErrorValue());
            return std::move(pinned).Value().Read();
        }

        /** @brief Reads bounded fixed internal evidence while preserving explicit absence. */
        [[nodiscard]] Result<std::optional<std::vector<std::byte>>> ReadOptional(const std::string &name,
                                                                                 const std::size_t maximumBytes) const {
            auto exists = Exists(name);
            if (exists.HasError())
                return Result<std::optional<std::vector<std::byte>>>::Failure(exists.ErrorValue());
            if (!exists.Value())
                return Result<std::optional<std::vector<std::byte>>>::Success(std::nullopt);
            auto bytes = ReadBytes(name, maximumBytes);
            if (bytes.HasError())
                return Result<std::optional<std::vector<std::byte>>>::Failure(bytes.ErrorValue());
            return Result<std::optional<std::vector<std::byte>>>::Success(std::move(bytes).Value());
        }

        /** @brief Serializes complete publication while retaining namespace lifetime ownership. */
        [[nodiscard]] Result<void> Replace(const std::string &name, const std::span<const std::byte> bytes,
                                           const bool replaceExisting = true, const bool externalExport = false) const {
            const std::lock_guard operationLock{operationMutex_};
#ifdef _WIN32
            return ReplaceWindows(name, bytes, replaceExisting, externalExport);
#else
            return ReplacePosix(name, bytes, replaceExisting, externalExport);
#endif
        }
#ifdef _WIN32
        State(std::filesystem::path path, std::vector<Native::Handle> opened, std::vector<std::wstring> components)
            : rootPath(std::move(path)), directories(std::move(opened)), names(std::move(components)) {}

        [[nodiscard]] const Native::Handle &Slots() const noexcept {
            return directories.back();
        }

        [[nodiscard]] Result<void> Verify() const {
            Native::Handle current{::CreateFileW(rootPath.c_str(), FILE_READ_ATTRIBUTES,
                                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            if (!current.IsValid())
                return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "Windows root replacement"));
            BY_HANDLE_FILE_INFORMATION actual{}, held{};
            if (!::GetFileInformationByHandle(current.Get(), &actual) || !::GetFileInformationByHandle(directories.front().Get(), &held) ||
                actual.dwVolumeSerialNumber != held.dwVolumeSerialNumber || actual.nFileIndexHigh != held.nFileIndexHigh ||
                actual.nFileIndexLow != held.nFileIndexLow)
                return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "Windows root replacement"));
            FILE_ATTRIBUTE_TAG_INFO tag{};
            if (!::GetFileInformationByHandleEx(current.Get(), FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "Windows root redirection"));
            for (std::size_t index = 1; index < directories.size(); ++index) {
                if (auto same = Native::SameEntry(directories[index - 1], names[index - 1], directories[index], Native::kDirectoryFile);
                    same.HasError())
                    return same;
            }
            if (processLock.IsValid())
                return Native::SameEntry(Slots(), L".namespace.lock", processLock, Native::kNonDirectoryFile);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AcquireProcessLock() {
            auto opened = Native::RelativeOpen(Slots(), L".namespace.lock", GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                                               Native::kOpenIf, Native::kNonDirectoryFile, FILE_ATTRIBUTE_NORMAL);
            if (opened.HasError())
                return Result<void>::Failure(opened.ErrorValue());
            Native::Handle lock = std::move(opened).Value();
            OVERLAPPED offset{};
            if (!::LockFileEx(lock.Get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &offset)) {
                const DWORD error = ::GetLastError();
                return Result<void>::Failure(Native::Failure(error == ERROR_LOCK_VIOLATION ? SaveErrors::OperationInProgress
                                                                                           : SaveErrors::StorageCapabilityUnsupported,
                                                             "namespace lock", error));
            }
            processLock = std::move(lock);
            return Verify();
        }

    private:
        Native::Handle processLock;
        std::filesystem::path rootPath;
        std::vector<Native::Handle> directories;
        std::vector<std::wstring> names;

    public:
        [[nodiscard]] static Result<std::unique_ptr<State>> OpenWindows(const ProductSaveRoot &root, const SaveNamespaceId &name);
        [[nodiscard]] Result<void> ReplaceWindows(const std::string &name, std::span<const std::byte> bytes, bool replaceExisting,
                                                  bool externalExport) const;
#else
        State(std::filesystem::path path, std::vector<Native::Directory> opened, std::vector<std::string> components)
            : rootPath(std::move(path)), directories(std::move(opened)), names(std::move(components)) {}

        [[nodiscard]] const Native::Directory &Slots() const noexcept {
            return directories.back();
        }

        [[nodiscard]] Result<void> Verify() const {
            struct stat actual{};
            struct stat held{};
            if (::lstat(rootPath.c_str(), &actual) != 0 || ::fstat(directories.front().Fd(), &held) != 0 || !S_ISDIR(actual.st_mode) ||
                actual.st_dev != held.st_dev || actual.st_ino != held.st_ino)
                return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "root replacement"));
            for (std::size_t index = 1; index < directories.size(); ++index) {
                if (::fstatat(directories[index - 1].Fd(), names[index - 1].c_str(), &actual, AT_SYMLINK_NOFOLLOW) != 0 ||
                    ::fstat(directories[index].Fd(), &held) != 0 || !S_ISDIR(actual.st_mode) || actual.st_dev != held.st_dev ||
                    actual.st_ino != held.st_ino)
                    return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "namespace replacement"));
            }
            if (processLock.Fd() >= 0 &&
                (::fstatat(Slots().Fd(), ".namespace.lock", &actual, AT_SYMLINK_NOFOLLOW) != 0 || ::fstat(processLock.Fd(), &held) != 0 ||
                 !S_ISREG(actual.st_mode) || actual.st_nlink != 1 || actual.st_dev != held.st_dev || actual.st_ino != held.st_ino))
                return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "namespace lock replacement"));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AcquireProcessLock() {
            const int fd = ::openat(Slots().Fd(), ".namespace.lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
            if (fd < 0)
                return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "namespace lock admission", errno));
            Native::Directory lock{fd};
            if (struct stat info{}; ::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1)
                return Result<void>::Failure(Native::Failure(SaveErrors::SaveRootContainmentViolation, "unsafe namespace lock"));
            int result;
            do {
                result = ::flock(fd, LOCK_EX | LOCK_NB);
            } while (result != 0 && errno == EINTR);
            if (result != 0) {
                const int error = errno;
                return Result<void>::Failure(Native::Failure(error == EWOULDBLOCK || error == EAGAIN
                                                                 ? SaveErrors::OperationInProgress
                                                                 : SaveErrors::StorageCapabilityUnsupported,
                                                             "namespace lock", error));
            }
            processLock = std::move(lock);
            return Verify();
        }

    private:
        Native::Directory processLock;
        std::filesystem::path rootPath;
        std::vector<Native::Directory> directories;
        std::vector<std::string> names;

    public:
        [[nodiscard]] static Result<std::unique_ptr<State>> OpenPosix(const ProductSaveRoot &root, const SaveNamespaceId &name);
        [[nodiscard]] Result<void> ReplacePosix(const std::string &name, std::span<const std::byte> bytes, bool replaceExisting,
                                                bool externalExport) const;
#endif
    };

}  // namespace Horo::Runtime
