#include "AndroidApkOpenXRLoader.h"

#include "Horo/Platform/PlatformErrors.h"

#include <algorithm>
#include <android/dlext.h>
#include <array>
#include <cerrno>
#include <dlfcn.h>
#include <fcntl.h>
#include <miniz.h>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace Horo::Platform::Android {
    namespace {
        using LoaderResult = Result<std::shared_ptr<const DynamicLibrary>>;
        constexpr std::uint64_t MaximumApkBytes = 256U * 1024U * 1024U;
        constexpr std::uint64_t MaximumLoaderBytes = 8U * 1024U * 1024U;
        constexpr std::uint64_t NativeAlignment = 16U * 1024U;
#if defined(__aarch64__)
        constexpr std::string_view LoaderMember{"lib/arm64-v8a/libopenxr_loader.so"};
#elif defined(__x86_64__)
        constexpr std::string_view LoaderMember{"lib/x86_64/libopenxr_loader.so"};
#else
#error The explicit Android OpenXR package has no loader pin for this ABI.
#endif

        /** @brief Short-lived owner of the same installed APK descriptor used by the ZIP reader and Android linker. */
        struct ApkFile final {
            explicit ApkFile(const char *path) noexcept : descriptor{open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)} {}

            ~ApkFile() {
                if (descriptor >= 0)
                    close(descriptor);
            }

            ApkFile(const ApkFile &) = delete;
            ApkFile &operator=(const ApkFile &) = delete;
            const int descriptor;
        };

        /** @brief Reads a complete bounded range without changing the descriptor's position. */
        bool ReadExact(int descriptor, std::uint64_t offset, std::span<std::byte> buffer) noexcept {
            while (!buffer.empty()) {
                const auto count = pread(descriptor, buffer.data(), buffer.size(), static_cast<off_t>(offset));
                if (count < 0 && errno == EINTR)
                    continue;
                if (count <= 0)
                    return false;
                offset += static_cast<std::uint64_t>(count);
                buffer = buffer.subspan(static_cast<std::size_t>(count));
            }
            return true;
        }

        /** @brief Adapts the retained base APK descriptor to the pinned ZIP parser. */
        std::size_t ReadZip(void *source, mz_uint64 offset, void *output, std::size_t size) noexcept {
            if (offset > MaximumApkBytes || size > MaximumApkBytes - offset)
                return 0U;
            const auto &file = *static_cast<const ApkFile *>(source);
            return ReadExact(file.descriptor, offset, {static_cast<std::byte *>(output), size}) ? size : 0U;
        }

        /** @brief Owns only the parser state. The caller's ApkFile outlives parsing and the final native load. */
        struct ZipReader final {
            explicit ZipReader(ApkFile &file, std::uint64_t size) noexcept {
                archive.m_pRead = ReadZip;
                archive.m_pIO_opaque = &file;
                ready = mz_zip_reader_init(&archive, size, MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY);
            }

            ~ZipReader() {
                if (ready)
                    mz_zip_reader_end(&archive);
            }

            ZipReader(const ZipReader &) = delete;
            ZipReader &operator=(const ZipReader &) = delete;
            mz_zip_archive archive{};
            bool ready{};
        };

        /** @brief Selects exactly one case-sensitive native library member and validates its ZIP headers. */
        std::optional<mz_zip_archive_file_stat> ReadLoaderEntry(mz_zip_archive &archive) {
            const auto count = mz_zip_reader_get_num_files(&archive);
            if (!count || count > 65536U)
                return {};
            std::optional<mz_uint> found;
            for (mz_uint index = 0; index < count; ++index) {
                std::array<char, LoaderMember.size() + 1U> name{};
                if (mz_zip_reader_get_filename(&archive, index, nullptr, 0U) != name.size() ||
                    mz_zip_reader_get_filename(&archive, index, name.data(), name.size()) != name.size() ||
                    std::string_view{name.data(), LoaderMember.size()} != LoaderMember)
                    continue;
                if (found)
                    return {};
                found = index;
            }
            mz_zip_archive_file_stat entry{};
            if (!found || !mz_zip_reader_file_stat(&archive, *found, &entry) || !entry.m_is_supported || entry.m_is_directory ||
                entry.m_is_encrypted || entry.m_method != 0U || !entry.m_uncomp_size || entry.m_uncomp_size > MaximumLoaderBytes ||
                entry.m_comp_size != entry.m_uncomp_size || !mz_zip_validate_file(&archive, *found, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY))
                return {};
            return entry;
        }

        /** @brief Gets the stored data offset from the validated local header and checks its complete range. */
        std::optional<std::uint64_t> LoaderOffset(const ApkFile &apk, const mz_zip_archive &archive,
                                                  const mz_zip_archive_file_stat &entry) noexcept {
            constexpr std::size_t FlagsOffset = 6U;
            constexpr std::size_t MethodOffset = 8U;
            constexpr std::size_t NameLengthOffset = 26U;
            constexpr std::size_t ExtraLengthOffset = 28U;
            constexpr std::size_t FixedHeaderSize = 30U;
            std::array<std::byte, FixedHeaderSize> local{};
            const auto boundary = archive.m_central_directory_file_ofs;
            const auto header = entry.m_local_header_ofs;
            if (header > boundary || boundary - header < local.size() || !ReadExact(apk.descriptor, header, local))
                return {};
            const auto word = [&local](std::size_t at) noexcept {
                return std::to_integer<unsigned>(local[at]) | (std::to_integer<unsigned>(local[at + 1U]) << 8U);
            };
            const auto nameLength = word(NameLengthOffset);
            const auto offset = header + local.size() + nameLength + word(ExtraLengthOffset);
            if (word(FlagsOffset) != entry.m_bit_flag || word(MethodOffset) != 0U || nameLength != LoaderMember.size() ||
                offset % NativeAlignment || offset > boundary || entry.m_uncomp_size > boundary - offset)
                return {};
            return offset;
        }

        /** @brief Hashes the precise stored data range later given to the dynamic linker. */
        bool MatchesDigest(const ApkFile &apk, std::uint64_t offset, std::uint64_t size, const Sha256Digest &expected) noexcept {
            Sha256Builder hash;
            std::array<std::byte, NativeAlignment> storage{};
            while (size) {
                const auto bytes = std::span{storage}.first(static_cast<std::size_t>(std::min<std::uint64_t>(size, storage.size())));
                if (!ReadExact(apk.descriptor, offset, bytes) || !hash.Update(bytes))
                    return false;
                offset += bytes.size();
                size -= bytes.size();
            }
            return hash.Finalize() == expected;
        }

        /** @brief Rejects an installed file that changed between index verification and native loading. */
        bool SameFileVersion(const ApkFile &apk, const struct stat &initial) noexcept {
            struct stat current{};
            return fstat(apk.descriptor, &current) == 0 && current.st_dev == initial.st_dev && current.st_ino == initial.st_ino &&
                   current.st_size == initial.st_size && current.st_mtim.tv_sec == initial.st_mtim.tv_sec &&
                   current.st_mtim.tv_nsec == initial.st_mtim.tv_nsec && current.st_ctim.tv_sec == initial.st_ctim.tv_sec &&
                   current.st_ctim.tv_nsec == initial.st_ctim.tv_nsec;
        }

        /** @brief Clears only failures raised inside the frame, then releases the two bounded JNI locals. */
        struct JniLocalFrame final {
            explicit JniLocalFrame(JNIEnv &environment) noexcept : env{environment} {}

            ~JniLocalFrame() {
                if (env.ExceptionCheck())
                    env.ExceptionClear();
                env.PopLocalFrame(nullptr);
            }

            JniLocalFrame(const JniLocalFrame &) = delete;
            JniLocalFrame &operator=(const JniLocalFrame &) = delete;
            JNIEnv &env;
        };

        /** @brief Reads the installed primary APK path from the retained Android application Context. */
        bool ReadApkPath(const GameActivityJniApplication &application, std::span<char> path) noexcept {
            const GameActivityJniAttachment attachment{application.vm};
            auto *env = attachment.Environment();
            if (!env || !application.applicationContext || env->ExceptionCheck())
                return false;
            if (env->PushLocalFrame(2) != JNI_OK) {
                if (env->ExceptionCheck())
                    env->ExceptionClear();
                return false;
            }
            const JniLocalFrame frame{*env};
            const auto context = application.applicationContext.get();
            const auto type = env->GetObjectClass(context);
            if (!type || env->ExceptionCheck())
                return false;
            const auto method = env->GetMethodID(type, "getPackageCodePath", "()Ljava/lang/String;");
            if (!method || env->ExceptionCheck())
                return false;
            const auto value = static_cast<jstring>(env->CallObjectMethod(context, method));
            if (!value || env->ExceptionCheck())
                return false;
            const auto length = env->GetStringUTFLength(value);
            if (length <= 0 || static_cast<std::size_t>(length) >= path.size())
                return false;
            env->GetStringUTFRegion(value, 0, env->GetStringLength(value), path.data());
            const std::string_view codePath{path.data(), static_cast<std::size_t>(length)};
            return !env->ExceptionCheck() && codePath.starts_with('/') && codePath.ends_with(".apk") &&
                   codePath.find('\0') == std::string_view::npos && codePath.find("!/") == std::string_view::npos;
        }

        /** @brief Ensures native code unloads before its borrowed Kotlin/Java Context can be released. */
        struct CloseLibrary final {
            void operator()(void *value) const noexcept {
                dlclose(value);
            }
        };

        using LibraryHandle = std::unique_ptr<void, CloseLibrary>;

        class AndroidApkDynamicLibrary final : public DynamicLibrary {
        public:
            AndroidApkDynamicLibrary(GameActivityGlobalRef context, LibraryHandle handle) noexcept
                : context_(std::move(context)), handle_(std::move(handle)) {}

            [[nodiscard]] void *GetSymbol(std::string_view name) const noexcept override {
                std::array<char, 256> terminated{};
                if (name.empty() || name.size() >= terminated.size() || name.find('\0') != std::string_view::npos)
                    return nullptr;
                std::ranges::copy(name, terminated.begin());
                return dlsym(handle_.get(), terminated.data());
            }

        private:
            GameActivityGlobalRef context_;
            LibraryHandle handle_;
        };
    }  // namespace

    /** @copydoc LoadBundledOpenXRLoader */
    Result<std::shared_ptr<const DynamicLibrary>> LoadBundledOpenXRLoader(const GameActivityJniApplication &application,
                                                                          const Sha256Digest &imageDigest) {
        std::array<char, 4096> path{};
        if (!ReadApkPath(application, path))
            return LoaderResult::Failure(MakeError(PlatformErrors::NotFound, "Installed application APK is unavailable"));
        ApkFile apk{path.data()};
        struct stat metadata{};
        if (apk.descriptor < 0 || fstat(apk.descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size <= 0 ||
            static_cast<std::uint64_t>(metadata.st_size) > MaximumApkBytes)
            return LoaderResult::Failure(MakeError(PlatformErrors::InvalidFormat, "Installed application APK range is invalid"));
        ZipReader zip{apk, static_cast<std::uint64_t>(metadata.st_size)};
        const auto entry = zip.ready ? ReadLoaderEntry(zip.archive) : std::nullopt;
        const auto offset = entry ? LoaderOffset(apk, zip.archive, *entry) : std::nullopt;
        if (!entry || !offset || !MatchesDigest(apk, *offset, entry->m_uncomp_size, imageDigest) || !SameFileVersion(apk, metadata))
            return LoaderResult::Failure(
                MakeError(PlatformErrors::InvalidFormat, "Installed OpenXR loader differs from the admitted ABI image"));

        // A full APK member name prevents ambient SONAME lookup. Android maps the exact verified descriptor and stored offset.
        std::string identity{path.data()};
        identity.append("!/").append(LoaderMember);
        android_dlextinfo options{};
        options.flags = ANDROID_DLEXT_USE_LIBRARY_FD | ANDROID_DLEXT_USE_LIBRARY_FD_OFFSET;
        options.library_fd = apk.descriptor;
        options.library_fd_offset = static_cast<off64_t>(*offset);
        LibraryHandle library{android_dlopen_ext(identity.c_str(), RTLD_NOW | RTLD_LOCAL, &options)};
        if (!library)
            return LoaderResult::Failure(MakeError(PlatformErrors::InvalidFormat, "Android could not load the verified APK entry"));
        return LoaderResult::Success(std::make_shared<AndroidApkDynamicLibrary>(application.applicationContext, std::move(library)));
    }
}  // namespace Horo::Platform::Android
