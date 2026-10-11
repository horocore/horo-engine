#pragma once

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Time.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo {
    /**
     * @file Platform.h
     * @brief Narrow operating-system service contracts selected by a host composition root.
     */

    class NativeExternalProcessRunner;

    /** @brief Reports availability of optional platform facilities for a composed host. */
    struct PlatformCapabilities {
        bool supportsProcessExecution{false}; /**< Whether the host permits process execution. */
        bool hasCredentialStore{false};       /**< Whether a credential store was composed. */
        bool hasNativeDialogs{false};         /**< Whether native dialogs were composed. */
        bool hasCrashService{false};          /**< Whether crash reporting was composed. */
    };

    /** @brief Provides the smallest filesystem query needed by initial platform consumers. */
    class FileSystem {
    public:
        virtual ~FileSystem() = default;

        /** @brief Tests whether a native path exists. @param path Native path to query. @return True when the path exists. */
        [[nodiscard]] virtual bool Exists(const std::filesystem::path &path) const = 0;
    };

    /** @brief Move-only operating-system-held exclusive file lock. */
    class ExclusiveFileLock {  // NOSONAR(cpp:S3624) Pimpl pattern: destructor defined in .cpp to complete State
    public:
        struct State;

        ExclusiveFileLock() noexcept;
        ExclusiveFileLock(const ExclusiveFileLock &) = delete;
        ExclusiveFileLock &operator=(const ExclusiveFileLock &) = delete;
        ExclusiveFileLock(ExclusiveFileLock &&) noexcept;
        ExclusiveFileLock &operator=(ExclusiveFileLock &&) noexcept;
        ~ExclusiveFileLock();

        /** @brief Reports whether this object currently owns the native lock. */
        [[nodiscard]] explicit operator bool() const noexcept;

        /**
         * @brief Verifies that this owned native lease protects one exact canonical lock-file path.
         * @param path Absolute canonical lock-file path selected by the host publication authority.
         * @return True only for a live lease acquired for that exact path; default and moved-from leases return false.
         * @details Diagnostic owner metadata never participates in this authority check. Callers must protect the
         *          parent directory against external rename or replacement for the lease's lifetime.
         */
        [[nodiscard]] bool ProtectsPath(const std::filesystem::path &path) const;

    private:
        friend class NativeDurableFileSystem;
        explicit ExclusiveFileLock(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };

    /** @brief Move-only shared product-launch or exclusive maintenance lease for one installation. */
    class ProductLaunchLease {
    public:
        struct State;

        ProductLaunchLease() noexcept;
        ProductLaunchLease(const ProductLaunchLease &) = delete;
        ProductLaunchLease &operator=(const ProductLaunchLease &) = delete;
        ProductLaunchLease(ProductLaunchLease &&) noexcept;
        ProductLaunchLease &operator=(ProductLaunchLease &&) noexcept;
        ~ProductLaunchLease();

        /** @brief Reports whether this object currently holds the native lease. */
        [[nodiscard]] explicit operator bool() const noexcept;

    private:
        friend class NativeDurableFileSystem;
        friend class NativeExternalProcessRunner;
        explicit ProductLaunchLease(std::unique_ptr<State> state) noexcept;
        [[nodiscard]] bool IsMaintenance() const noexcept;
        [[nodiscard]] std::uintptr_t NativeHandle() const noexcept;
        [[nodiscard]] static ProductLaunchLease AdoptMaintenanceNative(std::uintptr_t native);
        std::unique_ptr<State> state_;
    };

    /**
     * @brief Caller-owned receipt for one irreversible native atomic replacement.
     * @note The invoking thread owns mutation and observation; cross-thread use requires caller synchronization.
     * The receipt starts uncommitted, never resets, and outlives the replacement call including any exception.
     */
    class AtomicFileReplacementReceipt final {
    public:
        AtomicFileReplacementReceipt() noexcept = default;
        AtomicFileReplacementReceipt(const AtomicFileReplacementReceipt &) = delete;
        AtomicFileReplacementReceipt &operator=(const AtomicFileReplacementReceipt &) = delete;

        /** @brief Reports successful native replacement independently of later durability failure. @return True after commit. */
        [[nodiscard]] bool WasCommitted() const noexcept {
            return committed_;
        }

        /** @brief Records native replacement immediately after its successful OS return, without allocation or exceptions. */
        void RecordCommitted() noexcept {
            committed_ = true;
        }

    private:
        bool committed_{};
    };

    /** @brief Cross-platform durable filesystem primitives for user-data transactions. */
    class DurableFileSystem {
    public:
        virtual ~DurableFileSystem() = default;

        /** @brief Creates missing parent directories and immediately acquires an exclusive OS lock. @param path Lock-file path within
         * host-owned parent directories. Symlinks, reparse points, and multiply linked files are rejected.
         * @param ownerMetadata Diagnostic-only owner text. @return Move-only lock or typed busy/I/O failure. */
        [[nodiscard]] virtual Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                            std::string_view ownerMetadata) = 0;
        /** @brief Queries filesystem capacity. @param path Path on the target filesystem. @return Currently available bytes or typed I/O
         * failure. */
        [[nodiscard]] virtual Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const = 0;
        /** @brief Creates missing parent directories, then writes and flushes a complete file. @param path Destination path.
         * @param bytes Complete contents. @return Success after file and directory durability, or typed I/O failure. */
        [[nodiscard]] virtual Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) = 0;
        /** @brief Exclusively creates a private regular file in an existing host-owned parent and flushes it.
         * @param path Absolute prepared path whose parent is protected from replacement.
         * @param bytes Complete contents, including an empty document. @param created Fresh false receipt.
         * @param permissions Existing destination mode to apply before flushing; unknown keeps private defaults.
         * @return Durable success or typed failure; created is set immediately after native exclusive creation,
         * including failures and exceptions after creation. Existing files and redirects are never truncated.
         */
        [[nodiscard]] virtual Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes,
                                                               bool &created,
                                                               std::filesystem::perms permissions = std::filesystem::perms::unknown);
        /** @brief Creates missing destination parents, then copies and flushes a file. @param source Existing source file.
         * @param destination Destination on the transaction filesystem. @return Success after destination durability, or typed I/O failure.
         */
        [[nodiscard]] virtual Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) = 0;
        /** @brief Creates missing destination parents, then atomically replaces a destination. @param prepared Same-filesystem prepared
         * file. @param destination Published destination. @return Success after directory durability, or typed I/O failure. */
        [[nodiscard]] virtual Result<void> AtomicReplace(const std::filesystem::path &prepared,
                                                         const std::filesystem::path &destination) = 0;
        /**
         * @brief Replaces a prepared file and records the irreversible native commit separately from durability confirmation.
         * @param prepared Same-filesystem complete prepared file.
         * @param destination Published destination whose namespace is protected by the caller's writer authority.
         * @param receipt Fresh caller-owned receipt, retained even when the call fails or throws.
         * @return Success after directory durability, typed failure before commit, or a durability failure after commit.
         * @pre receipt.WasCommitted() is false and the invoking thread exclusively owns the receipt.
         * @post Overrides record commit immediately after successful native replacement, before sync, error allocation or callbacks.
         * A true receipt stays true across subsequent errors and exceptions; callers must not report rollback in that case.
         * @details The default rejects unsupported tracking before any write and never delegates the ambiguous AtomicReplace primitive.
         */
        [[nodiscard]] virtual Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared,
                                                                const std::filesystem::path &destination,
                                                                AtomicFileReplacementReceipt &receipt);
        /** @brief Durably removes a file. @param path File to remove. @return Success after directory durability, or typed I/O failure. */
        [[nodiscard]] virtual Result<void> RemoveDurable(const std::filesystem::path &path) = 0;
        /** @brief Synchronizes directory metadata. @param path Directory to synchronize. @return Success or typed I/O failure. */
        [[nodiscard]] virtual Result<void> SyncDirectory(const std::filesystem::path &path) = 0;
    };

    /** @brief Native Windows/macOS/Linux durable filesystem implementation. */
    class NativeDurableFileSystem final : public DurableFileSystem {
    public:
        /** @copydoc DurableFileSystem::TryAcquireExclusive
         * @pre path is absolute, lexically normalized, has a nonempty filename and contains no embedded NUL.
         * The host authorizes the path and protects its canonical parent from concurrent replacement.
         * @post Invalid path text is rejected before creating directories or writing lock metadata.
         */
        [[nodiscard]] Result<ExclusiveFileLock> TryAcquireExclusive(const std::filesystem::path &path,
                                                                    std::string_view ownerMetadata) override;
        /** @brief Holds a shared launch lease until the product process exits; fails while maintenance is active. */
        [[nodiscard]] Result<ProductLaunchLease> TryAcquireProductLaunch(const std::filesystem::path &installationRoot) const;
        /** @brief Holds an exclusive maintenance gate; fails while any product launch lease is active. */
        [[nodiscard]] Result<ProductLaunchLease> TryAcquireProductMaintenance(const std::filesystem::path &installationRoot) const;
        /**
         * @brief Adopts an OS-inherited exclusive lease only after matching its native file identity to this installation.
         * @param installationRoot Exact absolute installation root selected by the trusted product host.
         * @return Owned lease or an I/O failure when the inherited capability is absent, malformed, or for another installation.
         */
        [[nodiscard]] Result<ProductLaunchLease> AdoptInheritedProductMaintenance(const std::filesystem::path &installationRoot) const;
        [[nodiscard]] Result<std::uint64_t> AvailableBytes(const std::filesystem::path &path) const override;
        [[nodiscard]] Result<void> WriteDurable(const std::filesystem::path &path, std::span<const std::byte> bytes) override;
        /**
         * @brief Creates a new private file at offset zero or appends to one regular, single-link file at an exact offset.
         * @param path Host-owned private file path; parent directory already exists and is protected from concurrent mutation.
         * @param expectedOffset Required current file length; zero requires the file to be absent.
         * @param bytes Nonempty bytes to append and flush durably.
         * @return Success after file and directory durability, or typed I/O failure without publishing a checkpoint.
         */
        [[nodiscard]] Result<void> AppendPrivateDurable(const std::filesystem::path &path, std::uint64_t expectedOffset,
                                                        std::span<const std::byte> bytes);
        /** @copydoc DurableFileSystem::WritePrivateDurable */
        [[nodiscard]] Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                                       std::filesystem::perms permissions = std::filesystem::perms::unknown) override;
        [[nodiscard]] Result<void> CopyDurable(const std::filesystem::path &source, const std::filesystem::path &destination) override;
        [[nodiscard]] Result<void> AtomicReplace(const std::filesystem::path &prepared, const std::filesystem::path &destination) override;
        /** @copydoc DurableFileSystem::AtomicReplaceTracked */
        [[nodiscard]] Result<void> AtomicReplaceTracked(const std::filesystem::path &prepared, const std::filesystem::path &destination,
                                                        AtomicFileReplacementReceipt &receipt) override;
        [[nodiscard]] Result<void> RemoveDurable(const std::filesystem::path &path) override;
        [[nodiscard]] Result<void> SyncDirectory(const std::filesystem::path &path) override;

    private:
        [[nodiscard]] Result<ProductLaunchLease> TryAcquireProductLease(const std::filesystem::path &installationRoot,
                                                                        bool maintenance) const;
    };

    /** @copydoc DurableFileSystem::AtomicReplaceTracked */
    inline Result<void> DurableFileSystem::AtomicReplaceTracked(const std::filesystem::path &, const std::filesystem::path &,
                                                                AtomicFileReplacementReceipt &) {
        const ErrorCodeDescriptor unsupported{.domain = ErrorDomainId{"horo.platform.filesystem"},
                                              .code = ErrorCode{"filesystem.atomic_tracking_unsupported"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The filesystem does not support tracked atomic replacement.",
                                              .remediationHint = "Compose a filesystem implementation with native commit tracking.",
                                              .retryable = false,
                                              .userActionable = false};
        return Result<void>::Failure(MakeError(unsupported));
    }

    /** @copydoc DurableFileSystem::WritePrivateDurable */
    inline Result<void> DurableFileSystem::WritePrivateDurable(const std::filesystem::path &, std::span<const std::byte>, bool &,
                                                               std::filesystem::perms) {
        const ErrorCodeDescriptor unsupported{.domain = ErrorDomainId{"horo.platform.filesystem"},
                                              .code = ErrorCode{"filesystem.private_creation_unsupported"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The filesystem does not support exclusive private creation.",
                                              .remediationHint = "Compose a filesystem with exclusive creation receipts.",
                                              .retryable = false,
                                              .userActionable = false};
        return Result<void>::Failure(MakeError(unsupported));
    }

    /** @brief Provides monotonic time for scheduling without exposing wall-clock time. */
    class Clock {
    public:
        virtual ~Clock() = default;

        /** @brief Gets the elapsed monotonic time from an implementation-defined origin. @return Monotonic elapsed time. */
        [[nodiscard]] virtual Duration MonotonicNow() const = 0;
    };

    /** @brief Wall-clock source used only for durable records and user-facing timestamps. */
    class WallClock {
    public:
        virtual ~WallClock() = default;
        /** @brief Returns the current UTC system-clock time. */
        [[nodiscard]] virtual std::chrono::system_clock::time_point UtcNow() const = 0;
    };

    /** @brief Production wall clock backed by std::chrono::system_clock. */
    class SystemWallClock final : public WallClock {
    public:
        [[nodiscard]] std::chrono::system_clock::time_point UtcNow() const override;
    };

    /** @brief Identifies the current host process without exposing native handles. */
    struct ProcessMetadata {
        std::uint64_t id{};         /**< Stable process identifier within the host OS. */
        std::string executableName; /**< Host-provided executable display name. */
    };

    /** @brief Supplies read-only process metadata and environment access to portable code. */
    class ProcessService {
    public:
        virtual ~ProcessService() = default;

        /** @brief Gets metadata for the current process. @return Current process metadata. */
        [[nodiscard]] virtual ProcessMetadata CurrentProcess() const = 0;

        /** @brief Looks up a single environment value. @param name Variable name. @return Value when exposed by host policy. */
        [[nodiscard]] virtual std::optional<std::string> EnvironmentValue(std::string_view name) const = 0;
    };

    /** @brief Names the user-writable directory roots resolved by a platform host. */
    struct UserDirectoryPaths {
        std::filesystem::path config;
        std::filesystem::path state;
        std::filesystem::path cache;
        std::filesystem::path logs;
        std::filesystem::path crash;
        std::filesystem::path temporary;
    };

    /** @brief Resolves logical user-data directory roots without exposing environment conventions. */
    class UserDirectories {
    public:
        virtual ~UserDirectories() = default;

        /** @brief Gets the configuration directory. @return Resolved native path. */
        [[nodiscard]] virtual const std::filesystem::path &Config() const = 0;
        /** @brief Gets the persistent state directory. @return Resolved native path. */
        [[nodiscard]] virtual const std::filesystem::path &State() const = 0;
        /** @brief Gets the cache directory. @return Resolved native path. */
        [[nodiscard]] virtual const std::filesystem::path &Cache() const = 0;
        /** @brief Gets the log directory. @return Resolved native path. */
        [[nodiscard]] virtual const std::filesystem::path &Logs() const = 0;
        /** @brief Gets the crash-data directory. @return Resolved native path. */
        [[nodiscard]] virtual const std::filesystem::path &Crash() const = 0;
        /** @brief Gets the temporary directory. @return Resolved native path. */
        [[nodiscard]] virtual const std::filesystem::path &Temporary() const = 0;
    };

    class CredentialStore;

    /** @brief Optional host-owned native file picker; calls run synchronously on the UI thread. */
    class NativeDialogs {
    public:
        virtual ~NativeDialogs() = default;

        /** @brief Opens a multi-file picker. @param title Localized window title. @return Selected native paths, empty on cancellation. */
        [[nodiscard]] virtual std::vector<std::filesystem::path> ChooseOpenFiles(std::string_view title) = 0;
        /** @brief Opens a folder picker. @param title Localized window title. @return Selected native path, or none on cancellation. */
        [[nodiscard]] virtual std::optional<std::filesystem::path> ChooseFolder(std::string_view title) = 0;
    };
    class CrashService;

    /** @brief Explicitly composed baseline and optional platform services for one host lifetime. */
    class PlatformServices {
    public:
        /**
         * @brief Constructs a platform service bundle from host-owned services.
         * @param files Filesystem implementation.
         * @param clock Monotonic clock implementation.
         * @param processes Process metadata implementation.
         * @param directories User-directory implementation.
         * @param capabilities Host policy capabilities; optional service availability is derived from supplied pointers.
         * @param credentials Optional credential store.
         * @param dialogs Optional native dialog service.
         * @param crash Optional crash service.
         */
        PlatformServices(FileSystem &files, Clock &clock, ProcessService &processes, UserDirectories &directories,
                         PlatformCapabilities capabilities = {}, CredentialStore *credentials = nullptr, NativeDialogs *dialogs = nullptr,
                         CrashService *crash = nullptr) noexcept;

        /** @brief Gets the capabilities declared by the composition root. @return Capability values. */
        [[nodiscard]] const PlatformCapabilities &Capabilities() const noexcept;

        FileSystem &files;
        Clock &clock;
        ProcessService &processes;
        UserDirectories &directories;
        PlatformCapabilities capabilities;
        CredentialStore *credentials;
        NativeDialogs *dialogs;
        CrashService *crash;
    };

    /** @brief Headless filesystem adapter that reports every path as absent. */
    class NullFileSystem final : public FileSystem {
    public:
        /** @copydoc FileSystem::Exists */
        [[nodiscard]] bool Exists(const std::filesystem::path &path) const override;
    };

    /** @brief Test clock whose time advances only by explicit caller action. */
    class DeterministicClock final : public Clock {
    public:
        /** @brief Constructs a clock at an explicit monotonic time. @param initial Initial elapsed time. */
        explicit DeterministicClock(Duration initial = Duration::FromMilliseconds(0)) noexcept;

        /** @copydoc Clock::MonotonicNow */
        [[nodiscard]] Duration MonotonicNow() const override;
        /** @brief Advances the clock without reading host time. @param elapsed Duration to add. */
        void Advance(Duration elapsed) noexcept;

    private:
        Duration m_now;
    };

    /** @brief Production monotonic clock backed by std::chrono::steady_clock. */
    class SteadyClock final : public Clock {
    public:
        /** @copydoc Clock::MonotonicNow */
        [[nodiscard]] Duration MonotonicNow() const override;
    };

    /** @brief Headless process adapter that exposes no process identity or environment variables. */
    class NullProcessService final : public ProcessService {
    public:
        /** @copydoc ProcessService::CurrentProcess */
        [[nodiscard]] ProcessMetadata CurrentProcess() const override;
        /** @copydoc ProcessService::EnvironmentValue */
        [[nodiscard]] std::optional<std::string> EnvironmentValue(std::string_view name) const override;
    };

    /** @brief Deterministic user-directory adapter backed by explicitly supplied paths. */
    class StaticUserDirectories final : public UserDirectories {
    public:
        /** @brief Stores the supplied logical directory roots. @param paths Paths to expose. */
        explicit StaticUserDirectories(UserDirectoryPaths paths);

        /** @copydoc UserDirectories::Config */
        [[nodiscard]] const std::filesystem::path &Config() const override;
        /** @copydoc UserDirectories::State */
        [[nodiscard]] const std::filesystem::path &State() const override;
        /** @copydoc UserDirectories::Cache */
        [[nodiscard]] const std::filesystem::path &Cache() const override;
        /** @copydoc UserDirectories::Logs */
        [[nodiscard]] const std::filesystem::path &Logs() const override;
        /** @copydoc UserDirectories::Crash */
        [[nodiscard]] const std::filesystem::path &Crash() const override;
        /** @copydoc UserDirectories::Temporary */
        [[nodiscard]] const std::filesystem::path &Temporary() const override;

    private:
        UserDirectoryPaths m_paths;
    };
}  // namespace Horo
