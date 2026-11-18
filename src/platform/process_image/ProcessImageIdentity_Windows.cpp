#include "ProcessImageIdentity.h"
#include "ProcessImageWindowsInternal.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Tool Help needs the Win32 base types first.
// clang-format off
#include <Windows.h>
#include <TlHelp32.h>
// clang-format on
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>

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

        /** @brief Retains disk bytes with a read-only mapping and denies write/delete opens for the full lease. */
        Result<void> OpenReadOnlyFileView(NativeState &state, const wchar_t *path, DWORD flags) {
            // Deny new write/delete access for the complete evidence lifetime, including hashing and foreign-thread leases.
            state.file.value = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, flags, nullptr);
            if (state.file.value == INVALID_HANDLE_VALUE || !ReadEvidence(state.file.value, state.evidence))
                return Failure<void>("Cannot retain bounded immutable image file evidence.");
            state.mapping.value = CreateFileMappingW(state.file.value, nullptr, PAGE_READONLY, 0, 0, nullptr);
            if (!state.mapping.value)
                return Failure<void>("Cannot retain the image file mapping.");
            state.view.value = MapViewOfFile(state.mapping.value, FILE_MAP_READ, 0, 0, 0);
            if (!state.view.value)
                return Failure<void>("Cannot retain the read-only image file view.");
            return Result<void>::Success();
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
            if (const auto file = OpenReadOnlyFileView(state, path.data(), FILE_ATTRIBUTE_NORMAL); file.HasError())
                return file;
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

    namespace {
        /** @brief Validates the exact file object behind an already loaded image by OS comparison. */
        Result<void> VerifyMappedImageFileView(HMODULE image, const void *view) {
            MEMORY_BASIC_INFORMATION imageRegion{}, dataRegion{};
            if (!image || !Region(image, MEM_IMAGE, image, imageRegion) || !Region(view, MEM_MAPPED, view, dataRegion) ||
                (dataRegion.Protect & 0xFFU) != PAGE_READONLY)
                return Failure<void>("A committed image and retained read-only file view are required.");
            if (const auto compare = MappedFileComparator(); !compare || compare(image, const_cast<void *>(view)) != 0)
                return Failure<void>("The kernel did not bind the retained file to the actual loaded image.");
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc Detail::VerifyWindowsMainImageFileView */
    Result<void> Detail::VerifyWindowsMainImageFileView(const void *view) {
        return VerifyMappedImageFileView(GetModuleHandleW(nullptr), view);
    }

    namespace {
        /** @brief Owns a reversible reference to a module already mapped by Windows. */
        struct ModuleReference final {
            HMODULE value{};
            ModuleReference() = default;
            ModuleReference(const ModuleReference &) = delete;
            ModuleReference &operator=(const ModuleReference &) = delete;

            ~ModuleReference() {
                if (value)
                    FreeLibrary(value);
            }
        };

        /** @brief Compares length-bounded paths or names without locale-dependent case mapping. */
        bool EqualOrdinal(std::wstring_view first, std::wstring_view second) noexcept {
            return CompareStringOrdinal(first.data(), static_cast<int>(first.size()), second.data(), static_cast<int>(second.size()),
                                        TRUE) == CSTR_EQUAL;
        }

        /** @brief Accepts only finite ASCII DLL basenames that cannot select an alternate path. */
        bool IsDllBasename(std::wstring_view name) noexcept {
            if (name.size() <= 4 || name.size() > 128 || !EqualOrdinal(name.substr(name.size() - 4), L".dll"))
                return false;
            return std::ranges::all_of(name.substr(0, name.size() - 4), [](wchar_t ch) {
                return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_';
            });
        }

        /** @brief Rejects absent, ambiguous, incomplete or oversized observed module inventories. */
        HMODULE FindUniqueLoadedModule(std::wstring_view basename) noexcept {
            constexpr unsigned maximumModules = 1024;
            HandleOwner snapshot;
            snapshot.value = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
            if (snapshot.value == INVALID_HANDLE_VALUE)
                return nullptr;
            MODULEENTRY32W entry{};
            entry.dwSize = sizeof(entry);
            if (!Module32FirstW(snapshot.value, &entry))
                return nullptr;
            HMODULE found{};
            unsigned count{};
            do {
                if (++count > maximumModules)
                    return nullptr;
                if (!EqualOrdinal(entry.szModule, basename))
                    continue;
                if (found)
                    return nullptr;
                found = entry.hModule;
            } while (Module32NextW(snapshot.value, &entry));
            return GetLastError() == ERROR_NO_MORE_FILES ? found : nullptr;
        }

        /** @brief Constructs the absolute OS system candidate independently of the observed module. */
        std::wstring SystemFile(std::wstring_view basename) {
            std::array<wchar_t, 32768> directory{};
            const auto length = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
            if (length < 3 || length >= directory.size() || directory[1] != L':' || directory[2] != L'\\')
                return {};
            std::wstring result{L"\\\\?\\"};
            result.append(directory.data(), length);
            if (result.back() != L'\\')
                result.push_back(L'\\');
            result.append(basename);
            return result;
        }

        /** @brief Refuses redirection away from the OS system path or through a reparse-point leaf. */
        bool MatchesFinalSystemPath(const NativeState &state, std::wstring_view expected) noexcept {
            if ((state.evidence.basic.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                return false;
            std::array<wchar_t, 32768> actual{};
            const auto length = GetFinalPathNameByHandleW(state.file.value, actual.data(), static_cast<DWORD>(actual.size()),
                                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            return length > 0 && length < actual.size() && EqualOrdinal(std::wstring_view{actual.data(), length}, expected);
        }
    }  // namespace

    /** @brief Releases the file view and handles before decrementing the loader reference. */
    struct WindowsSystemModuleImage::State final {
        ModuleReference module;
        NativeState image;
        std::wstring basename;
    };

    /** @copydoc WindowsSystemModuleImage::WindowsSystemModuleImage */
    WindowsSystemModuleImage::WindowsSystemModuleImage(WindowsSystemModuleImage &&) noexcept = default;

    /** @copydoc WindowsSystemModuleImage::operator= */
    WindowsSystemModuleImage &WindowsSystemModuleImage::operator=(WindowsSystemModuleImage &&) noexcept = default;

    /** @copydoc WindowsSystemModuleImage::~WindowsSystemModuleImage */
    WindowsSystemModuleImage::~WindowsSystemModuleImage() = default;

    /** @copydoc WindowsSystemModuleImage::WindowsSystemModuleImage */
    WindowsSystemModuleImage::WindowsSystemModuleImage(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}

    /** @copydoc WindowsSystemModuleImage::ReadVerifiedFile */
    Result<std::span<const std::byte>> WindowsSystemModuleImage::ReadVerifiedFile() const {
        using Bytes = std::span<const std::byte>;
        FileEvidence current;
        if (!state_ || FindUniqueLoadedModule(state_->basename) != state_->module.value ||
            !ReadEvidence(state_->image.file.value, current) || !SameFile(state_->image.evidence, current))
            return Failure<Bytes>("The observed system module inventory or retained image file changed.");
        if (const auto mapped = VerifyMappedImageFileView(state_->module.value, state_->image.view.value); mapped.HasError())
            return Result<Bytes>::Failure(mapped.ErrorValue());
        return Result<Bytes>::Success(
            Bytes{static_cast<const std::byte *>(state_->image.view.value), static_cast<std::size_t>(current.standard.EndOfFile.QuadPart)});
    }

    /** @copydoc RetainLoadedWindowsSystemModule */
    Result<WindowsSystemModuleImage> RetainLoadedWindowsSystemModule(std::string_view basename) {
        using Outcome = Result<WindowsSystemModuleImage>;
        try {
            if (basename.size() <= 4 || basename.size() > 128)
                return Failure<WindowsSystemModuleImage>("A bounded Windows system DLL basename is required.");
            auto state = std::make_unique<WindowsSystemModuleImage::State>();
            state->basename.assign(basename.begin(), basename.end());
            if (!IsDllBasename(state->basename))
                return Failure<WindowsSystemModuleImage>("A bounded Windows system DLL basename is required.");
            const auto observed = FindUniqueLoadedModule(state->basename);
            if (!observed ||
                !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<const wchar_t *>(observed),
                                    &state->module.value) ||
                state->module.value != observed)
                return Failure<WindowsSystemModuleImage>("An unambiguous preloaded Windows module could not be retained.");
            const auto path = SystemFile(state->basename);
            if (path.empty())
                return Failure<WindowsSystemModuleImage>("The absolute Windows system directory is unavailable.");
            if (const auto file = OpenReadOnlyFileView(state->image, path.c_str(), FILE_FLAG_OPEN_REPARSE_POINT); file.HasError())
                return Outcome::Failure(file.ErrorValue());
            if (!MatchesFinalSystemPath(state->image, path))
                return Failure<WindowsSystemModuleImage>("The Windows system file has been redirected.");
            WindowsSystemModuleImage evidence{std::move(state)};
            if (const auto bytes = evidence.ReadVerifiedFile(); bytes.HasError())
                return Outcome::Failure(bytes.ErrorValue());
            return Outcome::Success(std::move(evidence));
        } catch (const std::bad_alloc &) {
            return Failure<WindowsSystemModuleImage>("System module evidence allocation failed.");
        }
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
