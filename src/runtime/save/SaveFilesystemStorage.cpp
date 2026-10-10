#include "Horo/Runtime/Save/SaveFilesystemStorage.h"

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveSlotLifecycle.h"
#include "SaveFilesystemStorageState.h"

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

#ifndef _WIN32
    namespace {
        /** @brief Retires the exclusive staging name on every failure and exception path. */
        struct PosixTemporaryRetirement final {
            int directory;
            const std::string &name;
            bool published{};

            ~PosixTemporaryRetirement() {
                if (!published)
                    ::unlinkat(directory, name.c_str(), 0);
            }
        };

        /** @brief Crosses the native replace/create-if-absent gate without weakening immutable-generation ownership. */
        [[nodiscard]] Result<void> SelectPosixFile(const Directory &directory, const std::string &temporary, const std::string &destination,
                                                   const bool replaceExisting) {
            // linkat is an atomic create-if-absent gate on Linux/macOS. Unlike renameat it cannot
            // overwrite a generation inserted after the initial absence check.
            const int selected = replaceExisting ? ::renameat(directory.Fd(), temporary.c_str(), directory.Fd(), destination.c_str())
                                                 : ::linkat(directory.Fd(), temporary.c_str(), directory.Fd(), destination.c_str(), 0);
            if (selected != 0)
                return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "atomic replacement", errno));
            if (!replaceExisting && ::unlinkat(directory.Fd(), temporary.c_str(), 0) != 0) {
                const int error = errno;
                return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "generation temporary retirement", error));
            }
            return Result<void>::Success();
        }
    }  // namespace
#endif

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
        return state_->ReadBytes(SlotName(slot), maximumBytes);
    }

#ifdef _WIN32
    Result<void> SaveFilesystemStorage::State::ReplaceWindows(const std::string &narrow, const std::span<const std::byte> bytes,
                                                              const bool replaceExisting, const bool externalExport) const {
        if (auto valid = Verify(); valid.HasError())
            return valid;
        auto publicationFailure = MakeError(narrow == ".lifecycle.catalog" || externalExport ? SaveErrors::SlotCommitOutcomeUnknown
                                                                                             : SaveErrors::StoragePermanentIo);
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
        if (auto admitted = Before(SaveSlotLifecycleIoStage::Write, narrow); admitted.HasError())
            return admitted;
        if (auto written = SaveFilesystemDetails::WriteWindowsBytes(file.Get(), bytes,
                                                                    [&] {
            return Before(SaveSlotLifecycleIoStage::WriteProgress, narrow);
        },
                                                                    [&] {
            return Before(SaveSlotLifecycleIoStage::FileSync, narrow);
        });
            written.HasError())
            return written;
        if (auto admitted = Before(SaveSlotLifecycleIoStage::Replace, narrow); admitted.HasError())
            return admitted;
        if (auto valid = Verify(); valid.HasError())
            return valid;
        if (auto safe = ExistingWindowsTargetSafe(Slots(), destination); safe.HasError())
            return safe;
        if (auto renamed = RenameWindowsFile(file.Get(), Slots().Get(), destination, replaceExisting); renamed.HasError())
            return renamed;
        cleanup.Published();
        return SynchronizePublication(narrow, std::move(publicationFailure), file.Get(), true, externalExport);
    }
#else
    Result<void> SaveFilesystemStorage::State::ReplacePosix(const std::string &destination, const std::span<const std::byte> bytes,
                                                            const bool replaceExisting, const bool externalExport) const {
        if (auto valid = Verify(); valid.HasError())
            return valid;
        auto publicationFailure = MakeError(destination == ".lifecycle.catalog" || externalExport ? SaveErrors::SlotCommitOutcomeUnknown
                                                                                                  : SaveErrors::StoragePermanentIo);
        if (auto safe = ExistingTargetSafe(Slots(), destination); safe.HasError())
            return safe;
        static std::atomic_uint64_t sequence{0};
        const std::string temporary =
            std::format(".{}.{}.{}.temporary", destination, ::getpid(), sequence.fetch_add(1, std::memory_order_relaxed));
        const int fd = ::openat(Slots().Fd(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return Result<void>::Failure(Failure(SaveErrors::StoragePermanentIo, "temporary creation", errno));
        Directory file{fd};

        PosixTemporaryRetirement cleanup{Slots().Fd(), temporary};
        if (auto admitted = Before(SaveSlotLifecycleIoStage::Write, destination); admitted.HasError()) {
            return admitted;
        }
        if (auto written = WritePosixBytes(fd, bytes,
                                           [&] {
            return Before(SaveSlotLifecycleIoStage::WriteProgress, destination);
        },
                                           [&] {
            return Before(SaveSlotLifecycleIoStage::FileSync, destination);
        });
            written.HasError()) {
            return written;
        }
        if (auto admitted = Before(SaveSlotLifecycleIoStage::Replace, destination); admitted.HasError()) {
            return admitted;
        }
        if (auto valid = Verify(); valid.HasError()) {
            return valid;
        }
        if (auto safe = ExistingTargetSafe(Slots(), destination); safe.HasError()) {
            return safe;
        }
        if (auto selected = SelectPosixFile(Slots(), temporary, destination, replaceExisting); selected.HasError())
            return selected;
        cleanup.published = true;
        return SynchronizePublication(destination, std::move(publicationFailure), Slots().Fd(), true, externalExport);
    }
#endif

    /** @copydoc SaveFilesystemStorage::Replace */
    Result<void> SaveFilesystemStorage::Replace(const SaveGameSlotId slot, const std::span<const std::byte> bytes) const {
        return ReplaceSlot(slot, bytes, false);
    }

    /** @copydoc SaveFilesystemStorage::ReplaceLifecycleExport */
    Result<void> SaveFilesystemStorage::ReplaceLifecycleExport(const SaveGameSlotId slot, const std::span<const std::byte> bytes) const {
        return ReplaceSlot(slot, bytes, true);
    }

    /** @copydoc SaveFilesystemStorage::ReplaceSlot */
    Result<void> SaveFilesystemStorage::ReplaceSlot(const SaveGameSlotId slot, const std::span<const std::byte> bytes,
                                                    const bool externalExport) const {
        if (!state_ || !slot.IsValid() || bytes.empty())
            return Result<void>::Failure(Failure(SaveErrors::StorageOperationInvalid, "replacement validation"));
        return state_->Replace(SlotName(slot), bytes, true, externalExport);
    }

    namespace {
        /** @brief Keeps lifecycle generations disjoint from logical slots and temporary records. */
        [[nodiscard]] std::string GenerationName(const SlotGenerationId generation) {
            return ".generation." + generation.ToString() + ".horosave";
        }

        constexpr const char *kLifecycleCatalog = ".lifecycle.catalog";
    }  // namespace

    /** @copydoc SaveFilesystemStorage::ReadLifecycleCatalog */
    Result<std::optional<std::vector<std::byte>>> SaveFilesystemStorage::ReadLifecycleCatalog(const std::size_t maximumBytes) const {
        return state_->ReadOptional(kLifecycleCatalog, maximumBytes);
    }

    /** @copydoc SaveFilesystemStorage::ReplaceLifecycleCatalog */
    Result<void> SaveFilesystemStorage::ReplaceLifecycleCatalog(const std::span<const std::byte> bytes) const {
        return state_->Replace(kLifecycleCatalog, bytes);
    }

    /** @copydoc SaveFilesystemStorage::SynchronizeLifecycleCatalog */
    Result<void> SaveFilesystemStorage::SynchronizeLifecycleCatalog() const {
        return state_->SynchronizeCatalog();
    }

    /** @copydoc SaveFilesystemStorage::ReadLifecycleGeneration */
    Result<std::vector<std::byte>> SaveFilesystemStorage::ReadLifecycleGeneration(const SlotGenerationId generation,
                                                                                  const std::size_t maximumBytes) const {
        return state_->ReadBytes(GenerationName(generation), maximumBytes);
    }

    /** @copydoc SaveFilesystemStorage::WriteLifecycleGeneration */
    Result<void> SaveFilesystemStorage::WriteLifecycleGeneration(const SlotGenerationId generation,
                                                                 const std::span<const std::byte> bytes) const {
        if (auto absent = VerifyLifecycleGenerationAbsent(generation); absent.HasError())
            return absent;
        return state_->Replace(GenerationName(generation), bytes, false);
    }

    /** @copydoc SaveFilesystemStorage::VerifyLifecycleGenerationAbsent */
    Result<void> SaveFilesystemStorage::VerifyLifecycleGenerationAbsent(const SlotGenerationId generation) const {
        auto exists = state_->Exists(GenerationName(generation));
        if (exists.HasError())
            return Result<void>::Failure(exists.ErrorValue());
        if (exists.Value())
            return Result<void>::Failure(MakeError(SaveErrors::SlotGenerationConflict));
        return Result<void>::Success();
    }

    /** @copydoc SaveFilesystemStorage::RemoveLifecycleGeneration */
    Result<void> SaveFilesystemStorage::RemoveLifecycleGeneration(const SlotGenerationId generation) const {
        return state_->Remove(GenerationName(generation));
    }

    /** @copydoc SaveFilesystemStorage::ReadLifecycleJournal */
    Result<std::optional<std::vector<std::byte>>> SaveFilesystemStorage::ReadLifecycleJournal() const {
        return state_->ReadOptional(".lifecycle.journal", 146);
    }

    /** @copydoc SaveFilesystemStorage::ReplaceLifecycleJournal */
    Result<void> SaveFilesystemStorage::ReplaceLifecycleJournal(const std::span<const std::byte> bytes) const {
        return state_->Replace(".lifecycle.journal", bytes);
    }

    /** @copydoc SaveFilesystemStorage::RemoveLifecycleJournal */
    Result<void> SaveFilesystemStorage::RemoveLifecycleJournal() const {
        return state_->Remove(".lifecycle.journal");
    }

    /** @copydoc SaveFilesystemStorage::SetLifecycleIoObserver */
    void SaveFilesystemStorage::SetLifecycleIoObserver(ISaveSlotLifecycleIoObserver *observer) noexcept {
        state_->SetObserver(observer);
    }
}  // namespace Horo::Runtime
