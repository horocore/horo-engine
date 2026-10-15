#pragma once
/** @file PackageProjectSession.h @brief Non-installed application-root package authority composition. */
#include "Horo/Application/PackageApplication.h"

namespace Horo::Application::Internal {
    /** @brief Explicit borrowed application services; all remain alive until host Close completes. */
    struct PackageProjectHostServices final {
        JobSystem &jobs;
        DurableFileSystem &files;
        const Clock &clock;
        const WallClock &wallClock;
        const Packages::IPackageExtensionTrust &trust;
    };

    /** @brief Explicit host policy; project metadata cannot choose transport, execution approval or native loading. */
    struct PackageProjectPolicy final {
        std::filesystem::path projectRoot;
        std::filesystem::path cacheRoot;
        Packages::PackagePlatform platform;
        Packages::PackageLifecycleConfiguration lifecycle;
        Packages::PackageRestoreLimits limits;
        Packages::PackageRestoreMode mode{Packages::PackageRestoreMode::Offline};
        bool requirePublisherVerification{};
    };

    /** @brief Bounded inert project snapshot prepared on a host-owned I/O job, never during modal drawing. */
    struct PreparedPackageProject final {
        PackageApplicationConfiguration application;
        Sha256Digest lockDigest;
    };

    /** @brief Reads exact portable metadata, validating request/lock freshness without resolving, fetching or executing code.
     * @param policy Host project/platform policy. @param cancellation Parent I/O cancellation.
     * @return Complete validated snapshot or typed source diagnostic. */
    [[nodiscard]] Result<PreparedPackageProject> PreparePackageProject(const PackageProjectPolicy &policy,
                                                                       const CancellationToken &cancellation);

    /** @brief Owns all project package authorities; aliasing application leases pin this entire owner.
     * @details Create/Pump/Close and final destruction belong to one host lane. The host retains its explicit session until
     * Close succeeds, closes every adapter, then releases the session before borrowed jobs/files/trust/clock. No hidden queue exists.
     */
    class PackageProjectSession final : public std::enable_shared_from_this<PackageProjectSession> {
        struct Impl;

    public:
        /** @brief Non-aggregate allocation authority issued only by the validating factory. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class PackageProjectSession;
        };

        /** @brief Adopts factory-validated state through standard RAII allocation.
         * @param key Private factory authority. @param impl Complete validated state and its existing lifetime owners.
         * @details Clients continue to use Create; the allocation constructor cannot be called with a default key. */
        PackageProjectSession(const ConstructionKey &key, std::unique_ptr<Impl> impl) noexcept;

        /** @brief Composes prepared metadata and existing host services without starting restore or native loading.
         * @param services Borrowed scheduler, filesystem, clocks and execution approval authority.
         * @param policy Explicit host policy.
         * @param prepared Validated project snapshot. @param source Optional exact artifact source.
         * @param publishers Optional publisher authority. @return Inert owning session or typed failure. */
        [[nodiscard]] static Result<std::shared_ptr<PackageProjectSession>> Create(
            PackageProjectHostServices services, PackageProjectPolicy policy, PreparedPackageProject prepared,
            Packages::IPackageRestoreSource *source = nullptr, Packages::PackagePublisherVerificationService *publishers = nullptr);
        ~PackageProjectSession() noexcept;
        /** @brief Issues a capability retaining all concrete authorities through pending callbacks.
         * @return Aliasing application capability or typed closed/owner-lane failure; no competing lifecycle owner. */
        [[nodiscard]] Result<std::shared_ptr<PackageApplication>> Application();
        /** @brief Advances the shared operation at an actual host boundary. @param boundary Actual host phase. @return Typed outcome. */
        [[nodiscard]] Result<void> Pump(Packages::PackageActivationBoundary boundary);
        /** @brief Requests cancellation and owner-lane retirement without waiting.
         * @return Busy while pending work must remain owned, or success once all authorities are closed. */
        [[nodiscard]] Result<void> Close();

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application::Internal
