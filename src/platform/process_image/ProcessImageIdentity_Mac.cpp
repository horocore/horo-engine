#include "ProcessImageIdentity.h"

#include <Security/Security.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
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
                                                  "Use a supported unchanged native process image with valid OS code identity.",
                                              .retryable = false,
                                              .userActionable = true};
        constexpr std::uint64_t MaximumImageBytes = 512ULL * 1024 * 1024;

        template <class T> struct CfOwner final {
            T value{};
            CfOwner() = default;
            CfOwner(const CfOwner &) = delete;
            CfOwner &operator=(const CfOwner &) = delete;

            ~CfOwner() {
                if (value)
                    CFRelease(value);
            }
        };

        struct NativeState final {
            CfOwner<SecCodeRef> code;
            std::array<std::uint8_t, 64> unique{};
            CFIndex uniqueBytes{};
            std::array<UInt8, PATH_MAX> path{};
            int file{-1};
            struct stat metadata{};

            ~NativeState() {
                if (file >= 0)
                    close(file);
            }
        };

        template <class T> Result<T> Failure(const char *message) {
            return Result<T>::Failure(MakeError(Unavailable, message));
        }

        /** @brief Requires unchanged inode, size and write/change epochs; path equality alone is insufficient. */
        bool SameFile(const struct stat &a, const struct stat &b) noexcept {
            return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
                   a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec &&
                   a.st_ctimespec.tv_sec == b.st_ctimespec.tv_sec && a.st_ctimespec.tv_nsec == b.st_ctimespec.tv_nsec;
        }

        /** @brief Binds the retained descriptor to the current OS-reported executable origin. */
        Result<void> CheckFile(const NativeState &state) {
            struct stat opened{}, named{};
            if (fstat(state.file, &opened) != 0 || stat(reinterpret_cast<const char *>(state.path.data()), &named) != 0)
                return Failure<void>("The retained executable file is no longer available.");
            if (!SameFile(state.metadata, opened) || !SameFile(opened, named))
                return Failure<void>("Executable file identity changed during process evidence lifetime.");
            return Result<void>::Success();
        }

        /** @brief Copies only bounded typed identity/origin evidence from successful native validation. */
        Result<void> ReadIdentity(NativeState &state, CFDictionaryRef info) {
            const auto unique = static_cast<CFDataRef>(CFDictionaryGetValue(info, kSecCodeInfoUnique));
            const auto url = static_cast<CFURLRef>(CFDictionaryGetValue(info, kSecCodeInfoMainExecutable));
            if (!unique || !url || CFGetTypeID(unique) != CFDataGetTypeID() || CFGetTypeID(url) != CFURLGetTypeID())
                return Failure<void>("OS did not provide signed executable identity and origin.");
            state.uniqueBytes = CFDataGetLength(unique);
            if (state.uniqueBytes <= 0 || state.uniqueBytes > static_cast<CFIndex>(state.unique.size()))
                return Failure<void>("OS executable identity exceeds the supported evidence bound.");
            CFDataGetBytes(unique, CFRangeMake(0, state.uniqueBytes), state.unique.data());
            if (!CFURLGetFileSystemRepresentation(url, true, state.path.data(), state.path.size()))
                return Failure<void>("OS executable origin exceeds the supported path bound.");
            return Result<void>::Success();
        }

        /** @brief Validates the actual calling process before consulting its filesystem origin. */
        Result<void> CaptureIdentity(NativeState &state) {
            if (SecCodeCopySelf(kSecCSDefaultFlags, &state.code.value) != errSecSuccess ||
                SecCodeCheckValidity(state.code.value, kSecCSDefaultFlags, nullptr) != errSecSuccess)
                return Failure<void>("OS dynamic validation of the actual calling process failed.");
            CfOwner<CFDictionaryRef> info;
            if (SecCodeCopySigningInformation(reinterpret_cast<SecStaticCodeRef>(state.code.value),
                                              kSecCSSigningInformation | kSecCSDynamicInformation, &info.value) != errSecSuccess)
                return Failure<void>("OS executable signing information is unavailable.");
            return ReadIdentity(state, info.value);
        }

        /** @brief Rejects changed code or file evidence before consumers can bind this identity. */
        Result<void> VerifyNative(const NativeState &state) {
            auto file = CheckFile(state);
            if (file.HasError())
                return file;
            if (SecCodeCheckValidity(state.code.value, kSecCSDefaultFlags, nullptr) != errSecSuccess)
                return Failure<void>("OS dynamic process code identity is no longer valid.");
            CfOwner<CFDictionaryRef> info;
            if (SecCodeCopySigningInformation(reinterpret_cast<SecStaticCodeRef>(state.code.value),
                                              kSecCSSigningInformation | kSecCSDynamicInformation, &info.value) != errSecSuccess)
                return Failure<void>("OS process signing information changed.");
            const auto value = static_cast<CFDataRef>(CFDictionaryGetValue(info.value, kSecCodeInfoUnique));
            if (!value || CFGetTypeID(value) != CFDataGetTypeID() || CFDataGetLength(value) != state.uniqueBytes)
                return Failure<void>("OS process code identifier changed.");
            const auto bytes = CFDataGetBytePtr(value);
            if (!std::equal(state.unique.begin(), state.unique.begin() + state.uniqueBytes, bytes))
                return Failure<void>("OS process code identifier no longer matches captured evidence.");
            return CheckFile(state);
        }

        /** @brief Pins the verified executable inode for the lifetime of derived native resource leases. */
        Result<void> OpenImage(NativeState &state) {
            state.file = open(reinterpret_cast<const char *>(state.path.data()), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (state.file < 0 || fstat(state.file, &state.metadata) != 0)
                return Failure<void>("Cannot retain actual executable file evidence.");
            if (!S_ISREG(state.metadata.st_mode) || state.metadata.st_size <= 0 ||
                static_cast<std::uint64_t>(state.metadata.st_size) > MaximumImageBytes)
                return Failure<void>("Executable file is invalid or exceeds the bounded image size.");
            return CheckFile(state);
        }

        /** @brief Streams the complete bounded image without mapping or loading another code module. */
        Result<Sha256Digest> HashImage(const NativeState &state) {
            Sha256Builder builder;
            std::array<std::byte, 65536> buffer{};
            off_t offset{};
            std::uint32_t interruptions{};
            while (offset < state.metadata.st_size) {
                const auto remaining = static_cast<std::uint64_t>(state.metadata.st_size - offset);
                const auto count = pread(state.file, buffer.data(), std::min<std::uint64_t>(buffer.size(), remaining), offset);
                if (count < 0 && errno == EINTR && ++interruptions <= 64)
                    continue;
                if (count <= 0)
                    return Failure<Sha256Digest>("Executable file changed or could not be completely read.");
                if (!builder.Update(std::span(buffer).first(static_cast<std::size_t>(count))))
                    return Failure<Sha256Digest>("Executable identity hash length exceeded its bound.");
                offset += count;
            }
            return Result<Sha256Digest>::Success(builder.Finalize());
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
        Dl_info image{};
        if (!address || dladdr(address, &image) == 0 || image.dli_fbase != _dyld_get_image_header(0))
            return Failure<void>("Participating code is not in the verified main executable image.");
        return Verify();
    }

    /** @copydoc CaptureCurrentProcessImageIdentity */
    Result<ProcessImageIdentity> CaptureCurrentProcessImageIdentity() {
        try {
            auto state = std::make_shared<NativeState>();
            auto captured = CaptureIdentity(*state);
            if (captured.HasError())
                return Result<ProcessImageIdentity>::Failure(captured.ErrorValue());
            auto opened = OpenImage(*state);
            if (opened.HasError())
                return Result<ProcessImageIdentity>::Failure(opened.ErrorValue());
            auto hash = HashImage(*state);
            if (hash.HasError())
                return Result<ProcessImageIdentity>::Failure(hash.ErrorValue());
            auto verified = VerifyNative(*state);
            if (verified.HasError())
                return Result<ProcessImageIdentity>::Failure(verified.ErrorValue());
            return Result<ProcessImageIdentity>::Success(ProcessImageIdentity{hash.Value(), std::move(state)});
        } catch (const std::bad_alloc &) {
            return Failure<ProcessImageIdentity>("Process image evidence allocation failed.");
        }
    }
}  // namespace Horo::Platform
