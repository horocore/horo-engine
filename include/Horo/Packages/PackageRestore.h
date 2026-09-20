#pragma once

/**
 * @file PackageRestore.h
 * @brief Cancellable, atomic project package-lock restore orchestration.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Packages/PackageCache.h"
#include "Horo/Packages/PackageLockfile.h"
#include "Horo/Security/ArtifactSignature.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Packages {
    class PackagePublisherVerificationService;

    /** @brief Selects whether restore may use configured remote or source transports. */
    enum class PackageRestoreMode : std::uint8_t {
        Online,
        Offline,
    };

    /** @brief Current background phase of one package restore operation. */
    enum class PackageRestorePhase : std::uint8_t {
        ValidatingRequest,
        AcquiringProjectLock,
        ReadingLockfile,
        ValidatingLockfile,
        RestoringPackages,
        FetchingArtifact,
        VerifyingArtifact,
        Committing,
        Completed,
        Failed,
        Cancelled,
    };

    /** @brief Terminal or active disposition projected to restore callers. */
    enum class PackageRestoreOutcome : std::uint8_t {
        Running,
        Ready,
        Failed,
        Cancelled,
    };

    /** @brief Process-local generation-safe identity of one admitted restore. */
    struct PackageRestoreOperationId final {
        std::uint64_t value{};

        [[nodiscard]] constexpr explicit operator bool() const noexcept {
            return value != 0U;
        }

        [[nodiscard]] constexpr auto operator<=>(const PackageRestoreOperationId &) const noexcept = default;
    };

    /** @brief Bounds applied before lockfile parsing, artifact validation, and restore graph publication. */
    struct PackageRestoreLimits {
        PackageLockfileLimits lockfile{};
        PackageValidationLimits validation{};
        std::uint32_t supportedPackageFormatVersion{1U};
    };

    /** @brief Immutable input snapshot for one restore attempt. */
    struct PackageRestoreRequest {
        std::filesystem::path projectRoot; /**< Absolute canonical project root; may be empty for an in-memory lockfile test/host. */
        std::string lockfileJson;          /**< Optional bounded lockfile bytes; empty reads `.horo/packages.lock`. */
        Sha256Digest expectedRequestHash;  /**< Current canonical dependency-request digest from `packages.json`. */
        PackagePlatform platform;
        PackageRestoreMode mode{PackageRestoreMode::Online};
        bool requirePublisherVerification{false};
        std::uint64_t nowUnixMilliseconds{};
        CancellationToken cancellation;
    };

    /** @brief Exact locked package requested from one already selected portable source. */
    struct PackageRestoreArtifactRequest {
        LockedPackage package;
    };

    /** @brief Source response containing exact archive bytes and optional detached publisher evidence. */
    struct PackageRestoreArtifact {
        std::vector<std::byte> bytes;
        std::optional<Security::DetachedSignatureEnvelope> signature;
    };

    /**
     * @brief Host-composed package source used only for exact lockfile artifact retrieval.
     * @details Implementations must not resolve another version or source, expose credentials, or execute package code.
     */
    class IPackageRestoreSource {
    public:
        virtual ~IPackageRestoreSource() = default;

        /**
         * @brief Fetches the exact artifact selected by the lockfile.
         * @param request Immutable package identity and expected artifact evidence.
         * @param cancellation Cooperative cancellation ancestry for the source call.
         * @return Artifact bytes and optional detached signature, or a typed source failure.
         */
        [[nodiscard]] virtual Result<PackageRestoreArtifact> Fetch(const PackageRestoreArtifactRequest &request,
                                                                   const CancellationToken &cancellation) = 0;
    };

    /** @brief One verified package archive admitted to the restored local graph. */
    struct PackageRestorePackage {
        LockedPackage lock;
        std::shared_ptr<const ValidatedPackageArchive> archive;
        bool cacheHit{};
    };

    /** @brief Complete immutable candidate graph published only after every locked package is ready. */
    struct PackageRestoreGraph {
        Sha256Digest requestHash;
        PackagePlatform platform;
        std::vector<PackageRestorePackage> packages;
    };

    /** @brief Thread-safe projection of one restore's progress and actionable terminal diagnostic. */
    struct PackageRestoreProgressSnapshot {
        PackageRestoreOperationId operationId;
        std::uint64_t revision{};
        PackageRestorePhase phase{PackageRestorePhase::ValidatingRequest};
        PackageRestoreOutcome outcome{PackageRestoreOutcome::Running};
        float progress{};
        std::size_t completedPackages{};
        std::size_t totalPackages{};
        std::size_t cacheHits{};
        std::size_t downloadedPackages{};
        std::string currentPackage;
        bool offline{};
        std::optional<Error> diagnostic;
    };

    /** @brief Lifecycle state of the host-owned restore service. */
    enum class PackageRestoreLifecycleState : std::uint8_t {
        Ready,
        Running,
        RecoverableFailure,
        Closed,
    };

    /** @brief Bounded service state suitable for diagnostics and host health surfaces. */
    struct PackageRestoreServiceState {
        PackageRestoreLifecycleState lifecycle{PackageRestoreLifecycleState::Ready};
        std::uint64_t revision{};
        std::uint64_t attempts{};
        std::optional<PackageRestoreOperationId> activeOperation;
        bool hasActiveGraph{};
    };

    /** @brief Move-only reference to an admitted restore operation. */
    class PackageRestoreOperationHandle final {
    public:
        PackageRestoreOperationHandle(PackageRestoreOperationHandle &&) noexcept = default;
        PackageRestoreOperationHandle &operator=(PackageRestoreOperationHandle &&) noexcept = default;
        PackageRestoreOperationHandle(const PackageRestoreOperationHandle &) = delete;
        PackageRestoreOperationHandle &operator=(const PackageRestoreOperationHandle &) = delete;

        /** @brief Returns the stable operation identity. @return Generation-safe operation ID. */
        [[nodiscard]] PackageRestoreOperationId Id() const noexcept {
            return id_;
        }

    private:
        friend class PackageRestoreService;

        explicit PackageRestoreOperationHandle(const PackageRestoreOperationId id) noexcept : id_(id) {}

        PackageRestoreOperationId id_;
    };

    /**
     * @brief Restores exact lockfile artifacts into a verified, atomically published local package graph.
     * @details Restore never resolves a new version, mutates the requested dependency set, executes package code, or publishes a
     * partial graph. Cache access and project admission are serialized by digest and project locks respectively.
     */
    class PackageRestoreService final {
    public:
        /**
         * @brief Creates a restore service from host-owned concurrency, filesystem, cache, source, and trust authorities.
         * @param jobs Process job system that outlives this service.
         * @param files Durable filesystem authority used for cross-process project admission.
         * @param cache Verified content-addressed package cache.
         * @param source Optional online source; offline restores never call it.
         * @param publisherVerification Optional publisher policy for fetched artifacts.
         * @param limits Bounded lockfile, archive, and package-format policy.
         * @return Ready service or a typed invalid-policy failure.
         */
        [[nodiscard]] static Result<PackageRestoreService> Create(JobSystem &jobs, DurableFileSystem &files, PackageCacheStore &cache,
                                                                  IPackageRestoreSource *source = nullptr,
                                                                  PackagePublisherVerificationService *publisherVerification = nullptr,
                                                                  PackageRestoreLimits limits = {});

        PackageRestoreService(PackageRestoreService &&) noexcept;
        PackageRestoreService &operator=(PackageRestoreService &&) noexcept;
        PackageRestoreService(const PackageRestoreService &) = delete;
        PackageRestoreService &operator=(const PackageRestoreService &) = delete;
        ~PackageRestoreService() noexcept;

        /**
         * @brief Starts one cancellable restore operation.
         * @param request Project root, current dependency-request hash, host platform, and restore mode.
         * @return Move-only operation handle or a typed admission failure.
         */
        [[nodiscard]] Result<PackageRestoreOperationHandle> Start(const PackageRestoreRequest &request);

        /**
         * @brief Reads and, when terminal, finalizes an operation projection.
         * @param operation Generation-safe operation identity.
         * @return Snapshot or empty for an unknown/evicted operation.
         */
        [[nodiscard]] std::optional<PackageRestoreProgressSnapshot> Query(PackageRestoreOperationId operation) const;

        /**
         * @brief Requests cooperative cancellation before graph publication.
         * @param operation Operation identity.
         * @return Success, stale identity, or closed-service failure.
         */
        [[nodiscard]] Result<void> RequestCancel(PackageRestoreOperationId operation);

        /** @brief Advances progress and commits completed worker candidates without blocking. */
        void Pump() const;

        /**
         * @brief Returns the last atomically published graph, if any.
         * @return Shared immutable graph snapshot; failed or cancelled restores leave it unchanged.
         */
        [[nodiscard]] std::shared_ptr<const PackageRestoreGraph> ActiveGraph() const;

        /** @brief Returns synchronized lifecycle counters and publication state. @return Current service state. */
        [[nodiscard]] PackageRestoreServiceState State() const;

        /**
         * @brief Stops admission, cancels active work, and waits for its safe terminal boundary.
         * @return Success or a typed shutdown failure.
         */
        [[nodiscard]] Result<void> Shutdown();

    private:
        struct Impl;

        explicit PackageRestoreService(std::unique_ptr<Impl> state);
        void RefreshLocked() const;

        std::unique_ptr<Impl> state_;
    };
}  // namespace Horo::Packages
