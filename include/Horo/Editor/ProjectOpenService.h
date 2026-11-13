#pragma once

/** @file ProjectOpenService.h @brief Owned project-open orchestration independent of GUI presentation. */

#include "Horo/Editor/ProjectMigrationTransaction.h"
#include "Horo/Editor/ProjectSession.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Result.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace Horo::Editor {
    class RendererAvailabilitySnapshot;

    /** @brief Read-only compatibility, recovery, and migration-plan projection shared by startup surfaces. */
    struct ProjectOpenPreflightSnapshot {
        Application::ProjectCompatibilitySnapshot compatibility;
        MigrationRecoverySnapshot recovery;
        std::optional<Application::ProjectMigrationPlan> migrationPlan;
    };

    /** @brief Performs bounded project-open classification without acquiring mutation authority. */
    class ProjectOpenPreflightService {
    public:
        /**
         * @brief Creates a read-only preflight authority from the transaction recovery inspector.
         * @param transactions Transaction service used only for non-mutating recovery classification.
         */
        explicit ProjectOpenPreflightService(ProjectMigrationTransactionService &transactions);
        /**
         * @brief Inspects recovery evidence, compatibility, and an available migration chain without mutation.
         * @param projectRoot Canonical project root to inspect.
         * @return Mutually exclusive compatibility result, recovery evidence, and optional migration plan.
         */
        [[nodiscard]] ProjectOpenPreflightSnapshot Inspect(const std::filesystem::path &projectRoot) const;

    private:
        ProjectMigrationTransactionService &transactions_;
        std::optional<Application::ProjectMigrationRegistry> registry_;
        std::optional<Application::ProjectMigrationSupportDescriptor> support_;
        std::optional<Error> catalogError_;
    };

    /** @brief Request for one authoritative project-open operation. */
    struct ProjectOpenRequest {
        std::filesystem::path projectRoot;
        std::string expectedProjectName;
        std::string engineBuildIdentity;
        Application::ProjectMigrationLimits migrationLimits;
    };

    /** @brief Observable phase of the single owned project-open state machine. */
    enum class ProjectOpenPhase : std::uint8_t {
        Inspecting,
        CleaningRecovery,
        Recovering,
        ValidatingCompatibility,
        PlanningMigration,
        Migrating,
        UpdatingProjectMetadata,
        ValidatingDefaultScene,
        RebuildingDerivedState,
        RendererPreflight,
        PreparingWorkspace,
        ReadyToActivate,
        RequiresRendererRestart,
        Failed,
        Cancelled,
    };

    /** @brief Terminal disposition or current non-terminal state. */
    enum class ProjectOpenOutcome : std::uint8_t {
        Running,
        ReadyToActivate,
        RequiresRendererRestart,
        Failed,
        Cancelled,
    };

    /** @brief Immutable projection consumed by GUI and headless hosts. */
    struct ProjectOpenProgressSnapshot {
        ProjectOpenOperationId operationId;
        ProjectOpenPhase phase{ProjectOpenPhase::Inspecting};
        ProjectOpenOutcome outcome{ProjectOpenOutcome::Running};
        float progress{};
        std::filesystem::path projectRoot;
        std::string projectName;
        std::string requiredRendererBackend;
        std::optional<Error> diagnostic;
        bool cancellationDeferred{};
        std::optional<ProjectSessionCandidateId> readySession;
    };

    /** @brief Move-only unpublished result of one derived-state preparation. */
    class IPreparedProjectOpenDerivedState {
    public:
        virtual ~IPreparedProjectOpenDerivedState() = default;
        /** @brief Publishes the prepared state on the application owner thread. @return Published revision text. */
        [[nodiscard]] virtual Result<std::string> Install() = 0;
    };

    /** @brief Prepares disposable project state in deterministic composition order. */
    class IProjectOpenDerivedStateContributor {
    public:
        virtual ~IProjectOpenDerivedStateContributor() = default;
        /** @brief Stable contributor identity used for diagnostics and deterministic revision projection. */
        [[nodiscard]] virtual std::string_view Id() const noexcept = 0;
        /** @brief Builds an unpublished candidate without mutating authoritative in-memory state. */
        [[nodiscard]] virtual Result<std::unique_ptr<IPreparedProjectOpenDerivedState>> Prepare(const std::filesystem::path &projectRoot,
                                                                                                const CancellationToken &cancellation) = 0;
    };

    /** @brief Move-only reference to an admitted project-open operation. */
    class ProjectOpenOperationHandle {
    public:
        ProjectOpenOperationHandle(ProjectOpenOperationHandle &&) noexcept = default;
        ProjectOpenOperationHandle &operator=(ProjectOpenOperationHandle &&) noexcept = default;
        ProjectOpenOperationHandle(const ProjectOpenOperationHandle &) = delete;
        ProjectOpenOperationHandle &operator=(const ProjectOpenOperationHandle &) = delete;

        [[nodiscard]] ProjectOpenOperationId Id() const noexcept {
            return id_;
        }

    private:
        friend class ProjectOpenService;

        explicit ProjectOpenOperationHandle(ProjectOpenOperationId id) noexcept : id_(id) {}

        ProjectOpenOperationId id_;
    };

    /** @brief Serial owner-thread project-open orchestration service. */
    class ProjectOpenService {
    public:
        /**
         * @brief Composes the project-open owner from host-lifetime services.
         * @param jobs Process job system used by the operation-owned serialized worker.
         * @param files Durable filesystem authority.
         * @param preflight Shared read-only compatibility and migration planning authority.
         * @param mutations Shared project mutation coordinator.
         * @param transactions Crash-safe migration transaction service.
         * @param rendererAvailability Immutable renderer installation snapshot.
         * @param derivedContributors Ordered disposable-state rebuild contributors.
         * @param platformPolicy Explicit host profile and trusted inert provider metadata, copied for the service lifetime.
         */
        ProjectOpenService(JobSystem &jobs, DurableFileSystem &files, ProjectOpenPreflightService &preflight,
                           ProjectMutationCoordinator &mutations, ProjectMigrationTransactionService &transactions,
                           const RendererAvailabilitySnapshot &rendererAvailability,
                           std::span<IProjectOpenDerivedStateContributor *const> derivedContributors = {},
                           Application::PlatformServicesProjectValidationContext platformPolicy = {});
        ~ProjectOpenService();
        ProjectOpenService(const ProjectOpenService &) = delete;
        ProjectOpenService &operator=(const ProjectOpenService &) = delete;

        /** @brief Borrows the immutable host policy used by this service's project-source admission.
         * @return Actual profile, linked provider metadata, trust and limits for the service lifetime; no activation grant. */
        [[nodiscard]] const Application::PlatformServicesProjectValidationContext &PlatformPolicy() const noexcept;

        /** @brief Starts the only active operation. @param request Valid project root and limits. @return Move-only handle
         * or typed busy error. */
        [[nodiscard]] Result<ProjectOpenOperationHandle> Start(ProjectOpenRequest request);
        /** @brief Reads an immutable operation projection. @param operation Generation-safe identity. @return Snapshot or
         * empty when stale. */
        [[nodiscard]] std::optional<ProjectOpenProgressSnapshot> Query(ProjectOpenOperationId operation) const;
        /** @brief Requests cooperative cancellation; terminal operations are an idempotent no-op.
         * @param operation Operation identity.
         * @return Success or stale-ID error.
         */
        [[nodiscard]] Result<void> RequestCancel(ProjectOpenOperationId operation);
        /** @brief Reserves a ready session for transactional workspace construction. */
        [[nodiscard]] Result<ProjectSessionActivationLease> ReserveSession(ProjectSessionCandidateId session) const;
        /** @brief Explicitly discards a ready, unconsumed session. */
        [[nodiscard]] Result<void> DiscardSession(ProjectSessionCandidateId session) const;
        /** @brief Advances owner-thread state and performs serialized authoritative boundaries. */
        void PumpOwnerThread();
        /** @brief Stops admission and cancels the current operation; idempotent. */
        void Shutdown() noexcept;

    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}  // namespace Horo::Editor
