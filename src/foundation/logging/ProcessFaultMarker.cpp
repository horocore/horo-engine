#include "ProcessFaultMarker.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <exception>
#include <random>
#endif

namespace Horo::Log::Internal {
#if defined(_WIN32)
    namespace {
        constexpr std::uint32_t markerVersion = 0x48465201U;
        constexpr std::uint32_t completionTag = 0x48465246U;
        /** @brief Marker identity prepared during normal startup. */
        struct Header {
            std::uint32_t version;
            std::array<std::uint32_t, 4> id;
        };

        /** @brief Scalar evidence written once by the native callback. */
        struct Footer {
            std::uint32_t code;
            std::uint32_t thread;
            std::uint32_t tag;
        };

        /** @brief Layout of a completed versioned Windows marker. */
        struct Marker {
            Header header;
            Footer footer;
        };
        static_assert(sizeof(Header) == 20 && sizeof(Footer) == 12 && sizeof(Marker) == 32);

        // The handler exchanges the file exactly once. Shutdown may close only a file it claims itself.
        // These globals have process lifetime because Windows can still be running an in-flight callback after disarming.
        constinit std::atomic<HANDLE> markerFile{};
        constinit std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> previousFilter{};
        static_assert(decltype(markerFile)::is_always_lock_free && decltype(previousFilter)::is_always_lock_free);

        /** @brief Claims the pre-opened marker without allocating, logging, or waiting on engine state. */
        LONG WINAPI RecordFault(EXCEPTION_POINTERS *const exception) noexcept {
            if (exception && exception->ExceptionRecord) {
                if (const HANDLE file = markerFile.exchange(nullptr)) {
                    const Footer footer{exception->ExceptionRecord->ExceptionCode, GetCurrentThreadId(), completionTag};
                    DWORD written{};
                    static_cast<void>(WriteFile(file, &footer, sizeof(footer), &written, nullptr));
                    CloseHandle(file);
                }
            }
            if (const auto previous = previousFilter.load())
                return previous(exception);
            return EXCEPTION_CONTINUE_SEARCH;
        }

        /** @brief Creates the per-fault identity before entering the exception path. */
        std::optional<Header> CreateHeader() {
            try {
                std::random_device random;
                return Header{.version = markerVersion, .id = {random(), random(), random(), random()}};
            } catch (const std::exception &) {
                return std::nullopt;
            }
        }
    }

    /** @copydoc ReadProcessFault */
    std::optional<ProcessFault> ReadProcessFault(const std::filesystem::path &path) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return std::nullopt;

        Marker record{};
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        LARGE_INTEGER size{};
        DWORD read{};
        const bool complete = GetFileType(file) == FILE_TYPE_DISK &&
                              GetFileInformationByHandleEx(file, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
                              !(attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                              GetFileSizeEx(file, &size) && size.QuadPart == sizeof(record) &&
                              ReadFile(file, &record, sizeof(record), &read, nullptr) && read == sizeof(record);
        CloseHandle(file);
        if (!complete || record.header.version != markerVersion || record.footer.tag != completionTag)
            return std::nullopt;
        return ProcessFault{.id = record.header.id, .code = record.footer.code, .thread = record.footer.thread};
    }

    /** @copydoc DiscardProcessFaultMarker */
    void DiscardProcessFaultMarker(const std::filesystem::path &path) noexcept {
        DeleteFileW(path.c_str());
    }

    /** @copydoc ArmProcessFaultMarker */
    bool ArmProcessFaultMarker(const std::filesystem::path &path) {
        const auto header = CreateHeader();
        if (!header)
            return false;
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written{};
        if (!WriteFile(file, &*header, sizeof(*header), &written, nullptr) || written != sizeof(*header)) {
            CloseHandle(file);
            DiscardProcessFaultMarker(path);
            return false;
        }
        markerFile.store(file);
        previousFilter.store(SetUnhandledExceptionFilter(&RecordFault));
        return true;
    }

    /** @copydoc DisarmProcessFaultMarker */
    bool DisarmProcessFaultMarker(const std::filesystem::path &path) noexcept {
        SetUnhandledExceptionFilter(previousFilter.load());
        const HANDLE file = markerFile.exchange(nullptr);
        if (!file)
            return false;
        CloseHandle(file);
        DiscardProcessFaultMarker(path);
        return true;
    }
#else
    /** @copydoc ReadProcessFault */
    std::optional<ProcessFault> ReadProcessFault(const std::filesystem::path &) {
        return std::nullopt;
    }

    /** @copydoc DiscardProcessFaultMarker */
    void DiscardProcessFaultMarker(const std::filesystem::path &) noexcept {}

    /** @copydoc ArmProcessFaultMarker */
    bool ArmProcessFaultMarker(const std::filesystem::path &) {
        return false;
    }

    /** @copydoc DisarmProcessFaultMarker */
    bool DisarmProcessFaultMarker(const std::filesystem::path &) noexcept {
        return true;
    }
#endif
}
