#pragma once
/** @file PackageProjectSession.h @brief Non-installed application-root package authority composition. */
#include "Horo/Application/PackageApplication.h"

#include <string_view>

namespace Horo::AI {
    class DecisionNodeGeneration;
}

namespace Horo::Assets {
    class IAssetRegistrySource;
    struct AssetRegistryCandidate;
}

namespace Horo::Packages {
    class GameplayPackageModuleHost;
    class NativePackageModuleHost;
}

namespace Horo::Platform {
    class IProjectReadFiles;
    struct ProjectInventoryLimits;
    struct ProjectReadContext;
}

namespace Horo::Application::Internal {
    struct PackageRemovalRegistrationEvidence;
    struct PackageRemovalSources;
    /** @brief Short-lived exact Decision declaration leases acquired on the package owner lane. */
    using PackageDecisionNodeLeaves = std::vector<std::shared_ptr<const AI::DecisionNodeGeneration>>;
    using PackageImporterCatalog = std::shared_ptr<const Assets::AssetImporterCatalogSnapshot>;

    /** @brief Finds the explicitly configured native backend owner that publishes importer receipts. */
    [[nodiscard]] const Packages::NativePackageModuleHost *FindPackageImporterOwner(
        const Packages::PackageLifecycleConfiguration &configuration) noexcept;

    /** @brief Explicit product factories for root-bound saved removal evidence. */
    struct PackageRemovalReadAuthorities final {
        Result<std::shared_ptr<const Platform::IProjectReadFiles>> (*openFiles)(const std::filesystem::path &, std::string,
                                                                               std::uint64_t){};
        Result<std::shared_ptr<const Assets::IAssetRegistrySource>> (*openAssets)(std::shared_ptr<const Platform::IProjectReadFiles>,
                                                                                  Platform::ProjectReadContext,
                                                                                  const Platform::ProjectInventoryLimits &){};
    };

    /** @brief Explicit borrowed application services; all remain alive until host Close completes. */
    struct PackageProjectHostServices final {
        JobSystem &jobs;
        DurableFileSystem &files;
        const Clock &clock;
        const WallClock &wallClock;
        const Packages::IPackageExtensionTrust &trust;
        PackageRemovalReadAuthorities removal;
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
        Packages::GameplayPackageModuleHost *gameplayModules{};    /**< Optional exact owner already retained by lifecycle.moduleHosts. */
        std::optional<std::filesystem::path> executionProjectRoot; /**< Explicit launcher pin for execution-enabled hosts only. */
    };

    /** @brief Bounded inert project snapshot prepared on a host-owned I/O job, never during modal drawing. */
    struct PreparedPackageProject final {
        PackageApplicationConfiguration application;
        Sha256Digest lockDigest;
        Sha256Digest requestDocumentDigest;
    };

    /** @brief Whether a metadata reader may repair an interrupted publication. */
    enum class PackageProjectReadMode {
        Recover,
        ReadOnly
    };

    /** @brief Reads exact portable metadata, validating request/lock freshness without resolving, fetching or executing code.
     * @param policy Host project/platform policy. @param files Filesystem for interrupted publication recovery.
     * @param cancellation Parent I/O cancellation.
     * @param mode Read-only callers reject pending recovery without changing any file.
     * @return Complete validated snapshot or typed source diagnostic. */
    [[nodiscard]] Result<PreparedPackageProject> PreparePackageProject(const PackageProjectPolicy &policy, DurableFileSystem &files,
                                                                       const CancellationToken &cancellation,
                                                                       PackageProjectReadMode mode = PackageProjectReadMode::Recover);

    /** @brief Publication reached the lock replacement commit point. */
    struct PackageMetadataPublication final {
        std::optional<Error> recoveryDiagnostic; /**< Durability or journal cleanup needs another recovery pass. */
    };

    /** @brief Digests of the exact base and candidate documents shown in the approved update review. */
    struct PackageUpdateApproval final {
        Sha256Digest baseRequest;
        Sha256Digest baseLock;
        Sha256Digest candidateRequest;
        Sha256Digest candidateLock;
    };

    /** @brief Inputs to the journaled publication of a staged candidate. */
    struct ApprovedPackageProjectUpdate final {
        const PreparedPackageProject &base;
        const Packages::StagedPackageRestoreRecord &staged;
        PackageUpdateApproval approval;
        std::string_view candidateRequest;
        std::string_view candidateLock;
    };

    /**
     * @brief Publishes exact approved metadata under the project mutation lease.
     * @param policy Host-selected project, platform and verification requirements.
     * @param update Approved base and candidate documents and verified staging record.
     * @param files Durable host filesystem.
     * @param context Live project admission and cancellation authority.
     * @return Commit result or a typed failure. A failed pre-commit publication attempts immediate journal recovery.
     * @pre The caller owns approval, has checked live executable ownership and reference safety, and uses the host's publisher policy
     * for staging. Any project-document migration must share a recovery boundary with publication.
     * @details Withdrawing packages requires the dedicated publication path with withdrawal evidence.
     * Publication never executes or activates a package. The unchanged lock is a no-op.
     */
    [[nodiscard]] Result<PackageMetadataPublication> PublishPackageProjectUpdate(const PackageProjectPolicy &policy,
                                                                                 const ApprovedPackageProjectUpdate &update,
                                                                                 DurableFileSystem &files,
                                                                                 const Platform::ProjectReadContext &context);

    /** @brief Publishes an approved metadata pair after checking every withdrawn package under the mutation lease.
     * @param policy Host-selected project, platform and verification requirements.
     * @param update Approved base and candidate documents with verified staging.
     * @param files Durable host filesystem.
     * @param sources Current project inventory and offline staging for the exact departing lock.
     * @return Committed publication or a typed approval, reference or recovery failure.
     */
    [[nodiscard]] Result<PackageMetadataPublication> PublishPackageUpdateWithWithdrawals(const PackageProjectPolicy &policy,
                                                                                         const ApprovedPackageProjectUpdate &update,
                                                                                         DurableFileSystem &files,
                                                                                         const PackageRemovalSources &sources);

    /** @brief Exact previous metadata bytes for the currently published update. */
    struct PackageRollbackDocuments final {
        std::string request;
        std::string lock;
        Sha256Digest publishedRequest;
        Sha256Digest publishedLock;
    };

    /**
     * @brief Reads the previous pair only when the current project still matches the published update.
     * @param policy Host project identity and metadata bounds.
     * @param files Host filesystem used for read-only checkpoint admission.
     * @param cancellation Parent read cancellation.
     * @return Previous valid documents or a typed missing, stale, or damaged checkpoint error.
     */
    [[nodiscard]] Result<PackageRollbackDocuments> ReadPackageProjectRollback(const PackageProjectPolicy &policy, DurableFileSystem &files,
                                                                              const CancellationToken &cancellation);

    /** @brief Project-owned asset whose recorded importer will be withdrawn. */
    struct PackageImporterReference final {
        Packages::HoroPackageId package;
        std::string assetPath;
    };

    /** @brief Lists all registered assets whose importer belongs to a removed package.
     * @param assets Fresh root-bound project asset source. @param removal Exact lock graph impact.
     * @param reviewed Optional destination for the same validated inventory, including degraded results.
     * @return Complete importer evidence, or an inventory failure with diagnostics for admitted references. */
    [[nodiscard]] Result<std::vector<PackageImporterReference>> InspectPackageRemovalImporters(
        const Assets::IAssetRegistrySource &assets, const Packages::PackageRemovalGraphPreview &removal,
        Assets::AssetRegistryCandidate *reviewed = nullptr);

    /** @brief Adds one canonical removal diagnostic for every confirmed saved importer reference. */
    void AppendPackageImporterDiagnostics(std::vector<Diagnostic> &diagnostics, std::span<const PackageImporterReference> references);

    /** @brief Reports asset inventory failures with their project-relative paths.
     * @param inventory The incomplete candidate from the project asset source.
     * @return A typed failure containing the inventory diagnostics.
     */
    [[nodiscard]] Error MakeIncompletePackageInventoryError(const Assets::AssetRegistryCandidate &inventory);

    /** @brief Publishes an approved removal and retains the prior metadata pair for explicit rollback.
     * @param policy Host project/platform policy. @param expected Exact approved request and lock snapshot.
     * @param package Direct request to withdraw. @param files Host filesystem authority.
     * @param sources Read authorities for the same project, exact registration evidence, and a native withdrawal reservation
     * or verified offline staging evidence.
     * @pre The caller has checked remaining reference classes and approved this removal.
     * @details Acquires the shared project mutation lease before recovery or reference scans and retains it through publication.
     * The project must still authorize removal before publishing the journal. Once published, the journal owns recovery and
     * the old live reservation continues to block native activation.
     * @return Committed publication, potentially with a recovery diagnostic, or a pre-commit failure. */
    [[nodiscard]] Result<PackageMetadataPublication> PublishPackageProjectRemoval(const PackageProjectPolicy &policy,
                                                                                  const PreparedPackageProject &expected,
                                                                                  const Packages::HoroPackageId &package,
                                                                                  DurableFileSystem &files,
                                                                                  const PackageRemovalSources &sources);

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
        /** @brief Reads commit evidence on the session lane. @return True when the old lock was withdrawn. */
        [[nodiscard]] bool RemovalPublished() const noexcept;
        /** @brief Reads the existing committed availability record on the owner lane without granting native execution.
         * @return Sealed immutable graph lease, empty before installation, or typed closed/foreign-lane failure.
         * @details The record retains archive bytes after session retirement; callers separately validate current host generation,
         * publisher/deployment trust and native lifetime. No store, restore, activation or XR dependency is introduced.
         */
        [[nodiscard]] Result<std::shared_ptr<const Packages::VerifiedPackageInstallRecord>> InstalledRecord() const;
        /** @brief Copies the exact package registrations observed in this project session, including withdrawn roles.
         * @return Inert evidence transferable to a removal worker, or typed observation failure.
         * @details The evidence carries no module lease and makes no claim about registrations never observed in this session. */
        [[nodiscard]] Result<std::shared_ptr<const PackageRemovalRegistrationEvidence>> RemovalRegistrations() const;
        /** @brief Observes the already published gameplay role activation on the owner lane.
         * @return Exact generation or zero when no configured gameplay role is active. */
        [[nodiscard]] std::uint64_t DecisionCatalogGeneration() const noexcept;
        /** @brief Acquires Decision leaves from only the expected live activation.
         * @param activation Exact previously observed generation. @return Admitted declarations or a typed stale/owner failure. */
        [[nodiscard]] Result<PackageDecisionNodeLeaves> AcquireDecisionNodes(std::uint64_t activation) const;
        /** @brief Acquires a combined catalog from active importer receipts on the owner lane. */
        [[nodiscard]] Result<PackageImporterCatalog> ComposeImporters(PackageImporterCatalog host) const;
        /** @brief Advances the shared operation at an actual host boundary. @param boundary Actual host phase. @return Typed outcome. */
        [[nodiscard]] Result<void> Pump(Packages::PackageActivationBoundary boundary);
        /** @brief Requests cancellation and owner-lane retirement without waiting.
         * @return Busy while pending work must remain owned, or success once all authorities are closed. */
        [[nodiscard]] Result<void> Close();

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application::Internal
