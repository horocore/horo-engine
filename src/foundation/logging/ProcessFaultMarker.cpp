#include "ProcessFaultMarker.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
#include <atomic>
#include <exception>
#include <random>
#endif

namespace Horo::Log::Internal {
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
    namespace {
        constexpr std::uint32_t windowsMarkerVersion = 0x48465201U;
        constexpr std::uint32_t posixMarkerVersion = 0x48465202U;
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

        /** @brief Layout of a completed versioned native marker. */
        struct Marker {
            Header header;
            Footer footer;
        };
        static_assert(sizeof(Header) == 20 && sizeof(Footer) == 12 && sizeof(Marker) == 32);

        /** @brief Creates the per-fault identity before entering the native handler. */
        std::optional<Header> CreateHeader(const std::uint32_t version) {
            try {
                std::random_device random;
                return Header{.version = version, .id = {random(), random(), random(), random()}};
            } catch (const std::exception &) {
                return std::nullopt;
            }
        }

        /** @brief Accepts only a completed marker from the expected native encoder. */
        std::optional<ProcessFault> DecodeMarker(const Marker &record, const std::uint32_t version,
                                                  const ProcessFaultClass classification) {
            if (record.header.version != version || record.footer.tag != completionTag)
                return std::nullopt;
            return ProcessFault{
                .id = record.header.id, .classification = classification, .code = record.footer.code, .thread = record.footer.thread};
        }
    }
#endif

#if defined(_WIN32)
    namespace {
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
        return complete ? DecodeMarker(record, windowsMarkerVersion, ProcessFaultClass::WindowsException) : std::nullopt;
    }

    /** @copydoc DiscardProcessFaultMarker */
    void DiscardProcessFaultMarker(const std::filesystem::path &path) noexcept {
        DeleteFileW(path.c_str());
    }

    /** @copydoc ArmProcessFaultMarker */
    bool ArmProcessFaultMarker(const std::filesystem::path &path) {
        const auto header = CreateHeader(windowsMarkerVersion);
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
#elif defined(__linux__) || defined(__APPLE__)
    namespace {
        constexpr std::array fatalSignals{SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGSEGV};

        // Ordinary lifecycle is serialized by Logger. The callback only touches lock-free process-lifetime state.
        constinit std::atomic<int> markerFile{-1};
        constinit std::atomic<pid_t> markerProcess{};
        static_assert(decltype(markerFile)::is_always_lock_free && decltype(markerProcess)::is_always_lock_free);
        std::array<bool, fatalSignals.size()> installedSignals{};

        /** @brief Constructs the default disposition using only signal-safe operations. */
        struct sigaction DefaultDisposition() noexcept {
            struct sigaction action{};
            action.sa_handler = SIG_DFL;
            sigemptyset(&action.sa_mask);
            return action;
        }

        /** @brief Writes one bounded record and re-delivers the signal under its default disposition. */
        void RecordSignal(const int signal) noexcept {
            if (getpid() == markerProcess.load()) {
                const int file = markerFile.exchange(-1);
                if (file >= 0) {
                    const Footer footer{static_cast<std::uint32_t>(signal), 0, completionTag};
                    // Startup validates the full record. A partial write must never delay signal delivery.
                    [[maybe_unused]] const auto written = write(file, &footer, sizeof(footer));
                    static_cast<void>(close(file));
                }
            }
            // Reset explicitly because some fatal signals cannot use automatic reset on every POSIX platform.
            const auto defaults = DefaultDisposition();
            if (sigaction(signal, &defaults, nullptr) != 0 || raise(signal) != 0)
                _exit(128 + signal);
        }

        /** @brief Installs capture only for fatal signals still using the default disposition. */
        bool InstallDefaultHandlers() noexcept {
            struct sigaction handler{};
            handler.sa_handler = &RecordSignal;
            sigemptyset(&handler.sa_mask);
            for (const int signal : fatalSignals)
                sigaddset(&handler.sa_mask, signal);

            bool installed = false;
            for (std::size_t index = 0; index < fatalSignals.size(); ++index) {
                const int signal = fatalSignals[index];
                struct sigaction current{};
                if (sigaction(signal, nullptr, &current) == 0 && !(current.sa_flags & SA_SIGINFO) && current.sa_handler == SIG_DFL)
                    installedSignals[index] = sigaction(signal, &handler, nullptr) == 0;
                installed |= installedSignals[index];
            }
            return installed;
        }

        /** @brief Releases only dispositions still owned by the marker. */
        void RestoreDefaultHandlers() noexcept {
            const auto defaults = DefaultDisposition();
            for (std::size_t index = 0; index < fatalSignals.size(); ++index) {
                if (!installedSignals[index])
                    continue;
                installedSignals[index] = false;
                const int signal = fatalSignals[index];
                struct sigaction current{};
                if (sigaction(signal, nullptr, &current) == 0 && !(current.sa_flags & SA_SIGINFO) && current.sa_handler == &RecordSignal)
                    static_cast<void>(sigaction(signal, &defaults, nullptr));
            }
        }

        /** @brief Opens and locks an ordinary marker only after its writer has released it. */
        int OpenRetiredMarker(const std::filesystem::path &path) noexcept {
            const int file = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (file < 0)
                return -1;
            struct stat info{};
            if (fstat(file, &info) == 0 && S_ISREG(info.st_mode) && flock(file, LOCK_EX | LOCK_NB) == 0)
                return file;
            static_cast<void>(close(file));
            return -1;
        }
    }

    /** @copydoc ReadProcessFault */
    std::optional<ProcessFault> ReadProcessFault(const std::filesystem::path &path) {
        const int file = OpenRetiredMarker(path);
        if (file < 0)
            return std::nullopt;
        Marker record{};
        struct stat info{};
        const bool complete = fstat(file, &info) == 0 && info.st_size == static_cast<off_t>(sizeof(record)) &&
                              read(file, &record, sizeof(record)) == static_cast<ssize_t>(sizeof(record));
        static_cast<void>(close(file));
        return complete ? DecodeMarker(record, posixMarkerVersion, ProcessFaultClass::PosixSignal) : std::nullopt;
    }

    /** @copydoc DiscardProcessFaultMarker */
    void DiscardProcessFaultMarker(const std::filesystem::path &path) noexcept {
        const int file = OpenRetiredMarker(path);
        if (file >= 0) {
            static_cast<void>(unlink(path.c_str()));
            static_cast<void>(close(file));
        }
    }

    /** @copydoc ArmProcessFaultMarker */
    bool ArmProcessFaultMarker(const std::filesystem::path &path) {
        const auto header = CreateHeader(posixMarkerVersion);
        if (!header)
            return false;
        const int file = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_SYNC, S_IRUSR | S_IWUSR);
        if (file < 0)
            return false;
        if (flock(file, LOCK_EX | LOCK_NB) != 0 || write(file, &*header, sizeof(*header)) != static_cast<ssize_t>(sizeof(*header))) {
            static_cast<void>(unlink(path.c_str()));
            static_cast<void>(close(file));
            return false;
        }
        markerProcess.store(getpid());
        markerFile.store(file);
        if (InstallDefaultHandlers())
            return true;
        static_cast<void>(DisarmProcessFaultMarker(path));
        return false;
    }

    /** @copydoc DisarmProcessFaultMarker */
    bool DisarmProcessFaultMarker(const std::filesystem::path &path) noexcept {
        RestoreDefaultHandlers();
        const int file = markerFile.exchange(-1);
        if (file < 0)
            return false;
        static_cast<void>(unlink(path.c_str()));
        static_cast<void>(close(file));
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
