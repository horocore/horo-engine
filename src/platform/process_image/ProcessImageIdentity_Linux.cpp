#include "ProcessImageIdentity.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <link.h>
#include <new>
#include <sys/stat.h>
#include <unistd.h>

namespace Horo::Platform {
    namespace {
        const ErrorCodeDescriptor Unavailable{.domain = ErrorDomainId{"horo.platform.process_image"},
                                              .code = ErrorCode{"platform.process_image.unavailable"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Current process image identity is unavailable.",
                                              .remediationHint =
                                                  "Provide the actual kernel executable image and its unchanged readable inode.",
                                              .retryable = false,
                                              .userActionable = true};
        constexpr std::uint64_t MaximumImageBytes = 512ULL * 1024 * 1024;

        struct FileOwner final {
            int value{-1};
            FileOwner() = default;
            FileOwner(const FileOwner &) = delete;
            FileOwner &operator=(const FileOwner &) = delete;

            ~FileOwner() {
                if (value >= 0)
                    close(value);
            }
        };

        struct NativeState final {
            FileOwner file;
            struct stat metadata{};
        };

        template <class T> Result<T> Failure(const char *message) {
            return Result<T>::Failure(MakeError(Unavailable, message));
        }

        /** @brief Rename may change ctime without changing the actual running executable; its kernel inode is authoritative. */
        bool SameExecutable(const struct stat &a, const struct stat &b) noexcept {
            return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size && a.st_mtim.tv_sec == b.st_mtim.tv_sec &&
                   a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
        }

        /** @brief Reopens the kernel's actual executable reference, never a caller or installation pathname. */
        Result<void> VerifyNative(const NativeState &state) {
            FileOwner current;
            current.value = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
            struct stat opened{};
            struct stat running{};
            if (current.value < 0 || fstat(current.value, &running) != 0 || fstat(state.file.value, &opened) != 0)
                return Failure<void>("Cannot verify the kernel executable image reference.");
            if (!SameExecutable(state.metadata, opened) || !SameExecutable(opened, running))
                return Failure<void>("Actual process executable inode or content metadata changed.");
            return Result<void>::Success();
        }

        /** @brief Pins the kernel-backed executable inode and admits its complete bounded byte range. */
        Result<void> OpenImage(NativeState &state) {
            state.file.value = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
            if (state.file.value < 0 || fstat(state.file.value, &state.metadata) != 0)
                return Failure<void>("Cannot retain actual executable file evidence.");
            if (!S_ISREG(state.metadata.st_mode) || state.metadata.st_size <= 0 ||
                static_cast<std::uint64_t>(state.metadata.st_size) > MaximumImageBytes)
                return Failure<void>("Executable image is invalid or exceeds its admitted size.");
            return VerifyNative(state);
        }

        /** @brief Streams actual executable bytes under finite byte and interruption budgets. */
        Result<Sha256Digest> HashImage(const NativeState &state) {
            Sha256Builder builder;
            std::array<std::byte, 65536> buffer{};
            off_t offset{};
            std::uint32_t interruptions{};
            while (offset < state.metadata.st_size) {
                const auto remaining = static_cast<std::uint64_t>(state.metadata.st_size - offset);
                const auto count = pread(state.file.value, buffer.data(), std::min<std::uint64_t>(buffer.size(), remaining), offset);
                if (count < 0 && errno == EINTR) {
                    ++interruptions;
                    if (interruptions <= 64)
                        continue;
                }
                if (count <= 0)
                    return Failure<Sha256Digest>("Cannot read the complete executable within the admitted I/O budget.");
                if (!builder.Update(std::span(buffer).first(static_cast<std::size_t>(count))))
                    return Failure<Sha256Digest>("Executable identity hash length exceeded its bound.");
                offset += count;
            }
            return Result<Sha256Digest>::Success(builder.Finalize());
        }

        struct AddressProbe final {
            std::uintptr_t address{};
            bool found{};
        };

        /** @brief Finds executable LOAD ranges only in the actual main program, excluding every shared object. */
        int InspectMain(struct dl_phdr_info *image, std::size_t, void *opaque) {
            if (image->dlpi_name && image->dlpi_name[0] != '\0')
                return 0;
            auto &probe = *static_cast<AddressProbe *>(opaque);
            const auto base = static_cast<std::uintptr_t>(image->dlpi_addr);
            for (std::size_t index = 0; index < image->dlpi_phnum; ++index) {
                const auto &segment = image->dlpi_phdr[index];
                if (segment.p_type != PT_LOAD || (segment.p_flags & PF_X) == 0)
                    continue;
                const auto start = base + segment.p_vaddr;
                if (start >= base && probe.address >= start && probe.address - start < segment.p_memsz)
                    probe.found = true;
            }
            return 1;
        }
    }  // namespace

    /** @copydoc ProcessImageIdentity::Verify */
    Result<void> ProcessImageIdentity::Verify() const {
        if (!state_)
            return Failure<void>("Process image evidence has no native owner.");
        return VerifyNative(*static_cast<const NativeState *>(state_.get()));
    }

    /** @copydoc ProcessImageIdentity::VerifyStaticCodeAddress */
    Result<void> ProcessImageIdentity::VerifyStaticCodeAddress(const void *address) const {
        AddressProbe probe{reinterpret_cast<std::uintptr_t>(address)};
        dl_iterate_phdr(InspectMain, &probe);
        if (!address || !probe.found)
            return Failure<void>("Participating code is not in the actual executable LOAD image.");
        return Verify();
    }

    /** @copydoc CaptureCurrentProcessImageIdentity */
    Result<ProcessImageIdentity> CaptureCurrentProcessImageIdentity() {
        try {
            auto state = std::make_shared<NativeState>();
            if (auto opened = OpenImage(*state); opened.HasError())
                return Result<ProcessImageIdentity>::Failure(opened.ErrorValue());
            auto hash = HashImage(*state);
            if (hash.HasError())
                return Result<ProcessImageIdentity>::Failure(hash.ErrorValue());
            if (auto verified = VerifyNative(*state); verified.HasError())
                return Result<ProcessImageIdentity>::Failure(verified.ErrorValue());
            return Result<ProcessImageIdentity>::Success(ProcessImageIdentity{hash.Value(), std::move(state)});
        } catch (const std::bad_alloc &) {
            return Failure<ProcessImageIdentity>("Process image evidence allocation failed.");
        }
    }
}  // namespace Horo::Platform
