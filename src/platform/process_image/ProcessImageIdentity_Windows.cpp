#include "ProcessImageIdentity.h"
#include "ProcessImageWindowsInternal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>

namespace Horo::Platform {
    namespace {
        const ErrorCodeDescriptor Unavailable{.domain = ErrorDomainId{"horo.platform.process_image"},
                                              .code = ErrorCode{"platform.process_image.unavailable"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Authoritative Windows mapped executable identity is unavailable.",
                                              .remediationHint = "Use an unchanged image with available kernel-backed file comparison.",
                                              .retryable = false,
                                              .userActionable = true};
        constexpr std::uint64_t MaximumImageBytes = 512ULL * 1024 * 1024;
        using CompareMappedFiles = LONG(WINAPI *)(PVOID, PVOID);

        template <typename T> Result<T> Failure(const char *message) {
            return Result<T>::Failure(MakeError(Unavailable, message));
        }

        struct HandleOwner final {
            HANDLE value{};
            HandleOwner() = default;
            HandleOwner(const HandleOwner &) = delete;
            HandleOwner &operator=(const HandleOwner &) = delete;
            HandleOwner(HandleOwner &&) = delete;
            HandleOwner &operator=(HandleOwner &&) = delete;

            ~HandleOwner() {
                if (value && value != INVALID_HANDLE_VALUE)
                    CloseHandle(value);
            }
        };

        struct ViewOwner final {
            void *value{};
            ViewOwner() = default;
            ViewOwner(const ViewOwner &) = delete;
            ViewOwner &operator=(const ViewOwner &) = delete;
            ViewOwner(ViewOwner &&) = delete;
            ViewOwner &operator=(ViewOwner &&) = delete;

            ~ViewOwner() {
                if (value)
                    UnmapViewOfFile(value);
            }
        };

        struct FileEvidence final {
            FILE_ID_INFO id{};
            FILE_STANDARD_INFO standard{};
            FILE_BASIC_INFO basic{};
        };

        struct NativeState final {
            // Reverse destruction unmaps the view before releasing its section and retained file.
            HandleOwner file;
            HandleOwner mapping;
            ViewOwner view;
            HMODULE main{};
            FileEvidence evidence;
        };

        /** @brief Observes only kernel metadata on the retained handle; no pathname is reauthorized. */
        bool ReadEvidence(HANDLE file, FileEvidence &out) noexcept {
            return GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandleEx(file, FileIdInfo, &out.id, sizeof(out.id)) &&
                   GetFileInformationByHandleEx(file, FileStandardInfo, &out.standard, sizeof(out.standard)) &&
                   GetFileInformationByHandleEx(file, FileBasicInfo, &out.basic, sizeof(out.basic)) && !out.standard.Directory &&
                   !out.standard.DeletePending && out.standard.EndOfFile.QuadPart > 0 &&
                   static_cast<std::uint64_t>(out.standard.EndOfFile.QuadPart) <= MaximumImageBytes;
        }

        /** @brief Access-time updates are not content changes; file identity, size and write/change fences are exact. */
        bool SameFile(const FileEvidence &left, const FileEvidence &right) noexcept {
            return left.id.VolumeSerialNumber == right.id.VolumeSerialNumber &&
                   std::memcmp(left.id.FileId.Identifier, right.id.FileId.Identifier, sizeof(left.id.FileId.Identifier)) == 0 &&
                   left.standard.EndOfFile.QuadPart == right.standard.EndOfFile.QuadPart &&
                   left.standard.NumberOfLinks == right.standard.NumberOfLinks &&
                   left.basic.CreationTime.QuadPart == right.basic.CreationTime.QuadPart &&
                   left.basic.LastWriteTime.QuadPart == right.basic.LastWriteTime.QuadPart &&
                   left.basic.ChangeTime.QuadPart == right.basic.ChangeTime.QuadPart &&
                   left.basic.FileAttributes == right.basic.FileAttributes;
        }

        /** @brief Rejects stale or foreign regions without dereferencing caller-provided addresses. */
        bool Region(const void *address, DWORD kind, const void *base, MEMORY_BASIC_INFORMATION &out) noexcept {
            return address && VirtualQuery(address, &out, sizeof(out)) == sizeof(out) && out.State == MEM_COMMIT && out.Type == kind &&
                   out.AllocationBase == base && (out.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0;
        }

        /** @brief Resolves a native capability at runtime; absence never falls back to path or PE comparisons. */
        CompareMappedFiles MappedFileComparator() noexcept {
            const auto library = GetModuleHandleW(L"ntdll.dll");
            return library ? reinterpret_cast<CompareMappedFiles>(GetProcAddress(library, "NtAreMappedFilesTheSame")) : nullptr;
        }

        Result<void> VerifyNative(const NativeState &state) {
            FileEvidence current;
            if (GetModuleHandleW(nullptr) != state.main || !ReadEvidence(state.file.value, current) || !SameFile(state.evidence, current))
                return Failure<void>("The retained executable file identity or content metadata changed.");
            return Detail::VerifyWindowsMainImageFileView(state.view.value);
        }

        /** @brief The OS path locates a candidate only; kernel comparison binds it to the actual mapped image. */
        Result<void> OpenImage(NativeState &state) {
            state.main = GetModuleHandleW(nullptr);
            if (!state.main || !MappedFileComparator())
                return Failure<void>("The actual main image or native mapped-file comparison is unavailable.");
            std::array<wchar_t, 32768> path{};
            if (const auto length = GetModuleFileNameW(state.main, path.data(), static_cast<DWORD>(path.size()));
                length == 0 || length >= path.size())
                return Failure<void>("The actual main-image candidate path is unavailable or oversized.");
            // Deny new write/delete access for the complete evidence lifetime, including hashing and foreign-thread leases.
            state.file.value =
                CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (state.file.value == INVALID_HANDLE_VALUE || !ReadEvidence(state.file.value, state.evidence))
                return Failure<void>("Cannot retain bounded immutable executable file evidence.");
            state.mapping.value = CreateFileMappingW(state.file.value, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!state.mapping.value)
                return Failure<void>("Cannot retain the executable file mapping.");
            state.view.value = MapViewOfFile(state.mapping.value, FILE_MAP_READ, 0, 0, 0);
            if (!state.view.value)
                return Failure<void>("Cannot retain the read-only executable file view.");
            return VerifyNative(state);
        }

        /** @brief Streams the complete retained file within the same finite byte ceiling as other providers. */
        Result<Sha256Digest> HashImage(const NativeState &state) {
            Sha256Builder builder;
            std::array<std::byte, 65536> buffer{};
            std::uint64_t offset{};
            const auto size = static_cast<std::uint64_t>(state.evidence.standard.EndOfFile.QuadPart);
            while (offset < size) {
                DWORD count{};
                if (const auto wanted = static_cast<DWORD>(std::min<std::uint64_t>(buffer.size(), size - offset));
                    !ReadFile(state.file.value, buffer.data(), wanted, &count, nullptr) || count == 0 || count > wanted)
                    return Failure<Sha256Digest>("Cannot read the complete retained executable within its byte bound.");
                if (!builder.Update(std::span(buffer).first(count)))
                    return Failure<Sha256Digest>("Executable identity hash exceeded its admitted length.");
                offset += count;
            }
            return Result<Sha256Digest>::Success(builder.Finalize());
        }
    }  // namespace

    /** @copydoc Detail::VerifyWindowsMainImageFileView */
    Result<void> Detail::VerifyWindowsMainImageFileView(const void *view) {
        const auto main = GetModuleHandleW(nullptr);
        MEMORY_BASIC_INFORMATION imageRegion{}, dataRegion{};
        if (!main || !Region(main, MEM_IMAGE, main, imageRegion) || !Region(view, MEM_MAPPED, view, dataRegion) ||
            (dataRegion.Protect & 0xFFU) != PAGE_READONLY)
            return Failure<void>("A committed main image and retained read-only file view are required.");
        if (const auto compare = MappedFileComparator(); !compare || compare(main, const_cast<void *>(view)) != 0)
            return Failure<void>("The kernel did not bind the retained file to the actual main executable image.");
        return Result<void>::Success();
    }

    /** @copydoc ProcessImageIdentity::Verify */
    Result<void> ProcessImageIdentity::Verify() const {
        if (!state_)
            return Failure<void>("Process image evidence has no native owner.");
        return VerifyNative(*static_cast<const NativeState *>(state_.get()));
    }

    /** @copydoc ProcessImageIdentity::VerifyStaticCodeAddress */
    Result<void> ProcessImageIdentity::VerifyStaticCodeAddress(const void *address) const {
        if (!state_)
            return Failure<void>("Process image evidence has no native owner.");
        const auto &state = *static_cast<const NativeState *>(state_.get());
        MEMORY_BASIC_INFORMATION region{};
        if (!Region(address, MEM_IMAGE, state.main, region))
            return Failure<void>("Participating code is not in the actual main executable image.");
        if (const auto protection = region.Protect & 0xFFU; protection != PAGE_EXECUTE && protection != PAGE_EXECUTE_READ &&
                                                            protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
            return Failure<void>("Participating image memory is not executable code.");
        return VerifyNative(state);
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
