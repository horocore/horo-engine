#pragma once
/** @file SaveFilesystemPinnedRead.h
 * @brief Non-installed internal boundary for immutable native file selection and byte reads.
 */
#include "SaveFilesystemStoragePlatform.h"

namespace Horo::Runtime {
    namespace SaveFilesystemReadDetails {
        using SaveFilesystemDetails::Failure;
        using SaveFilesystemDetails::SlotName;
#ifdef _WIN32
        using ArchiveFile = SaveFilesystemNative::Handle;
#else
        using ArchiveFile = SaveFilesystemNative::Directory;
#endif
        /** @brief Owns one immutable selected file; byte I/O requires no namespace mutex.
         * Replacement/deletion and directory-owner destruction preserve the held handle.
         * Cooperating writers must never mutate published files in place.
         */
        class PinnedArchiveRead final {
        public:
            PinnedArchiveRead(ArchiveFile file, std::size_t byteLength) : file_(std::move(file)), byteLength_(byteLength) {}

            /** @brief Reads the selected file once; callers must not share or reuse the moved capability. */
            [[nodiscard]] Result<std::vector<std::byte>> Read() && {
                ArchiveFile file{std::move(file_)};
                std::vector<std::byte> bytes(byteLength_);
                std::size_t offset = 0;
                while (offset < bytes.size()) {
#ifdef _WIN32
                    const DWORD amount =
                        static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, std::numeric_limits<DWORD>::max()));
                    DWORD received{};
                    if (!::ReadFile(file.Get(), bytes.data() + offset, amount, &received, nullptr) || received == 0)
                        return Result<std::vector<std::byte>>::Failure(
                            Failure(SaveErrors::StoragePermanentIo, "Windows slot read", ::GetLastError()));
                    offset += received;
#else
                    const std::size_t remaining =
                        std::min(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
                    // EINTR retries unchanged; EOF/errors return; a positive read advances by at most remaining.
                    // flawfinder: ignore - offset < bytes.size() and remaining <= bytes.size() - offset.
                    const ssize_t read = ::read(file.Fd(), bytes.data() + offset, remaining);
                    if (read < 0 && errno == EINTR)
                        continue;
                    if (read <= 0)
                        return Result<std::vector<std::byte>>::Failure(
                            Failure(SaveErrors::StoragePermanentIo, "slot read", read < 0 ? errno : 0));
                    offset += static_cast<std::size_t>(read);
#endif
                }
                return Result<std::vector<std::byte>>::Success(std::move(bytes));
            }

        private:
            ArchiveFile file_;
            std::size_t byteLength_;
        };

        /** @brief Selects and validates a file while the caller holds namespace operation ownership. */
        [[nodiscard]] inline Result<PinnedArchiveRead> PinArchiveRead(const ArchiveFile &directory, SaveGameSlotId slot,
                                                                      std::size_t maximumBytes) {
#ifdef _WIN32
            const std::string narrow = SlotName(slot);
            const std::wstring name(narrow.begin(), narrow.end());
            auto opened = SaveFilesystemNative::RelativeOpen(directory, name, GENERIC_READ, SaveFilesystemNative::kOpen,
                                                             SaveFilesystemNative::kNonDirectoryFile, FILE_ATTRIBUTE_NORMAL);
            if (opened.HasError())
                return Result<PinnedArchiveRead>::Failure(opened.ErrorValue());
            LARGE_INTEGER size{};
            if (!::GetFileSizeEx(opened.Value().Get(), &size) || size.QuadPart < 0 ||
                static_cast<std::uintmax_t>(size.QuadPart) > maximumBytes)
                return Result<PinnedArchiveRead>::Failure(Failure(SaveErrors::StorageOperationInvalid, "Windows slot size"));
            return Result<PinnedArchiveRead>::Success(
                PinnedArchiveRead{std::move(opened).Value(), static_cast<std::size_t>(size.QuadPart)});
#else
            const std::string name = SlotName(slot);
            const int fd = ::openat(directory.Fd(), name.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            if (fd < 0)
                return Result<PinnedArchiveRead>::Failure(Failure(SaveErrors::StoragePermanentIo, "slot open", errno));
            ArchiveFile file{fd};
            struct stat entry{};
            if (::fstat(fd, &entry) != 0)
                return Result<PinnedArchiveRead>::Failure(Failure(SaveErrors::StoragePermanentIo, "slot inspection", errno));
            if (!S_ISREG(entry.st_mode) || entry.st_nlink != 1)
                return Result<PinnedArchiveRead>::Failure(Failure(SaveErrors::SaveRootContainmentViolation, "unsafe slot"));
            if (entry.st_size < 0 || static_cast<std::uintmax_t>(entry.st_size) > maximumBytes)
                return Result<PinnedArchiveRead>::Failure(Failure(SaveErrors::StorageOperationInvalid, "slot size"));
            return Result<PinnedArchiveRead>::Success(PinnedArchiveRead{std::move(file), static_cast<std::size_t>(entry.st_size)});
#endif
        }
    }  // namespace SaveFilesystemReadDetails
}  // namespace Horo::Runtime
