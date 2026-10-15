#pragma once

/** @file PackageApplication.h
 * @brief One project-bound application operation owner shared by package presentation adapters.
 */
#include "Horo/Packages/PackageLifecycle.h"

#include <functional>
#include <thread>

namespace Horo::Application {
    /** @brief Adapter-independent operation kind; installation never implies execution approval. */
    enum class PackageOperationKind : std::uint8_t {
        Restore,
        Install,
        Activate,
        Disable
    };
    /** @brief Authoritative operation disposition, separate from installed and active package state. */
    enum class PackageOperationOutcome : std::uint8_t {
        Running,
        Succeeded,
        Failed,
        Cancelled
    };
    /** @brief Safe stable phase for structured UI/CLI/MCP progress. */
    enum class PackageOperationPhase : std::uint8_t {
        Admitted,
        Restoring,
        Installing,
        Activating,
        Completed
    };

    /** @brief Non-wrapping identity scoped to the explicitly injected application service. */
    struct PackageOperationId final {
        std::uint64_t value{};
        [[nodiscard]] auto operator<=>(const PackageOperationId &) const = default;
    };

    /** @brief Immutable bounded projection; diagnostics retain their typed original cause. */
    struct PackageOperationSnapshot final {
        PackageOperationId id;
        std::uint64_t revision{};
        PackageOperationKind kind{};
        PackageOperationPhase phase{PackageOperationPhase::Admitted};
        PackageOperationOutcome outcome{PackageOperationOutcome::Running};
        float completion{};
        std::size_t completedPackages{}, totalPackages{};
        std::optional<Error> diagnostic;
        std::optional<std::uint64_t> installedRevision; /**< Actual sealed availability publication, including before activation failure. */
        std::optional<std::uint64_t> activeGeneration;  /**< Actual existing active graph, never inferred from requested enablement. */
    };

    /** @brief Evidence shown by adapters; neither value grants execution trust. */
    enum class PackagePlanVerification : std::uint8_t {
        LockedOnly,
        ArchiveVerified
    };

    /** @brief Immutable presentation record built from existing authoritative locked/verified evidence. */
    struct PackagePlanEntry final {
        Packages::LockedPackage locked;
        PackagePlanVerification verification{PackagePlanVerification::LockedOnly};
        std::optional<std::uint64_t> archiveBytes;
        std::optional<std::uint64_t> expandedBytes;
        std::optional<std::size_t> verifiedFiles;
        std::optional<Packages::PackagePublisherVerificationDecision> publisher;
    };

    /** @brief Complete bounded plan; semantic package-kind/license/import-copy facts remain explicitly unavailable.
     * @details Records carry only existing locked and verified archive evidence. Adapters must display missing
     * semantic facts as unavailable, never infer them from filenames, or use this presentation as execution approval.
     */
    struct PackagePlanSnapshot final {
        std::uint64_t revision{};
        std::vector<PackagePlanEntry> packages;
    };

    /** @brief Exact host policy; no portable package request can approve execution or select a native loader. */
    struct PackageApplicationConfiguration final {
        Packages::PackageRestoreRequest restore;
        std::optional<Packages::ValidatedPackageLockfileV1> lockedPlan; /**< Already validated host project snapshot, when available. */
        std::function<Result<void>()> validateAdmission; /**< Owner-lane current metadata/policy check; retained through Close. */
        const WallClock *wallClock{};                    /**< Host clock retained through Close; mandatory for publisher verification. */
        std::size_t maximumEnabledDescriptors{64};
    };

    /**
     * @brief Serializes package mutations over existing restore, install and activation authorities.
     * @details All methods belong to the construction lane. The host exclusively routes mutations through this
     * service and retains jobs/authorities until Close succeeds. Restore and durable installation run on owned
     * background work; Pump never waits. Activation executes only at an explicit host quiescent boundary.
     * A failed/cancelled attempt preserves underlying authorities' committed state. Close requests cancellation
     * and returns busy while admitted work remains; abandoning a busy service violates its lifetime precondition.
     */
    class PackageApplication final {
        struct Impl;

    public:
        /** @brief Non-aggregate allocation authority issued only by the validating factory. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class PackageApplication;
        };

        /** @brief Adopts factory-validated state through standard RAII allocation.
         * @param key Private factory authority. @param impl Complete validated state and its existing lifetime owners.
         * @details Clients continue to use Create; the allocation constructor cannot be called with a default key. */
        PackageApplication(const ConstructionKey &key, std::unique_ptr<Impl> impl) noexcept;

        /** @brief Binds existing authorities without restoring, installing, loading or discovering services.
         * @param jobs Existing job owner with explicit I/O capacity.
         * @param restore Existing verified graph restore authority.
         * @param install Existing durable availability authority.
         * @param lifecycle Existing execution/trust authority.
         * @param configuration Immutable host project/platform/request policy.
         * @return Inert owner or typed invalid composition failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<PackageApplication>> Create(JobSystem &jobs, Packages::PackageRestoreService &restore,
                                                                                Packages::PackageInstallService &install,
                                                                                Packages::PackageLifecycleService &lifecycle,
                                                                                PackageApplicationConfiguration configuration);
        ~PackageApplication() noexcept;
        PackageApplication(const PackageApplication &) = delete;
        PackageApplication &operator=(const PackageApplication &) = delete;
        /** @brief Starts exact locked restore; never changes requested dependencies or activates code.
         * @param cancellation Parent operation cancellation. @return New operation or typed admission failure. */
        [[nodiscard]] Result<PackageOperationId> Restore(const CancellationToken &cancellation = {});
        /** @brief Restores the exact current host request then installs its complete graph on owned I/O work.
         * @param cancellation Parent operation cancellation. @return New operation or typed missing graph/busy failure. */
        [[nodiscard]] Result<PackageOperationId> Install(const CancellationToken &cancellation = {});
        /** @brief Restores/installs missing current availability, then queues explicit selection for the host safe point; trust remains
         * external.
         * @param enabled Exact installed package descriptor identities. @param cancellation Parent cancellation.
         * @return New operation or typed validation/admission failure. */
        [[nodiscard]] Result<PackageOperationId> Activate(std::span<const Packages::EnabledPackageExtension> enabled,
                                                          const CancellationToken &cancellation = {});
        /** @brief Queues empty enablement without uninstalling content or changing trust.
         * @param cancellation Parent cancellation. @return New operation or typed admission failure. */
        [[nodiscard]] Result<PackageOperationId> Disable(const CancellationToken &cancellation = {});
        /** @brief Advances completed work and, only when quiescent, queued activation without waiting.
         * @param boundary Actual host lifecycle boundary. @return Success or typed owner/closed failure. */
        [[nodiscard]] Result<void> Pump(Packages::PackageActivationBoundary boundary);
        /** @brief Returns the exact retained operation; stale IDs cannot observe a replacement attempt.
         * @param operation Expected operation identity. @return Immutable projection or typed stale/owner failure. */
        [[nodiscard]] Result<PackageOperationSnapshot> Query(PackageOperationId operation) const;
        /** @brief Returns the current operation projection without inventing an operation or mutating authorities.
         * @return Current retained operation, empty before admission, or typed owner-lane failure. */
        [[nodiscard]] Result<std::optional<PackageOperationSnapshot>> Current() const;
        /** @brief Returns the immutable precomputed plan, with evidence status separate from actual installed/active state.
         * @return Owning read-only plan or typed missing-evidence/owner-lane failure. */
        [[nodiscard]] Result<std::shared_ptr<const PackagePlanSnapshot>> Plan() const;
        /** @brief Requests cooperative cancellation of one exact current operation.
         * @param operation Expected operation identity. @return Success or typed stale/owner failure. */
        [[nodiscard]] Result<void> RequestCancel(PackageOperationId operation);
        /** @brief Stops admission and requests cancellation; never blocks or shuts borrowed authorities down.
         * @return Success once owned work is terminal, otherwise typed busy with all ownership retained. */
        [[nodiscard]] Result<void> Close();

    private:
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Application
